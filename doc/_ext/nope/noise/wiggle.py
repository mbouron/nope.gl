import pynopegl as ngl


@ngl.scene(width=360, height=360)
def wiggle(cfg: ngl.SceneCfg):
    cfg.duration = 3

    geometry = ngl.Circle(radius=0.25, npoints=6)
    draw = ngl.DrawColor(geometry=geometry)

    # Extend 2-dimensional noise into a vec3 for the Translate node using EvalVec3
    translate = ngl.EvalVec3("wiggle.x", "wiggle.y", "0")
    translate.update_resources(wiggle=ngl.NoiseVec2(octaves=8))

    return ngl.Translate(draw, vector=translate)
