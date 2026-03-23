import pynopegl as ngl


@ngl.scene(width=360, height=360)
def animated_scale(cfg: ngl.SceneCfg):
    cfg.duration = 3

    animkf = [
        ngl.AnimKeyFrameVec3(0, (1 / 3, 1 / 3, 1 / 3)),
        ngl.AnimKeyFrameVec3(cfg.duration / 2, (3 / 4, 3 / 4, 3 / 4)),
        ngl.AnimKeyFrameVec3(cfg.duration, (1 / 3, 1 / 3, 1 / 3)),
    ]

    scene = ngl.DrawColor(geometry=ngl.Circle(0.5))
    return ngl.Scale(scene, factors=ngl.AnimatedVec3(animkf))
