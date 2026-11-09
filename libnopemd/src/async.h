/*
 * This file is part of nope.media.
 *
 * Copyright (c) 2015 Stupeflix
 *
 * nope.media is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * nope.media is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with nope.media; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef ASYNC_H
#define ASYNC_H

#include <stdint.h>
#include <libavutil/frame.h>

#include "nopemd/nopemd.h"
#include "loop.h"
#include "opts.h"
#include "msg.h"

const char *nmdi_async_get_msg_type_string(enum msg_type type);

struct async_context *nmdi_async_alloc_context(void);

int nmdi_async_init(struct async_context *actx, void *log_ctx,
                    const char *filename, const struct nmdi_opts *o);

int nmdi_async_start(struct async_context *actx);

int nmdi_async_fetch_info(struct async_context *actx, struct nmd_info *info);

/* Playback range. */
void nmdi_async_set_playback_range(struct async_context *actx, int64_t start, int64_t end);
int64_t nmdi_async_get_playback_stopped_at(struct async_context *actx);
void nmdi_async_clear_playback_stopped_at(struct async_context *actx);

/* Loop range and pass history. */
int64_t nmdi_async_set_loop(struct async_context *actx, int64_t start, int64_t end);
int nmdi_async_get_pass(struct async_context *actx, int64_t pass_number, struct nmdi_pass *dst);
void nmdi_async_release_pass_history(struct async_context *actx, int64_t pass_number);

int nmdi_async_get_empty_pass(struct async_context *actx, struct nmdi_pass *dst);

int nmdi_async_get_pass_timeline(struct async_context *actx, struct nmdi_pass_timeline *timeline);

int nmdi_async_seek(struct async_context *actx, int64_t ts);
/* Wait for pending control requests, then set *target to the seek time in microseconds, or AV_NOPTS_VALUE. */
int nmdi_async_get_seek_target(struct async_context *actx, int64_t *target);

#define NMDI_RET_END_OF_PASS 1

/**
 * Pop a frame or consume a pass-start notification from the output queue.
 *
 * Return 0 with a frame, NMDI_RET_END_OF_PASS with no frame when a pass-start
 * notification is consumed, or a negative value on error. After a notification,
 * earlier passes have no frames left, and nmdi_async_get_pass_number() returns
 * the new pass number.
 */
int nmdi_async_pop_frame(struct async_context *actx, AVFrame **framep);

/**
 * Get the current pass number while consuming the output queue.
 *
 * Return the pass number from the latest start notification consumed by
 * nmdi_async_pop_frame(), or 0 if none has been consumed since startup or
 * the last seek or stop. The API may still be returning frames from an
 * earlier pass.
 */
int64_t nmdi_async_get_pass_number(const struct async_context *actx);

int nmdi_async_stop(struct async_context *actx);

int nmdi_nmdi_async_started(struct async_context *actx);

void nmdi_async_free(struct async_context **actxp);

#endif /* ASYNC_H */
