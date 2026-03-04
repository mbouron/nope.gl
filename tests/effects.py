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
Effect shader tests using DrawRect + CustomFill.

Each test loads a .fsh effect shader from tests/effects/, patches it for
DrawRect's CustomFill interface, and renders a single frame.
"""

import os
import re

import pynopegl as ngl
from pynopegl_utils.tests.cmp_png import test_png

W, H = 540, 540

_EFFECTS_DIR = os.path.join(os.path.dirname(__file__), "effects")
_ASSETS_DIR = os.path.join(
    os.path.dirname(__file__),
    "../python/pynopegl-utils/pynopegl_utils/assets",
)
_LANTERN = os.path.join(_ASSETS_DIR, "Unsplash-Andreas-Rasmussen-white-and-black-kanji-lantern.jpg")


def _read_effect(name: str) -> str:
    path = os.path.join(_EFFECTS_DIR, f"{name}.fsh")
    with open(path) as f:
        return f.read()


def _patch_effect(glsl: str, defines_as_uniforms: set[str] | None = None) -> tuple[str, str]:
    """Patch an effect shader for use in CustomFill.

    The effect shaders follow a convention:
      - Helper functions before void main()
      - Inside main(): read v_tex_coord, sample u_texture via texture2D,
        write gl_FragColor
      - UV origin is bottom-left (Y needs flipping for DrawRect's top-left)

    Returns (glsl_header, color_glsl) for CustomFill.
    """
    src = glsl

    # Strip #define lines for values provided as uniforms and collect all
    # define names so we can #undef them after the function body to prevent
    # them from leaking into drawrect_frag (e.g. #define aa would collide).
    defined_names = []

    def _process_define(m):
        name = m.group(1)
        rest = m.group(0)
        if defines_as_uniforms and name in defines_as_uniforms:
            return ""  # will be provided as a uniform
        defined_names.append(name)
        return rest  # keep the #define as-is

    src = re.sub(r"#define\s+(\w+)\s+.+", _process_define, src)

    # Also collect flag-style defines (no value)
    def _process_flag_define(m):
        defined_names.append(m.group(1))
        return m.group(0)

    src = re.sub(r"#define\s+(\w+)\s*$", _process_flag_define, src, flags=re.MULTILINE)

    # Patch texture sampling: texture2D(tex, uv) -> ngl_texvideo(tex, vec2(uv.x, 1.0 - uv.y))
    # Effects use bottom-left UV origin (Y-flipped) but nope.gl textures are
    # stored top-to-bottom, so we flip Y back before sampling.
    def _replace_texture2d(src_text):
        result = []
        i = 0
        tag = "texture2D"
        while i < len(src_text):
            idx = src_text.find(tag, i)
            if idx == -1:
                result.append(src_text[i:])
                break
            result.append(src_text[i:idx])
            # Find opening paren
            p = idx + len(tag)
            while p < len(src_text) and src_text[p] in " \t":
                p += 1
            if p >= len(src_text) or src_text[p] != "(":
                result.append(tag)
                i = idx + len(tag)
                continue
            # Find matching close paren
            depth = 1
            start = p + 1
            p += 1
            while p < len(src_text) and depth > 0:
                if src_text[p] == "(":
                    depth += 1
                elif src_text[p] == ")":
                    depth -= 1
                p += 1
            args = src_text[start : p - 1]
            # Split at first comma (outside nested parens)
            comma_depth = 0
            comma_pos = None
            for ci, ch in enumerate(args):
                if ch == "(":
                    comma_depth += 1
                elif ch == ")":
                    comma_depth -= 1
                elif ch == "," and comma_depth == 0:
                    comma_pos = ci
                    break
            if comma_pos is not None:
                tex = args[:comma_pos].strip()
                coords = args[comma_pos + 1 :].strip()
                result.append(f"ngl_texvideo({tex}, _ngl_tex_uv({coords}))")
            else:
                result.append(f"ngl_texvideo({args})")
            i = p
        return "".join(result)

    src = _replace_texture2d(src)

    # Patch output: gl_FragColor = expr -> ngl_frag_color = expr
    # (we'll wrap with return at the end)
    src = src.replace("gl_FragColor", "ngl_frag_color")
    src = src.replace("fragColor", "ngl_frag_color")

    # Split at void main()
    main_match = re.search(r"void\s+main\s*\(\s*\)\s*\{", src)
    if not main_match:
        raise ValueError("No void main() found in shader")

    header = src[: main_match.start()].strip()
    body_start = main_match.end()

    # Find matching closing brace
    depth = 1
    pos = body_start
    while depth > 0 and pos < len(src):
        if src[pos] == "{":
            depth += 1
        elif src[pos] == "}":
            depth -= 1
        pos += 1
    body = src[body_start : pos - 1].strip()

    # The ngl_color() function receives (vec2 uv, vec2 tex_coord) as parameters.
    # Effects declare "vec2 uv = v_tex_coord;" which would redeclare the parameter.
    # Convert declarations to assignments.
    body = re.sub(r"\bvec2\s+uv\s*=\s*", "uv = ", body)

    # Inject v_tex_coord with scaling undone so procedural effects (circles,
    # patterns) aren't distorted.  When frag_uv_scale=(1,1) this reduces to
    # the plain Y-flip: vec2(tex_coord.x, 1.0 - tex_coord.y).
    # Texture sampling goes through _ngl_tex_uv() which re-applies the scaling.
    body_prefix = (
        "vec2 v_tex_coord = (vec2(tex_coord.x, 1.0 - tex_coord.y) - 0.5)"
        " / frag_uv_scale + 0.5;\n"
        "vec4 ngl_frag_color;\n"
    )

    color_glsl = body_prefix + body + "\nreturn ngl_frag_color;\n"

    # Add #undef for all defines to prevent leaking into drawrect_frag
    if defined_names:
        undefs = "\n".join(f"#undef {name}" for name in defined_names)
        color_glsl += undefs + "\n"

    # GLES allows pow(vecN, float) but desktop GLSL 4.60 does not.
    # Replace pow() calls in effect code with _ngl_pow() which handles mixed types.
    header = re.sub(r"\bpow\s*\(", "_ngl_pow(", header)
    color_glsl = re.sub(r"\bpow\s*\(", "_ngl_pow(", color_glsl)
    _pow_compat = "float _ngl_pow(float v, float e) { return pow(v, e); }\n" + "\n".join(
        f"vec{n} _ngl_pow(vec{n} v, float e) {{ return pow(v, vec{n}(e)); }}\n"
        f"vec{n} _ngl_pow(vec{n} v, vec{n} e) {{ return pow(v, e); }}"
        for n in (2, 3, 4)
    )
    # Helper to remap effect UV (bottom-left, unscaled) to texture sampling
    # coordinates (top-left, scaled by frag_uv_scale from DrawRect).
    # When scaling=none, frag_uv_scale=(1,1) and this reduces to a plain Y-flip.
    _tex_uv_helper = (
        "vec2 _ngl_tex_uv(vec2 c) {\n" "    return (vec2(c.x, 1.0 - c.y) - 0.5) * frag_uv_scale + 0.5;\n" "}\n"
    )
    header = _pow_compat + "\n" + _tex_uv_helper + header

    return header, color_glsl


def _build_effect_scene(
    cfg: ngl.SceneCfg,
    effect_name: str,
    duration: float = 10.0,
    defines_as_uniforms: dict[str, float] | None = None,
    extra_resources: list | None = None,
):
    """Build a DrawRect scene applying an effect shader over a texture."""
    cfg.aspect_ratio = (W, H)
    cfg.duration = duration

    glsl = _read_effect(effect_name)
    header, color_glsl = _patch_effect(
        glsl, defines_as_uniforms=set(defines_as_uniforms.keys()) if defines_as_uniforms else None
    )

    resources = [
        ngl.Texture2D(
            data_src=ngl.Media(filename=_LANTERN),
            min_filter="linear",
            mag_filter="linear",
            label="u_texture",
        ),
        ngl.Time(label="i_time"),
        ngl.UniformVec2(value=(W, H), label="u_size"),
        ngl.UniformVec2(value=(W, H), label="i_size"),
        ngl.Time(label="u_time"),
        ngl.UniformFloat(value=1.0, label="i_play"),
        ngl.UniformFloat(value=duration, label="i_duration"),
        ngl.UniformVec2(value=(W, H), label="i_media_size"),
    ]

    if defines_as_uniforms:
        for name, value in defines_as_uniforms.items():
            resources.append(ngl.UniformFloat(value=value, label=name))

    if extra_resources:
        resources.extend(extra_resources)

    fill = ngl.CustomFill(
        glsl_header=header if header else None,
        color_glsl=color_glsl,
        frag_resources=resources,
        scaling="fit",
        wrap="discard",
    )
    return ngl.DrawRect(rect=(0, 0, W, H), fill=fill)


# ── Simple effects: i_time + u_texture (+ optional u_size) ──────────────────


@test_png(width=W, height=H)
@ngl.scene()
def effect_noise(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "noise")


@test_png(width=W, height=H)
@ngl.scene()
def effect_glitch(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "glitch")


@test_png(width=W, height=H)
@ngl.scene()
def effect_glitchvideo(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "glitchvideo")


@test_png(width=W, height=H)
@ngl.scene()
def effect_vhs(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "vhs")


@test_png(width=W, height=H)
@ngl.scene()
def effect_vhs2(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "vhs2")


@test_png(width=W, height=H)
@ngl.scene()
def effect_vhssoft(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "vhssoft")


@test_png(width=W, height=H)
@ngl.scene()
def effect_vhsstrong(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "vhsstrong")


@test_png(width=W, height=H)
@ngl.scene()
def effect_filmburns(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "filmburns")


@test_png(width=W, height=H)
@ngl.scene()
def effect_grainnoise(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "grainnoise")


@test_png(width=W, height=H)
@ngl.scene()
def effect_radialblurchroma(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "radialblurchroma")


@test_png(width=W, height=H)
@ngl.scene()
def effect_slideshowblackwhite(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "slideshowblackwhite")


@test_png(width=W, height=H)
@ngl.scene()
def effect_slideshow(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "slideshow")


@test_png(width=W, height=H)
@ngl.scene()
def effect_stripesshadows(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "stripesshadows")


@test_png(width=W, height=H)
@ngl.scene()
def effect_sliderepeat(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "sliderepeat")


# ── Effects using u_size ─────────────────────────────────────────────────────


@test_png(width=W, height=H)
@ngl.scene()
def effect_grain(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "grain")


@test_png(width=W, height=H)
@ngl.scene()
def effect_bokeh(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "bokeh")


@test_png(width=W, height=H)
@ngl.scene()
def effect_frostedglass(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "frostedglass")


@test_png(width=W, height=H)
@ngl.scene()
def effect_crt(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "crt")


@test_png(width=W, height=H)
@ngl.scene()
def effect_ember(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "ember")


@test_png(width=W, height=H)
@ngl.scene()
def effect_snow(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "snow")


@test_png(width=W, height=H)
@ngl.scene()
def effect_snow2(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "snow2")


@test_png(width=W, height=H)
@ngl.scene()
def effect_hearts(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "hearts")


@test_png(width=W, height=H)
@ngl.scene()
def effect_crosses(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "crosses")


@test_png(width=W, height=H)
@ngl.scene()
def effect_crosses2(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "crosses2")


@test_png(width=W, height=H)
@ngl.scene()
def effect_oldvideo(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "oldvideo")


@test_png(width=W, height=H)
@ngl.scene()
def effect_dustscratches(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "dustscratches")


@test_png(width=W, height=H)
@ngl.scene()
def effect_dustscratchessoft(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "dustscratchessoft")


@test_png(width=W, height=H)
@ngl.scene()
def effect_dustscratchesyellow(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "dustscratchesyellow")


@test_png(width=W, height=H)
@ngl.scene()
def effect_dustframe(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "dustframe")


@test_png(width=W, height=H)
@ngl.scene()
def effect_eightmmframe(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "eightmmframe")


@test_png(width=W, height=H)
@ngl.scene()
def effect_glitchblocks(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "glitchblocks")


@test_png(width=W, height=H)
@ngl.scene()
def effect_glitchchroma(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "glitchchroma")


@test_png(width=W, height=H)
@ngl.scene()
def effect_glitchlines(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "glitchlines")


@test_png(width=W, height=H)
@ngl.scene()
def effect_papergrain(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "papergrain")


@test_png(width=W, height=H)
@ngl.scene()
def effect_smokebottom(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "smokebottom")


@test_png(width=W, height=H)
@ngl.scene()
def effect_smokeframe(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "smokeframe")


@test_png(width=W, height=H)
@ngl.scene()
def effect_super8frame(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "super8frame")


@test_png(width=W, height=H)
@ngl.scene()
def effect_super8frame2(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "super8frame2")


@test_png(width=W, height=H)
@ngl.scene()
def effect_super8frame3(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "super8frame3")


@test_png(width=W, height=H)
@ngl.scene()
def effect_super8frame4(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "super8frame4")


# ── Effects with i_play ──────────────────────────────────────────────────────


@test_png(width=W, height=H)
@ngl.scene()
def effect_neonframe(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "neonframe")


@test_png(width=W, height=H)
@ngl.scene()
def effect_neonframefrance(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "neonframefrance")


@test_png(width=W, height=H)
@ngl.scene()
def effect_neonframerainbow(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "neonframerainbow")


@test_png(width=W, height=H)
@ngl.scene()
def effect_neonframesmoke(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "neonframesmoke")


@test_png(width=W, height=H)
@ngl.scene()
def effect_neonframestraight(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "neonframestraight")


@test_png(width=W, height=H)
@ngl.scene()
def effect_concreteframe(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "concreteframe")


@test_png(width=W, height=H)
@ngl.scene()
def effect_glitter(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "glitter")


@test_png(width=W, height=H)
@ngl.scene()
def effect_ice(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "ice")


@test_png(width=W, height=H)
@ngl.scene()
def effect_plastic(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "plastic")


# ── Effects with i_duration ──────────────────────────────────────────────────


@test_png(width=W, height=H)
@ngl.scene()
def effect_levelsramp(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "levelsramp")


@test_png(width=W, height=H)
@ngl.scene()
def effect_rotatecontinuous(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "rotatecontinuous")


@test_png(width=W, height=H)
@ngl.scene()
def effect_swirlout(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "swirlout")


# ── Effects with i_size only (no u_size) ─────────────────────────────────────


@test_png(width=W, height=H)
@ngl.scene()
def effect_shadowflat(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "shadowflat")


@test_png(width=W, height=H)
@ngl.scene()
def effect_shadowsoft(cfg: ngl.SceneCfg):
    return _build_effect_scene(cfg, "shadowsoft")
