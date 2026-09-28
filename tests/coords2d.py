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
The 2D coordinate conventions, checked pixel by pixel against values computed
here rather than against reference images: each scene writes one of the
coordinates a paint or an effect receives as a color, or passes a texture
through, and every pixel is compared with what the convention says it must be.

The canvas is 256x256 and rendered at 256x256, so a canvas unit is a pixel and
the pixel (x, y) is centered on (x + 0.5, y + 0.5), with (0, 0) the top-left
pixel.
"""

import array
import math

import pynopegl as ngl
from pynopegl_utils.tests.cmp_expected import test_expected

W, H = 256, 256


def _canvas(cfg: ngl.SceneCfg, *children):
    cfg.duration = 1.0
    return ngl.Canvas2D(width=W, height=H, children=list(children))


def _unorm(v: float) -> int:
    return round(min(max(v, 0.0), 1.0) * 255)


def _rg(u: float, v: float):
    return (_unorm(u), _unorm(v), 0, 255)


def _inside(x: int, y: int, rect, margin: float = 1.0) -> bool:
    """Whether the pixel center is inside rect, away from its anti-aliased edges."""
    cx, cy = x + 0.5, y + 0.5
    rx, ry, rw, rh = rect
    return rx + margin < cx < rx + rw - margin and ry + margin < cy < ry + rh - margin


def _pattern(x: int, y: int):
    """A texture where no two neighbouring texels are alike, to catch any shift or blend."""
    return ((x * 7 + y * 3) & 255, (y * 5 + 17) & 255, ((x * 11) ^ (y * 13)) & 255, 255)


def _pattern_texture(**kwargs) -> ngl.Texture2D:
    data = array.array("B")
    for y in range(H):
        for x in range(W):
            data.extend(_pattern(x, y))
    return ngl.Texture2D(
        width=W,
        height=H,
        data_src=ngl.BufferUBVec4(data=data),
        min_filter="linear",
        mag_filter="linear",
        **kwargs,
    )


def _coords_paint(expr: str, resources=None, **kwargs) -> ngl.CustomPaint:
    return ngl.CustomPaint(glsl_color=f"return vec4({expr}, 0.0, 1.0);", resources=resources, **kwargs)


_RECT = (16, 40, 200, 120)


def _expected_rect_uv(frame, x, y):
    if not _inside(x, y, _RECT):
        return None
    rx, ry, rw, rh = _RECT
    return _rg((x + 0.5 - rx) / rw, (y + 0.5 - ry) / rh)


@test_expected(expected=_expected_rect_uv)
@ngl.scene(width=W, height=H)
def coords2d_rect_uv(cfg: ngl.SceneCfg):
    """rect_uv spans the rect, origin at its top-left corner, Y down."""
    return _canvas(cfg, ngl.DrawRect2D(rect=_RECT, fill=_coords_paint("rect_uv")))


def _expected_rect_px(frame, x, y):
    if not _inside(x, y, _RECT):
        return None
    rx, ry, _, _ = _RECT
    return _rg((x + 0.5 - rx) / 255.0, (y + 0.5 - ry) / 255.0)


@test_expected(expected=_expected_rect_px)
@ngl.scene(width=W, height=H)
def coords2d_rect_px(cfg: ngl.SceneCfg):
    """rect_px is rect_uv in pixels of the rect."""
    return _canvas(cfg, ngl.DrawRect2D(rect=_RECT, fill=_coords_paint("rect_px / 255.0")))


_TRANSLATE = (40, 24)
_MOVED_RECT = (_TRANSLATE[0], _TRANSLATE[1], 160, 160)


def _expected_canvas_px(frame, x, y):
    if not _inside(x, y, _MOVED_RECT):
        return None
    return _rg((x + 0.5) / 256.0, (y + 0.5) / 256.0)


@test_expected(expected=_expected_canvas_px)
@ngl.scene(width=W, height=H)
def coords2d_canvas_px(cfg: ngl.SceneCfg):
    """canvas_px is the position on the canvas, whatever transforms the rect."""
    rect = ngl.DrawRect2D(rect=(0, 0, 160, 160), fill=_coords_paint("canvas_px / 256.0"))
    return _canvas(cfg, ngl.Group2D(children=[rect], translate=_TRANSLATE))


_ZOOM = 2.0
_CONTENT_TRANSLATE = (0.125, -0.0625)


def _content_coord(u, v, scale=(1.0, 1.0), zoom=_ZOOM, translate=_CONTENT_TRANSLATE, angle=90.0):
    co, so = round(math.cos(math.radians(angle))), round(math.sin(math.radians(angle)))
    px = (u - 0.5) * scale[0] / zoom + translate[0]
    py = (v - 0.5) * scale[1] / zoom + translate[1]
    return co * px - so * py + 0.5, so * px + co * py + 0.5


def _expected_content_uv(frame, x, y):
    if not _inside(x, y, (0, 0, W, H)):
        return None
    return _rg(*_content_coord((x + 0.5) / W, (y + 0.5) / H))


@test_expected(expected=_expected_content_uv)
@ngl.scene(width=W, height=H)
def coords2d_content_uv(cfg: ngl.SceneCfg):
    """content_uv is rect_uv rotated, zoomed and translated around the rect center."""
    rect = ngl.DrawRect2D(
        rect=(0, 0, W, H),
        fill=_coords_paint("content_uv"),
        content_zoom=_ZOOM,
        content_translate=_CONTENT_TRANSLATE,
        content_orientation=90.0,
    )
    return _canvas(cfg, rect)


# A 128x64 texture filling a square rect keeps the middle half of its width
_FILL_SCALE = (0.5, 1.0)


def _expected_tex_coord_fill(frame, x, y):
    if not _inside(x, y, (0, 0, W, H)):
        return None
    return _rg(*_content_coord((x + 0.5) / W, (y + 0.5) / H, scale=_FILL_SCALE, zoom=1.0, translate=(0, 0), angle=0))


@test_expected(expected=_expected_tex_coord_fill)
@ngl.scene(width=W, height=H)
def coords2d_tex_coord_fill(cfg: ngl.SceneCfg):
    """tex_coord is content_uv scaled to show the texture as the paint scaling says."""
    texture = ngl.Texture2D(width=128, height=64)
    fill = _coords_paint("tex_coord", resources={"tex": texture}, scaling="fill")
    return _canvas(cfg, ngl.DrawRect2D(rect=(0, 0, W, H), fill=fill))


def _expected_stroke_uv(frame, x, y):
    cx, cy = x + 0.5, y + 0.5
    band = min(cx, cy, W - cx, H - cy)
    if not 1.0 < band < 19.0:
        return None
    return _rg(cx / W, cy / H)


@test_expected(expected=_expected_stroke_uv)
@ngl.scene(width=W, height=H)
def coords2d_stroke_content_uv(cfg: ngl.SceneCfg):
    """The content transform only applies to the fill: a stroke's content_uv is its rect_uv."""
    rect = ngl.DrawRect2D(
        rect=(0, 0, W, H),
        fill=ngl.ColorPaint(color=(0.0, 0.0, 0.0, 1.0)),
        stroke=ngl.Stroke2D(paint=_coords_paint("content_uv"), width=20.0, alignment="inside"),
        content_zoom=_ZOOM,
        content_translate=_CONTENT_TRANSLATE,
    )
    return _canvas(cfg, rect)


def _expected_pattern(frame, x, y):
    return _pattern(x, y)


@test_expected(expected=_expected_pattern, tolerance=0)
@ngl.scene(width=W, height=H)
def coords2d_texture_passthrough(cfg: ngl.SceneCfg):
    """A texture drawn through a rect covering the canvas comes out unchanged, texel for pixel."""
    fill = ngl.TexturePaint(texture=_pattern_texture())
    return _canvas(cfg, ngl.DrawRect2D(rect=(0, 0, W, H), fill=fill))


def _offscreen_pattern() -> ngl.Texture2D:
    texture = ngl.Texture2D(width=W, height=H, min_filter="linear", mag_filter="linear")
    offscreen = ngl.OffscreenCanvas2D(
        children=[ngl.DrawRect2D(rect=(0, 0, W, H), fill=ngl.TexturePaint(texture=_pattern_texture()))],
        width=W,
        height=H,
        color_textures=[texture],
    )
    return offscreen, texture


@test_expected(expected=_expected_pattern, tolerance=0)
@ngl.scene(width=W, height=H)
def coords2d_offscreen_passthrough(cfg: ngl.SceneCfg):
    """So does a texture rendered to offscreen first, whatever the render target memory layout."""
    offscreen, texture = _offscreen_pattern()
    display = ngl.DrawRect2D(rect=(0, 0, W, H), fill=ngl.TexturePaint(texture=texture))
    return _canvas(cfg, offscreen, display)


@test_expected(expected=_expected_pattern, tolerance=0)
@ngl.scene(width=W, height=H)
def coords2d_effect_passthrough(cfg: ngl.SceneCfg):
    """An effect sampling its input at tex_coord leaves it unchanged."""
    content = ngl.DrawRect2D(rect=(0, 0, W, H), fill=ngl.TexturePaint(texture=_pattern_texture()))
    effect = ngl.Effect2D(
        children=[content],
        shaders=[ngl.Effect2DShader(glsl_color="return ngl_teximage(ngl_input, tex_coord);")],
    )
    return _canvas(cfg, effect)


# Integer texel offsets, so that the shifted texture is still sampled at texel centers
_SHIFT = (16, 8)


def _expected_shifted_pattern(frame, x, y):
    sx, sy = x + _SHIFT[0], y + _SHIFT[1]
    if sx >= W or sy >= H:
        return None
    return _pattern(sx, sy)


@test_expected(expected=_expected_shifted_pattern, tolerance=0)
@ngl.scene(width=W, height=H)
def coords2d_texture_content_translate(cfg: ngl.SceneCfg):
    """The content translate moves the image as it is seen."""
    fill = ngl.TexturePaint(texture=_pattern_texture())
    rect = ngl.DrawRect2D(rect=(0, 0, W, H), fill=fill, content_translate=(_SHIFT[0] / W, _SHIFT[1] / H))
    return _canvas(cfg, rect)


@test_expected(expected=_expected_shifted_pattern, tolerance=0)
@ngl.scene(width=W, height=H)
def coords2d_offscreen_content_translate(cfg: ngl.SceneCfg):
    """... in the same direction for a render target, whatever its memory layout."""
    offscreen, texture = _offscreen_pattern()
    fill = ngl.TexturePaint(texture=texture)
    rect = ngl.DrawRect2D(rect=(0, 0, W, H), fill=fill, content_translate=(_SHIFT[0] / W, _SHIFT[1] / H))
    return _canvas(cfg, offscreen, rect)


# An effect rect sticking out of the canvas on the left: its composite quad is
# cropped to the visible canvas, the effect coordinates are not
_EFFECT_RECT = (-64, 32, 256, 128)


def _expected_effect_rect_uv(frame, x, y):
    visible = (0, _EFFECT_RECT[1], _EFFECT_RECT[0] + _EFFECT_RECT[2], _EFFECT_RECT[3])
    if not _inside(x, y, visible):
        return None
    rx, ry, rw, rh = _EFFECT_RECT
    return _rg((x + 0.5 - rx) / rw, (y + 0.5 - ry) / rh)


def _effect(glsl: str):
    content = ngl.DrawRect2D(rect=(0, 0, W, H), fill=ngl.ColorPaint(color=(1.0, 1.0, 1.0, 1.0)))
    return ngl.Effect2D(
        children=[content],
        bounds="rect",
        rect=_EFFECT_RECT,
        shaders=[ngl.Effect2DShader(glsl_color=glsl)],
    )


@test_expected(expected=_expected_effect_rect_uv)
@ngl.scene(width=W, height=H)
def coords2d_effect_rect_uv(cfg: ngl.SceneCfg):
    """An effect's rect_uv spans its whole rect, even where it is cropped to the canvas."""
    return _canvas(cfg, _effect("return vec4(rect_uv, 0.0, 1.0);"))


def _expected_effect_canvas_px(frame, x, y):
    visible = (0, _EFFECT_RECT[1], _EFFECT_RECT[0] + _EFFECT_RECT[2], _EFFECT_RECT[3])
    if not _inside(x, y, visible):
        return None
    return _rg((x + 0.5) / 256.0, (y + 0.5) / 256.0)


@test_expected(expected=_expected_effect_canvas_px)
@ngl.scene(width=W, height=H)
def coords2d_effect_canvas_px(cfg: ngl.SceneCfg):
    """An effect's canvas_px is the position on the canvas."""
    return _canvas(cfg, _effect("return vec4(canvas_px / 256.0, 0.0, 1.0);"))


def _expected_tex_coord_func(frame, x, y):
    if not _inside(x, y, (0, 0, W, H)):
        return None
    return _rg(*_content_coord((x + 0.5) / W, (y + 0.5) / H, scale=_FILL_SCALE, angle=0))


@test_expected(expected=_expected_tex_coord_func)
@ngl.scene(width=W, height=H)
def coords2d_tex_coord_func(cfg: ngl.SceneCfg):
    """ngl_tex_coord() gives the tex_coord of any rect_uv."""
    texture = ngl.Texture2D(width=128, height=64)
    fill = _coords_paint("ngl_tex_coord(rect_uv)", resources={"tex": texture}, scaling="fill")
    rect = ngl.DrawRect2D(rect=(0, 0, W, H), fill=fill, content_zoom=_ZOOM, content_translate=_CONTENT_TRANSLATE)
    return _canvas(cfg, rect)


@test_expected(expected=_expected_pattern, tolerance=0)
@ngl.scene(width=W, height=H)
def coords2d_effect_cropped_passthrough(cfg: ngl.SceneCfg):
    """ngl_tex_coord() maps an effect's rect_uv to its input, even where the effect is cropped."""
    content = ngl.DrawRect2D(rect=(0, 0, W, H), fill=ngl.TexturePaint(texture=_pattern_texture()))
    effect = ngl.Effect2D(
        children=[content],
        bounds="rect",
        rect=(-64, -32, W + 128, H + 64),
        shaders=[ngl.Effect2DShader(glsl_color="return ngl_teximage(ngl_input, ngl_tex_coord(rect_uv));")],
    )
    return _canvas(cfg, effect)
