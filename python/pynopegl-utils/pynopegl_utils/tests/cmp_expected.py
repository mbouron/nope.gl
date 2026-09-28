#
# Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

import sys
from typing import Callable, List, Optional, Sequence, Tuple

from pynopegl_utils.tests.cmp import CompareSceneBase, get_test_decorator
from pynopegl_utils.tests.cmp_floats import _CompareFloats

# Expected RGBA (0-255) of the pixel (x, y) of a frame, with (0, 0) the top-left
# pixel, or None when the pixel is not checked
ExpectedFunc = Callable[[int, int, int], Optional[Tuple[int, int, int, int]]]


class _CompareExpected(CompareSceneBase):
    """
    Compare the rendered frames with pixels computed independently of the
    renderer, instead of with a reference image generated from its output.

    The reference only holds, per frame, the largest channel error (0 when
    every checked pixel matches) and the number of pixels checked, so the test
    also fails if a change leaves nothing to check.
    """

    def __init__(self, scene_func, expected: ExpectedFunc, tolerance: int = 1, **kwargs):
        super().__init__(scene_func, **kwargs)
        self._expected = expected
        self._tolerance = tolerance

    serialize = staticmethod(_CompareFloats.serialize)
    deserialize = staticmethod(_CompareFloats.deserialize)

    def _get_out_data(self) -> List[Tuple[str, List[float]]]:
        data = []
        for frame_id, (width, height, buf) in enumerate(self.render_frames()):
            max_err = 0
            checked = 0
            mismatches = []
            for y in range(height):
                for x in range(width):
                    expected = self._expected(frame_id, x, y)
                    if expected is None:
                        continue
                    checked += 1
                    offset = (y * width + x) * 4
                    out = tuple(buf[offset : offset + 4])
                    err = max(abs(a - b) for a, b in zip(out, expected))
                    if err > self._tolerance and len(mismatches) < 8:
                        mismatches.append(f"  ({x},{y}): got {out}, expected {tuple(expected)}")
                    max_err = max(max_err, err)
            if mismatches:
                sys.stderr.write(f"frame #{frame_id} mismatches:\n" + "\n".join(mismatches) + "\n")
            # Errors within the tolerance are reported as exact matches so
            # that the reference does not depend on the backend rounding
            data.append((f"frame{frame_id}", [float(max_err if max_err > self._tolerance else 0), float(checked)]))
        return data

    def _compare_data(self, test_name: str, ref_data, out_data) -> Sequence[str]:
        return _CompareFloats(None, tolerance=0.0)._compare_data(test_name, ref_data, out_data)

    run_with_ref = _CompareFloats.run_with_ref
    _set_ref_data = _CompareFloats._set_ref_data
    _get_ref_data = _CompareFloats._get_ref_data
    _run_test = _CompareFloats._run_test


test_expected = get_test_decorator(_CompareExpected)
