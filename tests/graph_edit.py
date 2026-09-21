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


def graph_edit_group_children_live():
    """Test live group mutations on a configured context"""
    inner = ngl.Group(children=[ngl.DrawColor((1.0, 0.0, 0.0), geometry=ngl.Quad())])
    root = ngl.Group(children=[inner])
    scene = ngl.Scene.from_params(root)

    capture_buffer = bytearray(16 * 16 * 4)
    ctx = ngl.Context()
    assert (
        ctx.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend, capture_buffer=capture_buffer))
        == 0
    )
    assert ctx.set_scene(scene) == 0
    assert ctx.draw(0) == 0
    center = (8 * 16 + 8) * 4
    assert tuple(capture_buffer[center : center + 3]) == (255, 0, 0)

    # Insert and stable move update both the option list and the runtime edge
    # order. Reusing the same timestamp also checks topology invalidation.
    green = ngl.DrawColor((0.0, 1.0, 0.0), geometry=ngl.Quad())
    assert root.insert_children(0, green) == 0
    assert ctx.draw(0) == 0
    assert tuple(capture_buffer[center : center + 3]) == (255, 0, 0)
    assert root.move_children(0, 1) == 0
    assert ctx.draw(0) == 0
    assert tuple(capture_buffer[center : center + 3]) == (0, 255, 0)
    assert root.remove_children(green) == 0

    # Several mutations between two frames are applied in order
    assert root.add_children(green) == 0
    assert root.remove_children(green) == 0
    assert ctx.draw(1) == 0

    # An edit applies on the spot: once inner is removed, editing it is a
    # construction-time edit on a detached sub-tree, which takes effect when it
    # rejoins the graph
    assert root.remove_children(inner) == 0
    assert inner.add_children(ngl.DrawColor((0.0, 0.0, 1.0), geometry=ngl.Quad())) == 0
    assert ctx.draw(2) == 0
    assert root.add_children(inner) == 0
    assert ctx.draw(3) == 0

    # Removing it a second time is an error, reported synchronously
    assert root.remove_children(inner) == 0
    _expect_topology_error(root.remove_children, inner)
    assert ctx.draw(4) == 0

    del ctx
    del scene


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


def _expect_topology_error(call, *args):
    ret = call(*args)
    assert ret < 0, "topology edit unexpectedly succeeded"
    return ngl.Error(ret)


@dataclass
class _Clock:
    t: int = -1

    def tick(self) -> int:
        self.t += 1
        return self.t


def graph_edit_group_reparent():
    """Test moving a child between groups, including invalid usage"""
    child = ngl.DrawColor((1.0, 0.0, 0.0), geometry=ngl.Quad())
    left = ngl.Group(children=[child])
    right = ngl.Group()
    root = ngl.Group(children=[left, right])
    scene = ngl.Scene.from_params(root)

    ctx = ngl.Context()
    assert ctx.configure(ngl.Config(offscreen=True, width=16, height=16, backend=_backend)) == 0
    assert ctx.set_scene(scene) == 0
    assert ctx.draw(0) == 0

    assert left.reparent_child(left, child) == 0

    assert left.reparent_child(right, child) == 0
    assert ctx.draw(1) == 0
    assert right.reparent_child(left, child) == 0
    assert ctx.draw(2) == 0

    _expect_topology_error(right.reparent_child, left, child)
    assert ctx.draw(3) == 0

    del ctx
    del scene


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
