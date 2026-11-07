/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NAE_H
#define NAE_H

#include <stdint.h>

#if defined(_WIN32) && !defined(NAE_STATIC)
# define NAE_API __declspec(dllimport)
# ifdef NAE_BUILD
#  undef NAE_API
#  define NAE_API __declspec(dllexport)
# endif
#elif defined(__GNUC__)
# define NAE_API __attribute__((visibility("default")))
#else
# define NAE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NAE_OK = 0,
    NAE_AGAIN = 1,       /* Missing PCM, queue full, or a pending plan. */
    NAE_EOF = 2,
    NAE_ERROR_INVALID = -1,
    NAE_ERROR_MEMORY = -2,
    NAE_ERROR_UNSUPPORTED = -3,
    NAE_ERROR_IO = -4,
};

#define NAE_INPUT_NONE UINT32_MAX

struct nae_engine;
struct nae_node;

struct nae_config {
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t block_frames;
};

/* All buses contain interleaved float32 at the engine rate/layout. Outputs
 * start cleared. Input pointers remain valid only during process(). */
struct nae_process {
    int64_t frame;
    uint32_t frames;
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t nb_inputs;
    const float *const *inputs;
    float *output;
};

struct nae_processor {
    uint32_t nb_inputs;
    void *user;
    int (*process)(void *user, const struct nae_process *process);
    /* Called on a seek or first use; must also be bounded/allocation-free. */
    void (*reset)(void *user, int64_t frame);
    /* Called on the control thread after the last reference is released. */
    void (*destroy)(void *user);
};

struct nae_source {
    void *user;
    /* Fill the requested source range; output starts cleared. Return AGAIN
     * for unavailable data, EOF for exhausted data, or a negative error.
     * This function runs on the render thread: no allocation or waiting. */
    int (*read)(void *user, int64_t frame, uint32_t frames, float *output);
    /* Optional, render thread, wait-free: the renderer jumped, and reads next
     * from the source frame `frame` (the start of the range before it, its end
     * after it). `wrap` is nonzero when a loop goes round, which a source
     * decoding the loop ahead can follow without seeking. Also called before
     * the first read. */
    void (*seek)(void *user, int64_t frame, int wrap);
    void (*destroy)(void *user);
    int64_t timeline_start;
    int64_t source_start;
    int64_t duration;
};

/* Inputs are indices in this transaction, one per node input bus. NONE
 * supplies silence. The arrays are copied by commit; any ordering is valid. */
struct nae_route {
    struct nae_node *node;
    const uint32_t *inputs;
};

/* One serialized control thread and one render thread per engine. All APIs
 * except render and gain_set belong to the control thread. Stop/join rendering
 * before freeing the engine. Node handles must not outlive their engine. */
NAE_API int nae_engine_create(const struct nae_config *config, struct nae_engine **engine);
NAE_API const struct nae_config *nae_engine_get_config(const struct nae_engine *engine);
NAE_API void nae_engine_freep(struct nae_engine **engine);

/* Compiles the output's reachable DAG, retains its nodes, and publishes one
 * transaction. Cycles, duplicate instances, and foreign nodes are rejected.
 * AGAIN means the previous transaction has not reached a render boundary.
 * An empty transaction (nb_routes=0) removes the active graph. */
NAE_API int nae_engine_commit(struct nae_engine *engine, const struct nae_route *routes,
                             uint32_t nb_routes, uint32_t output);
NAE_API int nae_engine_collect(struct nae_engine *engine);

/* Render exactly frames, splitting large requests into bounded blocks.
 * first_frame is the project sample position, >=0. Nonconsecutive requests
 * reset processor history. Missing source data stays silent; other tracks
 * continue. Float headroom is preserved. Custom processors must have zero
 * externally visible latency in v1. Never render an engine concurrently. */
NAE_API int nae_engine_render(struct nae_engine *engine, int64_t first_frame,
                             uint32_t frames, float *output);

/* Ownership of processor/source.user transfers only on successful creation. */
NAE_API int nae_node_create(struct nae_engine *engine, const struct nae_processor *processor,
                           struct nae_node **node);
NAE_API int nae_source_create(struct nae_engine *engine, const struct nae_source *source,
                             struct nae_node **node);
/* Control-thread edit, snapshotted by the next successful commit. Existing
 * plans continue to use their original range. Prepare decoder data separately. */
NAE_API int nae_source_set_range(struct nae_node *node, int64_t timeline_start,
                                int64_t source_start, int64_t duration);
NAE_API int nae_gain_create(struct nae_engine *engine, float gain, uint32_t ramp_frames,
                           struct nae_node **node);
NAE_API int nae_mix_create(struct nae_engine *engine, uint32_t nb_inputs, struct nae_node **node);
/* Atomic target update, adopted at the next processing block. The ramp
 * continues across blocks and graph replacements. Gains must be finite >=0. */
NAE_API int nae_gain_set(struct nae_node *node, float gain);

/* A volume automation point: from `frame`, the volume goes linearly from
 * the previous point's (1 before the first point) to `volume` over `length`
 * frames, then stays there. */
struct nae_keyframe {
    int64_t frame;
    int64_t length;
    float volume;
};

/* A fader follows its keyframes over the project frames (none: unity). */
NAE_API int nae_fader_create(struct nae_engine *engine, struct nae_node **node);
/* Control-thread edit, snapshotted by the next successful commit, as source
 * ranges are. Keyframes are copied; frames must not decrease, lengths and
 * volumes must be finite >=0. At most 65536 keyframes. */
NAE_API int nae_fader_set_keyframes(struct nae_node *node, const struct nae_keyframe *keyframes,
                                   uint32_t nb_keyframes);
NAE_API void nae_node_unrefp(struct nae_node **node);

/*
 * Transport: plays the engine's project over [0, duration), or over a loop
 * range, from the render thread, which calls nae_transport_render() in place
 * of nae_engine_render().
 *
 * Every jump (seek, loop going round, play, pause, end) is taken through a
 * short ramp to silence and back, so the step between two unrelated
 * waveforms is never heard: a seek is adopted once the output is silent,
 * a loop pass fades out before its end, and after a jump the output fades
 * back in once every source playing has its data, or after a timeout.
 *
 * The control functions are wait-free and applied by the next render. A
 * seek while paused, or the end of the project, positions the sources at
 * once, for them to have their data when playback goes on.
 */
struct nae_transport;

struct nae_transport_config {
    uint32_t declick_frames;        /* length of the ramps, 0 for none (offline rendering) */
    uint32_t ready_timeout_frames;  /* longest silence waiting for the sources after a jump */
};

NAE_API int nae_transport_create(struct nae_engine *engine, const struct nae_transport_config *config,
                                struct nae_transport **transport);
NAE_API struct nae_engine *nae_transport_get_engine(const struct nae_transport *transport);
/* The renderer must be stopped */
NAE_API void nae_transport_freep(struct nae_transport **transport);

NAE_API void nae_transport_play(struct nae_transport *transport);
NAE_API void nae_transport_pause(struct nae_transport *transport);
/* Nonzero while playing; the transport stops at the end of the project when not looping */
NAE_API int nae_transport_is_playing(const struct nae_transport *transport);
/* The frame to play from, cut to the duration, at most 2^48-1 */
NAE_API int nae_transport_seek(struct nae_transport *transport, int64_t frame);
NAE_API int nae_transport_set_duration(struct nae_transport *transport, int64_t frames);
NAE_API int64_t nae_transport_get_duration(const struct nae_transport *transport);
/* Looping, playback reaching the end goes on from the start of the loop range */
NAE_API void nae_transport_set_looping(struct nae_transport *transport, int looping);
NAE_API int nae_transport_is_looping(const struct nae_transport *transport);
/* [start, end), cut to the duration; an empty range loops over the whole
 * duration. A position before the range plays on until its end. */
NAE_API int nae_transport_set_loop_range(struct nae_transport *transport, int64_t start, int64_t end);
/* Silence played at the end of each pass, the position staying at the end */
NAE_API int nae_transport_set_loop_hold(struct nae_transport *transport, int64_t frames);
/* The frame rendered next, or the frame of a seek not applied yet */
NAE_API int64_t nae_transport_get_position(const struct nae_transport *transport);

/* The project frame heard at a time, in the host's nanoseconds */
struct nae_clock {
    int64_t position;
    int64_t time;
    uint32_t sample_rate;
    int playing;
};

/* A coherent snapshot of the last render, wait-free; AGAIN if none yet */
NAE_API int nae_transport_get_clock(const struct nae_transport *transport, struct nae_clock *clock);
/* The frame heard at `time`, extrapolated from the clock while playing */
NAE_API int64_t nae_clock_position_at(const struct nae_clock *clock, int64_t time);

/* Render thread, or the control thread while nothing renders (the device
 * stopped): applies what is pending (the graph committed, a seek) and
 * positions the sources, without rendering. The output restarts from
 * silence, with the processors' history reset as after a jump. */
NAE_API void nae_transport_sync(struct nae_transport *transport);

/* Render thread: fills `frames` frames, moving the position on. `time` is
 * when the first one is heard, in the host's nanoseconds (0 if unknown):
 * the times of the renders are smoothed into the clock. */
NAE_API int nae_transport_render(struct nae_transport *transport, int64_t time,
                                uint32_t frames, float *output);

#ifdef __cplusplus
}
#endif
#endif
