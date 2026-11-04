/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
 *
 * This file is available under the terms of either the Apache License,
 * Version 2.0, or the GNU Lesser General Public License, version 2.1 or (at
 * your option) any later version, at your option.
 */

#ifndef NOPE_MEDIA_LOOP_H
#define NOPE_MEDIA_LOOP_H

#include <stdint.h>

#include <libavformat/avformat.h>

#include "pthread_compat.h"

/*
 * A pass is one traversal of the media up to the loop end. Pass 0 starts at
 * the initial playback position or the latest seek target, which can be
 * outside the loop range. Later passes start at the loop start. A regular
 * seek resets pass numbering; a loop seek advances to the next pass.
 *
 *   media    |-------[======= loop =======)-------|
 *   pass 0      seek ---------------------)
 *   pass 1           [--------------------)
 *   pass 2           [--------------------)  ...
 *
 * Keep the loop range revision and cutoff timestamps so the API can check
 * whether queued frames still cover a request after a range change.
 * `end` and `drop` use the stream time base, without the pass offset.
 */
struct nmdi_pass {
    int64_t number;   // pass number; 0 at startup and after a regular seek
    int64_t loop_rev; // loop range revision at pass start; -1 for pass 0
    int64_t end;      // DTS of the first packet not sent; `AV_NOPTS_VALUE` if unknown, `INT64_MAX` at EOF
    int64_t drop;     // earliest PTS dropped at or after the pass end; `INT64_MAX` if none
};

int nmdi_pass_ends_after(const struct nmdi_pass *pass, int64_t ts);

/*
 * Loop settings and pass history shared by the API, demuxer, decoder and
 * filterer, protected by `lock`. Playback and loop bounds use media time,
 * in microseconds. The playback range can change during playback (see
 * `nmd_set_time_range()`).
 *
 * `loop_rev` identifies the requested loop range, since queued passes may
 * still use an older range. `seek_rev` distinguishes passes before and after
 * a regular seek, so late reports of dropped frames cannot affect new passes.
 *
 * `playback_stopped_at` keeps the playback end that stopped filtering, so the
 * API can resume if the end moves later. A seek or stop clears it.
 *
 * `empty_pass` keeps the full pass that stopped playback with no decoded or
 * filtered frame, even after its history is released. The API retries on the
 * next frame request if the loop range changes. Resetting pass numbering
 * clears it.
 *
 * Several passes can be queued ahead of the frame currently being shown:
 *
 *   demuxer -> packets -> decoder -> frames -> sink -> user
 *   pass 7                                             pass 4
 *
 *   `passes`  [ 4 | 5 | 6 | 7 ]
 *               ^           ^
 *               |           `nmdi_loop_start_pass()` at each wrap
 *               `nmdi_loop_release_pass_history()` as frames are shown
 */
struct nmdi_loop {
    pthread_mutex_t lock;
    int64_t playback_start;      // playback start (`start_time` option)
    int64_t playback_end;        // playback end (`end_time` option); `AV_NOPTS_VALUE` if unset
    int64_t playback_stopped_at; // playback end that stopped filtering; `AV_NOPTS_VALUE` if none
    int64_t loop_start;
    int64_t loop_end;            // exclusive loop end; `AV_NOPTS_VALUE` disables looping
    int64_t loop_rev;            // loop range revision; incremented on each update
    int64_t seek_rev;            // revision of pass numbering; incremented on each reset
    struct nmdi_pass empty_pass; // pass that stopped playback with no output; `loop_rev` is -1 if none
    struct nmdi_pass *passes;    // consecutive passes still needed by the API
    size_t passes_count;
    size_t passes_capacity;
};

/* Initialize before threads can access the state; uninitialize after they stop. */
int nmdi_loop_init(struct nmdi_loop *s);
void nmdi_loop_uninit(struct nmdi_loop *s);

/* Playback range. */
void nmdi_loop_set_playback_range(struct nmdi_loop *s, int64_t start, int64_t end);
void nmdi_loop_get_playback_range(struct nmdi_loop *s, int64_t *start, int64_t *end);
void nmdi_loop_set_playback_stopped_at(struct nmdi_loop *s, int64_t end);
int64_t nmdi_loop_get_playback_stopped_at(struct nmdi_loop *s);

/* Requested loop range. */
int64_t nmdi_loop_set(struct nmdi_loop *s, int64_t start, int64_t end);
void nmdi_loop_get(struct nmdi_loop *s, int64_t *start, int64_t *end, int64_t *loop_rev);
int64_t nmdi_loop_stream_end(int64_t end, int64_t end_time, AVRational time_base);

/* Pass history. */
int64_t nmdi_loop_reset_passes(struct nmdi_loop *s);
int nmdi_loop_start_pass(struct nmdi_loop *s, int64_t pass_number, int64_t loop_rev, int64_t prev_end);
void nmdi_loop_drop_frame(struct nmdi_loop *s, int64_t seek_rev, int64_t pass_number, int64_t ts);
int nmdi_loop_get_pass(struct nmdi_loop *s, int64_t pass_number, struct nmdi_pass *dst);
void nmdi_loop_release_pass_history(struct nmdi_loop *s, int64_t pass_number);

/* Empty-pass reporting. */
int nmdi_loop_report_empty_pass(struct nmdi_loop *s, int64_t pass_number);
int nmdi_loop_get_empty_pass(struct nmdi_loop *s, struct nmdi_pass *dst);

/*
 * Identify each frame's pass from its timestamp. The demuxer adds
 * `pass_number * stride` to packet timestamps to place each pass after the
 * previous one. The decoder removes the offset and attaches the pass number
 * to output frames, so public timestamps remain media timestamps.
 *
 * `origin`, `stride`, and `end` use the stream time base. `stride` covers the
 * whole media plus margins for timestamps outside the reported duration.
 * The timeline stays the same when the loop range changes.
 *
 *   |<---------- pass 0 ---------->|<---------- pass 1 ---------->|
 *   | margin |   media    | margin | margin |   media    | margin |  ...
 *   ^        ^            ^        ^
 *   `origin` media start  `end`    `origin + stride`
 */
struct nmdi_pass_timeline {
    int64_t origin;   // start of the interval reserved for pass 0
    int64_t stride;   // width of each pass interval; 0 if the timeline is unknown
    int64_t max_pass; // highest pass number allowed to avoid timestamp overflow
    int64_t end;      // probed end of the media
};

void nmdi_pass_timeline_init(struct nmdi_pass_timeline *s, const AVStream *st, int64_t duration);
int64_t nmdi_pass_from_timestamp(const struct nmdi_pass_timeline *s, int64_t ts);

/*
 * The demuxer sends pass information before the pass's first packet, or to
 * update its end. The decoder keeps it while frames from that pass may still
 * arrive.
 *
 * `start` and `end` use the stream time base, without the pass offset.
 * Set `reset` if the first packet cannot safely follow the previous pass:
 * the decoder must output buffered frames and flush its state first.
 */
struct nmdi_pass_info {
    int64_t seek_rev;    // revision of pass numbering for this pass
    int64_t pass_number; // pass number within `seek_rev`
    int64_t start;       // loop start for this pass; `AV_NOPTS_VALUE` if not looping
    int64_t end;         // discard frames at or after this timestamp; `INT64_MAX` if no cutoff
    int reset;
};

#endif
