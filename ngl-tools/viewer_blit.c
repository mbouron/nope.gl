/*
 * Copyright 2025-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "viewer_blit.h"

struct blit_ctx {
    struct ngpu_ctx *gpu_ctx;
    struct ngpu_pgcraft *crafter;
    struct ngpu_pipeline *pipeline;
    struct ngpu_bindgroup_layout *bindgroup_layout;
    struct ngpu_bindgroup *bindgroup;
    struct ngpu_texture *current_tex;
};

/*
 * Fullscreen triangle: 3 vertices covering [-1,1]x[-1,1] without a vertex
 * buffer (vertex index is used to compute position and UV).
 */
static const char blit_vert[] =
    "void main()"                                                        "\n"
    "{"                                                                  "\n"
    "    float x = -1.0 + float((ngl_vertex_index & 1) << 2);"           "\n"
    "    float y = -1.0 + float((ngl_vertex_index & 2) << 1);"           "\n"
    "    frag_uv = vec2(x * 0.5 + 0.5, y * 0.5 + 0.5);"                  "\n"
    "    ngl_out_pos = vec4(x, y, 0.0, 1.0);"                            "\n"
    "}";

static const char blit_frag[] =
    "void main()"                                                        "\n"
    "{"                                                                  "\n"
    "    ngl_out_color[0] = texture(tex, frag_uv);"                      "\n"
    "}";

struct blit_ctx *viewer_blit_create(struct ngpu_ctx *gpu_ctx)
{
    struct blit_ctx *s = SDL_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->gpu_ctx = gpu_ctx;

    const struct ngpu_pgcraft_texture textures[] = {
        {
            .name        = "tex",
            .type        = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .texture     = NULL, /* Set at draw time. */
            .no_metadata = true,
        },
    };

    static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "frag_uv", .type = NGPU_TYPE_VEC2},
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "blit",
        .vert_base        = blit_vert,
        .frag_base        = blit_frag,
        .textures         = textures,
        .nb_textures      = 1,
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = 1,
        .nb_frag_output   = 1,
    };

    s->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!s->crafter)
        goto fail;
    if (ngpu_pgcraft_craft(s->crafter, &crafter_params) < 0)
        goto fail;

    struct ngpu_bindgroup_layout_desc bg_layout_desc = ngpu_pgcraft_get_bindgroup_layout_desc(s->crafter);
    s->bindgroup_layout = ngpu_bindgroup_layout_create(gpu_ctx);
    if (!s->bindgroup_layout)
        goto fail;
    if (ngpu_bindgroup_layout_init(s->bindgroup_layout, &bg_layout_desc) < 0)
        goto fail;

    struct ngpu_bindgroup_resources bg_resources = ngpu_pgcraft_get_bindgroup_resources(s->crafter);

    const struct ngpu_bindgroup_params bg_params = {
        .layout    = s->bindgroup_layout,
        .resources = bg_resources,
    };
    s->bindgroup = ngpu_bindgroup_create(gpu_ctx);
    if (!s->bindgroup)
        goto fail;
    if (ngpu_bindgroup_init(s->bindgroup, &bg_params) < 0)
        goto fail;

    const struct ngpu_graphics_state state = {
        .blend            = 0,
        .color_write_mask = NGPU_COLOR_COMPONENT_R_BIT
                          | NGPU_COLOR_COMPONENT_G_BIT
                          | NGPU_COLOR_COMPONENT_B_BIT
                          | NGPU_COLOR_COMPONENT_A_BIT,
        .depth_test       = 0,
        .depth_write      = 0,
        .cull_mode        = NGPU_CULL_MODE_NONE,
    };

    const struct ngpu_rendertarget_layout *rt_layout = ngpu_ctx_get_default_rendertarget_layout(gpu_ctx);

    const struct ngpu_pipeline_params pipeline_params = {
        .type     = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics = {
            .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .state        = state,
            .rt_layout    = *rt_layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(s->crafter),
        },
        .program = ngpu_pgcraft_get_program(s->crafter),
        .layout  = {.bindgroup_layout = s->bindgroup_layout},
    };

    s->pipeline = ngpu_pipeline_create(gpu_ctx);
    if (!s->pipeline)
        goto fail;
    if (ngpu_pipeline_init(s->pipeline, &pipeline_params) < 0)
        goto fail;

    return s;

fail:
    viewer_blit_freep(&s);
    return NULL;
}

void viewer_blit_draw(struct blit_ctx *s, struct ngpu_texture *tex,
                      float x, float y, float w, float h)
{
    if (!tex)
        return;

    /* Update texture binding if it changed. */
    if (tex != s->current_tex) {
        const struct ngpu_texture_binding binding = {
            .texture = tex,
        };
        ngpu_bindgroup_update_texture(s->bindgroup, 0, &binding);
        s->current_tex = tex;
    }

    const struct ngpu_viewport viewport = {.x = x, .y = y, .width = w, .height = h};
    ngpu_ctx_set_viewport(s->gpu_ctx, &viewport);
    ngpu_ctx_set_pipeline(s->gpu_ctx, s->pipeline);
    ngpu_ctx_set_bindgroup(s->gpu_ctx, s->bindgroup, NULL, 0);
    ngpu_ctx_draw(s->gpu_ctx, 3, 1, 0);
}

void viewer_blit_freep(struct blit_ctx **sp)
{
    struct blit_ctx *s = *sp;
    if (!s)
        return;
    ngpu_pipeline_freep(&s->pipeline);
    ngpu_bindgroup_freep(&s->bindgroup);
    ngpu_bindgroup_layout_freep(&s->bindgroup_layout);
    ngpu_pgcraft_freep(&s->crafter);
    SDL_free(s);
    *sp = NULL;
}
