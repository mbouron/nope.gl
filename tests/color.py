#
# Copyright 2022 GoPro Inc.
# Copyright 2026 Nope Forge
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
from pynopegl_utils.tests.cmp_png import test_png

W, H = 128, 128


def _get_static_color_scene_func(c, space):
    @test_png(width=W, height=H)
    @ngl.scene()
    def scene_func(cfg: ngl.SceneCfg):
        cfg.aspect_ratio = (W, H)
        fill = ngl.CustomFill(
            color_glsl="return vec4(color.rgb, 1.0);",
            frag_resources=[ngl.UniformColor(c, space=space, label="color")],
        )
        return ngl.DrawRect(rect=(0, 0, W, H), fill=fill)

    return scene_func


color_static_srgb = _get_static_color_scene_func((1.0, 0.5, 0.0), "srgb")
color_static_hsl = _get_static_color_scene_func((0.6, 0.9, 0.4), "hsl")
color_static_hsv = _get_static_color_scene_func((0.3, 0.7, 0.6), "hsv")


@test_png(width=W, height=H)
@ngl.scene()
def color_negative_values_srgb(cfg: ngl.SceneCfg):
    cfg.aspect_ratio = (W, H)
    fill = ngl.CustomFill(
        color_glsl="""
            vec3 c = mix(color0, color1, uv.x);
            return vec4(clamp(c, 0.0, 1.0), 1.0);
        """,
        frag_resources=[
            ngl.UniformVec3(value=(0.0, 0.0, 0.0), label="color0"),
            ngl.UniformVec3(value=(-1.0, -1.0, 1.0), label="color1"),
        ],
    )
    return ngl.DrawRect(rect=(0, 0, W, H), fill=fill)
