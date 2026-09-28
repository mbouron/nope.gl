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
Clip2D, Layer2D and Mask2D, as grids of 64x64 tiles placed on pixel boundaries
(see coords2d.py).

The masks are ramps spanning roughly one tile, so that a half-pixel shift would
exceed the comparison tolerance.
"""

import array

import pynopegl as ngl
from pynopegl_utils.tests.cmp_render import test_render

T = 64


def _tile(col: float, row: float, cols: float = 1, rows: float = 1):
    return (col * T, row * T, cols * T, rows * T)


def _inset(rect, margin: float = T / 8):
    x, y, w, h = rect
    return (x + margin, y + margin, w - 2 * margin, h - 2 * margin)


def _rect(rect, color=(1.0, 1.0, 1.0, 1.0)) -> ngl.DrawRect2D:
    return ngl.DrawRect2D(rect=rect, fill=ngl.ColorPaint(color=color))


def _mask_texture(data: array.array, height: int = 1) -> ngl.Texture2D:
    return ngl.Texture2D(
        width=len(data) // (4 * height),
        height=height,
        data_src=ngl.BufferUBVec4(data=data),
        min_filter="linear",
        mag_filter="linear",
    )


_RAMP = [round(255 * i / 15) for i in range(16)]


def _ramp_mask() -> ngl.Texture2D:
    """A horizontal ramp, from transparent to opaque."""
    return _mask_texture(array.array("B", [c for v in _RAMP for c in (0, 0, 0, v)]))


def _rgb_ramp_mask() -> ngl.Texture2D:
    """Horizontal ramps from black to red, green and blue, one per row, each weighing its luminance."""
    data = array.array("B")
    for channel in range(3):
        for v in _RAMP:
            data.extend([v if c == channel else 0 for c in range(3)] + [255])
    return _mask_texture(data, height=3)


def _ramp_rect(rect) -> ngl.DrawRect2D:
    return ngl.DrawRect2D(
        rect=rect,
        fill=ngl.GradientPaint(color0=(1.0, 1.0, 1.0), color1=(1.0, 1.0, 1.0), opacity0=0.0, opacity1=1.0),
    )


@test_render()
@ngl.scene(width=4 * T, height=2 * T)
def layers2d_mask(cfg: ngl.SceneCfg):
    """Mask2D with mask textures over the children bounds or a mask rect, and with mask children.

    The masks span the children bounds, anti-aliased edges included, even where the
    children extend beyond the canvas, and follow the Mask2D transform.
    """
    cfg.duration = 1.0

    offscreen_mask = ngl.Texture2D(width=T, height=T, min_filter="linear", mag_filter="linear")
    offscreen = ngl.OffscreenCanvas2D(
        children=[_rect((0, 0, T, T / 2))], width=T, height=T, color_textures=[offscreen_mask]
    )

    row0 = [
        ngl.Mask2D(children=[_rect(_tile(-0.5, 0, 1.5))], mask=_ramp_mask()),
        ngl.Mask2D(children=[_rect(_tile(1, 0))], mask=_ramp_mask()),
        ngl.Mask2D(children=[_rect(_tile(2, 0))], mask=_rgb_ramp_mask(), channel="luminance"),
        ngl.Mask2D(children=[_rect(_tile(3, 0))], mask=offscreen_mask),
    ]

    row1 = [
        ngl.Mask2D(children=[_rect(_tile(0, 1))], mask_rect=_inset(_tile(0, 1)), mask=_ramp_mask()),
        ngl.Mask2D(children=[_rect(_tile(1, 1))], mask_children=[_ramp_rect(_inset(_tile(1, 1)))]),
        ngl.Mask2D(
            children=[_rect(_tile(0, 0, 2, 2))],
            mask_children=[_ramp_rect(_inset(_tile(0, 0, 2, 2), T / 4))],
            translate=(2 * T, T),
            scale=(0.5, 0.5),
        ),
        ngl.Mask2D(children=[_rect(_tile(3, 1, 1.5))], mask_children=[_ramp_rect(_inset(_tile(3, 1, 1.5)))]),
    ]

    return ngl.Canvas2D(width=4 * T, height=2 * T, children=[offscreen, *row0, *row1])


@test_render()
@ngl.scene(width=2 * T, height=T)
def layers2d_layer_clip(cfg: ngl.SceneCfg):
    """Layer2D opacity applied to the composited overlapping children, and Clip2D clipping."""
    cfg.duration = 1.0

    layer = ngl.Layer2D(
        children=[_rect((0, 0, 40, T), (1.0, 0.0, 0.0, 1.0)), _rect((24, 0, 40, T), (0.0, 0.0, 1.0, 1.0))],
        opacity=0.5,
    )
    clip = ngl.Clip2D(children=[_rect(_tile(1, 0))], clip_rect=_inset(_tile(1, 0)))

    return ngl.Canvas2D(width=2 * T, height=T, children=[layer, clip])
