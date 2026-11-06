/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NAE_NOPEMD_H
#define NAE_NOPEMD_H
#include "nae.h"

#if defined(_WIN32) && !defined(NAE_STATIC)
# ifdef NAE_NOPEMD_BUILD
#  define NAE_NOPEMD_API __declspec(dllexport)
# else
#  define NAE_NOPEMD_API __declspec(dllimport)
# endif
#elif defined(__GNUC__)
# define NAE_NOPEMD_API __attribute__((visibility("default")))
#else
# define NAE_NOPEMD_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct nae_media;

/* Stereo engine required. queue_blocks >=2, with one engine block per slot.
 * Opening/decoding may block in pump() and seek(), on a feeder/control thread.
 * Use a separate media instance for each simultaneous playback cursor. */
NAE_NOPEMD_API int nae_media_create(struct nae_engine *engine, const char *filename,
                            uint32_t queue_blocks, struct nae_media **media);
/* At most one source node per media instance. The node retains the media. */
NAE_NOPEMD_API int nae_media_source(struct nae_media *media, int64_t timeline_start,
                            int64_t source_start, int64_t duration, struct nae_node **node);
/* Publish at most one PCM block. May block in nope.media; never call on the
 * render thread. AGAIN: queue full, EOF: latched until seek, negative: error.
 * One producer may pump concurrently with rendering. */
NAE_NOPEMD_API int nae_media_pump(struct nae_media *media);
/* Available queued slots (not a readiness guarantee for a particular range). */
NAE_NOPEMD_API uint32_t nae_media_queued(const struct nae_media *media);
/* Both pump and rendering MUST be quiescent during seek. This resets the
 * queue, seeks the decoder, and permits prefill before restarting the sink.
 * source_frame uses the same coordinates as nae_source.source_start. */
NAE_NOPEMD_API int nae_media_seek(struct nae_media *media, int64_t source_frame);
/* Stop/join the producer before releasing its caller-owned reference. */
NAE_NOPEMD_API void nae_media_unrefp(struct nae_media **media);

#ifdef __cplusplus
}
#endif
#endif
