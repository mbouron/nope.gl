import pynopegl as ngl


@ngl.scene(width=360, height=360)
def rotate(cfg: ngl.SceneCfg):
    scene = ngl.DrawColor(geometry=ngl.Quad())
    return ngl.Rotate(scene, angle=45)
