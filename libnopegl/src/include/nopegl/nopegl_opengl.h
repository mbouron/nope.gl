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
     * Whether the OpenGL context is external or not. If the OpenGL context is
     * external, it is the user responsibility to manage the OpenGL context and
     * make sure it is current before calling any of the ngl_* functions.
     */
    int external;
    /*
     * External OpenGL framebuffer used for rendering. The framebuffer must
     * have a color attachment composed of 4 color components (R, G, B, A) and
     * a combined depth and stencil buffer attached to it.
     */
    uint32_t external_framebuffer;
};

/**
 * Wrap a new external OpenGL framebuffer and use it for rendering
 *
 * The framebuffer must have a color attachment composed of 4 color components
 * (R, G, B, A) and a combined depth and stencil buffer attached to it.
 *
 * This function only works if the OpenGL context is external.
 *
 * @param s               pointer to a nope.gl context
 * @param framebuffer     OpenGL framebuffer identifier to wrap
 *
 * @return 0 on success, NGL_ERROR_* (< 0) on error
 */
NGL_API int ngl_gl_wrap_framebuffer(struct ngl_ctx *s, uint32_t framebuffer);

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
