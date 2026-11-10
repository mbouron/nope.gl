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

#include <math.h>
#include <stdatomic.h>
#include <stdckdint.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nopeaudio/nopeaudio.h"
#include "effect/effect.h"
#include "export/export.h"
#include "ramp.h"
#include "utils/memory.h"
#include "utils/pthread_compat.h"
#include "utils/time.h"
#include "utils/utils.h"

/*
 * The player assembles an engine and a transport, with a sink for device output.
 *
 * Each track feeds the mix through its fader, volume and optional effects:
 *
 *    source --> [effects] --> fader --> gain --> [effects] --.
 *                                                            +--> mix --> master
 *    source --> [effects] --> fader --> gain --> [effects] --'
 *
 * Track edits publish a new graph. A commit can fail, including with NAU_AGAIN
 * while the previous commit is pending, so edits must leave the track intact
 * until publication succeeds. Changes are prepared in a local copy of the
 * track structure, initially pointing to the same nodes:
 *
 *    Copy the track structure
 *                |
 *                v
 *    Apply changes to the copy
 *                |
 *                v
 *    Commit the graph using the copy
 *                |
 *        +-------+------------------------------+
 *        |                                      |
 *        v                                      v
 *    Success:                               Failure:
 *    - Copy edited fields to the track      - Restore changed node settings
 *    - Release replaced nodes               - Release new nodes
 *                                           - Keep the original track
 *
 * A fader has no persistent state and can be replaced when its ramps change.
 *
 * An effect keeps its node to preserve filter history across edits. Its
 * configuration is snapshotted at commit, so automation can be swapped into
 * the node and swapped back without allocation if the commit fails.
 * Source ranges and node origins are also restored on failure.
 *
 * Automation is relative to the track's start, used as the origin of its
 * nodes: moving the track does not require changing its ramps. Volume and
 * effect parameter targets are atomic and need no commit.
 *
 * When rendering is stopped, the control thread calls nau_transport_sync()
 * after commits and seeks to adopt the graph and let sources decode ahead.
 * Offline mode also uses this path and requires serialized render and control
 * calls.
 */

#define BLOCK_FRAMES 256
#define DECLICK_MS 5          /* fades around the discontinuities of the transport */
#define READY_TIMEOUT_MS 250  /* readiness timeout when playback starts from silence */
#define VOLUME_RAMP_MS 5      /* ramp of a volume change */
#define PARAM_RAMP_MS 20      /* ramp of an effect parameter change */
#define MAX_TRACKS 64
#define MAX_EFFECTS 16        /* per track */
#define EXPORT_FRAMES 4096

struct nau_track_effect {
    struct nau_node *node;
};

struct nau_track {
    struct nau_media *media;
    struct nau_node *source;
    struct nau_node *fader;
    struct nau_node *gain;
    /*
     * Effects ordered by placement, regardless of effect type:
     * - The first pre_fader_effect_count use
     *   NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER and run before volume automation
     *   (fader) and track volume (gain).
     * - The rest use NAU_TRACK_EFFECT_PLACEMENT_POST_FADER and run after both
     *   volume controls.
     *
     * For example, a compressor before these controls reacts to the source
     * level. After them, volume changes also affect how much compression is
     * applied.
     */
    struct nau_track_effect *effects[MAX_EFFECTS];
    uint32_t effect_count;
    uint32_t pre_fader_effect_count;
    int64_t start;
    int64_t trim;
    int64_t duration;         /* frames played, limited to the length of the media */
    int64_t length;           /* length of the media, 0 if unknown */
};

struct nau_player {
    enum nau_player_mode mode;
    uint32_t sample_rate;
    uint32_t period_ms;
    /* Protects the tracks from concurrent reads by nau_player_get_stats(). */
    pthread_mutex_t lock;
    bool lock_ready;
    struct nau_engine *engine;
    struct nau_transport *transport;
    struct nau_sink *sink;
    _Atomic bool sink_started;     /* written by the control thread, read by the position getters */
    _Atomic bool host_rendering;
    /* Sums over the tracks, as of the last nau_player_get_stats() which got the lock */
    _Atomic int64_t silent_frames;
    _Atomic int64_t skipped_frames;
    _Atomic int64_t requests;
    _Atomic int64_t errors;
    struct nau_node *master;
    struct nau_track *tracks[MAX_TRACKS];
    uint32_t track_count;
    float volume;
    int64_t duration;
    int looping;
    int64_t loop_start;
    int64_t loop_end;
};

static int64_t frames_in(const struct nau_player *p, int64_t ms)
{
    return ms * p->sample_rate / 1000;
}

/*
 * Tell whether a render thread (device or host) is running. If so, it adopts
 * the commits and applies the seeks; otherwise the control thread has to.
 */
static bool rendering(const struct nau_player *p)
{
    return atomic_load_explicit(&p->sink_started, memory_order_acquire) ||
           atomic_load_explicit(&p->host_rendering, memory_order_acquire);
}

/* Index of a track in the player, or track_count if the player does not have it */
static uint32_t track_index(const struct nau_player *p, const struct nau_track *t)
{
    uint32_t i = 0;
    while (i < p->track_count && p->tracks[i] != t)
        i++;
    return i;
}

static bool owns(const struct nau_player *p, const struct nau_track *t)
{
    return track_index(p, t) < p->track_count;
}

/* Track to publish at an index: `replacement` in place of `original` */
static const struct nau_track *laid_track(const struct nau_player *p, uint32_t index,
                                          const struct nau_track *original, const struct nau_track *replacement)
{
    return p->tracks[index] == original ? replacement : p->tracks[index];
}

/* Effects, fader and gain; excludes the source and mix. */
static uint32_t chain_length(const struct nau_track *t)
{
    return t->effect_count + 2;
}

struct effect_range {
    uint32_t first;
    uint32_t count;
};

static struct effect_range get_placement_range(const struct nau_track *t, enum nau_track_effect_placement placement)
{
    const uint32_t pre_fader_count = t->pre_fader_effect_count;
    if (placement == NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER)
        return (struct effect_range){0, pre_fader_count};
    return (struct effect_range){pre_fader_count, t->effect_count - pre_fader_count};
}

/* Routes of a graph being built */
struct graph {
    struct nau_route *routes;
    uint32_t route_count;
    uint32_t *input;          /* next free input */
};

static void add_route(struct graph *g, struct nau_node *node, const uint32_t *inputs)
{
    g->routes[g->route_count++] = (struct nau_route){node, inputs};
}

/* Add a node fed by the previous route */
static void chain_node(struct graph *g, struct nau_node *node)
{
    *g->input = g->route_count - 1;
    add_route(g, node, g->input++);
}

static void chain_effects(struct graph *g, const struct nau_track *t, struct effect_range range)
{
    for (uint32_t i = range.first; i < range.first + range.count; i++)
        chain_node(g, t->effects[i]->node);
}

static int set_origins(const struct nau_track *t)
{
    int ret = nau_node_set_origin(t->fader, t->start);
    if (ret < 0)
        return ret;

    for (uint32_t i = 0; i < t->effect_count; i++) {
        ret = nau_node_set_origin(t->effects[i]->node, t->start);
        if (ret < 0)
            return ret;
    }
    return NAU_OK;
}

/*
 * Publish the graph of the player: the chain of every track into the mix,
 * and the mix into the master volume. `replacement` is used in place of the
 * track `original`, if any (see the edits of the tracks).
 */
static int commit(struct nau_player *p, const struct nau_track *original, const struct nau_track *replacement)
{
    const uint32_t n = p->track_count;
    /*
     * Each track needs one route for its source, then one route and one
     * input per chained node. The mix and master gain add two routes,
     * with one input per track for the mix and one for the master.
     */
    size_t route_count = 2, input_count = n + 1;
    for (uint32_t i = 0; i < n; i++) {
        const struct nau_track *t = laid_track(p, i, original, replacement);
        route_count += 1 + chain_length(t);
        input_count += chain_length(t);
    }
    struct nau_route *routes = nau_try_calloc(route_count, sizeof(*routes));
    uint32_t *inputs = nau_try_calloc(input_count, sizeof(*inputs));
    struct nau_node *mix = nau_node_create();
    int ret;
    if (!routes || !inputs || !mix) {
        ret = NAU_ERROR_MEMORY;
        goto end;
    }

    ret = nau_mix_init(mix, p->engine, n);
    if (ret < 0)
        goto end;
    /*
     * `inputs` holds the inputs of the mix (one per track), followed by the
     * input of every chained node: the route before it
     */
    uint32_t *mix_inputs = inputs;
    struct graph g = {.routes = routes, .input = inputs + n};
    for (uint32_t i = 0; i < n; i++) {
        const struct nau_track *t = laid_track(p, i, original, replacement);
        ret = set_origins(t);
        if (ret < 0)
            goto end;
        add_route(&g, t->source, NULL);
        chain_effects(&g, t, get_placement_range(t, NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER));
        chain_node(&g, t->fader);
        chain_node(&g, t->gain);
        chain_effects(&g, t, get_placement_range(t, NAU_TRACK_EFFECT_PLACEMENT_POST_FADER));
        mix_inputs[i] = g.route_count - 1;
    }
    add_route(&g, mix, mix_inputs);
    chain_node(&g, p->master);
    ret = nau_engine_commit(p->engine, routes, g.route_count, g.route_count - 1);
    if (ret == NAU_OK && !rendering(p))
        nau_transport_sync(p->transport);
end:
    /* Restore the origins after a failed commit; these values are already valid. */
    if (ret != NAU_OK) {
        for (uint32_t i = 0; i < n; i++) {
            int restored = set_origins(p->tracks[i]);
            nau_assert(restored == NAU_OK);
        }
    }
    nau_node_unrefp(&mix);
    nau_freep(&routes);
    nau_freep(&inputs);
    return ret;
}

/* Use the whole project when no loop range is set. */
static void play_range(const struct nau_player *p, int64_t *start, int64_t *end)
{
    *start = 0;
    *end = p->duration;
    if (p->loop_end > p->loop_start && p->loop_start < p->duration) {
        *start = p->loop_start;
        if (p->loop_end < p->duration)
            *end = p->loop_end;
    }
}

/*
 * Tell the media of a track which part of it the loop of the project plays,
 * so that it decodes the next pass ahead
 */
static void update_loop(const struct nau_player *p, struct nau_track *t)
{
    int64_t start, end;
    play_range(p, &start, &end);
    const int64_t first = NAU_MAX(start, t->start);
    const int64_t last = NAU_MIN(end, t->start + t->duration);
    if (!p->looping || last <= first)
        nau_media_set_loop(t->media, 0, 0);
    else
        nau_media_set_loop(t->media, t->trim + (first - t->start), t->trim + (last - t->start));
}

static void update_loops(const struct nau_player *p)
{
    for (uint32_t i = 0; i < p->track_count; i++)
        update_loop(p, p->tracks[i]);
}

static int set_timings(struct nau_track *t, int64_t start, int64_t trim, int64_t duration)
{
    if (start < 0 || trim < 0)
        return NAU_ERROR_INVALID;
    if (duration < 0 && !t->length)
        return NAU_ERROR_UNSUPPORTED;
    /*
     * Play until the end of the media if asked to, or if the duration goes
     * past it
     */
    if (duration < 0 || (t->length && (trim >= t->length || duration > t->length - trim)))
        duration = t->length > trim ? t->length - trim : 0;
    if (start > INT64_MAX - duration || trim > INT64_MAX - duration)
        return NAU_ERROR_INVALID;
    t->start = start;
    t->trim = trim;
    t->duration = duration;
    return nau_source_set_range(t->source, start, trim, duration);
}

static void free_effect(struct nau_track_effect **ep)
{
    struct nau_track_effect *e = *ep;
    if (!e)
        return;
    nau_node_unrefp(&e->node);
    nau_freep(ep);
}

static void free_track(struct nau_track **tp)
{
    struct nau_track *t = *tp;
    if (!t)
        return;
    nau_node_unrefp(&t->source);
    for (uint32_t i = 0; i < t->effect_count; i++)
        free_effect(&t->effects[i]);
    nau_node_unrefp(&t->fader);
    nau_node_unrefp(&t->gain);
    nau_media_unrefp(&t->media);
    nau_freep(tp);
}

static void player_uninit(struct nau_player *p)
{
    /*
     * Stop the device first: nothing renders afterwards. The graphs of the
     * engine still hold the nodes of the tracks, which are destroyed with it.
     */
    nau_sink_freep(&p->sink);
    for (uint32_t i = 0; i < p->track_count; i++)
        free_track(&p->tracks[i]);
    p->track_count = 0;
    nau_node_unrefp(&p->master);
    nau_transport_freep(&p->transport);
    nau_engine_freep(&p->engine);
    if (p->lock_ready) {
        pthread_mutex_destroy(&p->lock);
        p->lock_ready = false;
    }
    p->sample_rate = 0;
    p->volume = 0.f;
}

struct nau_player *nau_player_create(void)
{
    struct nau_player *p = nau_try_calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    atomic_init(&p->sink_started, false);
    atomic_init(&p->host_rendering, false);
    atomic_init(&p->silent_frames, 0);
    atomic_init(&p->skipped_frames, 0);
    atomic_init(&p->requests, 0);
    atomic_init(&p->errors, 0);
    return p;
}

int nau_player_init(struct nau_player *p, const struct nau_player_config *config)
{
    if (!p || p->engine)
        return NAU_ERROR_INVALID;

    if (!config)
        return NAU_ERROR_INVALID;

    if (config->mode < NAU_PLAYER_DEVICE || config->mode > NAU_PLAYER_OFFLINE)
        return NAU_ERROR_INVALID;

    int ret = pthread_mutex_init(&p->lock, NULL);
    if (ret != 0)
        return NAU_ERROR_MEMORY;
    p->lock_ready = true;
    p->mode = config->mode;
    p->sample_rate = config->sample_rate ? config->sample_rate : 48000;
    p->period_ms = config->period_ms;
    p->volume = 1.f;

    const struct nau_engine_config engine_config = {
        .sample_rate  = p->sample_rate,
        .channels     = 2,
        .block_frames = BLOCK_FRAMES,
    };
    p->engine = nau_engine_create();
    if (!p->engine) {
        ret = NAU_ERROR_MEMORY;
        goto fail;
    }
    ret = nau_engine_init(p->engine, &engine_config);
    if (ret < 0)
        goto fail;

    const bool offline = p->mode == NAU_PLAYER_OFFLINE;
    const struct nau_transport_config transport_config = {
        .declick_frames = offline ? 0 : (uint32_t)frames_in(p, DECLICK_MS),
        .ready_timeout_frames = offline ? 0 : (uint32_t)frames_in(p, READY_TIMEOUT_MS),
    };
    p->transport = nau_transport_create();
    if (!p->transport) {
        ret = NAU_ERROR_MEMORY;
        goto fail;
    }
    ret = nau_transport_init(p->transport, p->engine, &transport_config);
    if (ret < 0)
        goto fail;

    p->master = nau_node_create();
    if (!p->master) {
        ret = NAU_ERROR_MEMORY;
        goto fail;
    }
    ret = nau_gain_init(p->master, p->engine, 1.f, (uint32_t)frames_in(p, VOLUME_RAMP_MS));
    if (ret < 0)
        goto fail;

    if (p->mode == NAU_PLAYER_DEVICE || p->mode == NAU_PLAYER_NULL_DEVICE) {
        const struct nau_sink_config sink_config = {
            .null_device = p->mode == NAU_PLAYER_NULL_DEVICE,
            .period_ms = p->period_ms,
        };
        p->sink = nau_sink_create();
        if (!p->sink) {
            ret = NAU_ERROR_MEMORY;
            goto fail;
        }
        ret = nau_sink_init(p->sink, p->transport, &sink_config);
        if (ret < 0)
            goto fail;
    }

    ret = commit(p, NULL, NULL);
    if (ret < 0)
        goto fail;

    return NAU_OK;
fail:
    player_uninit(p);
    return ret;
}

void nau_player_freep(struct nau_player **pp)
{
    if (!pp || !*pp)
        return;
    player_uninit(*pp);
    nau_freep(pp);
}

uint32_t nau_player_get_sample_rate(const struct nau_player *p)
{
    return p && p->engine ? p->sample_rate : 0;
}

int nau_player_add_track(struct nau_player *p, const struct nau_track_params *params, struct nau_track **track)
{
    if (!track)
        return NAU_ERROR_INVALID;
    *track = NULL;
    if (!p || !p->engine || !params)
        return NAU_ERROR_INVALID;

    if (!params->filename)
        return NAU_ERROR_INVALID;

    if (params->start < 0 || params->trim < 0)
        return NAU_ERROR_INVALID;

    if (!isfinite(params->volume) || params->volume < 0)
        return NAU_ERROR_INVALID;

    pthread_mutex_lock(&p->lock);

    int ret;
    struct nau_track *t = NULL;
    if (p->track_count == MAX_TRACKS) {
        ret = NAU_ERROR_UNSUPPORTED;
        goto end;
    }

    /*
     * Avoid opening the media just to return NAU_AGAIN. Player calls are
     * serialized, so no new commit can become pending before publication.
     */
    ret = nau_engine_collect(p->engine);
    if (ret != NAU_OK)
        goto end;

    t = nau_try_calloc(1, sizeof(*t));
    if (!t) {
        ret = NAU_ERROR_MEMORY;
        goto end;
    }

    /*
     * Start decoding at the media frame the track plays at the current
     * position, so that its data is ready for the first read
     */
    const int64_t position = nau_transport_get_position(p->transport);
    int64_t offset = position > params->start ? position - params->start : 0;
    if (params->duration >= 0 && offset > params->duration)
        offset = params->duration;
    int64_t decode_start;
    if (ckd_add(&decode_start, params->trim, offset)) {
        ret = NAU_ERROR_INVALID;
        goto end;
    }

    const struct nau_media_params media_params = {
        .start = decode_start,
        .normalize = params->normalize,
        .blocking = p->mode == NAU_PLAYER_OFFLINE,
    };
    t->media = nau_media_create();
    if (!t->media) {
        ret = NAU_ERROR_MEMORY;
        goto end;
    }
    ret = nau_media_init(t->media, p->engine, params->filename, &media_params);
    if (ret < 0)
        goto end;
    t->length = nau_media_get_length(t->media);

    t->source = nau_node_create();
    t->fader = nau_node_create();
    t->gain = nau_node_create();
    if (!t->source || !t->fader || !t->gain) {
        ret = NAU_ERROR_MEMORY;
        goto end;
    }
    ret = nau_media_init_source(t->media, t->source, 0, 0, 0);
    if (ret < 0)
        goto end;

    ret = nau_fader_init(t->fader, p->engine);
    if (ret < 0)
        goto end;

    ret = nau_gain_init(t->gain, p->engine, params->volume, (uint32_t)frames_in(p, VOLUME_RAMP_MS));
    if (ret < 0)
        goto end;

    ret = set_timings(t, params->start, params->trim, params->duration);
    if (ret < 0)
        goto end;

    /*
     * Set the loop before the commit: the media must know it when the render
     * thread positions the source. Nothing to undo if the commit fails: the
     * media is freed with the track.
     */
    update_loop(p, t);
    p->tracks[p->track_count++] = t;
    ret = commit(p, NULL, NULL);
    if (ret != NAU_OK) {
        p->track_count--;
        goto end;
    }

    *track = t;
    t = NULL;
end:
    free_track(&t);
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int nau_player_set_track_timings(struct nau_player *p, struct nau_track *t, int64_t start, int64_t trim,
                                 int64_t duration)
{
    if (!p || !p->engine || !t)
        return NAU_ERROR_INVALID;

    pthread_mutex_lock(&p->lock);

    int ret;
    if (!owns(p, t)) {
        ret = NAU_ERROR_INVALID;
        goto end;
    }

    /*
     * Both copies of the track share the source node. set_timings() only
     * changes its range on success, and the range is restored if the commit
     * fails.
     */
    struct nau_track edited = *t;
    ret = set_timings(&edited, start, trim, duration);
    if (ret < 0)
        goto end;

    ret = commit(p, t, &edited);
    if (ret != NAU_OK) {
        int restored = nau_source_set_range(t->source, t->start, t->trim, t->duration);
        nau_assert(restored == NAU_OK);
        goto end;
    }

    *t = edited;
    update_loop(p, t);
end:
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int nau_player_set_track_volume(struct nau_player *p, struct nau_track *t, float volume)
{
    if (!p || !p->engine || !t)
        return NAU_ERROR_INVALID;
    pthread_mutex_lock(&p->lock);
    int ret = NAU_ERROR_INVALID;
    if (owns(p, t))
        ret = nau_gain_set(t->gain, volume);
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int nau_player_set_track_fader(struct nau_player *p, struct nau_track *t, const struct nau_ramp *ramps,
                               uint32_t count)
{
    if (!p || !p->engine || !t)
        return NAU_ERROR_INVALID;

    pthread_mutex_lock(&p->lock);

    int ret;
    struct nau_node *fader = NULL;
    if (!owns(p, t)) {
        ret = NAU_ERROR_INVALID;
        goto end;
    }

    fader = nau_node_create();
    if (!fader) {
        ret = NAU_ERROR_MEMORY;
        goto end;
    }
    ret = nau_fader_init(fader, p->engine);
    if (ret < 0)
        goto end;

    ret = nau_fader_set_ramps(fader, ramps, count);
    if (ret < 0)
        goto end;

    struct nau_track edited = *t;
    edited.fader = fader;
    ret = commit(p, t, &edited);
    if (ret != NAU_OK)
        goto end;

    fader = t->fader;
    *t = edited;
end:
    nau_node_unrefp(&fader);
    pthread_mutex_unlock(&p->lock);
    return ret;
}

/*
 * Find the track of an effect of the player, and the index of the effect in
 * it. Only pointers are compared: a stale effect is not dereferenced.
 */
static struct nau_track *find_effect(const struct nau_player *p, const struct nau_track_effect *e, uint32_t *index)
{
    for (uint32_t i = 0; i < p->track_count; i++) {
        struct nau_track *t = p->tracks[i];
        for (uint32_t j = 0; j < t->effect_count; j++) {
            if (t->effects[j] == e) {
                *index = j;
                return t;
            }
        }
    }
    return NULL;
}

/*
 * Insert an effect at an index within its placement group. An index equal
 * to the group's count appends; a larger index is invalid.
 */
static bool insert_effect(struct nau_track *t, struct nau_track_effect *e, enum nau_track_effect_placement placement,
                          uint32_t index)
{
    const struct effect_range range = get_placement_range(t, placement);
    if (index > range.count)
        return false;
    nau_assert(t->effect_count < MAX_EFFECTS);
    const uint32_t at = range.first + index;
    memmove(&t->effects[at + 1], &t->effects[at], (t->effect_count - at) * sizeof(*t->effects));
    t->effects[at] = e;
    t->effect_count++;
    if (placement == NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER)
        t->pre_fader_effect_count++;
    return true;
}

/* Take an effect out of a copy of a track */
static void extract_effect(struct nau_track *t, uint32_t index)
{
    nau_assert(index < t->effect_count);
    if (index < t->pre_fader_effect_count)
        t->pre_fader_effect_count--;
    t->effect_count--;
    memmove(&t->effects[index], &t->effects[index + 1], (t->effect_count - index) * sizeof(*t->effects));
    t->effects[t->effect_count] = NULL;
}

static bool placement_valid(enum nau_track_effect_placement placement)
{
    return placement == NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER || placement == NAU_TRACK_EFFECT_PLACEMENT_POST_FADER;
}

int nau_player_add_track_effect(struct nau_player *p, struct nau_track *t, enum nau_effect_type type,
                                enum nau_track_effect_placement placement, struct nau_track_effect **effect)
{
    if (!effect)
        return NAU_ERROR_INVALID;
    *effect = NULL;
    const struct nau_effect_info *info = nau_effect_get_info(type);
    if (!p || !p->engine || !t || !info)
        return NAU_ERROR_INVALID;

    if (!placement_valid(placement))
        return NAU_ERROR_INVALID;

    pthread_mutex_lock(&p->lock);

    int ret;
    struct nau_track_effect *e = NULL;
    if (!owns(p, t)) {
        ret = NAU_ERROR_INVALID;
        goto end;
    }
    if (t->effect_count == MAX_EFFECTS) {
        ret = NAU_ERROR_UNSUPPORTED;
        goto end;
    }

    e = nau_try_calloc(1, sizeof(*e));
    if (!e) {
        ret = NAU_ERROR_MEMORY;
        goto end;
    }

    e->node = nau_node_create();
    if (!e->node) {
        ret = NAU_ERROR_MEMORY;
        goto end;
    }
    ret = nau_effect_init(e->node, p->engine, type, (uint32_t)frames_in(p, PARAM_RAMP_MS));
    if (ret < 0)
        goto end;

    /*
     * Append the effect to its placement: the index is in range, and the
     * track has room for it
     */
    struct nau_track edited = *t;
    const uint32_t count = get_placement_range(t, placement).count;
    const bool inserted = insert_effect(&edited, e, placement, count);
    nau_assert(inserted);

    ret = commit(p, t, &edited);
    if (ret != NAU_OK)
        goto end;

    *t = edited;
    *effect = e;
    e = NULL;
end:
    free_effect(&e);
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int nau_player_move_track_effect(struct nau_player *p, struct nau_track_effect *e,
                                 enum nau_track_effect_placement placement, uint32_t index)
{
    if (!p || !p->engine || !e)
        return NAU_ERROR_INVALID;

    if (!placement_valid(placement))
        return NAU_ERROR_INVALID;

    pthread_mutex_lock(&p->lock);

    int ret;
    uint32_t current;
    struct nau_track *t = find_effect(p, e, &current);
    if (!t) {
        ret = NAU_ERROR_INVALID;
        goto end;
    }

    struct nau_track edited = *t;
    extract_effect(&edited, current);
    if (!insert_effect(&edited, e, placement, index)) {
        ret = NAU_ERROR_INVALID;
        goto end;
    }

    ret = commit(p, t, &edited);
    if (ret != NAU_OK)
        goto end;

    *t = edited;
end:
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int nau_player_remove_track_effect(struct nau_player *p, struct nau_track_effect **ep)
{
    if (!p || !p->engine || !ep || !*ep)
        return NAU_ERROR_INVALID;

    pthread_mutex_lock(&p->lock);

    int ret;
    uint32_t index;
    struct nau_track *t = find_effect(p, *ep, &index);
    if (!t) {
        ret = NAU_ERROR_INVALID;
        goto end;
    }

    struct nau_track edited = *t;
    extract_effect(&edited, index);
    ret = commit(p, t, &edited);
    if (ret != NAU_OK)
        goto end;

    /*
     * The graphs in use keep the node of the effect until the render thread
     * is done with it
     */
    *t = edited;
    free_effect(ep);
end:
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int nau_player_set_track_effect_param(struct nau_player *p, struct nau_track_effect *e, uint32_t param, float value)
{
    if (!p || !p->engine || !e)
        return NAU_ERROR_INVALID;
    pthread_mutex_lock(&p->lock);
    int ret = NAU_ERROR_INVALID;
    uint32_t index;
    if (find_effect(p, e, &index))
        ret = nau_effect_set_param(e->node, param, value);
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int nau_player_set_track_effect_automation(struct nau_player *p, struct nau_track_effect *e,
                                           const struct nau_lane *lanes, uint32_t lane_count)
{
    if (!p || !p->engine || !e || !lanes)
        return NAU_ERROR_INVALID;

    if (!lane_count || lane_count > NAU_EFFECT_MAX_PARAMS)
        return NAU_ERROR_INVALID;

    for (uint32_t i = 0; i < lane_count; i++) {
        if (lanes[i].ramp_count && !lanes[i].ramps)
            return NAU_ERROR_INVALID;

        if (lanes[i].ramp_count > NAU_MAX_RAMPS)
            return NAU_ERROR_INVALID;

        for (uint32_t j = 0; j < i; j++) {
            if (lanes[j].param == lanes[i].param)
                return NAU_ERROR_INVALID;
        }
    }

    pthread_mutex_lock(&p->lock);

    struct nau_ramp *items[NAU_EFFECT_MAX_PARAMS] = {0};
    uint32_t counts[NAU_EFFECT_MAX_PARAMS] = {0};
    uint32_t swapped_count = 0;
    int ret;
    uint32_t index;
    if (!find_effect(p, e, &index)) {
        ret = NAU_ERROR_INVALID;
        goto end;
    }

    for (uint32_t i = 0; i < lane_count; i++) {
        counts[i] = lanes[i].ramp_count;
        if (!counts[i])
            continue;
        items[i] = nau_try_memdup(lanes[i].ramps, counts[i] * sizeof(*items[i]));
        if (!items[i]) {
            ret = NAU_ERROR_MEMORY;
            goto end;
        }
    }

    /*
     * Keep the previous lanes so the whole edit can be rolled back if any
     * lane is invalid or the commit fails.
     */
    for (; swapped_count < lane_count; swapped_count++) {
        ret = nau_effect_swap_automation(e->node, lanes[swapped_count].param, &items[swapped_count], &counts[swapped_count]);
        if (ret < 0)
            goto end;
    }

    ret = commit(p, NULL, NULL);
    if (ret != NAU_OK)
        goto end;

    /* Keep the new lanes and free the replaced ones below. */
    swapped_count = 0;
end:
    /* Restore the previous lanes without allocating. */
    while (swapped_count > 0) {
        swapped_count--;
        int restored = nau_effect_swap_automation(e->node, lanes[swapped_count].param, &items[swapped_count],
                                                        &counts[swapped_count]);
        nau_assert(restored == NAU_OK);
    }
    for (uint32_t i = 0; i < lane_count; i++)
        nau_freep(&items[i]);
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int64_t nau_track_get_length(const struct nau_track *t)
{
    return t ? t->length : 0;
}

int64_t nau_track_get_duration(const struct nau_track *t)
{
    return t ? t->duration : 0;
}

int nau_player_remove_track(struct nau_player *p, struct nau_track **tp)
{
    if (!p || !p->engine || !tp || !*tp)
        return NAU_ERROR_INVALID;

    pthread_mutex_lock(&p->lock);

    int ret;
    const uint32_t i = track_index(p, *tp);
    if (i == p->track_count) {
        ret = NAU_ERROR_INVALID;
        goto end;
    }

    memmove(&p->tracks[i], &p->tracks[i + 1], (p->track_count - i - 1) * sizeof(*p->tracks));
    p->track_count--;
    ret = commit(p, NULL, NULL);
    if (ret != NAU_OK) {
        memmove(&p->tracks[i + 1], &p->tracks[i], (p->track_count - i) * sizeof(*p->tracks));
        p->tracks[i] = *tp;
        p->track_count++;
        goto end;
    }

    /*
     * The graphs in use keep the nodes of the track until the render thread
     * is done with them
     */
    free_track(tp);
end:
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int nau_player_clear_tracks(struct nau_player *p)
{
    if (!p || !p->engine)
        return NAU_ERROR_INVALID;
    pthread_mutex_lock(&p->lock);
    const uint32_t n = p->track_count;
    p->track_count = 0;
    int ret = commit(p, NULL, NULL);
    if (ret == NAU_OK) {
        for (uint32_t i = 0; i < n; i++)
            free_track(&p->tracks[i]);
    } else {
        p->track_count = n;
    }
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int nau_player_play(struct nau_player *p)
{
    if (!p || !p->engine)
        return NAU_ERROR_INVALID;
    pthread_mutex_lock(&p->lock);
    int ret = NAU_OK;
    if (p->sink && !atomic_load_explicit(&p->sink_started, memory_order_relaxed)) {
        ret = nau_sink_start(p->sink);
        atomic_store_explicit(&p->sink_started, ret == NAU_OK, memory_order_release);
    }
    if (ret == NAU_OK)
        nau_transport_play(p->transport);
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int nau_player_pause(struct nau_player *p)
{
    if (!p || !p->engine)
        return NAU_ERROR_INVALID;
    /*
     * The device keeps running. The playback resumes without restarting
     * it, and the render thread goes on applying the seeks, so that the
     * tracks decode from the new position right away.
     */
    nau_transport_pause(p->transport);
    return NAU_OK;
}

int nau_player_stop(struct nau_player *p)
{
    if (!p || !p->engine)
        return NAU_ERROR_INVALID;
    pthread_mutex_lock(&p->lock);
    nau_transport_pause(p->transport);
    nau_transport_seek(p->transport, 0);
    int ret = NAU_OK;
    if (atomic_load_explicit(&p->sink_started, memory_order_relaxed)) {
        /*
         * Stopping the device immediately would cut off the pause fade. Wait
         * until the clock reports a stationary segment and its presentation
         * time has passed: the fade has then been heard. Limit the wait to
         * 100 ms in case the device has stopped rendering.
         */
        int64_t silent = -1;
        for (int i = 0; i < 100; i++) {
            if (silent < 0) {
                struct nau_clock clock;
                ret = nau_transport_get_clock(p->transport, &clock);
                if (ret == NAU_OK && !clock.playing)
                    silent = clock.time;
            }
            if (silent >= 0 && nau_sink_now() >= silent)
                break;
            nau_sleep_ms(1);
        }
        ret = nau_sink_stop(p->sink);
        atomic_store_explicit(&p->sink_started, false, memory_order_release);
    }
    /* Apply the seek here if no render thread is running. */
    if (!rendering(p))
        nau_transport_sync(p->transport);
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int nau_player_is_playing(const struct nau_player *p)
{
    return p && p->engine ? nau_transport_is_playing(p->transport) : 0;
}

int nau_player_seek(struct nau_player *p, int64_t frame)
{
    if (!p || !p->engine)
        return NAU_ERROR_INVALID;
    pthread_mutex_lock(&p->lock);
    int ret = nau_transport_seek(p->transport, frame);
    if (ret == NAU_OK && !rendering(p))
        nau_transport_sync(p->transport);
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int64_t nau_player_get_position(const struct nau_player *p)
{
    return p && p->engine ? nau_transport_get_position(p->transport) : 0;
}

int nau_player_get_position_at(const struct nau_player *p, int64_t time, int64_t *position)
{
    if (!p || !p->engine || !position)
        return NAU_ERROR_INVALID;
    /* Without rendering, there is no clock history for the current position. */
    if (!rendering(p)) {
        *position = nau_transport_get_position(p->transport);
        return NAU_OK;
    }
    return nau_transport_get_position_at(p->transport, time, position);
}

int nau_player_set_duration(struct nau_player *p, int64_t frames)
{
    if (!p || !p->engine)
        return NAU_ERROR_INVALID;
    pthread_mutex_lock(&p->lock);
    int ret = nau_transport_set_duration(p->transport, frames);
    if (ret == NAU_OK) {
        p->duration = frames;
        update_loops(p);
    }
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int64_t nau_player_get_duration(const struct nau_player *p)
{
    return p && p->engine ? nau_transport_get_duration(p->transport) : 0;
}

int nau_player_set_looping(struct nau_player *p, int looping)
{
    if (!p || !p->engine)
        return NAU_ERROR_INVALID;
    pthread_mutex_lock(&p->lock);
    p->looping = !!looping;
    nau_transport_set_looping(p->transport, p->looping);
    update_loops(p);
    pthread_mutex_unlock(&p->lock);
    return NAU_OK;
}

int nau_player_is_looping(const struct nau_player *p)
{
    return p && p->engine ? nau_transport_is_looping(p->transport) : 0;
}

int nau_player_set_loop_range(struct nau_player *p, int64_t start, int64_t end)
{
    if (!p || !p->engine)
        return NAU_ERROR_INVALID;
    pthread_mutex_lock(&p->lock);
    int ret = nau_transport_set_loop_range(p->transport, start, end);
    if (ret == NAU_OK) {
        p->loop_start = start;
        p->loop_end = end;
        update_loops(p);
    }
    pthread_mutex_unlock(&p->lock);
    return ret;
}

int nau_player_set_loop_hold(struct nau_player *p, int64_t frames)
{
    return p && p->engine ? nau_transport_set_loop_hold(p->transport, frames) : NAU_ERROR_INVALID;
}

int nau_player_set_volume(struct nau_player *p, float volume)
{
    if (!p || !p->engine)
        return NAU_ERROR_INVALID;
    pthread_mutex_lock(&p->lock);
    int ret = nau_gain_set(p->master, volume);
    if (ret == NAU_OK)
        p->volume = volume;
    pthread_mutex_unlock(&p->lock);
    return ret;
}

float nau_player_get_volume(const struct nau_player *p)
{
    return p && p->engine ? p->volume : 0.f;
}

int nau_player_start_rendering(struct nau_player *p)
{
    if (!p || !p->engine)
        return NAU_ERROR_INVALID;
    if (p->mode != NAU_PLAYER_HOST)
        return NAU_ERROR_INVALID;
    pthread_mutex_lock(&p->lock);
    if (!atomic_load_explicit(&p->host_rendering, memory_order_relaxed)) {
        nau_transport_sync(p->transport);
        atomic_store_explicit(&p->host_rendering, true, memory_order_release);
    }
    pthread_mutex_unlock(&p->lock);
    return NAU_OK;
}

int nau_player_stop_rendering(struct nau_player *p)
{
    if (!p || !p->engine)
        return NAU_ERROR_INVALID;
    if (p->mode != NAU_PLAYER_HOST)
        return NAU_ERROR_INVALID;
    pthread_mutex_lock(&p->lock);
    atomic_store_explicit(&p->host_rendering, false, memory_order_release);
    nau_transport_sync(p->transport);
    nau_engine_collect(p->engine);
    pthread_mutex_unlock(&p->lock);
    return NAU_OK;
}

int nau_player_render(struct nau_player *p, int64_t time, uint32_t frames, float *output)
{
    if (!p || !p->engine)
        return NAU_ERROR_INVALID;
    if (p->sink)
        return NAU_ERROR_INVALID;
    if (p->mode == NAU_PLAYER_HOST && !atomic_load_explicit(&p->host_rendering, memory_order_acquire))
        return NAU_ERROR_INVALID;
    return nau_transport_render(p->transport, time, frames, output);
}

int nau_player_export(struct nau_player *p, const char *filename, enum nau_export_format format)
{
    if (!p || !p->engine || !filename)
        return NAU_ERROR_INVALID;

    if (p->mode != NAU_PLAYER_OFFLINE)
        return NAU_ERROR_INVALID;

    if (format < NAU_EXPORT_S16 || format > NAU_EXPORT_F32)
        return NAU_ERROR_INVALID;

    pthread_mutex_lock(&p->lock);

    float *frames = NULL;
    struct nau_exporter *exporter = NULL;
    int ret;
    const int64_t duration = nau_transport_get_duration(p->transport);

    frames = nau_try_calloc(EXPORT_FRAMES * 2, sizeof(*frames));
    if (!frames) {
        ret = NAU_ERROR_MEMORY;
        goto end;
    }

    exporter = nau_exporter_create();
    if (!exporter) {
        ret = NAU_ERROR_MEMORY;
        goto end;
    }
    const struct nau_exporter_option options[] = {{"rf64", "auto"}, {NULL, NULL}};
    const struct nau_exporter_config config = {
        .container = "wav",
        .encoder = format == NAU_EXPORT_S16 ? "pcm_s16le" : "pcm_f32le",
        .sample_format = format == NAU_EXPORT_S16 ? NAU_SAMPLE_FORMAT_S16 : NAU_SAMPLE_FORMAT_F32,
        .sample_rate = p->sample_rate,
        .container_options = options,
        .empty_packet = 1,
    };
    ret = nau_exporter_open(exporter, filename, &config);
    if (ret < 0)
        goto end;

    /* The export ignores the loop, which is restored afterwards */
    nau_transport_set_looping(p->transport, 0);
    for (uint32_t i = 0; i < p->track_count; i++)
        nau_media_set_loop(p->tracks[i]->media, 0, 0);
    nau_transport_seek(p->transport, 0);
    nau_transport_sync(p->transport);
    nau_transport_play(p->transport);
    for (int64_t written = 0; written < duration;) {
        const uint32_t n = (uint32_t)NAU_MIN(duration - written, EXPORT_FRAMES);
        ret = nau_transport_render(p->transport, 0, n, frames);
        if (ret < 0)
            break;
        ret = nau_exporter_write(exporter, frames, n);
        if (ret < 0)
            break;
        written += n;
    }
    if (ret > 0)
        ret = NAU_OK;
    nau_transport_pause(p->transport);
    nau_transport_set_looping(p->transport, p->looping);
    update_loops(p);
end:
    int close_ret = nau_exporter_closep(&exporter);
    if (ret >= 0)
        ret = close_ret;
    nau_freep(&frames);
    pthread_mutex_unlock(&p->lock);
    return ret;
}

void nau_player_get_stats(const struct nau_player *p, struct nau_player_stats *stats)
{
    if (!stats)
        return;
    *stats = (struct nau_player_stats){0};
    if (!p || !p->engine)
        return;
    /*
     * This function never waits. While another thread edits the player, it
     * returns the sums of the tracks computed by the previous call.
     */
    struct nau_player *mp = (struct nau_player *)p;
    int ret = pthread_mutex_trylock(&mp->lock);
    if (ret == 0) {
        struct nau_player_stats sum = {0};
        for (uint32_t i = 0; i < p->track_count; i++) {
            struct nau_media_stats media;
            nau_media_get_stats(p->tracks[i]->media, &media);
            sum.silent_frames += media.silent_frames;
            sum.skipped_frames += media.skipped_frames;
            sum.requests += media.requests;
            sum.errors += media.errors;
        }
        atomic_store_explicit(&mp->silent_frames, sum.silent_frames, memory_order_relaxed);
        atomic_store_explicit(&mp->skipped_frames, sum.skipped_frames, memory_order_relaxed);
        atomic_store_explicit(&mp->requests, sum.requests, memory_order_relaxed);
        atomic_store_explicit(&mp->errors, sum.errors, memory_order_relaxed);
        pthread_mutex_unlock(&mp->lock);
    }
    *stats = (struct nau_player_stats){
        .underruns = p->sink ? nau_sink_get_underruns(p->sink) : 0,
        .silent_frames = atomic_load_explicit(&p->silent_frames, memory_order_relaxed),
        .skipped_frames = atomic_load_explicit(&p->skipped_frames, memory_order_relaxed),
        .requests = atomic_load_explicit(&p->requests, memory_order_relaxed),
        .errors = atomic_load_explicit(&p->errors, memory_order_relaxed),
    };
    if (p->sink) {
        ret = nau_sink_get_status(p->sink);
        if (ret < 0)
            stats->errors++;
    }
}
