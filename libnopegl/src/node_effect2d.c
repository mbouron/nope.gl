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

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "blending.h"
#include "geometry.h"
#include "image.h"
#include "internal.h"
#include "log.h"
#include "math_utils.h"
#include <ngpu/ngpu.h>
#include "node_uniform.h"
#include "nopegl/nopegl.h"
#include "pipeline_compat.h"
#include "rtt.h"
#include "node_block.h"
#include "node_texture.h"
#include "utils/bits.h"
#include "utils/bstr.h"
#include "utils/darray.h"
#include "utils/memory.h"
#include "utils/utils.h"

/* GLSL fragments as string */
#include "effect2d_composite_frag.h"
#include "effect2d_composite_vert.h"

/* Blur shaders (shared with FastGaussianBlur) */
#include "blur_common_vert.h"
#include "blur_downsample_frag.h"
#include "blur_upsample_frag.h"
#include "blur_interpolate_frag.h"

#define MAX_MIP_LEVELS 16

struct down_up_data_block {
    float offset;
};

struct interpolate_block {
    float lod;
};

#define MAX_FRAG_UNIFORMS  16
#define MAX_FRAG_TEXTURES  8
#define MAX_FRAG_BLOCKS    8

struct frag_uniform_def {
    char name[64];
    enum ngpu_type type;
    const struct ngl_node *node;
};

struct frag_texture_def {
    char name[64];
    struct ngl_node *texture_node;
};

struct frag_block_def {
    char name[64];
    struct ngl_node *node;
};

struct effect2d_opts {
    struct ngl_node **children;
    size_t nb_children;
    struct ngl_node *blur_node;
    float blur;
    const char *fragment;
    const char *glsl_header;
    struct ngl_node **frag_resources;
    size_t nb_frag_resources;
    enum ngli_blending blending;
    int visible;
    struct ngl_node *translate_node;
    float translate[2];
    struct ngl_node *rotation_node;
    float rotation;
    struct ngl_node *scale_node;
    float scale[2];
    struct ngl_node *anchor_node;
    float anchor[2];
    struct ngl_node *opacity_node;
    float opacity;
};

struct effect2d_priv {
    struct draw_info draw_info;
    struct darray indices;

    /* Children rendering FBO */
    struct rtt_ctx *children_rtt;
    struct ngpu_rendertarget_layout children_layout;
    uint32_t fbo_width;
    uint32_t fbo_height;

    /* Blur (internalized FastGaussianBlur) */
    struct ngpu_rendertarget_layout blur_mip_layout;
    struct ngpu_block blur_down_up_block;
    struct rtt_ctx *blur_mip;
    struct rtt_ctx *blur_mips[MAX_MIP_LEVELS];
    struct rtt_ctx *blur_dst_rtt;
    uint32_t blur_max_lod;
    struct {
        struct ngpu_pgcraft *crafter;
        struct pipeline_compat *pl;
    } blur_dws, blur_ups;
    struct {
        struct ngpu_block block;
        struct ngpu_pgcraft *crafter;
        struct pipeline_compat *pl;
    } blur_interp;

    /* Custom fragment pass */
    char *frag_glsl;
    struct rtt_ctx *frag_rtt;
    struct ngpu_pgcraft *frag_crafter;
    struct pipeline_compat *frag_pl;
    struct frag_uniform_def frag_uniforms[MAX_FRAG_UNIFORMS];
    size_t nb_frag_uniforms;
    struct frag_texture_def frag_textures[MAX_FRAG_TEXTURES];
    size_t nb_frag_textures;
    struct frag_block_def frag_blocks[MAX_FRAG_BLOCKS];
    size_t nb_frag_blocks;
    struct darray frag_uniform_indices; /* int32_t */

    /* Composite quad */
    struct geometry *quad_geometry;
    struct ngpu_pgcraft *crafter;
    struct pipeline_compat *composite_pl;
    int32_t modelview_matrix_index;
    int32_t projection_matrix_index;
    int32_t opacity_index;
};

static int effect2d_swap_children(struct ngl_node *node, size_t from, size_t to)
{
    struct effect2d_priv *s = node->priv_data;

    size_t *indices = ngli_darray_data(&s->indices);
    NGLI_SWAP(size_t, indices[from], indices[to]);

    return 0;
}

#define OFFSET(x) offsetof(struct effect2d_opts, x)
static const struct node_param effect2d_params[] = {
    {
        .key       = "children",
        .type      = NGLI_PARAM_TYPE_NODELIST,
        .offset    = OFFSET(children),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .swap_func = effect2d_swap_children,
        .desc      = NGLI_DOCSTRING("2D scenes to render offscreen"),
    }, {
        .key       = "blur",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(blur_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("blur amount in the range [0,1] (0 means no blur)"),
    }, {
        .key       = "fragment",
        .type      = NGLI_PARAM_TYPE_STR,
        .offset    = OFFSET(fragment),
        .desc      = NGLI_DOCSTRING("custom fragment shader body; receives color (vec4) and uv (vec2), must return vec4"),
    }, {
        .key       = "glsl_header",
        .type      = NGLI_PARAM_TYPE_STR,
        .offset    = OFFSET(glsl_header),
        .desc      = NGLI_DOCSTRING("optional GLSL helper functions prepended before the fragment body"),
    }, {
        .key        = "frag_resources",
        .type       = NGLI_PARAM_TYPE_NODELIST,
        .offset     = OFFSET(frag_resources),
        .node_types = (const uint32_t[]){
            NGL_NODE_UNIFORMFLOAT,  NGL_NODE_UNIFORMVEC2,  NGL_NODE_UNIFORMVEC3,
            NGL_NODE_UNIFORMVEC4,   NGL_NODE_UNIFORMCOLOR, NGL_NODE_UNIFORMQUAT,
            NGL_NODE_UNIFORMMAT4,   NGL_NODE_UNIFORMINT,   NGL_NODE_UNIFORMIVEC2,
            NGL_NODE_UNIFORMIVEC3,  NGL_NODE_UNIFORMIVEC4, NGL_NODE_UNIFORMUINT,
            NGL_NODE_UNIFORMUIVEC2, NGL_NODE_UNIFORMUIVEC3,NGL_NODE_UNIFORMUIVEC4,
            NGL_NODE_UNIFORMBOOL,
            NGL_NODE_EVALFLOAT,    NGL_NODE_EVALVEC2,
            NGL_NODE_EVALVEC3,     NGL_NODE_EVALVEC4,
            NGL_NODE_ANIMATEDFLOAT,NGL_NODE_ANIMATEDVEC2,
            NGL_NODE_ANIMATEDVEC3, NGL_NODE_ANIMATEDVEC4,
            NGL_NODE_ANIMATEDQUAT, NGL_NODE_ANIMATEDCOLOR,
            NGL_NODE_NOISEFLOAT,   NGL_NODE_NOISEVEC2,
            NGL_NODE_NOISEVEC3,    NGL_NODE_NOISEVEC4,
            NGL_NODE_TIME,
            NGL_NODE_TEXTURE2D,    NGL_NODE_TEXTURE2DARRAY,
            NGL_NODE_TEXTURE3D,    NGL_NODE_TEXTURECUBE,
            NGL_NODE_TEXTUREVIEW,  NGL_NODE_CUSTOMTEXTURE,
            NGL_NODE_BLOCK,
            NGLI_NODE_NONE,
        },
        .desc = NGLI_DOCSTRING("uniform and texture nodes available to fragment; "
                               "each node's label is used as the GLSL name"),
    }, {
        .key       = "blending",
        .type      = NGLI_PARAM_TYPE_SELECT,
        .offset    = OFFSET(blending),
        .def_value = {.i32=NGLI_BLENDING_SRC_OVER},
        .choices   = &ngli_blending_choices,
        .desc      = NGLI_DOCSTRING("blending mode for the final composite"),
    }, {
        .key       = "visible",
        .type      = NGLI_PARAM_TYPE_BOOL,
        .offset    = OFFSET(visible),
        .def_value = {.i32=1},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE,
        .desc      = NGLI_DOCSTRING("whether the effect and its children are visible"),
    }, {
        .key       = "translate",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(translate_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("translation in pixels"),
    }, {
        .key       = "rotation",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(rotation_node),
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("rotation angle in degrees"),
    }, {
        .key       = "scale",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(scale_node),
        .def_value = {.vec={1.f, 1.f}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("scale factors"),
    }, {
        .key       = "anchor",
        .type      = NGLI_PARAM_TYPE_VEC2,
        .offset    = OFFSET(anchor_node),
        .def_value = {.vec={NAN, NAN}},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("anchor point in pixels (default: center of children)"),
    }, {
        .key       = "opacity",
        .type      = NGLI_PARAM_TYPE_F32,
        .offset    = OFFSET(opacity_node),
        .def_value = {.f32=1.f},
        .flags     = NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE | NGLI_PARAM_FLAG_ALLOW_NODE,
        .desc      = NGLI_DOCSTRING("opacity of the composited result"),
    },
    {NULL}
};

static const float quad_vertices[] = {
   -1.f,-1.f, 0.f,
    1.f,-1.f, 0.f,
   -1.f, 1.f, 0.f,
    1.f, 1.f, 0.f,
};

static const float quad_uvcoords[] = {
    0.f, 1.f,
    1.f, 1.f,
    0.f, 0.f,
    1.f, 0.f,
};

static int node_is_texture(const struct ngl_node *node)
{
    return node->cls->id == NGL_NODE_TEXTURE2D ||
           node->cls->id == NGL_NODE_TEXTURE2DARRAY ||
           node->cls->id == NGL_NODE_TEXTURE3D ||
           node->cls->id == NGL_NODE_TEXTURECUBE ||
           node->cls->id == NGL_NODE_TEXTUREVIEW ||
           node->cls->id == NGL_NODE_CUSTOMTEXTURE;
}

static int node_to_ngpu_type(const struct ngl_node *node, enum ngpu_type *type)
{
    switch (node->cls->id) {
    case NGL_NODE_UNIFORMFLOAT:
    case NGL_NODE_EVALFLOAT:
    case NGL_NODE_ANIMATEDFLOAT:
    case NGL_NODE_NOISEFLOAT:
    case NGL_NODE_TIME:           *type = NGPU_TYPE_F32;   return 0;
    case NGL_NODE_UNIFORMVEC2:
    case NGL_NODE_EVALVEC2:
    case NGL_NODE_ANIMATEDVEC2:
    case NGL_NODE_NOISEVEC2:      *type = NGPU_TYPE_VEC2;  return 0;
    case NGL_NODE_UNIFORMVEC3:
    case NGL_NODE_EVALVEC3:
    case NGL_NODE_ANIMATEDVEC3:
    case NGL_NODE_NOISEVEC3:      *type = NGPU_TYPE_VEC3;  return 0;
    case NGL_NODE_UNIFORMVEC4:
    case NGL_NODE_UNIFORMCOLOR:
    case NGL_NODE_UNIFORMQUAT:
    case NGL_NODE_EVALVEC4:
    case NGL_NODE_ANIMATEDVEC4:
    case NGL_NODE_ANIMATEDQUAT:
    case NGL_NODE_ANIMATEDCOLOR:
    case NGL_NODE_NOISEVEC4:      *type = NGPU_TYPE_VEC4;  return 0;
    case NGL_NODE_UNIFORMMAT4:    *type = NGPU_TYPE_MAT4;  return 0;
    case NGL_NODE_UNIFORMINT:     *type = NGPU_TYPE_I32;   return 0;
    case NGL_NODE_UNIFORMIVEC2:   *type = NGPU_TYPE_IVEC2; return 0;
    case NGL_NODE_UNIFORMIVEC3:   *type = NGPU_TYPE_IVEC3; return 0;
    case NGL_NODE_UNIFORMIVEC4:   *type = NGPU_TYPE_IVEC4; return 0;
    case NGL_NODE_UNIFORMUINT:    *type = NGPU_TYPE_U32;   return 0;
    case NGL_NODE_UNIFORMUIVEC2:  *type = NGPU_TYPE_UVEC2; return 0;
    case NGL_NODE_UNIFORMUIVEC3:  *type = NGPU_TYPE_UVEC3; return 0;
    case NGL_NODE_UNIFORMUIVEC4:  *type = NGPU_TYPE_UVEC4; return 0;
    case NGL_NODE_UNIFORMBOOL:    *type = NGPU_TYPE_BOOL;  return 0;
    default:
        LOG(ERROR, "unsupported resource node type \"%s\"", node->cls->name);
        return NGL_ERROR_INVALID_ARG;
    }
}

static const void *frag_node_get_data(const struct ngl_node *node)
{
    const struct variable_info *var = node->priv_data;
    return var->data;
}

/*
 * Blur pipeline setup (internalized from node_fgblur.c)
 */
static int setup_blur_down_up_pipeline(struct ngpu_pgcraft *crafter,
                                       const char *name,
                                       const char *frag_base,
                                       struct pipeline_compat *pipeline,
                                       const struct ngpu_rendertarget_layout *layout,
                                       struct ngpu_block *block)
{
    const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "tex_coord", .type = NGPU_TYPE_VEC2},
    };

    const struct ngpu_pgcraft_texture textures[] = {
        {
            .name      = "tex",
            .type      = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
            .precision = NGPU_PRECISION_HIGH,
            .stage     = NGPU_PROGRAM_STAGE_FRAG,
        },
    };

    const struct ngpu_pgcraft_block blocks[] = {
        {
            .name          = "data",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &block->block_desc,
            .buffer        = {
                .buffer    = block->buffer,
                .size      = block->block_size,
            },
        }
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = name,
        .vert_base        = blur_common_vert,
        .frag_base        = frag_base,
        .textures         = textures,
        .nb_textures      = NGLI_ARRAY_NB(textures),
        .blocks           = blocks,
        .nb_blocks        = NGLI_ARRAY_NB(blocks),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };

    int ret = ngpu_pgcraft_craft(crafter, &crafter_params);
    if (ret < 0)
        return ret;

    const struct pipeline_compat_params params = {
        .type         = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics     = {
            .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .state        = NGPU_GRAPHICS_STATE_DEFAULTS,
            .rt_layout    = *layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(crafter),
        },
        .program          = ngpu_pgcraft_get_program(crafter),
        .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(crafter),
        .resources        = ngpu_pgcraft_get_bindgroup_resources(crafter),
        .vertex_resources = ngpu_pgcraft_get_vertex_resources(crafter),
        .compat_info      = ngpu_pgcraft_get_compat_info(crafter),
    };

    return ngli_pipeline_compat_init(pipeline, &params);
}

static int setup_blur_interpolate_pipeline(struct effect2d_priv *s, struct ngpu_ctx *gpu_ctx)
{
    const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "tex_coord", .type = NGPU_TYPE_VEC2},
    };

    struct ngpu_pgcraft_texture textures[] = {
        {.name = "tex0", .type = NGPU_PGCRAFT_TEXTURE_TYPE_2D, .precision = NGPU_PRECISION_HIGH, .stage = NGPU_PROGRAM_STAGE_FRAG},
        {.name = "tex1", .type = NGPU_PGCRAFT_TEXTURE_TYPE_2D, .precision = NGPU_PRECISION_HIGH, .stage = NGPU_PROGRAM_STAGE_FRAG},
    };

    const struct ngpu_block_entry fields[] = {
        NGPU_BLOCK_FIELD(struct interpolate_block, lod, NGPU_TYPE_F32, 0),
    };
    const struct ngpu_block_params block_params = {
        .entries    = fields,
        .nb_entries = NGLI_ARRAY_NB(fields),
    };
    int ret = ngpu_block_init(gpu_ctx, &s->blur_interp.block, &block_params);
    if (ret < 0)
        return ret;

    const struct ngpu_pgcraft_block crafter_blocks[] = {
        {
            .name          = "interpolate",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_FRAG,
            .block         = &s->blur_interp.block.block_desc,
            .buffer        = {
                .buffer    = s->blur_interp.block.buffer,
                .size      = s->blur_interp.block.block_size,
            },
        }
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/effect2d-blur-interpolate",
        .vert_base        = blur_common_vert,
        .frag_base        = blur_interpolate_frag,
        .textures         = textures,
        .nb_textures      = NGLI_ARRAY_NB(textures),
        .blocks           = crafter_blocks,
        .nb_blocks        = NGLI_ARRAY_NB(crafter_blocks),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };

    ret = ngpu_pgcraft_craft(s->blur_interp.crafter, &crafter_params);
    if (ret < 0)
        return ret;

    const struct pipeline_compat_params params = {
        .type         = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics     = {
            .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .state        = NGPU_GRAPHICS_STATE_DEFAULTS,
            .rt_layout    = s->blur_mip_layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(s->blur_interp.crafter),
        },
        .program          = ngpu_pgcraft_get_program(s->blur_interp.crafter),
        .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(s->blur_interp.crafter),
        .resources        = ngpu_pgcraft_get_bindgroup_resources(s->blur_interp.crafter),
        .vertex_resources = ngpu_pgcraft_get_vertex_resources(s->blur_interp.crafter),
        .compat_info      = ngpu_pgcraft_get_compat_info(s->blur_interp.crafter),
    };

    return ngli_pipeline_compat_init(s->blur_interp.pl, &params);
}

static int init_blur_pipelines(struct effect2d_priv *s, struct ngpu_ctx *gpu_ctx)
{
    s->blur_mip_layout.nb_colors = 1;
    s->blur_mip_layout.colors[0].format = NGPU_FORMAT_R8G8B8A8_UNORM;

    const struct ngpu_block_entry down_up_fields[] = {
        NGPU_BLOCK_FIELD(struct down_up_data_block, offset, NGPU_TYPE_F32, 0),
    };
    const struct ngpu_block_params down_up_params = {
        .entries    = down_up_fields,
        .nb_entries = NGLI_ARRAY_NB(down_up_fields),
    };
    int ret = ngpu_block_init(gpu_ctx, &s->blur_down_up_block, &down_up_params);
    if (ret < 0)
        return ret;

    ret = ngpu_block_update(&s->blur_down_up_block, 0, &(struct down_up_data_block){.offset=1.f});
    if (ret < 0)
        return ret;

    s->blur_dws.crafter = ngpu_pgcraft_create(gpu_ctx);
    s->blur_ups.crafter = ngpu_pgcraft_create(gpu_ctx);
    s->blur_interp.crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!s->blur_dws.crafter || !s->blur_ups.crafter || !s->blur_interp.crafter)
        return NGL_ERROR_MEMORY;

    s->blur_dws.pl = ngli_pipeline_compat_create(gpu_ctx);
    s->blur_ups.pl = ngli_pipeline_compat_create(gpu_ctx);
    s->blur_interp.pl = ngli_pipeline_compat_create(gpu_ctx);
    if (!s->blur_dws.pl || !s->blur_ups.pl || !s->blur_interp.pl)
        return NGL_ERROR_MEMORY;

    if ((ret = setup_blur_down_up_pipeline(s->blur_dws.crafter, "nopegl/effect2d-blur-downsample",
                                           blur_downsample_frag, s->blur_dws.pl,
                                           &s->blur_mip_layout, &s->blur_down_up_block)) < 0 ||
        (ret = setup_blur_down_up_pipeline(s->blur_ups.crafter, "nopegl/effect2d-blur-upsample",
                                           blur_upsample_frag, s->blur_ups.pl,
                                           &s->blur_mip_layout, &s->blur_down_up_block)) < 0)
        return ret;

    return setup_blur_interpolate_pipeline(s, gpu_ctx);
}

static int effect2d_init(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct effect2d_priv *s = node->priv_data;

    ngli_darray_init(&s->indices, sizeof(size_t), 0);

    /* Set up children FBO layout (RGBA8, no depth) */
    s->children_layout.nb_colors = 1;
    s->children_layout.colors[0].format = NGPU_FORMAT_R8G8B8A8_UNORM;

    /* Initialize blur pipelines */
    int ret = init_blur_pipelines(s, gpu_ctx);
    if (ret < 0)
        return ret;

    /* Initialize custom fragment pipeline if fragment is specified */
    const struct effect2d_opts *o = node->opts;
    if (o->fragment && o->fragment[0]) {
        /* Validate resources: all must have labels */
        for (size_t i = 0; i < o->nb_frag_resources; i++) {
            const struct ngl_node *res = o->frag_resources[i];
            if (!res->label || !res->label[0]) {
                LOG(ERROR, "frag_resources[%zu]: node label is required as GLSL name", i);
                return NGL_ERROR_INVALID_USAGE;
            }
        }

        /* Build fragment shader dynamically */
        struct bstr *bstr = ngli_bstr_create();
        if (!bstr)
            return NGL_ERROR_MEMORY;

        if (o->glsl_header && o->glsl_header[0])
            ngli_bstr_printf(bstr, "%s\n", o->glsl_header);
        ngli_bstr_printf(bstr, "vec4 ngl_effect(vec4 color, vec2 uv) {\n%s\n}\n", o->fragment);
        ngli_bstr_printf(bstr, "void main() {\n");
        ngli_bstr_printf(bstr, "    vec4 color = texture(tex, tex_coord);\n");
        ngli_bstr_printf(bstr, "    ngl_out_color = ngl_effect(color, tex_coord);\n");
        ngli_bstr_printf(bstr, "}\n");

        s->frag_glsl = ngli_bstr_strdup(bstr);
        ngli_bstr_freep(&bstr);
        if (!s->frag_glsl)
            return NGL_ERROR_MEMORY;

        /* Parse frag_resources */
        ngli_darray_init(&s->frag_uniform_indices, sizeof(int32_t), 0);

        struct darray uniforms_arr;
        struct darray textures_arr;
        ngli_darray_init(&uniforms_arr, sizeof(struct ngpu_pgcraft_uniform), 0);
        ngli_darray_init(&textures_arr, sizeof(struct ngpu_pgcraft_texture), 0);

        /* The source texture (from children FBO or blur output) */
        const struct ngpu_pgcraft_texture src_tex = {
            .name  = "tex",
            .type  = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
            .stage = NGPU_PROGRAM_STAGE_FRAG,
        };
        if (!ngli_darray_push(&textures_arr, &src_tex)) {
            ret = NGL_ERROR_MEMORY;
            goto frag_fail;
        }

        for (size_t i = 0; i < o->nb_frag_resources; i++) {
            struct ngl_node *res = o->frag_resources[i];
            enum ngpu_type type;

            if (node_is_texture(res)) {
                ngli_assert(s->nb_frag_textures < MAX_FRAG_TEXTURES);
                struct frag_texture_def *ct = &s->frag_textures[s->nb_frag_textures++];
                snprintf(ct->name, sizeof(ct->name), "%s", res->label);
                ct->texture_node = res;

                struct texture_info *texture_info = res->priv_data;
                struct ngpu_pgcraft_texture tex = {
                    .type        = ngli_node_texture_get_pgcraft_texture_type(res),
                    .stage       = NGPU_PROGRAM_STAGE_FRAG,
                    .image       = &texture_info->image,
                    .format      = texture_info->params.format,
                    .clamp_video = texture_info->clamp_video,
                    .premult     = texture_info->premult,
                };
                snprintf(tex.name, sizeof(tex.name), "%s", res->label);
                if (!ngli_darray_push(&textures_arr, &tex)) {
                    ret = NGL_ERROR_MEMORY;
                    goto frag_fail;
                }
                continue;
            }

            if (res->cls->id == NGL_NODE_BLOCK) {
                ngli_assert(s->nb_frag_blocks < MAX_FRAG_BLOCKS);
                struct frag_block_def *cb = &s->frag_blocks[s->nb_frag_blocks++];
                snprintf(cb->name, sizeof(cb->name), "%s", res->label);
                cb->node = res;
                continue;
            }

            ret = node_to_ngpu_type(res, &type);
            if (ret < 0)
                goto frag_fail;
            ngli_assert(s->nb_frag_uniforms < MAX_FRAG_UNIFORMS);
            struct frag_uniform_def *cu = &s->frag_uniforms[s->nb_frag_uniforms++];
            snprintf(cu->name, sizeof(cu->name), "%s", res->label);
            cu->type = type;
            cu->node = res;

            struct ngpu_pgcraft_uniform u = {.type=type, .stage=NGPU_PROGRAM_STAGE_FRAG};
            snprintf(u.name, sizeof(u.name), "%s", res->label);
            if (!ngli_darray_push(&uniforms_arr, &u)) {
                ret = NGL_ERROR_MEMORY;
                goto frag_fail;
            }
        }

        /* Create fragment pipeline */
        const struct ngpu_pgcraft_iovar frag_vert_out_vars[] = {
            {.name = "tex_coord", .type = NGPU_TYPE_VEC2},
        };

        const struct ngpu_pgcraft_params frag_crafter_params = {
            .program_label    = "nopegl/effect2d-fragment",
            .vert_base        = blur_common_vert,
            .frag_base        = s->frag_glsl,
            .uniforms         = ngli_darray_data(&uniforms_arr),
            .nb_uniforms      = ngli_darray_count(&uniforms_arr),
            .textures         = ngli_darray_data(&textures_arr),
            .nb_textures      = ngli_darray_count(&textures_arr),
            .vert_out_vars    = frag_vert_out_vars,
            .nb_vert_out_vars = NGLI_ARRAY_NB(frag_vert_out_vars),
        };

        s->frag_crafter = ngpu_pgcraft_create(gpu_ctx);
        if (!s->frag_crafter) {
            ret = NGL_ERROR_MEMORY;
            goto frag_fail;
        }

        ret = ngpu_pgcraft_craft(s->frag_crafter, &frag_crafter_params);
        if (ret < 0)
            goto frag_fail;

        s->frag_pl = ngli_pipeline_compat_create(gpu_ctx);
        if (!s->frag_pl) {
            ret = NGL_ERROR_MEMORY;
            goto frag_fail;
        }

        const struct pipeline_compat_params frag_params = {
            .type = NGPU_PIPELINE_TYPE_GRAPHICS,
            .graphics = {
                .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                .state        = NGPU_GRAPHICS_STATE_DEFAULTS,
                .rt_layout    = s->children_layout,
                .vertex_state = ngpu_pgcraft_get_vertex_state(s->frag_crafter),
            },
            .program          = ngpu_pgcraft_get_program(s->frag_crafter),
            .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(s->frag_crafter),
            .resources        = ngpu_pgcraft_get_bindgroup_resources(s->frag_crafter),
            .vertex_resources = ngpu_pgcraft_get_vertex_resources(s->frag_crafter),
            .compat_info      = ngpu_pgcraft_get_compat_info(s->frag_crafter),
        };

        ret = ngli_pipeline_compat_init(s->frag_pl, &frag_params);
        if (ret < 0)
            goto frag_fail;

        /* Build uniform index map */
        for (size_t i = 0; i < s->nb_frag_uniforms; i++) {
            const struct frag_uniform_def *cu = &s->frag_uniforms[i];
            const int32_t idx = ngpu_pgcraft_get_uniform_index(s->frag_crafter, cu->name, NGPU_PROGRAM_STAGE_FRAG);
            if (!ngli_darray_push(&s->frag_uniform_indices, &idx)) {
                ret = NGL_ERROR_MEMORY;
                goto frag_fail;
            }
        }

        ngli_darray_reset(&uniforms_arr);
        ngli_darray_reset(&textures_arr);
        goto frag_done;

frag_fail:
        ngli_darray_reset(&uniforms_arr);
        ngli_darray_reset(&textures_arr);
        return ret;
frag_done:;
    }

    /* Create composite quad geometry */
    s->quad_geometry = ngli_geometry_create(gpu_ctx);
    if (!s->quad_geometry)
        return NGL_ERROR_MEMORY;

    if ((ret = ngli_geometry_set_vertices(s->quad_geometry, 4, quad_vertices)) < 0 ||
        (ret = ngli_geometry_set_uvcoords(s->quad_geometry, 4, quad_uvcoords)) < 0 ||
        (ret = ngli_geometry_init(s->quad_geometry, NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) < 0))
        return ret;

    /* Build composite shader pipeline */
    struct ngpu_pgcraft_attribute position_attr = {0};
    snprintf(position_attr.name, sizeof(position_attr.name), "position");
    position_attr.type   = NGPU_TYPE_VEC3;
    position_attr.format = NGPU_FORMAT_R32G32B32_SFLOAT;
    position_attr.stride = s->quad_geometry->vertices_layout.stride;
    position_attr.offset = s->quad_geometry->vertices_layout.offset;
    position_attr.buffer = s->quad_geometry->vertices_buffer;

    struct ngpu_pgcraft_attribute uvcoord_attr = {0};
    snprintf(uvcoord_attr.name, sizeof(uvcoord_attr.name), "uvcoord");
    uvcoord_attr.type   = NGPU_TYPE_VEC2;
    uvcoord_attr.format = NGPU_FORMAT_R32G32_SFLOAT;
    uvcoord_attr.stride = s->quad_geometry->uvcoords_layout.stride;
    uvcoord_attr.offset = s->quad_geometry->uvcoords_layout.offset;
    uvcoord_attr.buffer = s->quad_geometry->uvcoords_buffer;

    const struct ngpu_pgcraft_uniform uniforms[] = {
        {.name="modelview_matrix",  .type=NGPU_TYPE_MAT4, .stage=NGPU_PROGRAM_STAGE_VERT},
        {.name="projection_matrix", .type=NGPU_TYPE_MAT4, .stage=NGPU_PROGRAM_STAGE_VERT},
        {.name="opacity",           .type=NGPU_TYPE_F32,  .stage=NGPU_PROGRAM_STAGE_FRAG},
    };

    struct ngpu_pgcraft_texture textures[] = {
        {
            .name  = "tex",
            .type  = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
            .stage = NGPU_PROGRAM_STAGE_FRAG,
        },
    };

    static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
        {.name = "uv",        .type = NGPU_TYPE_VEC2},
        {.name = "tex_coord", .type = NGPU_TYPE_VEC2},
    };

    const struct ngpu_pgcraft_attribute attributes[] = {
        position_attr,
        uvcoord_attr,
    };

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/effect2d",
        .vert_base        = effect2d_composite_vert,
        .frag_base        = effect2d_composite_frag,
        .uniforms         = uniforms,
        .nb_uniforms      = NGLI_ARRAY_NB(uniforms),
        .textures         = textures,
        .nb_textures      = NGLI_ARRAY_NB(textures),
        .attributes       = attributes,
        .nb_attributes    = NGLI_ARRAY_NB(attributes),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };

    s->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!s->crafter)
        return NGL_ERROR_MEMORY;

    ret = ngpu_pgcraft_craft(s->crafter, &crafter_params);
    if (ret < 0)
        return ret;

    s->modelview_matrix_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "modelview_matrix", NGPU_PROGRAM_STAGE_VERT);
    s->projection_matrix_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "projection_matrix", NGPU_PROGRAM_STAGE_VERT);
    s->opacity_index = ngpu_pgcraft_get_uniform_index(
        s->crafter, "opacity", NGPU_PROGRAM_STAGE_FRAG);

    return 0;
}

static int effect2d_prepare(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct effect2d_priv *s = node->priv_data;
    const struct effect2d_opts *o = node->opts;
    struct rnode *rnode_pos = ctx->rnode_pos;

    /* Prepare children against the FBO rendertarget layout */
    struct ngpu_rendertarget_layout saved_layout = rnode_pos->rendertarget_layout;
    rnode_pos->rendertarget_layout = s->children_layout;

    for (size_t i = 0; i < o->nb_children; i++) {
        struct rnode *child_rnode = ngli_rnode_add_child(rnode_pos);
        if (!child_rnode)
            return NGL_ERROR_MEMORY;

        if (!ngli_darray_push(&s->indices, &i))
            return NGL_ERROR_MEMORY;

        struct ngl_node *child = o->children[i];

        ctx->rnode_pos = child_rnode;
        int ret = ngli_node_prepare(child);
        ctx->rnode_pos = rnode_pos;

        if (ret < 0)
            return ret;
    }

    /* Restore parent rendertarget layout */
    rnode_pos->rendertarget_layout = saved_layout;

    /* Create composite pipeline (single instance, not per-rnode) */
    if (!s->composite_pl) {
        struct ngpu_graphics_state state = rnode_pos->graphics_state;
        int ret = ngli_blending_apply_preset(&state, o->blending);
        if (ret < 0)
            return ret;

        s->composite_pl = ngli_pipeline_compat_create(gpu_ctx);
        if (!s->composite_pl)
            return NGL_ERROR_MEMORY;

        const struct pipeline_compat_params params = {
            .type = NGPU_PIPELINE_TYPE_GRAPHICS,
            .graphics = {
                .topology     = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                .state        = state,
                .rt_layout    = rnode_pos->rendertarget_layout,
                .vertex_state = ngpu_pgcraft_get_vertex_state(s->crafter),
            },
            .program          = ngpu_pgcraft_get_program(s->crafter),
            .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(s->crafter),
            .resources        = ngpu_pgcraft_get_bindgroup_resources(s->crafter),
            .vertex_resources = ngpu_pgcraft_get_vertex_resources(s->crafter),
            .compat_info      = ngpu_pgcraft_get_compat_info(s->crafter),
        };

        ret = ngli_node_prepare_children(node);
        if (ret < 0)
            return ret;

        ret = ngli_pipeline_compat_init(s->composite_pl, &params);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int resize_fbo(struct effect2d_priv *s, struct ngl_ctx *ctx, uint32_t width, uint32_t height)
{
    if (s->fbo_width == width && s->fbo_height == height && s->children_rtt)
        return 0;

    ngli_rtt_freep(&s->children_rtt);

    s->children_rtt = ngli_rtt_create(ctx);
    if (!s->children_rtt)
        return NGL_ERROR_MEMORY;

    const struct ngpu_texture_params tex_params = {
        .type    = NGPU_TEXTURE_TYPE_2D,
        .format  = NGPU_FORMAT_R8G8B8A8_UNORM,
        .width   = width,
        .height  = height,
        .usage   = NGPU_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
                   NGPU_TEXTURE_USAGE_SAMPLED_BIT,
        .min_filter = NGPU_FILTER_LINEAR,
        .mag_filter = NGPU_FILTER_LINEAR,
        .wrap_s  = NGPU_WRAP_CLAMP_TO_EDGE,
        .wrap_t  = NGPU_WRAP_CLAMP_TO_EDGE,
    };
    int ret = ngli_rtt_from_texture_params(s->children_rtt, &tex_params);
    if (ret < 0)
        return ret;

    /* Set the UV coordinate matrix so texture sampling matches the FBO orientation */
    struct image *image = ngli_rtt_get_image(s->children_rtt, 0);
    ngpu_ctx_get_rendertarget_uvcoord_matrix(ctx->gpu_ctx, image->coordinates_matrix);

    s->fbo_width = width;
    s->fbo_height = height;

    return 0;
}

static int resize_blur(struct effect2d_priv *s, struct ngl_ctx *ctx, uint32_t width, uint32_t height)
{
    /* Check if blur mips are already the right size */
    if (s->blur_mips[0] && s->fbo_width == width && s->fbo_height == height)
        return 0;

    const struct ngpu_texture_params mip_tex_params = {
        .type       = NGPU_TEXTURE_TYPE_2D,
        .format     = NGPU_FORMAT_R8G8B8A8_UNORM,
        .width      = width,
        .height     = height,
        .min_filter = NGPU_FILTER_LINEAR,
        .mag_filter = NGPU_FILTER_LINEAR,
        .wrap_s     = NGPU_WRAP_MIRRORED_REPEAT,
        .wrap_t     = NGPU_WRAP_MIRRORED_REPEAT,
        .usage      = NGPU_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
                      NGPU_TEXTURE_USAGE_SAMPLED_BIT,
    };

    /* Create full-res mip for upsample result */
    ngli_rtt_freep(&s->blur_mip);
    s->blur_mip = ngli_rtt_create(ctx);
    if (!s->blur_mip)
        return NGL_ERROR_MEMORY;

    struct ngpu_texture_params p = mip_tex_params;
    int ret = ngli_rtt_from_texture_params(s->blur_mip, &p);
    if (ret < 0)
        return ret;

    /* Create mip pyramid */
    uint32_t mip_w = width;
    uint32_t mip_h = height;
    for (size_t i = 0; i < MAX_MIP_LEVELS; i++) {
        ngli_rtt_freep(&s->blur_mips[i]);
        s->blur_mips[i] = ngli_rtt_create(ctx);
        if (!s->blur_mips[i])
            return NGL_ERROR_MEMORY;

        p.width = mip_w;
        p.height = mip_h;
        ret = ngli_rtt_from_texture_params(s->blur_mips[i], &p);
        if (ret < 0)
            return ret;

        mip_w = NGLI_MAX(mip_w >> 1, 1);
        mip_h = NGLI_MAX(mip_h >> 1, 1);
    }

    /* Create blur destination RTT (same size as source, for interpolation output) */
    ngli_rtt_freep(&s->blur_dst_rtt);
    s->blur_dst_rtt = ngli_rtt_create(ctx);
    if (!s->blur_dst_rtt)
        return NGL_ERROR_MEMORY;

    p = mip_tex_params;
    p.width = width;
    p.height = height;
    ret = ngli_rtt_from_texture_params(s->blur_dst_rtt, &p);
    if (ret < 0)
        return ret;

    /* Set UV coordinate matrix on blur output */
    struct image *dst_image = ngli_rtt_get_image(s->blur_dst_rtt, 0);
    ngpu_ctx_get_rendertarget_uvcoord_matrix(ctx->gpu_ctx, dst_image->coordinates_matrix);

    const uint32_t nb_mips = ngli_log2(NGLI_MAX(width, height)) + 1;
    s->blur_max_lod = NGLI_MIN(nb_mips - 1, MAX_MIP_LEVELS - 2);

    return 0;
}

static float compute_blur_lod(float radius)
{
    const float k = 5.17925f;
    if (radius <= k)
        return radius / k;
    return 1.34508f * logf(0.406057f * radius);
}

static void execute_blur_pass(struct ngl_ctx *ctx,
                              struct rtt_ctx *rtt_ctx,
                              struct pipeline_compat *pipeline,
                              const struct image *image)
{
    ngli_rtt_begin(rtt_ctx);
    ngpu_ctx_begin_render_pass(ctx->gpu_ctx, ctx->current_rendertarget);
    ngli_pipeline_compat_update_image(pipeline, 0, image);
    ngli_pipeline_compat_draw(pipeline, 3, 1, 0);
    ngli_rtt_end(rtt_ctx);
}

static const struct image *execute_blur(struct effect2d_priv *s, struct ngl_ctx *ctx,
                                        const struct image *src_image, float blurriness,
                                        uint32_t width, uint32_t height)
{
    const float diagonal = hypotf((float)width, (float)height);
    const float radius = blurriness * diagonal / 2.f;
    const float lod = NGLI_MIN(compute_blur_lod(radius), (float)s->blur_max_lod);
    const int32_t lod_i = (int32_t)lod;
    const float lod_f = lod - (float)lod_i;

    /* Downsample source to mips[1] */
    const struct image *mip = src_image;
    execute_blur_pass(ctx, s->blur_mips[1], s->blur_dws.pl, mip);

    /* Downsample successively until mips[lod_i+1] is generated */
    for (int32_t i = 2; i <= lod_i + 1; i++)
        execute_blur_pass(ctx, s->blur_mips[i], s->blur_dws.pl, ngli_rtt_get_image(s->blur_mips[i - 1], 0));

    /*
     * Upsample successively from mips[lod_i] back to full resolution.
     * If lod == 0, we simply use the source.
     */
    if (lod_i > 0) {
        for (int32_t i = lod_i - 1; i > 0; i--)
            execute_blur_pass(ctx, s->blur_mips[i], s->blur_ups.pl, ngli_rtt_get_image(s->blur_mips[i + 1], 0));
        execute_blur_pass(ctx, s->blur_mip, s->blur_ups.pl, ngli_rtt_get_image(s->blur_mips[1], 0));
        mip = ngli_rtt_get_image(s->blur_mip, 0);
    }

    /* Upsample from mips[lod_i+1] back to full resolution in mips[0] */
    for (int32_t i = lod_i; i >= 0; i--)
        execute_blur_pass(ctx, s->blur_mips[i], s->blur_ups.pl, ngli_rtt_get_image(s->blur_mips[i + 1], 0));

    /* Interpolate the two blurred layers */
    const struct interpolate_block ib = {.lod = lod_f};
    ngpu_block_update(&s->blur_interp.block, 0, &ib);

    ngli_rtt_begin(s->blur_dst_rtt);
    ngpu_ctx_begin_render_pass(ctx->gpu_ctx, ctx->current_rendertarget);
    ngli_pipeline_compat_update_image(s->blur_interp.pl, 0, mip);
    ngli_pipeline_compat_update_image(s->blur_interp.pl, 1, ngli_rtt_get_image(s->blur_mips[0], 0));
    ngli_pipeline_compat_draw(s->blur_interp.pl, 3, 1, 0);
    ngli_rtt_end(s->blur_dst_rtt);

    return ngli_rtt_get_image(s->blur_dst_rtt, 0);
}

static void effect2d_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;
    struct effect2d_priv *s = node->priv_data;
    const struct effect2d_opts *o = node->opts;

    if (!o->visible) {
        s->draw_info.screen_aabb = NGLI_AABB_EMPTY;
        return;
    }

    /* Resize FBO to match current viewport */
    const uint32_t w = (uint32_t)ctx->viewport.width;
    const uint32_t h = (uint32_t)ctx->viewport.height;
    int ret = resize_fbo(s, ctx, w, h);
    if (ret < 0)
        return;

    /*
     * Render children to internal FBO.
     * Save current 2D state and set up a fresh Canvas-like environment.
     */
    NGLI_ALIGNED_MAT(prev_projection_2d);
    memcpy(prev_projection_2d, ctx->projection_2d_matrix, sizeof(prev_projection_2d));
    struct darray prev_transform_2d_stack = ctx->transform_2d_stack;
    struct darray prev_opacity_2d_stack = ctx->opacity_2d_stack;

    ngli_darray_init(&ctx->transform_2d_stack, 4 * 4 * sizeof(float), NGLI_DARRAY_FLAG_ALIGNED);
    ngli_darray_init(&ctx->opacity_2d_stack, sizeof(float), 0);

    static const NGLI_ALIGNED_MAT(id_matrix) = NGLI_MAT4_IDENTITY;
    const float default_opacity = 1.f;
    if (!ngli_darray_push(&ctx->transform_2d_stack, id_matrix) ||
        !ngli_darray_push(&ctx->opacity_2d_stack, &default_opacity))
        goto restore_2d_state;

    /* Begin rendering to FBO, then set up orthographic projection
     * (must be done after rtt_begin to get the FBO's coordinate system) */
    ngli_rtt_begin(s->children_rtt);

    NGLI_ALIGNED_MAT(fbo_base_projection);
    ngpu_ctx_get_projection_matrix(gpu_ctx, fbo_base_projection);
    ngli_mat4_orthographic(ctx->projection_2d_matrix, 0.f, (float)w, (float)h, 0.f, -1.f, 1.f);
    ngli_mat4_mul(ctx->projection_2d_matrix, fbo_base_projection, ctx->projection_2d_matrix);

    {
        struct rnode *rnode_pos = ctx->rnode_pos;
        struct rnode *rnodes = ngli_darray_data(&rnode_pos->children);
        const size_t *indices = ngli_darray_data(&s->indices);
        for (size_t i = 0; i < o->nb_children; i++) {
            const size_t index = indices[i];
            ctx->rnode_pos = &rnodes[index];
            struct ngl_node *child = o->children[i];
            ngli_node_draw(child);
        }
        ctx->rnode_pos = rnode_pos;
    }

    ngli_rtt_end(s->children_rtt);

    /* Compute children bounding box in local space */
    struct aabb children_aabb = ngli_node_compute_children_bounding_box(o->children, o->nb_children);

restore_2d_state:
    /* Restore the parent's 2D state */
    ngli_darray_reset(&ctx->transform_2d_stack);
    ngli_darray_reset(&ctx->opacity_2d_stack);
    ctx->transform_2d_stack = prev_transform_2d_stack;
    ctx->opacity_2d_stack = prev_opacity_2d_stack;
    memcpy(ctx->projection_2d_matrix, prev_projection_2d, sizeof(prev_projection_2d));

    /*
     * Apply blur if blur > 0
     */
    const struct image *result_image = ngli_rtt_get_image(s->children_rtt, 0);

    const float blur_raw = *(const float *)ngli_node_get_data_ptr(o->blur_node, &o->blur);
    const float blur_val = NGLI_CLAMP(blur_raw, 0.f, 1.f);
    if (blur_val > 0.f) {
        ret = resize_blur(s, ctx, w, h);
        if (ret < 0)
            return;
        result_image = execute_blur(s, ctx, result_image, blur_val, w, h);
    }

    /*
     * Apply custom fragment shader if specified
     */
    if (s->frag_pl) {
        /* Resize fragment RTT if needed */
        if (!s->frag_rtt || s->fbo_width != w || s->fbo_height != h) {
            ngli_rtt_freep(&s->frag_rtt);
            s->frag_rtt = ngli_rtt_create(ctx);
            if (!s->frag_rtt)
                return;
            const struct ngpu_texture_params frag_tex_params = {
                .type       = NGPU_TEXTURE_TYPE_2D,
                .format     = NGPU_FORMAT_R8G8B8A8_UNORM,
                .width      = w,
                .height     = h,
                .usage      = NGPU_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
                              NGPU_TEXTURE_USAGE_SAMPLED_BIT,
                .min_filter = NGPU_FILTER_LINEAR,
                .mag_filter = NGPU_FILTER_LINEAR,
                .wrap_s     = NGPU_WRAP_CLAMP_TO_EDGE,
                .wrap_t     = NGPU_WRAP_CLAMP_TO_EDGE,
            };
            ret = ngli_rtt_from_texture_params(s->frag_rtt, &frag_tex_params);
            if (ret < 0)
                return;
            struct image *frag_img = ngli_rtt_get_image(s->frag_rtt, 0);
            ngpu_ctx_get_rendertarget_uvcoord_matrix(gpu_ctx, frag_img->coordinates_matrix);
        }

        /* Update source texture (index 0 = "tex") */
        ngli_pipeline_compat_update_image(s->frag_pl, 0, result_image);

        /* Update user uniforms */
        const int32_t *uniform_indices = ngli_darray_data(&s->frag_uniform_indices);
        for (size_t i = 0; i < s->nb_frag_uniforms; i++) {
            const struct frag_uniform_def *cu = &s->frag_uniforms[i];
            if (uniform_indices[i] >= 0) {
                const void *data = frag_node_get_data(cu->node);
                ngli_pipeline_compat_update_uniform(s->frag_pl, uniform_indices[i], data);
            }
        }

        /* Update user textures (indices 1+) */
        for (size_t i = 0; i < s->nb_frag_textures; i++) {
            const struct frag_texture_def *ct = &s->frag_textures[i];
            const struct texture_info *tex_info = ct->texture_node->priv_data;
            ngli_pipeline_compat_update_image(s->frag_pl, (int32_t)(i + 1), &tex_info->image);
        }

        /* Execute fragment pass */
        ngli_rtt_begin(s->frag_rtt);
        ngpu_ctx_begin_render_pass(gpu_ctx, ctx->current_rendertarget);
        ngli_pipeline_compat_draw(s->frag_pl, 3, 1, 0);
        ngli_rtt_end(s->frag_rtt);

        result_image = ngli_rtt_get_image(s->frag_rtt, 0);
    }

    /*
     * Draw composite quad into the parent scene.
     * Use the parent's 2D transform stack + Effect2D's own TRS.
     */
    const float *anchor_val = ngli_node_get_data_ptr(o->anchor_node, o->anchor);
    const float anchor[3] = {
        isnan(anchor_val[0]) ? 0.f : anchor_val[0],
        isnan(anchor_val[1]) ? 0.f : anchor_val[1],
        0.f,
    };

    const float *scale = ngli_node_get_data_ptr(o->scale_node, o->scale);
    const float *rotation = ngli_node_get_data_ptr(o->rotation_node, &o->rotation);
    const float *translate = ngli_node_get_data_ptr(o->translate_node, o->translate);

    NGLI_ALIGNED_MAT(SM);
    NGLI_ALIGNED_MAT(RM);
    NGLI_ALIGNED_MAT(TM);
    ngli_mat4_scale(SM, scale[0], scale[1], 1.f, anchor);
    float z_axis[3] = {0.f, 0.f, 1.f};
    ngli_mat4_rotate(RM, NGLI_DEG2RAD(*rotation), z_axis, anchor);
    ngli_mat4_translate(TM, translate[0], translate[1], 0.f);
    NGLI_ALIGNED_MAT(trs_matrix);
    ngli_mat4_mul(trs_matrix, RM, SM);
    ngli_mat4_mul(trs_matrix, TM, trs_matrix);

    NGLI_ALIGNED_MAT(modelview_matrix);
    const float *prev_matrix = ngli_darray_tail(&ctx->transform_2d_stack);
    ngli_mat4_mul(modelview_matrix, prev_matrix, trs_matrix);

    /* Update composite uniforms */
    struct pipeline_compat *pl = s->composite_pl;

    if (s->modelview_matrix_index >= 0)
        ngli_pipeline_compat_update_uniform(pl, s->modelview_matrix_index, modelview_matrix);
    if (s->projection_matrix_index >= 0)
        ngli_pipeline_compat_update_uniform(pl, s->projection_matrix_index, ctx->projection_2d_matrix);

    const float *group_opacity = ngli_darray_tail(&ctx->opacity_2d_stack);
    const float local_opacity = *(const float *)ngli_node_get_data_ptr(o->opacity_node, &o->opacity);
    const float final_opacity = local_opacity * *group_opacity;
    if (s->opacity_index >= 0)
        ngli_pipeline_compat_update_uniform(pl, s->opacity_index, &final_opacity);

    /* Update composite texture from the result (children FBO or blurred) */
    ngli_pipeline_compat_update_image(pl, 0, result_image);

    if (!ngpu_ctx_is_render_pass_active(gpu_ctx))
        ngpu_ctx_begin_render_pass(gpu_ctx, ctx->current_rendertarget);

    ngpu_ctx_set_viewport(gpu_ctx, &ctx->viewport);
    ngpu_ctx_set_scissor(gpu_ctx, &ctx->scissor);

    ngli_pipeline_compat_draw(pl, 4, 1, 0);

    /* Set AABB for parent nodes */
    struct draw_info *draw_info = &s->draw_info;
    memcpy(draw_info->transform_matrix, modelview_matrix, sizeof(draw_info->transform_matrix));
    draw_info->screen_aabb = children_aabb;
}

static void effect2d_release(struct ngl_node *node)
{
    struct effect2d_priv *s = node->priv_data;

    ngli_rtt_freep(&s->children_rtt);
    ngli_rtt_freep(&s->blur_mip);
    for (size_t i = 0; i < MAX_MIP_LEVELS; i++)
        ngli_rtt_freep(&s->blur_mips[i]);
    ngli_rtt_freep(&s->blur_dst_rtt);
    ngli_rtt_freep(&s->frag_rtt);
    s->fbo_width = 0;
    s->fbo_height = 0;
}

static void effect2d_uninit(struct ngl_node *node)
{
    struct effect2d_priv *s = node->priv_data;

    ngli_pipeline_compat_freep(&s->composite_pl);
    ngpu_pgcraft_freep(&s->crafter);
    ngli_geometry_freep(&s->quad_geometry);
    ngli_darray_reset(&s->indices);

    /* Custom fragment cleanup */
    ngli_pipeline_compat_freep(&s->frag_pl);
    ngpu_pgcraft_freep(&s->frag_crafter);
    ngli_freep(&s->frag_glsl);
    ngli_darray_reset(&s->frag_uniform_indices);

    /* Blur cleanup */
    ngli_pipeline_compat_freep(&s->blur_dws.pl);
    ngpu_pgcraft_freep(&s->blur_dws.crafter);
    ngli_pipeline_compat_freep(&s->blur_ups.pl);
    ngpu_pgcraft_freep(&s->blur_ups.crafter);
    ngli_pipeline_compat_freep(&s->blur_interp.pl);
    ngpu_pgcraft_freep(&s->blur_interp.crafter);
    ngpu_block_reset(&s->blur_interp.block);
    ngpu_block_reset(&s->blur_down_up_block);
}

const struct node_class ngli_effect2d_class = {
    .id        = NGL_NODE_EFFECT2D,
    .name      = "Effect2D",
    .priv_size = sizeof(struct effect2d_priv),
    .init      = effect2d_init,
    .prepare   = effect2d_prepare,
    .update    = ngli_node_update_children,
    .draw      = effect2d_draw,
    .release   = effect2d_release,
    .uninit    = effect2d_uninit,
    .opts_size = sizeof(struct effect2d_opts),
    .params    = effect2d_params,
    .flags     = NGLI_NODE_FLAG_BOUNDS,
    .file      = __FILE__,
};
