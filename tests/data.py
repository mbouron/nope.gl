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

import array

import pynopegl as ngl
from pynopegl_utils.tests.cmp_png import test_png
from pynopegl_utils.tests.data import (
    ANIM_DURATION,
    LAYOUTS,
    gen_floats,
    gen_ints,
    get_field_scene,
    match_fields,
)

W, H = 256, 256
_DATA_SIZE = 128


def _scene(cfg: ngl.SceneCfg, node, duration: float = 1.0):
    cfg.aspect_ratio = (W, H)
    cfg.duration = duration
    return node


# ── Bootstrap data upload tests ──────────────────────────────────────────────


def _get_data_spec(layout, i_count=6, f_count=7, v2_count=5, v3_count=9, v4_count=2, mat_count=3):
    f_list = gen_floats(f_count)
    v2_list = gen_floats(v2_count * 2)
    v3_list = gen_floats(v3_count * 3)
    v4_list = gen_floats(v4_count * 4)
    i_list = gen_ints(i_count)
    iv2_list = [int(x * 256) for x in v2_list]
    iv3_list = [int(x * 256) for x in v3_list]
    iv4_list = [int(x * 256) for x in v4_list]
    mat4_list = gen_floats(mat_count * 4 * 4)
    one_f = gen_floats(1)[0]
    one_v2 = gen_floats(2)
    one_v3 = gen_floats(3)
    one_v4 = gen_floats(4)
    one_i = gen_ints(1)[0]
    one_b = True
    one_iv2 = gen_ints(2)
    one_iv3 = gen_ints(3)
    one_iv4 = gen_ints(4)
    one_u = gen_ints(1)[0]
    one_uv2 = gen_ints(2)
    one_uv3 = gen_ints(3)
    one_uv4 = gen_ints(4)
    one_mat4 = gen_floats(4 * 4)
    one_quat = one_v4

    f_array = array.array("f", f_list)
    v2_array = array.array("f", v2_list)
    v3_array = array.array("f", v3_list)
    v4_array = array.array("f", v4_list)
    i_array = array.array("i", i_list)
    iv2_array = array.array("i", iv2_list)
    iv3_array = array.array("i", iv3_list)
    iv4_array = array.array("i", iv4_list)
    mat4_array = array.array("f", mat4_list)

    spec = []

    # fmt: off
    spec += [dict(name=f"b_{i}",    type="bool",      category="single", data=one_b)    for i in range(i_count)]
    spec += [dict(name=f"f_{i}",    type="float",     category="single", data=one_f)    for i in range(f_count)]
    spec += [dict(name=f"v2_{i}",   type="vec2",      category="single", data=one_v2)   for i in range(v2_count)]
    spec += [dict(name=f"v3_{i}",   type="vec3",      category="single", data=one_v3)   for i in range(v3_count)]
    spec += [dict(name=f"v4_{i}",   type="vec4",      category="single", data=one_v4)   for i in range(v4_count)]
    spec += [dict(name=f"i_{i}",    type="int",       category="single", data=one_i)    for i in range(i_count)]
    spec += [dict(name=f"iv2_{i}",  type="ivec2",     category="single", data=one_iv2)  for i in range(v2_count)]
    spec += [dict(name=f"iv3_{i}",  type="ivec3",     category="single", data=one_iv3)  for i in range(v3_count)]
    spec += [dict(name=f"iv4_{i}",  type="ivec4",     category="single", data=one_iv4)  for i in range(v4_count)]
    spec += [dict(name=f"u_{i}",    type="uint",      category="single", data=one_u)    for i in range(i_count)]
    spec += [dict(name=f"uiv2_{i}", type="uvec2",     category="single", data=one_uv2)  for i in range(v2_count)]
    spec += [dict(name=f"uiv3_{i}", type="uvec3",     category="single", data=one_uv3)  for i in range(v3_count)]
    spec += [dict(name=f"uiv4_{i}", type="uvec4",     category="single", data=one_uv4)  for i in range(v4_count)]
    spec += [dict(name=f"m4_{i}",   type="mat4",      category="single", data=one_mat4) for i in range(mat_count)]
    spec += [dict(name=f"qm_{i}",   type="quat_mat4", category="single", data=one_quat) for i in range(mat_count)]
    spec += [dict(name=f"qv_{i}",   type="quat_vec4", category="single", data=one_quat) for i in range(v4_count)]
    spec += [
        dict(name="t_f",   type="float",     category="array",    data=f_array,    len=f_count),
        dict(name="t_v2",  type="vec2",      category="array",    data=v2_array,   len=v2_count),
        dict(name="t_v3",  type="vec3",      category="array",    data=v3_array,   len=v3_count),
        dict(name="t_v4",  type="vec4",      category="array",    data=v4_array,   len=v4_count),
        dict(name="a_qm4", type="quat_mat4", category="animated", data=one_quat),
        dict(name="a_qv4", type="quat_vec4", category="animated", data=one_quat),
        dict(name="a_f",   type="float",     category="animated", data=None),
        dict(name="a_v2",  type="vec2",      category="animated", data=one_v2),
        dict(name="a_v3",  type="vec3",      category="animated", data=one_v3),
        dict(name="a_v4",  type="vec4",      category="animated", data=one_v4),
    ]

    if layout != "uniform":
        spec += [
            dict(name="t_i",    type="int",        category="array",           data=i_array,    len=i_count),
            dict(name="t_iv2",  type="ivec2",      category="array",           data=iv2_array,  len=v2_count),
            dict(name="t_iv3",  type="ivec3",      category="array",           data=iv3_array,  len=v3_count),
            dict(name="t_iv4",  type="ivec4",      category="array",           data=iv4_array,  len=v4_count),
            dict(name="t_mat4", type="mat4",       category="array",           data=mat4_array, len=mat_count),
        ]
    # fmt: on

    return spec


def _get_data_function(spec, category, field_type, layout):
    keyframes = 5 if "animated" in category else 1

    @test_png(
        width=_DATA_SIZE,
        height=_DATA_SIZE,
        keyframes=keyframes,
        threshold=1,
    )
    @ngl.scene(
        controls=dict(
            seed=ngl.scene.Range(range=[0, 100]),
            color_tint=ngl.scene.Bool(),
        )
    )
    def scene_func(cfg: ngl.SceneCfg, seed=0, color_tint=False):
        cfg.duration = ANIM_DURATION
        return get_field_scene(cfg, spec, category, field_type, seed, False, layout, color_tint, rect_size=_DATA_SIZE)

    return scene_func


for layout in LAYOUTS:
    spec = _get_data_spec(layout)
    test_fields = set((f["category"], f["type"]) for f in spec)
    for category, field_type in test_fields:
        globals()[f"data_{category}_{field_type}_{layout}"] = _get_data_function(spec, category, field_type, layout)


# ── Noise tests ──────────────────────────────────────────────────────────────


@test_png(width=W, height=H, keyframes=10, threshold=1)
@ngl.scene()
def data_noise_time(cfg: ngl.SceneCfg):
    """A small white square that moves across the screen driven by Time and NoiseFloat."""
    cfg.duration = 2

    size = 64
    fill = ngl.ColorFill(color=(1.0, 1.0, 1.0, 1.0))
    rect = ngl.DrawRect(rect=(W // 2 - size // 2, H // 2 - size // 2, size, size), fill=fill)

    translate = ngl.EvalVec3("(t - 1.0) * 100.0", "-signal * 100.0", "0")
    translate.update_resources(t=ngl.Time(), signal=ngl.NoiseFloat(octaves=8))

    return _scene(cfg, ngl.Translate(rect, vector=translate), duration=cfg.duration)


@test_png(width=W, height=H, keyframes=30, threshold=1)
@ngl.scene()
def data_noise_wiggle(cfg: ngl.SceneCfg):
    """A small white square that wiggles around driven by NoiseVec2."""
    cfg.duration = 3

    size = 64
    fill = ngl.ColorFill(color=(1.0, 1.0, 1.0, 1.0))
    rect = ngl.DrawRect(rect=(W // 2 - size // 2, H // 2 - size // 2, size, size), fill=fill)

    translate = ngl.EvalVec3("wiggle.x * 80.0", "wiggle.y * 80.0", "0")
    translate.update_resources(wiggle=ngl.NoiseVec2(octaves=8))

    return _scene(cfg, ngl.Translate(rect, vector=translate), duration=cfg.duration)


# ── Eval tests ──────────────────────────────────────────────────────────────


@test_png(width=W, height=H, keyframes=10, threshold=1)
@ngl.scene()
def data_eval(cfg: ngl.SceneCfg):
    """Test entangled Eval dependencies with CustomFill."""
    cfg.aspect_ratio = (W, H)

    t = ngl.Time()
    vec = ngl.UniformVec3(value=(0.7, 0.3, 4.0))
    a = ngl.EvalFloat("vec.0")
    a.update_resources(vec=vec)
    b = ngl.EvalFloat("vec.t")
    b.update_resources(vec=vec)
    x = ngl.EvalFloat("sin(v.x - v.g + t*v.p)")
    x.update_resources(t=t, v=vec)
    color = ngl.EvalVec4(
        expr0="sat(sin(x + t*4)/2 + wiggle/3)",
        expr1="abs(fract(sin(t + a)))",
        expr2=None,  # re-use expr1
        expr3="1",
    )
    color.update_resources(wiggle=ngl.NoiseFloat(), t=t, a=a, x=x)

    fill = ngl.CustomFill(
        color_glsl="return color;",
        frag_resources=[color],
    )
    return _scene(cfg, ngl.DrawRect(rect=(0, 0, W, H), fill=fill))


# ── Vertex and fragment blocks tests ────────────────────────────────────────


def _data_vertex_and_fragment_blocks(cfg: ngl.SceneCfg, layout):
    """
    This test ensures that the block bindings are properly set by pgcraft
    when UBOs or SSBOs are bound to the fragment stage.
    """
    cfg.aspect_ratio = (W, H)

    src = ngl.Block(
        fields=[
            ngl.UniformVec3(value=(1.0, 0.0, 0.0), label="color"),
            ngl.UniformFloat(value=0.5, label="opacity"),
        ],
        layout="std140",
    )
    dst = ngl.Block(
        fields=[
            ngl.UniformVec3(value=(1.0, 1.0, 1.0), label="color"),
        ],
        layout=layout,
    )

    fill = ngl.CustomFill(
        color_glsl="""\
    vec4 src_val = vec4(src.color, 1.0) * src.opacity;
    vec3 color = src_val.rgb + (1.0 - src_val.a) * dst.color;
    return vec4(color, 1.0);""",
        frag_resources=[src, dst],
    )
    return ngl.DrawRect(rect=(0, 0, W, H), fill=fill)


@test_png(width=W, height=H, keyframes=1, threshold=1)
@ngl.scene()
def data_vertex_and_fragment_blocks(cfg: ngl.SceneCfg):
    return _data_vertex_and_fragment_blocks(cfg, "std140")


@test_png(width=W, height=H, keyframes=1, threshold=1)
@ngl.scene()
def data_vertex_and_fragment_blocks_std430(cfg: ngl.SceneCfg):
    return _data_vertex_and_fragment_blocks(cfg, "std430")


