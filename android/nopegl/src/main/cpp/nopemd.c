/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jni.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <media/NdkImage.h>

#include <nopemd.h>
#include <ngpu/ngpu.h>

#define CHECK(cond) do {                                                                                 \
    if (!(cond)) {                                                                                       \
        __android_log_print(ANDROID_LOG_FATAL, "ngl", "Assert %s @ %s:%d\n", #cond, __FILE__, __LINE__); \
        abort();                                                                                         \
    }                                                                                                    \
} while (0)

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "nmd", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "nmd", __VA_ARGS__)

/* Match the Android BufferQueue limit so every decoder buffer can be cached */
#define MAX_TEXTURES 64

static const char *blit_vert =
    "const vec2 positions[] = vec2[](vec2(0.0, 0.0), vec2(2.0, 0.0), vec2(0.0, 2.0));\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = positions[ngl_vertex_index];\n"
    "    ngl_out_pos = vec4(uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "    tex_coord = (tex_coord_matrix * vec4(uv, 0.0, 1.0)).xy;\n"
    "}\n";

static const char *blit_frag =
    "void main()\n"
    "{\n"
    "    ngl_out_color = ngl_texvideo(tex, tex_coord);\n"
    "}\n";

static const struct ngpu_pgcraft_texture blit_textures[] = {
    {.name = "tex", .type = NGPU_PGCRAFT_TEXTURE_TYPE_VIDEO, .stage = NGPU_PROGRAM_STAGE_FRAG},
};

static const struct ngpu_pgcraft_iovar blit_vert_out_vars[] = {
    {.name = "tex_coord", .type = NGPU_TYPE_VEC2},
};

struct mat4 {
    _Alignas(16) float m[4 * 4];
};

static const struct mat4 identity = {.m = {
    1.f, 0.f, 0.f, 0.f,
    0.f, 1.f, 0.f, 0.f,
    0.f, 0.f, 1.f, 0.f,
    0.f, 0.f, 0.f, 1.f,
}};

struct ahb_texture {
    AHardwareBuffer *buffer;
    struct ngpu_texture *texture;
    uint64_t draw_index;
};

struct renderer {
    ANativeWindow *window;
    struct ngpu_ctx *gpu_ctx;

    struct ngpu_pgcraft *crafter;
    struct ngpu_bindgroup_layout *bindgroup_layout;
    struct ngpu_pipeline *pipeline;
    struct ngpu_bindgroup_cache *bindgroups;

    struct ngpu_staging_buffer **staging_buffers;
    uint32_t nb_staging_buffers;

    struct ngpu_pgcraft_texture_infos texture_infos;
    size_t metadata_size;
    size_t nb_texture_bindings;
    size_t nb_buffer_bindings;
    size_t nb_dynamic_offsets;
    int32_t metadata_dynamic_index;

    struct mat4 uvcoord_matrix;

    struct ahb_texture textures[MAX_TEXTURES];
    size_t nb_textures;
    uint64_t draw_index;
};

struct player {
    struct nmd_ctx *context;
    struct renderer renderer;
};

static void reset_texture(struct ahb_texture *t)
{
    ngpu_texture_freep(&t->texture);
    AHardwareBuffer_release(t->buffer);
    memset(t, 0, sizeof(*t));
}

static void renderer_reset(struct renderer *s)
{
    if (s->gpu_ctx)
        ngpu_ctx_wait_idle(s->gpu_ctx);

    ngpu_bindgroup_cache_freep(&s->bindgroups);
    for (size_t i = 0; i < s->nb_textures; i++)
        reset_texture(&s->textures[i]);

    for (uint32_t i = 0; i < s->nb_staging_buffers; i++)
        ngpu_staging_buffer_freep(&s->staging_buffers[i]);
    free(s->staging_buffers);

    ngpu_pipeline_freep(&s->pipeline);
    ngpu_bindgroup_layout_freep(&s->bindgroup_layout);
    ngpu_pgcraft_freep(&s->crafter);
    ngpu_ctx_freep(&s->gpu_ctx);

    if (s->window)
        ANativeWindow_release(s->window);

    memset(s, 0, sizeof(*s));
}

static int renderer_init(struct renderer *s, JNIEnv *env, jobject surface)
{
    memset(s, 0, sizeof(*s));

    s->window = ANativeWindow_fromSurface(env, surface);
    if (!s->window)
        return NGPU_ERROR_INVALID_ARG;

    const struct ngpu_ctx_params ctx_params = {
        .platform      = NGPU_PLATFORM_ANDROID,
        .backend       = NGPU_BACKEND_OPENGLES,
        .window        = (uintptr_t)s->window,
        .swap_interval = 0,
        .width         = (uint32_t)ANativeWindow_getWidth(s->window),
        .height        = (uint32_t)ANativeWindow_getHeight(s->window),
    };
    s->gpu_ctx = ngpu_ctx_create(&ctx_params);
    if (!s->gpu_ctx)
        return NGPU_ERROR_MEMORY;

    int ret = ngpu_ctx_init(s->gpu_ctx);
    if (ret < 0) {
        ngpu_ctx_freep(&s->gpu_ctx);
        return ret;
    }

    const uint64_t features = ngpu_ctx_get_features(s->gpu_ctx);
    if (!(features & NGPU_FEATURE_IMPORT_AHARDWARE_BUFFER_BIT)) {
        LOGE("hardware buffer import is not supported");
        return NGPU_ERROR_UNSUPPORTED;
    }

    ngpu_ctx_get_rendertarget_uvcoord_matrix(s->gpu_ctx, s->uvcoord_matrix.m);

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopemd/blit",
        .vert_base        = blit_vert,
        .frag_base        = blit_frag,
        .textures         = blit_textures,
        .nb_textures      = sizeof(blit_textures) / sizeof(*blit_textures),
        .vert_out_vars    = blit_vert_out_vars,
        .nb_vert_out_vars = sizeof(blit_vert_out_vars) / sizeof(*blit_vert_out_vars),
    };
    s->crafter = ngpu_pgcraft_create(s->gpu_ctx);
    if (!s->crafter)
        return NGPU_ERROR_MEMORY;

    ret = ngpu_pgcraft_craft(s->crafter, &crafter_params);
    if (ret < 0)
        return ret;

    s->texture_infos = ngpu_pgcraft_get_texture_infos(s->crafter);
    if (s->texture_infos.nb_infos != 1 || s->texture_infos.infos[0].sampler_oes_index < 0 ||
        s->texture_infos.block_index < 0)
        return NGPU_ERROR_BUG;

    s->metadata_size = s->texture_infos.nb_metadata * sizeof(struct ngpu_pgcraft_texture_info_block);

    struct ngpu_bindgroup_layout_desc layout_desc = ngpu_pgcraft_get_bindgroup_layout_desc(s->crafter);
    s->nb_texture_bindings = layout_desc.nb_textures;
    s->nb_buffer_bindings = layout_desc.nb_buffers;

    s->metadata_dynamic_index = -1;
    for (size_t i = 0, dynamic_index = 0; i < layout_desc.nb_buffers; i++) {
        const enum ngpu_type type = layout_desc.buffers[i].type;
        if (type != NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC && type != NGPU_TYPE_STORAGE_BUFFER_DYNAMIC)
            continue;

        if (i == (size_t)s->texture_infos.block_index)
            s->metadata_dynamic_index = (int32_t)dynamic_index;
        dynamic_index++;
    }

    s->bindgroup_layout = ngpu_bindgroup_layout_create(s->gpu_ctx);
    if (!s->bindgroup_layout)
        return NGPU_ERROR_MEMORY;

    ret = ngpu_bindgroup_layout_init(s->bindgroup_layout, &layout_desc);
    if (ret < 0)
        return ret;

    s->nb_dynamic_offsets = ngpu_bindgroup_layout_get_nb_dynamic_offsets(s->bindgroup_layout);
    if (s->nb_dynamic_offsets > 1 || (s->nb_dynamic_offsets && s->metadata_dynamic_index < 0))
        return NGPU_ERROR_BUG;

    const struct ngpu_pipeline_params pipeline_params = {
        .type     = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics = {
            .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .state        = NGPU_GRAPHICS_STATE_DEFAULTS,
            .rt_layout    = *ngpu_ctx_get_default_rendertarget_layout(s->gpu_ctx),
            .vertex_state = ngpu_pgcraft_get_vertex_state(s->crafter),
        },
        .program = ngpu_pgcraft_get_program(s->crafter),
        .layout  = {.bindgroup_layout = s->bindgroup_layout},
    };
    s->pipeline = ngpu_pipeline_create(s->gpu_ctx);
    if (!s->pipeline)
        return NGPU_ERROR_MEMORY;

    ret = ngpu_pipeline_init(s->pipeline, &pipeline_params);
    if (ret < 0)
        return ret;

    s->bindgroups = ngpu_bindgroup_cache_create(s->gpu_ctx, MAX_TEXTURES);
    if (!s->bindgroups)
        return NGPU_ERROR_MEMORY;

    s->nb_staging_buffers = ngpu_ctx_get_nb_in_flight_frames(s->gpu_ctx);
    s->staging_buffers = calloc(s->nb_staging_buffers, sizeof(*s->staging_buffers));
    if (!s->staging_buffers)
        return NGPU_ERROR_MEMORY;

    for (uint32_t i = 0; i < s->nb_staging_buffers; i++) {
        s->staging_buffers[i] = ngpu_staging_buffer_create(s->gpu_ctx);
        if (!s->staging_buffers[i])
            return NGPU_ERROR_MEMORY;
    }

    return 0;
}

static int get_texture(struct renderer *s, AHardwareBuffer *buffer, const AHardwareBuffer_Desc *desc,
                       struct ngpu_texture **texturep)
{
    s->draw_index++;

    size_t nb_textures = 0;
    for (size_t i = 0; i < s->nb_textures; i++) {
        struct ahb_texture *t = &s->textures[i];
        if (s->draw_index - t->draw_index > MAX_TEXTURES) {
            ngpu_bindgroup_cache_clear(s->bindgroups);
            reset_texture(t);
            continue;
        }

        s->textures[nb_textures++] = *t;
    }
    s->nb_textures = nb_textures;

    for (size_t i = 0; i < s->nb_textures; i++) {
        struct ahb_texture *t = &s->textures[i];
        if (t->buffer == buffer) {
            t->draw_index = s->draw_index;
            *texturep = t->texture;
            return 0;
        }
    }

    if (s->nb_textures == MAX_TEXTURES) {
        ngpu_bindgroup_cache_clear(s->bindgroups);
        for (size_t i = 0; i < s->nb_textures; i++)
            reset_texture(&s->textures[i]);
        s->nb_textures = 0;
    }

    const struct ngpu_texture_params params = {
        .type       = NGPU_TEXTURE_TYPE_2D,
        .format     = NGPU_FORMAT_UNDEFINED,
        .width      = desc->width,
        .height     = desc->height,
        .min_filter = NGPU_FILTER_LINEAR,
        .mag_filter = NGPU_FILTER_LINEAR,
        .usage      = NGPU_TEXTURE_USAGE_SAMPLED_BIT,
        .import_params = {
            .type = NGPU_IMPORT_TYPE_AHARDWARE_BUFFER,
            .ahardware_buffer = {
                .hardware_buffer  = buffer,
                .acquire_fence_fd = -1,
            },
        },
    };
    struct ngpu_texture *texture = ngpu_texture_create(s->gpu_ctx);
    if (!texture)
        return NGPU_ERROR_MEMORY;

    int ret = ngpu_texture_init(texture, &params);
    if (ret < 0) {
        ngpu_texture_freep(&texture);
        return ret;
    }

    /* Keep the buffer alive so another buffer cannot reuse its address as a cache key */
    AHardwareBuffer_acquire(buffer);
    s->textures[s->nb_textures++] = (struct ahb_texture){
        .buffer     = buffer,
        .texture    = texture,
        .draw_index = s->draw_index,
    };

    *texturep = texture;
    return 0;
}

static void get_crop_matrix(struct mat4 *dst, const AHardwareBuffer_Desc *desc, const AImageCropRect *rect)
{
    float sx = 1.f, sy = 1.f, tx = 0.f, ty = 0.f;
    const int32_t width = rect->right - rect->left;
    const int32_t height = rect->bottom - rect->top;
    if (width > 0 && height > 0) {
        const float shrink = 1.f;
        if ((uint32_t)width < desc->width) {
            tx = ((float)rect->left + shrink) / (float)desc->width;
            sx = ((float)width - 2.f * shrink) / (float)desc->width;
        }

        if ((uint32_t)height < desc->height) {
            ty = ((float)rect->top + shrink) / (float)desc->height;
            sy = ((float)height - 2.f * shrink) / (float)desc->height;
        }
    }

    *dst = (struct mat4){.m = {
        sx,  0.f, 0.f, 0.f,
        0.f, sy,  0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        tx,  ty,  0.f, 1.f,
    }};
}

static void mat4_mul(struct mat4 *restrict dst, const struct mat4 *m1, const struct mat4 *m2)
{
    dst->m[ 0] = m1->m[0]*m2->m[ 0] + m1->m[4]*m2->m[ 1] + m1->m[ 8]*m2->m[ 2] + m1->m[12]*m2->m[ 3];
    dst->m[ 1] = m1->m[1]*m2->m[ 0] + m1->m[5]*m2->m[ 1] + m1->m[ 9]*m2->m[ 2] + m1->m[13]*m2->m[ 3];
    dst->m[ 2] = m1->m[2]*m2->m[ 0] + m1->m[6]*m2->m[ 1] + m1->m[10]*m2->m[ 2] + m1->m[14]*m2->m[ 3];
    dst->m[ 3] = m1->m[3]*m2->m[ 0] + m1->m[7]*m2->m[ 1] + m1->m[11]*m2->m[ 2] + m1->m[15]*m2->m[ 3];

    dst->m[ 4] = m1->m[0]*m2->m[ 4] + m1->m[4]*m2->m[ 5] + m1->m[ 8]*m2->m[ 6] + m1->m[12]*m2->m[ 7];
    dst->m[ 5] = m1->m[1]*m2->m[ 4] + m1->m[5]*m2->m[ 5] + m1->m[ 9]*m2->m[ 6] + m1->m[13]*m2->m[ 7];
    dst->m[ 6] = m1->m[2]*m2->m[ 4] + m1->m[6]*m2->m[ 5] + m1->m[10]*m2->m[ 6] + m1->m[14]*m2->m[ 7];
    dst->m[ 7] = m1->m[3]*m2->m[ 4] + m1->m[7]*m2->m[ 5] + m1->m[11]*m2->m[ 6] + m1->m[15]*m2->m[ 7];

    dst->m[ 8] = m1->m[0]*m2->m[ 8] + m1->m[4]*m2->m[ 9] + m1->m[ 8]*m2->m[10] + m1->m[12]*m2->m[11];
    dst->m[ 9] = m1->m[1]*m2->m[ 8] + m1->m[5]*m2->m[ 9] + m1->m[ 9]*m2->m[10] + m1->m[13]*m2->m[11];
    dst->m[10] = m1->m[2]*m2->m[ 8] + m1->m[6]*m2->m[ 9] + m1->m[10]*m2->m[10] + m1->m[14]*m2->m[11];
    dst->m[11] = m1->m[3]*m2->m[ 8] + m1->m[7]*m2->m[ 9] + m1->m[11]*m2->m[10] + m1->m[15]*m2->m[11];

    dst->m[12] = m1->m[0]*m2->m[12] + m1->m[4]*m2->m[13] + m1->m[ 8]*m2->m[14] + m1->m[12]*m2->m[15];
    dst->m[13] = m1->m[1]*m2->m[12] + m1->m[5]*m2->m[13] + m1->m[ 9]*m2->m[14] + m1->m[13]*m2->m[15];
    dst->m[14] = m1->m[2]*m2->m[12] + m1->m[6]*m2->m[13] + m1->m[10]*m2->m[14] + m1->m[14]*m2->m[15];
    dst->m[15] = m1->m[3]*m2->m[12] + m1->m[7]*m2->m[13] + m1->m[11]*m2->m[14] + m1->m[15]*m2->m[15];
}

static int update_frame(struct renderer *s, struct ngpu_staging_buffer *staging_buffer, const struct nmd_frame *frame,
                        AHardwareBuffer *buffer, const AHardwareBuffer_Desc *desc, const AImageCropRect *rect,
                        struct ngpu_texture **texturep, size_t *metadata_offsetp)
{
    int ret = get_texture(s, buffer, desc, texturep);
    if (ret < 0)
        return ret;

    struct ngpu_pgcraft_texture_info_block *metadata = ngpu_staging_buffer_reserve(staging_buffer, s->metadata_size,
                                                                                   metadata_offsetp);
    if (!metadata)
        return NGPU_ERROR_MEMORY;

    struct ngpu_pgcraft_texture_info_block block = {
        .dimensions    = {(float)frame->width, (float)frame->height},
        .sampling_mode = NGPU_IMAGE_LAYOUT_MEDIACODEC,
    };

    struct mat4 crop, coord_matrix;
    get_crop_matrix(&crop, desc, rect);

    mat4_mul(&coord_matrix, &crop, &s->uvcoord_matrix);
    memcpy(block.coord_matrix, coord_matrix.m, sizeof(coord_matrix.m));
    memcpy(block.color_matrix, identity.m, sizeof(identity.m));
    memcpy(block.mapping_color_matrix, identity.m, sizeof(identity.m));

    const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[0];
    memcpy(&metadata[info->metadata_index], &block, sizeof(block));

    return ngpu_staging_buffer_flush(staging_buffer);
}

static int renderer_draw(struct renderer *s, struct nmd_frame **framep)
{
    struct nmd_frame *frame = *framep;
    struct ngpu_bindgroup *bindgroup = NULL;
    int fence_fd = -1;
    int ret;

    const AImage *image = (const AImage *)frame->datap[0];
    AHardwareBuffer *buffer = NULL;
    media_status_t status = AImage_getHardwareBuffer(image, &buffer);
    if (status != AMEDIA_OK || !buffer) {
        ret = NGPU_ERROR_EXTERNAL;
        goto end;
    }

    AImageCropRect rect;
    status = AImage_getCropRect(image, &rect);
    if (status != AMEDIA_OK) {
        ret = NGPU_ERROR_EXTERNAL;
        goto end;
    }

    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(buffer, &desc);

    uint32_t rt_width, rt_height;
    ngpu_ctx_get_default_rendertarget_size(s->gpu_ctx, &rt_width, &rt_height);
    const int32_t width = ANativeWindow_getWidth(s->window);
    const int32_t height = ANativeWindow_getHeight(s->window);
    if (width > 0 && height > 0 && ((uint32_t)width != rt_width || (uint32_t)height != rt_height)) {
        ret = ngpu_ctx_resize(s->gpu_ctx, (uint32_t)width, (uint32_t)height);
        if (ret < 0)
            goto end;

        ngpu_ctx_get_default_rendertarget_size(s->gpu_ctx, &rt_width, &rt_height);
    }

    const uint32_t frame_index = ngpu_ctx_advance_frame(s->gpu_ctx);
    struct ngpu_staging_buffer *staging_buffer = s->staging_buffers[frame_index];
    ngpu_staging_buffer_reset(staging_buffer);

    ret = ngpu_ctx_begin_update(s->gpu_ctx);
    if (ret < 0)
        goto end;

    struct ngpu_texture *texture = NULL;
    size_t metadata_offset = 0;
    ret = update_frame(s, staging_buffer, frame, buffer, &desc, &rect, &texture, &metadata_offset);
    if (ret < 0) {
        ngpu_ctx_end_update(s->gpu_ctx, NULL, NULL);
        goto end;
    }

    ret = ngpu_ctx_end_update(s->gpu_ctx, NULL, NULL);
    if (ret < 0)
        goto end;

    if (s->nb_texture_bindings > 8 || s->nb_buffer_bindings > 8) {
        ret = NGPU_ERROR_LIMIT_EXCEEDED;
        goto end;
    }

    struct ngpu_texture_binding textures[8] = {0};
    struct ngpu_buffer_binding buffers[8] = {0};
    const struct ngpu_pgcraft_texture_info *info = &s->texture_infos.infos[0];
    textures[info->sampler_oes_index].texture = texture;

    const uint32_t dynamic_offsets[1] = {(uint32_t)metadata_offset};
    buffers[s->texture_infos.block_index] = (struct ngpu_buffer_binding){
        .buffer = ngpu_staging_buffer_get_buffer(staging_buffer),
        .offset = s->metadata_dynamic_index >= 0 ? 0 : metadata_offset,
        .size   = s->metadata_size,
    };

    const struct ngpu_bindgroup_desc bindgroup_desc = {
        .layout      = s->bindgroup_layout,
        .textures    = textures,
        .nb_textures = s->nb_texture_bindings,
        .buffers     = buffers,
        .nb_buffers  = s->nb_buffer_bindings,
    };

    bindgroup = ngpu_bindgroup_cache_get(s->bindgroups, &bindgroup_desc);
    if (!bindgroup) {
        ret = NGPU_ERROR_MEMORY;
        goto end;
    }

    ret = ngpu_ctx_begin_draw(s->gpu_ctx);
    if (ret < 0)
        goto end;

    ngpu_ctx_begin_render_pass(s->gpu_ctx, ngpu_ctx_get_default_rendertarget(s->gpu_ctx));
    ngpu_ctx_set_viewport(s->gpu_ctx, &(const struct ngpu_viewport){0.f, 0.f, (float)rt_width, (float)rt_height});
    ngpu_ctx_set_scissor(s->gpu_ctx, &(const struct ngpu_scissor){0, 0, rt_width, rt_height});
    ngpu_ctx_set_pipeline(s->gpu_ctx, s->pipeline);
    ngpu_ctx_set_bindgroup(s->gpu_ctx, bindgroup, dynamic_offsets, s->nb_dynamic_offsets);
    ngpu_ctx_draw(s->gpu_ctx, 3, 1, 0);
    ngpu_ctx_end_render_pass(s->gpu_ctx);

    struct ngpu_fence *fence = NULL;
    ret = ngpu_ctx_end_draw(s->gpu_ctx, frame->ts, NULL, &fence);
    if (ret >= 0 && fence) {
        int export_ret = ngpu_fence_export_sync_file(fence, &fence_fd);
        if (export_ret < 0) {
            LOGW("unable to export the release fence, waiting for the GPU");
            fence_fd = -1;
        }
    }
    ngpu_fence_freep(&fence);

    if (fence_fd < 0)
        ngpu_ctx_wait_idle(s->gpu_ctx);

end:
    ngpu_bindgroup_freep(&bindgroup);
    nmd_mc_frame_releasep(framep, fence_fd);
    return ret;
}

static void player_reset(struct player *s)
{
    nmd_freep(&s->context);
    renderer_reset(&s->renderer);

    memset(s, 0, sizeof(*s));
}

static int player_init(struct player *s, JNIEnv *env, const char *filename, jobject surface)
{
    memset(s, 0, sizeof(*s));

    int ret = renderer_init(&s->renderer, env, surface);
    if (ret < 0) {
        LOGE("unable to initialize the renderer");
        return ret;
    }

    s->context = nmd_create(filename);
    if (!s->context)
        return NMD_ERR_MEMORY;

    nmd_set_option(s->context, "max_nb_packets", 1);
    nmd_set_option(s->context, "max_nb_frames", 1);
    nmd_set_option(s->context, "max_nb_sink", 1);
    nmd_set_option(s->context, "auto_hwaccel", 1);

    return 0;
}

static int player_draw_frame(struct player *s, double t)
{
    struct nmd_frame *frame = NULL;
    int ret = nmd_get_frame(s->context, t, &frame);
    if (ret == NMD_RET_UNCHANGED)
        return NMD_RET_SUCCESS;
    if (ret != NMD_RET_SUCCESS)
        return ret;

    if (frame->pix_fmt != NMD_PIXFMT_MEDIACODEC) {
        LOGW("unsupported frame type: %d", frame->pix_fmt);
        nmd_frame_releasep(&frame);
        return NMD_RET_SUCCESS;
    }

    return renderer_draw(&s->renderer, &frame);
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopemd_Player_nativeInit(JNIEnv *env,
                                                                    jclass clazz,
                                                                    jstring filename_,
                                                                    jobject surface)
{
    /* Matrices need 16-byte alignment, which malloc() does not guarantee on 32-bit Android */
    struct player *player = aligned_alloc(_Alignof(struct player), sizeof(*player));
    if (!player)
        return 0;

    const char *filename = (*env)->GetStringUTFChars(env, filename_, 0);
    CHECK(filename != NULL);

    int ret = player_init(player, env, filename, surface);
    (*env)->ReleaseStringUTFChars(env, filename_, filename);
    if (ret < 0) {
        player_reset(player);
        free(player);
        return 0;
    }

    return (jlong)(uintptr_t)player;
}

JNIEXPORT void JNICALL Java_org_nopeforge_nopemd_Player_nativeStop(JNIEnv *env, jclass clazz, jlong ptr)
{
    struct player *player = (struct player *)(uintptr_t)ptr;
    nmd_stop(player->context);
}

JNIEXPORT int JNICALL Java_org_nopeforge_nopemd_Player_nativeStart(JNIEnv *env, jclass clazz, jlong ptr)
{
    struct player *player = (struct player *)(uintptr_t)ptr;
    return nmd_start(player->context);
}

JNIEXPORT int JNICALL Java_org_nopeforge_nopemd_Player_nativeSeek(JNIEnv *env,
                                                                  jclass clazz,
                                                                  jlong ptr,
                                                                  jdouble position)
{
    struct player *player = (struct player *)(uintptr_t)ptr;
    return nmd_seek(player->context, position);
}

JNIEXPORT int JNICALL Java_org_nopeforge_nopemd_Player_nativeDraw(JNIEnv *env,
                                                                  jclass clazz,
                                                                  jlong ptr,
                                                                  jdouble position)
{
    struct player *player = (struct player *)(uintptr_t)ptr;
    return player_draw_frame(player, position);
}

JNIEXPORT void JNICALL Java_org_nopeforge_nopemd_Player_nativeRelease(JNIEnv *env, jclass clazz, jlong ptr)
{
    struct player *player = (struct player *)(uintptr_t)ptr;
    player_reset(player);
    free(player);
}
