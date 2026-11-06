/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NAE_MINIAUDIO_H
#define NAE_MINIAUDIO_H
#include "nae.h"

#if defined(_WIN32) && !defined(NAE_STATIC)
# ifdef NAE_MINIAUDIO_BUILD
#  define NAE_MINIAUDIO_API __declspec(dllexport)
# else
#  define NAE_MINIAUDIO_API __declspec(dllimport)
# endif
#elif defined(__GNUC__)
# define NAE_MINIAUDIO_API __attribute__((visibility("default")))
#else
# define NAE_MINIAUDIO_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct nae_sink;
struct nae_sink_config {
    /* Nonzero selects miniaudio's null device (headless testing). */
    int null_device;
};

/* Device lifecycle is serialized on the control thread. Engine must outlive
 * the sink. Do not call engine_render yourself while this sink is started. */
NAE_MINIAUDIO_API int nae_sink_create(struct nae_engine *engine, const struct nae_sink_config *config,
                           struct nae_sink **sink);
NAE_MINIAUDIO_API int nae_sink_start(struct nae_sink *sink);
/* stop waits for the callback to finish; it is also the barrier for seeks. */
NAE_MINIAUDIO_API int nae_sink_stop(struct nae_sink *sink);
NAE_MINIAUDIO_API int nae_sink_seek(struct nae_sink *sink, int64_t frame); /* Stopped only. */
/* Source underruns or errors observed since the last start. This does not
 * report backend/device XRuns. Device status is also exposed here. */
NAE_MINIAUDIO_API uint32_t nae_sink_underruns(const struct nae_sink *sink);
NAE_MINIAUDIO_API int nae_sink_status(const struct nae_sink *sink);
NAE_MINIAUDIO_API void nae_sink_freep(struct nae_sink **sink);

#ifdef __cplusplus
}
#endif
#endif
