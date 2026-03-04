#
# Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
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
import os
import tempfile

from PIL import Image

import pynopegl as ngl
from pynopegl_utils.tests.cmp_png import test_png

W, H = 128, 128
_N = 8


def _scene(cfg, node, duration=1.0):
    cfg.aspect_ratio = (W, H)
    cfg.duration = duration
    return node


# -- Histogram -----------------------------------------------------------------

_COMPUTE_HISTOGRAM_CLEAR = """
void main()
{
    uint i = gl_GlobalInvocationID.x;
    atomicAnd(hist.r[i], 0U);
    atomicAnd(hist.g[i], 0U);
    atomicAnd(hist.b[i], 0U);
    atomicAnd(hist.max.r, 0U);
    atomicAnd(hist.max.g, 0U);
    atomicAnd(hist.max.b, 0U);
}
"""

_COMPUTE_HISTOGRAM_EXEC = """
void main()
{
    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;

    ivec2 size = imageSize(source);
    if (x < uint(size.x) && y < uint(size.y)) {
        vec4 color = imageLoad(source, ivec2(x, y));
        uvec4 ucolor = uvec4(color * (%(hsize)d.0 - 1.0));
        uint r = atomicAdd(hist.r[ucolor.r], 1U);
        uint g = atomicAdd(hist.g[ucolor.g], 1U);
        uint b = atomicAdd(hist.b[ucolor.b], 1U);
        atomicMax(hist.max.r, r);
        atomicMax(hist.max.g, g);
        atomicMax(hist.max.b, b);
    }
}
"""


@test_png(width=W, height=H)
@ngl.scene()
def compute_histogram(cfg: ngl.SceneCfg):
    hsize, size, local_size = _N * _N, _N, _N // 2

    img = Image.new("RGBA", (size, size))
    pixels = []
    for _ in range(size * size):
        r = int(cfg.rng.uniform(0.0, 0.5) * 255)
        g = int(cfg.rng.uniform(0.25, 0.75) * 255)
        b = int(cfg.rng.uniform(0.5, 1.0) * 255)
        pixels.append((r, g, b, 255))
    img.putdata(pixels)
    img_path = os.path.join(tempfile.gettempdir(), "ngl_compute_histogram_source.png")
    img.save(img_path)

    texture = ngl.Texture2D(
        data_src=ngl.Media(filename=img_path),
        min_filter="nearest",
        mag_filter="nearest",
    )

    histogram_block = ngl.Block(layout="std140", label="hist")
    histogram_block.add_fields(
        ngl.BufferUInt(hsize, label="r"),
        ngl.BufferUInt(hsize, label="g"),
        ngl.BufferUInt(hsize, label="b"),
        ngl.UniformUIVec3(label="max"),
    )

    shader_params = dict(hsize=hsize, size=size, local_size=local_size)

    group_size = hsize // local_size
    clear_histogram_shader = _COMPUTE_HISTOGRAM_CLEAR % shader_params
    clear_histogram_program = ngl.ComputeProgram(clear_histogram_shader, workgroup_size=(local_size, 1, 1))
    clear_histogram_program.update_properties(hist=ngl.ResourceProps(writable=True))
    clear_histogram = ngl.Compute(
        workgroup_count=(group_size, 1, 1),
        program=clear_histogram_program,
        label="clear_histogram",
    )
    clear_histogram.update_resources(hist=histogram_block)

    group_size = size // local_size
    exec_histogram_shader = _COMPUTE_HISTOGRAM_EXEC % shader_params
    exec_histogram_program = ngl.ComputeProgram(exec_histogram_shader, workgroup_size=(local_size, local_size, 1))
    exec_histogram_program.update_properties(hist=ngl.ResourceProps(writable=True))
    exec_histogram = ngl.Compute(
        workgroup_count=(group_size, group_size, 1), program=exec_histogram_program, label="compute_histogram"
    )
    exec_histogram.update_resources(hist=histogram_block, source=texture)
    exec_histogram_program.update_properties(source=ngl.ResourceProps(as_image=True))

    fill = ngl.CustomFill(
        color_glsl="""
            uint x = uint(uv.x * %(size)d.0);
            uint y = uint(uv.y * %(size)d.0);
            uint i = clamp(x + y * %(size)dU, 0U, %(hsize)dU - 1U);
            vec3 rgb = vec3(hist.r[i], hist.g[i], hist.b[i]) / vec3(hist.max);
            return vec4(rgb, 1.0);
        """
        % shader_params,
        frag_resources=[histogram_block],
    )
    draw = ngl.DrawRect(rect=(0, 0, W, H), fill=fill)

    return _scene(cfg, ngl.Group(children=[clear_histogram, exec_histogram, draw]))


# -- Image load/store (2D) ----------------------------------------------------

_IMAGE_LOAD_STORE_COMPUTE = """
void main()
{
    ivec2 pos = ivec2(gl_LocalInvocationID.xy);
    vec4 color;
    color.r = imageLoad(texture_r, pos).r;
    color.g = imageLoad(texture_g, pos).r;
    color.b = imageLoad(texture_b, pos).r;
    color.a = 1.0;
    color.rgb = (color.rgb * scale.factors.x) + scale.factors.y;
    imageStore(texture_rgba, pos, color);
}
"""


@test_png(width=W, height=H)
@ngl.scene()
def compute_image_load_store(cfg: ngl.SceneCfg):
    size = _N
    texture_data = ngl.BufferFloat(data=array.array("f", [x / (size**2) for x in range(size**2)]))
    texture_r = ngl.Texture2D(
        format="r32_sfloat", width=size, height=size, data_src=texture_data, min_filter="nearest", mag_filter="nearest"
    )
    texture_g = ngl.Texture2D(
        format="r32_sfloat", width=size, height=size, data_src=texture_data, min_filter="nearest", mag_filter="nearest"
    )
    texture_b = ngl.Texture2D(
        format="r32_sfloat", width=size, height=size, data_src=texture_data, min_filter="nearest", mag_filter="nearest"
    )
    scale = ngl.Block(
        fields=[ngl.UniformVec2(value=(-1.0, 1.0), label="factors")],
        layout="std140",
    )
    texture_rgba = ngl.Texture2D(width=size, height=size, min_filter="nearest", mag_filter="nearest")

    program = ngl.ComputeProgram(_IMAGE_LOAD_STORE_COMPUTE, workgroup_size=(size, size, 1))
    program.update_properties(
        texture_r=ngl.ResourceProps(as_image=True),
        texture_g=ngl.ResourceProps(as_image=True),
        texture_b=ngl.ResourceProps(as_image=True),
        texture_rgba=ngl.ResourceProps(as_image=True, writable=True),
    )
    compute = ngl.Compute(workgroup_count=(1, 1, 1), program=program)
    compute.update_resources(
        texture_r=texture_r, texture_g=texture_g, texture_b=texture_b, scale=scale, texture_rgba=texture_rgba
    )

    fill = ngl.TextureFill(texture=texture_rgba, scaling="none")
    draw = ngl.DrawRect(rect=(0, 0, W, H), fill=fill)
    return _scene(cfg, ngl.Group(children=[compute, draw]))


# -- Image layered load/store (3D / 2D array) ---------------------------------

_IMAGE_LAYERED_STORE_COMPUTE = """
void main()
{
    ivec2 pos = ivec2(gl_LocalInvocationID.xy);
    float size = float(imageSize(texture).x);
    float color = (float(pos.y) * size + float(pos.x)) / (size * size);
    imageStore(texture, ivec3(pos, 0), vec4(color));
    imageStore(texture, ivec3(pos, 1), vec4(color));
    imageStore(texture, ivec3(pos, 2), vec4(color));
}
"""

_IMAGE_LAYERED_LOAD_STORE_COMPUTE = """
void main()
{
    ivec2 pos = ivec2(gl_LocalInvocationID.xy);
    vec4 color;
    color.r = imageLoad(texture, ivec3(pos, 0)).r;
    color.g = imageLoad(texture, ivec3(pos, 1)).r;
    color.b = imageLoad(texture, ivec3(pos, 2)).r;
    color.a = 1.0;
    color.rgb = color.rgb * scale.factors.x + scale.factors.y;
    imageStore(texture_rgba, pos, color);
}
"""


def _get_compute_image_layered_load_store_scene(cfg, texture_cls):
    size = _N
    texture = texture_cls(
        format="r32_sfloat",
        width=size,
        height=size,
        depth=3,
        min_filter="nearest",
        mag_filter="nearest",
    )
    texture_rgba = ngl.Texture2D(width=size, height=size, min_filter="nearest", mag_filter="nearest")

    program_store = ngl.ComputeProgram(_IMAGE_LAYERED_STORE_COMPUTE, workgroup_size=(size, size, 1))
    program_store.update_properties(texture=ngl.ResourceProps(as_image=True, writable=True))
    compute_store = ngl.Compute(workgroup_count=(1, 1, 1), program=program_store)
    compute_store.update_resources(texture=texture)

    scale = ngl.Block(
        fields=[ngl.UniformVec2(value=(-1.0, 1.0), label="factors")],
        layout="std140",
    )
    program_load_store = ngl.ComputeProgram(_IMAGE_LAYERED_LOAD_STORE_COMPUTE, workgroup_size=(size, size, 1))
    program_load_store.update_properties(
        texture=ngl.ResourceProps(as_image=True),
        texture_rgba=ngl.ResourceProps(as_image=True, writable=True),
    )
    compute_load_store = ngl.Compute(workgroup_count=(1, 1, 1), program=program_load_store)
    compute_load_store.update_resources(texture=texture, texture_rgba=texture_rgba, scale=scale)

    fill = ngl.TextureFill(texture=texture_rgba, scaling="none")
    draw = ngl.DrawRect(rect=(0, 0, W, H), fill=fill)
    return _scene(cfg, ngl.Group(children=[compute_store, compute_load_store, draw]))


@test_png(width=W, height=H)
@ngl.scene()
def compute_image_3d_load_store(cfg: ngl.SceneCfg):
    return _get_compute_image_layered_load_store_scene(cfg, ngl.Texture3D)


@test_png(width=W, height=H)
@ngl.scene()
def compute_image_2d_array_load_store(cfg: ngl.SceneCfg):
    return _get_compute_image_layered_load_store_scene(cfg, ngl.Texture2DArray)


# -- Image cube load/store ----------------------------------------------------

_IMAGE_CUBE_STORE_COMPUTE = """
void main()
{
    ivec3 pos = ivec3(gl_LocalInvocationID.xyz);
    vec4 color = vec4(0.0);
    if (pos.z == 0) // right
        color = vec4(1.0, 0.0, 0.0, 1.0);
    else if (pos.z == 1) // left
        color = vec4(0.0, 1.0, 0.0, 1.0);
    else if (pos.z == 2) // top
        color = vec4(0.0, 0.0, 1.0, 1.0);
    else if (pos.z == 3) // bottom
        color = vec4(1.0, 1.0, 0.0, 1.0);
    else if (pos.z == 4) // back
        color = vec4(0.0, 1.0, 1.0, 1.0);
    else if (pos.z == 5) // front
        color = vec4(1.0, 0.0, 1.0, 1.0);
    imageStore(texture, pos, color);
}
"""

_IMAGE_CUBE_READBACK_COMPUTE = """
vec2 get_sample_cube_coord(vec3 r, out int face)
{
    vec3 r_abs = abs(r);
    vec2 uv;
    float ma;
    if (r_abs.z >= r_abs.x && r_abs.z >= r_abs.y) {
        uv = vec2(sign(r.z) * r.x, -r.y);
        ma = r_abs.z;
        face = r.z < 0.0 ? 5 : 4;
    } else if (r_abs.y >= r_abs.x) {
        uv = vec2(r.x, sign(r.y) * r.z);
        ma = r_abs.y;
        face = r.y < 0.0 ? 3 : 2;
    } else {
        uv = vec2(sign(r.x) * -r.z, -r.y);
        ma = r_abs.x;
        face = r.x < 0.0 ? 1 : 0;
    }
    return (uv / ma + 1.0) / 2.0;
}

void main()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 out_size = imageSize(output_tex);
    vec2 ndc = (vec2(pos) + 0.5) / vec2(out_size) * 2.0 - 1.0;

    int face = 0;
    vec2 tc = get_sample_cube_coord(vec3(ndc, 0.5), face);
    float tex_size = float(imageSize(cube_tex).x);
    vec4 color = imageLoad(cube_tex, ivec3(tc * tex_size, face));
    imageStore(output_tex, pos, color);
}
"""


@test_png(width=W, height=H)
@ngl.scene()
def compute_image_cube_load_store(cfg: ngl.SceneCfg):
    size = _N
    texture = ngl.TextureCube(format="r32g32b32a32_sfloat", size=size, min_filter="nearest", mag_filter="nearest")

    program_store = ngl.ComputeProgram(_IMAGE_CUBE_STORE_COMPUTE, workgroup_size=(size, size, 6))
    program_store.update_properties(texture=ngl.ResourceProps(as_image=True, writable=True))
    compute_store = ngl.Compute(workgroup_count=(1, 1, 1), program=program_store)
    compute_store.update_resources(texture=texture)

    local_size = 16
    output_tex = ngl.Texture2D(width=W, height=H, min_filter="nearest", mag_filter="nearest")
    program_readback = ngl.ComputeProgram(
        _IMAGE_CUBE_READBACK_COMPUTE, workgroup_size=(local_size, local_size, 1)
    )
    program_readback.update_properties(
        cube_tex=ngl.ResourceProps(as_image=True),
        output_tex=ngl.ResourceProps(as_image=True, writable=True),
    )
    compute_readback = ngl.Compute(
        workgroup_count=(W // local_size, H // local_size, 1), program=program_readback
    )
    compute_readback.update_resources(cube_tex=texture, output_tex=output_tex)

    fill = ngl.TextureFill(texture=output_tex, scaling="none")
    draw = ngl.DrawRect(rect=(0, 0, W, H), fill=fill)
    return _scene(cfg, ngl.Group(children=[compute_store, compute_readback, draw]))
