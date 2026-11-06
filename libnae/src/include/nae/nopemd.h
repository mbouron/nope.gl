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

struct nae_media_params {
    int64_t start;      /* source frame decoded from until the first read */
    int normalize;      /* level the loudness of the media */
    int blocking;       /* reads wait for their data rather than play silence: offline rendering */
};

/*
 * A media decoded by nope.media on a thread of its own, ahead of the reads,
 * in stereo at the rate of the engine (which must be stereo). The renderer
 * never waits for it (unless blocking), takes no lock and frees nothing:
 * audio not decoded in time plays as silence, and the audio then late is
 * skipped for the media to stay in time.
 *
 * Seeks are requests the decoding thread serves while the renderer goes on,
 * and a loop range has the decoder go round it ahead of the renderer, which
 * moves onto the next pass of the loop when going round without seeking.
 */
NAE_NOPEMD_API int nae_media_create(struct nae_engine *engine, const char *filename,
                                    const struct nae_media_params *params, struct nae_media **media);
/* Frames of the media at the engine rate, 0 if unknown */
NAE_NOPEMD_API int64_t nae_media_get_length(const struct nae_media *media);
/* At most one source node per media instance. The node retains the media. */
NAE_NOPEMD_API int nae_media_source(struct nae_media *media, int64_t timeline_start,
                                    int64_t source_start, int64_t duration, struct nae_node **node);
/* The source frames [start, end) to decode round, an empty range for none;
 * from any thread. The renderer going round a loop of the project should
 * have its media loop over the part they play of it. */
NAE_NOPEMD_API int nae_media_set_loop(struct nae_media *media, int64_t start, int64_t end);

struct nae_media_stats {
    int64_t silent_frames;   /* played as silence, the audio not there in time */
    int64_t skipped_frames;  /* decoded too late, skipped */
    int64_t requests;        /* positions asked of the decoder, seeks included */
    int64_t errors;          /* failures of nope.media */
};

/* From any thread */
NAE_NOPEMD_API void nae_media_get_stats(const struct nae_media *media, struct nae_media_stats *stats);
/* The decoding thread stops with the last reference */
NAE_NOPEMD_API void nae_media_unrefp(struct nae_media **media);

#ifdef __cplusplus
}
#endif
#endif
