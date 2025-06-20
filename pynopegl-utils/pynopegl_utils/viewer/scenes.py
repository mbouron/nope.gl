#
# Copyright 2023 Clément Bœsch <u pkh.me>
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


@ngl.scene(controls=dict(shape=ngl.scene.List(choices=["triangle", "square", "circle"])))
def demo(cfg: ngl.SceneCfg, shape="square"):
    cfg.aspect_ratio = (1, 1)
    cfg.duration = 3

    if shape == "triangle":
        geometry = ngl.Triangle()
    elif shape == "square":
        geometry = ngl.Quad()
    elif shape == "circle":
        geometry = ngl.Circle(radius=0.6, npoints=128)
    else:
        assert False

    ulinear = ngl.UniformBool(value=True, live_id="linear")
    gradient = ngl.DrawGradient4(
        color_tl=ngl.UniformColor(value=(1, 0.5, 0), live_id="top-left"),
        color_tr=ngl.UniformColor(value=(0, 1, 0), live_id="top-right"),
        color_br=ngl.UniformColor(value=(0, 0.5, 1), live_id="bottom-right"),
        color_bl=ngl.UniformColor(value=(1, 0, 1), live_id="bottom-left"),
        linear=ulinear,
        geometry=geometry,
    )

    text = ngl.Text(
        "Hello",
        bg_color=(0, 0, 0),
        bg_opacity=0,
        box=(-0.5, -0.5, 1.0, 1.0),
        live_id="text",
    )
    scene = ngl.Group(children=[gradient, text])

    urotate = ngl.UniformFloat(value=15, live_id="angle", live_min=0, live_max=360)
    scene = ngl.Rotate(scene, angle=urotate)

    scale_animkf = [
        ngl.AnimKeyFrameVec3(0, (0.7, 0.7, 0.7)),
        ngl.AnimKeyFrameVec3(cfg.duration / 2, (1.3, 1.3, 1.3), "exp_out"),
        ngl.AnimKeyFrameVec3(cfg.duration, (0.7, 0.7, 0.7), "exp_in"),
    ]
    scene = ngl.Scale(scene, factors=ngl.AnimatedVec3(scale_animkf))

    translate = ngl.UniformVec2(live_id="translate", live_min=(-1.5, -1.5), live_max=(1.5, 1.5))
    translate3 = ngl.EvalVec3("t.x", "t.y", "0", resources=dict(t=translate))

    scene = ngl.Translate(scene, vector=translate3)
    return scene
