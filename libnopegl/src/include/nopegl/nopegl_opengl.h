/*
 * Copyright 2023-2025 Nope Forge
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

#ifndef NOPEGL_OPENGL_H
#define NOPEGL_OPENGL_H

#include <nopegl/nopegl.h>

struct ngl_config_gl {
    /*
     * Native OpenGL context handle (EGLContext, HGLRC, NSOpenGLContext, or
     * EAGLContext, depending on the platform), or zero.
     *
     * If nonzero, the OpenGL context created by nope.gl shares all shareable
     * data, as defined by the client API, with the specified context and the
     * contexts in its share group. The specified context must be valid and
     * must have been created on the display specified by ngl_config.display
     * and for the same client API (OpenGL or OpenGL ES) as the selected backend.
     *
     * The application can access textures from frames returned by ngl_draw()
     * using ngpu_texture_gl_get_name(). The application must synchronize access
     * to shared objects. See ngpu_fence_gl_get_sync().
     *
     * If shared_context is nonzero, ngl_config.shared_gpu_ctx must be NULL.
     * ngl_configure() returns NGL_ERROR_INVALID_ARG if both are specified.
     *
     * nope.gl makes its OpenGL context current to the calling thread for
     * operations that require it and releases it afterwards. If the application
     * uses the same thread for its own context, it must make that context
     * current again before issuing OpenGL commands.
     */
    uintptr_t shared_context;
};

struct ngl_custom_texture_info_gl {
   uint32_t texture;
   uint32_t target;
   uint32_t width;
   uint32_t height;
};

/**
 * Defines an OpenGL user-provided texture to be used by the node
 *
 * This function must only be called from the node user-defined functions of
 * the NGL_NODE_CUSTOMTEXTURE node.
 *
 * @param node      pointer to the target node
 * @param info      pointer to a ngl_custom_texture_info structure. NULL can be
 *                  passed in order to reset previous texture information.
 *
 * @return 0 on success, NGL_ERROR_* (< 0) on error
 */
NGL_API int ngl_custom_texture_set_texture_info_gl(struct ngl_node *node, const struct ngl_custom_texture_info_gl *info);

#endif
