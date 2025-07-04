import pynopegl as ngl
from pynopegl_utils.misc import get_shader, load_media


@ngl.scene()
def animated_uniform(cfg: ngl.SceneCfg):
    """Uniform mat4 animated with a transform chain"""
    m0 = load_media("mire")
    cfg.aspect_ratio = (m0.width, m0.height)

    q = ngl.Quad((-0.5, -0.5, 0), (1, 0, 0), (0, 1, 0))
    m = ngl.Media(m0.filename)
    t = ngl.Texture2D(data_src=m)
    p = ngl.Program(vertex=get_shader("texture.vert"), fragment=get_shader("matrix-transform.frag"))
    p.update_vert_out_vars(var_uvcoord=ngl.IOVec2(), var_tex0_coord=ngl.IOVec2())
    ts = ngl.Draw(q, p)
    ts.update_frag_resources(tex0=t)

    scale_animkf = [
        ngl.AnimKeyFrameVec3(0, (1, 1, 1)),
        ngl.AnimKeyFrameVec3(cfg.duration, (0.1, 0.1, 0.1), "quartic_out"),
    ]
    s = ngl.Scale(ngl.Identity(), factors=ngl.AnimatedVec3(scale_animkf))

    rotate_animkf = [ngl.AnimKeyFrameFloat(0, 0), ngl.AnimKeyFrameFloat(cfg.duration, 360, "exp_out")]
    r = ngl.Rotate(s, axis=(0, 0, 1), angle=ngl.AnimatedFloat(rotate_animkf))

    u = ngl.UniformMat4(transform=r)
    ts.update_frag_resources(matrix=u)

    return ts


@ngl.scene(controls=dict(rotate=ngl.scene.Bool()))
def animated_camera(cfg: ngl.SceneCfg, rotate=True):
    """Animated camera around a scene"""
    g = ngl.Group()

    q = ngl.Quad((-0.5, -0.5, 0), (1, 0, 0), (0, 1, 0))
    media = load_media("mire")
    m = ngl.Media(media.filename)
    t = ngl.Texture2D(data_src=m)
    node = ngl.DrawTexture(t, geometry=q)
    g.add_children(node)

    translate = ngl.Translate(node, vector=(-0.6, 0.8, -1))
    g.add_children(translate)

    translate = ngl.Translate(node, vector=(0.6, 0.8, -1))
    g.add_children(translate)

    translate = ngl.Translate(node, vector=(-0.6, -0.5, -1))
    g.add_children(translate)

    translate = ngl.Translate(node, vector=(0.6, -0.5, -1))
    g.add_children(translate)

    g = ngl.GraphicConfig(g, depth_test=True)
    camera = ngl.Camera(g)
    camera.set_eye(0, 0, 2)
    camera.set_center(0.0, 0.0, 0.0)
    camera.set_up(0.0, 1.0, 0.0)
    camera.set_clipping(0.1, 10.0)

    tr_animkf = [ngl.AnimKeyFrameVec3(0, (0.0, 0.0, 0.0)), ngl.AnimKeyFrameVec3(10, (0.0, 0.0, 3.0), "exp_out")]
    node = ngl.Translate(ngl.Identity(), vector=ngl.AnimatedVec3(tr_animkf))

    if rotate:
        rot_animkf = [ngl.AnimKeyFrameFloat(0, 0), ngl.AnimKeyFrameFloat(cfg.duration, 360, "exp_out")]
        node = ngl.Rotate(node, axis=(0, 1, 0), angle=ngl.AnimatedFloat(rot_animkf))

    camera.set_eye_transform(node)

    perspective_animkf = [
        ngl.AnimKeyFrameVec2(0.5, (60.0, cfg.aspect_ratio_float)),
        ngl.AnimKeyFrameVec2(cfg.duration, (45.0, cfg.aspect_ratio_float), "exp_out"),
    ]
    camera.set_perspective(ngl.AnimatedVec2(perspective_animkf))

    return camera
