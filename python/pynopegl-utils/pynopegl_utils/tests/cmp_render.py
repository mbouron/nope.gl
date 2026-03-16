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

import os
import os.path as op
import sys
from typing import List, Optional

from PIL import Image
from pynopegl_utils.tests.cmp import CompareSceneBase, get_test_decorator
from pynopegl_utils.tests.pixelmatch import pixelmatch
from pynopegl_utils.tests.refgen import RefGen
from pynopegl_utils.tests.report import generate_report, get_report_dir, save_test_result

_REF = ".ref"
_MODE = "RGBA"


def _ref_path(base: str, index: int, count: int) -> str:
    base = base[: -len(_REF)] if base.endswith(_REF) else base
    if count == 1:
        return base + ".png"
    return f"{base}_{index}.png"


class _CompareRender(CompareSceneBase):
    def __init__(self, scene_func, tolerance: int = 1, diff_threshold: float = 0.0, **kwargs):
        super().__init__(scene_func, **kwargs)
        self._tolerance = tolerance
        self._diff_threshold = diff_threshold

    @staticmethod
    def serialize(data):
        return data

    @staticmethod
    def deserialize(data):
        return data

    def _compare_data(self, test_name: str, ref_paths: List[str], out_images: List[Image.Image]) -> List[str]:
        report_dir = get_report_dir()
        err = []

        for i, (ref_path, out_img) in enumerate(zip(ref_paths, out_images)):
            ref_img = Image.open(ref_path).convert(_MODE)
            result = pixelmatch(ref_img, out_img, tolerance=self._tolerance)
            pixel_count = ref_img.size[0] * ref_img.size[1]

            diff_pct = result.diff_count / pixel_count
            max_allowed = max(self._diff_threshold * pixel_count, 0)
            failed = result.diff_count > max_allowed
            stats = dict(
                status="fail" if failed else "pass",
                diff_count=result.diff_count,
                aa_count=result.aa_count,
                max_diff=result.max_diff,
                tolerance=self._tolerance,
                diff_threshold=self._diff_threshold,
                total_pixels=pixel_count,
            )

            # Save diff image only when there are actual differences or AA pixels
            diff_img = result.diff_image if (result.max_diff > self._tolerance) else None
            save_test_result(report_dir, test_name, i, ref_paths[i], out_img, diff_img, stats)

            if failed:
                err.append(
                    f"{test_name} frame #{i}:\n"
                    f"  diff: {100.0 * diff_pct:.3f}% / {self._diff_threshold * 100:.3f}% ({result.diff_count}/{pixel_count})\n"
                    f"  max channel diff: {result.max_diff} / {self._tolerance}\n"
                    f"  aa (ignored): {result.aa_count} / {pixel_count}"
                )

        report_path = generate_report(report_dir)
        if report_path:
            anchor = f"{test_name}_0"
            print(f"Report: file://{report_path}#{anchor}")

        return err

    def _get_out_images(self) -> List[Image.Image]:
        images = []
        for width, height, capture_buffer in self.render_frames():
            img = Image.frombytes(_MODE, (width, height), bytes(capture_buffer), "raw", _MODE, 0, 1)
            images.append(img)
        return images

    def run_with_ref(self, func_name: str, ref_base: str, ref_gen: RefGen) -> Optional[List[str]]:
        out_images = self._get_out_images()
        out_image_count = len(out_images)
        ref_paths = [_ref_path(ref_base, i, out_image_count) for i in range(out_image_count)]
        missing_ref_paths = [p for p in ref_paths if not op.exists(p)]

        def save_refs():
            ref_dir = op.dirname(op.abspath(ref_paths[0]))
            os.makedirs(ref_dir, exist_ok=True)
            for img, path in zip(out_images, ref_paths):
                action = "re-generating" if op.exists(path) else "creating"
                sys.stderr.write(f"{func_name}: {action} {path}\n")
                img.save(path)

        def compare():
            return self._compare_data(func_name, ref_paths, out_images)

        if ref_gen == RefGen.FORCE:
            save_refs()
            return []

        if ref_gen == RefGen.CREATE:
            if missing_ref_paths:
                save_refs()
                return None
            return compare()

        if ref_gen == RefGen.UPDATE:
            if missing_ref_paths:
                save_refs()
                return None
            err = compare()
            if err:
                save_refs()
            return None

        if missing_ref_paths:
            sys.stderr.write(
                f"{func_name}: reference file(s) not found: {', '.join(missing_ref_paths)}\n"
                f"use REFGEN={RefGen.CREATE} to create them\n"
            )
            report_dir = get_report_dir()
            for i, img in enumerate(out_images):
                stats = dict(status="fail", missing_ref=True)
                save_test_result(report_dir, func_name, i, None, img, None, stats)
            report_path = generate_report(report_dir)
            if report_path:
                anchor = f"{func_name}_0"
                print(f"Report: file://{report_path}#{anchor}")
            return [f"{func_name}: reference file(s) missing: {', '.join(missing_ref_paths)}"]
        return compare()


test_render = get_test_decorator(_CompareRender)
