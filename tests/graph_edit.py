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
from dataclasses import dataclass

import pynopegl as ngl
from pynopegl_utils.misc import get_backend, load_media

_backend_str = os.environ.get("BACKEND")
_backend = get_backend(_backend_str) if _backend_str else ngl.Backend.AUTO


def graph_edit_sync_operation(width=32, height=32):
    """An edit is a synchronous operation and fully applied when the call returns"""
    rect1 = ngl.DrawRect2D(rect=(0, 0, width, height), fill=ngl.ColorPaint())
    group = ngl.Group2D(children=[rect1])
    canvas = ngl.Canvas2D(children=[group], width=width, height=height)
    scene = ngl.Scene.from_params(canvas, width=width, height=height)

    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=16,
            height=16,
            backend=_backend,
        )
    )
    assert ret == 0
    assert ctx.set_scene(scene) == 0
    assert ctx.draw(0) == 0

    assert scene.files == []
    filename = load_media("hamster").filename
    texture = ngl.Texture2D(data_src=ngl.Media(filename=filename))
    rect2 = ngl.DrawRect2D(
        rect=(0, 0, width, height),
        fill=ngl.TexturePaint(texture=texture),
    )
    assert group.add_children(rect2) == 0
    assert scene.files == [filename]

    assert group.remove_children(rect2) == 0
    assert scene.files == []

    del ctx
    del scene


@dataclass
class _Clock:
    t: int = -1

    def tick(self) -> int:
        self.t += 1
        return self.t


def graph_edit_group_children_keep_resources(width=64, height=64):
    """Test that a child taken out of the graph keeps its resources and can be put back"""
    clock = _Clock()

    rect1 = ngl.DrawRect2D(rect=(16, 16, 32, 32), fill=ngl.ColorPaint(color=(1.0, 0.25, 0.0, 1.0)))
    group = ngl.Group2D(children=[rect1])
    root = ngl.Canvas2D(children=[group], width=width, height=height)
    scene = ngl.Scene.from_params(root, width=width, height=height)

    capture_buffer = bytearray(width * height * 4)
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(
            offscreen=True,
            width=width,
            height=height,
            backend=_backend,
            capture_buffer=capture_buffer,
        )
    )
    assert ret == 0
    assert ctx.set_scene(scene) == 0
    assert ctx.draw(clock.tick()) == 0

    def output_color():
        o = (height // 2 * width + width // 2) * 4
        return tuple(capture_buffer[o : o + 3])

    initial_color = output_color()
    assert initial_color != (0, 0, 0)

    for _ in range(4):
        assert root.remove_children(group) == 0
        assert ctx.draw(clock.tick()) == 0
        assert output_color() == (0, 0, 0)
        assert root.add_children(group) == 0
        assert ctx.draw(clock.tick()) == 0
        assert output_color() == initial_color

    # Dropping every user reference to a node that holds resources must not free it prematurely
    rect2 = ngl.DrawRect2D(rect=(0, 0, 8, 8), fill=ngl.ColorPaint(color=(0.0, 1.0, 0.0, 1.0)))
    assert group.add_children(rect2) == 0
    assert ctx.draw(clock.tick()) == 0
    assert group.remove_children(rect2) == 0
    assert ctx.draw(clock.tick()) == 0
    assert rect2.holds_resources()
    del rect2

    assert ctx.set_scene(None) == 0
    assert ctx.draw(clock.tick()) == 0
    assert output_color() == (0, 0, 0)
    assert ctx.set_scene(scene) == 0
    assert ctx.draw(clock.tick()) == 0
    assert output_color() == initial_color

    del ctx
    del scene


def graph_edit_release_detached_resources(width=64, height=64):
    """Test reclaiming the resources of nodes taken out of the graph"""
    clock = _Clock()

    rect1 = ngl.DrawRect2D(rect=(16, 16, 32, 32), fill=ngl.ColorPaint(color=(1.0, 0.25, 0.0, 1.0)))
    group = ngl.Group2D(children=[rect1])
    rect2 = ngl.DrawRect2D(rect=(0, 0, 8, 8), fill=ngl.ColorPaint(color=(0.0, 1.0, 0.0, 1.0)))
    root = ngl.Canvas2D(children=[group, rect2], width=width, height=height)
    scene = ngl.Scene.from_params(root, width=width, height=height)

    capture_buffer = bytearray(width * height * 4)
    ctx = ngl.Context()
    ret = ctx.configure(
        ngl.Config(offscreen=True, width=width, height=height, backend=_backend, capture_buffer=capture_buffer)
    )
    assert ret == 0
    assert ctx.set_scene(scene) == 0
    assert ctx.draw(clock.tick()) == 0

    def output_color():
        o = (height // 2 * width + width // 2) * 4
        return tuple(capture_buffer[o : o + 3])

    initial_color = output_color()
    assert group.holds_resources()
    assert rect2.holds_resources()

    assert root.remove_children(group) == 0
    assert group.holds_resources()
    assert rect1.holds_resources()

    assert ctx.release_detached_resources() == 0
    assert not group.holds_resources()
    assert not rect1.holds_resources()
    assert rect2.holds_resources()
    assert ctx.get_nodes_at_point((width // 2, height // 2)) == []

    assert ctx.draw(clock.tick()) == 0
    assert output_color() == (0, 0, 0)
    assert ctx.draw(clock.tick()) == 0

    assert root.add_children(group) == 0
    assert ctx.draw(clock.tick()) == 0
    assert group.holds_resources()
    assert output_color() == initial_color

    assert ctx.release_detached_resources() == 0
    assert group.holds_resources()
    assert ctx.draw(clock.tick()) == 0
    assert output_color() == initial_color

    del ctx
    del scene
