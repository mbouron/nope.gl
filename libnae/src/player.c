/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE  /* nanosleep(), pthread_setname_np() with glibc */
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nae/miniaudio.h"
#include "nae/nopemd.h"
#include "nae/player.h"
#include "thread.h"

#define BLOCK_FRAMES 256
#define DECLICK_MS 5          /* ramps around the jumps of the transport */
#define READY_TIMEOUT_MS 250  /* longest silence waiting for the tracks after a jump */
#define VOLUME_RAMP_MS 5      /* volume changes */
#define COMMIT_TRIES 2000     /* of a millisecond, waiting for the previous commit to be adopted */
#define RECORD_FRAMES 4096

struct nae_track {
    struct nae_media *media;
    struct nae_node *source;
    struct nae_node *fader;
    struct nae_node *gain;
    int64_t start;
    int64_t trim;
    int64_t duration;         /* resolved against the length of the media */
    int64_t length;
    struct nae_keyframe *keyframes;  /* from the start of the track */
    uint32_t nb_keyframes;
};

struct nae_player {
    enum nae_player_mode mode;
    uint32_t sample_rate;
    uint32_t period_ms;
    nae_mutex lock;
    struct nae_engine *engine;
    struct nae_transport *transport;
    struct nae_sink *sink;
    bool sink_started;
    _Atomic bool host_rendering;  /* NAE_PLAYER_HOST: the host renders, since its first render */
    /* The counts of the tracks at the last nae_player_get_stats() that could read them */
    _Atomic int64_t silent_frames;
    _Atomic int64_t skipped_frames;
    _Atomic int64_t requests;
    _Atomic int64_t errors;
    struct nae_node *master;
    struct nae_track **tracks;
    uint32_t nb_tracks;
    uint32_t capacity;
    float volume;
    int64_t duration;
    int looping;
    int64_t loop_start;
    int64_t loop_end;
};

static void sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    const struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

static int64_t frames_in(const struct nae_player *p, int64_t ms)
{
    return ms * p->sample_rate / 1000;
}

/* Something renders the player concurrently: commits are adopted by it */
static bool rendering(const struct nae_player *p)
{
    return p->sink_started || atomic_load_explicit(&p->host_rendering, memory_order_acquire);
}

/* Lays the tracks, each through its fader and volume, into the mix under the master volume */
static int commit(struct nae_player *p)
{
    const uint32_t n = p->nb_tracks;
    const uint32_t count = 3 * n + 2;
    struct nae_route *routes = calloc(count, sizeof(*routes));
    uint32_t *inputs = calloc(3 * n + 1, sizeof(*inputs));
    struct nae_node *mix = NULL;
    int ret = routes && inputs ? nae_mix_create(p->engine, n, &mix) : NAE_ERROR_MEMORY;
    if (ret < 0)
        goto end;
    uint32_t *fader_inputs = inputs, *gain_inputs = inputs + n, *mix_inputs = inputs + 2 * n;
    uint32_t *master_input = inputs + 3 * n;
    for (uint32_t i = 0; i < n; i++) {
        const struct nae_track *t = p->tracks[i];
        fader_inputs[i] = 3 * i;
        gain_inputs[i] = 3 * i + 1;
        mix_inputs[i] = 3 * i + 2;
        routes[3 * i] = (struct nae_route){t->source, NULL};
        routes[3 * i + 1] = (struct nae_route){t->fader, &fader_inputs[i]};
        routes[3 * i + 2] = (struct nae_route){t->gain, &gain_inputs[i]};
    }
    *master_input = 3 * n;
    routes[3 * n] = (struct nae_route){mix, mix_inputs};
    routes[3 * n + 1] = (struct nae_route){p->master, master_input};
    for (int i = 0; i < COMMIT_TRIES; i++) {
        ret = nae_engine_commit(p->engine, routes, count, 3 * n + 1);
        if (ret != NAE_AGAIN)
            break;
        /* The previous commit not adopted yet */
        if (rendering(p))
            sleep_ms(1);
        else
            nae_transport_sync(p->transport);
    }
    if (ret == NAE_OK && !rendering(p))
        nae_transport_sync(p->transport);
end:
    nae_node_unrefp(&mix);
    free(routes);
    free(inputs);
    return ret;
}

/* The range of the project played: the loop range when looping over one, the whole duration otherwise */
static void play_range(const struct nae_player *p, int64_t *start, int64_t *end)
{
    *start = 0;
    *end = p->duration;
    if (p->loop_end > p->loop_start && p->loop_start < p->duration) {
        *start = p->loop_start;
        if (p->loop_end < p->duration)
            *end = p->loop_end;
    }
}

/* Has the media of a track decode ahead the part of it the loop plays again */
static void update_loop(const struct nae_player *p, struct nae_track *t)
{
    int64_t start, end;
    play_range(p, &start, &end);
    const int64_t first = start > t->start ? start : t->start;
    const int64_t last = end < t->start + t->duration ? end : t->start + t->duration;
    if (!p->looping || last <= first)
        nae_media_set_loop(t->media, 0, 0);
    else
        nae_media_set_loop(t->media, t->trim + first - t->start, t->trim + last - t->start);
}

static void update_loops(const struct nae_player *p)
{
    for (uint32_t i = 0; i < p->nb_tracks; i++)
        update_loop(p, p->tracks[i]);
}

/* The keyframes of the fader of a track, in the frames of the project */
static int set_fader(struct nae_track *t)
{
    struct nae_keyframe *keyframes = calloc(t->nb_keyframes + 1, sizeof(*keyframes));
    if (!keyframes)
        return NAE_ERROR_MEMORY;
    for (uint32_t i = 0; i < t->nb_keyframes; i++) {
        keyframes[i] = t->keyframes[i];
        keyframes[i].frame += t->start;
    }
    const int ret = nae_fader_set_keyframes(t->fader, keyframes, t->nb_keyframes);
    free(keyframes);
    return ret;
}

static int set_timings(struct nae_track *t, int64_t start, int64_t trim, int64_t duration)
{
    if (start < 0 || trim < 0)
        return NAE_ERROR_INVALID;
    if (duration < 0 || (t->length && trim + duration > t->length))
        duration = t->length > trim ? t->length - trim : 0;
    t->start = start;
    t->trim = trim;
    t->duration = duration;
    const int ret = nae_source_set_range(t->source, start, trim, duration);
    return ret < 0 ? ret : set_fader(t);
}

static void free_track(struct nae_track **tp)
{
    struct nae_track *t = *tp;
    if (!t)
        return;
    nae_node_unrefp(&t->source);
    nae_node_unrefp(&t->fader);
    nae_node_unrefp(&t->gain);
    nae_media_unrefp(&t->media);
    free(t->keyframes);
    free(t);
    *tp = NULL;
}

int nae_player_create(const struct nae_player_config *config, struct nae_player **player)
{
    if (!player)
        return NAE_ERROR_INVALID;
    *player = NULL;
    if (!config || config->mode > NAE_PLAYER_OFFLINE)
        return NAE_ERROR_INVALID;
    struct nae_player *p = calloc(1, sizeof(*p));
    if (!p)
        return NAE_ERROR_MEMORY;
    nae_mutex_init(&p->lock);
    atomic_init(&p->host_rendering, false);
    atomic_init(&p->silent_frames, 0);
    atomic_init(&p->skipped_frames, 0);
    atomic_init(&p->requests, 0);
    atomic_init(&p->errors, 0);
    p->mode = config->mode;
    p->sample_rate = config->sample_rate ? config->sample_rate : 48000;
    p->period_ms = config->period_ms;
    p->volume = 1.f;
    const struct nae_config engine_config = {p->sample_rate, 2, BLOCK_FRAMES};
    int ret = nae_engine_create(&engine_config, &p->engine);
    if (ret < 0)
        goto fail;
    const bool offline = p->mode == NAE_PLAYER_OFFLINE;
    const struct nae_transport_config transport_config = {
        .declick_frames = offline ? 0 : (uint32_t)frames_in(p, DECLICK_MS),
        .ready_timeout_frames = offline ? 0 : (uint32_t)frames_in(p, READY_TIMEOUT_MS),
    };
    ret = nae_transport_create(p->engine, &transport_config, &p->transport);
    if (ret < 0)
        goto fail;
    ret = nae_gain_create(p->engine, 1.f, (uint32_t)frames_in(p, VOLUME_RAMP_MS), &p->master);
    if (ret < 0)
        goto fail;
    if (p->mode == NAE_PLAYER_DEVICE || p->mode == NAE_PLAYER_NULL_DEVICE) {
        const struct nae_sink_config sink_config = {
            .null_device = p->mode == NAE_PLAYER_NULL_DEVICE,
            .transport = p->transport,
            .period_ms = p->period_ms,
        };
        ret = nae_sink_create(p->engine, &sink_config, &p->sink);
        if (ret < 0)
            goto fail;
    }
    ret = commit(p);
    if (ret < 0)
        goto fail;
    *player = p;
    return NAE_OK;
fail:
    nae_player_freep(&p);
    return ret;
}

void nae_player_freep(struct nae_player **pp)
{
    struct nae_player *p = *pp;
    if (!p)
        return;
    /* Nothing renders anymore, then the plans release the tracks */
    nae_sink_freep(&p->sink);
    for (uint32_t i = 0; i < p->nb_tracks; i++)
        free_track(&p->tracks[i]);
    free(p->tracks);
    nae_node_unrefp(&p->master);
    nae_engine_freep(&p->engine);
    nae_transport_freep(&p->transport);
    nae_mutex_destroy(&p->lock);
    free(p);
    *pp = NULL;
}

uint32_t nae_player_get_sample_rate(const struct nae_player *p)
{
    return p ? p->sample_rate : 0;
}

int nae_player_add_track(struct nae_player *p, const struct nae_track_params *params, struct nae_track **track)
{
    if (!track)
        return NAE_ERROR_INVALID;
    *track = NULL;
    if (!p || !params || !params->filename || params->start < 0 || params->trim < 0 ||
        !isfinite(params->volume) || params->volume < 0)
        return NAE_ERROR_INVALID;
    nae_mutex_lock(&p->lock);
    struct nae_track *t = calloc(1, sizeof(*t));
    int ret = t ? NAE_OK : NAE_ERROR_MEMORY;
    if (ret < 0)
        goto end;
    /* Decoding from where the track plays from at the position, ahead of its first read */
    const int64_t position = nae_transport_get_position(p->transport);
    const struct nae_media_params media_params = {
        .start = params->trim + (position > params->start ? position - params->start : 0),
        .normalize = params->normalize,
        .blocking = p->mode == NAE_PLAYER_OFFLINE,
    };
    ret = nae_media_create(p->engine, params->filename, &media_params, &t->media);
    if (ret < 0)
        goto end;
    t->length = nae_media_get_length(t->media);
    if ((ret = nae_media_source(t->media, 0, 0, 0, &t->source)) < 0 ||
        (ret = nae_fader_create(p->engine, &t->fader)) < 0 ||
        (ret = nae_gain_create(p->engine, params->volume, (uint32_t)frames_in(p, VOLUME_RAMP_MS), &t->gain)) < 0 ||
        (ret = set_timings(t, params->start, params->trim, params->duration)) < 0)
        goto end;
    if (p->nb_tracks == p->capacity) {
        const uint32_t capacity = p->capacity ? 2 * p->capacity : 16;
        struct nae_track **tracks = realloc(p->tracks, capacity * sizeof(*tracks));
        if (!tracks) {
            ret = NAE_ERROR_MEMORY;
            goto end;
        }
        p->tracks = tracks;
        p->capacity = capacity;
    }
    p->tracks[p->nb_tracks++] = t;
    update_loop(p, t);
    ret = commit(p);
    if (ret < 0) {
        p->nb_tracks--;
        goto end;
    }
    *track = t;
    t = NULL;
end:
    free_track(&t);
    nae_mutex_unlock(&p->lock);
    return ret;
}

static bool owns(const struct nae_player *p, const struct nae_track *t)
{
    for (uint32_t i = 0; i < p->nb_tracks; i++)
        if (p->tracks[i] == t)
            return true;
    return false;
}

int nae_player_set_track_timings(struct nae_player *p, struct nae_track *t, int64_t start, int64_t trim,
                                 int64_t duration)
{
    if (!p || !t)
        return NAE_ERROR_INVALID;
    nae_mutex_lock(&p->lock);
    int ret = owns(p, t) ? set_timings(t, start, trim, duration) : NAE_ERROR_INVALID;
    if (ret == NAE_OK) {
        update_loop(p, t);
        ret = commit(p);
    }
    nae_mutex_unlock(&p->lock);
    return ret;
}

int nae_player_set_track_volume(struct nae_player *p, struct nae_track *t, float volume)
{
    if (!p || !t)
        return NAE_ERROR_INVALID;
    nae_mutex_lock(&p->lock);
    const int ret = owns(p, t) ? nae_gain_set(t->gain, volume) : NAE_ERROR_INVALID;
    nae_mutex_unlock(&p->lock);
    return ret;
}

int nae_player_set_track_fader(struct nae_player *p, struct nae_track *t, const struct nae_keyframe *keyframes,
                               uint32_t count)
{
    if (!p || !t || (count && !keyframes))
        return NAE_ERROR_INVALID;
    nae_mutex_lock(&p->lock);
    int ret = owns(p, t) ? NAE_OK : NAE_ERROR_INVALID;
    struct nae_keyframe *copy = NULL;
    if (ret == NAE_OK && count) {
        copy = calloc(count, sizeof(*copy));
        if (!copy)
            ret = NAE_ERROR_MEMORY;
        else
            memcpy(copy, keyframes, count * sizeof(*copy));
    }
    if (ret == NAE_OK) {
        struct nae_keyframe *previous = t->keyframes;
        const uint32_t previous_count = t->nb_keyframes;
        t->keyframes = copy;
        t->nb_keyframes = count;
        ret = set_fader(t);
        if (ret < 0) {
            t->keyframes = previous;
            t->nb_keyframes = previous_count;
            free(copy);
        } else {
            free(previous);
            ret = commit(p);
        }
    }
    nae_mutex_unlock(&p->lock);
    return ret;
}

int64_t nae_track_get_length(const struct nae_track *t)
{
    return t ? t->length : 0;
}

int64_t nae_track_get_duration(const struct nae_track *t)
{
    return t ? t->duration : 0;
}

int nae_player_remove_track(struct nae_player *p, struct nae_track **tp)
{
    if (!p || !tp || !*tp)
        return NAE_ERROR_INVALID;
    nae_mutex_lock(&p->lock);
    int ret = NAE_ERROR_INVALID;
    for (uint32_t i = 0; i < p->nb_tracks; i++) {
        if (p->tracks[i] != *tp)
            continue;
        memmove(&p->tracks[i], &p->tracks[i + 1], (p->nb_tracks - i - 1) * sizeof(*p->tracks));
        p->nb_tracks--;
        /* The plans keep its nodes until the renderer is done with them */
        ret = commit(p);
        free_track(tp);
        break;
    }
    nae_mutex_unlock(&p->lock);
    return ret;
}

int nae_player_clear_tracks(struct nae_player *p)
{
    if (!p)
        return NAE_ERROR_INVALID;
    nae_mutex_lock(&p->lock);
    const uint32_t n = p->nb_tracks;
    p->nb_tracks = 0;
    const int ret = commit(p);
    for (uint32_t i = 0; i < n; i++)
        free_track(&p->tracks[i]);
    nae_mutex_unlock(&p->lock);
    return ret;
}

int nae_player_play(struct nae_player *p)
{
    if (!p)
        return NAE_ERROR_INVALID;
    nae_mutex_lock(&p->lock);
    int ret = NAE_OK;
    if (p->sink && !p->sink_started) {
        ret = nae_sink_start(p->sink);
        p->sink_started = ret == NAE_OK;
    }
    if (ret == NAE_OK)
        nae_transport_play(p->transport);
    nae_mutex_unlock(&p->lock);
    return ret;
}

int nae_player_pause(struct nae_player *p)
{
    if (!p)
        return NAE_ERROR_INVALID;
    /* The device runs on: playing again restarts nothing, and seeks reach the tracks at once */
    nae_transport_pause(p->transport);
    return NAE_OK;
}

int nae_player_stop(struct nae_player *p)
{
    if (!p)
        return NAE_ERROR_INVALID;
    nae_mutex_lock(&p->lock);
    nae_transport_pause(p->transport);
    nae_transport_seek(p->transport, 0);
    int ret = NAE_OK;
    if (p->sink_started) {
        /* Faded out before the device stops */
        struct nae_clock clock;
        for (int i = 0; i < 100; i++) {
            if (nae_transport_get_clock(p->transport, &clock) == NAE_OK && !clock.playing)
                break;
            sleep_ms(1);
        }
        ret = nae_sink_stop(p->sink);
        p->sink_started = false;
    }
    nae_transport_sync(p->transport);
    nae_mutex_unlock(&p->lock);
    return ret;
}

int nae_player_is_playing(const struct nae_player *p)
{
    return p ? nae_transport_is_playing(p->transport) : 0;
}

int nae_player_seek(struct nae_player *p, int64_t frame)
{
    if (!p)
        return NAE_ERROR_INVALID;
    nae_mutex_lock(&p->lock);
    const int ret = nae_transport_seek(p->transport, frame);
    if (ret == NAE_OK && !rendering(p))
        nae_transport_sync(p->transport);
    nae_mutex_unlock(&p->lock);
    return ret;
}

int64_t nae_player_get_position(const struct nae_player *p)
{
    return p ? nae_transport_get_position(p->transport) : 0;
}

int64_t nae_player_get_position_at(const struct nae_player *p, int64_t time)
{
    if (!p)
        return 0;
    struct nae_clock clock;
    if (!p->sink_started || nae_transport_get_clock(p->transport, &clock) != NAE_OK)
        return nae_transport_get_position(p->transport);
    return nae_clock_position_at(&clock, time);
}

int nae_player_set_duration(struct nae_player *p, int64_t frames)
{
    if (!p)
        return NAE_ERROR_INVALID;
    nae_mutex_lock(&p->lock);
    const int ret = nae_transport_set_duration(p->transport, frames);
    if (ret == NAE_OK) {
        p->duration = frames;
        update_loops(p);
    }
    nae_mutex_unlock(&p->lock);
    return ret;
}

int64_t nae_player_get_duration(const struct nae_player *p)
{
    return p ? nae_transport_get_duration(p->transport) : 0;
}

int nae_player_set_looping(struct nae_player *p, int looping)
{
    if (!p)
        return NAE_ERROR_INVALID;
    nae_mutex_lock(&p->lock);
    p->looping = !!looping;
    nae_transport_set_looping(p->transport, p->looping);
    update_loops(p);
    nae_mutex_unlock(&p->lock);
    return NAE_OK;
}

int nae_player_is_looping(const struct nae_player *p)
{
    return p ? nae_transport_is_looping(p->transport) : 0;
}

int nae_player_set_loop_range(struct nae_player *p, int64_t start, int64_t end)
{
    if (!p)
        return NAE_ERROR_INVALID;
    nae_mutex_lock(&p->lock);
    const int ret = nae_transport_set_loop_range(p->transport, start, end);
    if (ret == NAE_OK) {
        p->loop_start = start;
        p->loop_end = end;
        update_loops(p);
    }
    nae_mutex_unlock(&p->lock);
    return ret;
}

int nae_player_set_loop_hold(struct nae_player *p, int64_t frames)
{
    return p ? nae_transport_set_loop_hold(p->transport, frames) : NAE_ERROR_INVALID;
}

int nae_player_set_volume(struct nae_player *p, float volume)
{
    if (!p)
        return NAE_ERROR_INVALID;
    nae_mutex_lock(&p->lock);
    const int ret = nae_gain_set(p->master, volume);
    if (ret == NAE_OK)
        p->volume = volume;
    nae_mutex_unlock(&p->lock);
    return ret;
}

float nae_player_get_volume(const struct nae_player *p)
{
    return p ? p->volume : 0.f;
}

int nae_player_render(struct nae_player *p, int64_t time, uint32_t frames, float *output)
{
    if (!p || p->sink)
        return NAE_ERROR_INVALID;
    if (p->mode == NAE_PLAYER_HOST && !atomic_load_explicit(&p->host_rendering, memory_order_relaxed))
        atomic_store_explicit(&p->host_rendering, true, memory_order_release);
    return nae_transport_render(p->transport, time, frames, output);
}

static void put_u16(FILE *f, uint16_t v)
{
    const uint8_t b[] = {v & 0xff, v >> 8};
    fwrite(b, 1, sizeof(b), f);
}

static void put_u32(FILE *f, uint32_t v)
{
    const uint8_t b[] = {v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24};
    fwrite(b, 1, sizeof(b), f);
}

static void write_wav_header(FILE *f, enum nae_record_format format, uint32_t rate, uint32_t data_size)
{
    const uint16_t bits = format == NAE_RECORD_S16 ? 16 : 32;
    fwrite("RIFF", 1, 4, f);
    put_u32(f, 36 + data_size);
    fwrite("WAVEfmt ", 1, 8, f);
    put_u32(f, 16);
    put_u16(f, format == NAE_RECORD_S16 ? 1 : 3); /* PCM, IEEE float */
    put_u16(f, 2);
    put_u32(f, rate);
    put_u32(f, rate * 2 * bits / 8);
    put_u16(f, 2 * bits / 8);
    put_u16(f, bits);
    fwrite("data", 1, 4, f);
    put_u32(f, data_size);
}

int nae_player_record(struct nae_player *p, const char *filename, enum nae_record_format format)
{
    if (!p || !filename || p->mode != NAE_PLAYER_OFFLINE || format > NAE_RECORD_F32)
        return NAE_ERROR_INVALID;
    nae_mutex_lock(&p->lock);
    const int64_t duration = nae_transport_get_duration(p->transport);
    const size_t sample_size = format == NAE_RECORD_S16 ? 2 : 4;
    float *frames = calloc(RECORD_FRAMES * 2, sizeof(*frames));
    int16_t *s16 = calloc(RECORD_FRAMES * 2, sizeof(*s16));
    FILE *f = fopen(filename, "wb");
    int ret = frames && s16 ? NAE_OK : NAE_ERROR_MEMORY;
    if (ret == NAE_OK && !f)
        ret = NAE_ERROR_IO;
    if (ret == NAE_OK && (uint64_t)duration * 2 * sample_size > UINT32_MAX - 36)
        ret = NAE_ERROR_UNSUPPORTED;
    if (ret < 0)
        goto end;
    write_wav_header(f, format, p->sample_rate, (uint32_t)(duration * 2 * sample_size));
    nae_transport_seek(p->transport, 0);
    nae_transport_sync(p->transport);
    nae_transport_play(p->transport);
    for (int64_t written = 0; written < duration && ret >= 0;) {
        const uint32_t n = duration - written < RECORD_FRAMES ? (uint32_t)(duration - written) : RECORD_FRAMES;
        const int render = nae_transport_render(p->transport, 0, n, frames);
        if (render < 0)
            ret = render;
        if (format == NAE_RECORD_S16) {
            for (uint32_t i = 0; i < 2 * n; i++) {
                const float x = isfinite(frames[i]) ? fmaxf(-1.f, fminf(1.f, frames[i])) : 0.f;
                s16[i] = (int16_t)lrintf(x * 32767.f);
            }
            if (fwrite(s16, sample_size, 2 * n, f) != 2 * n)
                ret = NAE_ERROR_IO;
        } else if (fwrite(frames, sample_size, 2 * n, f) != 2 * n) {
            ret = NAE_ERROR_IO;
        }
        written += n;
    }
    nae_transport_pause(p->transport);
end:
    if (f && fclose(f) && ret >= 0)
        ret = NAE_ERROR_IO;
    free(frames);
    free(s16);
    nae_mutex_unlock(&p->lock);
    return ret;
}

void nae_player_get_stats(const struct nae_player *p, struct nae_player_stats *stats)
{
    if (!p || !stats)
        return;
    /* Never waiting: while the player is edited, the counts of the tracks are the last ones read */
    struct nae_player *mp = (struct nae_player *)p;
    if (nae_mutex_trylock(&mp->lock)) {
        struct nae_player_stats sum = {0};
        for (uint32_t i = 0; i < p->nb_tracks; i++) {
            struct nae_media_stats media;
            nae_media_get_stats(p->tracks[i]->media, &media);
            sum.silent_frames += media.silent_frames;
            sum.skipped_frames += media.skipped_frames;
            sum.requests += media.requests;
            sum.errors += media.errors;
        }
        atomic_store_explicit(&mp->silent_frames, sum.silent_frames, memory_order_relaxed);
        atomic_store_explicit(&mp->skipped_frames, sum.skipped_frames, memory_order_relaxed);
        atomic_store_explicit(&mp->requests, sum.requests, memory_order_relaxed);
        atomic_store_explicit(&mp->errors, sum.errors, memory_order_relaxed);
        nae_mutex_unlock(&mp->lock);
    }
    *stats = (struct nae_player_stats){
        .underruns = p->sink ? nae_sink_underruns(p->sink) : 0,
        .silent_frames = atomic_load_explicit(&p->silent_frames, memory_order_relaxed),
        .skipped_frames = atomic_load_explicit(&p->skipped_frames, memory_order_relaxed),
        .requests = atomic_load_explicit(&p->requests, memory_order_relaxed),
        .errors = atomic_load_explicit(&p->errors, memory_order_relaxed),
    };
    if (p->sink && nae_sink_status(p->sink) < 0)
        stats->errors++;
}
