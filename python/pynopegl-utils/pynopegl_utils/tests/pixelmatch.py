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
Anti-aliasing aware image comparison.

Based on the pixelmatch algorithm by Mapbox (ISC license).
Reference: https://github.com/mapbox/pixelmatch

Uses the "Anti-aliased Pixel and Intensity Slope Detector" paper
by V. Vysniauskas (2009) and YIQ color distance from "Measuring
perceived color difference using YIQ NTSC transmission color space
in mobile applications" by Y. Kotsarenko and F. Ramos.
"""

from collections import namedtuple

import numpy as np
from PIL import Image

PixelmatchResult = namedtuple("PixelmatchResult", ["diff_count", "aa_count", "max_diff", "diff_image"])

_COLOR_AA = np.array([255, 255, 0, 255], dtype=np.uint8)
_COLOR_DIFF = np.array([255, 0, 0, 255], dtype=np.uint8)

# 8 neighbor offsets: (dx, dy) — column-major order to match the JS reference
_NEIGHBOR_DX = np.array([-1, -1, -1, 0, 0, 1, 1, 1], dtype=np.int32)
_NEIGHBOR_DY = np.array([-1, 0, 1, -1, 1, -1, 0, 1], dtype=np.int32)

_Y_R = 0.29889531
_Y_G = 0.58662247
_Y_B = 0.11448223
_PHI = 1.618033988749895
_PHI_PLUS_1 = 2.618033988749895


def _check_siblings(img_arr, px, py, w, h):
    """
    For each pixel in (px, py), check if it has 3+ adjacent pixels of the
    same color (with edge adjustment).
    """
    n = len(px)
    center = img_arr[py, px]  # (n, 4)
    on_edge = (px == 0) | (px == w - 1) | (py == 0) | (py == h - 1)
    match_count = on_edge.astype(np.int32)

    for d in range(8):
        nx = px + int(_NEIGHBOR_DX[d])
        ny = py + int(_NEIGHBOR_DY[d])
        valid = (nx >= 0) & (nx < w) & (ny >= 0) & (ny < h)
        nx_safe = np.clip(nx, 0, w - 1)
        ny_safe = np.clip(ny, 0, h - 1)
        nb = img_arr[ny_safe, nx_safe]  # (n, 4)
        matches = np.all(nb == center, axis=1) & valid
        match_count += matches.astype(np.int32)

    return match_count > 2


def _is_antialiased_batch(img_arr, other_arr, cx, cy, w, h):
    """
    Check if candidate pixels are anti-aliased, processing all candidates at once.

    Examines the 8 neighbors of each candidate in img_arr for a brightness
    gradient pattern consistent with AA edges, then verifies that the
    darkest/brightest neighbor has 3+ identical siblings in both images.
    """
    n = len(cx)
    if n == 0:
        return np.array([], dtype=bool)

    # Precompute center pixel values (float64 for precision)
    center = img_arr[cy, cx].astype(np.float64)  # (n, 4)
    r1 = center[:, 0]
    g1 = center[:, 1]
    b1 = center[:, 2]
    a1 = center[:, 3]

    # Precompute position-dependent background colors for alpha blending.
    # k is the byte offset of the center pixel in raw RGBA data.
    k = ((cy * w + cx) * 4).astype(np.int64)
    # k % 2 is always 0 (since k is a multiple of 4), so bg_r = 48 always
    bg_r = np.float64(48)
    bg_g = 48.0 + 159.0 * ((k / _PHI).astype(np.int64) % 2)
    bg_b = 48.0 + 159.0 * ((k / _PHI_PLUS_1).astype(np.int64) % 2)

    # Edge penalty: pixels on any image border start with 1 extra zero
    on_edge = (cx == 0) | (cx == w - 1) | (cy == 0) | (cy == h - 1)

    zero_count = on_edge.astype(np.int32)
    min_deltas = np.zeros(n, dtype=np.float64)
    max_deltas = np.zeros(n, dtype=np.float64)
    min_dir = np.zeros(n, dtype=np.int32)
    max_dir = np.zeros(n, dtype=np.int32)

    for d in range(8):
        nx = cx + int(_NEIGHBOR_DX[d])
        ny = cy + int(_NEIGHBOR_DY[d])
        valid = (nx >= 0) & (nx < w) & (ny >= 0) & (ny < h)
        nx_safe = np.clip(nx, 0, w - 1)
        ny_safe = np.clip(ny, 0, h - 1)

        # Neighbor pixel values
        nb = img_arr[ny_safe, nx_safe].astype(np.float64)  # (n, 4)
        r2, g2, b2, a2 = nb[:, 0], nb[:, 1], nb[:, 2], nb[:, 3]

        dr = r1 - r2
        dg = g1 - g2
        db = b1 - b2
        da = a1 - a2

        all_zero = (dr == 0) & (dg == 0) & (db == 0) & (da == 0)
        needs_blend = (a1 < 255) | (a2 < 255)

        # Blended differences (for semi-transparent pixels)
        dr_b = (r1 * a1 - r2 * a2 - bg_r * da) / 255.0
        dg_b = (g1 * a1 - g2 * a2 - bg_g * da) / 255.0
        db_b = (b1 * a1 - b2 * a2 - bg_b * da) / 255.0

        dr_f = np.where(needs_blend, dr_b, dr)
        dg_f = np.where(needs_blend, dg_b, dg)
        db_f = np.where(needs_blend, db_b, db)

        delta = dr_f * _Y_R + dg_f * _Y_G + db_f * _Y_B
        delta = np.where(all_zero, 0.0, delta)

        # Count zero-delta valid neighbors
        is_zero = (delta == 0.0) & valid
        zero_count += is_zero.astype(np.int32)

        # Track most negative delta (min) and most positive delta (max)
        neg_mask = valid & (delta < min_deltas)
        min_deltas = np.where(neg_mask, delta, min_deltas)
        min_dir = np.where(neg_mask, d, min_dir)

        pos_mask = valid & (delta > max_deltas)
        max_deltas = np.where(pos_mask, delta, max_deltas)
        max_dir = np.where(pos_mask, d, max_dir)

    # Must have both a negative and positive gradient, and few zero neighbors
    has_gradient = (min_deltas < 0) & (max_deltas > 0)
    not_flat = zero_count <= 2

    # Get the coordinates of the darkest/brightest neighbors
    min_nx = np.clip(cx + _NEIGHBOR_DX[min_dir], 0, w - 1)
    min_ny = np.clip(cy + _NEIGHBOR_DY[min_dir], 0, h - 1)
    max_nx = np.clip(cx + _NEIGHBOR_DX[max_dir], 0, w - 1)
    max_ny = np.clip(cy + _NEIGHBOR_DY[max_dir], 0, h - 1)

    # Check if the darkest or brightest neighbor has many identical siblings
    # in both images
    min_sib = _check_siblings(img_arr, min_nx, min_ny, w, h) & _check_siblings(other_arr, min_nx, min_ny, w, h)
    max_sib = _check_siblings(img_arr, max_nx, max_ny, w, h) & _check_siblings(other_arr, max_nx, max_ny, w, h)

    return not_flat & has_gradient & (min_sib | max_sib)


def pixelmatch(img1, img2, tolerance=1):
    """
    Compare two RGBA images with anti-aliasing detection.

    Pixels that differ by more than `tolerance` (per-channel, 0-255) are
    checked for anti-aliasing. AA pixels are forgiven; only real differences
    are counted as failures.

    Returns a PixelmatchResult with:
        diff_count: number of truly different (non-AA) pixels
        aa_count:   number of anti-aliased pixels (forgiven)
        max_diff:   maximum per-channel difference across all pixels
        diff_image: colored diff (red=real diff, yellow=AA, original diff elsewhere)
    """
    assert img1.size == img2.size
    assert img1.mode == "RGBA" and img2.mode == "RGBA"

    w, h = img1.size

    a1 = np.array(img1, dtype=np.uint8)  # (h, w, 4)
    a2 = np.array(img2, dtype=np.uint8)  # (h, w, 4)

    # Per-channel absolute difference
    diff = np.abs(a1.astype(np.int16) - a2.astype(np.int16))  # (h, w, 4)
    max_per_pixel = diff.max(axis=2)  # (h, w)
    max_diff = int(max_per_pixel.max())
    diff_uint8 = diff.astype(np.uint8)

    if max_diff <= tolerance:
        return PixelmatchResult(0, 0, max_diff, Image.fromarray(diff_uint8, "RGBA"))

    # Find candidate pixels exceeding tolerance
    cy, cx = np.where(max_per_pixel > tolerance)

    # Check anti-aliasing in both directions
    aa1 = _is_antialiased_batch(a1, a2, cx, cy, w, h)
    aa2 = _is_antialiased_batch(a2, a1, cx, cy, w, h)
    is_aa = aa1 | aa2

    # Build output diff image
    out = diff_uint8.copy()
    out[cy[is_aa], cx[is_aa]] = _COLOR_AA
    out[cy[~is_aa], cx[~is_aa]] = _COLOR_DIFF

    diff_count = int((~is_aa).sum())
    aa_count = int(is_aa.sum())

    return PixelmatchResult(diff_count, aa_count, max_diff, Image.fromarray(out, "RGBA"))
