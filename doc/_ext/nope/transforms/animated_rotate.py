import pynopegl as ngl


@ngl.scene(width=360, height=360)
def animated_rotate(cfg: ngl.SceneCfg):
    cfg.duration = 3

    animkf = [
        ngl.AnimKeyFrameFloat(0, 0),
        ngl.AnimKeyFrameFloat(cfg.duration, 360),
    ]

    scene = ngl.DrawColor(geometry=ngl.Quad())
    return ngl.Rotate(scene, angle=ngl.AnimatedFloat(animkf))
