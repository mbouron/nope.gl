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

#ifndef SHADER2D_H
#define SHADER2D_H

#include <ngpu/ngpu.h>
#include "utils/darray.h"

#define NGLI_SHADER2D_NAME_LEN 64

struct bstr;

int ngli_shader2d_check_key(const char *key, const char *label);

struct shader2d_role {
    const char *name; /* Suffix used in generated varying names */
    const char *uv;   /* Vertex expression for the role's logical texture UV */
};

/* Owns copied texture names and generated varying declarations.
 * Callers manage the corresponding resources and bindings. */
struct shader2d {
    NGLI_DARRAY(char *) textures;
    NGLI_DARRAY(struct ngpu_pgcraft_iovar) varyings;
};

/* Initialize fresh storage to an empty assembly state. Reset before reinitializing
 * an object that owns data. Neither init nor reset allocates GPU resources. */
void ngli_shader2d_init(struct shader2d *s);

/* Copy a 2D/video texture name, also used as the helper suffix. The caller must
 * register it with pgcraft, which supplies <name>_coord_matrix in both stages.
 * Return 0 on success (including duplicates), or a negative error. */
int ngli_shader2d_add_texture(struct shader2d *s, const char *name);

/*
 * Both source writers rename ordinary-code main identifiers to entry, preserving
 * comments, preprocessor directives (including continued lines), and source line
 * counts. This lexical rewrite does not validate or preprocess GLSL.
 */
/* Append an internal shader template, expanding $ to prefix before rewriting. */
int ngli_shader2d_write_builtin(struct bstr *out, const char *source, const char *entry, const char *prefix);

/* Append user GLSL with an entry prototype (vec4, or void for multiple outputs)
 * and #line directives using source_id. Log label for diagnostics; it is never
 * inserted into the GLSL source. */
int ngli_shader2d_write_source(struct bstr *out, const char *source, const char *entry,
                               bool multiple_outputs, int source_id, const char *label);

/* Append the shared fragment declarations: ngl_FragmentInput, ngl_content_uv,
 * and the named texture helpers. Call after registering all textures and before
 * appending the shader bodies with write_builtin() or write_source(). */
void ngli_shader2d_write_header(const struct shader2d *s, struct bstr *out);

/* Assign the texture UV members of the caller's local ngl_FragmentInput frag. */
void ngli_shader2d_write_fragment_input_textures(const struct shader2d *s, struct bstr *out,
                                                 const struct shader2d_role *role);

/* Append base and a main() that calls ngli_vertex() and assigns texture varyings.
 * Each role's uv expression must be valid after ngli_vertex(). Rebuild s->varyings
 * from a copy of vars followed by the generated declarations. The caller owns out;
 * s owns the declarations until the next write_vertex() or reset. */
int ngli_shader2d_write_vertex(struct shader2d *s, struct bstr *out, const char *base,
                               const struct ngpu_pgcraft_iovar *vars, size_t var_count,
                               const struct shader2d_role *roles, size_t role_count);

/* Free owned data and return to an empty, reusable state. */
void ngli_shader2d_reset(struct shader2d *s);

#endif
