/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2023 Nope Forge
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#ifndef FGBLUR_H
#define FGBLUR_H

#include <stdint.h>

#include "image.h"
#include <ngpu/ngpu.h>
#include "pipeline_compat.h"
#include "rtt.h"

#define NGLI_FGBLUR_MAX_MIP_LEVELS 16

struct ngl_ctx;
struct ngpu_ctx;

struct fgblur_ctx {
    /* Mip pyramid layout */
    struct ngpu_rendertarget_layout mip_layout;

    /* Uniform block shared by downsample and upsample passes */
    struct ngpu_block down_up_block;

    /* Full-resolution intermediate for upsample result */
    struct rtt_ctx *mip;

    /* Mip pyramid */
    struct rtt_ctx *mips[NGLI_FGBLUR_MAX_MIP_LEVELS];

    /* Destination RTT (interpolation output) */
    struct rtt_ctx *dst_rtt;

    uint32_t width;
    uint32_t height;
    uint32_t max_lod;

    /* Downsampling pass */
    struct {
        struct ngpu_pgcraft *crafter;
        struct pipeline_compat *pl;
    } dws;

    /* Upsampling pass */
    struct {
        struct ngpu_pgcraft *crafter;
        struct pipeline_compat *pl;
    } ups;

    /* Interpolation pass */
    struct {
        struct ngpu_block block;
        struct ngpu_pgcraft *crafter;
        struct pipeline_compat *pl;
    } interpolate;
};

int ngli_fgblur_init(struct fgblur_ctx *s, struct ngpu_ctx *gpu_ctx,
                     const struct ngpu_rendertarget_layout *mip_layout,
                     const struct ngpu_rendertarget_layout *dst_layout);
int ngli_fgblur_resize(struct fgblur_ctx *s, struct ngl_ctx *ctx,
                       uint32_t width, uint32_t height,
                       enum ngpu_format format);
const struct image *ngli_fgblur_execute(struct fgblur_ctx *s, struct ngl_ctx *ctx,
                                        const struct image *src_image,
                                        float blurriness,
                                        uint32_t width, uint32_t height);
void ngli_fgblur_release(struct fgblur_ctx *s);
void ngli_fgblur_uninit(struct fgblur_ctx *s);

#endif
