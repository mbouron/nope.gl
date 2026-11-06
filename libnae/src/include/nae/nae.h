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
NAE_API void nae_node_unrefp(struct nae_node **node);

#ifdef __cplusplus
}
#endif
#endif
