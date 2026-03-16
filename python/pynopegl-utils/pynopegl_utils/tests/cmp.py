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

import os
from fractions import Fraction
from typing import Any, Callable, Generator, List, Optional, Sequence, Tuple, Union

import pynopegl as ngl
from pynopegl_utils.misc import get_backend, get_nopegl_tempdir
from pynopegl_utils.tests.refgen import RefGen


class CompareBase:
    @staticmethod
    def serialize(data: Any) -> str:
        return data

    @staticmethod
    def deserialize(data: str) -> Any:
        return data

    def run_with_ref(self, func_name: str, ref_base: str, ref_gen: RefGen) -> Optional[List[str]]:
        raise NotImplementedError


class CompareSceneBase(CompareBase):
    def __init__(
        self,
        scene_func: Callable[..., ngl.SceneInfo],
        width: int,
        height: int,
        keyframes: Union[int, Sequence[float]] = 1,  # either a number of keyframes or a sequence of absolute times
        keyframes_callback=None,
        clear_color: Tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0),
        exercise_serialization: bool = True,
        exercise_dot: bool = True,
        samples: int = 0,
        **scene_kwargs,
    ):
        self._width = width
        self._height = height
        self._keyframes = keyframes
        self._keyframes_callback = keyframes_callback
        self._clear_color = clear_color
        self._scene_func = scene_func
        self._scene_kwargs = scene_kwargs
        self._exercise_serialization = exercise_serialization
        self._exercise_dot = exercise_dot
        self._samples = samples
        self._hud = False
        self._hud_export_filename = None

    def render_frames(self) -> Generator[Tuple[int, int, bytearray], None, None]:
        backend = os.environ.get("BACKEND")

        width, height = self._width, self._height
        capture_buffer = bytearray(width * height * 4)
        ctx_cfg = ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=get_backend(backend) if backend else ngl.Backend.AUTO,
            samples=self._samples,
            clear_color=self._clear_color,
            capture_buffer=capture_buffer,
            hud=self._hud,
            hud_export_filename=self._hud_export_filename,
        )
        ctx = ngl.Context()
        ret = ctx.configure(ctx_cfg)
        assert ret == 0

        backend = ctx.get_backend()
        cfg = ngl.SceneCfg(samples=self._samples, clear_color=self._clear_color, backend=backend["id"])
        scene_info = self._scene_func(cfg, **self._scene_kwargs)
        scene = scene_info.scene
        duration = scene.duration

        aspect_ratio = scene.aspect_ratio
        if aspect_ratio[0] > 0 and aspect_ratio[1] > 1:
            assert Fraction(*aspect_ratio) == Fraction(width, height)

        if isinstance(self._keyframes, int):
            timescale = duration / self._keyframes
            keyframes = [t_id * timescale for t_id in range(self._keyframes)]
        else:
            keyframes = self._keyframes
        assert all(t <= duration for t in keyframes)

        if self._exercise_dot:
            assert scene.dot()

        if self._exercise_serialization:
            scene = ngl.Scene.from_string(scene.serialize())

        assert ctx.set_scene(scene) == 0

        for t_id, t in enumerate(keyframes):
            if self._keyframes_callback:
                self._keyframes_callback(t_id)
            ctx.draw(t)

            yield (width, height, capture_buffer)

            if not self._exercise_serialization and self._exercise_dot:
                scene.dot()


def get_test_decorator(cls):
    def test_func(*args, **kwargs):
        def test_decorator(user_func):
            # Inject a tester for ngl-test
            user_func.tester = cls(user_func, *args, **kwargs)
            return user_func

        return test_decorator

    return test_func
