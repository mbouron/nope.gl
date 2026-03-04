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

import os

import pynopegl as ngl
from pynopegl_utils.tests.cmp_png import test_png

W, H = 256, 256

_ASSETS_DIR = os.path.join(
    os.path.dirname(__file__),
    "../python/pynopegl-utils/pynopegl_utils/assets",
)
_LANTERN = os.path.join(_ASSETS_DIR, "Unsplash-Andreas-Rasmussen-white-and-black-kanji-lantern.jpg")


def _make_texture(path: str = _LANTERN) -> ngl.Texture2D:
    return ngl.Texture2D(data_src=ngl.Media(filename=path), min_filter="linear", mag_filter="linear")


def _scene(cfg: ngl.SceneCfg, node, duration: float = 1.0):
    cfg.aspect_ratio = (W, H)
    cfg.duration = duration
    return node


# ── Fill types ────────────────────────────────────────────────────────────────


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_color_fill(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.8, 0.2, 0.2, 1.0))
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_gradient_fill(cfg: ngl.SceneCfg):
    fill = ngl.GradientFill(
        color0=(0.1, 0.3, 0.9),
        color1=(0.9, 0.5, 0.1),
        pos0=(0.0, 0.0),
        pos1=(1.0, 0.0),
    )
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_gradient_fill_diagonal(cfg: ngl.SceneCfg):
    fill = ngl.GradientFill(
        color0=(0.05, 0.1, 0.6),
        color1=(0.8, 0.3, 0.05),
        pos0=(0.0, 0.0),
        pos1=(1.0, 1.0),
    )
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_gradient4_fill(cfg: ngl.SceneCfg):
    fill = ngl.Gradient4Fill(
        color_tl=(1.0, 0.0, 0.0),
        color_tr=(0.0, 1.0, 0.0),
        color_br=(1.0, 1.0, 0.0),
        color_bl=(0.0, 0.0, 1.0),
    )
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_noise_fill(cfg: ngl.SceneCfg):
    fill = ngl.NoiseFill(type="perlin", octaves=4, seed=42)
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


# ── Stroke ────────────────────────────────────────────────────────────────────


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_stroke_inside_corner(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.15, 0.4, 0.8, 1.0))
    stroke = ngl.Stroke(width=10, mode="inside", color=(1.0, 1.0, 1.0, 1.0))
    return _scene(cfg, ngl.DrawRect(rect=(20, 20, W - 40, H - 40), fill=fill, stroke=stroke, corner_radius=5))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_stroke_center_corner(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.15, 0.4, 0.8, 1.0))
    stroke = ngl.Stroke(width=10, mode="center", color=(1.0, 1.0, 0.0, 1.0))
    return _scene(cfg, ngl.DrawRect(rect=(20, 20, W - 40, H - 40), fill=fill, stroke=stroke, corner_radius=5))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_stroke_outside_corner(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.15, 0.4, 0.8, 1.0))
    stroke = ngl.Stroke(width=10, mode="outside", color=(1.0, 1.0, 1.0, 1.0))
    return _scene(cfg, ngl.DrawRect(rect=(20, 20, W - 40, H - 40), fill=fill, stroke=stroke, corner_radius=5))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_stroke_inside(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.15, 0.4, 0.8, 1.0))
    stroke = ngl.Stroke(width=10, mode="inside", color=(1.0, 1.0, 1.0, 1.0))
    return _scene(cfg, ngl.DrawRect(rect=(20, 20, W - 40, H - 40), fill=fill, stroke=stroke))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_stroke_center(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.15, 0.4, 0.8, 1.0))
    stroke = ngl.Stroke(width=10, mode="center", color=(1.0, 1.0, 0.0, 1.0))
    return _scene(cfg, ngl.DrawRect(rect=(20, 20, W - 40, H - 40), fill=fill, stroke=stroke))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_stroke_outside(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.15, 0.4, 0.8, 1.0))
    stroke = ngl.Stroke(width=10, mode="outside", color=(1.0, 1.0, 1.0, 1.0))
    return _scene(cfg, ngl.DrawRect(rect=(20, 20, W - 40, H - 40), fill=fill, stroke=stroke))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_stroke_dashed(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.08, 0.08, 0.12, 1.0))
    stroke = ngl.Stroke(
        width=6,
        mode="inside",
        color=(1.0, 1.0, 1.0, 1.0),
        dash_length=24.0,
        dash_ratio=0.5,
    )
    return _scene(cfg, ngl.DrawRect(rect=(20, 20, W - 40, H - 40), fill=fill, stroke=stroke))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_stroke_dashed_round(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.08, 0.08, 0.12, 1.0))
    stroke = ngl.Stroke(
        width=12,
        mode="outside",
        color=(1.0, 1.0, 1.0, 1.0),
        dash_length=40.0,
        dash_ratio=0.4,
        dash_cap="round",
    )
    return _scene(cfg, ngl.DrawRect(rect=(30, 30, W - 60, H - 60), fill=fill, stroke=stroke))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_stroke_dashed_square(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.08, 0.08, 0.12, 1.0))
    stroke = ngl.Stroke(
        width=12,
        mode="outside",
        color=(1.0, 1.0, 1.0, 1.0),
        dash_length=40.0,
        dash_ratio=0.4,
        dash_cap="square",
    )
    return _scene(cfg, ngl.DrawRect(rect=(30, 30, W - 60, H - 60), fill=fill, stroke=stroke))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_stroke_gradient(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.08, 0.08, 0.12, 1.0))
    stroke = ngl.StrokeGradient(
        width=12,
        mode="inside",
        color0=(1.0, 0.0, 0.0),
        color1=(0.0, 0.8, 1.0),
        pos0=(0.0, 0.0),
        pos1=(1.0, 1.0),
    )
    return _scene(cfg, ngl.DrawRect(rect=(20, 20, W - 40, H - 40), fill=fill, stroke=stroke))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_stroke_gradient4(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.05, 0.05, 0.08, 1.0))
    stroke = ngl.StrokeGradient4(
        width=14,
        mode="center",
        color_tl=(1.0, 0.0, 0.0),
        color_tr=(0.0, 1.0, 0.0),
        color_br=(1.0, 1.0, 0.0),
        color_bl=(0.0, 0.0, 1.0),
    )
    return _scene(cfg, ngl.DrawRect(rect=(30, 30, W - 60, H - 60), fill=fill, stroke=stroke))


# ── Corner radius ─────────────────────────────────────────────────────────────


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_corner_radius(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.3, 0.75, 0.3, 1.0))
    return _scene(cfg, ngl.DrawRect(rect=(20, 20, W - 40, H - 40), fill=fill, corner_radius=30))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_corner_radius_stroke(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.2, 0.35, 0.75, 1.0))
    stroke = ngl.Stroke(width=6, mode="inside", color=(1.0, 1.0, 1.0, 1.0))
    return _scene(
        cfg,
        ngl.DrawRect(rect=(20, 20, W - 40, H - 40), fill=fill, stroke=stroke, corner_radius=24),
    )


# ── Opacity ───────────────────────────────────────────────────────────────────


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_opacity(cfg: ngl.SceneCfg):
    bg = ngl.DrawRect(rect=(0, 0, W, H), fill=ngl.ColorFill(color=(0.8, 0.1, 0.1, 1.0)))
    fg = ngl.DrawRect(
        rect=(40, 40, W - 80, H - 80),
        fill=ngl.ColorFill(color=(0.1, 0.1, 0.9, 1.0)),
        opacity=0.5,
        blending="src_over",
    )
    return _scene(cfg, ngl.Group(children=[bg, fg]))


# ── Clip rect ─────────────────────────────────────────────────────────────────


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_clip_rect(cfg: ngl.SceneCfg):
    fill = ngl.GradientFill(
        color0=(0.9, 0.1, 0.1),
        color1=(0.1, 0.1, 0.9),
        pos0=(0.0, 0.0),
        pos1=(1.0, 0.0),
    )
    return _scene(
        cfg,
        ngl.DrawRect(
            rect=(0, 0, W, H),
            fill=fill,
            clip_rect=(64, 64, 128, 128),
        ),
    )


# ── Content transform ─────────────────────────────────────────────────────────


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_content_zoom(cfg: ngl.SceneCfg):
    fill = ngl.GradientFill(
        color0=(0.9, 0.1, 0.1),
        color1=(0.1, 0.9, 0.1),
        pos0=(0.0, 0.0),
        pos1=(1.0, 1.0),
    )
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill, content_zoom=2.5))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_content_translate(cfg: ngl.SceneCfg):
    fill = ngl.GradientFill(
        color0=(0.1, 0.1, 0.9),
        color1=(0.9, 0.9, 0.1),
        pos0=(0.0, 0.0),
        pos1=(1.0, 0.0),
    )
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill, content_translate=(0.3, 0.0)))


# ── Built-in transforms ───────────────────────────────────────────────────────


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_translate(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.9, 0.6, 0.1, 1.0))
    return _scene(cfg, ngl.DrawRect(rect=(64, 64, 128, 128), fill=fill, translate=(32.0, 32.0, 0.0)))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_scale(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.1, 0.75, 0.4, 1.0))
    return _scene(cfg, ngl.DrawRect(rect=(64, 64, 128, 128), fill=fill, scale=(0.5, 0.5, 1.0)))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_rotate(cfg: ngl.SceneCfg):
    fill = ngl.ColorFill(color=(0.75, 0.15, 0.65, 1.0))
    return _scene(cfg, ngl.DrawRect(rect=(64, 64, 128, 128), fill=fill, rotate_angle=45.0))


# ── Animation (multiple keyframes) ────────────────────────────────────────────


@test_png(width=W, height=H, keyframes=4)
@ngl.scene()
def drawrect_animated_opacity(cfg: ngl.SceneCfg):
    cfg.aspect_ratio = (W, H)
    cfg.duration = 4.0
    fill = ngl.ColorFill(color=(0.4, 0.5, 0.9, 1.0))
    opacity_anim = ngl.AnimatedFloat(
        [
            ngl.AnimKeyFrameFloat(0.0, 0.0),
            ngl.AnimKeyFrameFloat(1.0, 0.5),
            ngl.AnimKeyFrameFloat(2.0, 1.0),
            ngl.AnimKeyFrameFloat(3.0, 0.2),
        ]
    )
    return ngl.DrawRect(rect=(0, 0, W, H), fill=fill, opacity=opacity_anim)


# ── Group of DrawRects ────────────────────────────────────────────────────────


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_group(cfg: ngl.SceneCfg):
    colors = [
        (0.9, 0.1, 0.1, 1.0),
        (0.1, 0.8, 0.1, 1.0),
        (0.1, 0.1, 0.9, 1.0),
        (0.9, 0.8, 0.1, 1.0),
    ]
    rects = []
    for i, color in enumerate(colors):
        x = (i % 2) * (W // 2)
        y = (i // 2) * (H // 2)
        fill = ngl.ColorFill(color=color)
        rects.append(ngl.DrawRect(rect=(x, y, W // 2, H // 2), fill=fill))
    return _scene(cfg, ngl.Group(children=rects))


# ── Blending ──────────────────────────────────────────────────────────────────


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_blending_src_over(cfg: ngl.SceneCfg):
    bg = ngl.DrawRect(rect=(0, 0, W, H), fill=ngl.ColorFill(color=(0.2, 0.2, 0.8, 1.0)))
    fg = ngl.DrawRect(
        rect=(32, 32, W - 64, H - 64),
        fill=ngl.ColorFill(color=(0.9, 0.5, 0.1, 0.7)),
        blending="src_over",
    )
    return _scene(cfg, ngl.Group(children=[bg, fg]))


# ── TextureFill ───────────────────────────────────────────────────────────────
# Lantern image: 1080×1616 (portrait). Rect is 256×256 (square).
# fit  → pillarbox (black bars left/right): image is narrower than the square rect
# fill → crop top/bottom: image is taller than the square rect


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_texture_fill_none(cfg: ngl.SceneCfg):
    """scaling=none: texture stretched to fill the rect (no aspect-ratio preservation)."""
    fill = ngl.TextureFill(texture=_make_texture(), scaling="none")
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_texture_fill_fit(cfg: ngl.SceneCfg):
    """scaling=fit: texture fits inside rect, pillarboxed (black bars left/right)."""
    fill = ngl.TextureFill(texture=_make_texture(), scaling="fit")
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_texture_fill_fill(cfg: ngl.SceneCfg):
    """scaling=fill: texture fills rect, cropped top/bottom."""
    fill = ngl.TextureFill(texture=_make_texture(), scaling="fill")
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_texture_fill_wrap_discard(cfg: ngl.SceneCfg):
    """wrap=discard + content_translate: area outside texture UV [0,1] is discarded."""
    fill = ngl.TextureFill(texture=_make_texture(), scaling="fit", wrap="discard")
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill, content_translate=(0.2, 0.0)))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_texture_fill_fit_translate(cfg: ngl.SceneCfg):
    """scaling=fit + content_translate: pan within the pillarbox bounds (clamped by fit mode)."""
    fill = ngl.TextureFill(texture=_make_texture(), scaling="fit")
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill, content_translate=(0.2, 0.0)))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_texture_fill_fill_zoom(cfg: ngl.SceneCfg):
    """scaling=fill + content_zoom: zoom into the already-cropped texture."""
    fill = ngl.TextureFill(texture=_make_texture(), scaling="fill")
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill, content_zoom=2.0))


# ── CustomFill with extra uniforms ────────────────────────────────────────────


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_custom_checkerboard(cfg: ngl.SceneCfg):
    """Checkerboard pattern driven by a tile_count uniform."""
    fill = ngl.CustomFill(
        color_glsl="""
            float tx = floor(uv.x * tile_count);
            float ty = floor(uv.y * tile_count);
            float checker = mod(tx + ty, 2.0);
            return mix(color0, color1, checker);
        """,
        frag_resources=[
            ngl.UniformFloat(value=8.0, label="tile_count"),
            ngl.UniformVec4(value=(0.9, 0.9, 0.9, 1.0), label="color0"),
            ngl.UniformVec4(value=(0.15, 0.15, 0.15, 1.0), label="color1"),
        ],
    )
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_custom_radial_gradient(cfg: ngl.SceneCfg):
    """Radial gradient driven by center, radius, and two color uniforms."""
    fill = ngl.CustomFill(
        color_glsl="""
            float d = length(uv - center) / radius;
            return mix(inner_color, outer_color, clamp(d, 0.0, 1.0));
        """,
        frag_resources=[
            ngl.UniformVec2(value=(0.5, 0.5), label="center"),
            ngl.UniformFloat(value=0.6, label="radius"),
            ngl.UniformVec4(value=(1.0, 0.9, 0.2, 1.0), label="inner_color"),
            ngl.UniformVec4(value=(0.05, 0.1, 0.5, 1.0), label="outer_color"),
        ],
    )
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_custom_wave(cfg: ngl.SceneCfg):
    """Horizontal wave stripe pattern driven by frequency, amplitude, and colors."""
    fill = ngl.CustomFill(
        color_glsl="""
            float wave = sin(uv.x * frequency) * amplitude;
            float t = clamp((uv.y - 0.5 + wave) * sharpness + 0.5, 0.0, 1.0);
            return mix(color0, color1, t);
        """,
        frag_resources=[
            ngl.UniformFloat(value=20.0, label="frequency"),
            ngl.UniformFloat(value=0.08, label="amplitude"),
            ngl.UniformFloat(value=20.0, label="sharpness"),
            ngl.UniformVec4(value=(0.1, 0.4, 0.9, 1.0), label="color0"),
            ngl.UniformVec4(value=(0.9, 0.3, 0.1, 1.0), label="color1"),
        ],
    )
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_custom_vignette(cfg: ngl.SceneCfg):
    """Vignette: uniform base color darkened towards edges by a strength uniform."""
    fill = ngl.CustomFill(
        color_glsl="""
            vec2 d = uv - vec2(0.5);
            float v = 1.0 - clamp(dot(d, d) * strength, 0.0, 1.0);
            return vec4(base_color.rgb * v, base_color.a);
        """,
        frag_resources=[
            ngl.UniformVec4(value=(0.6, 0.8, 1.0, 1.0), label="base_color"),
            ngl.UniformFloat(value=6.0, label="strength"),
        ],
    )
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_custom_texture(cfg: ngl.SceneCfg):
    """CustomFill sampling a texture resource."""
    fill = ngl.CustomFill(
        color_glsl="return ngl_texvideo(tex, tex_coord);",
        frag_resources=[
            ngl.Texture2D(data_src=ngl.Media(filename=_LANTERN), min_filter="linear", mag_filter="linear", label="tex"),
        ],
    )
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_custom_block(cfg: ngl.SceneCfg):
    """CustomFill reading colors from a Block (UBO)."""
    block = ngl.Block(
        fields=[
            ngl.UniformVec4(value=(0.9, 0.2, 0.1, 1.0), label="color_a"),
            ngl.UniformVec4(value=(0.1, 0.2, 0.9, 1.0), label="color_b"),
        ],
        layout="std140",
        label="palette",
    )
    fill = ngl.CustomFill(
        color_glsl="""
            float t = smoothstep(0.0, 1.0, uv.x);
            return mix(palette.color_a, palette.color_b, t);
        """,
        frag_resources=[block],
    )
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


def _rect_from_center(x, y, width, height):
    half_w = width / 2
    half_h = height / 2
    return (
        x - half_w,
        y - half_h,
        width,
        height,
    )


@test_png(width=W, height=H)
@ngl.scene()
def drawrect_complex(cfg: ngl.SceneCfg):
    group = ngl.Group()
    fill = ngl.ColorFill(color=(0.05, 0.05, 0.08, 1.0))
    fill = ngl.TextureFill(texture=_make_texture(), scaling="fit", wrap="discard")
    stroke = ngl.StrokeGradient4(
        width=1.0,
        mode="center",
        color_tl=(1.0, 0.0, 0.0),
        color_tr=(0.0, 1.0, 0.0),
        color_br=(1.0, 1.0, 0.0),
        color_bl=(0.0, 0.0, 1.0),
        dash_length=4,
    )
    rect1 = ngl.DrawRect(
        rect=_rect_from_center(W / 4, H / 4, W / 4, H / 4),
        fill=fill,
        stroke=stroke,
        blending="src_over",
        opacity=1.0,
    )
    rect2 = ngl.DrawRect(
        _rect_from_center(2.5 * W / 4, H / 4, W / 4, H / 4),
        fill=fill,
        stroke=stroke,
        blending="src_over",
        opacity=1.0,
    )
    rect3 = ngl.DrawRect(
        _rect_from_center(W / 4, 2.5 * H / 4, W / 4, H / 4),
        fill=fill,
        stroke=stroke,
        blending="src_over",
        opacity=1.0,
    )
    rect4 = ngl.DrawRect(
        _rect_from_center(2.5 * W / 4, 2.5 * H / 4, W / 4, H / 4),
        fill=fill,
        stroke=stroke,
        blending="src_over",
        opacity=1.5,
    )
    group.add_children(
        rect1,
        rect2,
        rect3,
        rect4,
    )

    group.set_rotate_angle(0)

    return _scene(cfg, group)
