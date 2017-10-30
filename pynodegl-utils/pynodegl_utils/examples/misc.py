import os
import array
import math
import random

from OpenGL import GL

from pynodegl import (
        AnimKeyFrameFloat,
        AnimKeyFrameVec3,
        AnimationFloat,
        AnimationVec3,
        BufferFloat,
        BufferUBVec3,
        BufferVec2,
        BufferVec3,
        Camera,
        Compute,
        ComputeProgram,
        Geometry,
        Group,
        Media,
        Program,
        Quad,
        Render,
        Rotate,
        Texture2D,
        Translate,
        Triangle,
        UniformInt,
        UniformFloat,
        UniformVec4,
)

from pynodegl_utils.misc import scene, get_shader


@scene()
def buffer(cfg):
    # Credits: https://icons8.com/icon/40514/dove
    icon = open(os.path.join(os.path.dirname(__file__), 'data', 'icons8-dove.ppm'))
    header = icon.readline().rstrip()
    w, h = (int(x) for x in icon.readline().rstrip().split())
    depth = int(icon.readline().rstrip())
    assert header == 'P6'
    assert w, h == (80, 80)
    assert depth == 255
    icon_data = icon.read()
    assert len(icon_data) == w * h * 3

    array_data = array.array('B', icon_data)
    img_buf = BufferUBVec3(data=array_data)
    img_tex = Texture2D(data_src=img_buf, width=w, height=h)
    quad = Quad((-.5, -.5, 0), (1, 0, 0), (0, 1, 0))
    prog = Program()
    render = Render(quad, prog)
    render.update_textures(tex0=img_tex)
    return render

@scene(size={'type': 'range', 'range': [0,1.5], 'unit_base': 1000})
def triangle(cfg, size=0.5):
    b = size * math.sqrt(3) / 2.0
    c = size * 1/2.

    triangle = Triangle((-b, -c, 0), (b, -c, 0), (0, size, 0))
    p = Program(fragment=get_shader('triangle'))
    node = Render(triangle, p)
    animkf = [AnimKeyFrameFloat(0, 0),
              AnimKeyFrameFloat(cfg.duration, -360*2)]
    node = Rotate(node, anim=AnimationFloat(animkf))
    return node

@scene(n={'type': 'range', 'range': [2,10]})
def fibo(cfg, n=8):
    cfg.duration = 5.0

    p = Program(fragment=get_shader('color'))

    fib = [0, 1, 1]
    for i in range(2, n):
        fib.append(fib[i] + fib[i-1])
    fib = fib[::-1]

    shift = 1/3. # XXX: what's the exact math here?
    shape_scale = 1. / ((2.-shift) * sum(fib))

    orig = (-shift, -shift, 0)
    g = None
    root = None
    for i, x in enumerate(fib[:-1]):
        w = x * shape_scale
        gray = 1. - i/float(n)
        color = [gray, gray, gray, 1]
        q = Quad(orig, (w, 0, 0), (0, w, 0))
        render = Render(q, p)
        render.update_uniforms(color=UniformVec4(value=color))

        new_g = Group()
        animkf = [AnimKeyFrameFloat(0,               90),
                  AnimKeyFrameFloat(cfg.duration/2, -90, "exp_in_out"),
                  AnimKeyFrameFloat(cfg.duration,    90, "exp_in_out")]
        rot = Rotate(new_g, anchor=orig, anim=AnimationFloat(animkf))
        if g:
            g.add_children(rot)
        else:
            root = rot
        g = new_g
        new_g.add_children(render)
        orig = (orig[0] + w, orig[1] + w, 0)

    root = Camera(root)

    root.set_eye(0.0, 0.0, 2.0)
    root.set_up(0.0, 1.0, 0.0)
    root.set_perspective(45.0, cfg.aspect_ratio, 1.0, 10.0)
    return root

@scene(dim={'type': 'range', 'range': [1,50]})
def cropboard(cfg, dim=15):
    cfg.duration = 10

    kw = kh = 1. / dim
    qw = qh = 2. / dim
    tqs = []

    p = Program()
    m = Media(cfg.medias[0].filename)
    t = Texture2D(data_src=m)

    for y in range(dim):
        for x in range(dim):
            corner = (-1. + x*qw, 1. - (y+1.)*qh, 0)
            q = Quad(corner, (qw, 0, 0), (0, qh, 0))

            q.set_uv_corner(x*kw, 1. - (y+1.)*kh)
            q.set_uv_width(kw, 0)
            q.set_uv_height(0, kh)

            render = Render(q, p)
            render.update_textures(tex0=t)

            startx = random.uniform(-2, 2)
            starty = random.uniform(-2, 2)
            trn_animkf = [AnimKeyFrameVec3(0, (startx, starty, 0)),
                          AnimKeyFrameVec3(cfg.duration*2/3., (0, 0, 0), "exp_out")]
            trn = Translate(render, anim=AnimationVec3(trn_animkf))
            tqs.append(trn)

    return Group(children=tqs)

@scene(freq_precision={'type': 'range', 'range': [1,10]},
       overlay={'type': 'range', 'unit_base': 100})
def audiotex(cfg, freq_precision=7, overlay=0.6):
    media = cfg.medias[0]
    cfg.duration = media.duration

    q = Quad((-1, -1, 0), (2, 0, 0), (0, 2, 0))

    audio_m = Media(media.filename, audio_tex=1)
    audio_tex = Texture2D(data_src=audio_m)

    video_m = Media(media.filename)
    video_tex = Texture2D(data_src=video_m)

    p = Program(fragment=get_shader('audiotex'))
    render = Render(q, p)
    render.update_textures(tex0=audio_tex, tex1=video_tex)
    render.update_uniforms(overlay=UniformFloat(overlay))
    render.update_uniforms(freq_precision=UniformInt(freq_precision))
    return render

@scene(particules={'type': 'range', 'range': [1,1024]})
def particules(cfg, particules=32):
    random.seed(0)

    compute_data_version = "310 es" if cfg.glbackend == "gles" else "430"
    compute_data = '''
#version %s

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (std430, binding = 0) buffer ipositions_buffer {
    vec3 ipositions[];
};

layout (std430, binding = 1) buffer ivelocities_buffer {
    vec2 ivelocities[];
};

layout (std430, binding = 2) buffer opositions_buffer {
    vec3 opositions[];
};

uniform float time;
uniform float duration;

float bounceOut(float t)
{
    float c = 1.0;
    float a = 1.70158;

    if (t >= 1.0) {
        return c;
    } else if (t < 4.0 / 11.0) {
        return c * (7.5625 * t * t);
    } else if (t < 8.0 / 11.0) {
        t -= 6.0 / 11.0;
        return -a * (1.0 - (7.5625 * t * t + 0.75)) + c;
    } else if (t < 10.0 / 11.0) {
        t -= 9.0 / 11.0;
        return -a * (1.0 - (7.5625 * t * t + 0.9375)) + c;
    } else {
        t -= 21.0 / 22.0;
        return -a * (1.0 - (7.5625 * t * t + 0.984375)) + c;
    }
}

float bounce(float t)
{
    return 1.0 - bounceOut(t);
}

void main(void)
{
    uint i = gl_GlobalInvocationID.x +
             gl_GlobalInvocationID.y * 1024U;

    vec3 iposition = ipositions[i];
    vec2 ivelocity = ivelocities[i];
    float step = time * duration * 30.0;
    vec2 velocity = ivelocity;
    vec3 position = iposition;
    float yoffset = 1.0 - iposition.y;
    float speed = 1.0 + ivelocity.y;

    position.x = iposition.x + step * velocity.x;
    position.y = ((2.0 - yoffset) * bounce(time * speed * (1.0  + yoffset))) - 0.99;

    opositions[i] = position;
}
''' % (compute_data_version)

    fragment_data = '''
#version 100

precision highp float;

void main(void)
{
    gl_FragColor = vec4(0.0, 0.6, 0.8, 1.0);
}'''

    cfg.duration = 6

    x = 1024
    p = x * particules

    positions = array.array('f')
    velocities = array.array('f')

    for i in range(p):
        positions.extend([
            random.uniform(-1.0, 1.0),
            random.uniform(0.0, 1.0),
            0.0,
            0.0,
        ])

        velocities.extend([
            random.uniform(-0.01, 0.01),
            random.uniform(-0.05, 0.05),
        ])

    ipositions = BufferVec3()
    ipositions.set_data(positions)
    ipositions.set_stride(4 * 4)
    ivelocities = BufferVec2()
    ivelocities.set_data(velocities)

    opositions = BufferVec3(p)
    opositions.set_stride(4 * 4)

    animkf = [AnimKeyFrameFloat(0, 0),
              AnimKeyFrameFloat(cfg.duration, 1)]
    utime = UniformFloat(anim=AnimationFloat(animkf))
    uduration = UniformFloat(cfg.duration)

    cp = ComputeProgram(compute_data)

    c = Compute(1024, particules, 1, cp)
    c.update_uniforms(
        time=utime,
        duration=uduration,
    )
    c.update_buffers(
        ipositions_buffer=ipositions,
        ivelocities_buffer=ivelocities,
        opositions_buffer=opositions,
    )

    gm = Geometry(opositions)
    gm.set_draw_mode(GL.GL_POINTS)

    m = Media(cfg.medias[0].filename, initial_seek=5)
    p = Program(fragment=get_shader('color'))
    r = Render(gm, p)
    r.update_uniforms(color=UniformVec4(value=(0, .6, .8, 1)))

    g = Group()
    g.add_children(c, r)

    return Camera(g)
