#
# Copyright 2020-2022 GoPro Inc.
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
from pynopegl_utils.tests.data import (
    LAYOUTS,
    gen_floats,
    gen_ints,
    get_data_debug_positions,
    get_field_scene,
    match_fields,
)
from pynopegl_utils.toolbox.colors import COLORS

_RECT_SIZE = 128


def _get_group_reorder_function():
    colors = [
        COLORS.white,
        COLORS.orange,
        COLORS.sgreen,
        COLORS.rose,
    ]
    offset = _RECT_SIZE / len(colors)
    draws = []
    for i, color in enumerate(colors):
        tx = -_RECT_SIZE / 2 + (i + 0.5) * offset
        ty = tx
        draw = ngl.DrawRect(
            rect=(0, 0, _RECT_SIZE, _RECT_SIZE),
            fill=ngl.ColorFill(color=color + (1.0,)),
            translate=(tx, ty, 0),
        )
        draws.append(draw)
    group = ngl.Group(children=draws)

    def _reoder_group(t_id: int):
        if t_id == 1:
            group.swap_children(0, 3)
            group.swap_children(1, 2)
        elif t_id == 2:
            group.swap_children(0, 3)
            group.swap_children(1, 2)

    @test_png(
        width=_RECT_SIZE,
        height=_RECT_SIZE,
        threshold=1,
        exercise_serialization=False,
        keyframes_callback=_reoder_group,
        keyframes=[0.0, 1.0, 2.0],
    )
    @ngl.scene()
    def live_group_reorder_func(cfg: ngl.SceneCfg):
        cfg.aspect_ratio = (1, 1)
        return group

    return live_group_reorder_func


live_group_reorder = _get_group_reorder_function()


def _get_live_shared_uniform_scene(color):
    group = ngl.Group()
    for i in range(2):
        fill = ngl.CustomFill(
            color_glsl="return vec4(color.rgb, 1.0);",
            frag_resources=[color],
        )
        x = i * (_RECT_SIZE // 2)
        y = i * (_RECT_SIZE // 2)
        draw = ngl.DrawRect(rect=(x, y, _RECT_SIZE // 2, _RECT_SIZE // 2), fill=fill)
        group.add_children(draw)
    return group


def _get_live_shared_uniform_with_block_scene(color, layout):
    group = ngl.Group()
    for i in range(2):
        block = ngl.Block(fields=[color], layout=layout, label="data")
        fill = ngl.CustomFill(
            color_glsl="return vec4(data.color, 1.0);",
            frag_resources=[block],
        )
        x = i * (_RECT_SIZE // 2)
        y = i * (_RECT_SIZE // 2)
        draw = ngl.DrawRect(rect=(x, y, _RECT_SIZE // 2, _RECT_SIZE // 2), fill=fill)
        group.add_children(draw)
    return group


def _get_live_shared_uniform_function(layout=None):
    data = [COLORS.red, COLORS.blue]
    color = ngl.UniformVec3(value=COLORS.black, label="color")

    def keyframes_callback(t_id):
        color.set_value(*data[t_id])

    @test_png(
        width=_RECT_SIZE,
        height=_RECT_SIZE,
        keyframes=len(data),
        threshold=1,
        exercise_serialization=False,
        keyframes_callback=keyframes_callback,
    )
    @ngl.scene()
    def scene_func(cfg: ngl.SceneCfg):
        cfg.duration = 0
        cfg.aspect_ratio = (1, 1)
        if layout:
            return _get_live_shared_uniform_with_block_scene(color, layout)
        else:
            return _get_live_shared_uniform_scene(color)

    return scene_func


live_shared_uniform = _get_live_shared_uniform_function(None)
live_shared_uniform_std140 = _get_live_shared_uniform_function("std140")
live_shared_uniform_std430 = _get_live_shared_uniform_function("std430")


def _get_media_change_function():
    media0 = ngl.Media(filename=load_media("mire").filename)
    media1 = ngl.Media(filename=load_media("rooster").filename)

    def _change_media(t_id: int):
        if t_id == 1:
            media0.set_filename(load_media("cat").filename)
            media1.set_filename(load_media("panda").filename)

    @test_png(
        width=_RECT_SIZE,
        height=_RECT_SIZE,
        threshold=1,
        exercise_serialization=False,
        keyframes_callback=_change_media,
        keyframes=[0.0, 0.0, 12.0],
    )
    @ngl.scene()
    def live_media_change_func(cfg: ngl.SceneCfg):
        cfg.aspect_ratio = (1, 1)
        return ngl.Group(
            children=[
                ngl.TimeRangeFilter(
                    child=ngl.DrawRect(
                        rect=(0, 0, _RECT_SIZE, _RECT_SIZE),
                        fill=ngl.TextureFill(texture=ngl.Texture2D(data_src=media0)),
                    ),
                    start=0,
                    end=10,
                ),
                ngl.TimeRangeFilter(
                    child=ngl.DrawRect(
                        rect=(0, 0, _RECT_SIZE, _RECT_SIZE),
                        fill=ngl.TextureFill(texture=ngl.Texture2D(data_src=media1)),
                    ),
                    start=10,
                    end=20,
                ),
            ]
        )

    return live_media_change_func


live_media_change = _get_media_change_function()


def _get_timerangefilter_update_function():
    draw = ngl.DrawRect(
        rect=(0, 0, _RECT_SIZE, _RECT_SIZE),
        fill=ngl.ColorFill(color=COLORS.orange + (1.0,)),
    )
    range = ngl.TimeRangeFilter(draw, start=5.0, end=10.0)

    def _update_timerange(t_id: int):
        if t_id == 0:
            pass
        elif t_id == 1:
            range.set_range(0.0, 5.0)
        elif t_id == 2:
            range.set_start(0.0)
            range.set_end(15.0)
        elif t_id == 3:
            range.set_range(10.0, 15.0)

    @test_png(
        width=_RECT_SIZE,
        height=_RECT_SIZE,
        threshold=1,
        exercise_serialization=False,
        keyframes_callback=_update_timerange,
        keyframes=[0.0, 0.0, 10.0, 15.0],
    )
    @ngl.scene()
    def live_timerangefilter_update_func(cfg: ngl.SceneCfg):
        cfg.aspect_ratio = (1, 1)
        return range

    return live_timerangefilter_update_func


live_timerangefilter_update = _get_timerangefilter_update_function()


def _get_live_spec():
    livechange_b = [[True], [False]]
    livechange_f = [[v] for v in gen_floats(4)[1:3]]
    livechange_v2 = gen_floats(2), gen_floats(2)[::-1]
    livechange_v3 = gen_floats(3), gen_floats(3)[::-1]
    livechange_v4 = gen_floats(4), gen_floats(4)[::-1]
    livechange_i = [[v] for v in gen_ints(4)[1:3]]
    livechange_mat4 = gen_floats(4 * 4), gen_floats(4 * 4)[::-1]
    livechange_quat = livechange_v4

    # fmt: off
    spec = [
        dict(name="b",  type="bool",      category="single", livechange=livechange_b),
        dict(name="f",  type="float",     category="single", livechange=livechange_f),
        dict(name="v2", type="vec2",      category="single", livechange=livechange_v2),
        dict(name="v3", type="vec3",      category="single", livechange=livechange_v3),
        dict(name="v4", type="vec4",      category="single", livechange=livechange_v4),
        dict(name="i",  type="int",       category="single", livechange=livechange_i),
        dict(name="m4", type="mat4",      category="single", livechange=livechange_mat4),
        dict(name="qm", type="quat_mat4", category="single", livechange=livechange_quat),
        dict(name="qv", type="quat_vec4", category="single", livechange=livechange_quat),
    ]
    # fmt: on

    spec.append(
        dict(
            name="c",
            type="color",
            func=lambda data: ngl.UniformColor(data, space="hsl"),
            category="single",
            livechange=livechange_v3,
        ),
    )

    return spec


def _get_live_trf_spec():
    t0 = ngl.Identity()
    t1 = ngl.Transform(t0)
    t2 = ngl.Translate(t1)
    t3 = ngl.Rotate(t2)
    t4 = ngl.Scale(t3)
    t5 = ngl.RotateQuat(t4)
    t6 = ngl.Skew(t5)

    return [
        dict(
            name="m4",
            type="mat4",
            category="single",
            func=lambda data: ngl.UniformMat4(data, transform=t6),
            livechange=(
                # fmt: off
                lambda: t1.set_matrix(                      # trf_step=1
                    0.1, 0.2, 0.0, 0.0,
                    0.0, 0.3, 0.4, 0.0,
                    0.0, 0.0, 0.5, 0.0,
                    0.8, 0.7, 0.0, 0.6,
                ),
                lambda: t2.set_vector(0.2, 0.7, -0.4),      # trf_step=2
                lambda: t3.set_angle(123.4),                # trf_step=3
                lambda: t4.set_factors(0.7, 1.4, 0.2),      # trf_step=4
                lambda: t5.set_quat(0, 0, -0.474, 0.880),   # trf_step=5
                lambda: t6.set_angles(0, 50, -145),         # trf_step=6
                lambda: t6.set_angles(0, 0, 0),
                lambda: t5.set_quat(0, 0, 0, 1),
                lambda: t4.set_factors(1, 1, 1),
                lambda: t3.set_angle(0),
                lambda: t2.set_vector(0, 0, 0),
                lambda: t1.set_matrix(
                    1.0, 0.0, 0.0, 0.0,
                    0.0, 1.0, 0.0, 0.0,
                    0.0, 0.0, 1.0, 0.0,
                    0.0, 0.0, 0.0, 1.0,
                )
                # fmt: on
            ),
        ),
    ]


def _get_live_function(spec, category, field_type, layout):
    fields = match_fields(spec, category, field_type)
    assert len(fields) == 1
    field = fields[0]
    data_src = field["livechange"]

    def keyframes_callback(t_id):
        if t_id:
            v = data_src[t_id - 1]
            field["node"].set_value(*v)

    @test_png(
        width=_RECT_SIZE,
        height=_RECT_SIZE,
        keyframes=len(data_src) + 1,
        threshold=1,
        exercise_serialization=False,
        keyframes_callback=keyframes_callback,
    )
    @ngl.scene(
        controls=dict(
            seed=ngl.scene.Range(range=[0, 100]),
            color_tint=ngl.scene.Bool(),
        )
    )
    def scene_func(cfg: ngl.SceneCfg, seed=0, color_tint=False):
        cfg.duration = 0
        return get_field_scene(cfg, spec, category, field_type, seed, False, layout, color_tint)

    return scene_func


def _get_live_trf_function(spec, category, field_type, layout):
    fields = match_fields(spec, category, field_type)
    assert len(fields) == 1
    field = fields[0]
    livechange_funcs = field["livechange"]

    def keyframes_callback(t_id):
        livechange_funcs[t_id]()

    @test_png(
        width=_RECT_SIZE,
        height=_RECT_SIZE,
        keyframes=len(livechange_funcs),
        threshold=1,
        exercise_serialization=False,
        keyframes_callback=keyframes_callback,
    )
    @ngl.scene(
        controls=dict(
            seed=ngl.scene.Range(range=[0, 100]),
            color_tint=ngl.scene.Bool(),
            trf_step=ngl.scene.Range(range=[0, len(livechange_funcs)]),
        )
    )
    def scene_func(cfg: ngl.SceneCfg, seed=0, color_tint=False, trf_step=0):
        cfg.duration = 0
        s = get_field_scene(cfg, spec, category, field_type, seed, False, layout, color_tint)
        for i in range(trf_step):
            keyframes_callback(i)
        return s

    return scene_func


def _bootstrap():
    for layout in LAYOUTS:
        spec = _get_live_spec()
        for field_info in spec:
            category, field_type = field_info["category"], field_info["type"]
            func_name = f"live_{category}_{field_type}_{layout}"
            globals()[func_name] = _get_live_function(spec, category, field_type, layout)

        spec = _get_live_trf_spec()
        for field_info in spec:
            category, field_type = field_info["category"], field_info["type"]
            func_name = f"live_trf_{category}_{field_type}_{layout}"
            globals()[func_name] = _get_live_trf_function(spec, category, field_type, layout)


_bootstrap()
