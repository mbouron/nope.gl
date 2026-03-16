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

import os.path as op
import sys
from typing import List, Optional, Sequence

from pynopegl_utils.tests.cmp import CompareBase, get_test_decorator
from pynopegl_utils.tests.refgen import RefGen


class _CompareFloats(CompareBase):
    def __init__(self, func, tolerance: float = 0.001, **func_kwargs):
        self._func = func
        self._func_kwargs = func_kwargs
        self._tolerance = tolerance

    @staticmethod
    def serialize(data) -> str:
        ret = ""
        for name, floats in data:
            ret += "{}: {}\n".format(name, " ".join("%f" % f for f in floats))
        return ret

    @staticmethod
    def deserialize(data: str):
        ret = []
        for line in data.splitlines():
            name, floats = line.split(":", 1)
            ret.append((name, [float(f) for f in floats.split()]))
        return ret

    def _get_out_data(self):
        return self._func(**self._func_kwargs)

    def _compare_data(self, test_name: str, ref_data, out_data) -> Sequence[str]:
        err = []
        for float_set_id, (ref, out) in enumerate(zip(ref_data, out_data)):
            ref_name, ref_floats = ref
            out_name, out_floats = out
            if ref_name != out_name:
                err.append(
                    f"{test_name} float sets {float_set_id} have different names: " f'"{ref_name}" vs "{out_name}"'
                )
                break
            if len(ref_floats) != len(out_floats):
                err.append(
                    f"{test_name}: number of floats is different " f"(ref:{len(ref_floats)}, out:{len(out_floats)})"
                )
                break
            for i, (f0, f1) in enumerate(zip(ref_floats, out_floats)):
                diff = abs(f0 - f1)
                if diff > self._tolerance:
                    err.append(
                        f"{test_name} diff too large for float {ref_name}[{i}]: "
                        f"|{f0}-{f1}|={diff} (>{self._tolerance})"
                    )
        return err

    def _set_ref_data(self, ref_filepath, data):
        with open(ref_filepath, "w") as ref_file:
            ref_file.write(self.serialize(data))

    def _get_ref_data(self, ref_filepath):
        with open(ref_filepath) as ref_file:
            serialized_data = ref_file.read()
        return self.deserialize(serialized_data)

    def _run_test(self, func_name, ref_data, out_data):
        err = []
        if len(ref_data) != len(out_data):
            err = [f"{func_name}: data len mismatch (ref:{len(ref_data)} out:{len(out_data)})"]
        err += self._compare_data(func_name, ref_data, out_data)
        return err

    def run_with_ref(self, func_name: str, ref_base: str, ref_gen: RefGen) -> Optional[List[str]]:
        ref_filepath = ref_base
        out_data = self._get_out_data()
        ref_exists = op.exists(ref_filepath)

        def save_ref():
            action = "re-generating" if ref_exists else "creating"
            sys.stderr.write(f"{func_name}: {action} {ref_filepath}\n")
            self._set_ref_data(ref_filepath, out_data)

        def compare():
            ref_data = self._get_ref_data(ref_filepath)
            return self._run_test(func_name, ref_data, out_data)

        if ref_gen == RefGen.FORCE:
            save_ref()
            return []

        if ref_gen == RefGen.CREATE:
            if not ref_exists:
                save_ref()
                return None
            return compare()

        if ref_gen == RefGen.UPDATE:
            if not ref_exists:
                save_ref()
                return None
            err = compare()
            if err:
                save_ref()
            return None

        # ref_gen == RefGen.NO
        if not ref_exists:
            sys.stderr.write(
                f"{func_name}: reference file {ref_filepath} not found, use REFGEN={RefGen.CREATE} to create it\n"
            )
            sys.exit(1)
        return compare()


test_floats = get_test_decorator(_CompareFloats)
