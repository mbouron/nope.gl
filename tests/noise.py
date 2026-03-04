#
# Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
# Copyright 2023 Nope Forge
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

import pynopegl as ngl
from pynopegl_utils.tests.cmp_png import test_png

W, H = 320, 320


@test_png(width=W, height=H)
@ngl.scene()
def noise_blocky(cfg: ngl.SceneCfg):
    cfg.aspect_ratio = (W, H)
    fill = ngl.NoiseFill(type="blocky", octaves=3, seed=42)
    return ngl.DrawRect(rect=(0, 0, W, H), fill=fill)


@test_png(width=W, height=H)
@ngl.scene()
def noise_perlin(cfg: ngl.SceneCfg):
    cfg.aspect_ratio = (W, H)
    fill = ngl.NoiseFill(type="perlin", octaves=3, seed=42)
    return ngl.DrawRect(rect=(0, 0, W, H), fill=fill)
