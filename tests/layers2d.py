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
Clip2D, Layer2D and Mask2D, checked pixel by pixel against values computed here
(see coords2d.py): a 256x256 canvas rendered at 256x256, the pixel (x, y)
centered on (x + 0.5, y + 0.5) with (0, 0) the top-left pixel.
"""

import array
import math

import pynopegl as ngl
from pynopegl_utils.tests.cmp_expected import test_expected

W, H = 256, 256

# The mask is a horizontal ramp, its texel i holding RAMP[i]
MASK_W = 16
RAMP = [round(255 * i / (MASK_W - 1)) for i in range(MASK_W)]


def _canvas(cfg: ngl.SceneCfg, *children):
    cfg.duration = 1.0
    return ngl.Canvas2D(width=W, height=H, children=list(children))


def _inside(x: int, y: int, rect, margin: float = 1.0) -> bool:
    cx, cy = x + 0.5, y + 0.5
    rx, ry, rw, rh = rect
    return rx + margin < cx < rx + rw - margin and ry + margin < cy < ry + rh - margin


def _pattern(x: int, y: int):
    return ((x * 7 + y * 3) & 255, (y * 5 + 17) & 255, ((x * 11) ^ (y * 13)) & 255, 255)


def _pattern_texture() -> ngl.Texture2D:
    data = array.array("B")
    for y in range(H):
        for x in range(W):
            data.extend(_pattern(x, y))
    return ngl.Texture2D(
        width=W, height=H, data_src=ngl.BufferUBVec4(data=data), min_filter="linear", mag_filter="linear"
    )


def _white_rect(rect):
    return ngl.DrawRect2D(rect=rect, fill=ngl.ColorPaint(color=(1.0, 1.0, 1.0, 1.0)))


def _ramp_mask(channel: str) -> ngl.Texture2D:
    data = array.array("B")
    for value in RAMP:
        if channel == "luminance":
            data.extend((value, value, value, 255))
        else:
            data.extend((0, 0, 0, value))
    return ngl.Texture2D(
        width=MASK_W,
        height=1,
        data_src=ngl.BufferUBVec4(data=data),
        min_filter="linear",
        mag_filter="linear",
    )


def _ramp_at(u: float) -> float:
    """The ramp sampled with linear filtering and edge clamping at the image coordinate u."""
    t = min(max(u * MASK_W - 0.5, 0.0), MASK_W - 1.0)
    i = min(int(math.floor(t)), MASK_W - 2)
    f = t - i
    return RAMP[i] * (1.0 - f) + RAMP[i + 1] * f


@test_expected(expected=lambda frame, x, y: _pattern(x, y), tolerance=0)
@ngl.scene(width=W, height=H)
def layers2d_layer_passthrough(cfg: ngl.SceneCfg):
    """A layer composites its children unchanged, texel for pixel."""
    content = ngl.DrawRect2D(rect=(0, 0, W, H), fill=ngl.TexturePaint(texture=_pattern_texture()))
    return _canvas(cfg, ngl.Layer2D(children=[content]))


def _expected_layer_opacity(frame, x, y):
    r, g, b, _ = _pattern(x, y)
    return (round(r * 0.5), round(g * 0.5), round(b * 0.5), 255)


@test_expected(expected=_expected_layer_opacity)
@ngl.scene(width=W, height=H)
def layers2d_layer_opacity(cfg: ngl.SceneCfg):
    """A layer's opacity applies to the composited layer (here over black)."""
    content = ngl.DrawRect2D(rect=(0, 0, W, H), fill=ngl.TexturePaint(texture=_pattern_texture()))
    return _canvas(cfg, ngl.Layer2D(children=[content], opacity=0.5))


_MASKED = (32, 16, 192, 224)


# A DrawRect2D's bounds include its anti-aliased fringe, which is what the
# children bounds, and so the mask, span
_AA_FRINGE = 2.0


def _expected_mask(rect):
    def expected(frame, x, y):
        if not _inside(x, y, rect):
            return None
        u = (x + 0.5 - (rect[0] - _AA_FRINGE)) / (rect[2] + 2 * _AA_FRINGE)
        v = round(_ramp_at(u))
        return (v, v, v, 255)

    return expected


@test_expected(expected=_expected_mask(_MASKED))
@ngl.scene(width=W, height=H)
def layers2d_mask_alpha(cfg: ngl.SceneCfg):
    """The mask image spans the children bounds (anti-aliased edges included), its alpha weighting them."""
    masked = ngl.Mask2D(children=[_white_rect(_MASKED)], mask=_ramp_mask("alpha"), channel="alpha")
    return _canvas(cfg, masked)


@test_expected(expected=_expected_mask(_MASKED))
@ngl.scene(width=W, height=H)
def layers2d_mask_luminance(cfg: ngl.SceneCfg):
    """... or its luminance."""
    masked = ngl.Mask2D(children=[_white_rect(_MASKED)], mask=_ramp_mask("luminance"), channel="luminance")
    return _canvas(cfg, masked)


# Children sticking out of the canvas on the left: the composite is cropped to
# the canvas, the mask still spans the whole children bounds
_CROPPED = (-64, 0, 256, 256)


@test_expected(expected=_expected_mask(_CROPPED))
@ngl.scene(width=W, height=H)
def layers2d_mask_cropped(cfg: ngl.SceneCfg):
    """The mask spans the children bounds even where they are outside the canvas."""
    masked = ngl.Mask2D(children=[_white_rect(_CROPPED)], mask=_ramp_mask("alpha"), channel="alpha")
    return _canvas(cfg, masked)


_CLIP = (40, 24, 144, 96)


def _expected_clip(frame, x, y):
    if _inside(x, y, _CLIP):
        return (255, 255, 255, 255)
    cx, cy = x + 0.5, y + 0.5
    rx, ry, rw, rh = _CLIP
    if cx < rx - 1 or cx > rx + rw + 1 or cy < ry - 1 or cy > ry + rh + 1:
        return (0, 0, 0, 255)
    return None


@test_expected(expected=_expected_clip)
@ngl.scene(width=W, height=H)
def layers2d_clip_rect(cfg: ngl.SceneCfg):
    """A clip only lets its children through inside its rectangle."""
    clip = ngl.Clip2D(children=[_white_rect((0, 0, W, H))], clip_rect=_CLIP)
    return _canvas(cfg, clip)
