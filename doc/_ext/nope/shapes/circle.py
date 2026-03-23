import pynopegl as ngl


@ngl.scene(width=360, height=360)
def circle(cfg: ngl.SceneCfg):
    return ngl.DrawColor(geometry=ngl.Circle(radius=0.7, npoints=64))
