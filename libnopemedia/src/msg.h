/*
 * This file is part of nope.media.
 *
 * Copyright (c) 2016 Stupeflix
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

#ifndef MSG_H
#define MSG_H

#include <stdint.h>

#include "internal.h"
#include "loop.h"

struct AVFrame;
struct AVPacket;

enum msg_type {
    MSG_FRAME,
    MSG_PACKET,
    MSG_SEEK,
    MSG_INFO,
    MSG_START,
    MSG_STOP,
    MSG_SYNC,
    MSG_LOOP,
    MSG_PASS,   // a loop pass starts, after every frame of the earlier passes
    NB_MSG
};

struct message {
    enum msg_type type;
    union {
        struct {
            struct AVFrame *frame;
        } frame;

        struct {
            struct AVPacket *packet;
        } packet;

        struct {
            int64_t ts; // seek target in media time, in microseconds
        } seek;

        struct {
            struct media_info info;
            struct nmdi_pass_timeline pass_timeline;
        } info;

        struct {
            struct nmdi_pass_info info;
        } loop;

        struct {
            int64_t number;
        } pass;
    };
};

void nmdi_msg_free_data(void *arg);

#endif
