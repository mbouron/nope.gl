/*
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

#include <stddef.h>
#include <string.h>

#include "internal.h"
#include "log.h"
#include "node_effect2d_shader.h"
#include "nopegl/nopegl.h"
#include "timerange.h"
#include "shader2d.h"

struct effect2d_shader_opts {
    const char *glsl;
    struct hmap *resources;
    int premult;
    double start;
    double end;
};


int ngl_effect2dshader_set_range(struct ngl_node *node, double start, double end)
{
    if (!node)
        return NGL_ERROR_INVALID_ARG;

    if (node->cls->id != NGL_NODE_EFFECT2DSHADER)
        return NGL_ERROR_UNSUPPORTED;

    int ret = ngli_node_check_not_traversing(node);
    if (ret < 0)
        return ret;

    struct effect2d_shader_opts *o = node->opts;
    o->start = start;
    o->end = end;

    if (!node->ctx)
        return 0;

    ngli_node_invalidate_branch(node);
    return 0;
}


static int effect2dshader_init(struct ngl_node *node)
{
    const struct effect2d_shader_opts *o = node->opts;
    if (o->resources) {
        const struct hmap_entry *entry = NULL;
        while ((entry = ngli_hmap_next(o->resources, entry))) {
            if (!strcmp(entry->key.str, "input")) {
                LOG(ERROR, "Effect2DShader: resource key \"input\" is reserved for the effect input");
                return NGL_ERROR_INVALID_USAGE;
            }
            int ret = ngli_shader2d_check_key(entry->key.str, "Effect2DShader");
            if (ret < 0)
                return ret;
        }
    }
    return 0;
}

#define OFFSET(x) offsetof(struct effect2d_shader_opts, x)
static const struct node_param effect2d_shader_params[] = {
    {
        .key       = "glsl",
        .type      = NGLI_PARAM_TYPE_STR,
        .offset    = OFFSET(glsl),
        .flags     = NGLI_PARAM_FLAG_NON_NULL,
        .desc      = NGLI_DOCSTRING("GLSL source defining the effect function (see the [shader guide](../expl/shaders.md#2d-shaders)); if empty, the children are rendered unchanged"),
    }, {
        .key        = "resources",
        .type       = NGLI_PARAM_TYPE_NODEDICT,
        .offset     = OFFSET(resources),
        .node_types = (const uint32_t[]){
            NGL_NODE_UNIFORMFLOAT,
            NGL_NODE_UNIFORMVEC2,
            NGL_NODE_UNIFORMVEC3,
            NGL_NODE_UNIFORMVEC4,
            NGL_NODE_UNIFORMCOLOR,
            NGL_NODE_UNIFORMQUAT,
            NGL_NODE_UNIFORMMAT4,
            NGL_NODE_UNIFORMINT,
            NGL_NODE_UNIFORMIVEC2,
            NGL_NODE_UNIFORMIVEC3,
            NGL_NODE_UNIFORMIVEC4,
            NGL_NODE_UNIFORMUINT,
            NGL_NODE_UNIFORMUIVEC2,
            NGL_NODE_UNIFORMUIVEC3,
            NGL_NODE_UNIFORMUIVEC4,
            NGL_NODE_UNIFORMBOOL,
            NGL_NODE_EVALFLOAT,
            NGL_NODE_EVALVEC2,
            NGL_NODE_EVALVEC3,
            NGL_NODE_EVALVEC4,
            NGL_NODE_ANIMATEDFLOAT,
            NGL_NODE_ANIMATEDVEC2,
            NGL_NODE_ANIMATEDVEC3,
            NGL_NODE_ANIMATEDVEC4,
            NGL_NODE_ANIMATEDQUAT,
            NGL_NODE_ANIMATEDCOLOR,
            NGL_NODE_NOISEFLOAT,
            NGL_NODE_NOISEVEC2,
            NGL_NODE_NOISEVEC3,
            NGL_NODE_NOISEVEC4,
            NGL_NODE_TIME,
            NGL_NODE_TEXTURE2D,
            NGL_NODE_TEXTURE2DARRAY,
            NGL_NODE_TEXTURE3D,
            NGL_NODE_TEXTURECUBE,
            NGL_NODE_CUSTOMTEXTURE,
            NGL_NODE_BLOCK,
            NGLI_NODE_NONE,
        },
        .desc = NGLI_DOCSTRING("uniform, texture and block nodes; keys are used as GLSL names"),
    }, {
        .key       = "premult",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(premult),
        .desc      = NGLI_DOCSTRING("premultiply the shader output color by its alpha"),
    }, {
        .key         = "start",
        .type        = NGLI_PARAM_TYPE_F64,
        .offset      = OFFSET(start),
        .flags       = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc        = NGLI_DOCSTRING("active range start time (inclusive); a negative value is evaluated as 0"),
    }, {
        .key         = "end",
        .type        = NGLI_PARAM_TYPE_F64,
        .offset      = OFFSET(end),
        .def_value   = {.f64=-1.0},
        .flags       = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc        = NGLI_DOCSTRING("active range end time (exclusive); negative values mean no end; "
                                      "a value before `start` means an empty range"),
    },
    {NULL}
};
#undef OFFSET

struct effect2d_shader_info ngli_effect2d_shader_get_info(const struct ngl_node *node)
{
    ngli_assert(node->cls->id == NGL_NODE_EFFECT2DSHADER);
    const struct effect2d_shader_opts *o = node->opts;
    struct effect2d_shader_info info = {
        .glsl  = o->glsl,
        .resources   = o->resources,
        .premult     = o->premult,
    };
    ngli_timerange_sanitize(o->start, o->end, &info.start, &info.end);
    return info;
}

const struct node_class ngli_effect2dshader_class = {
    .id        = NGL_NODE_EFFECT2DSHADER,
    .name      = "Effect2DShader",
    .init      = effect2dshader_init,
    .update    = ngli_node_update_children,
    .opts_size = sizeof(struct effect2d_shader_opts),
    .params    = effect2d_shader_params,
    .flags     = NGLI_NODE_FLAG_SHAREABLE,
    .file      = __FILE__,
};
