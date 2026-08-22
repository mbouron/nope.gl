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

import array
import math
import textwrap

import pynopegl as ngl
from pynopegl_utils.tests.cmp_render import test_render

W, H = 256, 256


def _canvas(cfg: ngl.SceneCfg, *children, duration: float = 1.0):
    cfg.duration = duration
    return ngl.Canvas2D(width=W, height=H, children=list(children))


def _colored_rect(x, y, w, h, color):
    fill = ngl.ColorPaint(color=color)
    return ngl.DrawRect2D(rect=(x, y, w, h), fill=fill)


def _animated_scene():
    r1 = ngl.DrawRect2D(
        rect=(16, 16, 100, 100),
        fill=ngl.ColorPaint(color=(0.8, 0.2, 0.2, 1.0)),
        rotation=ngl.AnimatedFloat(
            [
                ngl.AnimKeyFrameFloat(0.0, 0.0),
                ngl.AnimKeyFrameFloat(3.0, 45.0),
            ]
        ),
    )
    r2 = ngl.DrawRect2D(
        rect=(80, 80, 120, 120),
        fill=ngl.GradientPaint(color0=(0.1, 0.1, 0.9), color1=(0.1, 0.9, 0.1)),
        translate=ngl.AnimatedVec2(
            [
                ngl.AnimKeyFrameVec2(0.0, (0.0, 0.0)),
                ngl.AnimKeyFrameVec2(3.0, (30.0, -20.0)),
            ]
        ),
    )
    r3 = ngl.DrawRect2D(
        rect=(140, 30, 80, 80),
        fill=ngl.ColorPaint(color=(0.2, 0.8, 0.2, 1.0)),
        scale=ngl.AnimatedVec2(
            [
                ngl.AnimKeyFrameVec2(0.0, (1.0, 1.0)),
                ngl.AnimKeyFrameVec2(3.0, (1.5, 0.8)),
            ]
        ),
        corner_radius=(10, 10),
    )
    return [r1, r2, r3]


_SIMPLE_INVERT_SHADER = textwrap.dedent("""
    vec4 color = ngl_texvideo(tex, tex_coord);
    return vec4(1.0 - color.rgb, color.a);
""")


@test_render(keyframes=4, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_passthrough(cfg: ngl.SceneCfg):
    """Effect2D passthrough with multiple animated/transformed children."""
    effect = ngl.Effect2D(children=_animated_scene())
    return _canvas(cfg, effect, duration=4.0)


@test_render(keyframes=4, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_opacity(cfg: ngl.SceneCfg):
    """Effect2D with reduced opacity."""
    bg = _colored_rect(0, 0, W, H, (0.1, 0.1, 0.4, 1.0))
    effect = ngl.Effect2D(children=_animated_scene(), opacity=0.5)
    return _canvas(cfg, bg, effect, duration=4.0)


@test_render(keyframes=4, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_visible_false(cfg: ngl.SceneCfg):
    """Effect2D with visible=False renders nothing."""
    bg = _colored_rect(0, 0, W, H, (0.1, 0.1, 0.4, 1.0))
    effect = ngl.Effect2D(children=_animated_scene(), visible=False)
    return _canvas(cfg, bg, effect, duration=4.0)


@test_render(keyframes=4, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_grayscale(cfg: ngl.SceneCfg):
    """Effect2D with a grayscale fragment shader."""
    effect = ngl.Effect2D(
        children=_animated_scene(),
        shaders=[
            ngl.Effect2DShader(
                glsl_color="vec4 color = ngl_texvideo(tex, tex_coord);\n"
                "float lum = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));\n"
                "return vec4(lum, lum, lum, color.a);",
            )
        ],
    )
    return _canvas(cfg, effect, duration=4.0)


@test_render(keyframes=4, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_invert(cfg: ngl.SceneCfg):
    """Effect2D with a color inversion fragment shader."""
    effect = ngl.Effect2D(
        children=_animated_scene(),
        shaders=[ngl.Effect2DShader(glsl_color=_SIMPLE_INVERT_SHADER)],
    )
    return _canvas(cfg, effect, duration=4.0)


def _get_effect2d_enabled_func():
    enabled = ngl.UniformBool(value=False)
    node_effect = ngl.Effect2D(
        children=[_colored_rect(16, 48, 96, 160, (0.8, 0.2, 0.1, 1.0))],
        shaders=[ngl.Effect2DShader(glsl_color=_SIMPLE_INVERT_SHADER)],
        enabled=enabled,
    )
    live_effect = ngl.Effect2D(
        children=[_colored_rect(144, 48, 96, 160, (0.8, 0.2, 0.1, 1.0))],
        shaders=[ngl.Effect2DShader(glsl_color=_SIMPLE_INVERT_SHADER)],
        enabled=False,
    )

    def keyframes_callback(t_id):
        enabled.set_value(t_id % 2 == 1)
        live_effect.set_enabled(t_id % 2 == 0)

    @test_render(
        keyframes=4,
        keyframes_callback=keyframes_callback,
        tolerance=3,
        diff_threshold=0.005,
        exercise_serialization=False,
    )
    @ngl.scene(width=W, height=H)
    def scene_func(cfg: ngl.SceneCfg):
        """Effect2D can be bypassed with live and node-driven parameters."""
        return _canvas(cfg, node_effect, live_effect, duration=4.0)

    return scene_func


effect2d_enabled = _get_effect2d_enabled_func()


_INVERT_SHADER = textwrap.dedent("""\
    vec4 color = ngl_texvideo(tex, tex_coord);
    if (color.a > 0.0)
        color.rgb /= color.a;
    return vec4(1.0 - color.rgb, color.a);
""")

_GRAYSCALE_SHADER = textwrap.dedent("""\
    vec4 color = ngl_texvideo(tex, tex_coord);
    if (color.a > 0.0)
        color.rgb /= color.a;
    float lum = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
    return vec4(lum, lum, lum, color.a);
""")


def _get_effect2d_timed_shaders_func():
    invert = ngl.Effect2DShader(
        glsl_color=_INVERT_SHADER,
        premult=True,
        start=1.0,
        end=2.0,
    )
    grayscale = ngl.Effect2DShader(
        glsl_color=_GRAYSCALE_SHADER,
        premult=True,
        start=3.0,
        end=4.0,
    )
    effect = ngl.Effect2D(
        children=[_colored_rect(48, 48, 160, 160, (0.8, 0.2, 0.1, 1.0))],
        bounds="canvas",
        shaders=[invert, grayscale],
    )

    def keyframes_callback(t_id):
        if t_id == 2:
            grayscale.set_range(2.0, 3.0)

    @test_render(
        keyframes=4,
        keyframes_callback=keyframes_callback,
        tolerance=3,
        diff_threshold=0.005,
        exercise_serialization=False,
    )
    @ngl.scene(width=W, height=H)
    def scene_func(cfg: ngl.SceneCfg):
        """Effect2D selects timed shaders and reflects live range changes."""
        return _canvas(cfg, effect, duration=4.0)

    return scene_func


effect2d_timed_shaders = _get_effect2d_timed_shaders_func()


@test_render(keyframes=1, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_bounds_rect(cfg: ngl.SceneCfg):
    """Effect2D bounds=rect clips the effect to an explicit rect, ignoring the children bbox."""
    bg = _colored_rect(0, 0, W, H, (0.1, 0.1, 0.4, 1.0))
    effect = ngl.Effect2D(
        children=[_colored_rect(16, 16, 224, 224, (0.8, 0.2, 0.1, 1.0))],
        bounds="rect",
        rect=(64, 32, 96, 160),
        shaders=[ngl.Effect2DShader(glsl_color=_INVERT_SHADER, premult=True)],
    )
    return _canvas(cfg, bg, effect)


@test_render(keyframes=1, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_bounds_rect_zero(cfg: ngl.SceneCfg):
    """Effect2D with bounds=rect and the default rect renders nothing."""
    bg = _colored_rect(0, 0, W, H, (0.1, 0.1, 0.4, 1.0))
    effect = ngl.Effect2D(
        children=[_colored_rect(16, 16, 224, 224, (0.8, 0.2, 0.1, 1.0))],
        bounds="rect",
        shaders=[ngl.Effect2DShader(glsl_color=_INVERT_SHADER, premult=True)],
    )
    return _canvas(cfg, bg, effect)


def _get_effect2d_bounds_live_func():
    uv_shader = "vec4 color = ngl_texvideo(tex, tex_coord); return vec4(uv, 0.0, 1.0) * color.a;"
    effect = ngl.Effect2D(
        children=[_colored_rect(48, 48, 160, 160, (0.8, 0.2, 0.1, 1.0))],
        rect=(0, 0, 128, H),
        shaders=[ngl.Effect2DShader(glsl_color=uv_shader)],
    )

    modes = ("children", "canvas", "rect")

    def keyframes_callback(t_id):
        effect.set_bounds(modes[t_id % len(modes)])

    @test_render(
        keyframes=3,
        keyframes_callback=keyframes_callback,
        tolerance=3,
        diff_threshold=0.005,
        exercise_serialization=False,
    )
    @ngl.scene(width=W, height=H)
    def scene_func(cfg: ngl.SceneCfg):
        """Effect2D bounds can be switched live between children, canvas and rect."""
        bg = _colored_rect(0, 0, W, H, (0.1, 0.1, 0.4, 1.0))
        return _canvas(cfg, bg, effect, duration=3.0)

    return scene_func


effect2d_bounds_live = _get_effect2d_bounds_live_func()


def _get_effect2d_enabled_shaders_func():
    enabled = ngl.UniformBool(value=True)
    effect = ngl.Effect2D(
        children=[_colored_rect(48, 48, 160, 160, (0.8, 0.2, 0.1, 1.0))],
        bounds="canvas",
        enabled=enabled,
        shaders=[ngl.Effect2DShader(glsl_color=_INVERT_SHADER, premult=True)],
    )

    def keyframes_callback(t_id):
        enabled.set_value(t_id % 2 == 0)

    @test_render(
        keyframes=4,
        keyframes_callback=keyframes_callback,
        tolerance=3,
        diff_threshold=0.005,
        exercise_serialization=False,
    )
    @ngl.scene(width=W, height=H)
    def scene_func(cfg: ngl.SceneCfg):
        """Effect2D with enabled=False bypasses the scheduled shaders."""
        return _canvas(cfg, effect, duration=4.0)

    return scene_func


effect2d_enabled_shaders = _get_effect2d_enabled_shaders_func()


@test_render(keyframes=3, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_shader_passthrough_entry(cfg: ngl.SceneCfg):
    """An empty Effect2DShader body masks a longer shader over its own range."""
    effect = ngl.Effect2D(
        children=[_colored_rect(48, 48, 160, 160, (0.8, 0.2, 0.1, 1.0))],
        shaders=[
            ngl.Effect2DShader(glsl_color="", start=1.0, end=2.0),
            ngl.Effect2DShader(glsl_color=_INVERT_SHADER, premult=True, start=0.0, end=3.0),
        ],
    )
    return _canvas(cfg, effect, duration=3.0)


@test_render(tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_shader_premultiplied_passthrough(cfg: ngl.SceneCfg):
    """The default shader contract preserves premultiplied offscreen samples."""
    color = (0.8, 0.2, 0.1, 0.5)
    baseline = ngl.Effect2D(children=[_colored_rect(16, 48, 96, 160, color)])
    identity = ngl.Effect2D(
        children=[_colored_rect(144, 48, 96, 160, color)],
        shaders=[ngl.Effect2DShader(glsl_color="return ngl_texvideo(tex, tex_coord);")],
    )
    bg = _colored_rect(0, 0, W, H, (0.1, 0.1, 0.25, 1.0))
    return _canvas(cfg, bg, baseline, identity)


@test_render(keyframes=4, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_nested_in_group2d(cfg: ngl.SceneCfg):
    """Effect2D nested inside a transformed Group2D."""
    effect = ngl.Effect2D(
        children=_animated_scene(),
        shaders=[ngl.Effect2DShader(glsl_color=_SIMPLE_INVERT_SHADER)],
    )
    group = ngl.Group2D(children=[effect], rotation=45.0, anchor=(W / 2, H / 2))
    return _canvas(cfg, group, duration=4.0)


@test_render(keyframes=2, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_nested_transform_bounds(cfg: ngl.SceneCfg):
    """A parent Effect2D tracks a transformed child's bounds and size changes."""
    scale = ngl.AnimatedVec2(
        [
            ngl.AnimKeyFrameVec2(0.0, (1.0, 1.0)),
            ngl.AnimKeyFrameVec2(1.0, (1.75, 1.25)),
        ]
    )
    inner = ngl.Effect2D(
        children=[_colored_rect(24, 72, 48, 80, (0.9, 0.4, 0.1, 1.0))],
        dilation=12.0,
        translate=(128, 0),
        scale=scale,
    )
    outer = ngl.Effect2D(children=[inner])
    bg = _colored_rect(0, 0, W, H, (0.1, 0.1, 0.25, 1.0))
    return _canvas(cfg, bg, outer, duration=2.0)


@test_render(tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_offscreen_canvas_density(cfg: ngl.SceneCfg):
    """A nested Effect2D uses the offscreen target density."""
    cfg.duration = 1.0

    offscreen_w = offscreen_h = 64
    density_scale = 4
    texture = ngl.Texture2D(
        width=W * density_scale,
        height=H * density_scale,
        min_filter="linear",
        mag_filter="linear",
    )
    bar = ngl.DrawRect2D(
        rect=(-32, 32, 128, 24),
        fill=ngl.ColorPaint(color=(0.95, 0.55, 0.15, 1.0)),
        rotation=30.0,
        anchor=(32, 32),
    )
    effect = ngl.Effect2D(children=[bar])
    offscreen = ngl.OffscreenCanvas2D(
        children=[effect],
        width=offscreen_w,
        height=offscreen_h,
        color_textures=[texture],
        clear_color=(0.1, 0.1, 0.25, 1.0),
    )

    def get_centered_crop_rect(width, height, scale):
        display_w = width * scale
        display_h = height * scale
        return ((width - display_w) / 2, (height - display_h) / 2, display_w, display_h)

    display_rect = get_centered_crop_rect(W, H, density_scale)
    display = ngl.DrawRect2D(rect=display_rect, fill=ngl.TexturePaint(texture=texture))
    return _canvas(cfg, offscreen, display)


@test_render(tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_offscreen_canvas_isolates_transform(cfg: ngl.SceneCfg):
    """Offscreen children ignore an enclosing Group2D transform."""
    cfg.duration = 1.0
    tex = ngl.Texture2D(width=W, height=H)

    effect = ngl.Effect2D(children=[_colored_rect(32, 32, 96, 96, (0.9, 0.4, 0.1, 1.0))])
    offscreen = ngl.OffscreenCanvas2D(
        children=[effect],
        width=0,
        height=0,
        color_textures=[tex],
        clear_color=(0.1, 0.2, 0.15, 1.0),
    )
    display = ngl.DrawRect2D(rect=(0, 0, W, H), fill=ngl.TexturePaint(texture=tex))
    group = ngl.Group2D(children=[offscreen, display], translate=(48, 32), rotation=20.0, anchor=(W / 2, H / 2))
    return _canvas(cfg, group)


def _gaussian_kernel_1d(sigma):
    """Compute a normalized 1D Gaussian kernel."""
    radius = math.ceil(3 * sigma)
    size = 2 * radius + 1
    kernel = []
    for i in range(size):
        x = i - radius
        kernel.append(math.exp(-x * x / (2.0 * sigma * sigma)))
    total = sum(kernel)
    return array.array("f", [w / total for w in kernel]), radius


_GAUSSIAN_BLUR_H_GLSL = """\
    vec2 texel = 1.0 / vec2(textureSize(tex, 0));
    vec4 sum = vec4(0.0);
    for (int x = -radius; x <= radius; x++) {
        float w = kernel.weights[x + radius];
        sum += ngl_texvideo(tex, tex_coord + vec2(float(x), 0.0) * texel) * w;
    }
    return sum;
"""


_GAUSSIAN_BLUR_V_GLSL = """\
    vec2 texel = 1.0 / vec2(textureSize(tex, 0));
    vec4 sum = vec4(0.0);
    for (int y = -radius; y <= radius; y++) {
        float w = kernel.weights[y + radius];
        sum += ngl_texvideo(tex, tex_coord + vec2(0.0, float(y)) * texel) * w;
    }
    return sum;
"""


@test_render(tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_resource_scene_texture(cfg: ngl.SceneCfg):
    """A scene-backed texture resource is pre-drawn and sampled by the effect shader."""
    gradient = ngl.DrawGradient(color0=(0.0, 0.0, 1.0), color1=(1.0, 0.0, 0.0), linear=True)
    mask = ngl.Texture2D(
        width=W,
        height=H,
        data_src=gradient,
        min_filter="linear",
        mag_filter="linear",
    )
    effect = ngl.Effect2D(
        children=[_colored_rect(32, 32, 192, 192, (1.0, 1.0, 1.0, 1.0))],
        shaders=[
            ngl.Effect2DShader(
                glsl_color="vec4 color = ngl_texvideo(tex, tex_coord);\n"
                "vec4 tint = ngl_texvideo(mask, tex_coord);\n"
                "return color * tint;",
                resources={"mask": mask},
            )
        ],
    )
    bg = _colored_rect(0, 0, W, H, (0.1, 0.1, 0.25, 1.0))
    return _canvas(cfg, bg, effect)


@test_render(keyframes=4, tolerance=3, diff_threshold=0.005)
@ngl.scene(width=W, height=H)
def effect2d_blur(cfg: ngl.SceneCfg):
    """Separable Gaussian blur."""
    sigma = 10.0
    weights, radius = _gaussian_kernel_1d(sigma)
    kernel = ngl.Block(
        fields=[ngl.BufferFloat(data=weights, label="weights")],
        label="kernel",
    )
    radius_uniform = ngl.UniformInt(value=radius, label="radius")
    dilation = math.ceil(3 * sigma)
    resources = {"kernel": kernel, "radius": radius_uniform}
    blur_h = ngl.Effect2D(
        children=_animated_scene(),
        shaders=[ngl.Effect2DShader(glsl_color=_GAUSSIAN_BLUR_H_GLSL, resources=resources)],
        dilation=dilation,
    )
    blur = ngl.Effect2D(
        children=[blur_h],
        shaders=[ngl.Effect2DShader(glsl_color=_GAUSSIAN_BLUR_V_GLSL, resources=resources)],
        dilation=dilation,
    )
    return _canvas(cfg, blur, duration=4.0)
