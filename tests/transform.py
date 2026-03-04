#
# Copyright 2020-2022 GoPro Inc.
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

import array

import pynopegl as ngl
from pynopegl_utils.tests.cmp_png import test_png

W, H = 320, 320


def _transform_shape():
    """A centered colored rectangle used as the base shape for transform tests."""
    w, h = 192, 116
    x = (W - w) // 2
    y = (H - h) // 2
    fill = ngl.ColorFill(color=(0.9, 0.3, 0.5, 1.0))
    return ngl.DrawRect(rect=(x, y, w, h), fill=fill)


def _scene(cfg: ngl.SceneCfg, node, duration: float = 1.0):
    cfg.aspect_ratio = (W, H)
    cfg.duration = duration
    return node


@test_png(width=W, height=H)
@ngl.scene()
def transform_matrix(cfg: ngl.SceneCfg):
    shape = _transform_shape()
    mat = (
        # fmt: off
        0.5, 0.5, 0.0, 0.0,
       -1.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,
       -0.2, 0.4, 0.0, 1.0,
        # fmt: on
    )
    return _scene(cfg, ngl.Transform(shape, matrix=mat))


@test_png(width=W, height=H)
@ngl.scene()
def transform_translate(cfg: ngl.SceneCfg):
    shape = _transform_shape()
    return _scene(cfg, ngl.Translate(shape, (50, 80, 0)))


@test_png(width=W, height=H, keyframes=8)
@ngl.scene()
def transform_translate_animated(cfg: ngl.SceneCfg):
    # Triangle path in pixel space
    p0 = (-80, -40, 0)
    p1 = (80, -40, 0)
    p2 = (0, 60, 0)
    anim = [
        ngl.AnimKeyFrameVec3(0, p0),
        ngl.AnimKeyFrameVec3(1, p1),
        ngl.AnimKeyFrameVec3(2, p2),
        ngl.AnimKeyFrameVec3(3, p0),
    ]
    shape = _transform_shape()
    return _scene(cfg, ngl.Translate(shape, vector=ngl.AnimatedVec3(anim)), duration=3.0)


@test_png(width=W, height=H)
@ngl.scene()
def transform_scale(cfg: ngl.SceneCfg):
    shape = _transform_shape()
    return _scene(cfg, ngl.Scale(shape, (0.7, 1.4, 0)))


@test_png(width=W, height=H, keyframes=8)
@ngl.scene()
def transform_scale_animated(cfg: ngl.SceneCfg):
    shape = _transform_shape()
    factors = (0.7, 1.4, 0)
    anim = [
        ngl.AnimKeyFrameVec3(0, (0, 0, 0)),
        ngl.AnimKeyFrameVec3(1, factors),
        ngl.AnimKeyFrameVec3(2, (0, 0, 0)),
    ]
    return _scene(cfg, ngl.Scale(shape, factors=ngl.AnimatedVec3(anim)), duration=2.0)


@test_png(width=W, height=H)
@ngl.scene()
def transform_scale_anchor(cfg: ngl.SceneCfg):
    shape = _transform_shape()
    return _scene(cfg, ngl.Scale(shape, (0.7, 1.4, 0), anchor=(100, 120, 0)))


@test_png(width=W, height=H, keyframes=8)
@ngl.scene()
def transform_scale_anchor_animated(cfg: ngl.SceneCfg):
    shape = _transform_shape()
    factors = (0.7, 1.4, 0)
    anchor = (100, 120, 0)
    anim = [
        ngl.AnimKeyFrameVec3(0, (0, 0, 0)),
        ngl.AnimKeyFrameVec3(1, factors),
        ngl.AnimKeyFrameVec3(2, (0, 0, 0)),
    ]
    return _scene(cfg, ngl.Scale(shape, factors=ngl.AnimatedVec3(anim), anchor=anchor), duration=2.0)


@test_png(width=W, height=H)
@ngl.scene()
def transform_skew(cfg: ngl.SceneCfg):
    shape = _transform_shape()
    return _scene(cfg, ngl.Skew(shape, angles=(0.0, -70, 14), axis=(1, 0, 0)))


@test_png(width=W, height=H, keyframes=8)
@ngl.scene()
def transform_skew_animated(cfg: ngl.SceneCfg):
    shape = _transform_shape()
    angles = (0, -60, 14)
    anim = [
        ngl.AnimKeyFrameVec3(0, (0, 0, 0)),
        ngl.AnimKeyFrameVec3(1, angles),
        ngl.AnimKeyFrameVec3(2, (0, 0, 0)),
    ]
    return _scene(
        cfg,
        ngl.Skew(shape, angles=ngl.AnimatedVec3(anim), axis=(1, 0, 0), anchor=(0, 0.05, -0.5)),
        duration=2.0,
    )


@test_png(width=W, height=H)
@ngl.scene()
def transform_rotate(cfg: ngl.SceneCfg):
    shape = _transform_shape()
    return _scene(cfg, ngl.Rotate(shape, 123.4))


@test_png(width=W, height=H)
@ngl.scene()
def transform_rotate_anchor(cfg: ngl.SceneCfg):
    shape = _transform_shape()
    return _scene(cfg, ngl.Rotate(shape, 123.4, anchor=(80, 100, 0)))


@test_png(width=W, height=H)
@ngl.scene()
def transform_rotate_quat(cfg: ngl.SceneCfg):
    shape = _transform_shape()
    return _scene(cfg, ngl.RotateQuat(shape, (0, 0, -0.474, 0.880)))


@test_png(width=W, height=H)
@ngl.scene()
def transform_rotate_quat_anchor(cfg: ngl.SceneCfg):
    shape = _transform_shape()
    return _scene(cfg, ngl.RotateQuat(shape, (0, 0, -0.474, 0.880), anchor=(80, 100, 0)))


@test_png(width=W, height=H, keyframes=8)
@ngl.scene()
def transform_rotate_quat_animated(cfg: ngl.SceneCfg):
    shape = _transform_shape()
    quat0 = (0, 0, -0.474, 0.880)
    quat1 = (0, 0, 0, 0)
    anim = [
        ngl.AnimKeyFrameQuat(0, quat0),
        ngl.AnimKeyFrameQuat(1, quat1),
        ngl.AnimKeyFrameQuat(2, quat0),
    ]
    return _scene(cfg, ngl.RotateQuat(shape, quat=ngl.AnimatedQuat(anim)), duration=2.0)


@test_png(width=W, height=H, keyframes=15, threshold=2)
@ngl.scene()
def transform_smoothpath(cfg: ngl.SceneCfg):
    shape = _transform_shape()

    # fmt: off
    points = (
        (30, 120, 0.0),
        (80, 40, 0.0),
        (140, 130, 0.0),
        (200, 50, 0.0),
        (270, 100, 0.0),
    )
    controls = (
        (10, 70, 0.0),
        (310, 70, 0.0),
    )
    # fmt: on

    flat_points = (elt for point in points for elt in point)
    points_array = array.array("f", flat_points)

    path = ngl.SmoothPath(
        ngl.BufferVec3(data=points_array),
        control1=controls[0],
        control2=controls[1],
        tension=0.4,
    )

    anim_kf = [
        ngl.AnimKeyFrameFloat(0, 0),
        ngl.AnimKeyFrameFloat(3, 1, "exp_in_out"),
    ]

    return _scene(cfg, ngl.Translate(shape, vector=ngl.AnimatedPath(anim_kf, path)), duration=3.0)


@test_png(width=W, height=H, keyframes=15, threshold=1)
@ngl.scene()
def transform_shared_anim(cfg: ngl.SceneCfg):
    # Two small colored rectangles at different positions
    fill = ngl.ColorFill(color=(0.9, 0.3, 0.5, 1.0))
    size = 64
    shape0 = ngl.DrawRect(rect=(40, 40, size, size), fill=fill)
    shape1 = ngl.DrawRect(rect=(40, 200, size, size), fill=fill)

    # Same animation, reused at different times
    duration = 6.0
    anim_d = duration * 2 / 3
    anim_kf = [
        ngl.AnimKeyFrameVec3(0, (0, 0, 0)),
        ngl.AnimKeyFrameVec3(anim_d / 2, (160, 0, 0), "exp_out"),
        ngl.AnimKeyFrameVec3(anim_d, (0, 0, 0), "back_in"),
    ]
    anim0 = ngl.Translate(shape0, vector=ngl.AnimatedVec3(anim_kf))
    anim1 = ngl.Translate(shape1, vector=ngl.AnimatedVec3(anim_kf, time_offset=duration * 1 / 3))

    return _scene(cfg, ngl.Group(children=[anim0, anim1]), duration=duration)
