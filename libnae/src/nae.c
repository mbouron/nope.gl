/* SPDX-License-Identifier: Apache-2.0 */
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "nae/nae.h"

#define MAX_NODES 4096u
#define MAX_INPUTS 64u

struct nae_node {
    struct nae_engine *engine;
    struct nae_processor processor;
    uint32_t refs;                 /* Control thread only. */
    uint64_t compile_serial;       /* Duplicate detection, control only. */
    int64_t next_frame;            /* Render thread only. */
    bool initialized;
};

struct step {
    struct nae_node *node;
    const float **inputs;
    float *output;
    void *user;
    struct nae_source source;
};

static int source_process(void *user, const struct nae_process *p);

struct plan {
    struct step *steps;
    uint32_t nb_steps;
    float *buffers;
};

struct nae_engine {
    struct nae_config config;
    float *silence;
    struct plan *active;           /* Render thread only. */
    struct plan *retired;          /* Control reads only after busy==0. */
    _Atomic(struct plan *) pending;
    _Atomic uint32_t busy;
    uint64_t compile_serial;
};

static void *alloc_array(size_t count, size_t size)
{
    return size && count > SIZE_MAX / size ? NULL : calloc(count, size);
}

static void free_plan(struct plan *p)
{
    if (!p)
        return;
    for (uint32_t i = 0; i < p->nb_steps; i++) {
        nae_node_unrefp(&p->steps[i].node);
        free(p->steps[i].inputs);
    }
    free(p->steps);
    free(p->buffers);
    free(p);
}

int nae_engine_create(const struct nae_config *config, struct nae_engine **engine)
{
    if (!engine)
        return NAE_ERROR_INVALID;
    *engine = NULL;
    if (!config || !config->sample_rate || config->sample_rate > 768000 ||
        !config->channels || config->channels > 8 ||
        !config->block_frames || config->block_frames > 8192)
        return NAE_ERROR_INVALID;
    struct nae_engine *s = calloc(1, sizeof(*s));
    if (!s)
        return NAE_ERROR_MEMORY;
    atomic_init(&s->busy, 0);
    atomic_init(&s->pending, NULL);
    if (!atomic_is_lock_free(&s->busy) || !atomic_is_lock_free(&s->pending)) {
        free(s);
        return NAE_ERROR_UNSUPPORTED;
    }
    s->config = *config;
    s->silence = alloc_array((size_t)config->channels * config->block_frames, sizeof(float));
    if (!s->silence) {
        free(s);
        return NAE_ERROR_MEMORY;
    }
    *engine = s;
    return NAE_OK;
}

const struct nae_config *nae_engine_get_config(const struct nae_engine *s)
{
    return s ? &s->config : NULL;
}

void nae_engine_freep(struct nae_engine **sp)
{
    if (!sp || !*sp)
        return;
    struct nae_engine *s = *sp;
    free_plan(atomic_load_explicit(&s->pending, memory_order_relaxed));
    free_plan(s->active);
    free_plan(s->retired);
    free(s->silence);
    free(s);
    *sp = NULL;
}

int nae_engine_collect(struct nae_engine *s)
{
    if (!s)
        return NAE_ERROR_INVALID;
    if (atomic_load_explicit(&s->busy, memory_order_acquire))
        return NAE_AGAIN;
    free_plan(s->retired);
    s->retired = NULL;
    return NAE_OK;
}

/* Iterative depth-first traversal: O(nodes + input edges), no recursion. */
struct visit { uint32_t index, input; };

int nae_engine_commit(struct nae_engine *s, const struct nae_route *routes,
                      uint32_t count, uint32_t output)
{
    if (!s || count > MAX_NODES || (count && (!routes || output >= count)))
        return NAE_ERROR_INVALID;
    int ret = nae_engine_collect(s);
    if (ret != NAE_OK)
        return ret;
    if (s->compile_serial == UINT64_MAX)
        return NAE_ERROR_UNSUPPORTED;
    const uint64_t serial = ++s->compile_serial;
    for (uint32_t i = 0; i < count; i++) {
        struct nae_node *node = routes[i].node;
        if (!node || node->engine != s || node->compile_serial == serial ||
            (node->processor.nb_inputs && !routes[i].inputs))
            return NAE_ERROR_INVALID;
        node->compile_serial = serial;
        for (uint32_t j = 0; j < node->processor.nb_inputs; j++) {
            uint32_t dep = routes[i].inputs[j];
            if (dep != NAE_INPUT_NONE && dep >= count)
                return NAE_ERROR_INVALID;
        }
    }

    struct plan *p = calloc(1, sizeof(*p));
    uint32_t *order = alloc_array(count + 1, sizeof(*order));
    uint32_t *map = alloc_array(count + 1, sizeof(*map));
    uint8_t *color = alloc_array(count + 1, sizeof(*color));
    struct visit *stack = alloc_array(count + 1, sizeof(*stack));
    if (!p || !order || !map || !color || !stack) {
        ret = NAE_ERROR_MEMORY;
        goto end;
    }
    uint32_t used = 0, depth = 0;
    if (count) {
        stack[depth++] = (struct visit){.index = output};
        color[output] = 1;
    }
    while (depth) {
        struct visit *v = &stack[depth - 1];
        const struct nae_route *route = &routes[v->index];
        if (v->input == route->node->processor.nb_inputs) {
            map[v->index] = used;
            order[used++] = v->index;
            color[v->index] = 2;
            depth--;
            continue;
        }
        uint32_t dep = route->inputs[v->input++];
        if (dep == NAE_INPUT_NONE || color[dep] == 2)
            continue;
        if (color[dep] == 1) {
            ret = NAE_ERROR_INVALID;
            goto end;
        }
        color[dep] = 1;
        stack[depth++] = (struct visit){.index = dep};
    }
    const size_t samples = (size_t)s->config.channels * s->config.block_frames;
    p->steps = alloc_array(used + 1, sizeof(*p->steps));
    p->buffers = alloc_array(used + 1, samples * sizeof(float));
    if (!p->steps || !p->buffers) {
        ret = NAE_ERROR_MEMORY;
        goto end;
    }
    for (uint32_t i = 0; i < used; i++) {
        const struct nae_route *route = &routes[order[i]];
        struct step *step = &p->steps[i];
        step->node = route->node;
        step->node->refs++;
        p->nb_steps++;
        step->user = step->node->processor.user;
        if (step->node->processor.process == source_process) {
            step->source = *(const struct nae_source *)step->user;
            step->user = &step->source;
        }
        step->output = p->buffers + i * samples;
        const uint32_t n = step->node->processor.nb_inputs;
        step->inputs = alloc_array(n + 1, sizeof(*step->inputs));
        if (!step->inputs) {
            ret = NAE_ERROR_MEMORY;
            goto end;
        }
        for (uint32_t j = 0; j < n; j++) {
            uint32_t dep = route->inputs[j];
            step->inputs[j] = dep == NAE_INPUT_NONE ? s->silence : p->buffers + map[dep] * samples;
        }
    }
    /* busy stays set until the renderer has retired the old plan. In
     * particular, an empty pending slot alone is NOT a retirement ack. */
    atomic_store_explicit(&s->busy, 1, memory_order_relaxed);
    atomic_store_explicit(&s->pending, p, memory_order_release);
    p = NULL;
    ret = NAE_OK;
end:
    free_plan(p);
    free(order);
    free(map);
    free(color);
    free(stack);
    return ret;
}

int nae_engine_render(struct nae_engine *s, int64_t first, uint32_t frames, float *output)
{
    if (!s || first < 0 || first > INT64_MAX - frames || (frames && !output) ||
        (size_t)frames > SIZE_MAX / sizeof(float) / s->config.channels)
        return NAE_ERROR_INVALID;
    int result = NAE_OK;
    while (frames) {
        struct plan *pending = atomic_load_explicit(&s->pending, memory_order_acquire);
        if (pending) {
            s->retired = s->active;
            s->active = pending;
            atomic_store_explicit(&s->pending, NULL, memory_order_relaxed);
            atomic_store_explicit(&s->busy, 0, memory_order_release);
        }
        const uint32_t n = frames < s->config.block_frames ? frames : s->config.block_frames;
        const size_t bytes = (size_t)n * s->config.channels * sizeof(float);
        struct plan *p = s->active;
        for (uint32_t i = 0; p && i < p->nb_steps; i++) {
            struct step *step = &p->steps[i];
            struct nae_node *node = step->node;
            if (!node->initialized || node->next_frame != first) {
                if (node->processor.reset)
                    node->processor.reset(node->processor.user, first);
                node->initialized = true;
            }
            const struct nae_process process = {
                .frame = first, .frames = n,
                .channels = s->config.channels, .sample_rate = s->config.sample_rate,
                .nb_inputs = node->processor.nb_inputs,
                .inputs = step->inputs, .output = step->output,
            };
            memset(step->output, 0, bytes);
            const int ret = node->processor.process(step->user, &process);
            if (ret < 0) {
                memset(step->output, 0, bytes);
                result = ret;
            } else if (ret == NAE_AGAIN && result == NAE_OK) {
                result = NAE_AGAIN;
            }
            node->next_frame = first + n;
        }
        if (p && p->nb_steps)
            memcpy(output, p->steps[p->nb_steps - 1].output, bytes);
        else
            memset(output, 0, bytes);
        first += n;
        frames -= n;
        output += (size_t)n * s->config.channels;
    }
    return result;
}

int nae_node_create(struct nae_engine *s, const struct nae_processor *processor, struct nae_node **node)
{
    if (!node)
        return NAE_ERROR_INVALID;
    *node = NULL;
    if (!s || !processor || !processor->process || processor->nb_inputs > MAX_INPUTS)
        return NAE_ERROR_INVALID;
    struct nae_node *n = calloc(1, sizeof(*n));
    if (!n)
        return NAE_ERROR_MEMORY;
    n->engine = s;
    n->processor = *processor;
    n->refs = 1;
    *node = n;
    return NAE_OK;
}

void nae_node_unrefp(struct nae_node **np)
{
    if (!np || !*np)
        return;
    struct nae_node *n = *np;
    *np = NULL;
    if (--n->refs)
        return;
    if (n->processor.destroy)
        n->processor.destroy(n->processor.user);
    free(n);
}

static int source_process(void *user, const struct nae_process *p)
{
    const struct nae_source *s = user;
    const int64_t end = s->timeline_start + s->duration;
    const int64_t lo = p->frame > s->timeline_start ? p->frame : s->timeline_start;
    const int64_t hi = p->frame + p->frames < end ? p->frame + p->frames : end;
    if (lo >= hi)
        return NAE_OK;
    return s->read(s->user, s->source_start + (lo - s->timeline_start),
                   (uint32_t)(hi - lo), p->output + (size_t)(lo - p->frame) * p->channels);
}

static void source_destroy(void *user)
{
    struct nae_source *s = user;
    if (s->destroy)
        s->destroy(s->user);
    free(s);
}

int nae_source_create(struct nae_engine *s, const struct nae_source *source, struct nae_node **node)
{
    if (!source || !source->read || source->timeline_start < 0 || source->source_start < 0 ||
        source->duration < 0 || source->timeline_start > INT64_MAX - source->duration ||
        source->source_start > INT64_MAX - source->duration)
        return NAE_ERROR_INVALID;
    struct nae_source *copy = malloc(sizeof(*copy));
    if (!copy)
        return NAE_ERROR_MEMORY;
    *copy = *source;
    const struct nae_processor processor = {
        .user = copy, .process = source_process, .destroy = source_destroy,
    };
    int ret = nae_node_create(s, &processor, node);
    if (ret < 0)
        free(copy);
    return ret;
}

int nae_source_set_range(struct nae_node *node, int64_t start, int64_t offset, int64_t duration)
{
    if (!node || node->processor.process != source_process || start < 0 || offset < 0 ||
        duration < 0 || start > INT64_MAX - duration || offset > INT64_MAX - duration)
        return NAE_ERROR_INVALID;
    struct nae_source *s = node->processor.user;
    s->timeline_start = start;
    s->source_start = offset;
    s->duration = duration;
    return NAE_OK;
}

struct gain {
    _Atomic uint32_t target_bits;
    uint32_t ramp_frames, remaining;
    float target, current;
};

static uint32_t float_bits(float value)
{
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(value));
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float gain_target(struct gain *g)
{
    uint32_t bits = atomic_load_explicit(&g->target_bits, memory_order_relaxed);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void gain_reset(void *user, int64_t frame)
{
    (void)frame;
    struct gain *g = user;
    g->current = g->target = gain_target(g);
    g->remaining = 0;
}

static int gain_process(void *user, const struct nae_process *p)
{
    struct gain *g = user;
    float target = gain_target(g);
    if (target != g->target) {
        g->target = target;
        g->remaining = g->ramp_frames;
    }
    for (uint32_t i = 0; i < p->frames; i++) {
        if (g->remaining) {
            g->current += (g->target - g->current) / (float)g->remaining;
            g->remaining--;
        } else {
            g->current = g->target;
        }
        for (uint32_t c = 0; c < p->channels; c++) {
            const size_t index = (size_t)i * p->channels + c;
            p->output[index] = p->inputs[0][index] * g->current;
        }
    }
    return NAE_OK;
}

int nae_gain_create(struct nae_engine *s, float value, uint32_t ramp_frames, struct nae_node **node)
{
    if (!isfinite(value) || value < 0)
        return NAE_ERROR_INVALID;
    struct gain *g = calloc(1, sizeof(*g));
    if (!g)
        return NAE_ERROR_MEMORY;
    atomic_init(&g->target_bits, float_bits(value));
    if (!atomic_is_lock_free(&g->target_bits)) {
        free(g);
        return NAE_ERROR_UNSUPPORTED;
    }
    g->ramp_frames = ramp_frames;
    const struct nae_processor processor = {
        .nb_inputs = 1, .user = g, .process = gain_process, .reset = gain_reset, .destroy = free,
    };
    int ret = nae_node_create(s, &processor, node);
    if (ret < 0)
        free(g);
    return ret;
}

int nae_gain_set(struct nae_node *node, float gain)
{
    if (!node || node->processor.process != gain_process || !isfinite(gain) || gain < 0)
        return NAE_ERROR_INVALID;
    struct gain *g = node->processor.user;
    atomic_store_explicit(&g->target_bits, float_bits(gain), memory_order_relaxed);
    return NAE_OK;
}

static int mix_process(void *user, const struct nae_process *p)
{
    (void)user;
    for (uint32_t bus = 0; bus < p->nb_inputs; bus++)
        for (size_t i = 0; i < (size_t)p->frames * p->channels; i++)
            p->output[i] += p->inputs[bus][i];
    return NAE_OK;
}

int nae_mix_create(struct nae_engine *s, uint32_t nb_inputs, struct nae_node **node)
{
    const struct nae_processor processor = {.nb_inputs = nb_inputs, .process = mix_process};
    return nae_node_create(s, &processor, node);
}
