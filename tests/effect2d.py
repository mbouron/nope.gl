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

import pynopegl as ngl
from pynopegl_utils.tests.cmp_render import test_render

W, H = 256, 256


def _canvas(cfg: ngl.SceneCfg, *children, duration: float = 1.0):
    cfg.duration = duration
    return ngl.Canvas(width=W, height=H, children=list(children))


@test_render()
@ngl.scene(width=W, height=H)
def effect2d_passthrough(cfg: ngl.SceneCfg):
    """Effect2D with no effects: should render identically to direct DrawRect."""
    fill = ngl.ColorFill(color=(0.8, 0.2, 0.2, 1.0))
    rect = ngl.DrawRect(rect=(32, 32, W - 64, H - 64), fill=fill)
    return _canvas(cfg, ngl.Effect2D(children=[rect]))


@test_render()
@ngl.scene(width=W, height=H)
def effect2d_multiple_children(cfg: ngl.SceneCfg):
    """Effect2D with multiple DrawRect children."""
    r0 = ngl.DrawRect(rect=(0, 0, W // 2, H // 2), fill=ngl.ColorFill(color=(0.9, 0.1, 0.1, 1.0)))
    r1 = ngl.DrawRect(rect=(W // 2, H // 2, W // 2, H // 2), fill=ngl.ColorFill(color=(0.1, 0.1, 0.9, 1.0)))
    return _canvas(cfg, ngl.Effect2D(children=[r0, r1]))


@test_render()
@ngl.scene(width=W, height=H)
def effect2d_opacity(cfg: ngl.SceneCfg):
    """Effect2D opacity applied to the composited result."""
    bg = ngl.DrawRect(rect=(0, 0, W, H), fill=ngl.ColorFill(color=(0.8, 0.1, 0.1, 1.0)))
    fg = ngl.DrawRect(rect=(32, 32, W - 64, H - 64), fill=ngl.ColorFill(color=(0.1, 0.1, 0.9, 1.0)))
    effect = ngl.Effect2D(children=[fg], opacity=0.5)
    return _canvas(cfg, bg, effect)


@test_render(tolerance=3)
@ngl.scene(width=W, height=H)
def effect2d_blur(cfg: ngl.SceneCfg):
    """Effect2D with blur applied to children."""
    fill = ngl.ColorFill(color=(0.9, 0.2, 0.1, 1.0))
    rect = ngl.DrawRect(rect=(48, 48, W - 96, H - 96), fill=fill)
    return _canvas(cfg, ngl.Effect2D(children=[rect], blur=0.05))


@test_render(tolerance=3)
@ngl.scene(width=W, height=H)
def effect2d_blur_large(cfg: ngl.SceneCfg):
    """Effect2D with large blur amount."""
    fill = ngl.ColorFill(color=(0.1, 0.6, 0.9, 1.0))
    rect = ngl.DrawRect(rect=(64, 64, W - 128, H - 128), fill=fill)
    return _canvas(cfg, ngl.Effect2D(children=[rect], blur=0.2))


@test_render()
@ngl.scene(width=W, height=H)
def effect2d_fragment_invert(cfg: ngl.SceneCfg):
    """Effect2D with custom fragment shader that inverts colors."""
    fill = ngl.ColorFill(color=(0.9, 0.1, 0.1, 1.0))
    rect = ngl.DrawRect(rect=(0, 0, W, H), fill=fill)
    effect = ngl.Effect2D(
        children=[rect],
        fragment="return vec4(1.0 - color.rgb, color.a);",
    )
    return _canvas(cfg, effect)


@test_render()
@ngl.scene(width=W, height=H)
def effect2d_fragment_uniform(cfg: ngl.SceneCfg):
    """Effect2D custom fragment with a uniform resource."""
    fill = ngl.ColorFill(color=(0.8, 0.4, 0.1, 1.0))
    rect = ngl.DrawRect(rect=(0, 0, W, H), fill=fill)
    effect = ngl.Effect2D(
        children=[rect],
        fragment="return color * vec4(vec3(tint), 1.0);",
        frag_resources=[ngl.UniformFloat(value=0.5, label="tint")],
    )
    return _canvas(cfg, effect)


@test_render()
@ngl.scene(width=W, height=H)
def effect2d_fragment_grayscale(cfg: ngl.SceneCfg):
    """Effect2D custom fragment converting to grayscale."""
    r0 = ngl.DrawRect(rect=(0, 0, W // 2, H), fill=ngl.ColorFill(color=(0.9, 0.1, 0.1, 1.0)))
    r1 = ngl.DrawRect(rect=(W // 2, 0, W // 2, H), fill=ngl.ColorFill(color=(0.1, 0.1, 0.9, 1.0)))
    effect = ngl.Effect2D(
        children=[r0, r1],
        fragment="""
            float luma = dot(color.rgb, vec3(0.299, 0.587, 0.114));
            return vec4(vec3(luma), color.a);
        """,
    )
    return _canvas(cfg, effect)


@test_render(tolerance=3)
@ngl.scene(width=W, height=H)
def effect2d_blur_and_fragment(cfg: ngl.SceneCfg):
    """Effect2D with blur followed by a custom fragment shader."""
    fill = ngl.ColorFill(color=(0.1, 0.8, 0.2, 1.0))
    rect = ngl.DrawRect(rect=(48, 48, W - 96, H - 96), fill=fill)
    effect = ngl.Effect2D(
        children=[rect],
        blur=0.05,
        fragment="return vec4(color.r, 0.0, color.b, color.a);",
    )
    return _canvas(cfg, effect)


@test_render()
@ngl.scene(width=W, height=H)
def effect2d_nested_in_group2d(cfg: ngl.SceneCfg):
    """Effect2D nested inside a Group2D with translation."""
    fill = ngl.ColorFill(color=(0.9, 0.6, 0.1, 1.0))
    rect = ngl.DrawRect(rect=(0, 0, 96, 96), fill=fill)
    effect = ngl.Effect2D(
        children=[rect],
        fragment="return vec4(1.0 - color.rgb, color.a);",
    )
    group = ngl.Group2D(children=[effect], translate=(80, 80))
    return _canvas(cfg, group)


@test_render()
@ngl.scene(width=W, height=H)
def effect2d_visible_false(cfg: ngl.SceneCfg):
    """Effect2D with visible=False: nothing should render from the effect."""
    bg = ngl.DrawRect(rect=(0, 0, W, H), fill=ngl.ColorFill(color=(0.2, 0.7, 0.3, 1.0)))
    fg = ngl.DrawRect(rect=(32, 32, W - 64, H - 64), fill=ngl.ColorFill(color=(0.9, 0.1, 0.1, 1.0)))
    effect = ngl.Effect2D(children=[fg], visible=False)
    return _canvas(cfg, bg, effect)
