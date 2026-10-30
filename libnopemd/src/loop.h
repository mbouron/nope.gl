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
 * startup or at the most recent seek target, which can be outside the range.
 * Later passes start at the loop start. A regular seek restarts numbering at
 * 0; an internal loop seek advances to the next pass.
 *
 * The demuxer starts the next pass while decoded frames from the previous
 * pass are still queued. If the restart packet cannot safely continue the
 * decoder's input, the decoder outputs its buffered frames and resets first.
 *
 *   media    |-------[======= loop =======)-------|
 *   pass 0      seek ---------------------)
 *   pass 1           [--------------------)
 *   pass 2           [--------------------)  ...
 *
 * Each pass keeps its range revision and cutoff timestamps,
 * so the API can check whether queued frames still cover a request after a
 * range change. The `end` and `drop` timestamps use the stream time base,
 * without the pass offset described in `nmdi_pass_timeline`.
 */
struct nmdi_pass {
    int64_t number;   // pass number since startup or the most recent regular seek
    int64_t loop_rev; // revision of the loop range used to start this pass; -1 for pass 0
    int64_t end;      // decode timestamp of the first packet not sent; `AV_NOPTS_VALUE` if unknown, `INT64_MAX` at EOF
    int64_t drop;     // earliest presentation timestamp dropped at or after the pass end; `INT64_MAX` if none
};

/**
 * Check whether a pass ends after a timestamp.
 *
 * For a completed pass, the effective end is the earlier of the first packet
 * not sent and the earliest frame dropped. If the end is unknown, or the pass
 * reached EOF without dropped frames, use INT64_MAX.
 *
 * @param pass  pass information
 * @param ts    timestamp in the stream time base, without the pass offset
 *
 * @return 1 if ts is strictly before the effective end, 0 otherwise
 */
int nmdi_pass_ends_after(const struct nmdi_pass *pass, int64_t ts);

/*
 * Shared loop settings and pass information, protected by `lock`. The API sets
 * the playback range, `playback_start` and `playback_end`, in microseconds.
 * This range can change during playback (see `nmd_set_time_range()`). The
 * demuxer and filterer read it as needed. When filtering stops at the playback
 * end, it records that boundary in `playback_stopped_at`, in microseconds.
 * The recorded value is kept across range changes, so the API can resume
 * playback if the end moves later. A seek or stop request clears it.
 *
 * The API sets `loop_start` and `loop_end` in media time, in microseconds.
 * The demuxer records each pass it starts, and the decoder records frames
 * dropped at the pass end. The API removes old passes as it returns frames
 * from later passes.
 *
 * `loop_rev` advances when the requested range changes. Each pass remembers
 * the revision it started with, since queued frames may still belong to an
 * older range.
 *
 * `seek_rev` advances when pass numbering restarts at 0, at startup or after
 * a regular seek. The decoder can still report dropped frames from before
 * the seek; their old revision prevents them from changing the new passes.
 * An internal loop seek only advances the pass number.
 *
 * `empty_pass` keeps a snapshot of the full pass that stopped playback with
 * no filtered frames. It survives removal of old passes and is cleared when
 * pass numbering resets. If the range changes, the API compares its revision
 * with the snapshot and seeks on the next frame request to try again.
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
    int64_t playback_start;   // start of the part of the media played (`start_time`), in microseconds
    int64_t playback_end;     // its end (`end_time`); `AV_NOPTS_VALUE` without one
    int64_t playback_stopped_at; // playback end that stopped filtering, in microseconds; `AV_NOPTS_VALUE` if none
    int64_t loop_start;
    int64_t loop_end;         // exclusive loop end; `AV_NOPTS_VALUE` when looping is disabled
    int64_t loop_rev;         // revision of the requested range; incremented on each change
    int64_t seek_rev;         // revision of pass numbering; incremented whenever it restarts at 0
    struct nmdi_pass empty_pass; // pass that stopped playback with no filtered frames; `loop_rev` is -1 if none
    struct nmdi_pass *passes; // consecutive passes still needed by the API
    size_t passes_count;      // number of retained passes
    size_t passes_capacity;   // allocated capacity of `passes`, in entries
};

/* Loop state lifetime. */
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

/**
 * Get the pass that stopped playback with no filtered frames.
 *
 * Copy the snapshot recorded when the empty pass was reported. If the pass
 * was no longer retained, the snapshot uses the reported number and current
 * loop revision, with end set to AV_NOPTS_VALUE and drop to INT64_MAX.
 *
 * @param s    loop state
 * @param dst  destination for the pass snapshot; unchanged if none is recorded
 *
 * @return 1 if a snapshot was copied, 0 otherwise
 */
int nmdi_loop_get_empty_pass(struct nmdi_loop *s, struct nmdi_pass *dst);

/*
 * Give each pass its own interval of timestamps through the decoder. The
 * demuxer adds `p * stride` to the packet timestamps of pass `p`, placing each
 * repetition after the previous one. The decoder removes this offset from
 * output frames and attaches the pass number separately. Public frame
 * timestamps remain media timestamps.
 *
 * `origin`, `stride`, and `end` use the stream time base. `stride` covers the
 * whole media plus margins and stays the same when the loop range changes.
 * The margins allow for packet timestamps outside the reported duration.
 * `end` is the probed media end, independent of the requested loop range.
 *
 *   |<---------- pass 0 ---------->|<---------- pass 1 ---------->|
 *   | margin |   media    | margin | margin |   media    | margin |  ...
 *   ^        ^            ^        ^
 *   `origin` media start  `end`    `origin + stride`
 */
struct nmdi_pass_timeline {
    int64_t origin;   // start of the timestamp interval reserved for pass 0
    int64_t stride;   // width of each pass interval; 0 if this timeline cannot be determined
    int64_t max_pass; // limit on pass numbers to prevent timestamp overflow
    int64_t end;      // probed end of the media
};

void nmdi_pass_timeline_init(struct nmdi_pass_timeline *s, const AVStream *st, int64_t duration);
int64_t nmdi_pass_from_timestamp(const struct nmdi_pass_timeline *s, int64_t ts);

/*
 * Message sent by the demuxer to the decoder before a pass's first packet,
 * or to update the current pass's end. The decoder retains these messages
 * while frames from their passes may still arrive.
 *
 * `start` and `end` use the stream time base without a pass offset. If `reset`
 * is set, output buffered frames and flush decoder state before accepting
 * subsequent packets.
 */
struct nmdi_pass_msg {
    int64_t seek_rev;    // revision of pass numbering when this message was sent
    int64_t pass_number; // pass number within `seek_rev`
    int64_t start;       // requested playback start for this pass; `AV_NOPTS_VALUE` if unset
    int64_t end;         // discard frames at or after this timestamp; `INT64_MAX` if no cutoff
    int reset;
};

#endif
