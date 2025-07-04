import array
import math

import pynopegl as ngl
from pynopegl_utils.misc import get_shader


@ngl.scene(controls=dict(square_color=ngl.scene.Color(), circle_color=ngl.scene.Color()))
def square2circle(cfg: ngl.SceneCfg, square_color=(0.9, 0.1, 0.3), circle_color=(1.0, 1.0, 1.0)):
    """Morphing of a square (composed of many vertices) into a circle"""
    cfg.duration = 5
    cfg.aspect_ratio = (1, 1)

    n = 1024  # number of vertices
    s = 0.625  # shapes scale
    interp = "exp_in_out"

    center_vertex = [0, 0, 0]

    square = lambda x: min(max(4 * abs(2 * ((x + 3 / 4) % 1) - 1) - 2, -1), 1)
    square_vertices = array.array("f", center_vertex)
    for i in range(n):
        x = square(i / n + 1 / 4)
        y = square(i / n)
        square_vertices.extend([x, y, 0])

    circle_vertices = array.array("f", center_vertex)
    for i in range(n):
        angle = i / n * math.tau
        x = math.cos(angle)
        y = math.sin(angle)
        circle_vertices.extend([x, y, 0])

    indices = array.array("H")
    for i in range(1, n + 1):
        indices.extend([0, i, i + 1])
    indices[-1] = 1

    vertices_animkf = [
        ngl.AnimKeyFrameBuffer(0, square_vertices),
        ngl.AnimKeyFrameBuffer(cfg.duration / 2.0, circle_vertices, interp),
        ngl.AnimKeyFrameBuffer(cfg.duration, square_vertices, interp),
    ]
    vertices = ngl.AnimatedBufferVec3(vertices_animkf)

    color_animkf = [
        ngl.AnimKeyFrameColor(0, square_color),
        ngl.AnimKeyFrameColor(cfg.duration / 2.0, circle_color, interp),
        ngl.AnimKeyFrameColor(cfg.duration, square_color, interp),
    ]
    ucolor = ngl.AnimatedColor(color_animkf)

    geom = ngl.Geometry(vertices, indices=ngl.BufferUShort(data=indices))
    p = ngl.Program(vertex=get_shader("color.vert"), fragment=get_shader("color.frag"))
    draw = ngl.Draw(geom, p)
    draw.update_frag_resources(color=ucolor, opacity=ngl.UniformFloat(1))
    return ngl.Scale(draw, factors=(s, s, 1))
