/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 * The control thread compiles the graph into a render plan: nodes in
 * processing order, their audio buffers, and a copy of their configuration.
 * The render thread can then process each block without traversing the graph
 * or allocating memory.
 *
 *    routes: source --> fader --> gain (output)
 *
 *    plan:   step[0]        step[1]        step[2]
 *            source         fader          gain
 *            process() ---> process() ---> process() ---> output
 *
 * Each step reads the output buffers of its inputs. Unconnected inputs read
 * from the engine's silence buffer. The buffers are reused for every block.
 *
 * Plans move between the threads without locking:
 *
 *    control thread                            render thread
 *
 *    commit: busy = 1
 *            pending = new plan ------------> adopt_pending(), before a block:
 *                                                retired = active
 *                                                active = pending
 *                                                pending = NULL
 *    collect: free retired plan <-----------     busy = 0
 *             (only when busy == 0)              render using active
 *
 * The release store to pending publishes the initialized plan; the render
 * thread acquires it before use. The release store clearing busy publishes
 * the retired plan; the control thread acquires it before freeing that plan
 * or publishing another one. While busy is set, commit returns NAU_AGAIN.
 *
 * Several plans can refer to the same node. Each plan keeps a reference and
 * its own copy of the source range, automation origin and ramps. Persistent
 * processor state stays with the node, so a commit does not reset it. Gain
 * and effect parameter targets use atomics and need no commit.
 */

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "nopeaudio/nopeaudio.h"
#include "engine_internal.h"
#include "internal.h"
#include "libnopeutil/memory.h"
#include "libnopeutil/utils.h"

#define MAX_NODES 4096U
#define MAX_INPUTS 64U
#define MAX_BLOCK_FRAMES 8192U

struct nau_node {
    struct nau_engine *engine;
    struct nau_processor processor;
    const struct nau_snapshot_callbacks *snapshot_callbacks; /* optional configuration snapshot callbacks */
    uint32_t refs;                 /* control thread only */
    /*
     * last plan-building attempt that visited this node;
     * duplicate-detection marker, control thread only
     */
    uint64_t compile_serial;
    int64_t origin;                /* origin of the automation; control thread only */
    int64_t next_frame;            /* frame following the last block rendered; render thread only */
    bool initialized;              /* rendered or positioned once; render thread only */
};

/*
 * One processing call in a render plan. Routing and configuration belong to
 * this step; persistent processor state belongs to the node. The control
 * thread builds and releases the step, including its snapshot and node
 * reference. Audio buffers belong to the plan or the engine.
 */
struct render_step {
    struct nau_node *node;        /* retained until the plan is freed */
    /*
     * owned array of input_count pointers to earlier steps' outputs,
     * or the engine's silence buffer for unconnected inputs
     */
    const float **inputs;
    float *output;                /* slice of the plan's buffers, cleared before process() */
    /*
     * process() argument: aliases node->processor.user, &source,
     * or snapshot; not freed separately
     */
    void *user;
    int64_t origin;               /* automation origin captured at commit */
    /*
     * source descriptor copied at commit; its user remains owned
     * by the node
     */
    struct nau_source source;
    void *snapshot;               /* owned processor configuration copy, NULL if unused */
    void (*release)(void *snapshot); /* snapshot destructor, NULL if unused */
};

static int source_process(void *user, const struct nau_block *p);

/*
 * Get the part [lo, hi) of the block [frame, frame + frames) the source
 * plays, in project frames. Return false if it plays none of it.
 */
static bool source_span(const struct nau_source *s, int64_t frame, uint32_t frames, int64_t *lo, int64_t *hi)
{
    const int64_t end = s->timeline_start + s->duration;
    *lo = frame > s->timeline_start ? frame : s->timeline_start;
    *hi = frame + frames < end ? frame + frames : end;
    return *lo < *hi;
}

/*
 * Only nodes reachable from the output are included, with inputs before their
 * consumers. Routing and configuration are fixed after publication; the render
 * thread writes the audio buffers. An empty plan renders silence.
 */
struct render_plan {
    struct render_step *steps;
    uint32_t step_count;             /* steps owning a node reference, including a partially built step */
    float *buffers;               /* one block of interleaved audio per step */
};

/*
 * Engine configuration and the plan handoff state shown at the top of the
 * file. busy prevents publishing another plan or collecting the previous
 * one until the render thread has completed the handoff.
 */
struct nau_engine {
    struct nau_engine_config config;
    float *silence;
    struct render_plan *active;           /* render thread only */
    struct render_plan *retired;          /* written by the render thread, read by the control thread once busy == 0 */
    _Atomic(struct render_plan *) pending;
    _Atomic uint32_t busy;
    uint64_t compile_serial;              /* generation of the current plan-building attempt; control thread only */
};

/*
 * Release a plan and clear its pointer. Also accepts a partially initialized
 * plan: step_count records the references acquired so far. Control thread only;
 * the plan must be unpublished, retired, or the rendering stopped.
 */
static void render_plan_freep(struct render_plan **pp)
{
    struct render_plan *p = *pp;
    if (!p)
        return;
    for (uint32_t i = 0; i < p->step_count; i++) {
        struct render_step *step = &p->steps[i];
        if (step->release)
            step->release(step->snapshot);
        nau_node_unrefp(&step->node);
        npriv_freep(&step->inputs);
    }
    npriv_freep(&p->steps);
    npriv_freep(&p->buffers);
    npriv_freep(pp);
}

struct nau_engine *nau_engine_create(void)
{
    struct nau_engine *s = npriv_try_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    atomic_init(&s->busy, 0);
    atomic_init(&s->pending, NULL);
    return s;
}

int nau_engine_init(struct nau_engine *s, const struct nau_engine_config *config)
{
    if (!s || s->silence)
        return NAU_ERROR_INVALID;

    if (!config)
        return NAU_ERROR_INVALID;

    if (!config->sample_rate || config->sample_rate > NAU_MAX_SAMPLE_RATE)
        return NAU_ERROR_INVALID;

    if (!config->channels || config->channels > NAU_MAX_CHANNELS)
        return NAU_ERROR_INVALID;

    if (!config->block_frames || config->block_frames > MAX_BLOCK_FRAMES)
        return NAU_ERROR_INVALID;

    if (!atomic_is_lock_free(&s->busy) || !atomic_is_lock_free(&s->pending))
        return NAU_ERROR_UNSUPPORTED;

    float *silence = npriv_try_calloc((size_t)config->channels * config->block_frames, sizeof(*silence));
    if (!silence)
        return NAU_ERROR_MEMORY;
    s->config = *config;
    s->silence = silence;
    return NAU_OK;
}

const struct nau_engine_config *nau_engine_get_config(const struct nau_engine *s)
{
    return s && s->silence ? &s->config : NULL;
}

void nau_engine_freep(struct nau_engine **sp)
{
    if (!sp || !*sp)
        return;
    struct nau_engine *s = *sp;
    struct render_plan *pending = atomic_load_explicit(&s->pending, memory_order_relaxed);
    render_plan_freep(&pending);
    render_plan_freep(&s->active);
    render_plan_freep(&s->retired);
    npriv_freep(&s->silence);
    npriv_freep(sp);
}

int nau_engine_collect(struct nau_engine *s)
{
    if (!s || !s->silence)
        return NAU_ERROR_INVALID;
    if (atomic_load_explicit(&s->busy, memory_order_acquire))
        return NAU_AGAIN;
    render_plan_freep(&s->retired);
    return NAU_OK;
}

/*
 * Traversal stack entry: a route and its next input to visit.
 */
struct visit {
    uint32_t index;
    uint32_t input;
};

/*
 * Build a plan on the control thread, without publishing it. The caller
 * supplies a zeroed plan and checks the route count and output index. On
 * failure, render_plan_freep() releases the partially built plan.
 */
static int render_plan_init(struct render_plan *p, struct nau_engine *s, const struct nau_route *routes,
                            uint32_t count, uint32_t output)
{
    /*
     * Stamp each node with this attempt's serial to detect duplicate routes
     * in one pass, without a separate set or a pass to clear the stamps.
     * Failed attempts consume a serial too. Reject overflow so an old stamp
     * cannot be mistaken for one from this attempt. Only the control thread
     * accesses these counters.
     */
    if (s->compile_serial == UINT64_MAX)
        return NAU_ERROR_UNSUPPORTED;
    const uint64_t serial = ++s->compile_serial;
    for (uint32_t i = 0; i < count; i++) {
        struct nau_node *node = routes[i].node;
        if (!node || node->engine != s)
            return NAU_ERROR_INVALID;

        if (node->compile_serial == serial)
            return NAU_ERROR_INVALID;

        if (node->processor.input_count && !routes[i].inputs)
            return NAU_ERROR_INVALID;

        node->compile_serial = serial;
        for (uint32_t j = 0; j < node->processor.input_count; j++) {
            uint32_t dep = routes[i].inputs[j];
            if (dep != NAU_INPUT_NONE && dep >= count)
                return NAU_ERROR_INVALID;
        }
    }

    int ret = NAU_OK;
    uint32_t *order = npriv_try_calloc(count + 1, sizeof(*order));
    uint32_t *map = npriv_try_calloc(count + 1, sizeof(*map));
    uint8_t *color = npriv_try_calloc(count + 1, sizeof(*color));
    struct visit *stack = npriv_try_calloc(count + 1, sizeof(*stack));
    if (!order || !map || !color || !stack) {
        ret = NAU_ERROR_MEMORY;
        goto end;
    }
    /*
     * Order the routes reachable from the output so that each one comes
     * after its inputs: depth-first traversal, without recursion. A route is
     * colored 1 while it is on the stack, 2 once ordered: reaching a route
     * colored 1 means a cycle.
     */
    uint32_t used = 0, depth = 0;
    if (count) {
        stack[depth++] = (struct visit){.index = output};
        color[output] = 1;
    }
    while (depth) {
        struct visit *v = &stack[depth - 1];
        const struct nau_route *route = &routes[v->index];
        if (v->input == route->node->processor.input_count) {
            map[v->index] = used;
            order[used++] = v->index;
            color[v->index] = 2;
            depth--;
            continue;
        }
        uint32_t dep = route->inputs[v->input++];
        if (dep == NAU_INPUT_NONE || color[dep] == 2)
            continue;
        if (color[dep] == 1) {
            ret = NAU_ERROR_INVALID;
            goto end;
        }
        color[dep] = 1;
        stack[depth++] = (struct visit){.index = dep};
    }
    const size_t samples = (size_t)s->config.channels * s->config.block_frames;
    p->steps = npriv_try_calloc(used + 1, sizeof(*p->steps));
    p->buffers = npriv_try_calloc(used + 1, samples * sizeof(float));
    if (!p->steps || !p->buffers) {
        ret = NAU_ERROR_MEMORY;
        goto end;
    }
    for (uint32_t i = 0; i < used; i++) {
        const struct nau_route *route = &routes[order[i]];
        struct render_step *step = &p->steps[i];
        step->node = route->node;
        step->node->refs++;
        p->step_count++;
        step->user = step->node->processor.user;
        step->origin = step->node->origin;
        if (step->node->processor.process == source_process) {
            step->source = *(const struct nau_source *)step->user;
            step->user = &step->source;
        } else if (step->node->snapshot_callbacks) {
            step->snapshot = step->node->snapshot_callbacks->take(step->user);
            if (!step->snapshot) {
                ret = NAU_ERROR_MEMORY;
                goto end;
            }
            step->release = step->node->snapshot_callbacks->release;
            step->user = step->snapshot;
        }
        step->output = p->buffers + i * samples;
        const uint32_t n = step->node->processor.input_count;
        step->inputs = npriv_try_calloc(n + 1, sizeof(*step->inputs));
        if (!step->inputs) {
            ret = NAU_ERROR_MEMORY;
            goto end;
        }
        for (uint32_t j = 0; j < n; j++) {
            uint32_t dep = route->inputs[j];
            step->inputs[j] = dep == NAU_INPUT_NONE ? s->silence : p->buffers + map[dep] * samples;
        }
    }
end:
    npriv_freep(&order);
    npriv_freep(&map);
    npriv_freep(&color);
    npriv_freep(&stack);
    return ret;
}

int nau_engine_commit(struct nau_engine *s, const struct nau_route *routes,
                      uint32_t count, uint32_t output)
{
    if (!s || !s->silence)
        return NAU_ERROR_INVALID;

    if (count > MAX_NODES)
        return NAU_ERROR_INVALID;

    if (count && !routes)
        return NAU_ERROR_INVALID;

    if (count && output >= count)
        return NAU_ERROR_INVALID;

    int ret = nau_engine_collect(s);
    if (ret != NAU_OK)
        return ret;

    struct render_plan *p = npriv_try_calloc(1, sizeof(*p));
    if (!p)
        return NAU_ERROR_MEMORY;
    ret = render_plan_init(p, s, routes, count, output);
    if (ret < 0) {
        render_plan_freep(&p);
        return ret;
    }

    /*
     * Set busy before publishing: the renderer may adopt the plan immediately.
     * Only its release store clearing busy makes retired safe to collect.
     */
    atomic_store_explicit(&s->busy, 1, memory_order_relaxed);
    atomic_store_explicit(&s->pending, p, memory_order_release);
    return NAU_OK;
}

static void adopt_pending(struct nau_engine *s)
{
    struct render_plan *pending = atomic_load_explicit(&s->pending, memory_order_acquire);
    if (pending) {
        s->retired = s->active;
        s->active = pending;
        atomic_store_explicit(&s->pending, NULL, memory_order_relaxed);
        atomic_store_explicit(&s->busy, 0, memory_order_release);
    }
}

static void source_seek(const struct nau_source *s, int64_t frame, int wrap)
{
    if (!s->seek)
        return;
    int64_t offset = frame > s->timeline_start ? frame - s->timeline_start : 0;
    if (offset > s->duration)
        offset = s->duration;
    s->seek(s->user, s->source_start + offset, wrap);
}

/*
 * Reset a node (or seek a source) before rendering it from `frame`, if this
 * is a jump: the node was never rendered, or its last block ended at another
 * frame. `force` resets it in any case. The first positioning of a source is
 * never a loop wrap.
 */
static void jump(struct render_step *step, int64_t frame, int wrap, int force)
{
    struct nau_node *node = step->node;
    if (node->initialized && node->next_frame == frame && !force)
        return;
    if (node->processor.process == source_process)
        source_seek(&step->source, frame, wrap && node->initialized);
    else if (node->processor.reset)
        node->processor.reset(node->processor.user, frame);
    node->initialized = true;
}

void nau_engine_prepare(struct nau_engine *s, int64_t frame, int wrap, int force)
{
    adopt_pending(s);
    struct render_plan *p = s->active;
    for (uint32_t i = 0; p && i < p->step_count; i++) {
        jump(&p->steps[i], frame, wrap, force);
        p->steps[i].node->next_frame = frame;
    }
}

void nau_engine_reset_processors(struct nau_engine *s, int64_t frame)
{
    const struct render_plan *p = s->active;
    for (uint32_t i = 0; p && i < p->step_count; i++) {
        const struct nau_node *node = p->steps[i].node;
        /* A node never positioned is reset by its first block */
        if (!node->initialized || node->processor.process == source_process)
            continue;

        if (node->processor.reset)
            node->processor.reset(node->processor.user, frame);
    }
}

int nau_engine_ready(struct nau_engine *s, int64_t frame, uint32_t frames)
{
    const struct render_plan *p = s->active;
    for (uint32_t i = 0; p && i < p->step_count; i++) {
        const struct render_step *step = &p->steps[i];
        const struct nau_source *source = &step->source;
        int64_t lo, hi;
        if (step->node->processor.process != source_process || !source->ready)
            continue;

        if (!source_span(source, frame, frames, &lo, &hi))
            continue;

        if (!source->ready(source->user, source->source_start + (lo - source->timeline_start)))
            return NAU_AGAIN;
    }
    return NAU_OK;
}

int nau_engine_render(struct nau_engine *s, int64_t first, uint32_t frames, float *output)
{
    return nau_engine_render_jump(s, first, frames, output, 0, 1);
}

int nau_engine_render_jump(struct nau_engine *s, int64_t first, uint32_t frames, float *output, int wrap,
                           int adopt)
{
    if (!s || !s->silence)
        return NAU_ERROR_INVALID;

    if (first < 0 || first > INT64_MAX - frames)
        return NAU_ERROR_INVALID;

    if (frames && !output)
        return NAU_ERROR_INVALID;

    if ((size_t)frames > SIZE_MAX / sizeof(float) / s->config.channels)
        return NAU_ERROR_INVALID;

    int result = NAU_OK;
    while (frames) {
        if (adopt)
            adopt_pending(s);
        const uint32_t n = NPRIV_MIN(frames, s->config.block_frames);
        const size_t bytes = (size_t)n * s->config.channels * sizeof(float);
        struct render_plan *p = s->active;
        for (uint32_t i = 0; p && i < p->step_count; i++) {
            struct render_step *step = &p->steps[i];
            struct nau_node *node = step->node;
            jump(step, first, wrap, 0);
            const struct nau_block block = {
                .frame       = first,
                .frames      = n,
                .channels    = s->config.channels,
                .sample_rate = s->config.sample_rate,
                .input_count = node->processor.input_count,
                .inputs      = step->inputs,
                .output      = step->output,
                .origin      = step->origin,
            };
            memset(step->output, 0, bytes);
            int ret = node->processor.process(step->user, &block);
            if (ret < 0) {
                /* A failed node outputs silence, the others are still rendered */
                memset(step->output, 0, bytes);
                result = ret;
            } else if (ret == NAU_AGAIN && result == NAU_OK) {
                result = NAU_AGAIN;
            }
            node->next_frame = first + n;
        }
        if (p && p->step_count)
            memcpy(output, p->steps[p->step_count - 1].output, bytes);
        else
            memset(output, 0, bytes);
        first += n;
        frames -= n;
        wrap = 0; /* Only the first block can wrap to the start of the loop. */
        output += (size_t)n * s->config.channels;
    }
    return result;
}

struct nau_node *nau_node_create(void)
{
    struct nau_node *node = npriv_try_calloc(1, sizeof(*node));
    if (node)
        node->refs = 1;
    return node;
}

int nau_node_init(struct nau_node *node, struct nau_engine *s, const struct nau_processor *processor)
{
    return nau_node_init_with_snapshot_callbacks(node, s, processor, NULL);
}

int nau_node_init_with_snapshot_callbacks(struct nau_node *node, struct nau_engine *s,
                                          const struct nau_processor *processor,
                                          const struct nau_snapshot_callbacks *snapshot_callbacks)
{
    if (!node || node->engine)
        return NAU_ERROR_INVALID;

    if (!s || !s->silence)
        return NAU_ERROR_INVALID;

    if (!processor || !processor->process)
        return NAU_ERROR_INVALID;

    if (processor->input_count > MAX_INPUTS)
        return NAU_ERROR_INVALID;

    if (snapshot_callbacks && (!snapshot_callbacks->take || !snapshot_callbacks->release))
        return NAU_ERROR_INVALID;

    node->engine = s;
    node->processor = *processor;
    node->snapshot_callbacks = snapshot_callbacks;
    return NAU_OK;
}

void *nau_node_get_user(const struct nau_node *node, int (*process)(void *user, const struct nau_block *p))
{
    return node && node->processor.process == process ? node->processor.user : NULL;
}

int nau_node_set_origin(struct nau_node *node, int64_t frame)
{
    if (!node || !node->engine)
        return NAU_ERROR_INVALID;

    if (frame < 0)
        return NAU_ERROR_INVALID;

    node->origin = frame;
    return NAU_OK;
}

void nau_node_unrefp(struct nau_node **np)
{
    if (!np || !*np)
        return;
    struct nau_node *n = *np;
    *np = NULL;
    if (--n->refs)
        return;
    if (n->processor.destroy)
        n->processor.destroy(n->processor.user);
    npriv_freep(&n);
}

static int source_process(void *user, const struct nau_block *p)
{
    const struct nau_source *s = user;
    int64_t lo, hi;
    if (!source_span(s, p->frame, p->frames, &lo, &hi))
        return NAU_OK;
    return s->read(s->user, s->source_start + (lo - s->timeline_start),
                   (uint32_t)(hi - lo), p->output + (size_t)(lo - p->frame) * p->channels);
}

static void source_destroy(void *user)
{
    struct nau_source *s = user;
    if (s->destroy)
        s->destroy(s->user);
    npriv_freep(&s);
}

int nau_source_init(struct nau_node *node, struct nau_engine *s, const struct nau_source *source)
{
    if (!source || !source->read)
        return NAU_ERROR_INVALID;

    if (source->timeline_start < 0 || source->source_start < 0 || source->duration < 0)
        return NAU_ERROR_INVALID;

    if (source->timeline_start > INT64_MAX - source->duration ||
        source->source_start > INT64_MAX - source->duration)
        return NAU_ERROR_INVALID;

    struct nau_source *copy = npriv_try_malloc(sizeof(*copy));
    if (!copy)
        return NAU_ERROR_MEMORY;
    *copy = *source;

    const struct nau_processor processor = {
        .user    = copy,
        .process = source_process,
        .destroy = source_destroy,
    };
    int ret = nau_node_init(node, s, &processor);
    if (ret < 0)
        npriv_freep(&copy);
    return ret;
}

int nau_source_set_range(struct nau_node *node, int64_t start, int64_t offset, int64_t duration)
{
    if (!node || node->processor.process != source_process)
        return NAU_ERROR_INVALID;

    if (start < 0 || offset < 0 || duration < 0)
        return NAU_ERROR_INVALID;

    if (start > INT64_MAX - duration || offset > INT64_MAX - duration)
        return NAU_ERROR_INVALID;

    struct nau_source *s = node->processor.user;
    s->timeline_start = start;
    s->source_start = offset;
    s->duration = duration;
    return NAU_OK;
}
