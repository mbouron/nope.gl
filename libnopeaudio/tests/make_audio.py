#!/usr/bin/env python3
#
# Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

"""Deterministic PCM fixtures; no codec tools required."""

import math
import struct
import sys
import wave

with wave.open(sys.argv[1], "wb") as out:
    out.setparams((2, 2, 48000, 4096, "NONE", "not compressed"))
    out.writeframes(b"".join(struct.pack("<hh", 1000 + i, -1000 - i) for i in range(4096)))

with wave.open(sys.argv[2], "wb") as out:
    out.setparams((2, 2, 44100, 4410, "NONE", "not compressed"))
    samples = [round(6000 * math.sin(2 * math.pi * 1000 * i / 44100)) for i in range(4410)]
    out.writeframes(b"".join(struct.pack("<hh", x, -x) for x in samples))

# One second whose frames tell their index, and are never silent: the left
# sample is 1 + the 14 low bits of the index, the right one 1 + the others
with wave.open(sys.argv[3], "wb") as out:
    out.setparams((2, 2, 48000, 48000, "NONE", "not compressed"))
    out.writeframes(b"".join(struct.pack("<hh", 1 + (i & 0x3FFF), 1 + (i >> 14)) for i in range(48000)))

with wave.open(sys.argv[4], "wb") as out:
    out.setparams((1, 2, 48000, 4800, "NONE", "not compressed"))
    out.writeframes(b"".join(struct.pack("<h", 1000 + i) for i in range(4800)))

# Four quiet seconds: a 440 Hz tone at 0.05, its channels with no layout, as in
# most WAV files
with wave.open(sys.argv[5], "wb") as out:
    out.setparams((2, 2, 48000, 192000, "NONE", "not compressed"))
    samples = [round(1638 * math.sin(2 * math.pi * 440 * i / 48000)) for i in range(192000)]
    out.writeframes(b"".join(struct.pack("<hh", x, x) for x in samples))
