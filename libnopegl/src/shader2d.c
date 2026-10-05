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

#include <stdio.h>
#include <string.h>

#include "glsl_lexer.h"
#include "log.h"
#include "shader2d.h"
#include "utils/bstr.h"
#include "utils/memory.h"
#include "utils/string.h"

static bool is_glsl_ident(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

int ngli_shader2d_check_key(const char *key, const char *label)
{
    if (!key[0] || (key[0] >= '0' && key[0] <= '9'))
        goto fail;

    for (const char *p = key; *p; p++)
        if (!is_glsl_ident(*p))
            goto fail;

    if (strlen(key) >= NGLI_SHADER2D_NAME_LEN)
        goto fail;

    if (!strcmp(key, "main"))
        goto fail;

    const char * const reserved[] = {"gl_", "ngl_", "ngli_"};
    for (size_t i = 0; i < NGLI_ARRAY_NB(reserved); i++) {
        if (!strncmp(key, reserved[i], strlen(reserved[i])))
            goto fail;
    }
    return 0;

fail:
    LOG(ERROR, "%s: invalid or reserved GLSL resource key \"%s\" (maximum %d characters)",
        label, key, NGLI_SHADER2D_NAME_LEN - 1);
    return NGL_ERROR_INVALID_USAGE;
}

void ngli_shader2d_init(struct shader2d *s)
{
    *s = (struct shader2d){0};
}

static int rewrite_entry(struct bstr *out, const char *source, const char *entry)
{
    struct ngli_glsl_lexer *lexer = ngli_glsl_lexer_create(source);
    const char *segment = source;
    struct ngli_glsl_token token;
    while (ngli_glsl_lexer_next(lexer, &token)) {
        if (token.directive || strcmp(token.text, "main"))
            continue;
        ngli_bstr_write(out, segment, (size_t)(token.start - segment));
        ngli_bstr_print(out, entry);
        for (size_t i = 0; i < token.spliced_lines; i++)
            ngli_bstr_print(out, "\\\n");
        segment = token.end;
    }
    ngli_bstr_print(out, segment);
    ngli_glsl_lexer_freep(&lexer);
    return ngli_bstr_check(out);
}

int ngli_shader2d_write_builtin(struct bstr *out, const char *source, const char *entry, const char *prefix)
{
    struct bstr *expanded = ngli_bstr_create();
    if (!expanded)
        return NGL_ERROR_MEMORY;

    const char *segment = source;
    for (const char *p = segment; *p; p++) {
        if (*p != '$')
            continue;
        ngli_bstr_write(expanded, segment, (size_t)(p - segment));
        ngli_bstr_print(expanded, prefix);
        segment = p + 1;
    }
    ngli_bstr_print(expanded, segment);
    int ret = ngli_bstr_check(expanded);
    if (ret >= 0)
        ret = rewrite_entry(out, ngli_bstr_strptr(expanded), entry);
    ngli_bstr_freep(&expanded);
    return ret;
}

int ngli_shader2d_write_source(struct bstr *out, const char *source, const char *entry,
                               bool multiple_outputs, int source_id, const char *label)
{
    ngli_bstr_printf(out, "%s %s(const ngl_FragmentInput frag);\n", multiple_outputs ? "void" : "vec4", entry);
    LOG(DEBUG, "GLSL source %d: %s", source_id, label);
    ngli_bstr_printf(out, "#line 1 %d\n", source_id);
    int ret = rewrite_entry(out, source, entry);
    ngli_bstr_print(out, "\n#line 1 0\n");
    return ret;
}

int ngli_shader2d_add_texture(struct shader2d *s, const char *name)
{
    for (size_t i = 0; i < s->textures.count; i++)
        if (!strcmp(s->textures.data[i], name))
            return 0;
    char *copy = ngli_strdup(name);
    if (!copy)
        return NGL_ERROR_MEMORY;
    int ret = ngli_darray_try_push(&s->textures, copy);
    if (ret < 0)
        ngli_free(copy);
    return ret;
}

void ngli_shader2d_write_header(const struct shader2d *s, struct bstr *out)
{
    ngli_bstr_print(out,
        "struct ngl_FragmentInput {\n"
        "    vec2 rect_uv;\n"
        "    vec2 rect_size;\n"
        "    vec2 content_uv;\n"
        "    vec2 canvas_px;\n"
        "    vec2 _uv;\n"
        "    vec2 _coord;\n"
        "    vec4 _lin;\n"
        "    vec2 _off;\n"
        "    vec4 _content_lin;\n"
        "    vec2 _content_off;\n");
    for (size_t i = 0; i < s->textures.count; i++)
        ngli_bstr_printf(out, "    vec2 _%s_coord;\n", s->textures.data[i]);
    ngli_bstr_print(out,
        "};\n"
        "vec2 ngl_content_uv(const ngl_FragmentInput frag, vec2 p) {\n"
        "    return mat2(frag._content_lin.xy, frag._content_lin.zw) * p + frag._content_off;\n"
        "}\n");
    for (size_t i = 0; i < s->textures.count; i++) {
        const char *k = s->textures.data[i];
        ngli_bstr_printf(out,
            "vec2 %s_uv(const ngl_FragmentInput frag) { return frag._uv; }\n"
            "vec2 %s_uv(const ngl_FragmentInput frag, vec2 p) {\n"
            "    return mat2(frag._lin.xy, frag._lin.zw) * p + frag._off;\n"
            "}\n", k, k);
        ngli_bstr_printf(out,
            "vec2 %s_coord(const ngl_FragmentInput frag) { return frag._%s_coord; }\n"
            "vec2 %s_coord(const ngl_FragmentInput frag, vec2 uv) {\n"
            "    return (%s_coord_matrix * vec4(uv, 0.0, 1.0)).xy;\n"
            "}\n", k, k, k, k);
        ngli_bstr_printf(out,
            "vec4 %s_sample(const ngl_FragmentInput frag) { return ngl_texvideo(%s, %s_coord(frag)); }\n"
            "vec4 %s_sample(const ngl_FragmentInput frag, vec2 uv) { return ngl_texvideo(%s, %s_coord(frag, uv)); }\n",
            k, k, k, k, k, k);
    }
}

void ngli_shader2d_write_texture_helpers(struct bstr *out, const char *name)
{
    ngli_bstr_printf(out,
        "vec2 %s_uv(const ngl_FragmentInput frag) { return frag._uv; }\n"
        "vec2 %s_uv(const ngl_FragmentInput frag, vec2 p) {\n"
        "    return mat2(frag._lin.xy, frag._lin.zw) * p + frag._off;\n"
        "}\n"
        "vec2 %s_coord(const ngl_FragmentInput frag) { return frag._coord; }\n"
        "vec2 %s_coord(const ngl_FragmentInput frag, vec2 uv) {\n"
        "    return (%s_coord_matrix * vec4(uv, 0.0, 1.0)).xy;\n"
        "}\n"
        "vec4 %s_sample(const ngl_FragmentInput frag) { return ngl_texvideo(%s, %s_coord(frag)); }\n"
        "vec4 %s_sample(const ngl_FragmentInput frag, vec2 uv) { return ngl_texvideo(%s, %s_coord(frag, uv)); }\n",
        name, name, name, name, name, name, name, name, name, name, name);
}

void ngli_shader2d_write_fragment_input_textures(const struct shader2d *s, struct bstr *out,
                                                 const struct shader2d_role *role)
{
    for (size_t i = 0; i < s->textures.count; i++) {
        const char *name = s->textures.data[i];
        ngli_bstr_printf(out, "    frag._%s_coord = ngli_v_%s_%s_coord;\n", name, role->name, name);
    }
}

int ngli_shader2d_write_vertex(struct shader2d *s, struct bstr *out, const char *base,
                               const struct ngpu_pgcraft_iovar *vars, size_t var_count,
                               const struct shader2d_role *roles, size_t role_count)
{
    ngli_darray_reset(&s->varyings);
    for (size_t i = 0; i < var_count; i++) {
        if (ngli_darray_try_push(&s->varyings, vars[i]) < 0)
            return NGL_ERROR_MEMORY;
    }
    ngli_bstr_print(out, base);
    ngli_bstr_print(out, "\nvoid main() {\n    ngli_vertex();\n");
    for (size_t i = 0; i < s->textures.count; i++) {
        const char *name = s->textures.data[i];
        for (size_t j = 0; j < role_count; j++) {
            struct ngpu_pgcraft_iovar var = {.type = NGPU_TYPE_VEC2};
            snprintf(var.name, sizeof(var.name), "ngli_v_%s_%s_coord", roles[j].name, name);
            int ret = ngli_darray_try_push(&s->varyings, var);
            if (ret < 0)
                return ret;
            ngli_bstr_printf(out, "    %s = (%s_coord_matrix * vec4(%s, 0.0, 1.0)).xy;\n",
                             var.name, name, roles[j].uv);
        }
    }
    ngli_bstr_print(out, "}\n");
    return ngli_bstr_check(out);
}

void ngli_shader2d_reset(struct shader2d *s)
{
    for (size_t i = 0; i < s->textures.count; i++)
        ngli_free(s->textures.data[i]);
    ngli_darray_reset(&s->textures);
    ngli_darray_reset(&s->varyings);
}
