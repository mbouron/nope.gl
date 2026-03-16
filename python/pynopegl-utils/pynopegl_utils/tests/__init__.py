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
import os.path as op
import sys

from pynopegl_utils.module import load_script
from pynopegl_utils.tests.refgen import RefGen


def run():
    try:
        refgen = RefGen(os.environ.get("REFGEN", RefGen.NO))
    except ValueError:
        allowed_str = ", ".join(e.value for e in RefGen)
        sys.stderr.write(f"REFGEN environment variable must be any of {allowed_str}\n")
        sys.exit(1)

    if len(sys.argv) not in (3, 4):
        sys.stderr.write(
            "Usage: [REFGEN={}] {} <script_path> <func_name> [<ref_filepath>]\n".format(
                "|".join(e.value for e in RefGen), op.basename(sys.argv[0])
            )
        )
        sys.exit(1)

    if len(sys.argv) == 3:
        script_path, func_name, ref_filepath = sys.argv[1], sys.argv[2], None
    else:
        script_path, func_name, ref_filepath = sys.argv[1:4]
    module = load_script(script_path)
    func = getattr(module, func_name)

    # Ensure PySide/Qt is not imported
    assert not any(k.startswith(("PySide", "Qt")) for k in globals().keys())

    if ref_filepath is None:
        ret = func()
        if ret:
            print(ret)
        sys.exit(0)

    tester = func.tester
    err = tester.run_with_ref(func_name, ref_filepath, refgen)
    if err:
        sys.stderr.write(f"{func_name} failed\n")
        sys.stderr.write("\n".join(err) + "\n")
        sys.exit(1)
    print(f"{func_name} passed")
    sys.exit(0)
