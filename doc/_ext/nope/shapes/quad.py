import pynopegl as ngl


@ngl.scene(width=360, height=360)
def quad(cfg: ngl.SceneCfg):
    return ngl.DrawColor(geometry=ngl.Quad())
