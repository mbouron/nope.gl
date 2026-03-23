import pynopegl as ngl


@ngl.scene(width=360, height=360)
def hello_world(cfg: ngl.SceneCfg):
    return ngl.Text("Hello World!")
