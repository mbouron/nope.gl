#
# Copyright 2019-2022 GoPro Inc.
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

import atexit
import csv
import hashlib
import locale
import math
import os
import pprint
import random
import tempfile
from collections import namedtuple
from pathlib import Path

import pynopegl as ngl
from pynopegl_utils.misc import get_backend, load_media
from pynopegl_utils.toolbox.grid import autogrid_simple

_backend_str = os.environ.get("BACKEND")
_backend = get_backend(_backend_str) if _backend_str else ngl.Backend.AUTO


def _is_close(a, b, tol=1):
    return abs(a - b) <= tol


def _get_scene():
    return ngl.Scene.from_params(ngl.DrawColor(geometry=ngl.Quad()))


def api_backend():
    ctx = ngl.Context()
    fake_backend_cls = namedtuple("FakeBackend", "value")
    fake_backend = fake_backend_cls(value=0x1234)
    ret = ctx.configure(ngl.Config(offscreen=True, width=16, height=16, backend=fake_backend))
    assert ret == ngl.Error.INVALID_ARG
    del ctx


def api_debug():
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=16, height=16, debug=True, backend=_backend))
    assert ret == 0
    del ctx


def api_reconfigure():
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend))
    assert ret == 0
    scene = _get_scene()
    assert ctx.set_scene(scene) == 0
    assert ctx.draw(0) == 0
    ret = ctx.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend))
    assert ret == 0
    assert ctx.draw(1) == 0
    del ctx


def api_reconfigure_clearcolor(width=16, height=16):
    import zlib

    capture_buffer = bytearray(width * height * 4)
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(offscreen=True, width=width, height=height, backend=_backend, capture_buffer=capture_buffer)
    )
    assert ret == 0
    scene = _get_scene()
    assert ctx.set_scene(scene) == 0
    assert ctx.draw(0) == 0
    assert zlib.crc32(capture_buffer) == 0xB4BD32FA
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=_backend,
            capture_buffer=capture_buffer,
            clear_color=(0.4, 0.4, 0.4, 1.0),
        )
    )
    assert ret == 0
    assert ctx.draw(0) == 0
    assert zlib.crc32(capture_buffer) == 0x05C44869
    del capture_buffer
    del ctx


def api_reconfigure_fail():
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend))
    assert ret == 0
    scene = _get_scene()
    assert ctx.set_scene(scene) == 0
    assert ctx.draw(0) == 0
    ret = ctx.configure(ngl.Config(offscreen=False, backend=_backend))
    assert ret != 0
    assert ctx.draw(1) != 0
    del ctx


def api_resize_fail():
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend))
    assert ret == 0
    ret = ctx.resize(32, 32)
    assert ret != 0
    del ctx


def api_capture_buffer(width=16, height=16):
    import zlib

    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=width, height=height, backend=_backend))
    assert ret == 0
    scene = _get_scene()
    assert ctx.set_scene(scene) == 0
    for _ in range(2):
        capture_buffer = bytearray(width * height * 4)
        assert ctx.set_capture_buffer(capture_buffer) == 0
        assert ctx.draw(0) == 0
        assert ctx.set_capture_buffer(None) == 0
        assert ctx.draw(0) == 0
        assert zlib.crc32(capture_buffer) == 0xB4BD32FA
    del ctx


def api_ctx_ownership():
    ctx = ngl.Context()
    ctx2 = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend))
    assert ret == 0
    ret = ctx2.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend))
    assert ret == 0
    scene = _get_scene()
    assert ctx.set_scene(scene) == 0
    assert ctx.draw(0) == 0
    assert ctx2.set_scene(scene) != 0
    assert ctx2.draw(0) == 0
    del ctx
    del ctx2


def api_scene_context_transfer():
    """Test transfering a scene from one context to another"""
    scene = _get_scene()
    ctx = ngl.Context()
    ctx2 = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend))
    assert ret == 0
    ret = ctx2.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend))
    assert ret == 0
    assert ctx.set_scene(scene) == 0
    assert ctx.draw(0) == 0
    assert ctx.set_scene(None) == 0
    assert ctx2.set_scene(scene) == 0
    assert ctx2.draw(0) == 0
    assert ctx2.set_scene(None) == 0


def api_scene_lifetime():
    """Test if the context is still working properly when we release the user scene ownership"""
    scene = _get_scene()
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend))
    assert ret == 0
    assert ctx.set_scene(scene) == 0
    del scene
    assert ctx.draw(0) == 0
    del ctx


def api_scene_mutate():
    """Test if the scene association is prevent graph structure changes"""
    hook = ngl.Group()
    eval = ngl.EvalVec3()
    draw = ngl.DrawColor(color=eval)
    root = ngl.Group(children=[hook])
    assert hook.add_children(draw) == 0
    scene = ngl.Scene.from_params(root)
    assert hook.add_children(ngl.DrawColor()) != 0
    assert draw.set_opacity(0.5) == 0  # we can change a direct value...
    assert draw.set_opacity(ngl.UniformFloat()) != 0  # ...but we can't change the structure
    assert eval.update_resources(t=ngl.Time()) != 0
    del scene


def api_scene_ownership():
    """Test if part of a graph is shared between 2 different scenes"""
    shared_geometry = ngl.Quad()
    scene0 = ngl.Scene.from_params(ngl.DrawColor(geometry=shared_geometry))
    try:
        scene1 = ngl.Scene.from_params(ngl.DrawColor(geometry=shared_geometry))
    except Exception:
        pass
    else:
        del scene1
        assert False
    del scene0


def api_scene_resilience():
    """Similar to API the scene ownership test but make sure the API is error resilient"""
    shared_geometry = ngl.Quad()
    scene0 = ngl.Scene.from_params(ngl.DrawColor(geometry=shared_geometry))

    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend))
    assert ret == 0
    assert ctx.set_scene(scene0) == 0
    assert ctx.draw(0) == 0

    try:
        scene1 = ngl.Scene.from_params(ngl.DrawColor(geometry=shared_geometry))
    except Exception:
        pass
    else:
        del scene1
        assert False

    assert ctx.draw(0) == 0
    del scene0
    del ctx


def api_scene_files():
    # Store abstract file references in the graph
    media = ngl.Media(filename="nope")
    root = ngl.Group(
        children=[
            ngl.Texture2D(data_src=media),
            ngl.Texture2D(data_src=ngl.BufferByte(filename="hamster")),
        ]
    )
    scene = ngl.Scene.from_params(root)

    # Update the ref after the node has been associated with its scene
    media.set_filename("cat")

    assert scene.files == ["cat", "hamster"]

    # Replace the simple filename strings with their corresponding actual file paths
    for i, filepath in enumerate(scene.files):
        new_filepath = load_media(filepath).filename
        scene.update_filepath(i, new_filepath)

    # Query again the files and check if they've been updated
    assert all(Path(filepath).exists() for filepath in scene.files)

    # Associate with the context
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend))
    assert ret == 0
    assert ctx.set_scene(scene) == 0
    assert ctx.draw(0) == 0

    # Change one path using live controls
    new_ref = "cat was here"
    media.set_filename(new_ref)

    # Check if the change is effective back in the scene
    assert any(filepath == new_ref for filepath in scene.files)

    # Detach the context and check if the change is still persistent like other live controls
    ctx.set_scene(None)
    del ctx
    assert any(filepath == new_ref for filepath in scene.files)


def api_capture_buffer_lifetime(width=1024, height=1024):
    capture_buffer = bytearray(width * height * 4)
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(offscreen=True, width=width, height=height, backend=_backend, capture_buffer=capture_buffer)
    )
    assert ret == 0
    del capture_buffer
    scene = _get_scene()
    assert ctx.set_scene(scene) == 0
    assert ctx.draw(0) == 0
    del ctx


# Exercise the HUD rasterization. We can't really check the output, so this is
# just for blind coverage and similar code instrumentalization.
def api_hud(width=234, height=123):
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=width, height=height, backend=_backend, hud=True))
    assert ret == 0
    scene = _get_scene()
    assert ctx.set_scene(scene) == 0
    for i in range(60 * 3):
        assert ctx.draw(i / 60.0) == 0
    del ctx


def api_hud_csv(width=16, height=16):
    ctx = ngl.Context()

    # We can't use NamedTemporaryFile because we may not be able to open it
    # twice on some systems
    fd, csvpath = tempfile.mkstemp(suffix=".csv", prefix="ngl-test-hud-")
    os.close(fd)
    atexit.register(lambda: os.remove(csvpath))

    ret = ctx.configure(
        ngl.Config(offscreen=True, width=width, height=height, backend=_backend, hud=True, hud_export_filename=csvpath)
    )
    assert ret == 0
    scene = _get_scene()
    assert ctx.set_scene(scene) == 0
    for t in [0.0, 0.15, 0.30, 0.45, 1.0]:
        # Try to set a locale that messes up the representation of floats
        try:
            locale.setlocale(locale.LC_ALL, "fr_FR.UTF-8")
        except locale.Error:
            print("unable to set french locale")

        assert ctx.draw(t) == 0
    del ctx

    with open(csvpath) as csvfile:
        reader = csv.DictReader(csvfile)
        time_column = [row["time"] for row in reader]

    assert time_column == ["0.000000", "0.150000", "0.300000", "0.450000", "1.000000"], time_column


def _api_text_live_change(width=320, height=240, font_faces=None):
    import zlib

    ctx = ngl.Context()
    capture_buffer = bytearray(width * height * 4)
    ret = ctx.configure(
        ngl.Config(offscreen=True, width=width, height=height, backend=_backend, capture_buffer=capture_buffer)
    )
    assert ret == 0

    # An empty string forces the text node to deal with a pipeline with nul
    # attributes, this is what we exercise here, along with a varying up and
    # down number of characters
    text_strings = ["foo", "", "foobar", "world", "hello\nworld", "\n\n", "last"]

    # Exercise the diamond-form/prepare mechanism
    text_node = ngl.Text(font_faces=font_faces)
    root = autogrid_simple([text_node] * 4)
    scene = ngl.Scene.from_params(root)
    assert ctx.set_scene(scene) == 0

    ctx.draw(0)
    last_crc = zlib.crc32(capture_buffer)
    for i, s in enumerate(text_strings, 1):
        text_node.set_text(s)
        ctx.draw(i)
        crc = zlib.crc32(capture_buffer)
        assert crc != last_crc
        last_crc = crc


def api_text_live_change():
    return _api_text_live_change()


def api_text_live_change_with_font():
    font_faces = Path(__file__).resolve().parent / "assets" / "fonts" / "Quicksand-Medium.ttf"
    return _api_text_live_change(font_faces=[ngl.FontFace(font_faces.as_posix())])


def api_media_sharing_failure():
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend))
    assert ret == 0
    m = ngl.Media("/dev/null")
    root = ngl.Group(children=[ngl.Texture2D(data_src=m), ngl.Texture2D(data_src=m)])
    scene = ngl.Scene.from_params(root)
    assert ctx.set_scene(scene) == ngl.Error.INVALID_USAGE


def api_denied_node_live_change(width=320, height=240):
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=width, height=height, backend=_backend))
    assert ret == 0

    root = ngl.Translate(ngl.Group())
    scene = ngl.Scene.from_params(root)

    # Check that we can live change but not into a node
    assert ctx.set_scene(scene) == 0
    assert root.set_vector(1, 2, 3) == 0
    assert root.set_vector(ngl.UniformVec3(value=(3, 2, 1))) != 0

    # Check that we can not live unplug a node from a live changeable parameter
    assert root.set_vector(ngl.UniformVec3(value=(7, 8, 9))) != 0


def api_livectls():
    # Build a scene and extract its live controls
    rng = random.Random(0)
    root = ngl.Group(
        children=[
            ngl.UniformBool(live_id="b"),
            ngl.UniformFloat(live_id="f"),
            ngl.UniformIVec3(live_id="iv3"),
            ngl.UserSwitch(
                ngl.Group(
                    children=[
                        ngl.UniformMat4(live_id="m4"),
                        ngl.UniformColor(live_id="clr"),
                        ngl.UniformQuat(as_mat4=True, live_id="rot"),
                    ]
                ),
                live_id="switch",
            ),
            ngl.Text(live_id="txt"),
        ]
    )
    scene = ngl.Scene.from_params(root)
    livectls = ngl.get_livectls(scene)
    assert len(livectls) == 8

    # Attach scene and run a dummy draw to make sure it's valid
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend))
    assert ret == 0
    assert ctx.set_scene(scene) == 0
    assert ctx.draw(0) == 0

    # Apply live changes on nodes previously tracked by get_livectls()
    values = dict(
        b=True,
        f=rng.uniform(-1, 1),
        iv3=[rng.randint(-100, 100) for _ in range(3)],
        switch=False,
        m4=[rng.uniform(-1, 1) for _ in range(16)],
        clr=(0.9, 0.3, 0.8),
        rot=(0.1, -0.2, 0.5, -0.3),
        txt="test string",
    )
    for live_id, value in values.items():
        node = livectls[live_id]["node"]
        node_type = livectls[live_id]["node_type"]
        assert node_type == node.__class__.__name__
        if node_type == "UserSwitch":
            node.set_enabled(value)
        elif node_type == "Text":
            node.set_text(value)
        elif hasattr(value, "__iter__"):
            node.set_value(*value)
        else:
            node.set_value(value)

    # Detach scene from context and grab all live controls again
    assert ctx.set_scene(None) == 0
    livectls = ngl.get_livectls(scene)

    # Inspect nodes to check if they were properly altered by the live changes
    for live_id, expected_value in values.items():
        value = livectls[live_id]["val"]
        node_type = livectls[live_id]["node_type"]
        if node_type == "Text":
            assert value == expected_value, (value, expected_value)
        elif hasattr(value, "__iter__"):
            assert all(math.isclose(v, e, rel_tol=1e-6) for v, e in zip(value, expected_value))
        else:
            assert math.isclose(value, expected_value, rel_tol=1e-6)


def api_reset_scene(width=320, height=240):
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=width, height=height, backend=_backend))
    assert ret == 0
    draw = _get_scene()
    assert ctx.set_scene(draw) == 0
    ctx.draw(0)
    assert ctx.set_scene(None) == 0
    ctx.draw(1)
    assert ctx.set_scene(draw) == 0
    ctx.draw(2)
    assert ctx.set_scene(None) == 0
    ctx.draw(3)


def api_shader_init_fail(width=320, height=240):
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=width, height=height, backend=_backend))
    assert ret == 0

    draw = ngl.Draw(ngl.Quad(), ngl.Program(vertex="<bug>", fragment="<bug>"))
    scene = ngl.Scene.from_params(draw)

    assert ctx.set_scene(scene) != 0
    assert ctx.set_scene(scene) != 0  # another try to make sure the state stays consistent
    assert ctx.draw(0) == 0


def _create_trf(scene, start, end, prefetch_time=None):
    trf = ngl.TimeRangeFilter(scene, start, end)
    if prefetch_time is not None:
        trf.set_prefetch_time(prefetch_time)
    return trf


def _create_trf_scene(start, end, keep_active=False):
    texture = ngl.Texture2D(width=64, height=64, min_filter="nearest", mag_filter="nearest")
    # A subgraph using a RTT will produce a clear crash if its draw is called without a prefetch
    rtt = ngl.RenderToTexture(ngl.Identity(), clear_color=(1.0, 0.0, 0.0, 1.0), color_textures=[texture])
    draw = ngl.DrawTexture(texture=texture)
    group = ngl.Group(children=[rtt, draw])
    trf = _create_trf(group, start, start + 1)

    trf_start = _create_trf(trf, start, start + 1)
    # This group could be any node as long as it has no prefetch/release callback
    group = ngl.Group(children=[trf])
    trf_end = _create_trf(group, end - 1.0, end + 1.0)

    children = [trf_start, trf_end]

    if keep_active:
        # This TimeRangeFilter keeps the underyling graph active while preventing
        # the update/draw operations to descent into the graph for the [start, end]
        # time interval
        offset = 10.0
        prefetch_time = offset + end - start
        trf_keep_active = _create_trf(group, end + offset, end + offset + 1.0, prefetch_time)
        children += [trf_keep_active]

    root = ngl.Group(children=children)
    return ngl.Scene.from_params(root)


def api_trf_seek(width=320, height=240):
    """
    Run a special time sequence on a particularly crafted time filtered
    diamond-tree graph to detect potential release/prefetch issues.
    """
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=width, height=height, backend=_backend))
    assert ret == 0

    start = 0.0
    end = 10.0
    scene = _create_trf_scene(start, end)
    ret = ctx.set_scene(scene)
    assert ret == 0

    # The following time sequence is designed to create an inconsistent time state
    assert ctx.draw(end) == 0
    assert ctx.draw(start) == 0
    assert ctx.draw(end) == 0


def api_trf_seek_keep_alive(width=320, height=240):
    """
    Run a special time sequence on a particularly crafted time filtered
    diamond-tree graph (with nodes being kept active artificially) to detect
    potential release/prefetch issues.
    """
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=width, height=height, backend=_backend))
    assert ret == 0

    start = 0.0
    end = 10.0
    scene = _create_trf_scene(start, end, True)
    ret = ctx.set_scene(scene)
    assert ret == 0

    # The following time sequence is designed to create an inconsistent time state
    assert ctx.draw(end) == 0
    assert ctx.draw(start) == 0
    assert ctx.draw(end) == 0


def api_dot(width=320, height=240):
    """
    Exercise the ngl.dot() API.
    """
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=width, height=height, backend=_backend))
    scene = _get_scene()
    assert ctx.set_scene(scene) == 0
    assert ctx.dot(0.0) is not None
    assert ctx.dot(1.0) is not None


def api_probing():
    """
    Exercise the probing APIs; the result is platform/hardware specific so
    tricky to test its output
    """
    backends = ngl.get_backends()
    probe = ngl.probe_backends()
    pprint.pprint(backends)
    pprint.pprint(probe)


def api_caps():
    # Manually build a scene config with the default backend and explicit capabilities
    scene_cfg = ngl.SceneCfg(backend=next(k for k, v in ngl.get_backends().items() if v["is_default"]))
    backends = ngl.probe_backends()
    scene_cfg.caps = backends[scene_cfg.backend]["caps"]

    # Building this scene is expected to succeed because caps are consistent
    scene_func = ngl.scene()(lambda _: ngl.Group())
    scene_func(scene_cfg)

    # This one must fail because it lacks a capability
    scene_cfg.caps.pop(ngl.Cap.TEXT_LIBRARIES)
    try:
        scene_func(scene_cfg)
    except Exception:
        pass
    else:
        assert False


def api_get_backend():
    """
    Exercise the ngl.Context.get_backend() API; the result is platform/hardware
    specific so tricky to test its output
    """
    ctx = ngl.Context()
    cfg = ngl.Config(offscreen=True, width=32, height=32, backend=ngl.Backend.AUTO)
    try:
        backend = ctx.get_backend()
    except Exception:
        pass
    else:
        assert False
    ret = ctx.configure(cfg)
    assert ret == 0
    backend = ctx.get_backend()
    assert isinstance(backend["id"], ngl.Backend)
    assert backend["id"] != ngl.Backend.AUTO
    assert backend["is_default"] == True
    assert backend["caps"]
    pprint.pprint(backend)


def api_viewport():
    ctx = ngl.Context()
    ret = ctx.configure(ngl.Config(offscreen=True, width=640, height=480, backend=_backend))
    assert ret == 0
    assert ctx.viewport == (0, 0, 640, 480)

    scene = ngl.Scene.from_params(ngl.Group(), width=320, height=360)
    ctx.set_scene(scene)
    assert ctx.viewport == (106, 0, 426, 480)

    ctx.set_scene(None)
    assert ctx.viewport == (0, 0, 640, 480)


def api_transform_chain_check():
    invalid_chain = ngl.Translate(ngl.Rotate(ngl.Skew()))
    root = ngl.Camera(eye_transform=invalid_chain)
    try:
        ngl.Scene.from_params(root)
    except Exception:
        pass
    else:
        assert False


def _create_trf_scene_with_media(start, end):
    mi = load_media("city")
    media = ngl.Media(mi.filename)
    texture = ngl.Texture2D(data_src=media)
    draw = ngl.DrawTexture(texture=texture)
    group = ngl.Group(children=(draw,))
    trf = _create_trf(group, start, end)

    return ngl.Scene.from_params(trf)


def api_update_with_timeranges(width=320, height=240):
    """
    Exercise the `ngl_update()` API
    """
    ngl.log_set_min_level(ngl.Log.VERBOSE)
    capture_buffer = bytearray(width * height * 4)
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=_backend,
            capture_buffer=capture_buffer,
            clear_color=(0.0, 0.0, 0.0, 0.0),
        )
    )
    assert ret == 0

    start = 5.0
    end = 10.0
    scene = _create_trf_scene_with_media(start, end)
    assert ctx.set_scene(scene) == 0

    initial_hash = hashlib.md5(capture_buffer).hexdigest()
    assert ctx.update(start - 0.5) == 0
    assert initial_hash == hashlib.md5(capture_buffer).hexdigest()

    assert ctx.draw(start) == 0
    draw_hash = hashlib.md5(capture_buffer).hexdigest()
    assert initial_hash != draw_hash

    assert ctx.draw(end - 1.0) == 0
    assert draw_hash == hashlib.md5(capture_buffer).hexdigest()

    assert ctx.update(end + 1.5) == 0
    assert draw_hash == hashlib.md5(capture_buffer).hexdigest()

    assert ctx.draw(end + 1.5) == 0
    assert initial_hash == hashlib.md5(capture_buffer).hexdigest()

    assert ctx.update(end - 1.0) == 0
    assert initial_hash == hashlib.md5(capture_buffer).hexdigest()

    assert ctx.draw(end - 1.0) == 0
    assert draw_hash == hashlib.md5(capture_buffer).hexdigest()

    assert ctx.draw(0.0) == 0
    assert initial_hash == hashlib.md5(capture_buffer).hexdigest()


def api_bounding_box_before_draw(width=256, height=256):
    """Test that bounding box and transform APIs do not crash before the graph is initialized"""
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=_backend,
        )
    )
    assert ret == 0

    fill = ngl.ColorFill(color=(1.0, 0.0, 0.0, 1.0))
    rect = ngl.DrawRect2D(rect=(0, 0, 100, 80), fill=fill, label="rect")
    group = ngl.Group2D(children=[rect], translate=(50, 30), label="group")
    canvas = ngl.Canvas2D(children=[group], width=width, height=height)

    # Query before set_scene: should return zeros without crashing
    box = rect.get_bounding_box()
    assert box["center"] == (0.0, 0.0)
    assert box["extent"] == (0.0, 0.0)

    pos = rect.get_global_position()
    assert pos == (0.0, 0.0)

    rot = rect.get_global_rotation()
    assert rot == 0.0

    scl = rect.get_global_scale()
    assert scl == (0.0, 0.0)

    m = rect.get_global_transform_matrix()
    assert all(v == 0.0 for v in m)

    # Same for group and canvas
    box = group.get_bounding_box()
    assert box["center"] == (0.0, 0.0)
    box = canvas.get_bounding_box()
    assert box["center"] == (0.0, 0.0)

    # Query after set_scene but before draw: should still return zeros
    scene = ngl.Scene.from_params(canvas, width=width, height=height)
    ret = ctx.set_scene(scene)
    assert ret == 0

    box = rect.get_bounding_box()
    assert box["center"] == (0.0, 0.0)

    pos = group.get_global_position()
    assert pos == (0.0, 0.0)

    # After draw: should return actual values
    ret = ctx.draw(0.0)
    assert ret == 0

    # rect (0,0,100,80) translated by (50,30): center=(100,70), extent=(50,40)
    box = rect.get_bounding_box()
    assert _is_close(box["center"][0], 100), f"center_x: {box['center'][0]} != 100"
    assert _is_close(box["center"][1], 70), f"center_y: {box['center'][1]} != 70"
    assert _is_close(box["extent"][0], 50), f"extent_x: {box['extent'][0]} != 50"
    assert _is_close(box["extent"][1], 40), f"extent_y: {box['extent'][1]} != 40"

    # group and canvas should have the same bounding box as the rect
    gbox = group.get_bounding_box()
    assert _is_close(gbox["center"][0], 100), f"group center_x: {gbox['center'][0]} != 100"
    assert _is_close(gbox["center"][1], 70), f"group center_y: {gbox['center'][1]} != 70"
    assert _is_close(gbox["extent"][0], 50), f"group extent_x: {gbox['extent'][0]} != 50"
    assert _is_close(gbox["extent"][1], 40), f"group extent_y: {gbox['extent'][1]} != 40"

    cbox = canvas.get_bounding_box()
    assert _is_close(cbox["center"][0], 100), f"canvas center_x: {cbox['center'][0]} != 100"
    assert _is_close(cbox["center"][1], 70), f"canvas center_y: {cbox['center'][1]} != 70"
    assert _is_close(cbox["extent"][0], 50), f"canvas extent_x: {cbox['extent'][0]} != 50"
    assert _is_close(cbox["extent"][1], 40), f"canvas extent_y: {cbox['extent'][1]} != 40"

    pos = group.get_global_position()
    assert _is_close(pos[0], 50), f"position_x: {pos[0]} != 50"
    assert _is_close(pos[1], 30), f"position_y: {pos[1]} != 30"

    ctx.set_scene(None)


def api_bounding_box(width=256, height=256):
    """Test the bounding box API with cascading 2D transforms"""
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=_backend,
        )
    )
    assert ret == 0

    fill = ngl.ColorFill(color=(1.0, 0.5, 0.0, 1.0))
    rect = ngl.DrawRect2D(rect=(0, 0, 100, 80), fill=fill, label="rect")
    group = ngl.Group2D(children=[rect], translate=(50, 30), label="group")
    canvas = ngl.Canvas2D(children=[group], width=width, height=height)

    scene = ngl.Scene.from_params(canvas, width=width, height=height)
    ret = ctx.set_scene(scene)
    assert ret == 0

    ret = ctx.draw(0.0)
    assert ret == 0

    def check_bbox(node, expected_center, expected_extent):
        box = node.get_bounding_box()
        assert _is_close(box["center"][0], expected_center[0]), f"center_x: {box['center'][0]} != {expected_center[0]}"
        assert _is_close(box["center"][1], expected_center[1]), f"center_y: {box['center'][1]} != {expected_center[1]}"
        assert _is_close(box["extent"][0], expected_extent[0]), f"extent_x: {box['extent'][0]} != {expected_extent[0]}"
        assert _is_close(box["extent"][1], expected_extent[1]), f"extent_y: {box['extent'][1]} != {expected_extent[1]}"

    # DrawRect2D: translated by (50,30), so center = (50+50, 30+40) = (100, 70), extent = (50, 40)
    check_bbox(rect, (100, 70), (50, 40))

    # Group2D: same as its only child
    check_bbox(group, (100, 70), (50, 40))

    # Canvas2D: same as its only child
    check_bbox(canvas, (100, 70), (50, 40))

    ctx.set_scene(None)


def api_bounding_box_drawrect(width=256, height=256):
    """Test the bounding box API with a scaled DrawRect2D"""
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=_backend,
        )
    )
    assert ret == 0

    fill = ngl.ColorFill(color=(1.0, 0.0, 0.0, 1.0))
    rect = ngl.DrawRect2D(rect=(0, 0, 100, 80), fill=fill, label="rect")
    group = ngl.Group2D(children=[rect], scale=(2.0, 0.5), anchor=(0, 0))
    canvas = ngl.Canvas2D(children=[group], width=width, height=height)

    scene = ngl.Scene.from_params(canvas, width=width, height=height)
    ret = ctx.set_scene(scene)
    assert ret == 0

    ret = ctx.draw(0.0)
    assert ret == 0

    # rect (0,0,100,80) scaled by (2.0, 0.5) around anchor (0,0)
    # center = (100*2/2, 80*0.5/2) = (100, 20), extent = (100, 20)
    box = rect.get_bounding_box()
    assert _is_close(box["center"][0], 100), f"center_x: {box['center'][0]} != 100"
    assert _is_close(box["center"][1], 20), f"center_y: {box['center'][1]} != 20"
    assert _is_close(box["extent"][0], 100), f"extent_x: {box['extent'][0]} != 100"
    assert _is_close(box["extent"][1], 20), f"extent_y: {box['extent'][1]} != 20"

    # Group2D and Canvas should have the same AABB
    gbox = group.get_bounding_box()
    assert _is_close(gbox["center"][0], 100), f"group center_x: {gbox['center'][0]} != 100"
    assert _is_close(gbox["center"][1], 20), f"group center_y: {gbox['center'][1]} != 20"
    assert _is_close(gbox["extent"][0], 100), f"group extent_x: {gbox['extent'][0]} != 100"
    assert _is_close(gbox["extent"][1], 20), f"group extent_y: {gbox['extent'][1]} != 20"

    cbox = canvas.get_bounding_box()
    assert _is_close(cbox["center"][0], 100), f"canvas center_x: {cbox['center'][0]} != 100"
    assert _is_close(cbox["center"][1], 20), f"canvas center_y: {cbox['center'][1]} != 20"
    assert _is_close(cbox["extent"][0], 100), f"canvas extent_x: {cbox['extent'][0]} != 100"
    assert _is_close(cbox["extent"][1], 20), f"canvas extent_y: {cbox['extent'][1]} != 20"

    ctx.set_scene(None)


def api_bounding_box_multiple_children(width=256, height=256):
    """Test the bounding box union with multiple children"""
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=_backend,
        )
    )
    assert ret == 0

    fill = ngl.ColorFill(color=(1.0, 0.0, 0.0, 1.0))
    # Two non-overlapping rects:
    # r0 at (10, 20, 40, 30) → center=(30, 35), extent=(20, 15), spans x=[10,50], y=[20,50]
    # r1 at (100, 80, 60, 40) → center=(130, 100), extent=(30, 20), spans x=[100,160], y=[80,120]
    r0 = ngl.DrawRect2D(rect=(10, 20, 40, 30), fill=fill, label="r0")
    r1 = ngl.DrawRect2D(rect=(100, 80, 60, 40), fill=fill, label="r1")
    group = ngl.Group2D(children=[r0, r1], label="group")
    canvas = ngl.Canvas2D(children=[group], width=width, height=height)

    scene = ngl.Scene.from_params(canvas, width=width, height=height)
    ret = ctx.set_scene(scene)
    assert ret == 0

    ret = ctx.draw(0.0)
    assert ret == 0

    # Individual rects
    box0 = r0.get_bounding_box()
    assert _is_close(box0["center"][0], 30), f"r0 center_x: {box0['center'][0]} != 30"
    assert _is_close(box0["center"][1], 35), f"r0 center_y: {box0['center'][1]} != 35"
    assert _is_close(box0["extent"][0], 20), f"r0 extent_x: {box0['extent'][0]} != 20"
    assert _is_close(box0["extent"][1], 15), f"r0 extent_y: {box0['extent'][1]} != 15"

    box1 = r1.get_bounding_box()
    assert _is_close(box1["center"][0], 130), f"r1 center_x: {box1['center'][0]} != 130"
    assert _is_close(box1["center"][1], 100), f"r1 center_y: {box1['center'][1]} != 100"
    assert _is_close(box1["extent"][0], 30), f"r1 extent_x: {box1['extent'][0]} != 30"
    assert _is_close(box1["extent"][1], 20), f"r1 extent_y: {box1['extent'][1]} != 20"

    # Union: x=[10,160] → center_x=85, extent_x=75; y=[20,120] → center_y=70, extent_y=50
    gbox = group.get_bounding_box()
    assert _is_close(gbox["center"][0], 85), f"group center_x: {gbox['center'][0]} != 85"
    assert _is_close(gbox["center"][1], 70), f"group center_y: {gbox['center'][1]} != 70"
    assert _is_close(gbox["extent"][0], 75), f"group extent_x: {gbox['extent'][0]} != 75"
    assert _is_close(gbox["extent"][1], 50), f"group extent_y: {gbox['extent'][1]} != 50"

    cbox = canvas.get_bounding_box()
    assert _is_close(cbox["center"][0], 85), f"canvas center_x: {cbox['center'][0]} != 85"
    assert _is_close(cbox["center"][1], 70), f"canvas center_y: {cbox['center'][1]} != 70"
    assert _is_close(cbox["extent"][0], 75), f"canvas extent_x: {cbox['extent'][0]} != 75"
    assert _is_close(cbox["extent"][1], 50), f"canvas extent_y: {cbox['extent'][1]} != 50"

    ctx.set_scene(None)


def api_bounding_box_intersection(width=256, height=256):
    """Test the node intersection API with a rotated DrawRect2D"""
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=_backend,
        )
    )
    assert ret == 0

    fill = ngl.ColorFill(color=(1.0, 0.5, 0.0, 1.0))
    rect = ngl.DrawRect2D(rect=(78, 78, 100, 100), fill=fill, label="target")
    canvas = ngl.Canvas2D(children=[rect], width=width, height=height)

    scene = ngl.Scene.from_params(canvas, width=width, height=height)
    ret = ctx.set_scene(scene)
    assert ret == 0

    ret = ctx.draw(0.0)
    assert ret == 0

    # Center of rect (128, 128) should intersect
    nodes = ctx.get_nodes_at_point((128, 128))
    assert len(nodes) > 0, "center of rect should intersect"

    # Inside the rect
    nodes = ctx.get_nodes_at_point((100, 100))
    assert len(nodes) > 0, "inside rect should intersect"

    # Outside the rect
    nodes = ctx.get_nodes_at_point((10, 10))
    assert len(nodes) == 0, "outside rect should not intersect"

    ctx.set_scene(None)


def api_bounding_box_rotation(width=256, height=256):
    """Test the bounding box API with a 90° rotated DrawRect2D"""
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=_backend,
        )
    )
    assert ret == 0

    # 200x100 rect at origin, rotated 90° around its center (100, 50)
    fill = ngl.ColorFill(color=(1.0, 0.0, 0.0, 1.0))
    rect = ngl.DrawRect2D(rect=(0, 0, 200, 100), fill=fill, label="rect")
    group = ngl.Group2D(children=[rect], rotation=90.0, anchor=(100, 50))
    canvas = ngl.Canvas2D(children=[group], width=width, height=height)

    scene = ngl.Scene.from_params(canvas, width=width, height=height)
    ret = ctx.set_scene(scene)
    assert ret == 0

    ret = ctx.draw(0.0)
    assert ret == 0

    # After 90° rotation of a 200x100 rect around its center (100, 50):
    # The AABB should swap width/height: center stays (100, 50), extent becomes (50, 100)
    box = rect.get_bounding_box()
    assert _is_close(box["center"][0], 100), f"center_x: {box['center'][0]} != 100"
    assert _is_close(box["center"][1], 50), f"center_y: {box['center'][1]} != 50"
    assert _is_close(box["extent"][0], 50), f"extent_x: {box['extent'][0]} != 50"
    assert _is_close(box["extent"][1], 100), f"extent_y: {box['extent'][1]} != 100"

    # Group2D and Canvas should have the same AABB
    gbox = group.get_bounding_box()
    assert _is_close(gbox["center"][0], 100), f"group center_x: {gbox['center'][0]} != 100"
    assert _is_close(gbox["extent"][0], 50), f"group extent_x: {gbox['extent'][0]} != 50"
    assert _is_close(gbox["extent"][1], 100), f"group extent_y: {gbox['extent'][1]} != 100"

    ctx.set_scene(None)


def api_bounding_box_intersection_rotated(width=256, height=256):
    """Test hit-testing with a 45° rotated DrawRect2D (diamond shape)"""
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=_backend,
        )
    )
    assert ret == 0

    # 100x100 square centered at (128, 128), rotated 45° → diamond shape
    fill = ngl.ColorFill(color=(1.0, 0.5, 0.0, 1.0))
    rect = ngl.DrawRect2D(rect=(78, 78, 100, 100), fill=fill, label="diamond")
    group = ngl.Group2D(children=[rect], rotation=45.0, anchor=(128, 128))
    canvas = ngl.Canvas2D(children=[group], width=width, height=height)

    scene = ngl.Scene.from_params(canvas, width=width, height=height)
    ret = ctx.set_scene(scene)
    assert ret == 0

    ret = ctx.draw(0.0)
    assert ret == 0

    # Center of diamond should hit
    nodes = ctx.get_nodes_at_point((128, 128))
    assert len(nodes) > 0, "center should intersect"

    # Point along the diagonal (still inside the rotated square)
    nodes = ctx.get_nodes_at_point((128, 80))
    assert len(nodes) > 0, "along diamond axis should intersect"

    # Inside the AABB but outside the rotated square
    nodes = ctx.get_nodes_at_point((60, 80))
    assert len(nodes) == 0, "inside AABB but outside rotated square should not intersect"

    # Outside everything
    nodes = ctx.get_nodes_at_point((10, 10))
    assert len(nodes) == 0, "far outside should not intersect"

    ctx.set_scene(None)


def _decompose_transform(matrix):
    """Decompose a 2D TRS 4x4 column-major matrix into position, rotation, scale"""
    # Column-major: m[0],m[1] = first column, m[4],m[5] = second column, m[12],m[13] = translation
    sx = math.sqrt(matrix[0] ** 2 + matrix[1] ** 2)
    sy = math.sqrt(matrix[4] ** 2 + matrix[5] ** 2)
    rotation = math.degrees(math.atan2(matrix[1], matrix[0]))
    position = (matrix[12], matrix[13])
    return position, rotation, (sx, sy)


def api_transform_matrix(width=256, height=256):
    """Test the transform matrix API with translate + rotation + scale"""
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=_backend,
        )
    )
    assert ret == 0

    fill = ngl.ColorFill(color=(1.0, 0.0, 0.0, 1.0))
    rect = ngl.DrawRect2D(rect=(0, 0, 100, 80), fill=fill, label="rect")
    group = ngl.Group2D(
        children=[rect],
        translate=(50, 30),
        rotation=45.0,
        scale=(2.0, 0.5),
        anchor=(0, 0),
        label="group",
    )
    canvas = ngl.Canvas2D(children=[group], width=width, height=height)

    scene = ngl.Scene.from_params(canvas, width=width, height=height)
    ret = ctx.set_scene(scene)
    assert ret == 0

    ret = ctx.draw(0.0)
    assert ret == 0

    # Canvas transform should be identity
    m = canvas.get_global_transform_matrix()
    assert _is_close(m[0], 1) and _is_close(m[5], 1), f"canvas should be identity: {m}"

    # Group2D: translate=(50,30), rotation=45°, scale=(2.0, 0.5), anchor=(0,0)
    m = group.get_global_transform_matrix()
    position, rotation, scale = _decompose_transform(m)
    assert _is_close(position[0], 50), f"group position_x: {position[0]} != 50"
    assert _is_close(position[1], 30), f"group position_y: {position[1]} != 30"
    assert _is_close(rotation, 45), f"group rotation: {rotation} != 45"
    assert _is_close(scale[0] * 10, 20), f"group scale_x: {scale[0]} != 2.0"
    assert _is_close(scale[1] * 10, 5), f"group scale_y: {scale[1]} != 0.5"

    # DrawRect2D: same accumulated transform as Group2D (DrawRect2D has no own TRS)
    m_rect = rect.get_global_transform_matrix()
    position_r, rotation_r, scale_r = _decompose_transform(m_rect)
    assert _is_close(position_r[0], 50), f"rect position_x: {position_r[0]} != 50"
    assert _is_close(rotation_r, 45), f"rect rotation: {rotation_r} != 45"
    assert _is_close(scale_r[0] * 10, 20), f"rect scale_x: {scale_r[0]} != 2.0"

    ctx.set_scene(None)


def api_transform_matrix_nested(width=256, height=256):
    """Test the transform matrix API with nested Group2D transforms"""
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=_backend,
        )
    )
    assert ret == 0

    fill = ngl.ColorFill(color=(1.0, 0.0, 0.0, 1.0))
    rect = ngl.DrawRect2D(rect=(0, 0, 50, 50), fill=fill, label="rect")

    # Inner group: translate by (10, 20)
    inner = ngl.Group2D(children=[rect], translate=(10, 20), label="inner")
    # Outer group: translate by (100, 50)
    outer = ngl.Group2D(children=[inner], translate=(100, 50), label="outer")
    canvas = ngl.Canvas2D(children=[outer], width=width, height=height)

    scene = ngl.Scene.from_params(canvas, width=width, height=height)
    ret = ctx.set_scene(scene)
    assert ret == 0

    ret = ctx.draw(0.0)
    assert ret == 0

    # Outer group: position should be (100, 50)
    m = outer.get_global_transform_matrix()
    position, _, _ = _decompose_transform(m)
    assert _is_close(position[0], 100), f"outer position_x: {position[0]} != 100"
    assert _is_close(position[1], 50), f"outer position_y: {position[1]} != 50"

    # Inner group: accumulated position should be (110, 70)
    m = inner.get_global_transform_matrix()
    position, _, _ = _decompose_transform(m)
    assert _is_close(position[0], 110), f"inner position_x: {position[0]} != 110"
    assert _is_close(position[1], 70), f"inner position_y: {position[1]} != 70"

    # DrawRect2D: same accumulated transform as inner group
    m = rect.get_global_transform_matrix()
    position, _, _ = _decompose_transform(m)
    assert _is_close(position[0], 110), f"rect position_x: {position[0]} != 110"
    assert _is_close(position[1], 70), f"rect position_y: {position[1]} != 70"

    ctx.set_scene(None)


def api_param_get(width=256, height=256):
    """Test the ngl_node_param_get_* API via generated getter methods"""
    fill = ngl.ColorFill(color=(0.8, 0.2, 0.1, 1.0))
    rect = ngl.DrawRect2D(rect=(10, 20, 100, 80), fill=fill)

    # vec4 getter
    assert rect.get_rect() == (10.0, 20.0, 100.0, 80.0)

    # f32 getter (default value)
    assert rect.get_rotation() == 0.0

    # Group2D with explicit values
    group = ngl.Group2D(children=[rect], rotation=45.0, opacity=0.5)
    assert group.get_rotation() == 45.0
    assert group.get_opacity() == 0.5

    # vec2 getter
    group2 = ngl.Group2D(children=[rect], translate=(100, 200), scale=(2.0, 0.5))
    assert group2.get_translate() == (100.0, 200.0)
    assert group2.get_scale() == (2.0, 0.5)

    # i32 getter
    canvas = ngl.Canvas2D(children=[rect], width=320, height=240)
    assert canvas.get_width() == 320
    assert canvas.get_height() == 240

    # str getter (label is a base node param)
    labeled = ngl.DrawRect2D(rect=(0, 0, 10, 10), fill=fill, label="my-rect")
    assert labeled.get_label() == "my-rect"

    # Live-changed value round-trips
    group.set_rotation(90.0)
    assert group.get_rotation() == 90.0


def api_transform_matrix_rotation_stability(width=256, height=256):
    """Test that rotation round-trips through the transform matrix for all angles"""
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=_backend,
        )
    )
    assert ret == 0

    fill = ngl.ColorFill(color=(1.0, 0.0, 0.0, 1.0))
    rect = ngl.DrawRect2D(rect=(0, 0, 100, 80), fill=fill, label="rect")
    group = ngl.Group2D(children=[rect], anchor=(0, 0), label="group")
    canvas = ngl.Canvas2D(children=[group], width=width, height=height)

    scene = ngl.Scene.from_params(canvas, width=width, height=height)
    ret = ctx.set_scene(scene)
    assert ret == 0

    for angle in range(-180, 181, 15):
        group.set_rotation(float(angle))
        ret = ctx.draw(0.0)
        assert ret == 0

        rotation = group.get_global_rotation()
        # Compare using angular difference (handles ±180° wrap)
        diff = (rotation - angle + 180) % 360 - 180
        assert abs(diff) < 0.1, f"angle={angle}: got rotation={rotation:.2f} (diff={diff:.2f})"


def api_global_transform_getters(width=256, height=256):
    """Test the convenience getters for global position, rotation, scale"""
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=_backend,
        )
    )
    assert ret == 0

    fill = ngl.ColorFill(color=(1.0, 0.0, 0.0, 1.0))
    rect = ngl.DrawRect2D(rect=(0, 0, 100, 80), fill=fill, label="rect")
    group = ngl.Group2D(
        children=[rect],
        translate=(50, 30),
        rotation=30.0,
        scale=(2.0, 0.5),
        anchor=(0, 0),
        label="group",
    )
    canvas = ngl.Canvas2D(children=[group], width=width, height=height)

    scene = ngl.Scene.from_params(canvas, width=width, height=height)
    ret = ctx.set_scene(scene)
    assert ret == 0

    ret = ctx.draw(0.0)
    assert ret == 0

    # Canvas2D: identity transform
    pos = canvas.get_global_position()
    rot = canvas.get_global_rotation()
    scl = canvas.get_global_scale()
    assert _is_close(pos[0], 0) and _is_close(pos[1], 0), f"canvas position: {pos}"
    assert _is_close(rot, 0), f"canvas rotation: {rot}"
    assert _is_close(scl[0], 1) and _is_close(scl[1], 1), f"canvas scale: {scl}"

    # Group2D: translate=(50,30), rotation=30, scale=(2.0, 0.5)
    pos = group.get_global_position()
    rot = group.get_global_rotation()
    scl = group.get_global_scale()
    assert _is_close(pos[0], 50), f"group position_x: {pos[0]} != 50"
    assert _is_close(pos[1], 30), f"group position_y: {pos[1]} != 30"
    assert _is_close(rot, 30), f"group rotation: {rot} != 30"
    assert _is_close(scl[0] * 10, 20), f"group scale_x: {scl[0]} != 2.0"
    assert _is_close(scl[1] * 10, 5), f"group scale_y: {scl[1]} != 0.5"

    # DrawRect2D: inherits group transform (no own TRS)
    pos = rect.get_global_position()
    rot = rect.get_global_rotation()
    scl = rect.get_global_scale()
    assert _is_close(pos[0], 50), f"rect position_x: {pos[0]} != 50"
    assert _is_close(pos[1], 30), f"rect position_y: {pos[1]} != 30"
    assert _is_close(rot, 30), f"rect rotation: {rot} != 30"
    assert _is_close(scl[0] * 10, 20), f"rect scale_x: {scl[0]} != 2.0"

    ctx.set_scene(None)
