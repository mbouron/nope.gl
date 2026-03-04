#
# Copyright 2021-2022 GoPro Inc.
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
from pynopegl_utils.toolbox.colors import COLORS

W, H = 320, 320

_OPERATORS = (
    "src_over",
    "dst_over",
    "src_out",
    "dst_out",
    "src_in",
    "dst_in",
    "src_atop",
    "dst_atop",
    "xor",
)

_CIRCLE_GLSL = """
    float sd = length(uv - center) - 0.5;
    float alpha = step(sd, 0.0);
    return vec4(color, 1.0) * alpha;
"""


def _get_compositing_scene(cfg: ngl.SceneCfg, op):
    cfg.aspect_ratio = (W, H)

    # Circle A: azure, offset to the left
    fill_a = ngl.CustomFill(
        color_glsl=_CIRCLE_GLSL,
        frag_resources=[
            ngl.UniformVec3(value=COLORS.azure, label="color"),
            ngl.UniformVec2(value=(0.35, 0.5), label="center"),
        ],
    )
    a = ngl.DrawRect(rect=(0, 0, W, H), fill=fill_a)

    # Circle B: orange, offset to the right, composited with the given operator
    fill_b = ngl.CustomFill(
        color_glsl=_CIRCLE_GLSL,
        frag_resources=[
            ngl.UniformVec3(value=COLORS.orange, label="color"),
            ngl.UniformVec2(value=(0.65, 0.5), label="center"),
        ],
    )
    b = ngl.DrawRect(rect=(0, 0, W, H), fill=fill_b, blending=op)

    # White background drawn behind the result using dst_over
    bg = ngl.DrawRect(
        rect=(0, 0, W, H),
        fill=ngl.ColorFill(color=(1.0, 1.0, 1.0, 1.0)),
        blending="dst_over",
    )

    return ngl.Group(children=[a, b, bg])


def _get_compositing_func(op):
    @test_png(width=W, height=H)
    @ngl.scene()
    def scene_func(cfg: ngl.SceneCfg):
        return _get_compositing_scene(cfg, op)

    return scene_func


for operator in _OPERATORS:
    globals()[f"compositing_{operator}"] = _get_compositing_func(operator)
