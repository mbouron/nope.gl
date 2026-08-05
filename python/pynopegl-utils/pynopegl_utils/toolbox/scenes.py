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


def compare(cfg: ngl.SceneCfg, scene0: ngl.Node, scene1: ngl.Node, xsplit: float) -> ngl.Node:
    """
    Compare two scenes by splitting them vertically.
    """

    # Render each scene into its own texture (automatically sized from the
    # viewport dimensions)
    tex0 = ngl.Texture2D(min_filter="nearest", mag_filter="nearest")
    tex1 = ngl.Texture2D(min_filter="nearest", mag_filter="nearest")
    rtt0 = ngl.RenderToTexture(scene0, [tex0])
    rtt1 = ngl.RenderToTexture(scene1, [tex1])

    # Draw the left part of the 1st scene and the right part of the 2nd scene
    # using quads with matching texture coordinates
    quad0 = ngl.Quad(
        corner=(-1, -1, 0),
        width=(2 * xsplit, 0, 0),
        height=(0, 2, 0),
        uv_corner=(0, 0),
        uv_width=(xsplit, 0),
    )
    quad1 = ngl.Quad(
        corner=(-1 + 2 * xsplit, -1, 0),
        width=(2 * (1 - xsplit), 0, 0),
        height=(0, 2, 0),
        uv_corner=(xsplit, 0),
        uv_width=(1 - xsplit, 0),
    )
    draw0 = ngl.DrawTexture(tex0, geometry=quad0, blend_mode="src_over")
    draw1 = ngl.DrawTexture(tex1, geometry=quad1, blend_mode="src_over")

    return ngl.Group(children=[rtt0, rtt1, draw0, draw1])
