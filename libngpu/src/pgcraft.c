/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2023 Nope Forge
 * Copyright 2020-2022 GoPro Inc.
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

#include "config.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "utils/log.h"
#include "ctx.h"
#include "ngpu/ngpu.h"
#include "pgcraft.h"

#include "utils/bstr.h"
#include "utils/darray.h"
#include "utils/memory.h"
#include "utils/utils.h"

#if defined(BACKEND_GL) || defined(BACKEND_GLES)
#include "opengl/ctx_gl.h"
#include "opengl/program_gl_utils.h"
#endif

enum {
    NGPU_BINDING_TYPE_UBO,
    NGPU_BINDING_TYPE_SSBO,
    NGPU_BINDING_TYPE_TEXTURE,
    NGPU_BINDING_TYPE_IMAGE,
    NGPU_BINDING_TYPE_NB
};

struct pgcraft_symbol {
    char name[NGPU_ID_LEN];
};

struct pgcraft_pipeline_info {
    struct {
        NGPU_DARRAY(struct ngpu_bindgroup_layout_entry) textures;
        NGPU_DARRAY(struct ngpu_bindgroup_layout_entry) buffers;
        NGPU_DARRAY(struct ngpu_vertex_buffer_layout) vertex_buffers;
    } desc;
    struct {
        NGPU_DARRAY(struct ngpu_texture_binding) textures;
        NGPU_DARRAY(struct ngpu_buffer_binding) buffers;
        NGPU_DARRAY(struct ngpu_buffer *) vertex_buffers;
    } data;
};

struct ngpu_pgcraft {
    struct ngpu_ctx *gpu_ctx;

    NGPU_DARRAY(struct ngpu_pgcraft_texture_info) texture_infos;

    struct bstr *shaders[NGPU_PROGRAM_STAGE_NB];

    NGPU_DARRAY(struct pgcraft_symbol) symbols;

    struct pgcraft_pipeline_info pipeline_info;

    NGPU_DARRAY(struct ngpu_pgcraft_iovar) vert_out_vars;
    NGPU_DARRAY(struct ngpu_pgcraft_texture) textures;

    struct ngpu_program *program;
    struct ngpu_bindgroup_layout *bindgroup_layout;

    uint32_t bindings[NGPU_BINDING_TYPE_NB];
    uint32_t *next_bindings[NGPU_BINDING_TYPE_NB];
    uint32_t next_in_locations[NGPU_PROGRAM_STAGE_NB];
    uint32_t next_out_locations[NGPU_PROGRAM_STAGE_NB];

    /* GLSL info */
    int glsl_version;
    const char *glsl_version_suffix;
    const char *sym_vertex_index;
    const char *sym_instance_index;
    bool has_in_out_layout_qualifiers;
    bool has_precision_qualifiers;
    bool has_explicit_bindings;
};

/*
 * Currently unmapped formats: r11f_g11f_b10f, rgb10_a2, rgb10_a2ui
 */
static const struct {
    const char *format;
    const char *prefix;
} image_glsl_format_map[NGPU_FORMAT_NB] = {
    [NGPU_FORMAT_R8_UNORM]             = {"r8", ""},
    [NGPU_FORMAT_R8_SNORM]             = {"r8_snorm", ""},
    [NGPU_FORMAT_R8_UINT]              = {"r8ui", "u"},
    [NGPU_FORMAT_R8_SINT]              = {"r8i", "i"},
    [NGPU_FORMAT_R8G8_UNORM]           = {"rg8", ""},
    [NGPU_FORMAT_R8G8_SNORM]           = {"rg8_snorm", ""},
    [NGPU_FORMAT_R8G8_UINT]            = {"rg8ui", "u"},
    [NGPU_FORMAT_R8G8_SINT]            = {"rg8i", "i"},
    [NGPU_FORMAT_R8G8B8_UNORM]         = {NULL, NULL},
    [NGPU_FORMAT_R8G8B8_SNORM]         = {NULL, NULL},
    [NGPU_FORMAT_R8G8B8_UINT]          = {NULL, NULL},
    [NGPU_FORMAT_R8G8B8_SINT]          = {NULL, NULL},
    [NGPU_FORMAT_R8G8B8_SRGB]          = {NULL, NULL},
    [NGPU_FORMAT_R8G8B8A8_UNORM]       = {"rgba8", ""},
    [NGPU_FORMAT_R8G8B8A8_SNORM]       = {"rgba8_snorm", ""},
    [NGPU_FORMAT_R8G8B8A8_UINT]        = {"rgba8ui", "u"},
    [NGPU_FORMAT_R8G8B8A8_SINT]        = {"rgba8i", "i"},
    [NGPU_FORMAT_R8G8B8A8_SRGB]        = {NULL, NULL},
    [NGPU_FORMAT_B8G8R8A8_UNORM]       = {"rgba8", ""},
    [NGPU_FORMAT_B8G8R8A8_SNORM]       = {"rgba8_snorm", ""},
    [NGPU_FORMAT_B8G8R8A8_UINT]        = {"rgba8ui", "u"},
    [NGPU_FORMAT_B8G8R8A8_SINT]        = {"rgba8i", "i"},
    [NGPU_FORMAT_R16_UNORM]            = {"r16", ""},
    [NGPU_FORMAT_R16_SNORM]            = {"r16_snorm", ""},
    [NGPU_FORMAT_R16_UINT]             = {"r16ui", "u"},
    [NGPU_FORMAT_R16_SINT]             = {"r16i", "i"},
    [NGPU_FORMAT_R16_SFLOAT]           = {"r16f", ""},
    [NGPU_FORMAT_R16G16_UNORM]         = {"rg16", ""},
    [NGPU_FORMAT_R16G16_SNORM]         = {"rg16_snorm", ""},
    [NGPU_FORMAT_R16G16_UINT]          = {"rg16ui", "u"},
    [NGPU_FORMAT_R16G16_SINT]          = {"rg16i", "i"},
    [NGPU_FORMAT_R16G16_SFLOAT]        = {"rg16f", ""},
    [NGPU_FORMAT_R16G16B16_UNORM]      = {NULL, NULL},
    [NGPU_FORMAT_R16G16B16_SNORM]      = {NULL, NULL},
    [NGPU_FORMAT_R16G16B16_UINT]       = {NULL, NULL},
    [NGPU_FORMAT_R16G16B16_SINT]       = {NULL, NULL},
    [NGPU_FORMAT_R16G16B16_SFLOAT]     = {NULL, NULL},
    [NGPU_FORMAT_R16G16B16A16_UNORM]   = {"rgba16", ""},
    [NGPU_FORMAT_R16G16B16A16_SNORM]   = {"rgba16_snorm", ""},
    [NGPU_FORMAT_R16G16B16A16_UINT]    = {"rgba16ui", "u"},
    [NGPU_FORMAT_R16G16B16A16_SINT]    = {"rgba16i", "i"},
    [NGPU_FORMAT_R16G16B16A16_SFLOAT]  = {"rgba16f", ""},
    [NGPU_FORMAT_R32_UINT]             = {"r32ui", "u"},
    [NGPU_FORMAT_R32_SINT]             = {"r32i", "i"},
    [NGPU_FORMAT_R32_SFLOAT]           = {"r32f", ""},
    [NGPU_FORMAT_R32G32_UINT]          = {"rg32ui", "u"},
    [NGPU_FORMAT_R32G32_SINT]          = {"rg32i", "i"},
    [NGPU_FORMAT_R32G32_SFLOAT]        = {"rg32f", ""},
    [NGPU_FORMAT_R32G32B32_UINT]       = {NULL, NULL},
    [NGPU_FORMAT_R32G32B32_SINT]       = {NULL, NULL},
    [NGPU_FORMAT_R32G32B32_SFLOAT]     = {NULL, NULL},
    [NGPU_FORMAT_R32G32B32A32_UINT]    = {"rgba32ui", "u"},
    [NGPU_FORMAT_R32G32B32A32_SINT]    = {"rgba32i", "i"},
    [NGPU_FORMAT_R32G32B32A32_SFLOAT]  = {"rgba32f", ""},
    [NGPU_FORMAT_D16_UNORM]            = {NULL, NULL},
    [NGPU_FORMAT_X8_D24_UNORM_PACK32]  = {NULL, NULL},
    [NGPU_FORMAT_D32_SFLOAT]           = {NULL, NULL},
    [NGPU_FORMAT_D24_UNORM_S8_UINT]    = {NULL, NULL},
    [NGPU_FORMAT_D32_SFLOAT_S8_UINT]   = {NULL, NULL},
    [NGPU_FORMAT_S8_UINT]              = {NULL, NULL},
};

enum {
    TYPE_FLAG_IS_SAMPLER          = 1 << 0,
    TYPE_FLAG_HAS_PRECISION       = 1 << 1,
    TYPE_FLAG_IS_INT              = 1 << 2,
    TYPE_FLAG_IS_IMAGE            = 1 << 3,
};

static const int type_flags_map[NGPU_TYPE_NB] = {
    [NGPU_TYPE_I32]                         = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_INT,
    [NGPU_TYPE_IVEC2]                       = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_INT,
    [NGPU_TYPE_IVEC3]                       = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_INT,
    [NGPU_TYPE_IVEC4]                       = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_INT,
    [NGPU_TYPE_U32]                         = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_INT,
    [NGPU_TYPE_UVEC2]                       = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_INT,
    [NGPU_TYPE_UVEC3]                       = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_INT,
    [NGPU_TYPE_UVEC4]                       = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_INT,
    [NGPU_TYPE_F32]                         = TYPE_FLAG_HAS_PRECISION,
    [NGPU_TYPE_VEC2]                        = TYPE_FLAG_HAS_PRECISION,
    [NGPU_TYPE_VEC3]                        = TYPE_FLAG_HAS_PRECISION,
    [NGPU_TYPE_VEC4]                        = TYPE_FLAG_HAS_PRECISION,
    [NGPU_TYPE_MAT3]                        = TYPE_FLAG_HAS_PRECISION,
    [NGPU_TYPE_MAT4]                        = TYPE_FLAG_HAS_PRECISION,
    [NGPU_TYPE_BOOL]                        = 0,
    [NGPU_TYPE_SAMPLER_2D]                  = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_SAMPLER,
    [NGPU_TYPE_SAMPLER_2D_ARRAY]            = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_SAMPLER,
    [NGPU_TYPE_SAMPLER_2D_RECT]             = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_SAMPLER,
    [NGPU_TYPE_SAMPLER_3D]                  = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_SAMPLER,
    [NGPU_TYPE_SAMPLER_CUBE]                = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_SAMPLER,
    [NGPU_TYPE_SAMPLER_EXTERNAL_OES]        = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_SAMPLER,
    [NGPU_TYPE_SAMPLER_EXTERNAL_2D_Y2Y_EXT] = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_SAMPLER,
    [NGPU_TYPE_IMAGE_2D]                    = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_IMAGE,
    [NGPU_TYPE_IMAGE_2D_ARRAY]              = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_IMAGE,
    [NGPU_TYPE_IMAGE_3D]                    = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_IMAGE,
    [NGPU_TYPE_IMAGE_CUBE]                  = TYPE_FLAG_HAS_PRECISION|TYPE_FLAG_IS_IMAGE,
    [NGPU_TYPE_UNIFORM_BUFFER]              = 0,
    [NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC]      = 0,
    [NGPU_TYPE_STORAGE_BUFFER]              = 0,
    [NGPU_TYPE_STORAGE_BUFFER_DYNAMIC]      = 0,
};

static const int type_binding_map[NGPU_TYPE_NB] = {
    [NGPU_TYPE_SAMPLER_2D]                  = NGPU_BINDING_TYPE_TEXTURE,
    [NGPU_TYPE_SAMPLER_2D_ARRAY]            = NGPU_BINDING_TYPE_TEXTURE,
    [NGPU_TYPE_SAMPLER_2D_RECT]             = NGPU_BINDING_TYPE_TEXTURE,
    [NGPU_TYPE_SAMPLER_3D]                  = NGPU_BINDING_TYPE_TEXTURE,
    [NGPU_TYPE_SAMPLER_CUBE]                = NGPU_BINDING_TYPE_TEXTURE,
    [NGPU_TYPE_SAMPLER_EXTERNAL_OES]        = NGPU_BINDING_TYPE_TEXTURE,
    [NGPU_TYPE_SAMPLER_EXTERNAL_2D_Y2Y_EXT] = NGPU_BINDING_TYPE_TEXTURE,
    [NGPU_TYPE_IMAGE_2D]                    = NGPU_BINDING_TYPE_IMAGE,
    [NGPU_TYPE_IMAGE_2D_ARRAY]              = NGPU_BINDING_TYPE_IMAGE,
    [NGPU_TYPE_IMAGE_3D]                    = NGPU_BINDING_TYPE_IMAGE,
    [NGPU_TYPE_IMAGE_CUBE]                  = NGPU_BINDING_TYPE_IMAGE,
    [NGPU_TYPE_UNIFORM_BUFFER]              = NGPU_BINDING_TYPE_UBO,
    [NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC]      = NGPU_BINDING_TYPE_UBO,
    [NGPU_TYPE_STORAGE_BUFFER]              = NGPU_BINDING_TYPE_SSBO,
    [NGPU_TYPE_STORAGE_BUFFER_DYNAMIC]      = NGPU_BINDING_TYPE_SSBO,
};

static int is_sampler(enum ngpu_type type)
{
    return type_flags_map[type] & TYPE_FLAG_IS_SAMPLER;
}

static int is_image(enum ngpu_type type)
{
    return type_flags_map[type] & TYPE_FLAG_IS_IMAGE;
}

static int type_has_precision(enum ngpu_type type)
{
    return type_flags_map[type] & TYPE_FLAG_HAS_PRECISION;
}

static int type_is_int(enum ngpu_type type)
{
    return type_flags_map[type] & TYPE_FLAG_IS_INT;
}

static const char *get_glsl_type(enum ngpu_type type)
{
    const char *ret = ngpu_type_get_name(type);
    ngpu_assert(ret);
    return ret;
}

static uint32_t request_next_binding(struct ngpu_pgcraft *s, enum ngpu_type type)
{
    ngpu_assert(type >= 0 && type < NGPU_TYPE_NB);
    const int binding_type = type_binding_map[type];

    uint32_t *next_bind = s->next_bindings[binding_type];
    ngpu_assert(next_bind);

    return (*next_bind)++;
}

static const char *get_precision_qualifier(const struct ngpu_pgcraft *s, enum ngpu_type type, enum ngpu_precision precision, const char *defaultp)
{
    if (!s->has_precision_qualifiers || !type_has_precision(type))
        return "";
    static const char *precision_qualifiers[NGPU_PRECISION_NB] = {
        [NGPU_PRECISION_AUTO]   = NULL,
        [NGPU_PRECISION_HIGH]   = "highp",
        [NGPU_PRECISION_MEDIUM] = "mediump",
        [NGPU_PRECISION_LOW]    = "lowp",
    };
    const char *ret = precision_qualifiers[precision];
    return ret ? ret : defaultp;
}

static const char *get_array_suffix(size_t count, char *buf, size_t len)
{
    if (count == NGPU_BLOCK_DESC_VARIADIC_COUNT)
        snprintf(buf, len, "[]");
    else if (count > 0)
        snprintf(buf, len, "[%zu]", count);
    return buf;
}

#define GET_ARRAY_SUFFIX(count) get_array_suffix(count, (char[32]){0}, 32)

struct sampler_desc {
    const char *suffix;          /* GLSL name suffix ("", "_1", "_2", "_oes", "_rect_0", "_rect_1") */
    size_t info_offset;          /* offsetof(struct ngpu_pgcraft_texture_info, sampler_*_index) */
    enum ngpu_type types[NGPU_PGCRAFT_TEXTURE_TYPE_NB]; /* sampler type per texture type, NONE if unused */
};

static const struct sampler_desc sampler_descs[] = {
    {
        .suffix = "",
        .info_offset = offsetof(struct ngpu_pgcraft_texture_info, sampler_index),
        .types = {
            [NGPU_PGCRAFT_TEXTURE_TYPE_VIDEO]          = NGPU_TYPE_SAMPLER_2D,
            [NGPU_PGCRAFT_TEXTURE_TYPE_2D]             = NGPU_TYPE_SAMPLER_2D,
            [NGPU_PGCRAFT_TEXTURE_TYPE_2D_ARRAY]       = NGPU_TYPE_SAMPLER_2D_ARRAY,
            [NGPU_PGCRAFT_TEXTURE_TYPE_3D]             = NGPU_TYPE_SAMPLER_3D,
            [NGPU_PGCRAFT_TEXTURE_TYPE_CUBE]           = NGPU_TYPE_SAMPLER_CUBE,
            [NGPU_PGCRAFT_TEXTURE_TYPE_IMAGE_2D]       = NGPU_TYPE_IMAGE_2D,
            [NGPU_PGCRAFT_TEXTURE_TYPE_IMAGE_2D_ARRAY] = NGPU_TYPE_IMAGE_2D_ARRAY,
            [NGPU_PGCRAFT_TEXTURE_TYPE_IMAGE_3D]       = NGPU_TYPE_IMAGE_3D,
            [NGPU_PGCRAFT_TEXTURE_TYPE_IMAGE_CUBE]     = NGPU_TYPE_IMAGE_CUBE,
        },
    },
    {
        .suffix = "_1",
        .info_offset = offsetof(struct ngpu_pgcraft_texture_info, sampler_1_index),
        .types = {
            [NGPU_PGCRAFT_TEXTURE_TYPE_VIDEO] = NGPU_TYPE_SAMPLER_2D,
        },
    },
    {
        .suffix = "_2",
        .info_offset = offsetof(struct ngpu_pgcraft_texture_info, sampler_2_index),
        .types = {
            [NGPU_PGCRAFT_TEXTURE_TYPE_VIDEO] = NGPU_TYPE_SAMPLER_2D,
        },
    },
    {
        .suffix = "_oes",
        .info_offset = offsetof(struct ngpu_pgcraft_texture_info, sampler_oes_index),
        .types = {
            [NGPU_PGCRAFT_TEXTURE_TYPE_VIDEO] = NGPU_TYPE_SAMPLER_EXTERNAL_OES,
        },
    },
    {
        .suffix = "_rect_0",
        .info_offset = offsetof(struct ngpu_pgcraft_texture_info, sampler_rect_0_index),
        .types = {
            [NGPU_PGCRAFT_TEXTURE_TYPE_VIDEO] = NGPU_TYPE_SAMPLER_2D_RECT,
        },
    },
    {
        .suffix = "_rect_1",
        .info_offset = offsetof(struct ngpu_pgcraft_texture_info, sampler_rect_1_index),
        .types = {
            [NGPU_PGCRAFT_TEXTURE_TYPE_VIDEO] = NGPU_TYPE_SAMPLER_2D_RECT,
        },
    },
};
#define NB_SAMPLER_DESCS NGPU_ARRAY_NB(sampler_descs)

static int is_type_supported(const struct ngpu_pgcraft *s, enum ngpu_type type)
{
    const struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    switch(type) {
    case NGPU_TYPE_SAMPLER_2D_RECT:
        return gpu_ctx->params.backend == NGPU_BACKEND_OPENGL &&
            NGPU_HAS_ALL_FLAGS(gpu_ctx->features, NGPU_FEATURE_IMPORT_IOSURFACE_BIT);
    case NGPU_TYPE_SAMPLER_EXTERNAL_OES:
    case NGPU_TYPE_SAMPLER_EXTERNAL_2D_Y2Y_EXT:
        return gpu_ctx->params.backend == NGPU_BACKEND_OPENGLES &&
            NGPU_HAS_ALL_FLAGS(gpu_ctx->features, NGPU_FEATURE_IMPORT_AHARDWARE_BUFFER_BIT);
    default:
        return 1;
    }
}

static enum ngpu_program_stage get_texture_field_stage(const struct ngpu_pgcraft_texture *texture)
{
    return texture->stage;
}

static int prepare_texture_infos(struct ngpu_pgcraft *s, const struct ngpu_pgcraft_params *params, int graphics)
{
    for (size_t i = 0; i < params->nb_textures; i++) {
        const struct ngpu_pgcraft_texture *texture = &params->textures[i];
        ngpu_assert(!(texture->type == NGPU_PGCRAFT_TEXTURE_TYPE_VIDEO && texture->texture));

        if (ngpu_darray_push(&s->textures, params->textures[i]) < 0)
            return NGPU_ERROR_MEMORY;

        struct pgcraft_symbol sym = {0};
        snprintf(sym.name, NGPU_ID_LEN, "%s", texture->name);
        if (ngpu_darray_push(&s->symbols, sym) < 0)
            return NGPU_ERROR_MEMORY;

        const struct ngpu_pgcraft_texture_info info = {
            .sampler_index        = -1,
            .sampler_1_index      = -1,
            .sampler_2_index      = -1,
            .sampler_oes_index    = -1,
            .sampler_rect_0_index = -1,
            .sampler_rect_1_index = -1,
            .block_index          = -1,
            .image                = texture->image,
        };

        if (ngpu_darray_push(&s->texture_infos, info) < 0)
            return NGPU_ERROR_MEMORY;
    }
    return 0;
}

static int inject_texture(struct ngpu_pgcraft *s, const struct ngpu_pgcraft_texture *texture, struct ngpu_pgcraft_texture_info *info, enum ngpu_program_stage stage)
{
    for (size_t i = 0; i < NB_SAMPLER_DESCS; i++) {
        const struct sampler_desc *sd = &sampler_descs[i];
        const enum ngpu_type field_type = sd->types[texture->type];
        if (field_type == NGPU_TYPE_NONE || !is_type_supported(s, field_type))
            continue;
        const enum ngpu_program_stage field_stage = get_texture_field_stage(texture);
        if (field_stage != stage)
            continue;

        char name[NGPU_ID_LEN];
        int len = snprintf(name, sizeof(name), "%s%s", texture->name, sd->suffix);
        if (len >= sizeof(name)) {
            LOG(ERROR, "texture name \"%s\" is too long", texture->name);
            return NGPU_ERROR_MEMORY;
        }

        struct bstr *b = s->shaders[stage];

        if (is_sampler(field_type) || is_image(field_type)) {
            struct pgcraft_symbol sym = {0};
            snprintf(sym.name, NGPU_ID_LEN, "%s", name);
            if (ngpu_darray_push(&s->symbols, sym) < 0)
                return NGPU_ERROR_MEMORY;

            const struct ngpu_bindgroup_layout_entry layout_entry = {
                .id          = s->symbols.count - 1,
                .type        = field_type,
                .binding     = request_next_binding(s, field_type),
                .access      = texture->writable ? NGPU_ACCESS_READ_WRITE : NGPU_ACCESS_READ_BIT,
                .stage_flags = 1U << stage,
            };

            const char *prefix = "";
            if (is_image(field_type)) {
                if (texture->format == NGPU_FORMAT_UNDEFINED) {
                    LOG(ERROR, "texture format must be set when accessing it as an image");
                    return NGPU_ERROR_INVALID_ARG;
                }
                const char *format = image_glsl_format_map[texture->format].format;
                prefix = image_glsl_format_map[texture->format].prefix;
                if (!format || !prefix) {
                    LOG(ERROR, "unsupported texture format");
                    return NGPU_ERROR_UNSUPPORTED;
                }

                ngpu_bstr_printf(b, "layout(%s", format);

                if (s->has_explicit_bindings)
                    ngpu_bstr_printf(b, ", binding=%u", layout_entry.binding);

                /*
                 * Restrict memory qualifier according to the OpenGLES 3.2 spec
                 * (Section 4.10. Memory qualifiers):
                 *
                 *     Except for image variables qualified with the format
                 *     qualifiers r32f, r32i, and r32ui, image variables must
                 *     specify a memory qualifier (readonly, writeonly, or both).
                 */
                const char *writable_qualifier= "";
                if (texture->format != NGPU_FORMAT_R32_SFLOAT &&
                    texture->format != NGPU_FORMAT_R32_SINT &&
                    texture->format != NGPU_FORMAT_R32_UINT) {
                    writable_qualifier = "writeonly";
                }
                ngpu_bstr_printf(b, ") %s ", texture->writable ? writable_qualifier : "readonly");
            } else if (s->has_explicit_bindings) {
                ngpu_bstr_printf(b, "layout(binding=%u) ", layout_entry.binding);
            }

            const char *type = get_glsl_type(field_type);
            const char *precision = get_precision_qualifier(s, field_type, texture->precision, "lowp");
            ngpu_bstr_printf(b, "uniform %s %s%s %s;\n", precision, prefix, type, name);

            if (ngpu_darray_push(&s->pipeline_info.desc.textures, layout_entry) < 0)
                return NGPU_ERROR_MEMORY;

            const struct ngpu_texture_binding texture_binding = {
                .texture = texture->texture,
            };
            if (ngpu_darray_push(&s->pipeline_info.data.textures, texture_binding) < 0)
                return NGPU_ERROR_MEMORY;
        }
    }

    return 0;
}

static int inject_textures(struct ngpu_pgcraft *s, const struct ngpu_pgcraft_params *params, enum ngpu_program_stage stage)
{
    for (size_t i = 0; i < s->texture_infos.count; i++) {
        int ret = inject_texture(s, &s->textures.data[i], &s->texture_infos.data[i], stage);
        if (ret < 0)
            return ret;
    }
    return 0;
}

/* Find an existing buffer binding by symbol name, return binding index or -1 */
static int32_t find_buffer_binding(const struct ngpu_pgcraft *s, const char *name)
{
    for (size_t i = 0; i < s->symbols.count; i++) {
        if (!strcmp(ngpu_pgcraft_get_symbol_name(s, i), name)) {
            for (size_t k = 0; k < s->pipeline_info.desc.buffers.count; k++) {
                if (s->pipeline_info.desc.buffers.data[k].id == i)
                    return (int32_t)s->pipeline_info.desc.buffers.data[k].binding;
            }
            break;
        }
    }
    return -1;
}

/* Register a new uniform buffer binding, return binding index via *bindingp */
static int register_buffer_binding(struct ngpu_pgcraft *s, const char *name,
                                   uint32_t stage_flags, int32_t *bindingp)
{
    struct pgcraft_symbol sym = {0};
    snprintf(sym.name, NGPU_ID_LEN, "%s", name);
    if (ngpu_darray_push(&s->symbols, sym) < 0)
        return NGPU_ERROR_MEMORY;

    const int32_t binding = (int32_t)request_next_binding(s, NGPU_TYPE_UNIFORM_BUFFER);

    const struct ngpu_bindgroup_layout_entry layout_entry = {
        .id          = s->symbols.count - 1,
        .type        = NGPU_TYPE_UNIFORM_BUFFER,
        .binding     = (uint32_t)binding,
        .access      = NGPU_ACCESS_READ_BIT,
        .stage_flags = stage_flags,
    };
    if (ngpu_darray_push(&s->pipeline_info.desc.buffers, layout_entry) < 0)
        return NGPU_ERROR_MEMORY;

    if (ngpu_darray_push(&s->pipeline_info.data.buffers, (struct ngpu_buffer_binding){0}) < 0)
        return NGPU_ERROR_MEMORY;

    *bindingp = binding;
    return 0;
}

static int inject_texture_info_block(struct ngpu_pgcraft *s, enum ngpu_program_stage stage)
{
    ngpu_darray_foreach(texture, &s->textures) {
        if (texture->no_metadata)
            continue;

        struct bstr *b = s->shaders[stage];
        if (!b)
            continue;

        const bool is_graphics = (s->shaders[NGPU_PROGRAM_STAGE_VERT] != NULL);
        const uint32_t stage_flags = is_graphics
            ? (NGPU_PROGRAM_STAGE_VERTEX_BIT | NGPU_PROGRAM_STAGE_FRAGMENT_BIT)
            : NGPU_PROGRAM_STAGE_COMPUTE_BIT;

        char block_name[NGPU_ID_LEN];
        snprintf(block_name, sizeof(block_name), "%s_info", texture->name);

        int32_t binding = find_buffer_binding(s, block_name);
        if (binding < 0) {
            int ret = register_buffer_binding(s, block_name, stage_flags, &binding);
            if (ret < 0)
                return ret;
        }

        if (s->has_explicit_bindings)
            ngpu_bstr_printf(b, "layout(std140,binding=%u)", (uint32_t)binding);
        else
            ngpu_bstr_print(b, "layout(std140)");

        ngpu_bstr_printf(b, " uniform %s_block {\n", block_name);
        ngpu_bstr_printf(b, "    mat4 %s_coord_matrix;\n", texture->name);
        ngpu_bstr_printf(b, "    mat4 %s_color_matrix;\n", texture->name);
        ngpu_bstr_printf(b, "    mat4 %s_mapping_color_matrix;\n", texture->name);
        ngpu_bstr_printf(b, "    vec2 %s_dimensions;\n", texture->name);
        ngpu_bstr_printf(b, "    float %s_ts;\n", texture->name);
        ngpu_bstr_printf(b, "    int %s_sampling_mode;\n", texture->name);
        ngpu_bstr_print(b, "} ;\n");
    }
    return 0;
}

static const char *glsl_layout_str_map[NGPU_BLOCK_NB_LAYOUTS] = {
    [NGPU_BLOCK_LAYOUT_STD140] = "std140",
    [NGPU_BLOCK_LAYOUT_STD430] = "std430",
};

static int inject_block(struct ngpu_pgcraft *s, struct bstr *b,
                        const struct ngpu_pgcraft_block *named_block)
{
    struct pgcraft_symbol sym = {0};
    snprintf(sym.name, NGPU_ID_LEN, "%s", named_block->name);
    if (ngpu_darray_push(&s->symbols, sym) < 0)
        return NGPU_ERROR_MEMORY;

    const struct ngpu_bindgroup_layout_entry layout_entry = {
        .id          = s->symbols.count - 1,
        .type        = named_block->type,
        .binding     = request_next_binding(s, named_block->type),
        .access      = named_block->writable ? NGPU_ACCESS_READ_WRITE : NGPU_ACCESS_READ_BIT,
        .stage_flags = 1U << named_block->stage,
    };

    const struct ngpu_block_desc *block = named_block->block;
    const char *layout = glsl_layout_str_map[block->layout];
    if (s->has_explicit_bindings) {
        ngpu_bstr_printf(b, "layout(%s,binding=%u)", layout, layout_entry.binding);
    } else {
        ngpu_bstr_printf(b, "layout(%s)", layout);
    }

    if (named_block->type == NGPU_TYPE_STORAGE_BUFFER && !named_block->writable)
        ngpu_bstr_print(b, " readonly");

    const char *keyword = get_glsl_type(named_block->type);
    ngpu_bstr_printf(b, " %s %s_block {\n", keyword, named_block->name);
    const struct ngpu_block_field *field_info = block->fields;
    for (size_t i = 0; i < block->nb_fields; i++) {
        const struct ngpu_block_field *fi = &field_info[i];
        const char *type = get_glsl_type(fi->type);
        const char *precision = get_precision_qualifier(s, fi->type, fi->precision, "");
        const char *array_suffix = GET_ARRAY_SUFFIX(fi->count);
        ngpu_bstr_printf(b, "    %s %s %s%s;\n", precision, type, fi->name, array_suffix);
    }
    const char *instance_name = named_block->instance_name ? named_block->instance_name : named_block->name;
    ngpu_bstr_printf(b, "} %s;\n", instance_name);

    if (ngpu_darray_push(&s->pipeline_info.desc.buffers, layout_entry) < 0)
        return NGPU_ERROR_MEMORY;

    if (ngpu_darray_push(&s->pipeline_info.data.buffers, named_block->buffer) < 0)
        return NGPU_ERROR_MEMORY;

    return (int)layout_entry.binding;
}

static int inject_blocks(struct ngpu_pgcraft *s, struct bstr *b,
                         const struct ngpu_pgcraft_params *params, enum ngpu_program_stage stage)
{
    for (size_t i = 0; i < params->nb_blocks; i++) {
        const struct ngpu_pgcraft_block *block = &params->blocks[i];
        if (block->stage != stage)
            continue;
        int ret = inject_block(s, b, &params->blocks[i]);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static uint32_t get_location_count(enum ngpu_type type)
{
    switch (type) {
    case NGPU_TYPE_MAT3: return 3;
    case NGPU_TYPE_MAT4: return 4;
    default:             return 1;
    }
}

static int inject_attribute(struct ngpu_pgcraft *s, struct bstr *b,
                            const struct ngpu_pgcraft_attribute *attribute)
{
    const char *type = get_glsl_type(attribute->type);
    const uint32_t attribute_count = get_location_count(attribute->type);

    const uint32_t base_location = s->next_in_locations[NGPU_PROGRAM_STAGE_VERT];
    s->next_in_locations[NGPU_PROGRAM_STAGE_VERT] += attribute_count;

    if (s->has_in_out_layout_qualifiers) {
        ngpu_bstr_printf(b, "layout(location=%u) ", base_location);
    }

    const char *precision = get_precision_qualifier(s, attribute->type, attribute->precision, "highp");
    ngpu_bstr_printf(b, "in %s %s %s;\n", precision, type, attribute->name);

    struct ngpu_vertex_buffer_layout vertex_buffer = {
        .stride = attribute->stride,
        .rate = attribute->rate,
    };

    const size_t attribute_offset = ngpu_format_get_bytes_per_pixel(attribute->format);
    for (uint32_t i = 0; i < attribute_count; i++) {
        struct pgcraft_symbol sym = {0};
        snprintf(sym.name, NGPU_ID_LEN, "%s", attribute->name);
        if (ngpu_darray_push(&s->symbols, sym) < 0)
            return NGPU_ERROR_MEMORY;

        ngpu_assert(vertex_buffer.nb_attributes < NGPU_MAX_ATTRIBUTES_PER_BUFFER);
        vertex_buffer.attributes[vertex_buffer.nb_attributes++] = (struct ngpu_vertex_attribute) {
            .id = s->symbols.count - 1,
            .location = base_location + i,
            .format = attribute->format,
            .offset = attribute->offset + i * attribute_offset,
        };
    }

    if (ngpu_darray_push(&s->pipeline_info.desc.vertex_buffers, vertex_buffer) < 0)
        return NGPU_ERROR_MEMORY;
    if (ngpu_darray_push(&s->pipeline_info.data.vertex_buffers, attribute->buffer) < 0)
        return NGPU_ERROR_MEMORY;

    return 0;
}

static int inject_attributes(struct ngpu_pgcraft *s, struct bstr *b,
                             const struct ngpu_pgcraft_params *params)
{
    for (size_t i = 0; i < params->nb_attributes; i++) {
        int ret = inject_attribute(s, b, &params->attributes[i]);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int params_have_ssbos(struct ngpu_pgcraft *s, const struct ngpu_pgcraft_params *params, enum ngpu_program_stage stage)
{
    for (size_t i = 0; i < params->nb_blocks; i++) {
        const struct ngpu_pgcraft_block *pgcraft_block = &params->blocks[i];
        if (pgcraft_block->stage == stage && pgcraft_block->type == NGPU_TYPE_STORAGE_BUFFER)
            return 1;
    }
    return 0;
}

static int params_have_images(struct ngpu_pgcraft *s, const struct ngpu_pgcraft_params *params, enum ngpu_program_stage stage)
{
    ngpu_darray_foreach(texture, &s->textures) {
        for (size_t j = 0; j < NB_SAMPLER_DESCS; j++) {
            const struct sampler_desc *sd = &sampler_descs[j];
            const enum ngpu_type sampler_type = sd->types[texture->type];
            if (sampler_type == NGPU_TYPE_NONE || !is_type_supported(s, sampler_type))
                continue;
            const enum ngpu_program_stage field_stage = get_texture_field_stage(texture);
            if (field_stage == stage && is_image(sampler_type))
                return 1;
        }
    }
    return 0;
}

static void set_glsl_header(struct ngpu_pgcraft *s, struct bstr *b, const struct ngpu_pgcraft_params *params, enum ngpu_program_stage stage)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;
    const struct ngpu_ctx_params *ctx_params = &gpu_ctx->params;

    ngpu_bstr_printf(b, "#version %d%s\n", s->glsl_version, s->glsl_version_suffix);

    const int require_ssbo_feature = params_have_ssbos(s, params, stage);
    const int require_image_feature = params_have_images(s, params, stage);
#if defined(TARGET_ANDROID)
    const int require_image_external_essl3_feature = s->texture_infos.count > 0;
#endif

    const struct {
        int backend;
        const char *extension;
        int glsl_version;
        int required;
    } features[] = {
        /* OpenGL */
        {NGPU_BACKEND_OPENGL, "GL_ARB_shading_language_420pack",       420, s->has_explicit_bindings},
        {NGPU_BACKEND_OPENGL, "GL_ARB_shader_image_load_store",        420, require_image_feature},
        {NGPU_BACKEND_OPENGL, "GL_ARB_shader_image_size",              430, require_image_feature},
        {NGPU_BACKEND_OPENGL, "GL_ARB_shader_storage_buffer_object",   430, require_ssbo_feature},
        {NGPU_BACKEND_OPENGL, "GL_ARB_compute_shader",                 430, stage == NGPU_PROGRAM_STAGE_COMP},

        /* OpenGLES */
#if defined(TARGET_ANDROID)
        {NGPU_BACKEND_OPENGLES, "GL_OES_EGL_image_external_essl3", INT_MAX, require_image_external_essl3_feature},
#endif
    };

    for (size_t i = 0; i < NGPU_ARRAY_NB(features); i++) {
        if (features[i].backend == ctx_params->backend &&
            features[i].glsl_version > s->glsl_version &&
            features[i].required)
            ngpu_bstr_printf(b, "#extension %s : require\n", features[i].extension);
    }

    ngpu_bstr_print(b, "\n");
}

static int texture_needs_clamping(const struct ngpu_pgcraft_params *params,
                                  const char *name, size_t name_len)
{
    for (size_t i = 0; i < params->nb_textures; i++) {
        const struct ngpu_pgcraft_texture *pgcraft_texture = &params->textures[i];
        if (!strncmp(name, pgcraft_texture->name, name_len))
            return pgcraft_texture->clamp_video;
    }
    return 0;
}

static int texture_needs_premultiply(const struct ngpu_pgcraft_params *params,
                                        const char *name, size_t name_len)
{
    for (size_t i = 0; i < params->nb_textures; i++) {
        const struct ngpu_pgcraft_texture *pgcraft_texture = &params->textures[i];
        if (!strncmp(name, pgcraft_texture->name, name_len))
            return pgcraft_texture->premult;
    }
    return 0;
}

static enum ngpu_pgcraft_texture_type get_texture_type(const struct ngpu_pgcraft_params *params,
                                                     const char *name, size_t name_len)
{
    for (size_t i = 0; i < params->nb_textures; i++) {
        const struct ngpu_pgcraft_texture *pgcraft_texture = &params->textures[i];
        if (!strncmp(name, pgcraft_texture->name, name_len))
            return pgcraft_texture->type;
    }
    return NGPU_PGCRAFT_TEXTURE_TYPE_NONE;
}

#define WHITESPACES     "\r\n\t "
#define TOKEN_ID_CHARS  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"

static const char *read_token_id(const char *p, char *buf, size_t size)
{
    const size_t len = strspn(p, TOKEN_ID_CHARS);
    snprintf(buf, size, "%.*s", (int)len, p);
    return p + len;
}

static const char *skip_arg(const char *p)
{
    /*
     * TODO: need to error out on directive lines since evaluating them is too
     * complex (and you could close a '(' in a #ifdef and close again in the
     * #else branch, so it's a problem for us)
     */
    int opened_paren = 0;
    while (*p) {
        if (*p == ',' && !opened_paren) {
            break;
        } else if (*p == '(') {
            opened_paren++;
            p++;
        } else if (*p == ')') {
            if (opened_paren == 0)
                break;
            opened_paren--;
            p++;
        } else if (!strncmp(p, "//", 2)) {
            p += strcspn(p, "\r\n");
            // TODO: skip to EOL (handle '\' at EOL?)
        } else if (!strncmp(p, "/*", 2)) {
            p += 2;
            const char *eoc = strstr(p, "*/");
            if (eoc)
                p = eoc + 2;
        } else {
            p++;
        }
    }
    return p;
}

struct token {
    char id[16];
    size_t pos;
};

#define ARG_FMT(x) (int)x##_len, x##_start

static int handle_token(struct ngpu_pgcraft *s, const struct ngpu_pgcraft_params *params,
                        const struct token *token, const char *p, struct bstr *dst)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;
    const struct ngpu_ctx_params *gpu_ctx_params = &gpu_ctx->params;

    /* Skip "ngl_XXX(" and the whitespaces */
    p += strlen(token->id);
    p += strspn(p, WHITESPACES);
    if (*p++ != '(')
        return NGPU_ERROR_INVALID_ARG;
    p += strspn(p, WHITESPACES);

    /* Extract the first argument (texture base name) from which we later
     * derive all the uniform names */
    const char *arg0_start = p;
    p = skip_arg(p);
    size_t arg0_len = (size_t)(p - arg0_start);

    if (!strcmp(token->id, "ngl_texvideo")) {
        if (*p != ',')
            return NGPU_ERROR_INVALID_ARG;
        p++;
        p += strspn(p, WHITESPACES);

        const char *coords_start = p;
        p = skip_arg(p);
        ptrdiff_t coords_len = p - coords_start;
        if (*p != ')')
            return NGPU_ERROR_INVALID_ARG;
        p++;

        const int premultiply = texture_needs_premultiply(params, arg0_start, arg0_len);

        const enum ngpu_pgcraft_texture_type texture_type = get_texture_type(params, arg0_start, arg0_len);
        if (texture_type != NGPU_PGCRAFT_TEXTURE_TYPE_VIDEO) {
            if (premultiply)
                ngpu_bstr_print(dst, "ngli_premultiply(");

            ngpu_bstr_printf(dst, "texture(%.*s, %.*s)", ARG_FMT(arg0), ARG_FMT(coords));
            if (premultiply)
                ngpu_bstr_print(dst, ")");

            ngpu_bstr_print(dst, p);
            return 0;
        }

        if (premultiply)
            ngpu_bstr_print(dst, "ngli_premultiply(");

        const int clamp = texture_needs_clamping(params, arg0_start, arg0_len);
        if (clamp)
            ngpu_bstr_print(dst, "clamp(");

        ngpu_bstr_print(dst, "(");
        ngpu_bstr_print(dst, "(");

        if (gpu_ctx_params->backend == NGPU_BACKEND_OPENGLES &&
            NGPU_HAS_ALL_FLAGS(gpu_ctx->features, NGPU_FEATURE_IMPORT_AHARDWARE_BUFFER_BIT)) {
            ngpu_bstr_printf(dst, "%.*s_sampling_mode == %d ? ", ARG_FMT(arg0), NGPU_IMAGE_LAYOUT_MEDIACODEC);
            ngpu_bstr_printf(dst, "texture(%.*s_oes, %.*s) : ", ARG_FMT(arg0), ARG_FMT(coords));
        }

        if (NGPU_HAS_ALL_FLAGS(gpu_ctx->features, NGPU_FEATURE_IMPORT_IOSURFACE_BIT)) {
            ngpu_bstr_printf(dst, " %.*s_sampling_mode == %d ? ", ARG_FMT(arg0), NGPU_IMAGE_LAYOUT_NV12_RECTANGLE);
            ngpu_bstr_printf(dst, "%.*s_color_matrix * vec4(texture(%.*s_rect_0, (%.*s) * textureSize(%.*s_rect_0)).r, "
                                                           "texture(%.*s_rect_1, (%.*s) * textureSize(%.*s_rect_1)).rg, 1.0) : ",
                             ARG_FMT(arg0),
                             ARG_FMT(arg0), ARG_FMT(coords), ARG_FMT(arg0),
                             ARG_FMT(arg0), ARG_FMT(coords), ARG_FMT(arg0));
        }

        if (NGPU_HAS_ALL_FLAGS(gpu_ctx->features, NGPU_FEATURE_IMPORT_IOSURFACE_BIT)) {
            ngpu_bstr_printf(dst, "%.*s_sampling_mode == %d ? ", ARG_FMT(arg0), NGPU_IMAGE_LAYOUT_RECTANGLE);
            ngpu_bstr_printf(dst, "texture(%.*s_rect_0, (%.*s) * textureSize(%.*s_rect_0)) : ",
                             ARG_FMT(arg0), ARG_FMT(coords), ARG_FMT(arg0));
        }

            ngpu_bstr_printf(dst, "%.*s_sampling_mode == %d ? ", ARG_FMT(arg0), NGPU_IMAGE_LAYOUT_NV12);
            ngpu_bstr_printf(dst, "%.*s_color_matrix * vec4(texture(%.*s,   %.*s).r, "
                                                           "texture(%.*s_1, %.*s).rg, 1.0) : ",
                             ARG_FMT(arg0),
                             ARG_FMT(arg0), ARG_FMT(coords),
                             ARG_FMT(arg0), ARG_FMT(coords));

            ngpu_bstr_printf(dst, "%.*s_sampling_mode == %d ? ", ARG_FMT(arg0), NGPU_IMAGE_LAYOUT_YUV);
            ngpu_bstr_printf(dst, "%.*s_color_matrix * vec4(texture(%.*s,   %.*s).r, "
                                                           "texture(%.*s_1, %.*s).r, "
                                                           "texture(%.*s_2, %.*s).r, 1.0) : ",
                             ARG_FMT(arg0),
                             ARG_FMT(arg0), ARG_FMT(coords),
                             ARG_FMT(arg0), ARG_FMT(coords),
                             ARG_FMT(arg0), ARG_FMT(coords));

        ngpu_bstr_printf(dst, "texture(%.*s, %.*s)", ARG_FMT(arg0), ARG_FMT(coords));

        ngpu_bstr_printf(dst, ") * %.*s_mapping_color_matrix", ARG_FMT(arg0));

        ngpu_bstr_print(dst, ")");
        if (clamp)
            ngpu_bstr_print(dst, ", 0.0, 1.0)");
        if (premultiply)
            ngpu_bstr_print(dst, ")");
        ngpu_bstr_print(dst, p);
    } else {
        ngpu_assert(0);
    }
    return 0;
}

/*
 * We can not make use of the GLSL preproc to create these custom ngl_*()
 * operators because token pasting (##) is needed but illegal in GLES.
 *
 * Implementing a complete preprocessor is too much of a hassle and risky,
 * especially since we need to evaluate all directives in addition to ours.
 * Instead, we do a simple search & replace for our custom texture helpers. We
 * make sure it supports basic nesting, but aside from that, it's pretty basic.
 */
static int samplers_preproc(struct ngpu_pgcraft *s, const struct ngpu_pgcraft_params *params, struct bstr *b)
{
    /*
     * If there is no texture, no point in looking for these custom "ngl_"
     * texture picking symbols.
     */
    if (!s->texture_infos.data)
        return 0;

    struct bstr *tmp_buf = ngpu_bstr_create();
    if (!tmp_buf)
        return NGPU_ERROR_MEMORY;

    /*
     * Construct a stack of "ngl*" tokens found in the shader.
     */
    NGPU_DARRAY(struct token) token_stack = {0};
    const char *base_str = ngpu_bstr_strptr(b);
    const char *p = base_str;
    while ((p = strstr(p, "ngl"))) {
        struct token token = {.pos = (size_t)(p - base_str)};
        p = read_token_id(p, token.id, sizeof(token.id));
        if (strcmp(token.id, "ngl_texvideo"))
            continue;
        ngpu_darray_push(&token_stack, token);
    }

    /*
     * Read and process the stack from the bottom-up so that we know there is
     * never anything left to substitute up until the end of the buffer.
     */
    int ret = 0;
    const size_t nb_tokens = token_stack.count;
    for (size_t i = 0; i < nb_tokens; i++) {
        const struct token *token = &token_stack.data[nb_tokens - i - 1];
        ngpu_bstr_clear(tmp_buf);

        /*
         * We get back the pointer in case it changed in a previous iteration
         * (internal realloc while extending it). The token offset on the other
         * hand wouldn't change since we're doing the replacements backward.
         */
        p = ngpu_bstr_strptr(b);
        ret = handle_token(s, params, token, p + token->pos, tmp_buf);
        if (ret < 0)
            break;

        /*
         * The token function did print into the temporary buffer everything
         * up until the end of the buffer, so we can just truncate the main
         * buffer, and re-append the new payload.
         */
        ngpu_bstr_truncate(b, token->pos);
        ngpu_bstr_print(b, ngpu_bstr_strptr(tmp_buf));
    }

    ngpu_darray_reset(&token_stack);
    ngpu_bstr_freep(&tmp_buf);
    return ret;
}

static int inject_iovars(struct ngpu_pgcraft *s, struct bstr *b, enum ngpu_program_stage stage)
{
    static const char *qualifiers[2] = {
        [NGPU_PROGRAM_STAGE_VERT] = "out",
        [NGPU_PROGRAM_STAGE_FRAG] = "in",
    };
    const char *qualifier = qualifiers[stage];
    uint32_t location = 0;
    ngpu_darray_foreach(iovar, &s->vert_out_vars) {
        if (s->has_in_out_layout_qualifiers)
            ngpu_bstr_printf(b, "layout(location=%u) ", location);
        const char *precision = stage == NGPU_PROGRAM_STAGE_VERT
                              ? get_precision_qualifier(s, iovar->type, iovar->precision_out, "highp")
                              : get_precision_qualifier(s, iovar->type, iovar->precision_in, "highp");
        const char *type = get_glsl_type(iovar->type);
        if (type_is_int(iovar->type))
            ngpu_bstr_print(b, "flat ");
        ngpu_bstr_printf(b, "%s %s %s %s;\n", qualifier, precision, type, iovar->name);
        location += get_location_count(iovar->type);
    }
    return 0;
}

static int craft_vert(struct ngpu_pgcraft *s, const struct ngpu_pgcraft_params *params)
{
    struct bstr *b = s->shaders[NGPU_PROGRAM_STAGE_VERT];

    set_glsl_header(s, b, params, NGPU_PROGRAM_STAGE_VERT);

    ngpu_bstr_printf(b, "#define ngl_out_pos gl_Position\n"
                        "#define ngl_vertex_index %s\n"
                        "#define ngl_instance_index %s\n",
                        s->sym_vertex_index, s->sym_instance_index);

    int ret;
    if ((ret = inject_iovars(s, b, NGPU_PROGRAM_STAGE_VERT)) < 0 ||
        (ret = inject_textures(s, params, NGPU_PROGRAM_STAGE_VERT)) < 0 ||
        (ret = inject_blocks(s, b, params, NGPU_PROGRAM_STAGE_VERT)) < 0 ||
        (ret = inject_texture_info_block(s, NGPU_PROGRAM_STAGE_VERT)) < 0 ||
        (ret = inject_attributes(s, b, params)) < 0)
        return ret;

    ngpu_bstr_print(b, params->vert_base);
    return samplers_preproc(s, params, b);
}

static int craft_frag(struct ngpu_pgcraft *s, const struct ngpu_pgcraft_params *params)

{
    struct bstr *b = s->shaders[NGPU_PROGRAM_STAGE_FRAG];

    set_glsl_header(s, b, params, NGPU_PROGRAM_STAGE_FRAG);

    if (s->has_precision_qualifiers)
        ngpu_bstr_print(b, "#if GL_FRAGMENT_PRECISION_HIGH\n"
                           "precision highp float;\n"
                           "precision highp int;\n"
                           "#else\n"
                           "precision mediump float;\n"
                           "precision mediump int;\n"
                           "#endif\n");
    else
        /*
         * The OpenGL wiki states the following:
         *     Precision qualifiers in GLSL are supported for compatibility
         *     with OpenGL ES. They use the same syntax as ES's qualifiers, but
         *     they have no functional effects.
         *  But as safety measure, we define them anyway.
         */
        ngpu_bstr_print(b, "#define lowp\n"
                           "#define mediump\n"
                           "#define highp\n");

    ngpu_bstr_print(b, "\n");

    ngpu_bstr_printf(b, "vec4 ngli_premultiply(vec4 color) { return vec4(color.rgb, 1.0) * color.a; }\n");

    if (s->has_in_out_layout_qualifiers) {
        const uint32_t out_location = s->next_out_locations[NGPU_PROGRAM_STAGE_FRAG]++;
        ngpu_bstr_printf(b, "layout(location=%u) ", out_location);
    }
    if (params->nb_frag_output)
        ngpu_bstr_printf(b, "out vec4 ngl_out_color[%zu];\n", params->nb_frag_output);
    else
        ngpu_bstr_print(b, "out vec4 ngl_out_color;\n");

    int ret;
    if ((ret = inject_iovars(s, b, NGPU_PROGRAM_STAGE_FRAG)) < 0 ||
        (ret = inject_textures(s, params, NGPU_PROGRAM_STAGE_FRAG)) < 0 ||
        (ret = inject_blocks(s, b, params, NGPU_PROGRAM_STAGE_FRAG)) < 0 ||
        (ret = inject_texture_info_block(s, NGPU_PROGRAM_STAGE_FRAG)) < 0)
        return ret;

    ngpu_bstr_print(b, "\n");

    ngpu_bstr_print(b, params->frag_base);
    return samplers_preproc(s, params, b);
}

static int craft_comp(struct ngpu_pgcraft *s, const struct ngpu_pgcraft_params *params)
{
    struct bstr *b = s->shaders[NGPU_PROGRAM_STAGE_COMP];

    set_glsl_header(s, b, params, NGPU_PROGRAM_STAGE_COMP);

    const uint32_t *wg_size = params->workgroup_size;
    ngpu_bstr_printf(b, "layout(local_size_x=%u, local_size_y=%u, local_size_z=%u) in;\n", NGPU_ARG_VEC3(wg_size));

    int ret;
    if ((ret = inject_textures(s, params, NGPU_PROGRAM_STAGE_COMP)) < 0 ||
        (ret = inject_blocks(s, b, params, NGPU_PROGRAM_STAGE_COMP)) < 0 ||
        (ret = inject_texture_info_block(s, NGPU_PROGRAM_STAGE_COMP)) < 0)
        return ret;

    ngpu_bstr_print(b, params->comp_base);
    return samplers_preproc(s, params, b);
}

NGPU_STATIC_ASSERT(offsetof(struct ngpu_bindgroup_layout_entry, id) == 0, "resource name offset");


static int32_t get_texture_index(const struct ngpu_pgcraft *s, const char *name)
{
    for (int32_t i = 0; i < (int32_t)s->pipeline_info.desc.textures.count; i++) {
        const struct ngpu_bindgroup_layout_entry *entry = &s->pipeline_info.desc.textures.data[i];
        const char *texture_name = ngpu_pgcraft_get_symbol_name(s, entry->id);
        if (!strcmp(texture_name, name))
            return i;
    }
    return -1;
}

static void probe_texture_info_elems(const struct ngpu_pgcraft *s,
                                     const struct ngpu_pgcraft_texture *texture,
                                     struct ngpu_pgcraft_texture_info *info)
{
    for (size_t i = 0; i < NB_SAMPLER_DESCS; i++) {
        const struct sampler_desc *sd = &sampler_descs[i];
        const enum ngpu_type sampler_type = sd->types[texture->type];
        if (sampler_type == NGPU_TYPE_NONE || !is_type_supported(s, sampler_type))
            continue;
        if (!(is_sampler(sampler_type) || is_image(sampler_type)))
            continue;
        char name[NGPU_ID_LEN];
        int len = snprintf(name, sizeof(name), "%s%s", texture->name, sd->suffix);
        ngpu_assert(len < sizeof(name));
        int32_t *index_ptr = (int32_t *)((uint8_t *)info + sd->info_offset);
        *index_ptr = get_texture_index(s, name);
    }
}

static void probe_texture_infos(struct ngpu_pgcraft *s)
{
    for (size_t i = 0; i < s->texture_infos.count; i++) {
        const struct ngpu_pgcraft_texture *texture = &s->textures.data[i];
        struct ngpu_pgcraft_texture_info *info = &s->texture_infos.data[i];
        probe_texture_info_elems(s, texture, info);

        /* Resolve per-texture block index (single shared block) */
        info->block_index = -1;
        if (!texture->no_metadata) {
            char block_name[NGPU_ID_LEN];
            snprintf(block_name, sizeof(block_name), "%s_info", texture->name);
            for (int32_t j = 0; j < (int32_t)s->pipeline_info.desc.buffers.count; j++) {
                const char *entry_name = ngpu_pgcraft_get_symbol_name(s, s->pipeline_info.desc.buffers.data[j].id);
                if (!strcmp(entry_name, block_name)) {
                    info->block_index = j;
                    break;
                }
            }
        }
    }
}

static int probe_pipeline_elems(struct ngpu_pgcraft *s)
{
    probe_texture_infos(s);

    return 0;
}

#if defined(BACKEND_GL) || defined(BACKEND_GLES)

#define IS_GLSL_ES_MIN(min) (ctx_params->backend == NGPU_BACKEND_OPENGLES && s->glsl_version >= (min))
#define IS_GLSL_MIN(min)    (ctx_params->backend == NGPU_BACKEND_OPENGL   && s->glsl_version >= (min))

static void setup_glsl_info_gl(struct ngpu_pgcraft *s)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;
    const struct ngpu_ctx_params *ctx_params = &gpu_ctx->params;
    const struct ngpu_ctx_gl *gpu_ctx_gl = (struct ngpu_ctx_gl *)gpu_ctx;
    const struct glcontext *gl = gpu_ctx_gl->glcontext;

    s->sym_vertex_index   = "gl_VertexID";
    s->sym_instance_index = "gl_InstanceID";

    s->glsl_version = gpu_ctx->language_version;

    if (ctx_params->backend == NGPU_BACKEND_OPENGLES)
        s->glsl_version_suffix = " es";

    s->has_in_out_layout_qualifiers = IS_GLSL_ES_MIN(310) || IS_GLSL_MIN(410);
    s->has_precision_qualifiers     = IS_GLSL_ES_MIN(100);

    s->has_explicit_bindings = IS_GLSL_ES_MIN(310) || IS_GLSL_MIN(420) ||
                               (gl->features & NGPU_FEATURE_GL_SHADING_LANGUAGE_420PACK);

    /*
     * Bindings are shared across all stages. UBO, SSBO, texture and image
     * bindings use distinct binding points.
     */
    s->next_bindings[NGPU_BINDING_TYPE_UBO]     = &s->bindings[NGPU_BINDING_TYPE_UBO];
    s->next_bindings[NGPU_BINDING_TYPE_SSBO]    = &s->bindings[NGPU_BINDING_TYPE_SSBO];
    s->next_bindings[NGPU_BINDING_TYPE_TEXTURE] = &s->bindings[NGPU_BINDING_TYPE_TEXTURE];
    s->next_bindings[NGPU_BINDING_TYPE_IMAGE]   = &s->bindings[NGPU_BINDING_TYPE_IMAGE];
}
#endif

#if defined(BACKEND_VK)
static void setup_glsl_info_vk(struct ngpu_pgcraft *s)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;

    s->glsl_version = gpu_ctx->language_version;

    s->sym_vertex_index   = "gl_VertexIndex";
    s->sym_instance_index = "gl_InstanceIndex";

    s->has_explicit_bindings        = true;
    s->has_in_out_layout_qualifiers = true;
    s->has_precision_qualifiers     = false;

    /* Bindings are shared across stages and types */
    for (size_t i = 0; i < NGPU_BINDING_TYPE_NB; i++)
        s->next_bindings[i] = &s->bindings[0];
}
#endif

static void setup_glsl_info(struct ngpu_pgcraft *s)
{
    struct ngpu_ctx *gpu_ctx = s->gpu_ctx;
    ngpu_unused const struct ngpu_ctx_params *ctx_params = &gpu_ctx->params;

    s->glsl_version_suffix = "";

#if defined(BACKEND_GL) || defined(BACKEND_GLES)
    if (ctx_params->backend == NGPU_BACKEND_OPENGL || ctx_params->backend == NGPU_BACKEND_OPENGLES) {
        setup_glsl_info_gl(s);
        return;
    }
#endif

#if defined(BACKEND_VK)
    if (ctx_params->backend == NGPU_BACKEND_VULKAN) {
        setup_glsl_info_vk(s);
        return;
    }
#endif

    ngpu_assert(0);
}

struct ngpu_pgcraft *ngpu_pgcraft_create(struct ngpu_ctx *gpu_ctx)
{
    struct ngpu_pgcraft *s = ngpu_calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->gpu_ctx = gpu_ctx;

    setup_glsl_info(s);

    return s;
}

static int alloc_shader(struct ngpu_pgcraft *s, enum ngpu_program_stage stage)
{
    ngpu_assert(!s->shaders[stage]);
    struct bstr *b = ngpu_bstr_create();
    if (!b)
        return NGPU_ERROR_MEMORY;
    s->shaders[stage] = b;
    return 0;
}

static int get_program_compute(struct ngpu_pgcraft *s, const struct ngpu_pgcraft_params *params)
{
    int ret;

    if ((ret = alloc_shader(s, NGPU_PROGRAM_STAGE_COMP)) < 0 ||
        (ret = prepare_texture_infos(s, params, 0)) < 0 ||
        (ret = craft_comp(s, params)) < 0)
        return ret;

    const struct ngpu_program_params program_params = {
        .label   = params->program_label,
        .compute = ngpu_bstr_strptr(s->shaders[NGPU_PROGRAM_STAGE_COMP]),
    };
    ret = ngpu_pgcache_get_compute_program(s->gpu_ctx->program_cache, &s->program, &program_params);
    ngpu_bstr_freep(&s->shaders[NGPU_PROGRAM_STAGE_COMP]);
    return ret;
}

static int get_program_graphics(struct ngpu_pgcraft *s, const struct ngpu_pgcraft_params *params)
{
    int ret;

    for (size_t i = 0; i < params->nb_vert_out_vars; i++) {
        if (ngpu_darray_push(&s->vert_out_vars, params->vert_out_vars[i]) < 0)
            return NGPU_ERROR_MEMORY;
    }

    if ((ret = alloc_shader(s, NGPU_PROGRAM_STAGE_VERT)) < 0 ||
        (ret = alloc_shader(s, NGPU_PROGRAM_STAGE_FRAG)) < 0 ||
        (ret = prepare_texture_infos(s, params, 1)) < 0 ||
        (ret = craft_vert(s, params)) < 0 ||
        (ret = craft_frag(s, params)) < 0)
        return ret;

    const struct ngpu_program_params program_params = {
        .label    = params->program_label,
        .vertex   = ngpu_bstr_strptr(s->shaders[NGPU_PROGRAM_STAGE_VERT]),
        .fragment = ngpu_bstr_strptr(s->shaders[NGPU_PROGRAM_STAGE_FRAG]),
    };
    ret = ngpu_pgcache_get_graphics_program(s->gpu_ctx->program_cache, &s->program, &program_params);
    ngpu_bstr_freep(&s->shaders[NGPU_PROGRAM_STAGE_VERT]);
    ngpu_bstr_freep(&s->shaders[NGPU_PROGRAM_STAGE_FRAG]);
    return ret;
}

int ngpu_pgcraft_craft(struct ngpu_pgcraft *s, const struct ngpu_pgcraft_params *params)
{
    int ret = params->comp_base ? get_program_compute(s, params)
                                : get_program_graphics(s, params);
    if (ret < 0)
        return ret;

    ret = probe_pipeline_elems(s);
    if (ret < 0)
        return ret;

#if defined(BACKEND_GL) || defined(BACKEND_GLES)
    struct ngpu_ctx *gpu_ctx  = s->gpu_ctx;
    struct ngpu_ctx_params *ctx_params = &gpu_ctx->params;
    if (ctx_params->backend == NGPU_BACKEND_OPENGL ||
        ctx_params->backend == NGPU_BACKEND_OPENGLES) {
        if (!s->has_explicit_bindings) {
            /* Force locations and bindings for contexts that do not support
             * explicit locations and bindings */
            ret = ngpu_program_gl_set_locations_and_bindings(s->program, s);
            if (ret < 0)
                return ret;
        }
    }
#endif

    return 0;
}


int32_t ngpu_pgcraft_get_block_index(const struct ngpu_pgcraft *s, const char *name, enum ngpu_program_stage stage)
{
    for (int32_t i = 0; i < (int32_t)s->pipeline_info.desc.buffers.count; i++) {
        const struct ngpu_bindgroup_layout_entry *entry = &s->pipeline_info.desc.buffers.data[i];
        const char *desc_name = ngpu_pgcraft_get_symbol_name(s, entry->id);
        if (!strcmp(desc_name, name) && entry->stage_flags == (1U << stage))
            return i;
    }
    return -1;
}

int32_t ngpu_pgcraft_get_image_index(const struct ngpu_pgcraft *s, const char *name)
{
    for (int32_t i = 0; i < (int32_t)s->textures.count; i++) {
        if (!strcmp(s->textures.data[i].name, name))
            return i;
    }
    return -1;
}

struct ngpu_pgcraft_texture_infos ngpu_pgcraft_get_texture_infos(const struct ngpu_pgcraft *s)
{
    return (struct ngpu_pgcraft_texture_infos){
        .infos    = s->texture_infos.data,
        .nb_infos = s->texture_infos.count,
    };
}

struct ngpu_program *ngpu_pgcraft_get_program(const struct ngpu_pgcraft *s)
{
    return s->program;
}

struct ngpu_vertex_state ngpu_pgcraft_get_vertex_state(const struct ngpu_pgcraft *s)
{
    return (const struct ngpu_vertex_state) {
        .buffers    = s->pipeline_info.desc.vertex_buffers.data,
        .nb_buffers = s->pipeline_info.desc.vertex_buffers.count,
    };
}

struct ngpu_vertex_resources ngpu_pgcraft_get_vertex_resources(const struct ngpu_pgcraft *s)
{
    const struct ngpu_vertex_resources resources = {
        .vertex_buffers    = s->pipeline_info.data.vertex_buffers.data,
        .nb_vertex_buffers = s->pipeline_info.data.vertex_buffers.count,
    };
    return resources;
}

int32_t ngpu_pgcraft_get_vertex_buffer_index(const struct ngpu_pgcraft *s, const char *name)
{
    for (int32_t i = 0; i < (int32_t)s->pipeline_info.desc.vertex_buffers.count; i++) {
        struct ngpu_vertex_buffer_layout *layout = &s->pipeline_info.desc.vertex_buffers.data[i];
        for (size_t j = 0; j < layout->nb_attributes; j++) {
            struct ngpu_vertex_attribute *attribute = &layout->attributes[j];
            const char *attribute_name = ngpu_pgcraft_get_symbol_name(s, attribute->id);
            if (!strcmp(attribute_name, name))
                return i;
        }
    }
    return -1;
}

const char *ngpu_pgcraft_get_symbol_name(const struct ngpu_pgcraft *s, size_t id)
{
    return s->symbols.data[id].name;
}

struct ngpu_bindgroup_layout_desc ngpu_pgcraft_get_bindgroup_layout_desc(const struct ngpu_pgcraft *s)
{
    const struct ngpu_bindgroup_layout_desc bindgroup_layout_params = {
        .textures    = s->pipeline_info.desc.textures.data,
        .nb_textures = s->pipeline_info.desc.textures.count,
        .buffers     = s->pipeline_info.desc.buffers.data,
        .nb_buffers  = s->pipeline_info.desc.buffers.count,
    };
    return bindgroup_layout_params;
}

struct ngpu_bindgroup_resources ngpu_pgcraft_get_bindgroup_resources(const struct ngpu_pgcraft *s)
{
    const struct ngpu_bindgroup_resources resources = {
        .textures          = s->pipeline_info.data.textures.data,
        .nb_textures       = s->pipeline_info.data.textures.count,
        .buffers           = s->pipeline_info.data.buffers.data,
        .nb_buffers        = s->pipeline_info.data.buffers.count,
    };
    return resources;
}

void ngpu_pgcraft_freep(struct ngpu_pgcraft **sp)
{
    struct ngpu_pgcraft *s = *sp;
    if (!s)
        return;

    ngpu_darray_reset(&s->textures);
    ngpu_darray_reset(&s->texture_infos);
    ngpu_darray_reset(&s->vert_out_vars);

    for (size_t i = 0; i < NGPU_ARRAY_NB(s->shaders); i++)
        ngpu_bstr_freep(&s->shaders[i]);

    ngpu_darray_reset(&s->symbols);

    ngpu_darray_reset(&s->pipeline_info.desc.textures);
    ngpu_darray_reset(&s->pipeline_info.desc.buffers);
    ngpu_darray_reset(&s->pipeline_info.desc.vertex_buffers);

    ngpu_darray_reset(&s->pipeline_info.data.textures);
    ngpu_darray_reset(&s->pipeline_info.data.buffers);
    ngpu_darray_reset(&s->pipeline_info.data.vertex_buffers);

    ngpu_freep(sp);
}
