#!/usr/bin/env python3
"""Deterministic PCM fixtures; no codec tools required."""
import struct
import sys
import wave
import math

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
    out.writeframes(b"".join(struct.pack("<hh", 1 + (i & 0x3fff), 1 + (i >> 14)) for i in range(48000)))

with wave.open(sys.argv[4], "wb") as out:
    out.setparams((1, 2, 48000, 4800, "NONE", "not compressed"))
    out.writeframes(b"".join(struct.pack("<h", 1000 + i) for i in range(4800)))
