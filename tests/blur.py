#
# Copyright 2023 Nope Forge
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
from pynopegl_utils.misc import load_media
from pynopegl_utils.tests.cmp_png import test_png

_NOISE_W, _NOISE_H = 256, 256
_CITY_MI = None


def _get_city_media():
    global _CITY_MI
    if _CITY_MI is None:
        _CITY_MI = load_media("city")
    return _CITY_MI


@test_png(width=_NOISE_W, height=_NOISE_H, keyframes=10, threshold=1)
@ngl.scene()
def blur_gaussian(cfg: ngl.SceneCfg):
    cfg.aspect_ratio = (_NOISE_W, _NOISE_H)
    cfg.duration = 10

    noise = ngl.DrawRect(rect=(0, 0, _NOISE_W, _NOISE_H), fill=ngl.NoiseFill(type="blocky", octaves=3, seed=42))
    noise_texture = ngl.Texture2D(data_src=noise)
    blurred_texture = ngl.Texture2D()
    blur = ngl.GaussianBlur(
        source=noise_texture,
        destination=blurred_texture,
        blurriness=ngl.AnimatedFloat(
            [
                ngl.AnimKeyFrameFloat(0, 0),
                ngl.AnimKeyFrameFloat(cfg.duration, 1),
            ]
        ),
    )
    display = ngl.DrawRect(
        rect=(0, 0, _NOISE_W, _NOISE_H),
        fill=ngl.TextureFill(texture=blurred_texture, scaling="none"),
    )
    return ngl.Group(children=[blur, display])


@test_png(width=800, height=800, keyframes=10, threshold=5)
@ngl.scene()
def blur_fast_gaussian(cfg: ngl.SceneCfg):
    w, h = 800, 800
    cfg.aspect_ratio = (w, h)
    cfg.duration = 10

    noise = ngl.DrawRect(rect=(0, 0, w, h), fill=ngl.NoiseFill(type="blocky", octaves=3, seed=42))
    noise_texture = ngl.Texture2D(data_src=noise)
    blurred_texture = ngl.Texture2D()
    blur = ngl.FastGaussianBlur(
        source=noise_texture,
        destination=blurred_texture,
        blurriness=ngl.AnimatedFloat(
            [
                ngl.AnimKeyFrameFloat(0, 0),
                ngl.AnimKeyFrameFloat(cfg.duration, 1),
            ]
        ),
    )
    display = ngl.DrawRect(
        rect=(0, 0, w, h),
        fill=ngl.TextureFill(texture=blurred_texture, scaling="none"),
    )
    return ngl.Group(children=[blur, display])


@test_png(width=540, height=808, keyframes=5, threshold=5)
@ngl.scene()
def blur_hexagonal(cfg: ngl.SceneCfg):
    mi = _get_city_media()
    cfg.aspect_ratio = (mi.width, mi.height)
    cfg.duration = 5

    source_texture = ngl.Texture2D(data_src=ngl.Media(filename=mi.filename))
    blurred_texture = ngl.Texture2D()
    blur = ngl.HexagonalBlur(
        source=source_texture,
        destination=blurred_texture,
        blurriness=ngl.AnimatedFloat(
            [
                ngl.AnimKeyFrameFloat(0, 0),
                ngl.AnimKeyFrameFloat(cfg.duration, 1),
            ]
        ),
    )
    display = ngl.DrawRect(
        rect=(0, 0, mi.width, mi.height),
        fill=ngl.TextureFill(texture=blurred_texture, scaling="none"),
    )
    return ngl.Group(children=[blur, display])


@test_png(width=540, height=808, keyframes=5, threshold=5)
@ngl.scene()
def blur_hexagonal_with_map(cfg: ngl.SceneCfg):
    mi = _get_city_media()
    cfg.aspect_ratio = (mi.width, mi.height)
    cfg.duration = 5

    # Generate the blur map with a rounded-box SDF via CustomFill
    map_fill = ngl.CustomFill(
        color_glsl="""
            vec2 p = uv * 2.0 - 1.0;
            vec2 pos = abs(p + vec2(-0.016, 0.11)) - vec2(0.19, 0.19) + 0.13;
            float sd = length(max(pos, 0.0)) + min(max(pos.x, pos.y), 0.0) - 0.13;
            float value = clamp(sd / 0.8, 0.0, 1.0);
            return vec4(value, value, value, value);
        """,
    )
    map_rect = ngl.DrawRect(rect=(0, 0, mi.width // 2, mi.height // 2), fill=map_fill)
    map_texture = ngl.Texture2D(width=mi.width // 2, height=mi.height // 2, format="r8_unorm", data_src=map_rect)

    source_texture = ngl.Texture2D(data_src=ngl.Media(filename=mi.filename))
    blurred_texture = ngl.Texture2D()
    blur = ngl.HexagonalBlur(
        source=source_texture,
        destination=blurred_texture,
        blurriness=ngl.AnimatedFloat(
            [
                ngl.AnimKeyFrameFloat(0, 0),
                ngl.AnimKeyFrameFloat(cfg.duration, 1),
            ]
        ),
        map=map_texture,
    )
    display = ngl.DrawRect(
        rect=(0, 0, mi.width, mi.height),
        fill=ngl.TextureFill(texture=blurred_texture, scaling="none"),
    )
    return ngl.Group(children=[blur, display])
