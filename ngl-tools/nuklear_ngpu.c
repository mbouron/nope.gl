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

#define NK_IMPLEMENTATION
#include "nuklear_ngpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define MAX_VERTEX_BUFFER  (512 * 1024)
#define MAX_INDEX_BUFFER   (128 * 1024)

struct nk_ngpu_vertex {
    float position[2];
    float uv[2];
    float col[4];
};

struct nk_ngpu_proj {
    float proj_mat[16];
};

struct nk_ngpu_ctx {
    struct ngpu_ctx *gpu_ctx;
    struct nk_context nk;
    struct nk_font_atlas atlas;
    struct nk_buffer cmds;
    struct nk_draw_null_texture tex_null;

    /* GPU resources. */
    struct ngpu_texture *font_tex;
    struct ngpu_buffer *vbo;
    struct ngpu_pgcraft *crafter;
    struct ngpu_pipeline *pipeline;
    struct ngpu_bindgroup_layout *bg_layout;
    struct ngpu_bindgroup *bindgroup;

    /* Projection UBO (dedicated buffer, updated via upload). */
    struct ngpu_block_desc proj_block_desc;
    struct ngpu_buffer *proj_buffer;
    int32_t proj_block_index;

    /* Single index buffer for all draw commands. */
    struct ngpu_buffer *ibo;
    size_t nb_draw_cmds;

    /* Scratch buffers reused across frames to avoid per-frame allocations. */
    void *vmem;
    void *imem;
};

static const char nk_vert[] =
    "void main()"                                                            "\n"
    "{"                                                                      "\n"
    "    frag_uv = texcoord;"                                                "\n"
    "    frag_color = color;"                                                "\n"
    "    ngl_out_pos = proj_mat * vec4(position, 0.0, 1.0);"                 "\n"
    "}";

static const char nk_frag[] =
    "void main()"                                                            "\n"
    "{"                                                                      "\n"
    "    ngl_out_color[0] = frag_color * texture(tex, frag_uv);"             "\n"
    "}";

struct nk_ngpu_ctx *nk_ngpu_create(struct ngpu_ctx *gpu_ctx)
{
    struct nk_ngpu_ctx *s = SDL_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->gpu_ctx = gpu_ctx;
    return s;
}

static int init_font_atlas(struct nk_ngpu_ctx *s, float font_size, const char *font_path)
{
    const void *image;
    int w, h;

    nk_font_atlas_init_default(&s->atlas);
    nk_font_atlas_begin(&s->atlas);
    /* Try the requested TTF first; fall back to Nuklear's bundled default
     * font if the file is missing or unreadable. */
    struct nk_font *font = NULL;
    if (font_path)
        font = nk_font_atlas_add_from_file(&s->atlas, font_path, font_size, NULL);
    if (!font)
        font = nk_font_atlas_add_default(&s->atlas, font_size, NULL);
    image = nk_font_atlas_bake(&s->atlas, &w, &h, NK_FONT_ATLAS_RGBA32);
    if (!image)
        return -1;

    s->font_tex = ngpu_texture_create(s->gpu_ctx);
    if (!s->font_tex)
        return NGPU_ERROR_MEMORY;

    const struct ngpu_texture_params tex_params = {
        .type       = NGPU_TEXTURE_TYPE_2D,
        .format     = NGPU_FORMAT_R8G8B8A8_UNORM,
        .width      = (uint32_t)w,
        .height     = (uint32_t)h,
        .depth      = 1,
        .min_filter = NGPU_FILTER_LINEAR,
        .mag_filter = NGPU_FILTER_LINEAR,
        .usage      = NGPU_TEXTURE_USAGE_SAMPLED_BIT
                    | NGPU_TEXTURE_USAGE_TRANSFER_DST_BIT,
    };
    int ret = ngpu_texture_init(s->font_tex, &tex_params);
    if (ret < 0)
        return ret;

    ret = ngpu_texture_upload(s->font_tex, image, 0);
    if (ret < 0)
        return ret;

    nk_font_atlas_end(&s->atlas, nk_handle_id(0), &s->tex_null);
    nk_style_set_font(&s->nk, &font->handle);

    return 0;
}

static int init_buffers(struct nk_ngpu_ctx *s)
{
    s->vbo = ngpu_buffer_create(s->gpu_ctx);
    if (!s->vbo)
        return NGPU_ERROR_MEMORY;
    int ret = ngpu_buffer_init(s->vbo, MAX_VERTEX_BUFFER,
                               NGPU_BUFFER_USAGE_VERTEX_BUFFER_BIT
                             | NGPU_BUFFER_USAGE_DYNAMIC_BIT
                             | NGPU_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (ret < 0)
        return ret;

    s->ibo = ngpu_buffer_create(s->gpu_ctx);
    if (!s->ibo)
        return NGPU_ERROR_MEMORY;
    ret = ngpu_buffer_init(s->ibo, MAX_INDEX_BUFFER,
                           NGPU_BUFFER_USAGE_INDEX_BUFFER_BIT
                         | NGPU_BUFFER_USAGE_DYNAMIC_BIT
                         | NGPU_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (ret < 0)
        return ret;

    s->vmem = SDL_malloc(MAX_VERTEX_BUFFER);
    s->imem = SDL_malloc(MAX_INDEX_BUFFER);
    if (!s->vmem || !s->imem)
        return NGPU_ERROR_MEMORY;

    return 0;
}

static struct ngpu_buffer *create_ubo(struct ngpu_ctx *gpu_ctx, size_t size, const void *data)
{
    struct ngpu_buffer *buf = ngpu_buffer_create(gpu_ctx);
    if (!buf)
        return NULL;
    int ret = ngpu_buffer_init(buf, size,
                               NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                             | NGPU_BUFFER_USAGE_DYNAMIC_BIT
                             | NGPU_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (ret < 0) {
        ngpu_buffer_freep(&buf);
        return NULL;
    }
    if (data) {
        ret = ngpu_buffer_upload(buf, data, 0, size);
        if (ret < 0) {
            ngpu_buffer_freep(&buf);
            return NULL;
        }
    }
    return buf;
}

static int init_pipeline(struct nk_ngpu_ctx *s)
{
    int ret;

    /* Projection uniform block descriptor. */
    ngpu_block_desc_init(s->gpu_ctx, &s->proj_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ret = ngpu_block_desc_add_field(&s->proj_block_desc, "proj_mat", NGPU_TYPE_MAT4, 0);
    if (ret < 0)
        return ret;
    const size_t proj_size = ngpu_block_desc_get_size(&s->proj_block_desc, 0);

    /* Create projection UBO (zero-initialized, updated per-frame). */
    struct nk_ngpu_proj zero_proj = {0};
    s->proj_buffer = create_ubo(s->gpu_ctx, proj_size, &zero_proj);
    if (!s->proj_buffer)
        return NGPU_ERROR_MEMORY;

    /* Pgcraft setup. */
    const struct ngpu_pgcraft_attribute attributes[] = {
        {
            .name   = "position",
            .type   = NGPU_TYPE_VEC2,
            .format = NGPU_FORMAT_R32G32_SFLOAT,
            .stride = sizeof(struct nk_ngpu_vertex),
            .offset = offsetof(struct nk_ngpu_vertex, position),
            .buffer = s->vbo,
        }, {
            .name   = "texcoord",
            .type   = NGPU_TYPE_VEC2,
            .format = NGPU_FORMAT_R32G32_SFLOAT,
            .stride = sizeof(struct nk_ngpu_vertex),
            .offset = offsetof(struct nk_ngpu_vertex, uv),
            .buffer = s->vbo,
        }, {
            .name   = "color",
            .type   = NGPU_TYPE_VEC4,
            .format = NGPU_FORMAT_R32G32B32A32_SFLOAT,
            .stride = sizeof(struct nk_ngpu_vertex),
            .offset = offsetof(struct nk_ngpu_vertex, col),
            .buffer = s->vbo,
        },
    };

    const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "frag_uv",    .type = NGPU_TYPE_VEC2},
        {.name = "frag_color", .type = NGPU_TYPE_VEC4},
    };

    const struct ngpu_pgcraft_texture textures[] = {
        {
            .name        = "tex",
            .type        = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .texture     = s->font_tex,
            .no_metadata = true,
        },
    };

    const struct ngpu_pgcraft_block blocks[] = {
        {
            .name          = "projection",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_VERT,
            .block         = &s->proj_block_desc,
            .buffer        = {.buffer = s->proj_buffer, .size = proj_size},
        },
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nuklear",
        .vert_base        = nk_vert,
        .frag_base        = nk_frag,
        .attributes       = attributes,
        .nb_attributes    = 3,
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = 2,
        .textures         = textures,
        .nb_textures      = 1,
        .blocks           = blocks,
        .nb_blocks        = 1,
        .nb_frag_output   = 1,
    };

    s->crafter = ngpu_pgcraft_create(s->gpu_ctx);
    if (!s->crafter)
        return NGPU_ERROR_MEMORY;
    ret = ngpu_pgcraft_craft(s->crafter, &crafter_params);
    if (ret < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "nuklear_ngpu: pgcraft_craft failed: %d", ret);
        return ret;
    }

    s->proj_block_index = ngpu_pgcraft_get_block_index(s->crafter, "projection", NGPU_PROGRAM_STAGE_VERT);

    struct ngpu_bindgroup_layout_desc bg_layout_desc = ngpu_pgcraft_get_bindgroup_layout_desc(s->crafter);
    s->bg_layout = ngpu_bindgroup_layout_create(s->gpu_ctx);
    if (!s->bg_layout)
        return NGPU_ERROR_MEMORY;

    ret = ngpu_bindgroup_layout_init(s->bg_layout, &bg_layout_desc);
    if (ret < 0)
        return ret;

    struct ngpu_bindgroup_resources bg_resources =
        ngpu_pgcraft_get_bindgroup_resources(s->crafter);

    const struct ngpu_bindgroup_params bg_params = {
        .layout    = s->bg_layout,
        .resources = bg_resources,
    };
    s->bindgroup = ngpu_bindgroup_create(s->gpu_ctx);
    if (!s->bindgroup)
        return NGPU_ERROR_MEMORY;
    ret = ngpu_bindgroup_init(s->bindgroup, &bg_params);
    if (ret < 0)
        return ret;

    /* Explicitly set all bindings (GL backend requires update calls after init). */
    for (size_t i = 0; i < bg_resources.nb_textures; i++)
        ngpu_bindgroup_update_texture(s->bindgroup, (int32_t)i, &bg_resources.textures[i]);
    for (size_t i = 0; i < bg_resources.nb_buffers; i++)
        ngpu_bindgroup_update_buffer(s->bindgroup, (int32_t)i, &bg_resources.buffers[i]);

    /* Pipeline. */
    const struct ngpu_graphics_state graphics_state = {
        .blend            = 1,
        .blend_src_factor   = NGPU_BLEND_FACTOR_SRC_ALPHA,
        .blend_dst_factor   = NGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .blend_src_factor_a = NGPU_BLEND_FACTOR_ONE,
        .blend_dst_factor_a = NGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .blend_op           = NGPU_BLEND_OP_ADD,
        .blend_op_a         = NGPU_BLEND_OP_ADD,
        .color_write_mask   = NGPU_COLOR_COMPONENT_R_BIT
                            | NGPU_COLOR_COMPONENT_G_BIT
                            | NGPU_COLOR_COMPONENT_B_BIT
                            | NGPU_COLOR_COMPONENT_A_BIT,
        .depth_test         = 0,
        .depth_write        = 0,
        .cull_mode          = NGPU_CULL_MODE_NONE,
    };

    const struct ngpu_rendertarget_layout *rt_layout =
        ngpu_ctx_get_default_rendertarget_layout(s->gpu_ctx);

    const struct ngpu_pipeline_params pipeline_params = {
        .type     = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics = {
            .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .state        = graphics_state,
            .rt_layout    = *rt_layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(s->crafter),
        },
        .program = ngpu_pgcraft_get_program(s->crafter),
        .layout  = {.bindgroup_layout = s->bg_layout},
    };

    s->pipeline = ngpu_pipeline_create(s->gpu_ctx);
    if (!s->pipeline) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "nuklear_ngpu: pipeline_create failed");
        return NGPU_ERROR_MEMORY;
    }
    ret = ngpu_pipeline_init(s->pipeline, &pipeline_params);
    if (ret < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "nuklear_ngpu: pipeline_init failed: %d", ret);
        return ret;
    }

    return 0;
}

int nk_ngpu_init(struct nk_ngpu_ctx *s, float font_size, const char *font_path)
{
    if (!nk_init_default(&s->nk, NULL))
        return -1;
    nk_buffer_init_default(&s->cmds);

    int ret = init_buffers(s);
    if (ret < 0)
        return ret;

    ret = init_font_atlas(s, font_size, font_path);
    if (ret < 0)
        return ret;

    ret = init_pipeline(s);
    if (ret < 0)
        return ret;

    return 0;
}

struct nk_context *nk_ngpu_get_nk_ctx(struct nk_ngpu_ctx *s)
{
    return &s->nk;
}

static void update_projection(struct nk_ngpu_ctx *s, uint32_t width, uint32_t height)
{
    const int vk = ngpu_ctx_get_backend_type(s->gpu_ctx) == NGPU_BACKEND_VULKAN;
    const float L = 0.0f;
    const float R = (float)width;
    const float T = vk ? (float)height : 0.0f;
    const float B = vk ? 0.0f : (float)height;
    const struct nk_ngpu_proj proj = {
        .proj_mat = {
            2.0f/(R-L),     0.0f,           0.0f, 0.0f,
            0.0f,           2.0f/(T-B),     0.0f, 0.0f,
            0.0f,           0.0f,          -1.0f, 0.0f,
           -(R+L)/(R-L),   -(T+B)/(T-B),    0.0f, 1.0f,
        },
    };
    int ret = ngpu_buffer_upload(s->proj_buffer, &proj,
                                 0, ngpu_block_desc_get_size(&s->proj_block_desc, 0));
    if (ret < 0)
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "nuklear_ngpu: projection upload failed: %d", ret);
}

/*
 * Prepare Nuklear draw data: convert commands to vertex/index buffers and
 * upload to GPU. Must be called BEFORE the render pass (outside
 * begin_render_pass / end_render_pass).
 */
void nk_ngpu_prepare(struct nk_ngpu_ctx *s, enum nk_anti_aliasing aa,
                     uint32_t width, uint32_t height)
{
    if (!width || !height)
        return;

    update_projection(s, width, height);

    static const struct nk_draw_vertex_layout_element vertex_layout[] = {
        {NK_VERTEX_POSITION, NK_FORMAT_FLOAT,    offsetof(struct nk_ngpu_vertex, position)},
        {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT,    offsetof(struct nk_ngpu_vertex, uv)},
        {NK_VERTEX_COLOR,    NK_FORMAT_R32G32B32A32_FLOAT, offsetof(struct nk_ngpu_vertex, col)},
        {NK_VERTEX_LAYOUT_END}
    };

    struct nk_convert_config config = {
        .vertex_layout     = vertex_layout,
        .vertex_size       = sizeof(struct nk_ngpu_vertex),
        .vertex_alignment  = NK_ALIGNOF(struct nk_ngpu_vertex),
        .tex_null          = s->tex_null,
        .circle_segment_count  = 22,
        .curve_segment_count   = 22,
        .arc_segment_count     = 22,
        .global_alpha          = 1.0f,
        .shape_AA              = aa,
        .line_AA               = aa,
    };

    struct nk_buffer vbuf, ibuf;
    nk_buffer_init_fixed(&vbuf, s->vmem, MAX_VERTEX_BUFFER);
    nk_buffer_init_fixed(&ibuf, s->imem, MAX_INDEX_BUFFER);

    const nk_flags conv_ret = nk_convert(&s->nk, &s->cmds, &vbuf, &ibuf, &config);
    if (conv_ret != NK_CONVERT_SUCCESS) {
        /* The UI exceeded our scratch buffers; the truncated draw stream is
         * still safe to submit but will render incomplete. Bump
         * MAX_VERTEX_BUFFER/MAX_INDEX_BUFFER if this fires regularly. */
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "nuklear_ngpu: nk_convert overflow (flags=0x%x)", conv_ret);
    }

    int ret = ngpu_buffer_upload(s->vbo, nk_buffer_memory_const(&vbuf), 0, vbuf.allocated);
    if (ret < 0)
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "nuklear_ngpu: vbo upload failed: %d", ret);

    ret = ngpu_buffer_upload(s->ibo, nk_buffer_memory_const(&ibuf), 0, ibuf.allocated);
    if (ret < 0)
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "nuklear_ngpu: ibo upload failed: %d", ret);

    size_t cmd_count = 0;
    const struct nk_draw_command *cmd;
    nk_draw_foreach(cmd, &s->nk, &s->cmds)
        if (cmd->elem_count)
            cmd_count++;
    s->nb_draw_cmds = cmd_count;

    nk_buffer_free(&vbuf);
    nk_buffer_free(&ibuf);
}

/*
 * Render Nuklear draw commands. Must be called inside an active render pass.
 * Dimensions are physical pixels — Nuklear's layout uses physical units for
 * everything, so the viewport and scissor coords go straight through.
 */
void nk_ngpu_render(struct nk_ngpu_ctx *s, uint32_t width, uint32_t height)
{
    if (!width || !height || !s->nb_draw_cmds)
        return;
    /* Set pipeline state. */
    ngpu_ctx_set_pipeline(s->gpu_ctx, s->pipeline);
    ngpu_ctx_set_bindgroup(s->gpu_ctx, s->bindgroup, NULL, 0);

    /* Bind all vertex buffer slots (all attributes share the same vbo). */
    static const char *attr_names[] = {"position", "texcoord", "color"};
    for (size_t i = 0; i < 3; i++) {
        const int32_t vb_index = ngpu_pgcraft_get_vertex_buffer_index(s->crafter, attr_names[i]);
        if (vb_index >= 0)
            ngpu_ctx_set_vertex_buffer(s->gpu_ctx, (uint32_t)vb_index, s->vbo);
    }

    /* Viewport spans the full framebuffer (also resets it after viewer_blit_draw). */
    const struct ngpu_viewport full_vp = {.x = 0, .y = 0, .width = (float)width, .height = (float)height};
    ngpu_ctx_set_viewport(s->gpu_ctx, &full_vp);

    ngpu_ctx_set_index_buffer(s->gpu_ctx, s->ibo, NGPU_FORMAT_R16_UNORM);

    const struct nk_draw_command *cmd;
    uint32_t first_index = 0;
    nk_draw_foreach(cmd, &s->nk, &s->cmds) {
        if (!cmd->elem_count)
            continue;

        const int32_t clip_y = (int32_t)NK_MAX(cmd->clip_rect.y, 0);
        const int32_t clip_h = (int32_t)cmd->clip_rect.h;
        const struct ngpu_scissor scissor = {
            .x      = (uint32_t)NK_MAX(cmd->clip_rect.x, 0),
            .y      = (uint32_t)NK_MAX((int32_t)height - clip_y - clip_h, 0),
            .width  = (uint32_t)cmd->clip_rect.w,
            .height = (uint32_t)clip_h,
        };
        ngpu_ctx_set_scissor(s->gpu_ctx, &scissor);

        ngpu_ctx_draw_indexed(s->gpu_ctx, cmd->elem_count, 1, first_index);
        first_index += cmd->elem_count;
    }

    nk_clear(&s->nk);
    nk_buffer_clear(&s->cmds);
    s->nb_draw_cmds = 0;
}

void nk_ngpu_freep(struct nk_ngpu_ctx **sp)
{
    struct nk_ngpu_ctx *s = *sp;
    if (!s)
        return;

    ngpu_pipeline_freep(&s->pipeline);
    ngpu_bindgroup_freep(&s->bindgroup);
    ngpu_bindgroup_layout_freep(&s->bg_layout);
    ngpu_pgcraft_freep(&s->crafter);
    ngpu_buffer_freep(&s->proj_buffer);
    ngpu_block_desc_reset(&s->proj_block_desc);
    ngpu_buffer_freep(&s->vbo);
    ngpu_buffer_freep(&s->ibo);
    ngpu_texture_freep(&s->font_tex);

    nk_buffer_free(&s->cmds);
    nk_free(&s->nk);
    nk_font_atlas_clear(&s->atlas);

    SDL_free(s->vmem);
    SDL_free(s->imem);

    SDL_free(s);
    *sp = NULL;
}
