import pynopegl as ngl


@ngl.scene(width=640, height=360)
def noise(cfg: ngl.SceneCfg):
    cfg.duration = 3
    return ngl.DrawNoise(type="perlin", octaves=4, scale=(16, 9), evolution=ngl.Time())
