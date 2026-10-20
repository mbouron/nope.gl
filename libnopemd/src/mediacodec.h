/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
 *
 * This file is available under the terms of either the Apache License,
 * Version 2.0, or the GNU Lesser General Public License, version 2.1 or (at
 * your option) any later version, at your option.
 */

#ifndef NOPE_MEDIA_MEDIACODEC_H
#define NOPE_MEDIA_MEDIACODEC_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>

/**
 * Opaque structure identifying a MediaCodec decoder.
 *
 * Video frames store an AImage pointer in data[3]. They keep the image reader
 * alive, so the image remains valid after a flush, reset or decoder destruction.
 * Audio frames contain PCM samples.
 */
struct nmdi_mediacodec;

/**
 * MediaCodec decoder parameters
 */
struct nmdi_mediacodec_params {
    const AVCodecParameters *par; /* codec parameters, must remain valid for the decoder lifetime */
    AVRational time_base;         /* time base for packet timestamps and durations */
    bool software;                /* use Android's software decoder */
    int max_images;               /* maximum number of images held by queues, the pipeline and the caller */
    void *log_ctx;                /* opaque user argument forwarded to the logging callback */
    void (*log)(void *log_ctx, int level, const char *fmt, va_list vl); /* optional callback using AV_LOG_* levels */
};

/**
 * Create and start a MediaCodec decoder.
 *
 * Must be destroyed using nmdi_mediacodec_freep().
 *
 * @param sp     address where the decoder pointer is stored; *sp is NULL on error
 * @param params pointer to the decoder parameters
 *
 * @return 0 on success, AVERROR_DECODER_NOT_FOUND if the stream is unsupported,
 *         or another AVERROR code (< 0) on error
 */
int nmdi_mediacodec_create(struct nmdi_mediacodec **sp, const struct nmdi_mediacodec_params *params);

/**
 * Send a packet to the decoder.
 *
 * On AVERROR(EAGAIN), receive frames before retrying the packet.
 *
 * @param s   pointer to the decoder
 * @param pkt pointer to the packet, or NULL to drain the decoder
 *
 * @return 0 on success, an AVERROR code (< 0) on error
 */
int nmdi_mediacodec_send(struct nmdi_mediacodec *s, const AVPacket *pkt);

/**
 * Receive a decoded frame.
 *
 * timeout_us limits each wait for codec output. Waiting for the rendered image
 * is separate and may block until previously returned frames are released.
 *
 * @param s          pointer to the decoder
 * @param frame      pointer to an empty frame to fill with decoded output
 * @param timeout_us timeout for each wait for codec output, in microseconds
 *
 * @return 0 on success, AVERROR(EAGAIN) if no frame is ready, AVERROR_EOF when
 *         drained, or another AVERROR code (< 0) on error
 */
int nmdi_mediacodec_receive(struct nmdi_mediacodec *s, AVFrame *frame, int64_t timeout_us);

/**
 * Recreate a failed or stalled codec.
 *
 * Resume from the last key frame if its packets are still cached, or from
 * the next key frame otherwise.
 * Repeated hardware failures fall back to an Android software decoder.
 *
 * @param s      pointer to the decoder
 * @param reason reason for the reset, used in log messages
 *
 * @return 0 on success, an AVERROR code (< 0) on error
 */
int nmdi_mediacodec_reset(struct nmdi_mediacodec *s, const char *reason);

/**
 * Discard queued packets and output.
 *
 * @param s pointer to the decoder
 */
void nmdi_mediacodec_flush(struct nmdi_mediacodec *s);

/**
 * Destroy the decoder and set the passed pointer to NULL.
 *
 * @param sp pointer to the pointer to the decoder
 */
void nmdi_mediacodec_freep(struct nmdi_mediacodec **sp);

/**
 * Release a video frame and set the passed frame pointer to NULL.
 *
 * Buffer reuse is deferred until release_fence_fd signals. The function takes
 * ownership of the fd and closes it when no longer needed.
 *
 * @param framep           pointer to the pointer to the frame to release
 * @param release_fence_fd sync file fd signaled when image access completes,
 *                         or -1 if already complete
 */
void nmdi_mediacodec_release_frame(AVFrame **framep, int release_fence_fd);

#endif
