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

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#if defined(_WIN32)
#  if defined(BUILD_NGPU_SHARED_LIB)
#    define NGPU_API __declspec(dllexport) /* On Windows, we need to export the symbols while building */
#  elif defined(USE_NGPU_STATIC_LIB) /* Static library users don't need anything special */
#    define NGPU_API
#  else
#    define NGPU_API __declspec(dllimport) /* Dynamic library users (default) need a DLL import spec */
#  endif
#else
#  define NGPU_API __attribute__((visibility("default"))) /* This cancel the hidden GNU symbol visibility attribute */
#endif
/*
 * Forward declarations
 */

struct ngpu_ctx;

/*
 * Errors
 */
#define NGPU_FOURCC(a, b, c, d) \
   (((uint32_t)((uint8_t)(a)) << 24) | \
    ((uint32_t)((uint8_t)(b)) << 16) | \
    ((uint32_t)((uint8_t)(c)) << 8)  | \
    ((uint32_t)((uint8_t)(d)) << 0))   \

#define NGPU_ERROR(a,b,c,d) (-(int32_t)NGPU_FOURCC(a,b,c,d))

#define NGPU_SUCCESS                        0
#define NGPU_ERROR_GENERIC                  -1                             /* Generic error */
#define NGPU_ERROR_ACCESS                   NGPU_ERROR('E','a','c','c')    /* Operation not allowed */
#define NGPU_ERROR_BUG                      NGPU_ERROR('E','b','u','g')    /* A buggy code path was triggered, please report if it happens */
#define NGPU_ERROR_EXTERNAL                 NGPU_ERROR('E','e','x','t')    /* An error occurred in an external dependency */
#define NGPU_ERROR_INVALID_ARG              NGPU_ERROR('E','a','r','g')    /* Invalid user argument specified */
#define NGPU_ERROR_INVALID_DATA             NGPU_ERROR('E','d','a','t')    /* Invalid input data */
#define NGPU_ERROR_INVALID_USAGE            NGPU_ERROR('E','u','s','g')    /* Invalid public API usage */
#define NGPU_ERROR_IO                       NGPU_ERROR('E','i','o',' ')    /* Input/Output error */
#define NGPU_ERROR_LIMIT_EXCEEDED           NGPU_ERROR('E','l','i','m')    /* Hardware or resource limit exceeded */
#define NGPU_ERROR_MEMORY                   NGPU_ERROR('E','m','e','m')    /* Memory/allocation error */
#define NGPU_ERROR_NOT_FOUND                NGPU_ERROR('E','f','n','d')    /* Target not found */
#define NGPU_ERROR_UNSUPPORTED              NGPU_ERROR('E','s','u','p')    /* Unsupported operation */

#define NGPU_ERROR_GRAPHICS_GENERIC         NGPU_ERROR('G','g','e','n')    /* Generic graphics error */
#define NGPU_ERROR_GRAPHICS_LIMIT_EXCEEDED  NGPU_ERROR('G','l','i','m')    /* Graphics hardware or resource limit exceeded */
#define NGPU_ERROR_GRAPHICS_MEMORY          NGPU_ERROR('G','m','e','m')    /* Graphics memory/allocation error */
#define NGPU_ERROR_GRAPHICS_UNSUPPORTED     NGPU_ERROR('G','s','u','p')    /* Unsupported graphics operation */

/*
 * Logs
 */

enum ngpu_log_level {
    NGPU_LOG_VERBOSE,
    NGPU_LOG_DEBUG,
    NGPU_LOG_INFO,
    NGPU_LOG_WARNING,
    NGPU_LOG_ERROR,
    NGPU_LOG_QUIET = 1 << 8,
    NGPU_LOG_MAX_ENUM = 0x7FFFFFFF
};

typedef void (*ngpu_log_callback_type)(void *arg, enum ngpu_log_level level, const char *filename,
                                       int ln, const char *fn, const char *fmt, va_list vl);

NGPU_API void ngpu_log_set_callback(void *arg, ngpu_log_callback_type callback);
NGPU_API void ngpu_log_set_min_level(enum ngpu_log_level level);

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

NGPU_API const char *ngpu_type_get_name(enum ngpu_type type);

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

NGPU_API size_t ngpu_format_get_bytes_per_pixel(enum ngpu_format format);
NGPU_API size_t ngpu_format_get_nb_comp(enum ngpu_format format);
NGPU_API int ngpu_format_has_depth(enum ngpu_format format);
NGPU_API int ngpu_format_has_stencil(enum ngpu_format format);

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

struct ngpu_buffer;

NGPU_API struct ngpu_buffer *ngpu_buffer_create(struct ngpu_ctx *gpu_ctx);
NGPU_API int ngpu_buffer_init(struct ngpu_buffer *s, size_t size, uint32_t usage);
NGPU_API int ngpu_buffer_wait(struct ngpu_buffer *s);
NGPU_API int ngpu_buffer_upload(struct ngpu_buffer *s, const void *data, size_t offset, size_t size);
NGPU_API int ngpu_buffer_map(struct ngpu_buffer *s, size_t offset, size_t size, void **datap);
NGPU_API void ngpu_buffer_unmap(struct ngpu_buffer *s);
NGPU_API void ngpu_buffer_freep(struct ngpu_buffer **sp);

NGPU_API size_t ngpu_buffer_get_size(const struct ngpu_buffer *s);
NGPU_API uint32_t ngpu_buffer_get_usage(const struct ngpu_buffer *s);

/*
 * Import
 */

enum ngpu_import_type {
    NGPU_IMPORT_TYPE_NONE,
    NGPU_IMPORT_TYPE_DMA_BUF,
    NGPU_IMPORT_TYPE_AHARDWARE_BUFFER,
    NGPU_IMPORT_TYPE_IOSURFACE,
    NGPU_IMPORT_TYPE_COREVIDEO_BUFFER,
    NGPU_IMPORT_TYPE_METAL_TEXTURE,
    NGPU_IMPORT_TYPE_OPENGL_TEXTURE,
};

struct ngpu_import_dma_buf_params {
    int fd;
    size_t size;
    size_t offset;
    size_t pitch;
    uint32_t drm_format;
    uint64_t drm_format_mod;
};

struct ngpu_import_ahardware_buffer_params {
    void *hardware_buffer; // AHardwareBuffer
    void *ycbcr_sampler; // YCbCrSampler (Vulkan only)
};

struct ngpu_import_iosurface_params {
    void *iosurface; // IOSurfaceRef
    uint32_t plane;
};

struct ngpu_import_corevideo_buffer_params {
    void *corevideo_buffer; // CVPixelBufferRef
    uint32_t plane;
};

struct ngpu_import_metal_texture_params {
    void *metal_texture; // MTLTexture
};

struct ngpu_import_opengl_texture_params {
    uint32_t texture; // OpenGL texture
    uint32_t target; // OpenGL texture target
};

struct ngpu_import_params {
    enum ngpu_import_type type;
    union {
        struct ngpu_import_dma_buf_params dma_buf;
        struct ngpu_import_ahardware_buffer_params ahardware_buffer;
        struct ngpu_import_iosurface_params iosurface;
        struct ngpu_import_corevideo_buffer_params corevideo_buffer;
        struct ngpu_import_metal_texture_params metal_texture;
        struct ngpu_import_opengl_texture_params opengl_texture;
    };
};

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
    struct ngpu_import_params import_params;
};

struct ngpu_texture_transfer_params {
    uint32_t pixels_per_row;
    uint32_t x, y, z;
    uint32_t width, height, depth;
    uint32_t base_layer;
    uint32_t layer_count;
};

struct ngpu_texture;

NGPU_API struct ngpu_texture *ngpu_texture_create(struct ngpu_ctx *gpu_ctx);
NGPU_API int ngpu_texture_init(struct ngpu_texture *s, const struct ngpu_texture_params *params);
NGPU_API int ngpu_texture_upload(struct ngpu_texture *s, const uint8_t *data, uint32_t linesize);
NGPU_API int ngpu_texture_upload_with_params(struct ngpu_texture *s, const uint8_t *data, const struct ngpu_texture_transfer_params *transfer_params);
NGPU_API int ngpu_texture_generate_mipmap(struct ngpu_texture *s);
NGPU_API void ngpu_texture_freep(struct ngpu_texture **sp);

NGPU_API const struct ngpu_texture_params *ngpu_texture_get_params(const struct ngpu_texture *s);

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

struct ngpu_bindgroup_layout;

NGPU_API struct ngpu_bindgroup_layout *ngpu_bindgroup_layout_create(struct ngpu_ctx *gpu_ctx);
NGPU_API int ngpu_bindgroup_layout_init(struct ngpu_bindgroup_layout *s, struct ngpu_bindgroup_layout_desc *desc);
NGPU_API int ngpu_bindgroup_layout_is_compatible(const struct ngpu_bindgroup_layout *a, const struct ngpu_bindgroup_layout *b);
NGPU_API size_t ngpu_bindgroup_layout_get_nb_dynamic_offsets(const struct ngpu_bindgroup_layout *s);
NGPU_API void ngpu_bindgroup_layout_freep(struct ngpu_bindgroup_layout **sp);

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

struct ngpu_bindgroup;

NGPU_API struct ngpu_bindgroup *ngpu_bindgroup_create(struct ngpu_ctx *gpu_ctx);
NGPU_API int ngpu_bindgroup_init(struct ngpu_bindgroup *s, const struct ngpu_bindgroup_params *params);
NGPU_API int ngpu_bindgroup_update_texture(struct ngpu_bindgroup *s, int32_t index, const struct ngpu_texture_binding *binding);
NGPU_API int ngpu_bindgroup_update_buffer(struct ngpu_bindgroup *s, int32_t index, const struct ngpu_buffer_binding *binding);
NGPU_API void ngpu_bindgroup_freep(struct ngpu_bindgroup **sp);

NGPU_API size_t ngpu_bindgroup_get_refcount(const struct ngpu_bindgroup *s);

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

struct ngpu_rendertarget;

NGPU_API struct ngpu_rendertarget *ngpu_rendertarget_create(struct ngpu_ctx *gpu_ctx);
NGPU_API int ngpu_rendertarget_init(struct ngpu_rendertarget *s, const struct ngpu_rendertarget_params *params);
NGPU_API void ngpu_rendertarget_freep(struct ngpu_rendertarget **sp);

NGPU_API const struct ngpu_rendertarget_layout *ngpu_rendertarget_get_layout(const struct ngpu_rendertarget *s);
NGPU_API uint32_t ngpu_rendertarget_get_width(const struct ngpu_rendertarget *s);
NGPU_API uint32_t ngpu_rendertarget_get_height(const struct ngpu_rendertarget *s);

/*
 * Program
 */

#define NGPU_ID_LEN 128

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

struct ngpu_program;

NGPU_API struct ngpu_program *ngpu_program_create(struct ngpu_ctx *gpu_ctx);
NGPU_API int ngpu_program_init(struct ngpu_program *s, const struct ngpu_program_params *params);
NGPU_API void ngpu_program_freep(struct ngpu_program **sp);

NGPU_API struct ngpu_ctx *ngpu_program_get_ctx(const struct ngpu_program *s);

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

struct ngpu_pipeline;

NGPU_API int ngpu_pipeline_graphics_copy(struct ngpu_pipeline_graphics *dst, const struct ngpu_pipeline_graphics *src);
NGPU_API void ngpu_pipeline_graphics_reset(struct ngpu_pipeline_graphics *graphics);

NGPU_API struct ngpu_pipeline *ngpu_pipeline_create(struct ngpu_ctx *gpu_ctx);
NGPU_API int ngpu_pipeline_init(struct ngpu_pipeline *s, const struct ngpu_pipeline_params *params);
NGPU_API void ngpu_pipeline_freep(struct ngpu_pipeline **sp);

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
    char name[NGPU_ID_LEN];
    enum ngpu_type type;
    size_t count;
    size_t offset;
    size_t size;
    size_t stride;
    enum ngpu_precision precision;
};

NGPU_API void ngpu_block_field_copy(const struct ngpu_block_field *fi, uint8_t *dst, const uint8_t *src);
NGPU_API void ngpu_block_field_copy_count(const struct ngpu_block_field *fi, uint8_t *dst, const uint8_t *src, size_t count);

struct ngpu_block_desc {
    struct ngpu_ctx *gpu_ctx;
    enum ngpu_block_layout layout;
    struct ngpu_block_field *fields;
    size_t nb_fields;
    size_t size;
};

#define NGPU_BLOCK_DESC_VARIADIC_COUNT SIZE_MAX

NGPU_API void ngpu_block_desc_init(struct ngpu_ctx *gpu_ctx, struct ngpu_block_desc *s, enum ngpu_block_layout layout);
NGPU_API size_t ngpu_block_desc_get_size(const struct ngpu_block_desc *s, size_t variadic_count);

/*
 * Get the block size aligned to the minimum offset alignment required by the
 * GPU. This function is useful when one wants to pack multiple blocks into the
 * same buffer as it ensures that the next block offset into the buffer will
 * honor the GPU offset alignment constraints.
 */
NGPU_API size_t ngpu_block_desc_get_aligned_size(const struct ngpu_block_desc *s, size_t variadic_count);

NGPU_API int ngpu_block_desc_add_field(struct ngpu_block_desc *s, const char *name, enum ngpu_type type, size_t count);
NGPU_API int ngpu_block_desc_add_fields(struct ngpu_block_desc *s, const struct ngpu_block_field *fields, size_t count);

NGPU_API void ngpu_block_desc_reset(struct ngpu_block_desc *s);

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
    size_t *offsets;
    size_t nb_offsets;
    struct ngpu_buffer *buffer;
};

NGPU_API int ngpu_block_init(struct ngpu_ctx *gpu_ctx, struct ngpu_block *s, const struct ngpu_block_params *params);
NGPU_API int ngpu_block_update(struct ngpu_block *s, size_t index, const void *data);
NGPU_API void ngpu_block_reset(struct ngpu_block *s);

NGPU_API void ngpu_block_get_buffer_offsets(const struct ngpu_block *s, size_t *nb_offsets, size_t **offsets);
NGPU_API struct ngpu_buffer *ngpu_block_get_buffer(const struct ngpu_block *s);

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

NGPU_API int ngpu_get_available_backends(size_t  *backend_count, enum ngpu_backend_type *backernds);
NGPU_API const char *ngpu_backend_get_string_id(enum ngpu_backend_type backend);
NGPU_API const char *ngpu_backend_get_full_name(enum ngpu_backend_type backend);

struct ngpu_viewport {
    float x, y, width, height;
};

struct ngpu_scissor {
    uint32_t x, y, width, height;
};

NGPU_API int ngpu_viewport_is_valid(const struct ngpu_viewport *viewport);

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

    ngpu_log_callback_type log_callback;
};

NGPU_API int ngpu_ctx_params_copy(struct ngpu_ctx_params *dst, const struct ngpu_ctx_params *src);
NGPU_API void ngpu_ctx_params_reset(struct ngpu_ctx_params *params);

enum {
    NGPU_FEATURE_SOFTWARE_BIT                          = 1U << 0,
    NGPU_FEATURE_COMPUTE_BIT                           = 1U << 1,
    NGPU_FEATURE_BUFFER_MAP_PERSISTENT_BIT             = 1U << 2,
    NGPU_FEATURE_DEPTH_STENCIL_RESOLVE_BIT             = 1U << 3,
    NGPU_FEATURE_IMPORT_DMA_BUF_BIT                    = 1U << 4,
    NGPU_FEATURE_IMPORT_AHARDWARE_BUFFER_BIT           = 1U << 5,
    NGPU_FEATURE_IMPORT_IOSURFACE_BIT                  = 1U << 6,
    NGPU_FEATURE_IMPORT_COREVIDEO_BUFFER_BIT           = 1U << 7,
    NGPU_FEATURE_IMPORT_METAL_TEXTURE_BIT              = 1U << 8,
    NGPU_FEATURE_IMPORT_OPENGL_TEXTURE_BIT             = 1U << 9,
    NGPU_FEATURE_MAX_ENUM                              = 0x7FFFFFFF,
};

NGPU_API struct ngpu_ctx *ngpu_ctx_create(const struct ngpu_ctx_params *params);
NGPU_API int ngpu_ctx_init(struct ngpu_ctx *s);
NGPU_API int ngpu_ctx_resize(struct ngpu_ctx *s, uint32_t width, uint32_t height);
NGPU_API int ngpu_ctx_set_capture_buffer(struct ngpu_ctx *s, void *capture_buffer);
NGPU_API uint32_t ngpu_ctx_advance_frame(struct ngpu_ctx *s);
NGPU_API uint32_t ngpu_ctx_get_current_frame_index(struct ngpu_ctx *s);
NGPU_API uint32_t ngpu_ctx_get_nb_in_flight_frames(struct ngpu_ctx *s);
NGPU_API int ngpu_ctx_begin_update(struct ngpu_ctx *s);
NGPU_API int ngpu_ctx_end_update(struct ngpu_ctx *s);
NGPU_API int ngpu_ctx_begin_draw(struct ngpu_ctx *s);
NGPU_API int ngpu_ctx_end_draw(struct ngpu_ctx *s, double t);
NGPU_API int ngpu_ctx_query_draw_time(struct ngpu_ctx *s, int64_t *time);
NGPU_API void ngpu_ctx_wait_idle(struct ngpu_ctx *s);
NGPU_API void ngpu_ctx_freep(struct ngpu_ctx **sp);

NGPU_API enum ngpu_backend_type ngpu_ctx_get_backend_type(const struct ngpu_ctx *s);
NGPU_API enum ngpu_platform_type ngpu_ctx_get_platform_type(const struct ngpu_ctx *s);
NGPU_API void *ngpu_ctx_get_display(const struct ngpu_ctx *s);
NGPU_API void *ngpu_ctx_get_window(const struct ngpu_ctx *s);
NGPU_API int ngpu_ctx_get_version(const struct ngpu_ctx *s);
NGPU_API int ngpu_ctx_get_language_version(const struct ngpu_ctx *s);
NGPU_API uint64_t ngpu_ctx_get_features(const struct ngpu_ctx *s);
NGPU_API const struct ngpu_limits *ngpu_ctx_get_limits(const struct ngpu_ctx *s);

NGPU_API enum ngpu_cull_mode ngpu_ctx_get_cull_mode(struct ngpu_ctx *s, enum ngpu_cull_mode cull_mode);
NGPU_API void ngpu_ctx_get_projection_matrix(struct ngpu_ctx *s, float *dst);
NGPU_API void ngpu_ctx_get_rendertarget_uvcoord_matrix(struct ngpu_ctx *s, float *dst);

NGPU_API struct ngpu_rendertarget *ngpu_ctx_get_default_rendertarget(struct ngpu_ctx *s, enum ngpu_load_op load_op);
NGPU_API const struct ngpu_rendertarget_layout *ngpu_ctx_get_default_rendertarget_layout(struct ngpu_ctx *s);
NGPU_API void ngpu_ctx_get_default_rendertarget_size(struct ngpu_ctx *s, uint32_t *width, uint32_t *height);

NGPU_API void ngpu_ctx_begin_render_pass(struct ngpu_ctx *s, struct ngpu_rendertarget *rt);
NGPU_API void ngpu_ctx_end_render_pass(struct ngpu_ctx *s);
NGPU_API bool ngpu_ctx_is_render_pass_active(const struct ngpu_ctx *s);

NGPU_API void ngpu_ctx_set_viewport(struct ngpu_ctx *s, const struct ngpu_viewport *viewport);
NGPU_API void ngpu_ctx_set_scissor(struct ngpu_ctx *s, const struct ngpu_scissor *scissor);

NGPU_API enum ngpu_format ngpu_ctx_get_preferred_depth_format(struct ngpu_ctx *s);
NGPU_API enum ngpu_format ngpu_ctx_get_preferred_depth_stencil_format(struct ngpu_ctx *s);
NGPU_API uint32_t ngpu_ctx_get_format_features(struct ngpu_ctx *s, enum ngpu_format format);

NGPU_API void ngpu_ctx_generate_texture_mipmap(struct ngpu_ctx *s, struct ngpu_texture *texture);

NGPU_API void ngpu_ctx_set_pipeline(struct ngpu_ctx *s, struct ngpu_pipeline *pipeline);
NGPU_API void ngpu_ctx_set_bindgroup(struct ngpu_ctx *s, struct ngpu_bindgroup *bindgroup, const uint32_t *offsets, size_t nb_offsets);
NGPU_API void ngpu_ctx_draw(struct ngpu_ctx *s, uint32_t nb_vertices, uint32_t nb_instances, uint32_t first_vertex);
NGPU_API void ngpu_ctx_draw_indexed(struct ngpu_ctx *s, uint32_t nb_indices, uint32_t nb_instances);
NGPU_API void ngpu_ctx_dispatch(struct ngpu_ctx *s, uint32_t nb_group_x, uint32_t nb_group_y, uint32_t nb_group_z);

NGPU_API void ngpu_ctx_set_vertex_buffer(struct ngpu_ctx *s, uint32_t index, const struct ngpu_buffer *buffer);
NGPU_API void ngpu_ctx_set_index_buffer(struct ngpu_ctx *s, const struct ngpu_buffer *buffer, enum ngpu_format format);


/*
 * Pgcraft
 */

struct ngpu_pgcraft_uniform { // also buffers (for arrays)
    char name[NGPU_ID_LEN];
    enum ngpu_type type;
    enum ngpu_program_stage stage;
    enum ngpu_precision precision;
    const void *data;
    size_t count;
};

enum ngpu_pgcraft_texture_type {
    NGPU_PGCRAFT_TEXTURE_TYPE_NONE,
    NGPU_PGCRAFT_TEXTURE_TYPE_VIDEO,
    NGPU_PGCRAFT_TEXTURE_TYPE_2D,
    NGPU_PGCRAFT_TEXTURE_TYPE_IMAGE_2D,
    NGPU_PGCRAFT_TEXTURE_TYPE_2D_ARRAY,
    NGPU_PGCRAFT_TEXTURE_TYPE_IMAGE_2D_ARRAY,
    NGPU_PGCRAFT_TEXTURE_TYPE_3D,
    NGPU_PGCRAFT_TEXTURE_TYPE_IMAGE_3D,
    NGPU_PGCRAFT_TEXTURE_TYPE_CUBE,
    NGPU_PGCRAFT_TEXTURE_TYPE_IMAGE_CUBE,
    NGPU_PGCRAFT_TEXTURE_TYPE_NB,
    NGPU_PGCRAFT_TEXTURE_MAX_ENUM = 0x7FFFFFFF
};

struct ngpu_pgcraft_texture {
    char name[NGPU_ID_LEN];
    enum ngpu_pgcraft_texture_type type;
    enum ngpu_program_stage stage;
    enum ngpu_precision precision;
    int writable;
    enum ngpu_format format;
    int clamp_video;
    int premult;
    /*
     * Just like the other types (uniforms, blocks, attributes), this field
     * exists in order to be transmitted to the pipeline (through the
     * pipeline_resources destination). That way, these resources can be
     * associated with the pipeline straight after the pipeline initialization
     * (using ngpu_pipeline_set_resources()). In the case of the texture
     * though, there is one exception: if the specified type is
     * NGPU_PGCRAFT_SHADER_TEX_TYPE_VIDEO, then the texture field must be NULL.
     * Indeed, this type implies the potential use of multiple samplers (which
     * can be hardware accelerated and platform/backend specific) depending on
     * the image layout. This means that a single texture cannot be used as a
     * default resource for all these samplers. Moreover, the image layout
     * (which determines which samplers are used) is generally unknown at
     * pipeline initialization and it is only known once a frame has been
     * decoded/mapped. The image structure describes which layout to use and
     * which textures to bind and the ngpu_pgcraft_texture_info.fields describes
     * where to bind the textures and their associated data.
     */
    struct ngpu_texture *texture;
    /*
     * The image field is a bit special, it is not transmitted directly to the
     * pipeline but instead to the corresponding ngpu_pgcraft_texture_info entry
     * accessible through pgcraft.texture_infos. The user may optionally set it
     * if they plan to have access to the image information directly through
     * the ngpu_pgcraft_texture_info structure. The field is pretty much mandatory
     * if the user plans to use ngpu_pipeline_compat_update_image() in
     * conjunction with pgcraft.texture_infos to instruct a pipeline on which
     * texture resources to use.
     */
    struct image *image;
};

struct ngpu_pgcraft_block {
    char name[NGPU_ID_LEN];
    const char *instance_name;
    enum ngpu_type type;
    enum ngpu_program_stage stage;
    int writable;
    const struct ngpu_block_desc *block;
    struct ngpu_buffer_binding buffer;
};

struct ngpu_pgcraft_attribute {
    char name[NGPU_ID_LEN];
    enum ngpu_type type;
    enum ngpu_precision precision;
    enum ngpu_format format;
    size_t stride;
    size_t offset;
    uint32_t rate;
    struct ngpu_buffer *buffer;
};

struct ngpu_pgcraft_iovar {
    char name[NGPU_ID_LEN];
    enum ngpu_precision precision_out;
    enum ngpu_precision precision_in;
    enum ngpu_type type;
};

enum ngpu_image_layout {
    NGPU_IMAGE_LAYOUT_NONE           = 0,
    NGPU_IMAGE_LAYOUT_DEFAULT        = 1,
    NGPU_IMAGE_LAYOUT_MEDIACODEC     = 2,
    NGPU_IMAGE_LAYOUT_NV12           = 3,
    NGPU_IMAGE_LAYOUT_NV12_RECTANGLE = 4,
    NGPU_IMAGE_LAYOUT_YUV            = 5,
    NGPU_IMAGE_LAYOUT_RECTANGLE      = 6,
    NGPU_IMAGE_LAYOUT_NB,
    NGPU_IMAGE_LAYOUT_MAX_ENUM       = 0x7FFFFFFF,
};

enum {
    NGPU_INFO_FIELD_SAMPLING_MODE,
    NGPU_INFO_FIELD_COORDINATE_MATRIX,
    NGPU_INFO_FIELD_COLOR_MATRIX,
    NGPU_INFO_FIELD_DIMENSIONS,
    NGPU_INFO_FIELD_TIMESTAMP,
    NGPU_INFO_FIELD_SAMPLER_0,
    NGPU_INFO_FIELD_SAMPLER_1,
    NGPU_INFO_FIELD_SAMPLER_2,
    NGPU_INFO_FIELD_SAMPLER_OES,
    NGPU_INFO_FIELD_SAMPLER_RECT_0,
    NGPU_INFO_FIELD_SAMPLER_RECT_1,
    NGPU_INFO_FIELD_NB,
    NGPU_INFO_FIELD_MAX_ENUM = 0x7FFFFFFF
};

struct ngpu_pgcraft_texture_info_field {
    enum ngpu_type type;
    int32_t index;
    enum ngpu_program_stage stage;
};

struct ngpu_pgcraft_texture_info {
    size_t id;
    struct ngpu_pgcraft_texture_info_field fields[NGPU_INFO_FIELD_NB];
};

/*
 * Oldest OpenGL flavours exclusively support single uniforms (no concept of
 * blocks). And while OpenGL added support for blocks in addition to uniforms,
 * modern backends such as Vulkan do not support the concept of single uniforms
 * at all. This means there is no cross-API common ground.
 *
 * Since NopeGL still exposes some form of cross-backend uniform abstraction to
 * the user (through the Uniform* nodes), we have to make a compatibility
 * layer, which we name "ublock" (for uniform-block). This compatibility layer
 * maps single uniforms to dedicated uniform blocks.
 */
struct ngpu_pgcraft_compat_info {
    struct ngpu_block_desc ublocks[NGPU_PROGRAM_STAGE_NB];
    int32_t ubindings[NGPU_PROGRAM_STAGE_NB];
    int32_t uindices[NGPU_PROGRAM_STAGE_NB];

    const struct ngpu_pgcraft_texture_info *texture_infos;
    const struct image **images;
    size_t nb_texture_infos;
};

struct ngpu_pgcraft_params {
    const char *program_label;
    const char *vert_base;
    const char *frag_base;
    const char *comp_base;

    const struct ngpu_pgcraft_uniform *uniforms;
    size_t nb_uniforms;
    const struct ngpu_pgcraft_texture *textures;
    size_t nb_textures;
    const struct ngpu_pgcraft_block *blocks;
    size_t nb_blocks;
    const struct ngpu_pgcraft_attribute *attributes;
    size_t nb_attributes;

    const struct ngpu_pgcraft_iovar *vert_out_vars;
    size_t nb_vert_out_vars;

    size_t nb_frag_output;

    uint32_t workgroup_size[3];
};

struct ngpu_pgcraft;

NGPU_API struct ngpu_pgcraft *ngpu_pgcraft_create(struct ngpu_ctx *gpu_ctx);
NGPU_API int ngpu_pgcraft_craft(struct ngpu_pgcraft *s, const struct ngpu_pgcraft_params *params);
NGPU_API int32_t ngpu_pgcraft_get_uniform_index(const struct ngpu_pgcraft *s, const char *name, enum ngpu_program_stage stage);
NGPU_API int32_t ngpu_pgcraft_get_block_index(const struct ngpu_pgcraft *s, const char *name, enum ngpu_program_stage stage);
NGPU_API int32_t ngpu_pgcraft_get_image_index(const struct ngpu_pgcraft *s, const char *name);
NGPU_API const struct ngpu_pgcraft_compat_info *ngpu_pgcraft_get_compat_info(const struct ngpu_pgcraft *s);
NGPU_API const char *ngpu_pgcraft_get_symbol_name(const struct ngpu_pgcraft *s, size_t id);
NGPU_API struct ngpu_vertex_state ngpu_pgcraft_get_vertex_state(const struct ngpu_pgcraft *s);
NGPU_API struct ngpu_vertex_resources ngpu_pgcraft_get_vertex_resources(const struct ngpu_pgcraft *s);
NGPU_API int32_t ngpu_pgcraft_get_vertex_buffer_index(const struct ngpu_pgcraft *s, const char *name);
NGPU_API struct ngpu_program *ngpu_pgcraft_get_program(const struct ngpu_pgcraft *s);
NGPU_API struct ngpu_bindgroup_layout_desc ngpu_pgcraft_get_bindgroup_layout_desc(const struct ngpu_pgcraft *s);
NGPU_API struct ngpu_bindgroup_resources ngpu_pgcraft_get_bindgroup_resources(const struct ngpu_pgcraft *s);
NGPU_API void ngpu_pgcraft_freep(struct ngpu_pgcraft **sp);

#endif
