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

import array
import math

import pynopegl as ngl
from pynopegl_utils.tests.cmp_render import test_render

W, H = 256, 256


def _canvas(cfg: ngl.SceneCfg, *children, duration: float = 1.0):
    cfg.duration = duration
    return ngl.Canvas2D(width=W, height=H, children=list(children))


def _colored_rect(x, y, w, h, color):
    fill = ngl.ColorFill(color=color)
    return ngl.DrawRect2D(rect=(x, y, w, h), fill=fill)


def _animated_scene():
    r1 = ngl.DrawRect2D(
        rect=(16, 16, 100, 100),
        fill=ngl.ColorFill(color=(0.8, 0.2, 0.2, 1.0)),
        rotation=ngl.AnimatedFloat(
            [
                ngl.AnimKeyFrameFloat(0.0, 0.0),
                ngl.AnimKeyFrameFloat(3.0, 45.0),
            ]
        ),
    )
    r2 = ngl.DrawRect2D(
        rect=(80, 80, 120, 120),
        fill=ngl.GradientFill(color0=(0.1, 0.1, 0.9), color1=(0.1, 0.9, 0.1)),
        translate=ngl.AnimatedVec2(
            [
                ngl.AnimKeyFrameVec2(0.0, (0.0, 0.0)),
                ngl.AnimKeyFrameVec2(3.0, (30.0, -20.0)),
            ]
        ),
    )
    r3 = ngl.DrawRect2D(
        rect=(140, 30, 80, 80),
        fill=ngl.ColorFill(color=(0.2, 0.8, 0.2, 1.0)),
        scale=ngl.AnimatedVec2(
            [
                ngl.AnimKeyFrameVec2(0.0, (1.0, 1.0)),
                ngl.AnimKeyFrameVec2(3.0, (1.5, 0.8)),
            ]
        ),
        corner_radius=10,
    )
    return [r1, r2, r3]


@test_render(keyframes=4, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_passthrough(cfg: ngl.SceneCfg):
    """Effect2D passthrough with multiple animated/transformed children."""
    effect = ngl.Effect2D(children=_animated_scene())
    return _canvas(cfg, effect, duration=4.0)


@test_render(keyframes=4, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_opacity(cfg: ngl.SceneCfg):
    """Effect2D with reduced opacity."""
    bg = _colored_rect(0, 0, W, H, (0.1, 0.1, 0.4, 1.0))
    effect = ngl.Effect2D(children=_animated_scene(), opacity=0.5)
    return _canvas(cfg, bg, effect, duration=4.0)


@test_render(keyframes=4, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_visible_false(cfg: ngl.SceneCfg):
    """Effect2D with visible=False renders nothing."""
    bg = _colored_rect(0, 0, W, H, (0.1, 0.1, 0.4, 1.0))
    effect = ngl.Effect2D(children=_animated_scene(), visible=False)
    return _canvas(cfg, bg, effect, duration=4.0)


@test_render(keyframes=4, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_grayscale(cfg: ngl.SceneCfg):
    """Effect2D with a grayscale fragment shader."""
    effect = ngl.Effect2D(
        children=_animated_scene(),
        glsl_color="vec4 color = ngl_texvideo(tex, uv);\n"
        "float lum = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));\n"
        "return vec4(lum, lum, lum, color.a);",
    )
    return _canvas(cfg, effect, duration=4.0)


@test_render(keyframes=4, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_invert(cfg: ngl.SceneCfg):
    """Effect2D with a color inversion fragment shader."""
    effect = ngl.Effect2D(
        children=_animated_scene(),
        glsl_color="vec4 color = ngl_texvideo(tex, uv);\n" "return vec4(1.0 - color.rgb, color.a);",
    )
    return _canvas(cfg, effect, duration=4.0)


@test_render(keyframes=4, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_nested_in_group2d(cfg: ngl.SceneCfg):
    """Effect2D nested inside a Group2D with a translate."""
    effect = ngl.Effect2D(
        children=_animated_scene(),
        glsl_color="vec4 color = ngl_texvideo(tex, uv);\n" "return vec4(1.0 - color.rgb, color.a);",
    )
    group = ngl.Group2D(children=[effect], rotation=45.0, anchor=(W / 2, H / 2))
    return _canvas(cfg, group, duration=4.0)


def _gaussian_kernel_1d(sigma):
    """Compute a normalized 1D Gaussian kernel."""
    radius = math.ceil(3 * sigma)
    size = 2 * radius + 1
    kernel = []
    for i in range(size):
        x = i - radius
        kernel.append(math.exp(-x * x / (2.0 * sigma * sigma)))
    total = sum(kernel)
    return array.array("f", [w / total for w in kernel]), radius


_GAUSSIAN_BLUR_GLSL = """\
    vec2 texel = 1.0 / vec2(textureSize(tex, 0));
    vec4 sum = vec4(0.0);
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            float w = kernel.weights[x + radius] * kernel.weights[y + radius];
            sum += ngl_texvideo(tex, uv + vec2(float(x), float(y)) * texel) * w;
        }
    }
    return sum;
"""


@test_render(keyframes=4, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_blur(cfg: ngl.SceneCfg):
    """Gaussian blur with precomputed kernel weights passed via Block."""
    sigma = 10.0
    weights, radius = _gaussian_kernel_1d(sigma)
    kernel = ngl.Block(
        fields=[ngl.BufferFloat(data=weights, label="weights")],
        label="kernel",
    )
    radius_uniform = ngl.UniformInt(value=radius, label="radius")
    effect = ngl.Effect2D(
        children=_animated_scene(),
        glsl_color=_GAUSSIAN_BLUR_GLSL,
        resources=[kernel, radius_uniform],
        dilation=math.ceil(3 * sigma),
    )
    return _canvas(cfg, effect, duration=4.0)
