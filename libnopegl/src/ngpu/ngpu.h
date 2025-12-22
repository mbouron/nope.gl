/*
 * Copyright 2023-2025 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2018-2022 GoPro Inc.
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

#ifndef NGPU_H
#define NGPU_H

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#include "utils/utils.h"
#include "utils/refcount.h"
#include "utils/darray.h"

/*
 * Forward declarations
 */

struct ngpu_capture_ctx;
struct ngpu_ctx;
struct ngpu_ctx_class;
struct ngpu_pgcache;

/*
 * Types
 */

enum ngpu_precision {
    NGPU_PRECISION_AUTO,
    NGPU_PRECISION_HIGH,
    NGPU_PRECISION_MEDIUM,
    NGPU_PRECISION_LOW,
    NGPU_PRECISION_NB,
    NGPU_PRECISION_MAX_ENUM = 0x7FFFFFFF
};

enum ngpu_type {
    NGPU_TYPE_NONE,
    NGPU_TYPE_I32,
    NGPU_TYPE_IVEC2,
    NGPU_TYPE_IVEC3,
    NGPU_TYPE_IVEC4,
    NGPU_TYPE_U32,
    NGPU_TYPE_UVEC2,
    NGPU_TYPE_UVEC3,
    NGPU_TYPE_UVEC4,
    NGPU_TYPE_F32,
    NGPU_TYPE_VEC2,
    NGPU_TYPE_VEC3,
    NGPU_TYPE_VEC4,
    NGPU_TYPE_MAT3,
    NGPU_TYPE_MAT4,
    NGPU_TYPE_BOOL,
    NGPU_TYPE_SAMPLER_2D,
    NGPU_TYPE_SAMPLER_2D_ARRAY,
    NGPU_TYPE_SAMPLER_2D_RECT,
    NGPU_TYPE_SAMPLER_3D,
    NGPU_TYPE_SAMPLER_CUBE,
    NGPU_TYPE_SAMPLER_EXTERNAL_OES,
    NGPU_TYPE_SAMPLER_EXTERNAL_2D_Y2Y_EXT,
    NGPU_TYPE_IMAGE_2D,
    NGPU_TYPE_IMAGE_2D_ARRAY,
    NGPU_TYPE_IMAGE_3D,
    NGPU_TYPE_IMAGE_CUBE,
    NGPU_TYPE_UNIFORM_BUFFER,
    NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC,
    NGPU_TYPE_STORAGE_BUFFER,
    NGPU_TYPE_STORAGE_BUFFER_DYNAMIC,
    NGPU_TYPE_NB,
    NGPU_TYPE_MAX_ENUM = 0x7FFFFFFF
};

const char *ngpu_type_get_name(enum ngpu_type type);

/*
 * Limits
 */

#define NGPU_MAX_ATTRIBUTES_PER_BUFFER 16
#define NGPU_MAX_UNIFORM_BUFFERS_DYNAMIC 8
#define NGPU_MAX_STORAGE_BUFFERS_DYNAMIC 4
#define NGPU_MAX_DYNAMIC_OFFSETS (NGPU_MAX_UNIFORM_BUFFERS_DYNAMIC + NGPU_MAX_STORAGE_BUFFERS_DYNAMIC)
#define NGPU_MAX_VERTEX_BUFFERS 32

#define NGPU_MAX_COLOR_ATTACHMENTS 8

struct ngpu_limits {
    uint32_t max_vertex_attributes;
    uint32_t max_texture_image_units;
    uint32_t max_image_units;
    uint32_t max_compute_work_group_count[3];
    uint32_t max_compute_work_group_invocations;
    uint32_t max_compute_work_group_size[3];
    uint32_t max_compute_shared_memory_size;
    uint32_t max_uniform_block_size;
    size_t min_uniform_block_offset_alignment;
    uint32_t max_storage_block_size;
    size_t min_storage_block_offset_alignment;
    uint32_t max_samples;
    uint32_t max_texture_dimension_1d;
    uint32_t max_texture_dimension_2d;
    uint32_t max_texture_dimension_3d;
    uint32_t max_texture_dimension_cube;
    uint32_t max_texture_array_layers;
    uint32_t max_color_attachments;
    uint32_t max_draw_buffers;
};

/*
 * Formats
 */

enum ngpu_format {
    NGPU_FORMAT_UNDEFINED,
    NGPU_FORMAT_R8_UNORM,
    NGPU_FORMAT_R8_SNORM,
    NGPU_FORMAT_R8_UINT,
    NGPU_FORMAT_R8_SINT,
    NGPU_FORMAT_R8_SRGB,
    NGPU_FORMAT_R8G8_UNORM,
    NGPU_FORMAT_R8G8_SNORM,
    NGPU_FORMAT_R8G8_UINT,
    NGPU_FORMAT_R8G8_SINT,
    NGPU_FORMAT_R8G8_SRGB,
    NGPU_FORMAT_R8G8B8_UNORM,
    NGPU_FORMAT_R8G8B8_SNORM,
    NGPU_FORMAT_R8G8B8_UINT,
    NGPU_FORMAT_R8G8B8_SINT,
    NGPU_FORMAT_R8G8B8_SRGB,
    NGPU_FORMAT_R8G8B8A8_UNORM,
    NGPU_FORMAT_R8G8B8A8_SNORM,
    NGPU_FORMAT_R8G8B8A8_UINT,
    NGPU_FORMAT_R8G8B8A8_SINT,
    NGPU_FORMAT_R8G8B8A8_SRGB,
    NGPU_FORMAT_B8G8R8A8_UNORM,
    NGPU_FORMAT_B8G8R8A8_SNORM,
    NGPU_FORMAT_B8G8R8A8_UINT,
    NGPU_FORMAT_B8G8R8A8_SINT,
    NGPU_FORMAT_B8G8R8A8_SRGB,
    NGPU_FORMAT_R16_UNORM,
    NGPU_FORMAT_R16_SNORM,
    NGPU_FORMAT_R16_UINT,
    NGPU_FORMAT_R16_SINT,
    NGPU_FORMAT_R16_SFLOAT,
    NGPU_FORMAT_R16G16_UNORM,
    NGPU_FORMAT_R16G16_SNORM,
    NGPU_FORMAT_R16G16_UINT,
    NGPU_FORMAT_R16G16_SINT,
    NGPU_FORMAT_R16G16_SFLOAT,
    NGPU_FORMAT_R16G16B16_UNORM,
    NGPU_FORMAT_R16G16B16_SNORM,
    NGPU_FORMAT_R16G16B16_UINT,
    NGPU_FORMAT_R16G16B16_SINT,
    NGPU_FORMAT_R16G16B16_SFLOAT,
    NGPU_FORMAT_R16G16B16A16_UNORM,
    NGPU_FORMAT_R16G16B16A16_SNORM,
    NGPU_FORMAT_R16G16B16A16_UINT,
    NGPU_FORMAT_R16G16B16A16_SINT,
    NGPU_FORMAT_R16G16B16A16_SFLOAT,
    NGPU_FORMAT_R32_UINT,
    NGPU_FORMAT_R32_SINT,
    NGPU_FORMAT_R32_SFLOAT,
    NGPU_FORMAT_R32G32_UINT,
    NGPU_FORMAT_R32G32_SINT,
    NGPU_FORMAT_R32G32_SFLOAT,
    NGPU_FORMAT_R32G32B32_UINT,
    NGPU_FORMAT_R32G32B32_SINT,
    NGPU_FORMAT_R32G32B32_SFLOAT,
    NGPU_FORMAT_R32G32B32A32_UINT,
    NGPU_FORMAT_R32G32B32A32_SINT,
    NGPU_FORMAT_R32G32B32A32_SFLOAT,
    NGPU_FORMAT_R64_SINT,
    NGPU_FORMAT_D16_UNORM,
    NGPU_FORMAT_X8_D24_UNORM_PACK32,
    NGPU_FORMAT_D32_SFLOAT,
    NGPU_FORMAT_D24_UNORM_S8_UINT,
    NGPU_FORMAT_D32_SFLOAT_S8_UINT,
    NGPU_FORMAT_S8_UINT,
    NGPU_FORMAT_NB,
    NGPU_FORMAT_MAX_ENUM = 0x7FFFFFFF
};

enum {
    NGPU_FORMAT_FEATURE_SAMPLED_IMAGE_BIT               = 1 << 0,
    NGPU_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT = 1 << 1,
    NGPU_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT            = 1 << 2,
    NGPU_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT      = 1 << 3,
    NGPU_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT    = 1 << 4,
};

size_t ngpu_format_get_bytes_per_pixel(enum ngpu_format format);
size_t ngpu_format_get_nb_comp(enum ngpu_format format);
int ngpu_format_has_depth(enum ngpu_format format);
int ngpu_format_has_stencil(enum ngpu_format format);

/*
 * Graphics state
 */

enum ngpu_blend_factor {
    NGPU_BLEND_FACTOR_ZERO,
    NGPU_BLEND_FACTOR_ONE,
    NGPU_BLEND_FACTOR_SRC_COLOR,
    NGPU_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    NGPU_BLEND_FACTOR_DST_COLOR,
    NGPU_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
    NGPU_BLEND_FACTOR_SRC_ALPHA,
    NGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    NGPU_BLEND_FACTOR_DST_ALPHA,
    NGPU_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    NGPU_BLEND_FACTOR_CONSTANT_COLOR,
    NGPU_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
    NGPU_BLEND_FACTOR_CONSTANT_ALPHA,
    NGPU_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
    NGPU_BLEND_FACTOR_SRC_ALPHA_SATURATE,
    NGPU_BLEND_FACTOR_NB,
    NGPU_BLEND_FACTOR_MAX_ENUM = 0x7FFFFFFF
};

enum ngpu_blend_op {
    NGPU_BLEND_OP_ADD,
    NGPU_BLEND_OP_SUBTRACT,
    NGPU_BLEND_OP_REVERSE_SUBTRACT,
    NGPU_BLEND_OP_MIN,
    NGPU_BLEND_OP_MAX,
    NGPU_BLEND_OP_NB,
    NGPU_BLEND_OP_MAX_ENUM = 0x7FFFFFFF
};

enum ngpu_compare_op {
    NGPU_COMPARE_OP_NEVER,
    NGPU_COMPARE_OP_LESS,
    NGPU_COMPARE_OP_EQUAL,
    NGPU_COMPARE_OP_LESS_OR_EQUAL,
    NGPU_COMPARE_OP_GREATER,
    NGPU_COMPARE_OP_NOT_EQUAL,
    NGPU_COMPARE_OP_GREATER_OR_EQUAL,
    NGPU_COMPARE_OP_ALWAYS,
    NGPU_COMPARE_OP_NB,
    NGPU_COMPARE_OP_MAX_ENUM = 0x7FFFFFFF
};

enum ngpu_stencil_op {
    NGPU_STENCIL_OP_KEEP,
    NGPU_STENCIL_OP_ZERO,
    NGPU_STENCIL_OP_REPLACE,
    NGPU_STENCIL_OP_INCREMENT_AND_CLAMP,
    NGPU_STENCIL_OP_DECREMENT_AND_CLAMP,
    NGPU_STENCIL_OP_INVERT,
    NGPU_STENCIL_OP_INCREMENT_AND_WRAP,
    NGPU_STENCIL_OP_DECREMENT_AND_WRAP,
    NGPU_STENCIL_OP_NB,
    NGPU_STENCIL_OP_MAX_ENUM = 0x7FFFFFFF
};

enum ngpu_cull_mode {
    NGPU_CULL_MODE_NONE,
    NGPU_CULL_MODE_FRONT_BIT,
    NGPU_CULL_MODE_BACK_BIT,
    NGPU_CULL_MODE_NB,
    NGPU_CULL_MODE_MAX_ENUM = 0x7FFFFFFF
};

enum {
    NGPU_COLOR_COMPONENT_R_BIT = 1 << 0,
    NGPU_COLOR_COMPONENT_G_BIT = 1 << 1,
    NGPU_COLOR_COMPONENT_B_BIT = 1 << 2,
    NGPU_COLOR_COMPONENT_A_BIT = 1 << 3,
};

enum ngpu_front_face {
    NGPU_FRONT_FACE_COUNTER_CLOCKWISE,
    NGPU_FRONT_FACE_CLOCKWISE,
    NGPU_FRONT_FACE_NB,
    NGPU_FRONT_FACE_MAX_ENUM = 0x7FFFFFFF
};

struct ngpu_stencil_op_state {
    uint32_t write_mask;
    enum ngpu_compare_op func;
    uint32_t ref;
    uint32_t read_mask;
    enum ngpu_stencil_op fail;
    enum ngpu_stencil_op depth_fail;
    enum ngpu_stencil_op depth_pass;
};

struct ngpu_graphics_state {
    int blend;
    enum ngpu_blend_factor blend_dst_factor;
    enum ngpu_blend_factor blend_src_factor;
    enum ngpu_blend_factor blend_dst_factor_a;
    enum ngpu_blend_factor blend_src_factor_a;
    enum ngpu_blend_op blend_op;
    enum ngpu_blend_op blend_op_a;

    uint32_t color_write_mask;

    int depth_test;
    int depth_write;
    enum ngpu_compare_op depth_func;

    int stencil_test;
    struct ngpu_stencil_op_state stencil_front;
    struct ngpu_stencil_op_state stencil_back;

    enum ngpu_cull_mode cull_mode;
    enum ngpu_front_face front_face;
};

/* Make sure to keep this in sync with the blending documentation */
#define NGPU_GRAPHICS_STATE_DEFAULTS (struct ngpu_graphics_state) {    \
    .blend              = 0,                                           \
    .blend_src_factor   = NGPU_BLEND_FACTOR_ONE,                       \
    .blend_dst_factor   = NGPU_BLEND_FACTOR_ZERO,                      \
    .blend_src_factor_a = NGPU_BLEND_FACTOR_ONE,                       \
    .blend_dst_factor_a = NGPU_BLEND_FACTOR_ZERO,                      \
    .blend_op           = NGPU_BLEND_OP_ADD,                           \
    .blend_op_a         = NGPU_BLEND_OP_ADD,                           \
    .color_write_mask   = NGPU_COLOR_COMPONENT_R_BIT                   \
                        | NGPU_COLOR_COMPONENT_G_BIT                   \
                        | NGPU_COLOR_COMPONENT_B_BIT                   \
                        | NGPU_COLOR_COMPONENT_A_BIT,                  \
    .depth_test         = 0,                                           \
    .depth_write        = 1,                                           \
    .depth_func         = NGPU_COMPARE_OP_LESS,                        \
    .stencil_test       = 0,                                           \
    .stencil_front      = {                                            \
        .write_mask = 0xff,                                            \
        .func       = NGPU_COMPARE_OP_ALWAYS,                          \
        .ref        = 0,                                               \
        .read_mask  = 0xff,                                            \
        .fail       = NGPU_STENCIL_OP_KEEP,                            \
        .depth_fail = NGPU_STENCIL_OP_KEEP,                            \
        .depth_pass = NGPU_STENCIL_OP_KEEP,                            \
    },                                                                 \
    .stencil_back = {                                                  \
         .write_mask = 0xff,                                           \
         .func       = NGPU_COMPARE_OP_ALWAYS,                         \
         .ref        = 0,                                              \
         .read_mask  = 0xff,                                           \
         .fail       = NGPU_STENCIL_OP_KEEP,                           \
         .depth_fail = NGPU_STENCIL_OP_KEEP,                           \
         .depth_pass = NGPU_STENCIL_OP_KEEP,                           \
     },                                                                \
    .cull_mode          = NGPU_CULL_MODE_NONE,                         \
    .front_face         = NGPU_FRONT_FACE_COUNTER_CLOCKWISE            \
}                                                                      \

/*
 * Buffer
 */

#define NGPU_BUFFER_WHOLE_SIZE SIZE_MAX

enum {
    NGPU_BUFFER_USAGE_DYNAMIC_BIT        = 1 << 0,
    NGPU_BUFFER_USAGE_TRANSFER_SRC_BIT   = 1 << 1,
    NGPU_BUFFER_USAGE_TRANSFER_DST_BIT   = 1 << 2,
    NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT = 1 << 3,
    NGPU_BUFFER_USAGE_STORAGE_BUFFER_BIT = 1 << 4,
    NGPU_BUFFER_USAGE_INDEX_BUFFER_BIT   = 1 << 5,
    NGPU_BUFFER_USAGE_VERTEX_BUFFER_BIT  = 1 << 6,
    NGPU_BUFFER_USAGE_MAP_READ           = 1 << 7,
    NGPU_BUFFER_USAGE_MAP_WRITE          = 1 << 8,
    NGPU_BUFFER_USAGE_MAP_PERSISTENT     = 1 << 9,
};

struct ngpu_buffer {
    struct ngli_rc rc;
    struct ngpu_ctx *gpu_ctx;
    size_t size;
    uint32_t usage;
};

NGLI_RC_CHECK_STRUCT(ngpu_buffer);

struct ngpu_buffer *ngpu_buffer_create(struct ngpu_ctx *gpu_ctx);
int ngpu_buffer_init(struct ngpu_buffer *s, size_t size, uint32_t usage);
int ngpu_buffer_wait(struct ngpu_buffer *s);
int ngpu_buffer_upload(struct ngpu_buffer *s, const void *data, size_t offset, size_t size);
int ngpu_buffer_map(struct ngpu_buffer *s, size_t offset, size_t size, void **datap);
void ngpu_buffer_unmap(struct ngpu_buffer *s);
void ngpu_buffer_freep(struct ngpu_buffer **sp);

/*
 * Texture
 */

enum ngpu_mipmap_filter {
    NGPU_MIPMAP_FILTER_NONE = 0,
    NGPU_MIPMAP_FILTER_NEAREST,
    NGPU_MIPMAP_FILTER_LINEAR,
    NGPU_NB_MIPMAP,
    NGPU_MIPMAP_MAX_ENUM = 0x7FFFFFFF
};

enum ngpu_filter {
    NGPU_FILTER_NEAREST = 0,
    NGPU_FILTER_LINEAR,
    NGPU_NB_FILTER,
    NGPU_FILTER_MAX_ENUM = 0x7FFFFFFF
};

enum ngpu_wrap {
    NGPU_WRAP_CLAMP_TO_EDGE = 0,
    NGPU_WRAP_MIRRORED_REPEAT,
    NGPU_WRAP_REPEAT,
    NGPU_NB_WRAP,
    NGPU_WRAP_MAX_ENUM = 0x7FFFFFFF
};

enum {
    NGPU_TEXTURE_USAGE_TRANSFER_SRC_BIT             = 1 << 0,
    NGPU_TEXTURE_USAGE_TRANSFER_DST_BIT             = 1 << 1,
    NGPU_TEXTURE_USAGE_SAMPLED_BIT                  = 1 << 2,
    NGPU_TEXTURE_USAGE_STORAGE_BIT                  = 1 << 3,
    NGPU_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT         = 1 << 4,
    NGPU_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT = 1 << 5,
    NGPU_TEXTURE_USAGE_TRANSIENT_ATTACHMENT_BIT     = 1 << 6,
};

enum ngpu_texture_type {
    NGPU_TEXTURE_TYPE_2D = 0,
    NGPU_TEXTURE_TYPE_2D_ARRAY,
    NGPU_TEXTURE_TYPE_3D,
    NGPU_TEXTURE_TYPE_CUBE,
    NGPU_TEXTURE_TYPE_NB,
    NGPU_TEXTURE_TYPE_MAX_ENUM = 0x7FFFFFFF
};

struct ngpu_texture_params {
    enum ngpu_texture_type type;
    enum ngpu_format format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t samples;
    enum ngpu_filter min_filter;
    enum ngpu_filter mag_filter;
    enum ngpu_mipmap_filter mipmap_filter;
    enum ngpu_wrap wrap_s;
    enum ngpu_wrap wrap_t;
    enum ngpu_wrap wrap_r;
    uint32_t usage;
};

struct ngpu_texture {
    struct ngli_rc rc;
    struct ngpu_ctx *gpu_ctx;
    struct ngpu_texture_params params;
};

struct ngpu_texture_transfer_params {
    uint32_t pixels_per_row;
    uint32_t x, y, z;
    uint32_t width, height, depth;
    uint32_t base_layer;
    uint32_t layer_count;
};

NGLI_RC_CHECK_STRUCT(ngpu_texture);

struct ngpu_texture *ngpu_texture_create(struct ngpu_ctx *gpu_ctx);
int ngpu_texture_init(struct ngpu_texture *s, const struct ngpu_texture_params *params);
int ngpu_texture_upload(struct ngpu_texture *s, const uint8_t *data, uint32_t linesize);
int ngpu_texture_upload_with_params(struct ngpu_texture *s, const uint8_t *data, const struct ngpu_texture_transfer_params *transfer_params);
int ngpu_texture_generate_mipmap(struct ngpu_texture *s);
void ngpu_texture_freep(struct ngpu_texture **sp);

/*
 * Bindgroup
 */

enum ngpu_access {
    NGPU_ACCESS_UNDEFINED  = 0,
    NGPU_ACCESS_READ_BIT   = 1 << 0,
    NGPU_ACCESS_WRITE_BIT  = 1 << 1,
    NGPU_ACCESS_READ_WRITE = NGPU_ACCESS_READ_BIT | NGPU_ACCESS_WRITE_BIT,
    NGPU_ACCESS_NB,
    NGPU_ACCESS_MAX_ENUM = 0x7FFFFFFF
};

struct ngpu_bindgroup_layout_entry {
    size_t id;
    enum ngpu_type type;
    uint32_t binding;
    enum ngpu_access access;
    uint32_t stage_flags;
    void *immutable_sampler;
};

struct ngpu_bindgroup_layout_desc {
    struct ngpu_bindgroup_layout_entry *textures;
    size_t nb_textures;
    struct ngpu_bindgroup_layout_entry *buffers;
    size_t nb_buffers;
};

struct ngpu_bindgroup_layout {
    struct ngli_rc rc;
    struct ngpu_ctx *gpu_ctx;
    struct ngpu_bindgroup_layout_entry *textures;
    size_t nb_textures;
    struct ngpu_bindgroup_layout_entry *buffers;
    size_t nb_buffers;
    size_t nb_dynamic_offsets;
};

NGLI_RC_CHECK_STRUCT(ngpu_bindgroup_layout);

struct ngpu_texture_binding {
    const struct ngpu_texture *texture;
    void *immutable_sampler;
};

struct ngpu_buffer_binding {
    const struct ngpu_buffer *buffer;
    size_t offset;
    size_t size;
};

struct ngpu_bindgroup_resources {
    struct ngpu_texture_binding *textures;
    size_t nb_textures;
    struct ngpu_buffer_binding *buffers;
    size_t nb_buffers;
};

struct ngpu_bindgroup_params {
    struct ngpu_bindgroup_layout *layout;
    struct ngpu_bindgroup_resources resources;
};

struct ngpu_bindgroup {
    struct ngli_rc rc;
    struct ngpu_ctx *gpu_ctx;
    struct ngpu_bindgroup_layout *layout;
};

NGLI_RC_CHECK_STRUCT(ngpu_bindgroup);

struct ngpu_bindgroup_layout *ngpu_bindgroup_layout_create(struct ngpu_ctx *gpu_ctx);
int ngpu_bindgroup_layout_init(struct ngpu_bindgroup_layout *s, struct ngpu_bindgroup_layout_desc *desc);
int ngpu_bindgroup_layout_is_compatible(const struct ngpu_bindgroup_layout *a, const struct ngpu_bindgroup_layout *b);
void ngpu_bindgroup_layout_freep(struct ngpu_bindgroup_layout **sp);

struct ngpu_bindgroup *ngpu_bindgroup_create(struct ngpu_ctx *gpu_ctx);
int ngpu_bindgroup_init(struct ngpu_bindgroup *s, const struct ngpu_bindgroup_params *params);
int ngpu_bindgroup_update_texture(struct ngpu_bindgroup *s, int32_t index, const struct ngpu_texture_binding *binding);
int ngpu_bindgroup_update_buffer(struct ngpu_bindgroup *s, int32_t index, const struct ngpu_buffer_binding *binding);
void ngpu_bindgroup_freep(struct ngpu_bindgroup **sp);

/*
 * Rendertarget
 */

enum ngpu_load_op {
    NGPU_LOAD_OP_LOAD,
    NGPU_LOAD_OP_CLEAR,
    NGPU_LOAD_OP_DONT_CARE,
    NGPU_LOAD_OP_MAX_ENUM = 0x7FFFFFFF
};

enum ngpu_store_op {
    NGPU_STORE_OP_STORE,
    NGPU_STORE_OP_DONT_CARE,
    NGPU_STORE_OP_MAX_ENUM = 0x7FFFFFFF
};

struct ngpu_rendertarget_layout_entry {
    enum ngpu_format format;
    int resolve;
};

struct ngpu_rendertarget_layout {
    uint32_t samples;
    size_t nb_colors;
    struct ngpu_rendertarget_layout_entry colors[NGPU_MAX_COLOR_ATTACHMENTS];
    struct ngpu_rendertarget_layout_entry depth_stencil;
};

struct ngpu_attachment {
    struct ngpu_texture *attachment;
    uint32_t attachment_layer;
    struct ngpu_texture *resolve_target;
    uint32_t resolve_target_layer;
    enum ngpu_load_op load_op;
    float clear_value[4];
    enum ngpu_store_op store_op;
};

struct ngpu_rendertarget_params {
    uint32_t width;
    uint32_t height;
    size_t nb_colors;
    struct ngpu_attachment colors[NGPU_MAX_COLOR_ATTACHMENTS];
    struct ngpu_attachment depth_stencil;
};

struct ngpu_rendertarget {
    struct ngli_rc rc;
    struct ngpu_ctx *gpu_ctx;
    struct ngpu_rendertarget_params params;
    uint32_t width;
    uint32_t height;
    struct ngpu_rendertarget_layout layout;
};

NGLI_RC_CHECK_STRUCT(ngpu_rendertarget);

struct ngpu_rendertarget *ngpu_rendertarget_create(struct ngpu_ctx *gpu_ctx);
int ngpu_rendertarget_init(struct ngpu_rendertarget *s, const struct ngpu_rendertarget_params *params);
void ngpu_rendertarget_freep(struct ngpu_rendertarget **sp);

/*
 * Program
 */

#define MAX_ID_LEN 128

enum ngpu_program_stage {
    NGPU_PROGRAM_STAGE_VERT,
    NGPU_PROGRAM_STAGE_FRAG,
    NGPU_PROGRAM_STAGE_COMP,
    NGPU_PROGRAM_STAGE_NB,
    NGPU_PROGRAM_STAGE_MAX_ENUM = 0x7FFFFFFF
};

enum {
    NGPU_PROGRAM_STAGE_VERTEX_BIT   = 1U << NGPU_PROGRAM_STAGE_VERT,
    NGPU_PROGRAM_STAGE_FRAGMENT_BIT = 1U << NGPU_PROGRAM_STAGE_FRAG,
    NGPU_PROGRAM_STAGE_COMPUTE_BIT  = 1U << NGPU_PROGRAM_STAGE_COMP,
};

struct ngpu_program_params {
    const char *label;
    const char *vertex;
    const char *fragment;
    const char *compute;
};

struct ngpu_program {
    struct ngpu_ctx *gpu_ctx;
};

struct ngpu_program *ngpu_program_create(struct ngpu_ctx *gpu_ctx);
int ngpu_program_init(struct ngpu_program *s, const struct ngpu_program_params *params);
void ngpu_program_freep(struct ngpu_program **sp);

/*
 * Pipeline
 */

struct ngpu_vertex_attribute {
    size_t id;
    uint32_t location;
    enum ngpu_format format;
    size_t offset;
};

struct ngpu_vertex_buffer_layout {
    struct ngpu_vertex_attribute attributes[NGPU_MAX_ATTRIBUTES_PER_BUFFER];
    size_t nb_attributes;
    uint32_t rate;
    size_t stride;
};

struct ngpu_vertex_state {
    struct ngpu_vertex_buffer_layout *buffers;
    size_t nb_buffers;
};

struct ngpu_vertex_resources {
    struct ngpu_buffer **vertex_buffers;
    size_t nb_vertex_buffers;
};

enum ngpu_primitive_topology {
    NGPU_PRIMITIVE_TOPOLOGY_POINT_LIST,
    NGPU_PRIMITIVE_TOPOLOGY_LINE_LIST,
    NGPU_PRIMITIVE_TOPOLOGY_LINE_STRIP,
    NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    NGPU_PRIMITIVE_TOPOLOGY_NB,
    NGPU_PRIMITIVE_TOPOLOGY_MAX_ENUM = 0x7FFFFFFF
};

struct ngpu_pipeline_graphics {
    enum ngpu_primitive_topology topology;
    struct ngpu_graphics_state state;
    struct ngpu_rendertarget_layout rt_layout;
    struct ngpu_vertex_state vertex_state;
};

enum ngpu_pipeline_type {
    NGPU_PIPELINE_TYPE_GRAPHICS,
    NGPU_PIPELINE_TYPE_COMPUTE,
    NGPU_PIPELINE_TYPE_MAX_ENUM = 0x7FFFFFFF
};

struct ngpu_pipeline_layout {
    const struct ngpu_bindgroup_layout *bindgroup_layout;
};

struct ngpu_pipeline_params {
    enum ngpu_pipeline_type type;
    const struct ngpu_pipeline_graphics graphics;
    const struct ngpu_program *program;
    const struct ngpu_pipeline_layout layout;
};

struct ngpu_pipeline {
    struct ngli_rc rc;
    struct ngpu_ctx *gpu_ctx;

    enum ngpu_pipeline_type type;
    struct ngpu_pipeline_graphics graphics;
    const struct ngpu_program *program;
    struct ngpu_pipeline_layout layout;
};

NGLI_RC_CHECK_STRUCT(ngpu_pipeline);

int ngpu_pipeline_graphics_copy(struct ngpu_pipeline_graphics *dst, const struct ngpu_pipeline_graphics *src);
void ngpu_pipeline_graphics_reset(struct ngpu_pipeline_graphics *graphics);

struct ngpu_pipeline *ngpu_pipeline_create(struct ngpu_ctx *gpu_ctx);
int ngpu_pipeline_init(struct ngpu_pipeline *s, const struct ngpu_pipeline_params *params);
void ngpu_pipeline_freep(struct ngpu_pipeline **sp);

/*
 * Block descriptor
 */

enum ngpu_block_layout {
    NGPU_BLOCK_LAYOUT_UNKNOWN,
    NGPU_BLOCK_LAYOUT_STD140,
    NGPU_BLOCK_LAYOUT_STD430,
    NGPU_BLOCK_NB_LAYOUTS,
    NGPU_BLOCK_LAYOUT_MAX_ENUM = 0x7FFFFFFF
};

struct ngpu_block_field {
    char name[MAX_ID_LEN];
    enum ngpu_type type;
    size_t count;
    size_t offset;
    size_t size;
    size_t stride;
    enum ngpu_precision precision;
};

void ngpu_block_field_copy(const struct ngpu_block_field *fi, uint8_t *dst, const uint8_t *src);
void ngpu_block_field_copy_count(const struct ngpu_block_field *fi, uint8_t *dst, const uint8_t *src, size_t count);

struct ngpu_block_desc {
    struct ngpu_ctx *gpu_ctx;
    enum ngpu_block_layout layout;
    struct darray fields; // block_field
    size_t size;
};

#define NGPU_BLOCK_DESC_VARIADIC_COUNT SIZE_MAX

void ngpu_block_desc_init(struct ngpu_ctx *gpu_ctx, struct ngpu_block_desc *s, enum ngpu_block_layout layout);
size_t ngpu_block_desc_get_size(const struct ngpu_block_desc *s, size_t variadic_count);

/*
 * Get the block size aligned to the minimum offset alignment required by the
 * GPU. This function is useful when one wants to pack multiple blocks into the
 * same buffer as it ensures that the next block offset into the buffer will
 * honor the GPU offset alignment constraints.
 */
size_t ngpu_block_desc_get_aligned_size(const struct ngpu_block_desc *s, size_t variadic_count);

int ngpu_block_desc_add_field(struct ngpu_block_desc *s, const char *name, enum ngpu_type type, size_t count);
int ngpu_block_desc_add_fields(struct ngpu_block_desc *s, const struct ngpu_block_field *fields, size_t count);

void ngpu_block_desc_reset(struct ngpu_block_desc *s);

/*
 * Block
 */

#define NGPU_BLOCK_FIELD(_st, _name, _type, _count) \
    {.field={.name= #_name, .type=_type, .count=_count}, .offset=offsetof(_st, _name)}

struct ngpu_block_entry {
    struct ngpu_block_field field;
    size_t offset;
};

struct ngpu_block_params {
    enum ngpu_block_layout layout;
    uint32_t usage;
    size_t count;
    const struct ngpu_block_entry *entries;
    size_t nb_entries;
};

struct ngpu_block {
    struct ngpu_ctx *gpu_ctx;
    struct ngpu_block_desc block_desc;
    size_t block_size;
    struct darray offsets; // array of size_t
    struct ngpu_buffer *buffer;
};

int ngpu_block_init(struct ngpu_ctx *gpu_ctx, struct ngpu_block *s, const struct ngpu_block_params *params);
int ngpu_block_update(struct ngpu_block *s, size_t index, const void *data);
void ngpu_block_reset(struct ngpu_block *s);

/*
 * Context
 */

enum ngpu_platform_type {
    NGPU_PLATFORM_XLIB,
    NGPU_PLATFORM_WAYLAND,
    NGPU_PLATFORM_ANDROID,
    NGPU_PLATFORM_MACOS,
    NGPU_PLATFORM_IOS,
    NGPU_PLATFORM_WINDOWS,
    NGPU_PLATFORM_MAX_ENUM = 0x7FFFFFFF
};

enum ngpu_backend_type {
    NGPU_BACKEND_OPENGL,
    NGPU_BACKEND_OPENGLES,
    NGPU_BACKEND_VULKAN,
    NGPU_BACKEND_MAX_ENUM = 0x7FFFFFFF
};

enum ngpu_capture_buffer_type {
    NGPU_CAPTURE_BUFFER_TYPE_CPU,
    NGPU_CAPTURE_BUFFER_TYPE_COREVIDEO,
    NGPU_CAPTURE_BUFFER_TYPE_MAX_ENUM = 0x7FFFFFFF
};

int ngpu_get_available_backends(size_t *backend_count, enum ngpu_backend_type *backends);
const char *ngpu_backend_get_string_id(enum ngpu_backend_type backend);
const char *ngpu_backend_get_full_name(enum ngpu_backend_type backend);

struct ngpu_viewport {
    float x, y, width, height;
};

struct ngpu_scissor {
    uint32_t x, y, width, height;
};

int ngpu_viewport_is_valid(const struct ngpu_viewport *viewport);

struct ngpu_ctx_params {
    enum ngpu_platform_type platform;  /* Platform-specific identifier */

    enum ngpu_backend_type backend;   /* Rendering backend */

    void *backend_params; /* Optional backend specific configuration (any of ngpu_ctx_params_*
                             depending on the selected backend) */

    uintptr_t display; /* A native display handle */

    uintptr_t window;  /* A native window handle */

    int swap_interval; /* Specifies the minimum number of video frames that are
                          displayed before a buffer swap will occur. -1 can be
                          used to use the default system implementation value.
                          This option is only honored on Linux, macOS, and
                          Android (iOS does not provide swap interval control).
                          */

    int offscreen; /* Whether the rendering should happen offscreen or not.
                      This field is ignored if the context is external. */

    uint32_t width; /* Graphics context width, mandatory for offscreen rendering */

    uint32_t height; /* Graphics context height, mandatory for offscreen rendering */

    uint32_t samples;     /* Number of samples used for multisample anti-aliasing */

    int set_surface_pts; /* Whether pts should be set to the surface or not (Android only).
                            Unsupported with offscreen rendering. */

    float clear_color[4]; /* Clear color (red, green, blue, alpha) */

    void *capture_buffer; /* An optional pointer to a capture buffer.
                             - If the capture buffer type is CPU, the user
                               allocated size of the specified buffer must be of
                               at least width * height * 4 bytes (RGBA)
                             - If the capture buffer type is COREVIDEO, the
                               specified pointer must reference a CVPixelBuffer */

    enum ngpu_capture_buffer_type capture_buffer_type;

    int debug; /* Enable graphics context debugging */

    int timer_queries; /* Enable graphics context timer queries */
};

int ngpu_ctx_params_copy(struct ngpu_ctx_params *dst, const struct ngpu_ctx_params *src);
void ngpu_ctx_params_reset(struct ngpu_ctx_params *params);

#define NGPU_FEATURE_SOFTWARE                          (1U << 0)
#define NGPU_FEATURE_COMPUTE                           (1U << 1)
#define NGPU_FEATURE_IMAGE_LOAD_STORE                  (1U << 2)
#define NGPU_FEATURE_STORAGE_BUFFER                    (1U << 3)
#define NGPU_FEATURE_BUFFER_MAP_PERSISTENT             (1U << 4)
#define NGPU_FEATURE_DEPTH_STENCIL_RESOLVE             (1U << 5)

struct ngpu_ctx {
    struct ngpu_ctx_params params;
    const struct ngpu_ctx_class *cls;

    int version;
    int language_version;
    uint64_t features;
    struct ngpu_limits limits;

    uint32_t nb_in_flight_frames;
    uint32_t current_frame_index;

    struct ngpu_pgcache *program_cache;

    struct ngpu_capture_ctx *gpu_capture_ctx;
    int gpu_capture;

    /* State */
    struct ngpu_rendertarget *rendertarget;
    struct ngpu_pipeline *pipeline;
    struct ngpu_bindgroup *bindgroup;
    uint32_t dynamic_offsets[NGPU_MAX_DYNAMIC_OFFSETS];
    size_t nb_dynamic_offsets;
    const struct ngpu_buffer *vertex_buffers[NGPU_MAX_VERTEX_BUFFERS];
    const struct ngpu_buffer *index_buffer;
    enum ngpu_format index_format;
};

struct ngpu_ctx *ngpu_ctx_create(const struct ngpu_ctx_params *params);
int ngpu_ctx_init(struct ngpu_ctx *s);
int ngpu_ctx_resize(struct ngpu_ctx *s, uint32_t width, uint32_t height);
int ngpu_ctx_set_capture_buffer(struct ngpu_ctx *s, void *capture_buffer);
uint32_t ngpu_ctx_advance_frame(struct ngpu_ctx *s);
uint32_t ngpu_ctx_get_current_frame_index(struct ngpu_ctx *s);
uint32_t ngpu_ctx_get_nb_in_flight_frames(struct ngpu_ctx *s);
int ngpu_ctx_begin_update(struct ngpu_ctx *s);
int ngpu_ctx_end_update(struct ngpu_ctx *s);
int ngpu_ctx_begin_draw(struct ngpu_ctx *s);
int ngpu_ctx_end_draw(struct ngpu_ctx *s, double t);
int ngpu_ctx_query_draw_time(struct ngpu_ctx *s, int64_t *time);
void ngpu_ctx_wait_idle(struct ngpu_ctx *s);
void ngpu_ctx_freep(struct ngpu_ctx **sp);

enum ngpu_cull_mode ngpu_ctx_transform_cull_mode(struct ngpu_ctx *s, enum ngpu_cull_mode cull_mode);
void ngpu_ctx_transform_projection_matrix(struct ngpu_ctx *s, float *dst);
void ngpu_ctx_get_rendertarget_uvcoord_matrix(struct ngpu_ctx *s, float *dst);

struct ngpu_rendertarget *ngpu_ctx_get_default_rendertarget(struct ngpu_ctx *s, enum ngpu_load_op load_op);
const struct ngpu_rendertarget_layout *ngpu_ctx_get_default_rendertarget_layout(struct ngpu_ctx *s);
void ngpu_ctx_get_default_rendertarget_size(struct ngpu_ctx *s, uint32_t *width, uint32_t *height);

void ngpu_ctx_begin_render_pass(struct ngpu_ctx *s, struct ngpu_rendertarget *rt);
void ngpu_ctx_end_render_pass(struct ngpu_ctx *s);
bool ngpu_ctx_is_render_pass_active(const struct ngpu_ctx *s);

void ngpu_ctx_set_viewport(struct ngpu_ctx *s, const struct ngpu_viewport *viewport);
void ngpu_ctx_set_scissor(struct ngpu_ctx *s, const struct ngpu_scissor *scissor);

enum ngpu_format ngpu_ctx_get_preferred_depth_format(struct ngpu_ctx *s);
enum ngpu_format ngpu_ctx_get_preferred_depth_stencil_format(struct ngpu_ctx *s);
uint32_t ngpu_ctx_get_format_features(struct ngpu_ctx *s, enum ngpu_format format);

void ngpu_ctx_generate_texture_mipmap(struct ngpu_ctx *s, struct ngpu_texture *texture);

void ngpu_ctx_set_pipeline(struct ngpu_ctx *s, struct ngpu_pipeline *pipeline);
void ngpu_ctx_set_bindgroup(struct ngpu_ctx *s, struct ngpu_bindgroup *bindgroup, const uint32_t *offsets, size_t nb_offsets);
void ngpu_ctx_draw(struct ngpu_ctx *s, uint32_t nb_vertices, uint32_t nb_instances, uint32_t first_vertex);
void ngpu_ctx_draw_indexed(struct ngpu_ctx *s, uint32_t nb_indices, uint32_t nb_instances);
void ngpu_ctx_dispatch(struct ngpu_ctx *s, uint32_t nb_group_x, uint32_t nb_group_y, uint32_t nb_group_z);

void ngpu_ctx_set_vertex_buffer(struct ngpu_ctx *s, uint32_t index, const struct ngpu_buffer *buffer);
void ngpu_ctx_set_index_buffer(struct ngpu_ctx *s, const struct ngpu_buffer *buffer, enum ngpu_format format);

#endif
