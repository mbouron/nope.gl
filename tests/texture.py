#
# Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
# Copyright 2020-2022 GoPro Inc.
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

import array

import pynopegl as ngl
from pynopegl_utils.misc import load_media
from pynopegl_utils.tests.cmp_png import test_png


# ── Buffer → Texture2D ──────────────────────────────────────────────────────


def _make_buffer_texture(w, h):
    n = w * h
    data = array.array("B", [i * 255 // n for i in range(n)])
    buf = ngl.BufferUByte(data=data)
    return ngl.Texture2D(
        width=w, height=h, data_src=buf, min_filter="nearest", mag_filter="nearest", label="tex0"
    )


def _draw_buffer_rect(w, h, rect_w, rect_h):
    texture = _make_buffer_texture(w, h)
    fill = ngl.CustomFill(
        color_glsl="""
            float color = texture(tex0, uv).r;
            return vec4(color, 0.0, 0.0, 1.0);
        """,
        frag_resources=[texture],
    )
    return ngl.DrawRect(rect=(0, 0, rect_w, rect_h), fill=fill)


@test_png(width=128, height=128)
@ngl.scene(controls=dict(w=ngl.scene.Range(range=[1, 128]), h=ngl.scene.Range(range=[1, 128])))
def texture_data(cfg: ngl.SceneCfg, w=4, h=5):
    cfg.aspect_ratio = (128, 128)
    return _draw_buffer_rect(w, h, 128, 128)


@test_png(width=128, height=128)
@ngl.scene(controls=dict(h=ngl.scene.Range(range=[1, 32])))
def texture_data_unaligned_row(cfg: ngl.SceneCfg, h=32):
    """Tests upload of buffers with rows that are not 4-byte aligned"""
    cfg.aspect_ratio = (128, 128)
    return _draw_buffer_rect(1, h, 128, 128)


@test_png(width=128, height=128, keyframes=(0, 1, 0))
@ngl.scene(controls=dict(w=ngl.scene.Range(range=[1, 128]), h=ngl.scene.Range(range=[1, 128])))
def texture_data_seek_timeranges(cfg: ngl.SceneCfg, w=4, h=5):
    cfg.aspect_ratio = (128, 128)
    cfg.duration = 1
    return ngl.TimeRangeFilter(_draw_buffer_rect(w, h, 128, 128), end=1)


# ── Cubemap ──────────────────────────────────────────────────────────────────


def _get_texture_cubemap(mipmap_filter="none"):
    n = 64
    p = n * n
    cb_data = array.array(
        "B",
        (255, 0, 0, 255) * p
        + (0, 255, 0, 255) * p
        + (0, 0, 255, 255) * p
        + (255, 255, 0, 255) * p
        + (0, 255, 255, 255) * p
        + (255, 0, 255, 255) * p,
    )
    cb_buffer = ngl.BufferUBVec4(data=cb_data)
    cube = ngl.TextureCube(
        size=n,
        min_filter="linear",
        mag_filter="linear",
        data_src=cb_buffer,
        mipmap_filter=mipmap_filter,
        label="tex0",
    )
    return cube


_CUBEMAP_SAMPLE_GLSL = "return texture(tex0, vec3(uv * 2.0 - 1.0, 0.5));"


def _cubemap_display_rect(cube, w, h, glsl=_CUBEMAP_SAMPLE_GLSL):
    fill = ngl.CustomFill(
        color_glsl=glsl,
        frag_resources=[cube],
    )
    return ngl.DrawRect(rect=(0, 0, w, h), fill=fill)


@test_png(width=800, height=800)
@ngl.scene()
def texture_cubemap(cfg: ngl.SceneCfg):
    cfg.aspect_ratio = (1, 1)
    cube = _get_texture_cubemap()
    return _cubemap_display_rect(cube, 800, 800)


@test_png(width=800, height=800, threshold=1)
@ngl.scene()
def texture_cubemap_mipmap(cfg: ngl.SceneCfg):
    cfg.aspect_ratio = (1, 1)
    cube = _get_texture_cubemap(mipmap_filter="nearest")
    return _cubemap_display_rect(
        cube,
        800,
        800,
        glsl="return textureLod(tex0, vec3(uv * 2.0 - 1.0, 0.5), 1.0);",
    )


def _get_texture_cubemap_from_mrt_scene(cfg: ngl.SceneCfg, samples=0):
    cfg.aspect_ratio = (1, 1)
    cube = ngl.TextureCube(size=64, min_filter="linear", mag_filter="linear", label="tex0")
    fill = ngl.CustomFill(
        color_glsl="""
            ngl_out_color[0] = vec4(1.0, 0.0, 0.0, 1.0); // right
            ngl_out_color[1] = vec4(0.0, 1.0, 0.0, 1.0); // left
            ngl_out_color[2] = vec4(0.0, 0.0, 1.0, 1.0); // top
            ngl_out_color[3] = vec4(1.0, 1.0, 0.0, 1.0); // bottom
            ngl_out_color[4] = vec4(0.0, 1.0, 1.0, 1.0); // back
            ngl_out_color[5] = vec4(1.0, 0.0, 1.0, 1.0); // front
        """,
        nb_frag_output=6,
    )
    draw = ngl.DrawRect(rect=(0, 0, 64, 64), fill=fill)
    rtt = ngl.RenderToTexture(draw, [cube], samples=samples)
    display = _cubemap_display_rect(cube, 800, 800)
    return ngl.Group(children=[rtt, display])


def _get_texture_cubemap_from_mrt_scene_2_pass(cfg: ngl.SceneCfg, samples=0):
    cfg.aspect_ratio = (1, 1)
    cube = ngl.TextureCube(size=64, min_filter="linear", mag_filter="linear", label="tex0")

    # Pass 1: first 2 faces
    fill_x2 = ngl.CustomFill(
        color_glsl="""
            ngl_out_color[0] = vec4(1.0, 0.0, 0.0, 1.0); // right
            ngl_out_color[1] = vec4(0.0, 1.0, 0.0, 1.0); // left
        """,
        nb_frag_output=2,
    )
    draw_x2 = ngl.DrawRect(rect=(0, 0, 64, 64), fill=fill_x2)
    views_x2 = [ngl.TextureView(cube, layer) for layer in range(2)]
    rtt_x2 = ngl.RenderToTexture(draw_x2, views_x2, samples=samples)

    # Pass 2: last 4 faces
    fill_x4 = ngl.CustomFill(
        color_glsl="""
            ngl_out_color[0] = vec4(0.0, 0.0, 1.0, 1.0); // top
            ngl_out_color[1] = vec4(1.0, 1.0, 0.0, 1.0); // bottom
            ngl_out_color[2] = vec4(0.0, 1.0, 1.0, 1.0); // back
            ngl_out_color[3] = vec4(1.0, 0.0, 1.0, 1.0); // front
        """,
        nb_frag_output=4,
    )
    draw_x4 = ngl.DrawRect(rect=(0, 0, 64, 64), fill=fill_x4)
    views_x4 = [ngl.TextureView(cube, layer) for layer in range(2, 6)]
    rtt_x4 = ngl.RenderToTexture(draw_x4, views_x4, samples=samples)

    display = _cubemap_display_rect(cube, 800, 800)
    return ngl.Group(children=[rtt_x2, rtt_x4, display])


@test_png(width=800, height=800)
@ngl.scene()
def texture_cubemap_from_mrt(cfg: ngl.SceneCfg):
    return _get_texture_cubemap_from_mrt_scene(cfg)


@test_png(width=800, height=800)
@ngl.scene()
def texture_cubemap_from_mrt_msaa(cfg: ngl.SceneCfg):
    return _get_texture_cubemap_from_mrt_scene(cfg, 4)


@test_png(width=800, height=800)
@ngl.scene()
def texture_cubemap_from_mrt_2_pass(cfg: ngl.SceneCfg):
    return _get_texture_cubemap_from_mrt_scene_2_pass(cfg)


@test_png(width=800, height=800)
@ngl.scene()
def texture_cubemap_from_mrt_2_pass_msaa(cfg: ngl.SceneCfg):
    return _get_texture_cubemap_from_mrt_scene_2_pass(cfg, 4)


# ── Texture2DArray ───────────────────────────────────────────────────────────

_TEXTURE2D_ARRAY_SAMPLE_GLSL = """\
    return texture(tex0, vec3(uv, 0.0))
         + texture(tex0, vec3(uv, 1.0))
         + texture(tex0, vec3(uv, 2.0));
"""


def _get_texture_2d_array(cfg: ngl.SceneCfg, mipmap_filter="none"):
    width, height, depth = 9, 9, 3
    n = width * height
    data = array.array("B")
    for i in cfg.rng.sample(range(n), n):
        data.extend([i * 255 // n, 0, 0, 255])
    for i in cfg.rng.sample(range(n), n):
        data.extend([0, i * 255 // n, 0, 255])
    for i in cfg.rng.sample(range(n), n):
        data.extend([0, 0, i * 255 // n, 255])
    texture_buffer = ngl.BufferUBVec4(data=data)
    texture = ngl.Texture2DArray(
        width=width,
        height=height,
        depth=depth,
        data_src=texture_buffer,
        mipmap_filter=mipmap_filter,
        min_filter="nearest",
        mag_filter="nearest",
        label="tex0",
    )
    return texture


@test_png(width=320, height=320)
@ngl.scene()
def texture_2d_array(cfg: ngl.SceneCfg):
    cfg.aspect_ratio = (1, 1)
    texture = _get_texture_2d_array(cfg)
    fill = ngl.CustomFill(
        color_glsl=_TEXTURE2D_ARRAY_SAMPLE_GLSL,
        frag_resources=[texture],
    )
    return ngl.DrawRect(rect=(0, 0, 320, 320), fill=fill)


@test_png(width=1280, height=720, threshold=4)
@ngl.scene()
def texture_2d_array_mipmap(cfg: ngl.SceneCfg):
    cfg.aspect_ratio = (16, 9)
    texture = _get_texture_2d_array(cfg, mipmap_filter="nearest")
    fill = ngl.CustomFill(
        color_glsl="""
            return textureLod(tex0, vec3(uv, 0.0), 2.0)
                 + textureLod(tex0, vec3(uv, 1.0), 2.0)
                 + textureLod(tex0, vec3(uv, 2.0), 2.0);
        """,
        frag_resources=[texture],
    )
    return ngl.DrawRect(rect=(0, 0, 1280, 720), fill=fill)


_STEPS = 4

_MRT_FILL_GLSL = """\
    float x = floor(uv.x * steps) / steps;
    x = uv.y < 0.5 ? x : 1.0 - x;
    ngl_out_color[0] = vec4(x, 0.0, 0.0, 1.0);
    ngl_out_color[1] = vec4(0.0, x, 0.0, 1.0);
    ngl_out_color[2] = vec4(0.0, 0.0, x, 1.0);
"""


def _get_texture_2d_array_from_mrt_scene(cfg: ngl.SceneCfg, samples=0):
    cfg.aspect_ratio = (1, 1)
    depth = 3
    texture = ngl.Texture2DArray(
        width=64, height=64, depth=depth, min_filter="nearest", mag_filter="nearest", label="tex0"
    )
    fill = ngl.CustomFill(
        color_glsl=_MRT_FILL_GLSL,
        frag_resources=[ngl.UniformFloat(value=_STEPS, label="steps")],
        nb_frag_output=depth,
    )
    draw = ngl.DrawRect(rect=(0, 0, 64, 64), fill=fill)
    rtt = ngl.RenderToTexture(draw, [texture], samples=samples)

    display_fill = ngl.CustomFill(
        color_glsl=_TEXTURE2D_ARRAY_SAMPLE_GLSL,
        frag_resources=[texture],
    )
    display = ngl.DrawRect(rect=(0, 0, 128, 128), fill=display_fill)
    return ngl.Group(children=[rtt, display])


@test_png(width=128, height=128, threshold=1)
@ngl.scene()
def texture_2d_array_from_mrt(cfg: ngl.SceneCfg):
    return _get_texture_2d_array_from_mrt_scene(cfg)


@test_png(width=128, height=128, threshold=1)
@ngl.scene()
def texture_2d_array_from_mrt_msaa(cfg: ngl.SceneCfg):
    return _get_texture_2d_array_from_mrt_scene(cfg, 4)


# ── Texture3D ────────────────────────────────────────────────────────────────

_TEXTURE3D_SAMPLE_GLSL = """\
    return texture(tex0, vec3(uv, 0.0))
         + texture(tex0, vec3(uv, 0.5))
         + texture(tex0, vec3(uv, 1.0));
"""


@test_png(width=400, height=400)
@ngl.scene()
def texture_3d(cfg: ngl.SceneCfg):
    cfg.aspect_ratio = (1, 1)

    width, height, depth = 9, 9, 3
    n = width * height
    data = array.array("B")
    for i in cfg.rng.sample(range(n), n):
        data.extend([i * 255 // n, 0, 0, 255])
    for i in cfg.rng.sample(range(n), n):
        data.extend([0, i * 255 // n, 0, 255])
    for i in cfg.rng.sample(range(n), n):
        data.extend([0, 0, i * 255 // n, 255])
    texture_buffer = ngl.BufferUBVec4(data=data)
    texture = ngl.Texture3D(
        width=width,
        height=height,
        depth=depth,
        data_src=texture_buffer,
        min_filter="nearest",
        mag_filter="nearest",
        label="tex0",
    )

    fill = ngl.CustomFill(
        color_glsl=_TEXTURE3D_SAMPLE_GLSL,
        frag_resources=[texture],
    )
    return ngl.DrawRect(rect=(0, 0, 400, 400), fill=fill)


def _get_texture_3d_from_mrt_scene(cfg: ngl.SceneCfg, samples=0):
    cfg.aspect_ratio = (1, 1)
    depth = 3
    texture = ngl.Texture3D(
        width=64, height=64, depth=depth, min_filter="nearest", mag_filter="nearest", label="tex0"
    )
    fill = ngl.CustomFill(
        color_glsl=_MRT_FILL_GLSL,
        frag_resources=[ngl.UniformFloat(value=_STEPS, label="steps")],
        nb_frag_output=depth,
    )
    draw = ngl.DrawRect(rect=(0, 0, 64, 64), fill=fill)
    rtt = ngl.RenderToTexture(draw, [texture], samples=samples)

    display_fill = ngl.CustomFill(
        color_glsl=_TEXTURE3D_SAMPLE_GLSL,
        frag_resources=[texture],
    )
    display = ngl.DrawRect(rect=(0, 0, 128, 128), fill=display_fill)
    return ngl.Group(children=[rtt, display])


@test_png(width=128, height=128, threshold=1)
@ngl.scene()
def texture_3d_from_mrt(cfg: ngl.SceneCfg):
    return _get_texture_3d_from_mrt_scene(cfg)


@test_png(width=128, height=128, threshold=1)
@ngl.scene()
def texture_3d_from_mrt_msaa(cfg: ngl.SceneCfg):
    return _get_texture_3d_from_mrt_scene(cfg, 4)


# ── Mipmap ───────────────────────────────────────────────────────────────────

_N = 8


@test_png(width=128, height=128, threshold=1)
@ngl.scene()
def texture_mipmap(cfg: ngl.SceneCfg):
    cfg.aspect_ratio = (1, 1)
    black = (0, 0, 0, 255)
    white = (255, 255, 255, 255)
    p = _N // 2
    cb_data = array.array(
        "B",
        ((black + white) * p + (white + black) * p) * p,
    )
    cb_buffer = ngl.BufferUBVec4(data=cb_data)

    texture = ngl.Texture2D(
        width=_N,
        height=_N,
        min_filter="nearest",
        mipmap_filter="linear",
        data_src=cb_buffer,
        label="tex0",
    )

    fill = ngl.CustomFill(
        color_glsl="return textureLod(tex0, uv, 0.5);",
        frag_resources=[texture],
    )
    return ngl.DrawRect(rect=(0, 0, 128, 128), fill=fill)


# ── Reframing ────────────────────────────────────────────────────────────────


def _get_texture_reframing_scene(d, wrap="default"):
    media = load_media("hamster")
    anim_pos_kf = [
        ngl.AnimKeyFrameVec3(0, (-1, -1, 0)),
        ngl.AnimKeyFrameVec3(d / 2, (1, 1, 0)),
        ngl.AnimKeyFrameVec3(d, (-1, -1, 0)),
    ]
    anim_angle_kf = [ngl.AnimKeyFrameFloat(0, 0), ngl.AnimKeyFrameFloat(d, 360)]
    tex = ngl.Texture2D(data_src=ngl.Media(filename=media.filename))
    tex = ngl.Scale(tex, factors=(1.2, 1.2, 1))
    tex = ngl.Rotate(tex, angle=ngl.AnimatedFloat(anim_angle_kf))
    tex = ngl.Translate(tex, vector=ngl.AnimatedVec3(anim_pos_kf))
    fill = ngl.TextureFill(texture=tex, wrap=wrap)
    return ngl.DrawRect(rect=(32, 32, 256, 256), fill=fill)


@test_png(width=320, height=320, keyframes=5, threshold=1)
@ngl.scene()
def texture_reframing(cfg: ngl.SceneCfg):
    cfg.aspect_ratio = (1, 1)
    cfg.duration = d = 3
    return _get_texture_reframing_scene(d)


@test_png(width=320, height=320, keyframes=5, threshold=1)
@ngl.scene()
def texture_reframing_wrap_discard(cfg: ngl.SceneCfg):
    cfg.aspect_ratio = (1, 1)
    cfg.duration = d = 3
    return _get_texture_reframing_scene(d, "discard")


# ── Masking ──────────────────────────────────────────────────────────────────


@test_png(width=640, height=360, keyframes=5, threshold=1)
@ngl.scene()
def texture_masking(cfg: ngl.SceneCfg):
    media = load_media("cat")

    cfg.aspect_ratio = (media.width, media.height)
    cfg.duration = d = media.duration

    w, h = 640, 360

    content = ngl.Texture2D(data_src=ngl.Media(filename=media.filename), label="content")

    # Procedural circular mask rendered to texture (radius grows over time)
    mask_fill = ngl.CustomFill(
        color_glsl="""
            vec2 center = vec2(0.5, 0.5);
            float dist = length(uv - center);
            float progress = t / duration;
            float radius = mix(0.05, 0.8, progress * progress);
            float alpha = 1.0 - smoothstep(radius - 0.01, radius + 0.01, dist);
            return vec4(alpha);
        """,
        frag_resources=[
            ngl.Time(label="t"),
            ngl.UniformFloat(value=d, label="duration"),
        ],
    )
    mask_draw = ngl.DrawRect(rect=(0, 0, w, h), fill=mask_fill)
    mask_tex = ngl.Texture2D(width=w, height=h, min_filter="linear", mag_filter="linear", label="mask")
    rtt = ngl.RenderToTexture(mask_draw, [mask_tex])

    # Composite: background gradient, then masked content on top
    bg = ngl.DrawRect(rect=(0, 0, w, h), fill=ngl.Gradient4Fill())

    fg = ngl.DrawRect(
        rect=(0, 0, w, h),
        fill=ngl.CustomFill(
            color_glsl="""
                vec4 c = ngl_texvideo(content, tex_coord);
                float a = texture(mask, uv).r;
                return vec4(c.rgb, c.a * a);
            """,
            frag_resources=[content, mask_tex],
        ),
        blending="src_over",
    )

    return ngl.Group(children=[rtt, bg, fg])
