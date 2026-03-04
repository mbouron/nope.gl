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
from pynopegl_utils.toolbox.colors import COLORS

W, H = 128, 128


def _get_userlive_switch_func():
    # Three differently colored/sized DrawRect nodes at different positions
    scene0 = ngl.DrawRect(
        rect=(4, 32, 40, 64),
        fill=ngl.ColorFill(color=(*COLORS.white, 1.0)),
    )
    scene1 = ngl.DrawRect(
        rect=(32, 16, 64, 96),
        fill=ngl.ColorFill(color=(*COLORS.red, 1.0)),
    )
    scene2 = ngl.DrawRect(
        rect=(84, 32, 40, 64),
        fill=ngl.ColorFill(color=(*COLORS.azure, 1.0)),
    )

    switch0 = ngl.UserSwitch(scene0)
    switch1 = ngl.UserSwitch(scene1)
    switch2 = ngl.UserSwitch(scene2)

    def keyframes_callback(t_id):
        # Build a "random" composition of switches
        switch0.set_enabled(t_id % 2 == 0)
        switch1.set_enabled(t_id % 3 == 0)
        switch2.set_enabled(t_id % 4 == 0)

    @test_png(
        width=W,
        height=H,
        keyframes=10,
        keyframes_callback=keyframes_callback,
        exercise_serialization=False,
    )
    @ngl.scene(controls=dict(s0=ngl.scene.Bool(), s1=ngl.scene.Bool(), s2=ngl.scene.Bool()))
    def scene_func(cfg: ngl.SceneCfg, s0_enabled=True, s1_enabled=True, s2_enabled=True):
        cfg.aspect_ratio = (W, H)
        switch0.set_enabled(s0_enabled)
        switch1.set_enabled(s1_enabled)
        switch2.set_enabled(s2_enabled)
        return ngl.Group(children=[switch0, switch1, switch2])

    return scene_func


def _get_userlive_select_func():
    # Background rect
    below = ngl.DrawRect(
        rect=(16, 32, 48, 64),
        fill=ngl.ColorFill(color=(*COLORS.white, 1.0)),
        opacity=0.5,
        blending="src_over",
    )

    # Foreground rect at a different position
    above_plain = ngl.DrawRect(
        rect=(64, 16, 48, 64),
        fill=ngl.ColorFill(color=(*COLORS.white, 1.0)),
        opacity=0.5,
        blending="src_over",
    )

    # Same rect with different blending for branch 1 (additive-like via src_over)
    above_bright = ngl.DrawRect(
        rect=(64, 16, 48, 64),
        fill=ngl.ColorFill(color=(1.0, 1.0, 1.0, 0.8)),
        blending="src_over",
    )

    # Same rect with low opacity for branch 2 (darker effect)
    above_dark = ngl.DrawRect(
        rect=(64, 16, 48, 64),
        fill=ngl.ColorFill(color=(0.3, 0.3, 0.3, 0.5)),
        blending="src_over",
    )

    select = ngl.UserSelect(branches=[above_plain, above_bright, above_dark])

    def keyframes_callback(t_id):
        # 4 states: 3 blending branches and one extra for nothing
        # (branch ID overflow). We remain on each state for 2 frames.
        select.set_branch((t_id // 2) % 4)

    @test_png(
        width=W,
        height=H,
        keyframes=8,
        keyframes_callback=keyframes_callback,
        exercise_serialization=False,
    )
    @ngl.scene(controls=dict(branch=ngl.scene.Range([0, 3])))
    def scene_func(cfg: ngl.SceneCfg, branch=0):
        cfg.aspect_ratio = (W, H)
        select.set_branch(branch)
        return ngl.Group(children=[below, select])

    return scene_func


userlive_switch = _get_userlive_switch_func()
userlive_select = _get_userlive_select_func()
