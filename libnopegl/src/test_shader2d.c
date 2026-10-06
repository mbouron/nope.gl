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
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "shader2d.h"
#include "utils/bstr.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static int test_assembly(void)
{
    struct shader2d shader;
    ngli_shader2d_init(&shader);
    char name[] = "source";
    CHECK(ngli_shader2d_add_texture(&shader, name) == 0);
    name[0] = 'x'; /* The assembler owns a copy of the name. */
    CHECK(ngli_shader2d_add_texture(&shader, "source") == 0);
    CHECK(shader.textures.count == 1);
    struct bstr *out = ngli_bstr_create();
    CHECK(out);
    ngli_shader2d_write_header(&shader, out);
    const char *header = ngli_bstr_strptr(out);
    CHECK(strstr(header, "return ngl_texvideo(source, frag._source_uv);"));
    CHECK(strstr(header, "(source_coord_matrix * vec4(uv, 0.0, 1.0)).xy"));
    CHECK(strstr(header, "(source_coord_matrix * vec4(ngl_tex_uv_source(frag, p), 0.0, 1.0)).xy"));

    const struct shader2d_role roles[] = {
        {.name = "front", .uv = "front_uv"},
        {.name = "back",  .uv = "back_uv"},
    };
    ngli_bstr_clear(out);
    ngli_shader2d_write_fragment_input_textures(&shader, out, &roles[1]);
    CHECK(!strcmp(ngli_bstr_strptr(out), "    frag._source_uv = ngli_v_back_source_coord;\n"));

    const struct ngpu_pgcraft_iovar vars[] = {
        {.name = "front_uv", .type = NGPU_TYPE_VEC2},
    };
    ngli_bstr_clear(out);
    ngli_bstr_print(out, "// caller prefix\n");
    CHECK(ngli_shader2d_write_vertex(&shader, out, "void ngli_vertex() {}\n", vars, 1, roles, 2) == 0);
    CHECK(!strncmp(ngli_bstr_strptr(out), "// caller prefix\n", strlen("// caller prefix\n")));
    CHECK(shader.varyings.count == 3);
    CHECK(!strcmp(shader.varyings.data[0].name, "front_uv"));
    CHECK(strstr(ngli_bstr_strptr(out), "ngli_v_front_source_coord = (source_coord_matrix * vec4(front_uv, 0.0, 1.0)).xy"));
    CHECK(strstr(ngli_bstr_strptr(out), "ngli_v_back_source_coord = (source_coord_matrix * vec4(back_uv, 0.0, 1.0)).xy"));
    ngli_bstr_clear(out);
    CHECK(ngli_shader2d_write_vertex(&shader, out, "void ngli_vertex() {}\n", vars, 1, roles, 1) == 0);
    CHECK(shader.varyings.count == 2);
    CHECK(!strstr(ngli_bstr_strptr(out), "ngli_v_back_source_coord"));
    ngli_shader2d_reset(&shader);
    CHECK(!shader.textures.count && !shader.varyings.count);
    CHECK(strstr(ngli_bstr_strptr(out), "ngli_v_front_source_coord"));
    ngli_bstr_freep(&out);
    CHECK(ngli_shader2d_add_texture(&shader, "source") == 0);
    CHECK(shader.textures.count == 1);
    ngli_shader2d_reset(&shader);
    return 0;
}

int main(void)
{
    CHECK(test_assembly() == 0);
    struct bstr *out = ngli_bstr_create();
    CHECK(out);

    /* Directives are preserved verbatim; GLSL validation belongs to the compiler. */
    static const char * const directives[] = {
        "#define main() main()\n",
        "#define main foo\n#undef main\n",
        "#define main() main() { ngl_out_color = vec4(1.0); } void unused()\n",
        "#define X(main) main\n", "#undef main\n",
        "#if main\n#endif\n", "#ifdef main\n#endif\n", "#ifndef main\n#endif\n",
        "#if 0\n#elif main\n#endif\n",
        "#if 0\n#define main unused\n#endif\n",
        "#define X \\\n main\n", "#define X \\\r\n main\r\n",
        "#define X /* first\nsecond */ main\n",
        "#define ma\\\nin unused\n",
        "#version 450\n", "#extension GL_EXT_test : enable\n",
    };
    for (size_t i = 0; i < sizeof(directives) / sizeof(*directives); i++) {
        ngli_bstr_clear(out);
        CHECK(ngli_shader2d_write_builtin(out, directives[i], "ngli_entry", "ngli_test_") == 0);
        CHECK(!strcmp(ngli_bstr_strptr(out), directives[i]));
    }

    const char *source =
        "// main ngl_sample_comment\r\n/* main ngl_tex_uv_comment */\n"
        "#define domain main_color // main\n"
        "#define SAMPLE(c) \\\n ngl_sample_first(c)\n"
        "#if 0\nvec4 unused = ngl_sample_inactive(frag);\n#endif\n"
        "#pragma main\n#if 0\n#error main\n#endif\n"
        "vec4 main(const ngl_FragmentInput pos);\n"
        "vec4 main(const ngl_FragmentInput pos) { return SAMPLE(pos); }\n";
    ngli_bstr_clear(out);
    CHECK(ngli_shader2d_write_builtin(out, source, "ngli_entry", "ngli_test_") == 0);
    const char *result = ngli_bstr_strptr(out);
    CHECK(strstr(result, "// main ngl_sample_comment\r\n/* main ngl_tex_uv_comment */\n"));
    CHECK(strstr(result, "#define domain main_color // main\n"));
    CHECK(strstr(result, "vec4 ngli_entry(const ngl_FragmentInput pos);\n"));
    CHECK(strstr(result, "vec4 ngli_entry(const ngl_FragmentInput pos) { return SAMPLE(pos); }\n"));

    ngli_bstr_clear(out);
    CHECK(ngli_shader2d_write_source(out, "vec4 main(const ngl_FragmentInput c) { return vec4(1.0); }",
                                     "ngli_fill", false, 37, "test\n#error must not enter GLSL") == 0);
    CHECK(strstr(ngli_bstr_strptr(out), "vec4 ngli_fill(const ngl_FragmentInput frag);\n"));
    CHECK(strstr(ngli_bstr_strptr(out), "#line 1 37\nvec4 ngli_fill("));
    CHECK(strstr(ngli_bstr_strptr(out), "#line 1 0\n"));
    CHECK(!strstr(ngli_bstr_strptr(out), "#error"));

    ngli_bstr_clear(out);
    CHECK(ngli_shader2d_write_builtin(out, "vec4 main() { return $color; }", "ngli_fill", "ngli_fill_") == 0);
    CHECK(!strcmp(ngli_bstr_strptr(out), "vec4 ngli_fill() { return ngli_fill_color; }"));

    const char * const keys[] = {"rect_uv", "rect_px", "content_uv", "tex_coord", "canvas_px"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
        CHECK(ngli_shader2d_check_key(keys[i], "paint") == 0);
    }
    CHECK(ngli_shader2d_check_key("input", "paint") == 0);

    /* Storage extraction must preserve zero crop scales and independent offsets. */
    struct ngli_image_params params = {
        .width = 16, .height = 16, .layout = NGLI_IMAGE_LAYOUT_DEFAULT,
        .coordinates_matrix.m = {0.f, 0.f, 0.f, 0.f, 0.f, -0.75f, 0.f, 0.f,
                                  0.f, 0.f, 1.f, 0.f, 0.25f, 0.875f, 0.f, 1.f},
    };
    struct ngli_image *image = ngli_image_create(&params);
    CHECK(image);
    float storage[4];
    ngli_image_get_coordinates_scale_offset(image, storage, storage + 2);
    CHECK(storage[0] == 0.f && storage[1] == -0.75f && storage[2] == 0.25f && storage[3] == 0.875f);
    ngli_image_unrefp(&image);
    ngli_bstr_freep(&out);
    return 0;
}
