from OpenGL import GL

from pynodegl import (
        AnimKeyFrameVec3,
        AnimationVec3,
        Camera,
        GLState,
        Group,
        Media,
        Program,
        Quad,
        Render,
        RenderToTexture,
        Rotate,
        Texture,
        UniformVec3,
)

from pynodegl_utils.misc import scene

def _get_cube_quads():
            # corner             width        height      color
    return (((-0.5, -0.5,  0.5), ( 1, 0,  0), (0, 1,  0), (1, 1, 0)), # front
            (( 0.5, -0.5, -0.5), (-1, 0,  0), (0, 1,  0), (0, 0, 1)), # back
            ((-0.5, -0.5, -0.5), ( 0, 0,  1), (0, 1,  0), (0, 1, 0)), # left
            (( 0.5, -0.5,  0.5), ( 0, 0, -1), (0, 1,  0), (0, 1, 1)), # right
            ((-0.5, -0.5, -0.5), ( 1, 0,  0), (0, 0,  1), (1, 0, 0)), # bottom
            ((-0.5,  0.5,  0.5), ( 1, 0,  0), (0, 0, -1), (1, 0, 1))) # top

def _get_cube_side(texture, program, corner, width, height, color):
    ts = Render(Quad(corner, width, height), program)
    ts.update_textures(tex0=texture)
    ts.update_uniforms(blend_color=UniformVec3(value=color))
    return ts

@scene()
def rotating_cube(cfg):
    cube = Group(name="cube")
    cube.add_glstates(GLState(GL.GL_DEPTH_TEST, GL.GL_TRUE))

    frag_data = """
#version 100
precision mediump float;
uniform sampler2D tex0_sampler;
varying vec2 var_texcoord;
uniform vec3 blend_color;
void main(void)
{
    vec4 t = texture2D(tex0_sampler, var_texcoord);
    gl_FragColor = vec4(mix(t.rgb, blend_color, 0.2), 1.0);
}"""
    p = Program(fragment=frag_data)

    t = Texture(data_src=Media(cfg.medias[0].filename))
    cube_quads_info = _get_cube_quads()
    children = [_get_cube_side(t, p, qi[0], qi[1], qi[2], qi[3]) for qi in _get_cube_quads()]
    cube.add_children(*children)

    rot_animkf = AnimationVec3([AnimKeyFrameVec3(0, (0, 0, 0)),
                                AnimKeyFrameVec3(cfg.duration, (360, 360*2, 360*3))])

    rot = Rotate(cube, anim=rot_animkf)

    camera = Camera(rot)
    camera.set_eye(0.0, 0.0, 2.0)
    camera.set_up(0.0, 1.0, 0.0)
    camera.set_perspective(45.0, cfg.aspect_ratio, 1.0, 10.0)

    return camera

@scene()
def scene_with_framebuffer(cfg):
    g = Group()

    d = Texture()
    d.set_format(GL.GL_DEPTH_COMPONENT)
    d.set_internal_format(GL.GL_DEPTH_COMPONENT)
    d.set_type(GL.GL_FLOAT)
    d.set_width(640)
    d.set_height(480)

    t = Texture()
    t.set_width(640)
    t.set_height(480)
    rtt = RenderToTexture(rotating_cube(cfg), t)
    rtt.set_depth_texture(d)

    q = Quad((-1.0, -1.0, 0), (1, 0, 0), (0, 1, 0))
    p = Program()
    render = Render(q, p)
    render.update_textures(tex0=t)
    g.add_children(rtt, render)

    q = Quad((0.0, 0.0, 0), (1, 0, 0), (0, 1, 0))
    p = Program()
    render = Render(q, p)
    render.update_textures(tex0=d)
    g.add_children(rtt, render)

    return g
