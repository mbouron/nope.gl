/*
 * Copyright 2026 Nope Forge
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

#ifndef NODE_MASK_H
#define NODE_MASK_H

#include <stddef.h>
#include <ngpu/ngpu.h>
#include "params.h"

/* Maximum number of prebuilt uniforms per mask node */
#define MASK_MAX_UNIFORMS 4

/* MaskBlur style values (must match ngli_maskblur_style_choices in node_mask.c) */
#define MASK_BLUR_NORMAL 0
#define MASK_BLUR_SOLID  1
#define MASK_BLUR_OUTER  2
#define MASK_BLUR_INNER  3

/* MaskTexture channel values (must match ngli_masktexture_channel_choices in node_mask.c) */
#define MASK_CHANNEL_ALPHA     0
#define MASK_CHANNEL_LUMINANCE 1
#define MASK_CHANNEL_RED       2
#define MASK_CHANNEL_GREEN     3
#define MASK_CHANNEL_BLUE      4

extern const struct param_choices ngli_maskblur_style_choices;
extern const struct param_choices ngli_masktexture_channel_choices;

/* One prebuilt uniform from mask node opts */
struct mask_uniform_def {
    char name[64];
    enum ngpu_type type;
    size_t opts_offset;
};

/*
 * mask_info is stored as the FIRST member of every mask node's priv_data.
 * DrawRect casts mask_node->priv_data to (const struct mask_info *) at init.
 */
struct mask_info {
    const char *glsl;                                   /* ngl_mask_alpha() function */
    struct mask_uniform_def uniforms[MASK_MAX_UNIFORMS];
    size_t nb_uniforms;
    float dilation;                                     /* extra margin expansion in pixels */
    struct ngl_node *texture_node;                      /* non-NULL for MaskTexture */
    const void *opts;                                   /* pointer to mask node opts */
};

#endif
