#
# Copyright 2022 GoPro Inc.
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
from pynopegl_utils.toolbox.colors import COLORS


def _draw_quad(corner=(-1, -1, 0), width=(2, 0, 0), height=(0, 2, 0), color=(1, 1, 1), opacity=1.0, **kwargs):
    quad = ngl.Quad(corner, width, height)
    return ngl.DrawColor(color, opacity=opacity, geometry=quad, blend_mode="src_over", **kwargs)


@test_render(keyframes=2, tolerance=1)
@ngl.scene(width=16, height=16)
def depth_stencil_depth(cfg: ngl.SceneCfg):
    group = ngl.Group()

    count = 4
    for i in range(count):
        depth = (i + 1) / count
        corner = (-1 + (count - 1 - i) * 2 / count, -1, depth)
        draw = _draw_quad(corner=corner, color=(depth, depth, depth), depth_mode="read_write")
        group.add_children(draw)

    for i, depth in enumerate((0.4, 0.6)):
        corner = (-1, -0.5 + 0.25 * i, depth)
        height = (0, 1 - 0.25 * i * 2, 0)
        draw = _draw_quad(corner=corner, height=height, color=COLORS.red, opacity=0.5, depth_mode="read_only")
        group.add_children(draw)

    return group


@test_render(keyframes=2, tolerance=1)
@ngl.scene(width=16, height=16)
def depth_stencil_stencil(cfg: ngl.SceneCfg):
    group = ngl.Group()

    # Write the stencil mask
    for xpos in (-1, 0):
        draw = _draw_quad(
            corner=(xpos, -1, 0),
            width=(0.5, 0, 0),
            color=COLORS.black,
            color_write_mask="",
            stencil_mode="write",
        )
        group.add_children(draw)

    # Draw white where the stencil mask is set
    draw = _draw_quad(color=COLORS.white, stencil_mode="read")
    group.add_children(draw)

    return group
