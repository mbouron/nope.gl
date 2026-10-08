#
# Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

import itertools
import math
import random

import pynopegl as ngl
from pynopegl_utils.tests.cmp_floats import test_floats


def _easing_split(easing):
    name_split = easing.split(":")
    easing_name = name_split[0]
    args = [float(x) for x in name_split[1:]] if len(name_split) > 1 else None
    return easing_name, args


def _easing_join(easing, args):
    return easing if not args else easing + ":" + ":".join("%g" % x for x in args)


_easing_specs = (
    # fmt: off
    ("linear",          0),
    ("quadratic",       3),
    ("cubic",           3),
    ("quartic",         3),
    ("quintic",         3),
    ("power:7.3",       3),
    ("sinus",           3),
    ("exp",             3),
    ("circular",        3),
    ("bounce",          1),
    ("elastic",         1),
    ("elastic:1.5:1.2", 1),
    ("back",            3),
    ("bezier_cubic:0.42:0:0.58:1",     0),
    ("bezier_cubic:0.68:-0.6:0.32:1.6", 0),
    # fmt: on
)


def _get_easing_list():
    easings = []
    for easing, flags in _easing_specs:
        versions = []
        if flags & 1:
            versions += ["_in", "_out"]
        if flags & 2:
            versions += ["_in_out", "_out_in"]
        if not flags:
            versions = [""]

        for version in versions:
            base_name, args = _easing_split(easing)
            easing_name = _easing_join(base_name + version, args)
            easings.append(easing_name)
    return easings


_offsets = (None, (0.0, 0.7), (0.3, 1.0), (0.3, 0.7))
_easing_list = _get_easing_list()


def _bezier_cubic_reference(args, x):
    """
    Evaluates the curve at x by bisecting its parameter t, which only relies
    on x(t) being non-decreasing.
    """
    x1, y1, x2, y2 = args
    bezier = lambda t, p1, p2: 3 * (1 - t) ** 2 * t * p1 + 3 * (1 - t) * t**2 * p2 + t**3
    lo, hi = 0.0, 1.0
    for _ in range(100):
        t = (lo + hi) / 2
        if bezier(t, x1, x2) < x:
            lo = t
        else:
            hi = t
    return bezier((lo + hi) / 2, y1, y2)


def _test_bezier_cubic():
    # The CSS keyword curves, an overshooting one and the linear equivalent
    curves = (
        (0.25, 0.1, 0.25, 1.0),
        (0.42, 0.0, 1.0, 1.0),
        (0.0, 0.0, 0.58, 1.0),
        (0.42, 0.0, 0.58, 1.0),
        (0.68, -0.6, 0.32, 1.6),
        (0.0, 0.0, 1.0, 1.0),
    )
    for args in curves:
        for i in range(101):
            x = i / 100
            value = ngl.easing_evaluate("bezier_cubic", x, args)
            assert abs(value - _bezier_cubic_reference(args, x)) < 1e-9, (args, x)

    # Slopes at the ends of the curve where x'(t) vanishes: the limit of
    # y''/x'' where y'(t) vanishes as well, an infinite slope otherwise
    for i in range(1, 100):
        p = i / 100
        for x, args, expected in (
            (0.0, (0.0, 0.0, p, 0.3), 0.3 / p),
            (1.0, (p, 0.3, 1.0, 1.0), (1.0 - 0.3) / (1.0 - p)),
            (0.0, (0.0, 0.3, p, 1.0), math.inf),
            (1.0, (p, 0.0, 1.0, 1.3), -math.inf),
        ):
            value = ngl.easing_derivate("bezier_cubic", x, args)
            assert value == expected or abs(value - expected) < 1e-9, (args, x, value, expected)

    # The curve must be a function of x: x1 and x2 in [0,1], y1 and y2
    # finite, and all 4 control point coordinates given
    nan, inf = float("nan"), float("inf")
    invalid_args = (
        (1.2, 0.0, 0.5, 1.0),
        (0.5, 0.0, -0.1, 1.0),
        (nan, 0.0, 0.5, 1.0),
        (0.5, 0.0, nan, 1.0),
        (0.5, nan, 0.5, 1.0),
        (0.5, 0.0, 0.5, inf),
        (0.42, 0.0, 0.58),
    )
    for args in invalid_args:
        try:
            ngl.easing_evaluate("bezier_cubic", 0.5, args)
        except Exception:
            pass
        else:
            assert False, args


@test_floats()
def anim_forward_api(nb_points=7):
    # We slip the cubic Bézier checks in this function because all the test
    # functions must have a reference in this file.
    _test_bezier_cubic()

    scale = 1.0 / float(nb_points)
    ret = []
    times = [i * scale for i in range(nb_points + 1)]
    for easing in _easing_list:
        easing_name, easing_args = _easing_split(easing)
        for offsets in _offsets:
            values = [ngl.easing_evaluate(easing_name, t, easing_args, offsets) for t in times]
            ret.append((easing_name, values))
    return ret


def _approx_derivative(easing_name, t, easing_args, offsets):
    delta = 1e-10
    t0 = max(0, t - delta)
    t1 = min(1, t + delta)
    y0 = ngl.easing_evaluate(easing_name, t0, easing_args, offsets)
    y1 = ngl.easing_evaluate(easing_name, t1, easing_args, offsets)
    return (y1 - y0) / (t1 - t0)


def _test_derivative_approximations(nb_points=20):
    """
    This test approximates the derivatives numerically by using 2 very close
    points around a given point. This is useful to test the exactitude of the
    derivative function implementation.
    """
    scale = 1.0 / float(nb_points)

    times = [i * scale for i in range(nb_points + 1)]
    max_err = 0.0005

    for easing in _easing_list:
        easing_name, easing_args = _easing_split(easing)
        for offsets in _offsets:
            out_vals = [ngl.easing_derivate(easing_name, t, easing_args, offsets) for t in times]
            approx_vals = [_approx_derivative(easing_name, t, easing_args, offsets) for t in times]
            errors = [abs(a - b) for a, b in zip(out_vals, approx_vals)]

            # Special exceptions for a few easings due to infinite derivatives
            # (vertical slopes) for which the approximation can not come close
            # enough.
            if easing_name == "circular_in_out":
                errors[nb_points // 2] = -1
            elif easing_name == "circular_out_in":
                errors[0] = errors[-1] = -1
            elif easing_name in {"circular_in", "bounce_out", "elastic_out"}:
                errors[-1] = -1
            elif easing_name in {"circular_out", "bounce_in", "elastic_in"}:
                errors[0] = -1

            if max(errors) > max_err:
                print(f"{easing=}")
                for t, out, approx, error in zip(times, out_vals, approx_vals, errors):
                    prefix = "!" if error > max_err else " "
                    error = "ignored" if error < 0 else error
                    print(f" {prefix} {t=} {out=} {approx=} {error=}")
                assert False


@test_floats()
def anim_derivative_api(nb_points=7):
    # We slip the approximation test in this function because all the test
    # functions must have a reference in this file.
    _test_derivative_approximations()

    scale = 1.0 / float(nb_points)
    ret = []
    times = [i * scale for i in range(nb_points + 1)]
    for easing in _easing_list:
        easing_name, easing_args = _easing_split(easing)
        for offsets in _offsets:
            values = [ngl.easing_derivate(easing_name, t, easing_args, offsets) for t in times]
            ret.append((easing_name, values))
    return ret


@test_floats()
def anim_resolution_api(nb_points=7):
    scale = 1.0 / float(nb_points)
    ret = []
    times = [i * scale for i in range(nb_points + 1)]
    for easing in _easing_list:
        easing_name, easing_args = _easing_split(easing)
        for offsets in _offsets:
            try:
                values = [ngl.easing_solve(easing_name, t, easing_args, offsets) for t in times]
            except Exception:
                pass
            else:
                ret.append((easing_name, values))
    return ret


def _get_anim_func(size, animated_type, kf_func, velocity_type=None):
    @test_floats()
    def test_func():
        offsets = ((None, None), (None, 0.7), (0.3, None), (0.3, 0.7))
        nb_kf = len(_easing_specs) + 1
        nb_queries = nb_kf - 1
        scale = 1.0 / float(nb_kf)
        rng = random.Random(0)
        kfvalues = [[rng.uniform(0, 1) for _ in range(size)] for _ in range(nb_kf + 1)]

        ret = []
        for i, (easing_start_offset, easing_end_offset) in enumerate(offsets):
            anim_kf = [kf_func(0, kfvalues[0])]
            for j in range(nb_kf):
                t = (j + 1) * scale
                v = kfvalues[j + 1]
                easing_name, easing_args = _easing_split(_easing_list[j])
                anim_kf.append(
                    kf_func(
                        t,
                        v,
                        easing=easing_name,
                        easing_args=easing_args,
                        easing_start_offset=easing_start_offset,
                        easing_end_offset=easing_end_offset,
                    )
                )
            anim = animated_type(anim_kf)
            if velocity_type is not None:
                anim = velocity_type(anim)

            # Query between times
            values = [anim.evaluate((t_id + 1) * scale) for t_id in range(nb_queries)]

            # Query boundaries and out of them (to trigger a copy instead of a mix)
            values += [anim.evaluate(0)]
            values += [anim.evaluate(1)]
            values += [anim.evaluate(5)]

            if hasattr(values[0], "__iter__"):
                values = list(itertools.chain(*values))
            ret.append(("off%d" % i, values))

        return ret

    return test_func


_float_kf_func = lambda t, v, **kw: ngl.AnimKeyFrameFloat(t, v[0], **kw)
_vec2_kf_func = lambda t, v, **kw: ngl.AnimKeyFrameVec2(t, v, **kw)
_vec3_kf_func = lambda t, v, **kw: ngl.AnimKeyFrameVec3(t, v, **kw)
_vec4_kf_func = lambda t, v, **kw: ngl.AnimKeyFrameVec4(t, v, **kw)
_quat_kf_func = lambda t, v, **kw: ngl.AnimKeyFrameQuat(t, v, **kw)

anim_forward_float = _get_anim_func(1, ngl.AnimatedFloat, _float_kf_func)
anim_forward_vec2 = _get_anim_func(2, ngl.AnimatedVec2, _vec2_kf_func)
anim_forward_vec3 = _get_anim_func(3, ngl.AnimatedVec3, _vec3_kf_func)
anim_forward_vec4 = _get_anim_func(4, ngl.AnimatedVec4, _vec4_kf_func)
anim_forward_quat = _get_anim_func(4, ngl.AnimatedQuat, _quat_kf_func)

anim_velocity_float = _get_anim_func(1, ngl.AnimatedFloat, _float_kf_func, velocity_type=ngl.VelocityFloat)
anim_velocity_vec2 = _get_anim_func(2, ngl.AnimatedVec2, _vec2_kf_func, velocity_type=ngl.VelocityVec2)
anim_velocity_vec3 = _get_anim_func(3, ngl.AnimatedVec3, _vec3_kf_func, velocity_type=ngl.VelocityVec3)
anim_velocity_vec4 = _get_anim_func(4, ngl.AnimatedVec4, _vec4_kf_func, velocity_type=ngl.VelocityVec4)
