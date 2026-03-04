#
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

import os
import os.path as op
import sys
from typing import List, Optional

from PIL import Image, ImageChops

from .cmp import CompareBase, CompareSceneBase, get_test_decorator

_MODE = "RGBA"


def _ref_path(base: str, index: int, total: int) -> str:
    """Single frame: {base}.png; multiple frames: {base}_{N}.png"""
    if total == 1:
        return base + ".png"
    return f"{base}_{index}.png"


class _ComparePng(CompareSceneBase):
    def __init__(self, scene_func, threshold: int = 1, **kwargs):
        super().__init__(scene_func, **kwargs)
        self._threshold = threshold

    def get_out_data(self, dump: bool = False, func_name: Optional[str] = None) -> List[Image.Image]:
        images = []
        dump_index = 0
        for width, height, capture_buffer in self.render_frames():
            img = Image.frombytes(_MODE, (width, height), bytes(capture_buffer), "raw", _MODE, 0, 1)
            if dump:
                CompareBase.dump_image(img, dump_index, func_name)
                dump_index += 1
            images.append(img)
        return images

    @staticmethod
    def serialize(data):
        return data

    @staticmethod
    def deserialize(data):
        return data

    def compare_data(self, test_name: str, ref_data, out_data) -> List[str]:
        err = []
        for i, (ref_img, out_img) in enumerate(zip(ref_data, out_data)):
            diff = ImageChops.difference(ref_img, out_img)
            extrema = diff.getextrema()
            max_diff = max(ch[1] for ch in extrema)
            if max_diff > self._threshold:
                err.append(
                    f"{test_name} frame #{i}: max pixel diff {max_diff} > threshold {self._threshold}"
                )
        return err

    def run_with_ref(self, func_name: str, ref_base: str, dump: bool, refgen_opt: str) -> Optional[List[str]]:
        """
        Handle the full PNG test flow.

        ref_base: path prefix without extension.
          - single frame  → {ref_base}.png
          - multiple frames → {ref_base}_0.png, {ref_base}_1.png, …

        Returns a list of error strings (empty = pass) or None when only generating refs.
        """
        out_images = self.get_out_data(dump=dump, func_name=func_name)
        total = len(out_images)
        ref_paths = [_ref_path(ref_base, i, total) for i in range(total)]
        all_exist = all(op.exists(p) for p in ref_paths)

        def save_refs():
            ref_dir = op.dirname(op.abspath(ref_paths[0]))
            os.makedirs(ref_dir, exist_ok=True)
            for img, path in zip(out_images, ref_paths):
                action = "re-generating" if op.exists(path) else "creating"
                sys.stderr.write(f"{func_name}: {action} {path}\n")
                img.save(path)

        def compare():
            ref_images = [Image.open(p).convert(_MODE) for p in ref_paths]
            return self.compare_data(func_name, ref_images, out_images)

        if refgen_opt == "force":
            save_refs()
            return []

        if refgen_opt == "create":
            if not all_exist:
                save_refs()
                return None
            return compare()

        if refgen_opt == "update":
            if not all_exist:
                save_refs()
                return None
            err = compare()
            if err:
                save_refs()
            return None

        # refgen_opt == "no"
        if not all_exist:
            missing = [p for p in ref_paths if not op.exists(p)]
            sys.stderr.write(
                f"{func_name}: reference file(s) not found: {', '.join(missing)}\n"
                "use REFGEN=create to create them\n"
            )
            sys.exit(1)
        return compare()


test_png = get_test_decorator(_ComparePng)
