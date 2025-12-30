/*
 * Copyright 2023-2025 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2016-2022 GoPro Inc.
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

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "ngpu/ctx.h"
#include "ngpu/ngpu.h"
#include "ngpu/opengl/glcontext.h"
#include "ngpu/opengl/glincludes.h"
#include "utils/bstr.h"
#include "utils/memory.h"
#include "utils/utils.h"

#ifdef HAVE_GLPLATFORM_EGL
#include "ngpu/opengl/egl.h"
#endif

NGLI_STATIC_ASSERT(sizeof(GLfloat)  == sizeof(float),          "GLfloat size");
NGLI_STATIC_ASSERT(sizeof(GLbyte)   == sizeof(char),           "GLbyte size");
NGLI_STATIC_ASSERT(sizeof(GLshort)  == sizeof(short),          "GLshort size");
NGLI_STATIC_ASSERT(sizeof(GLint)    == sizeof(int),            "GLint size");
NGLI_STATIC_ASSERT(sizeof(GLubyte)  == sizeof(unsigned char),  "GLubyte size");
NGLI_STATIC_ASSERT(sizeof(GLushort) == sizeof(unsigned short), "GLushort size");
NGLI_STATIC_ASSERT(sizeof(GLuint)   == sizeof(unsigned int),   "GLuint size");
NGLI_STATIC_ASSERT(GL_FALSE == 0 && GL_TRUE == 1,              "GLboolean values");

enum {
    GLPLATFORM_EGL,
    GLPLATFORM_NSGL,
    GLPLATFORM_EAGL,
    GLPLATFORM_WGL,
};

extern const struct glcontext_class ngpu_glcontext_egl_class;
extern const struct glcontext_class ngpu_glcontext_egl_external_class;
extern const struct glcontext_class ngpu_glcontext_nsgl_class;
extern const struct glcontext_class ngpu_glcontext_nsgl_external_class;
extern const struct glcontext_class ngpu_glcontext_eagl_class;
extern const struct glcontext_class ngpu_glcontext_eagl_external_class;
extern const struct glcontext_class ngpu_glcontext_wgl_class;
extern const struct glcontext_class ngpu_glcontext_wgl_external_class;

static const struct {
    const struct glcontext_class *cls;
    const struct glcontext_class *external_cls;
} glcontext_class_map[] = {
#ifdef HAVE_GLPLATFORM_EGL
    [GLPLATFORM_EGL] = {
        .cls = &ngpu_glcontext_egl_class,
        .external_cls = &ngpu_glcontext_egl_external_class,
    },
#endif
#ifdef HAVE_GLPLATFORM_NSGL
    [GLPLATFORM_NSGL] = {
        .cls = &ngpu_glcontext_nsgl_class,
        .external_cls = &ngpu_glcontext_nsgl_external_class,
    },
#endif
#ifdef HAVE_GLPLATFORM_EAGL
    [GLPLATFORM_EAGL] = {
        .cls = &ngpu_glcontext_eagl_class,
        .external_cls = &ngpu_glcontext_eagl_external_class,
    },
#endif
#ifdef HAVE_GLPLATFORM_WGL
    [GLPLATFORM_WGL] = {
        .cls = &ngpu_glcontext_wgl_class,
        .external_cls = &ngpu_glcontext_wgl_external_class,
    },
#endif
};

static const int platform_to_glplatform[] = {
    [NGPU_PLATFORM_XLIB]    = GLPLATFORM_EGL,
    [NGPU_PLATFORM_ANDROID] = GLPLATFORM_EGL,
    [NGPU_PLATFORM_MACOS]   = GLPLATFORM_NSGL,
    [NGPU_PLATFORM_IOS]     = GLPLATFORM_EAGL,
    [NGPU_PLATFORM_WINDOWS] = GLPLATFORM_WGL,
    [NGPU_PLATFORM_WAYLAND] = GLPLATFORM_EGL,
};

static const char * const backend_names[] = {
    [NGPU_BACKEND_OPENGL]   = "NGPU_BACKEND_OPENGL",
    [NGPU_BACKEND_OPENGLES] = "NGPU_BACKEND_OPENGLES",
};

#define OFFSET(x) offsetof(struct glfunctions, x)
static const struct glfeature {
    const char *name;
    uint64_t flag;
    size_t offset;
    int version;
    int es_version;
    const char **extensions;
    const char **es_extensions;
    const size_t *funcs_offsets;
} glfeatures[] = {
    {
        .name           = "texture_storage",
        .flag           = NGPU_FEATURE_GL_TEXTURE_STORAGE,
        .version        = 420,
        .es_version     = 310,
        .funcs_offsets  = (const size_t[]){OFFSET(TexStorage2D),
                                           OFFSET(TexStorage3D),
                                           SIZE_MAX}
    }, {
        .name           = "compute_shader",
        .flag           = NGPU_FEATURE_GL_COMPUTE_SHADER,
        .version        = 430,
        .es_version     = 310,
        .extensions     = (const char*[]){"GL_ARB_compute_shader", NULL},
        .funcs_offsets  = (const size_t[]){OFFSET(DispatchCompute),
                                           SIZE_MAX}
    }, {
        .name           = "program_interface_query",
        .flag           = NGPU_FEATURE_GL_PROGRAM_INTERFACE_QUERY,
        .version        = 430,
        .es_version     = 310,
        .extensions     = (const char*[]){"GL_ARB_program_interface_query", NULL},
        .funcs_offsets  = (const size_t[]){OFFSET(GetProgramResourceIndex),
                                           OFFSET(GetProgramResourceiv),
                                           OFFSET(GetProgramResourceLocation),
                                           OFFSET(GetProgramInterfaceiv),
                                           OFFSET(GetProgramResourceName),
                                           SIZE_MAX}
    }, {
        .name           = "shader_image_load_store",
        .flag           = NGPU_FEATURE_GL_SHADER_IMAGE_LOAD_STORE,
        .version        = 420,
        .es_version     = 310,
        .extensions     = (const char*[]){"GL_ARB_shader_image_load_store", NULL},
        .funcs_offsets  = (const size_t[]){OFFSET(BindImageTexture),
                                           OFFSET(MemoryBarrier),
                                           SIZE_MAX}
    }, {
        .name           = "shader_storage_buffer_object",
        .flag           = NGPU_FEATURE_GL_SHADER_STORAGE_BUFFER_OBJECT,
        .version        = 430,
        .es_version     = 310,
        .extensions     = (const char*[]){"GL_ARB_shader_storage_buffer_object", NULL},
        .funcs_offsets  = (const size_t[]){OFFSET(TexStorage2D),
                                           OFFSET(TexStorage3D),
                                           SIZE_MAX}
    }, {
        .name           = "internalformat_query",
        .flag           = NGPU_FEATURE_GL_INTERNALFORMAT_QUERY,
        .version        = 420,
        .es_version     = 300,
        .extensions     = (const char*[]){"GL_ARB_internalformat_query", NULL},
        .funcs_offsets  = (const size_t[]){OFFSET(GetInternalformativ),
                                           SIZE_MAX}
    }, {
        .name           = "timer_query",
        .flag           = NGPU_FEATURE_GL_TIMER_QUERY,
        .version        = 330,
        .extensions     = (const char*[]){"GL_ARB_timer_query", NULL},
    }, {
        .name           = "ext_disjoint_timer_query",
        .flag           = NGPU_FEATURE_GL_EXT_DISJOINT_TIMER_QUERY,
        .es_extensions  = (const char*[]){"GL_EXT_disjoint_timer_query", NULL},
        .funcs_offsets  = (const size_t[]){OFFSET(BeginQueryEXT),
                                           OFFSET(EndQueryEXT),
                                           OFFSET(GenQueriesEXT),
                                           OFFSET(DeleteQueriesEXT),
                                           OFFSET(QueryCounterEXT),
                                           OFFSET(GetQueryObjectui64vEXT),
                                           SIZE_MAX}
    }, {
        .name           = "invalidate_subdata",
        .flag           = NGPU_FEATURE_GL_INVALIDATE_SUBDATA,
        .version        = 430,
        .es_version     = 300,
        .extensions     = (const char*[]){"GL_ARB_invalidate_subdata"},
        .funcs_offsets  = (const size_t[]){OFFSET(InvalidateFramebuffer),
                                           SIZE_MAX}
    }, {
        .name           = "oes_egl_external_image",
        .flag           = NGPU_FEATURE_GL_OES_EGL_EXTERNAL_IMAGE,
        .es_extensions  = (const char*[]){"GL_OES_EGL_image_external",
                                          "GL_OES_EGL_image_external_essl3",
                                          NULL},
        .funcs_offsets  = (const size_t[]){OFFSET(EGLImageTargetTexture2DOES),
                                           SIZE_MAX}
    }, {
        .name           = "oes_egl_image",
        .flag           = NGPU_FEATURE_GL_OES_EGL_IMAGE,
        .extensions     = (const char*[]){"GL_OES_EGL_image", NULL},
        .es_extensions  = (const char*[]){"GL_OES_EGL_image", NULL},
        .funcs_offsets  = (const size_t[]){OFFSET(EGLImageTargetTexture2DOES),
                                           SIZE_MAX}
    }, {
        .name           = "ext_egl_image_storage",
        .flag           = NGPU_FEATURE_GL_EXT_EGL_IMAGE_STORAGE,
        .extensions     = (const char*[]){"GL_EXT_EGL_image_storage", NULL},
        .es_extensions  = (const char*[]){"GL_EXT_EGL_image_storage", NULL},
        .funcs_offsets  = (const size_t[]){OFFSET(EGLImageTargetTexStorageEXT),
                                           SIZE_MAX}
    }, {
        .name           = "yuv_target",
        .flag           = NGPU_FEATURE_GL_YUV_TARGET,
        .es_extensions  = (const char*[]){"GL_EXT_YUV_target", NULL}
    }, {
        .name           = "khr_debug",
        .flag           = NGPU_FEATURE_GL_KHR_DEBUG,
        .version        = 430,
        .es_version     = 320,
        .es_extensions  = (const char*[]){"GL_KHR_debug", NULL},
        .funcs_offsets  = (const size_t[]){OFFSET(DebugMessageCallback),
                                        SIZE_MAX}
    }, {
        .name           = "shader_image_size",
        .flag           = NGPU_FEATURE_GL_SHADER_IMAGE_SIZE,
        .version        = 430,
        .es_version     = 310,
        .extensions     = (const char*[]){"GL_ARB_shader_image_size", NULL},
    }, {
        .name           = "shading_language_420pack",
        .flag           = NGPU_FEATURE_GL_SHADING_LANGUAGE_420PACK,
        .version        = 420,
        .extensions     = (const char*[]){"GL_ARB_shading_language_420pack", NULL},
    }, {
        .name           = "color_buffer_float",
        .flag           = NGPU_FEATURE_GL_COLOR_BUFFER_FLOAT,
        .version        = 300,
        .es_version     = 320,
        .es_extensions  = (const char*[]){"GL_EXT_color_buffer_float", NULL},
    }, {
        .name           = "color_buffer_half_float",
        .flag           = NGPU_FEATURE_GL_COLOR_BUFFER_HALF_FLOAT,
        .version        = 300,
        .es_version     = 320,
        .es_extensions  = (const char*[]){"GL_EXT_color_buffer_half_float", NULL},
    }, {
        .name           = "buffer_storage",
        .flag           = NGPU_FEATURE_GL_BUFFER_STORAGE,
        .version        = 440,
        .extensions     = (const char*[]){"GL_ARB_buffer_storage", NULL},
        .funcs_offsets  = (const size_t[]){OFFSET(BufferStorage),
                                           SIZE_MAX}
    }, {
        .name           = "ext_buffer_storage",
        .flag           = NGPU_FEATURE_GL_EXT_BUFFER_STORAGE,
        .es_extensions  = (const char*[]){"GL_EXT_buffer_storage", NULL},
        .funcs_offsets  = (const size_t[]){OFFSET(BufferStorageEXT),
                                           SIZE_MAX}
    }, {
        .name           = "texture_norm16",
        .flag           = NGPU_FEATURE_GL_TEXTURE_NORM16,
        .version        = 300,
        .es_extensions  = (const char*[]){"GL_EXT_texture_norm16", NULL},
    }, {
        .name           = "texture_float_linear",
        .flag           = NGPU_FEATURE_GL_TEXTURE_FLOAT_LINEAR,
        .version        = 300,
        .es_version     = 320,
        .es_extensions  = (const char*[]){"GL_OES_texture_float_linear", NULL},
    }, {
        .name           = "float_blend",
        .flag           = NGPU_FEATURE_GL_FLOAT_BLEND,
        .version        = 300,
        .es_version     = 320,
        .es_extensions  = (const char*[]){"GL_EXT_float_blend", NULL},
    }, {
        .name           = "viewport_array",
        .flag           = NGPU_FEATURE_GL_VIEWPORT_ARRAY,
        .version        = 410,
        .extensions     = (const char*[]){"GL_ARB_viewport_array", NULL},
        .funcs_offsets  = (const size_t[]){OFFSET(ViewportIndexedf),
                                           SIZE_MAX}
    },
};

static int glcontext_load_functions(struct glcontext *glcontext)
{
    const struct glfunctions *gl = &glcontext->funcs;

    for (size_t i = 0; i < NGLI_ARRAY_NB(glfunction_load_infos); i++) {
        const struct glfunction_load_info *func_load_info = &glfunction_load_infos[i];

        void *func = ngpu_glcontext_get_proc_address(glcontext, func_load_info->name);
        if (!func && !func_load_info->optional) {
            LOG(ERROR, "could not find core function: %s", func_load_info->name);
            return NGL_ERROR_NOT_FOUND;
        }

        *(void **)((uintptr_t)gl + func_load_info->offset) = func;
    }

    return 0;
}

static int glcontext_probe_version(struct glcontext *glcontext)
{
    GLint major_version = 0;
    GLint minor_version = 0;

    const char *gl_version = (const char *)glcontext->funcs.GetString(GL_VERSION);
    if (!gl_version) {
        LOG(ERROR, "could not get OpenGL version");
        return NGL_ERROR_BUG;
    }

    const char *es_prefix = "OpenGL ES";
    const int es = !strncmp(es_prefix, gl_version, strlen(es_prefix));
    const int backend = es ? NGPU_BACKEND_OPENGLES : NGPU_BACKEND_OPENGL;
    if (glcontext->backend != backend) {
        LOG(ERROR, "OpenGL context (%s) does not match requested backend (%s)",
            backend_names[backend], backend_names[glcontext->backend]);
        return NGL_ERROR_INVALID_USAGE;
    }

    if (glcontext->backend == NGPU_BACKEND_OPENGL) {
        glcontext->funcs.GetIntegerv(GL_MAJOR_VERSION, &major_version);
        glcontext->funcs.GetIntegerv(GL_MINOR_VERSION, &minor_version);
    } else if (glcontext->backend == NGPU_BACKEND_OPENGLES) {
        int ret = sscanf(gl_version,
                         "OpenGL ES %d.%d",
                         &major_version,
                         &minor_version);
        if (ret != 2) {
            LOG(ERROR, "could not parse OpenGL ES version: \"%s\"", gl_version);
            return NGL_ERROR_BUG;
        }
    } else {
        ngli_assert(0);
    }

    LOG(INFO, "OpenGL version: %d.%d %s",
        major_version,
        minor_version,
        glcontext->backend == NGPU_BACKEND_OPENGLES ? "ES " : "");

    const char *renderer = (const char *)glcontext->funcs.GetString(GL_RENDERER);
    if (!renderer) {
        LOG(ERROR, "could not get OpenGL renderer");
        return NGL_ERROR_BUG;
    }
    LOG(INFO, "OpenGL renderer: %s", renderer);

    if (strstr(renderer, "llvmpipe") || // Mesa llvmpipe
        strstr(renderer, "softpipe") || // Mesa softpipe
        strstr(renderer, "SWR")) {      // Mesa swrast
        glcontext->features |= NGPU_FEATURE_GL_SOFTWARE;
        LOG(INFO, "software renderer detected");
    }

    glcontext->version = major_version * 100 + minor_version * 10;

    if (glcontext->backend == NGPU_BACKEND_OPENGL && glcontext->version < 330) {
        LOG(ERROR, "nope.gl only supports OpenGL >= 3.3");
        return NGL_ERROR_UNSUPPORTED;
    } else if (glcontext->backend == NGPU_BACKEND_OPENGLES && glcontext->version < 300) {
        LOG(ERROR, "nope.gl only supports OpenGL ES >= 3.0");
        return NGL_ERROR_UNSUPPORTED;
    }

    return 0;
}

static int glcontext_probe_glsl_version(struct glcontext *glcontext)
{
    if (glcontext->backend == NGPU_BACKEND_OPENGL) {
        const char *glsl_version = (const char *)glcontext->funcs.GetString(GL_SHADING_LANGUAGE_VERSION);
        if (!glsl_version) {
            LOG(ERROR, "could not get GLSL version");
            return NGL_ERROR_BUG;
        }

        int major_version;
        int minor_version;
        int ret = sscanf(glsl_version, "%d.%d", &major_version, &minor_version);
        if (ret != 2) {
            LOG(ERROR, "could not parse GLSL version: \"%s\"", glsl_version);
            return NGL_ERROR_BUG;
        }
        glcontext->glsl_version = major_version * 100 + minor_version;
    } else if (glcontext->backend == NGPU_BACKEND_OPENGLES) {
        glcontext->glsl_version = glcontext->version;
    } else {
        ngli_assert(0);
    }

    return 0;
}

static int glcontext_check_extension(const char *extension,
                                     const struct glcontext *glcontext)
{
    GLint nb_extensions;
    glcontext->funcs.GetIntegerv(GL_NUM_EXTENSIONS, &nb_extensions);

    for (GLint i = 0; i < nb_extensions; i++) {
        const char *tmp = (const char *)glcontext->funcs.GetStringi(GL_EXTENSIONS, (GLuint)i);
        if (!tmp)
            break;
        if (!strcmp(extension, tmp))
            return 1;
    }

    return 0;
}

static int glcontext_check_extensions(struct glcontext *glcontext,
                                      const char **extensions)
{
    if (!extensions || !*extensions)
        return 0;

    if (glcontext->backend == NGPU_BACKEND_OPENGL ||
        glcontext->backend == NGPU_BACKEND_OPENGLES) {
        while (*extensions) {
            if (!glcontext_check_extension(*extensions, glcontext))
                return 0;

            extensions++;
        }
    } else {
        ngli_assert(0);
    }

    return 1;
}

static int glcontext_check_functions(struct glcontext *glcontext,
                                     const size_t *funcs_offsets)
{
    const struct glfunctions *gl = &glcontext->funcs;

    if (!funcs_offsets)
        return 1;

    while (*funcs_offsets != -1) {
        void *func_ptr = *(void **)((uintptr_t)gl + *funcs_offsets);
        if (!func_ptr)
            return 0;
        funcs_offsets++;
    }

    return 1;
}

static int glcontext_probe_extensions(struct glcontext *glcontext)
{
    const int es = glcontext->backend == NGPU_BACKEND_OPENGLES;
    struct bstr *features_str = ngli_bstr_create();

    if (!features_str)
        return NGL_ERROR_MEMORY;

    for (size_t i = 0; i < NGLI_ARRAY_NB(glfeatures); i++) {
        const struct glfeature *glfeature = &glfeatures[i];

        const char **extensions = es ? glfeature->es_extensions : glfeature->extensions;
        ngli_assert(!extensions || *extensions);

        int version = es ? glfeature->es_version : glfeature->version;
        if (!version && !extensions)
            continue;

        if (!version || glcontext->version < version) {
            if (!glcontext_check_extensions(glcontext, extensions))
                continue;
        }

        if (!glcontext_check_functions(glcontext, glfeature->funcs_offsets))
            continue;

        ngli_bstr_printf(features_str, " %s", glfeature->name);
        glcontext->features |= glfeature->flag;
    }

    LOG(INFO, "OpenGL%s features:%s", es ? " ES" : "", ngli_bstr_strptr(features_str));
    ngli_bstr_freep(&features_str);

    return 0;
}

#define GET(name, value) do {                         \
    GLint gl_value;                                   \
    glcontext->funcs.GetIntegerv((name), &gl_value);  \
    *(value) = (__typeof__(*(value)))gl_value;        \
} while (0)                                           \

#define GET_I(name, index, value) do {                           \
    GLint gl_value;                                              \
    glcontext->funcs.GetIntegeri_v((name), (index), &gl_value);  \
    *(value) = (__typeof__((*value)))gl_value;                   \
} while (0)                                                      \

static int glcontext_probe_limits(struct glcontext *glcontext)
{
    struct ngpu_limits *limits = &glcontext->limits;

    GET(GL_MAX_VERTEX_ATTRIBS, &limits->max_vertex_attributes);
    limits->max_vertex_attributes = NGLI_MIN(limits->max_vertex_attributes, NGPU_MAX_VERTEX_BUFFERS);
    /*
     * macOS and iOS OpenGL drivers pass gl_VertexID and gl_InstanceID as
     * standard attributes and forget to count them in GL_MAX_VERTEX_ATTRIBS.
     */
    if (glcontext->platform == NGPU_PLATFORM_MACOS || glcontext->platform == NGPU_PLATFORM_IOS)
        limits->max_vertex_attributes -= 2;
    GET(GL_MAX_TEXTURE_IMAGE_UNITS, &limits->max_texture_image_units);
    GET(GL_MAX_TEXTURE_SIZE, &limits->max_texture_dimension_1d);
    GET(GL_MAX_TEXTURE_SIZE, &limits->max_texture_dimension_2d);
    GET(GL_MAX_3D_TEXTURE_SIZE, &limits->max_texture_dimension_3d);
    GET(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &limits->max_texture_dimension_cube);
    GET(GL_MAX_ARRAY_TEXTURE_LAYERS, &limits->max_texture_array_layers);
    GET(GL_MAX_SAMPLES, &limits->max_samples);
    GET(GL_MAX_COLOR_ATTACHMENTS, &limits->max_color_attachments);
    limits->max_color_attachments = NGLI_MIN(limits->max_color_attachments, NGPU_MAX_COLOR_ATTACHMENTS);
    GET(GL_MAX_UNIFORM_BLOCK_SIZE, &limits->max_uniform_block_size);
    GET(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &limits->min_uniform_block_offset_alignment);

    if (glcontext->features & NGPU_FEATURE_GL_SHADER_STORAGE_BUFFER_OBJECT) {
        GET(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &limits->max_storage_block_size);
        GET(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &limits->min_storage_block_offset_alignment);
    }

    if (glcontext->features & NGPU_FEATURE_GL_SHADER_IMAGE_LOAD_STORE) {
        GET(GL_MAX_IMAGE_UNITS, &limits->max_image_units);
    }

    if (glcontext->features & NGPU_FEATURE_GL_COMPUTE_SHADER) {
        for (GLuint i = 0; i < (GLuint)NGLI_ARRAY_NB(limits->max_compute_work_group_count); i++) {
            GET_I(GL_MAX_COMPUTE_WORK_GROUP_COUNT, i, &limits->max_compute_work_group_count[i]);
        }

        GET(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &limits->max_compute_work_group_invocations);

        for (GLuint i = 0; i < (GLuint)NGLI_ARRAY_NB(limits->max_compute_work_group_size); i++) {
            GET_I(GL_MAX_COMPUTE_WORK_GROUP_SIZE, i, &limits->max_compute_work_group_size[i]);
        }

        GET(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, &limits->max_compute_shared_memory_size);
    }

    GET(GL_MAX_DRAW_BUFFERS, &limits->max_draw_buffers);

    return 0;
}

static int glcontext_probe_formats(struct glcontext *glcontext)
{
    ngpu_format_gl_init(glcontext);

    return 0;
}

static void NGLI_GL_APIENTRY GenQueriesNoop(GLsizei n, GLuint * ids) {}
static void NGLI_GL_APIENTRY DeleteQueriesNoop(GLsizei n, const GLuint *ids) {}
static void NGLI_GL_APIENTRY BeginQueryNoop(GLenum target, GLuint id) {}
static void NGLI_GL_APIENTRY EndQueryNoop(GLenum target) {}
static void NGLI_GL_APIENTRY QueryCounterNoop(GLuint id, GLenum target) {}
static void NGLI_GL_APIENTRY GetQueryObjectui64vNoop(GLuint id, GLenum pname, GLuint64 *params) {}

static int glcontext_probe_timer_functions(struct glcontext *gl)
{
    if (gl->features & NGPU_FEATURE_GL_TIMER_QUERY) {
        gl->timer_funcs.GenQueries          = gl->funcs.GenQueries;
        gl->timer_funcs.DeleteQueries       = gl->funcs.DeleteQueries;
        gl->timer_funcs.BeginQuery          = gl->funcs.BeginQuery;
        gl->timer_funcs.EndQuery            = gl->funcs.EndQuery;
        gl->timer_funcs.QueryCounter        = gl->funcs.QueryCounter;
        gl->timer_funcs.GetQueryObjectui64v = gl->funcs.GetQueryObjectui64v;
    } else if (gl->features & NGPU_FEATURE_GL_EXT_DISJOINT_TIMER_QUERY) {
        gl->timer_funcs.GenQueries          = gl->funcs.GenQueriesEXT;
        gl->timer_funcs.DeleteQueries       = gl->funcs.DeleteQueriesEXT;
        gl->timer_funcs.BeginQuery          = gl->funcs.BeginQueryEXT;
        gl->timer_funcs.EndQuery            = gl->funcs.EndQueryEXT;
        gl->timer_funcs.QueryCounter        = gl->funcs.QueryCounterEXT;
        gl->timer_funcs.GetQueryObjectui64v = gl->funcs.GetQueryObjectui64vEXT;
    } else {
        gl->timer_funcs.GenQueries          = &GenQueriesNoop;
        gl->timer_funcs.DeleteQueries       = &DeleteQueriesNoop;
        gl->timer_funcs.BeginQuery          = &BeginQueryNoop;
        gl->timer_funcs.EndQuery            = &EndQueryNoop;
        gl->timer_funcs.QueryCounter        = &QueryCounterNoop;
        gl->timer_funcs.GetQueryObjectui64v = &GetQueryObjectui64vNoop;
    }
    return 0;
}

static int glcontext_check_driver(struct glcontext *glcontext)
{
    const char *gl_version = (const char *)glcontext->funcs.GetString(GL_VERSION);
    if (!gl_version) {
        LOG(ERROR, "could not get OpenGL version");
        return NGL_ERROR_BUG;
    }

    int mesa_version[3] = {0};
    const char *mesa = strstr(gl_version, "Mesa");
    if (mesa) {
        int ret = sscanf(mesa, "Mesa %d.%d.%d", &mesa_version[0], &mesa_version[1], &mesa_version[2]);
        if (ret != 3) {
            LOG(ERROR, "could not parse Mesa version: \"%s\"", mesa);
            return NGL_ERROR_BUG;
        }
        LOG(INFO, "Mesa version: %d.%d.%d", NGLI_ARG_VEC3(mesa_version));
    }

#ifdef HAVE_GLPLATFORM_EGL
    if (glcontext->features & NGPU_FEATURE_GL_EGL_MESA_QUERY_DRIVER) {
        const char *driver_name = ngli_eglGetDisplayDriverName(glcontext);
        if (driver_name) {
            LOG(INFO, "EGL driver name: %s", driver_name);
            if (!strcmp(driver_name, "radeonsi"))
                glcontext->workaround_radeonsi_sync = 1;
        }
    }
#endif

    return 0;
}

static int glcontext_load_extensions(struct glcontext *glcontext)
{
    int ret = glcontext_load_functions(glcontext);
    if (ret < 0)
        return ret;

    ret = glcontext_probe_version(glcontext);
    if (ret < 0)
        return ret;

    ret = glcontext_probe_glsl_version(glcontext);
    if (ret < 0)
        return ret;

    ret = glcontext_probe_extensions(glcontext);
    if (ret < 0)
        return ret;

    ret = glcontext_probe_limits(glcontext);
    if (ret < 0)
        return ret;

    ret = glcontext_probe_formats(glcontext);
    if (ret < 0)
        return ret;

    ret = glcontext_probe_timer_functions(glcontext);
    if (ret < 0)
        return ret;

    ret = glcontext_check_driver(glcontext);
    if (ret < 0)
        return ret;

    return 0;
}

struct glcontext *ngpu_glcontext_create(const struct glcontext_params *params)
{
    if (params->platform < 0 || params->platform >= NGLI_ARRAY_NB(platform_to_glplatform))
        return NULL;

    const int glplatform = platform_to_glplatform[params->platform];
    if (glplatform < 0 || glplatform >= NGLI_ARRAY_NB(glcontext_class_map))
        return NULL;

    struct glcontext *glcontext = ngli_calloc(1, sizeof(*glcontext));
    if (!glcontext)
        return NULL;
    if (params->external) {
        glcontext->cls = glcontext_class_map[glplatform].external_cls;
    } else {
        glcontext->cls = glcontext_class_map[glplatform].cls;
    }

    if (glcontext->cls->priv_size) {
        glcontext->priv_data = ngli_calloc(1, glcontext->cls->priv_size);
        if (!glcontext->priv_data) {
            ngli_free(glcontext);
            return NULL;
        }
    }

    glcontext->platform = params->platform;
    glcontext->backend = params->backend;
    glcontext->external = params->external;
    glcontext->offscreen = params->offscreen;
    glcontext->width = params->width;
    glcontext->height = params->height;
    glcontext->samples = params->samples;
    glcontext->debug = params->debug;

    if (glcontext->cls->init) {
        int ret = glcontext->cls->init(glcontext, params->display, params->window, params->shared_ctx);
        if (ret < 0)
            goto fail;
    }

    int ret = ngpu_glcontext_make_current(glcontext, 1);
    if (ret < 0)
        goto fail;

    ret = glcontext_load_extensions(glcontext);
    if (ret < 0)
        goto fail;

    if (glcontext->backend == NGPU_BACKEND_OPENGL) {
        glcontext->funcs.Enable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
        glcontext->funcs.Enable(GL_FRAMEBUFFER_SRGB);
    }

    if (!glcontext->external && !glcontext->offscreen) {
        ret = ngpu_glcontext_resize(glcontext, glcontext->width, glcontext->height);
        if (ret < 0)
            goto fail;
    }

    if (!params->external && params->swap_interval >= 0)
        ngpu_glcontext_set_swap_interval(glcontext, params->swap_interval);

    return glcontext;
fail:
    ngpu_glcontext_freep(&glcontext);
    return NULL;
}

int ngpu_glcontext_make_current(struct glcontext *glcontext, int current)
{
    if (glcontext->cls->make_current)
        return glcontext->cls->make_current(glcontext, current);

    return 0;
}

int ngpu_glcontext_set_swap_interval(struct glcontext *glcontext, int interval)
{
    if (glcontext->cls->set_swap_interval)
        return glcontext->cls->set_swap_interval(glcontext, interval);

    return 0;
}

void ngpu_glcontext_swap_buffers(struct glcontext *glcontext)
{
    if (glcontext->cls->swap_buffers)
        glcontext->cls->swap_buffers(glcontext);
}

void ngpu_glcontext_set_surface_pts(struct glcontext *glcontext, double t)
{
    if (glcontext->cls->set_surface_pts)
        glcontext->cls->set_surface_pts(glcontext, t);
}

int ngpu_glcontext_resize(struct glcontext *glcontext, uint32_t width, uint32_t height)
{
    if (glcontext->offscreen) {
        LOG(ERROR, "offscreen context does not support resize operation");
        return NGL_ERROR_INVALID_USAGE;
    }

    if (glcontext->external) {
        LOG(ERROR, "external context does not support resize operation");
        return NGL_ERROR_INVALID_USAGE;
    }

    if (glcontext->cls->resize)
        return glcontext->cls->resize(glcontext, width, height);

    return NGL_ERROR_UNSUPPORTED;
}

void ngpu_glcontext_freep(struct glcontext **glcontextp)
{
    struct glcontext *glcontext;

    if (!glcontextp || !*glcontextp)
        return;

    glcontext = *glcontextp;

    if (glcontext->cls->uninit)
        glcontext->cls->uninit(glcontext);

    ngli_free(glcontext->priv_data);
    ngli_freep(glcontextp);
}

void *ngpu_glcontext_get_proc_address(struct glcontext *glcontext, const char *name)
{
    void *ptr = NULL;

    if (glcontext->cls->get_proc_address)
        ptr = glcontext->cls->get_proc_address(glcontext, name);

    return ptr;
}

void *ngpu_glcontext_get_texture_cache(struct glcontext *glcontext)
{
    void *texture_cache = NULL;

    if (glcontext->cls->get_texture_cache)
        texture_cache = glcontext->cls->get_texture_cache(glcontext);

    return texture_cache;
}

uintptr_t ngpu_glcontext_get_display(struct glcontext *glcontext)
{
    uintptr_t handle = 0;

    if (glcontext->cls->get_display)
        handle = glcontext->cls->get_display(glcontext);

    return handle;
}

uintptr_t ngpu_glcontext_get_handle(struct glcontext *glcontext)
{
    uintptr_t handle = 0;

    if (glcontext->cls->get_handle)
        handle = glcontext->cls->get_handle(glcontext);

    return handle;
}

GLuint ngpu_glcontext_get_default_framebuffer(struct glcontext *glcontext)
{
    GLuint fbo_id = 0;

    if (glcontext->cls->get_default_framebuffer)
        fbo_id = glcontext->cls->get_default_framebuffer(glcontext);

    return fbo_id;
}

int ngpu_glcontext_check_extension(const char *extension, const char *extensions)
{
    if (!extension || !extensions)
        return 0;

    size_t len = strlen(extension);
    const char *cur = extensions;
    const char *end = extensions + strlen(extensions);

    while (cur < end) {
        cur = strstr(extensions, extension);
        if (!cur)
            break;

        cur += len;
        if (cur[0] == ' ' || cur[0] == '\0')
            return 1;
    }

    return 0;
}

#define GL_ERROR_STR_CASE(error) case error: error_str = #error; break

int ngpu_glcontext_check_gl_error(const struct glcontext *glcontext, const char *context)
{
    const GLenum error = glcontext->funcs.GetError();
    if (!error)
        return 0;

    const char *error_str = NULL;
    switch (error) {
    GL_ERROR_STR_CASE(GL_INVALID_ENUM);
    GL_ERROR_STR_CASE(GL_INVALID_VALUE);
    GL_ERROR_STR_CASE(GL_INVALID_OPERATION);
    GL_ERROR_STR_CASE(GL_INVALID_FRAMEBUFFER_OPERATION);
    GL_ERROR_STR_CASE(GL_OUT_OF_MEMORY);
    default:
        error_str = "unknown error";
    }

    LOG(ERROR, "%s: GL error: %s (0x%04x)", context, error_str, error);

#if DEBUG_GL
    ngli_assert(0);
#endif

    return NGL_ERROR_GRAPHICS_GENERIC;
}
