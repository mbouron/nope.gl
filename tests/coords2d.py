#
# Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

"""
Test 2D coordinate conventions using tiles of 64x64 pixels aligned to pixel
boundaries. One canvas unit equals one pixel, and pixel (x, y) is centered at
(x + 0.5, y + 0.5).

The small tiles make coordinate gradients change by about 4 color levels per
pixel, so a half-pixel shift exceeds the comparison tolerance.

The sampling tests draw textures at their original size, mapping one texel
to each pixel. Each tile must show the texture unchanged, apart from an
integer texel shift. The tiles fill the canvas without gaps, so incorrect
coverage causes blending at tile boundaries.
"""

import array
import textwrap

import pynopegl as ngl
from pynopegl_utils.tests.cmp_render import test_render

T = 64


def _tile(col: int, row: int, cols: int = 1, rows: int = 1):
    return (col * T, row * T, cols * T, rows * T)


def _paint(expr: str, **kwargs) -> ngl.CustomPaint:
    return ngl.CustomPaint(glsl=f"vec4 main(const ngl_FragmentInput frag) {{ return {expr}; }}", **kwargs)


# RG encode canvas position; B is 0.5 at half-integer pixel centers.
_CANVAS_PX = "vec4(frag.canvas_px / 256.0, 0.5 * (fract(frag.canvas_px.x) + fract(frag.canvas_px.y)), 1.0)"


@test_render()
@ngl.scene(width=4 * T, height=2 * T)
def coords2d_paint_inputs(cfg: ngl.SceneCfg):
    """Coordinates exposed to paint shaders: rect_uv, rect_size, canvas_px, content_uv and ngl_tex_coord_tex().

    Also checks that content transforms affect the fill coordinates but not the stroke coordinates.
    """
    cfg.duration = 1.0

    content_zoom = 2.0
    content_translate = (0.125, -0.0625)

    rect_uv = ngl.DrawRect2D(rect=_tile(0, 0), fill=_paint("vec4(frag.rect_uv, 0.0, 1.0)"))
    rect_px = ngl.DrawRect2D(
        rect=_tile(1, 0, 2), fill=_paint("vec4(fract(frag.rect_uv * frag.rect_size / 64.0), 0.0, 1.0)")
    )
    canvas_px = ngl.Group2D(
        children=[ngl.DrawRect2D(rect=(0, 0, T / 2, T / 2), fill=_paint(_CANVAS_PX))],
        translate=(3 * T, 0),
        scale=(2.0, 2.0),
    )

    content_uv = ngl.DrawRect2D(
        rect=_tile(0, 1),
        fill=_paint("vec4(frag.content_uv, 0.0, 1.0)"),
        content_zoom=content_zoom,
        content_translate=content_translate,
        content_orientation=90.0,
    )
    tex_coord_fill = ngl.DrawRect2D(
        rect=_tile(1, 1),
        fill=_paint(
            "vec4(ngl_tex_coord_tex(frag), 0.0, 1.0)",
            resources={"tex": ngl.Texture2D(width=2 * T, height=T)},
            scaling="fill",
        ),
    )
    tex_coord_func = ngl.DrawRect2D(
        rect=_tile(2, 1),
        fill=_paint(
            "vec4(ngl_tex_coord_tex(frag, frag.rect_uv), 0.0, 1.0)",
            resources={"tex": ngl.Texture2D(width=2 * T, height=T)},
            scaling="fill",
        ),
        content_zoom=content_zoom,
        content_translate=content_translate,
    )
    stroke_content_uv = ngl.DrawRect2D(
        rect=_tile(3, 1),
        fill=_paint("vec4(frag.content_uv, 0.0, 1.0)"),
        stroke=ngl.Stroke2D(paint=_paint("vec4(frag.content_uv, 0.0, 1.0)"), width=T / 4, alignment="inside"),
        content_zoom=content_zoom,
        content_translate=content_translate,
    )

    return ngl.Canvas2D(
        width=4 * T,
        height=2 * T,
        children=[
            rect_uv,
            rect_px,
            canvas_px,
            content_uv,
            tex_coord_fill,
            tex_coord_func,
            stroke_content_uv,
        ],
    )


def _pattern_texture() -> ngl.Texture2D:
    """A texture where no two neighbouring texels are alike, to catch any shift or blend."""
    data = array.array("B")
    for y in range(T):
        for x in range(T):
            data.extend(((x * 7 + y * 3) & 255, (y * 5 + 17) & 255, ((x * 11) ^ (y * 13)) & 255, 255))
    return ngl.Texture2D(
        width=T,
        height=T,
        data_src=ngl.BufferUBVec4(data=data),
        min_filter="linear",
        mag_filter="linear",
    )


def _pattern_rect(col: int, row: int, **kwargs) -> ngl.DrawRect2D:
    return ngl.DrawRect2D(rect=_tile(col, row), fill=ngl.TexturePaint(texture=_pattern_texture()), **kwargs)


_TEX_COORD_AT_RECT_UV = textwrap.dedent("""\
    vec4 main(const ngl_FragmentInput frag) {
        return ngl_texvideo(tex, ngl_tex_coord_tex(frag, frag.rect_uv));
    }
""")


@test_render()
@ngl.scene(width=4 * T, height=2 * T)
def coords2d_paint_sampling(cfg: ngl.SceneCfg):
    """Texel-for-pixel texture sampling through paints, offscreen canvases and content translates."""
    cfg.duration = 1.0

    # Integer texel offsets, so that the shifted texture is still sampled at texel centers
    shift = (16 / T, 8 / T)
    tex_coord_step = textwrap.dedent(f"""\
        vec4 main(const ngl_FragmentInput frag) {{
            vec2 step_tc = ngl_tex_coord_tex(frag, frag.rect_uv + vec2({shift[0]}, {shift[1]})) - ngl_tex_coord_tex(frag);
            return ngl_texvideo(tex, ngl_tex_coord_tex(frag) + step_tc);
        }}
    """)

    texture = ngl.Texture2D(width=T, height=T, min_filter="linear", mag_filter="linear")
    offscreen = ngl.OffscreenCanvas2D(
        children=[ngl.DrawRect2D(rect=(0, 0, T, T), fill=ngl.TexturePaint(texture=_pattern_texture()))],
        width=T,
        height=T,
        color_textures=[texture],
    )

    row0 = [
        _pattern_rect(0, 0),
        ngl.DrawRect2D(rect=_tile(1, 0), fill=ngl.TexturePaint(texture=texture)),
        ngl.DrawRect2D(rect=_tile(2, 0), fill=_paint("ngl_sample_tex(frag)", resources={"tex": texture})),
        ngl.DrawRect2D(rect=_tile(3, 0), fill=ngl.CustomPaint(glsl=_TEX_COORD_AT_RECT_UV, resources={"tex": texture})),
    ]

    row1 = [
        _pattern_rect(0, 1, content_translate=shift),
        ngl.DrawRect2D(rect=_tile(1, 1), fill=ngl.TexturePaint(texture=texture), content_translate=shift),
        ngl.DrawRect2D(
            rect=_tile(2, 1),
            fill=_paint("ngl_sample_tex(frag)", resources={"tex": texture}),
            content_translate=shift,
        ),
        ngl.DrawRect2D(rect=_tile(3, 1), fill=ngl.CustomPaint(glsl=tex_coord_step, resources={"tex": texture})),
    ]

    return ngl.Canvas2D(
        width=4 * T,
        height=2 * T,
        children=[offscreen, *row0, *row1],
    )


def _effect(rect, expr: str) -> ngl.Effect2D:
    content = ngl.DrawRect2D(rect=rect, fill=ngl.ColorPaint(color=(1.0, 1.0, 1.0, 1.0)))
    return ngl.Effect2D(
        children=[content],
        bounds="rect",
        rect=rect,
        shaders=[ngl.Effect2DShader(glsl=f"vec4 main(const ngl_FragmentInput frag) {{ return {expr}; }}")],
    )


@test_render()
@ngl.scene(width=4 * T, height=T)
def coords2d_effect_inputs(cfg: ngl.SceneCfg):
    """Coordinates exposed to effect shaders: rect_uv and canvas_px.

    The rect_uv case uses a rectangle partially outside the canvas.
    """
    cfg.duration = 1.0

    return ngl.Canvas2D(
        width=4 * T,
        height=T,
        children=[
            _effect((-T / 2, 0, 1.5 * T, T), "vec4(frag.rect_uv, 0.0, 1.0)"),
            _effect(_tile(1, 0, 3), _CANVAS_PX),
        ],
    )


_SAMPLE_INPUT = textwrap.dedent("""\
    vec4 main(const ngl_FragmentInput frag) {
        return ngl_sample_input(frag);
    }
""")

# Samples the input through the explicit helper, at the tex_coord of the effect rect_uv
_SAMPLE_INPUT_AT_RECT_UV = textwrap.dedent("""\
    vec4 main(const ngl_FragmentInput frag) {
        return ngl_texvideo(ngl_input, ngl_tex_coord_input(frag, frag.rect_uv));
    }
""")


def _passthrough_effect(children, glsl: str, **kwargs) -> ngl.Effect2D:
    return ngl.Effect2D(children=children, shaders=[ngl.Effect2DShader(glsl=glsl)], **kwargs)


@test_render()
@ngl.scene(width=4 * T, height=T)
def coords2d_effect_sampling(cfg: ngl.SceneCfg):
    """Texel-for-pixel input sampling through pass-through effects, with rect and canvas bounds."""
    cfg.duration = 1.0

    return ngl.Canvas2D(
        width=4 * T,
        height=T,
        children=[
            _passthrough_effect([_pattern_rect(0, 0)], _SAMPLE_INPUT, bounds="rect", rect=_tile(0, 0)),
            _passthrough_effect([_pattern_rect(1, 0)], _SAMPLE_INPUT_AT_RECT_UV, bounds="rect", rect=_tile(1, 0)),
            _passthrough_effect([_pattern_rect(2, 0)], _SAMPLE_INPUT, bounds="canvas"),
            _passthrough_effect([_pattern_rect(3, 0)], _SAMPLE_INPUT_AT_RECT_UV, bounds="canvas"),
        ],
    )
