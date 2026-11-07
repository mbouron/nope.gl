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
    /* Played by the device when set, rather than the engine from a cursor:
     * the device can then run on while paused, and each callback times the
     * transport's clock. Must belong to the engine and outlive the sink. */
    struct nae_transport *transport;
    /* Duration of the device periods, 0 for the backend's default */
    uint32_t period_ms;
};

/* Device lifecycle is serialized on the control thread. Engine must outlive
 * the sink. Do not render yourself while this sink is started. */
NAE_MINIAUDIO_API int nae_sink_create(struct nae_engine *engine, const struct nae_sink_config *config,
                           struct nae_sink **sink);
NAE_MINIAUDIO_API int nae_sink_start(struct nae_sink *sink);
/* stop waits for the callback to finish; it is also the barrier for seeks. */
NAE_MINIAUDIO_API int nae_sink_stop(struct nae_sink *sink);
/* Stopped only, without a transport (which seeks itself) */
NAE_MINIAUDIO_API int nae_sink_seek(struct nae_sink *sink, int64_t frame);
/* Estimated time from a callback to its first frame heard, in nanoseconds */
NAE_MINIAUDIO_API int64_t nae_sink_latency(const struct nae_sink *sink);
/* The time base of the transport clocks the sinks time: monotonic nanoseconds
 * (CLOCK_MONOTONIC where available, the base of Android's System.nanoTime()) */
NAE_MINIAUDIO_API int64_t nae_sink_now(void);
/* Source underruns or errors observed since the last start. This does not
 * report backend/device XRuns. Device status is also exposed here. */
NAE_MINIAUDIO_API uint32_t nae_sink_underruns(const struct nae_sink *sink);
NAE_MINIAUDIO_API int nae_sink_status(const struct nae_sink *sink);
NAE_MINIAUDIO_API void nae_sink_freep(struct nae_sink **sink);

#ifdef __cplusplus
}
#endif
#endif
