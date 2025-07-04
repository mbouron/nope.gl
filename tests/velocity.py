#
# Copyright 2021-2022 GoPro Inc.
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

import textwrap

import pynopegl as ngl
from pynopegl_utils.misc import get_shader
from pynopegl_utils.tests.cmp_fingerprint import test_fingerprint
from pynopegl_utils.toolbox.colors import COLORS
from pynopegl_utils.toolbox.shapes import equilateral_triangle_coords


@test_fingerprint(width=320, height=320, keyframes=20, tolerance=1)
@ngl.scene()
def velocity_triangle_rotate(cfg: ngl.SceneCfg):
    cfg.duration = 5.0
    cfg.aspect_ratio = (1, 1)

    anim_kf = [
        ngl.AnimKeyFrameFloat(0, 0),
        ngl.AnimKeyFrameFloat(cfg.duration, 360 * 3, "circular_in_out"),
    ]
    anim = ngl.AnimatedFloat(anim_kf)
    velocity = ngl.VelocityFloat(anim)

    frag = textwrap.dedent(
        """\
    void main()
    {
        float v = clamp(velocity / 3000., 0.0, 1.0);
        ngl_out_color = vec4(v, v / 2.0, 0.0, 1.0);
    }
    """
    )

    p0, p1, p2 = equilateral_triangle_coords(2.0)
    triangle = ngl.DrawColor(COLORS.white, geometry=ngl.Triangle(p0, p1, p2))
    triangle = ngl.Rotate(triangle, angle=anim)

    prog_c = ngl.Program(vertex=get_shader("color.vert"), fragment=frag)
    circle = ngl.Draw(ngl.Circle(radius=1.0, npoints=128), prog_c)
    circle.update_frag_resources(velocity=velocity)
    return ngl.Group(children=[circle, triangle])


@test_fingerprint(width=320, height=320, keyframes=20, tolerance=1)
@ngl.scene()
def velocity_circle_distort_2d(cfg: ngl.SceneCfg):
    cfg.duration = 4.0
    cfg.aspect_ratio = (1, 1)

    coords = list(equilateral_triangle_coords())
    coords.append(coords[0])

    pos_kf = [ngl.AnimKeyFrameVec2(cfg.duration * i / 3.0, pos[0:2], "exp_in_out") for i, pos in enumerate(coords)]
    anim = ngl.AnimatedVec2(pos_kf)
    velocity = ngl.VelocityVec2(anim)

    vert = textwrap.dedent(
        """\
        void main()
        {
            float distort_max = 0.1;
            float velocity_l = length(velocity);
            float direction_l = length(ngl_position);
            vec2 normed_velocity = velocity_l == 0.0 ? vec2(0.0) : -velocity / velocity_l;
            vec2 normed_direction = direction_l == 0.0 ? vec2(0.0) : ngl_position.xy / direction_l;
            float distort = clamp(dot(normed_velocity, normed_direction) / 2.0 * distort_max, 0.0, 1.0);
            vec4 pos = vec4(ngl_position, 1.0) + vec4(translate, 0.0, 0.0) + vec4(-distort * velocity, 0.0, 0.0);
            ngl_out_pos = ngl_projection_matrix * ngl_modelview_matrix * pos;
        }
        """
    )

    geom = ngl.Circle(radius=0.2, npoints=128)
    prog = ngl.Program(vertex=vert, fragment=get_shader("color.frag"))
    shape = ngl.Draw(geom, prog)
    shape.update_frag_resources(color=ngl.UniformVec3(COLORS.white), opacity=ngl.UniformFloat(1))
    shape.update_vert_resources(velocity=velocity, translate=anim)
    return shape


@test_fingerprint(width=320, height=320, keyframes=20, tolerance=1)
@ngl.scene()
def velocity_circle_distort_3d(cfg: ngl.SceneCfg):
    cfg.duration = 4.0
    cfg.aspect_ratio = (1, 1)

    coords = list(equilateral_triangle_coords())
    coords.append(coords[0])
    pos_kf = [ngl.AnimKeyFrameVec3(cfg.duration * i / 3.0, pos, "exp_in_out") for i, pos in enumerate(coords)]
    anim = ngl.AnimatedVec3(pos_kf)
    velocity = ngl.VelocityVec3(anim)

    vert = textwrap.dedent(
        """\
        void main()
        {
            float distort_max = 0.1;
            float velocity_l = length(velocity);
            float direction_l = length(ngl_position);
            vec3 normed_velocity = velocity_l == 0.0 ? vec3(0.0) : -velocity / velocity_l;
            vec3 normed_direction = direction_l == 0.0 ? vec3(0.0) : ngl_position / direction_l;
            float distort = clamp(dot(normed_velocity, normed_direction) / 2.0 * distort_max, 0.0, 1.0);
            vec4 pos = vec4(ngl_position, 1.0) + vec4(-distort * velocity, 0.0);
            ngl_out_pos = ngl_projection_matrix * ngl_modelview_matrix * pos;
        }
        """
    )

    geom = ngl.Circle(radius=0.2, npoints=128)
    prog = ngl.Program(vertex=vert, fragment=get_shader("color.frag"))
    shape = ngl.Draw(geom, prog)
    shape.update_frag_resources(color=ngl.UniformVec3(COLORS.white), opacity=ngl.UniformFloat(1))
    shape.update_vert_resources(velocity=velocity)
    return ngl.Translate(shape, vector=anim)
