import pynopegl as ngl


@ngl.scene(width=360, height=360)
def simple(cfg: ngl.SceneCfg):
    keyframes = [
        ngl.PathKeyMove(to=(-0.7, 0.0, 0.0)),  # starting point
        ngl.PathKeyBezier3(
            control1=(-0.2, -0.9, 0.0),  # first control point
            control2=(0.2, 0.8, 0.0),  # 2nd control point
            to=(0.8, 0.0, 0.0),  # final coordinate
        ),
    ]
    path = ngl.Path(keyframes)
    return ngl.DrawPath(path)
