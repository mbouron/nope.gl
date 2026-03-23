import pynopegl as ngl


@ngl.scene(width=360, height=360)
def bg_fg_composition(cfg: ngl.SceneCfg):
    bg = ngl.DrawGradient(color0=(0, 0.5, 0.5), color1=(1, 0.5, 0))
    fg = ngl.Text("Hello World!", bg_opacity=0)
    return ngl.Group(children=[bg, fg])
