/*
 * Copyright 2025 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef VIEWER_EXPORT_H
#define VIEWER_EXPORT_H

#include <stdint.h>

struct export_profile {
    const char *name;
    const char *format;
    const char *encoder;
    const char *pix_fmt;
    int crf;
    int faststart;
};

extern const struct export_profile export_profiles[];
extern const int nb_export_profiles;

/*
 * Returns non-zero if the encoder named by export_profiles[index].encoder is
 * available in the linked libavcodec. Cheap; intended for one-shot startup
 * filtering of the profile combo.
 */
int export_is_profile_available(int index);

struct scene_cmd_queue;

struct export_params {
    const char *filename;
    /*
     * The export worker posts SCENE_CMD_SNAPSHOT to this queue and blocks
     * until the scene thread hands back a duplicate. This keeps duplication
     * (which must run on the rendering thread) off the UI thread.
     */
    struct scene_cmd_queue *snapshot_queue;
    int profile_index;
    /*
     * height is the target encode height. Width is derived from the
     * snapshot's intrinsic aspect (ngl_scene_params width/height; falls back
     * to 16:9 when the scene declares no canvas size). Both end up rounded
     * down to even values so yuv420p-class encoders accept them.
     */
    uint32_t height;
    int32_t framerate[2];
    double duration;
};

enum export_state {
    EXPORT_IDLE,
    EXPORT_RUNNING,
    EXPORT_DONE,
    EXPORT_ERROR,
    EXPORT_CANCELLED,
};

struct export_ctx;

struct export_ctx *export_create(void);
int export_start(struct export_ctx *s, const struct export_params *params);
enum export_state export_get_state(struct export_ctx *s);
float export_get_progress(struct export_ctx *s);
const char *export_get_error(struct export_ctx *s);
void export_cancel(struct export_ctx *s);
void export_freep(struct export_ctx **sp);

#endif /* VIEWER_EXPORT_H */
