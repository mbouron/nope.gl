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

"""
HTML report generation for render test results.

Generates an interactive HTML report with side-by-side reference/output
images and mouseover comparison, similar to Blender's render test reports.
"""

import glob as glob_mod
import json
import os
import os.path as op
import tempfile

from jinja2 import Environment, FileSystemLoader
from pynopegl_utils.misc import get_nopegl_tempdir

_TEMPLATE_DIR = op.dirname(__file__)


def get_report_dir():
    backend = os.environ.get("BACKEND", "unknown")
    base = os.environ.get("REPORT_DIR", op.join(get_nopegl_tempdir(), "report"))
    report_dir = op.join(base, backend)
    os.makedirs(report_dir, exist_ok=True)
    return report_dir


def _atomic_write_text(path, content):
    dir_path = op.dirname(path)
    fd, tmp_path = tempfile.mkstemp(dir=dir_path, suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(content)
        os.replace(tmp_path, path)
    except BaseException:
        os.unlink(tmp_path)
        raise


def save_test_result(report_dir, test_name, frame, ref_img, out_img, diff_img, stats, ref_path=None):
    """Save images and result metadata for one test frame."""
    data_dir = op.join(report_dir, "data")
    os.makedirs(data_dir, exist_ok=True)

    key = f"{test_name}_{frame}"
    has_diff = stats["status"] == "fail" or diff_img is not None

    # Store the absolute path to the on-disk ref file so the report can link to it
    ref_abs = op.abspath(ref_path) if ref_path else None

    if has_diff:
        img_dir = op.join(report_dir, "images")
        os.makedirs(img_dir, exist_ok=True)
        save_opts = dict(compress_level=1)
        ref_img.save(op.join(img_dir, f"{key}_ref.png"), **save_opts)
        out_img.save(op.join(img_dir, f"{key}_out.png"), **save_opts)
        if diff_img is not None:
            diff_img.save(op.join(img_dir, f"{key}_diff.png"), **save_opts)

    result = {"key": key, "test_name": test_name, "frame": frame, "has_images": has_diff, **stats}
    if ref_abs:
        result["ref_path"] = ref_abs
    _atomic_write_text(op.join(data_dir, f"{key}.json"), json.dumps(result))


def _build_stats_html(r):
    parts = []
    max_diff = r.get("max_diff", 0)
    diff_count = r.get("diff_count", 0)
    aa_count = r.get("aa_count", 0)
    tolerance = r.get("tolerance", 0)
    diff_threshold = r.get("diff_threshold", 0)
    total_pixels = r.get("total_pixels", 0)
    if total_pixels > 0:
        diff_pct = 100.0 * diff_count / total_pixels
        threshold_pct = 100.0 * diff_threshold
        parts.append(f"diff: {diff_pct:.3f}% / {threshold_pct:.3f}%")
    if max_diff > 0:
        parts.append(f"max diff: {max_diff}")
    if diff_count > 0:
        parts.append(f"<b>different: {diff_count}</b>")
    if aa_count > 0:
        parts.append(f"aa (ignored): {aa_count}")
    if tolerance > 0:
        parts.append(f"tolerance: {tolerance}")
    return "<br>".join(parts) if parts else "identical"


def _prepare_result(r, report_dir):
    key = r["key"]
    has_images = r.get("has_images", False)
    ref_path = r.get("ref_path", "")

    entry = dict(
        key=key,
        name=r["test_name"],
        frame=r.get("frame", 0),
        failed=r["status"] == "fail",
        has_images=has_images,
        ref_path=ref_path,
        stats_html=_build_stats_html(r),
    )

    if has_images:
        entry["out_url"] = f"images/{key}_out.png"
        entry["ref_url"] = f"images/{key}_ref.png"
        entry["diff_url"] = f"images/{key}_diff.png"
        entry["has_diff"] = op.exists(op.join(report_dir, entry["diff_url"]))

    return entry


def generate_report(report_dir):
    """Generate HTML report from all saved test results. Returns the report path."""
    data_dir = op.join(report_dir, "data")
    if not op.exists(data_dir):
        return None

    raw_results = []
    for path in sorted(glob_mod.glob(op.join(data_dir, "*.json"))):
        with open(path) as f:
            raw_results.append(json.load(f))

    # Failures first, then alphabetical
    raw_results.sort(key=lambda r: (r["status"] == "pass", r["test_name"], r["frame"]))

    backend = os.environ.get("BACKEND", "unknown")
    n_fail = sum(1 for r in raw_results if r["status"] == "fail")
    n_total = len(raw_results)

    results = [_prepare_result(r, report_dir) for r in raw_results]

    env = Environment(loader=FileSystemLoader(_TEMPLATE_DIR), autoescape=False)
    template = env.get_template("report.html.j2")
    html = template.render(
        backend=backend,
        n_pass=n_total - n_fail,
        n_fail=n_fail,
        n_total=n_total,
        results=results,
    )

    report_path = op.join(report_dir, "report.html")
    _atomic_write_text(report_path, html)
    return report_path
