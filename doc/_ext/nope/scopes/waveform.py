from pynopegl_utils.misc import SceneCfg, scene

import pynopegl as ngl


@scene()
def waveform(cfg: SceneCfg):
    jpegs = (m for m in cfg.medias if m.filename.endswith(".jpg"))
    image = next(jpegs)

    stats = ngl.ColorStats(texture=ngl.Texture2D(data_src=ngl.Media(image.filename)))
    return ngl.RenderWaveform(stats, mode="luma_only")