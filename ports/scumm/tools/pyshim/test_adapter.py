# SPDX-FileCopyrightText: 2026 Mikey Sklar
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run emu_scumm.py on CPython against the host core, driving it with pad
masks the way fjarcade.run() does, and write the framebuffer as a PPM.

    python3 tools/pyshim/test_adapter.py GAME_PATH [frames] [out.ppm]
"""
import array
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
sys.path.insert(1, os.path.join(here, "..", ".."))

import fjarcade as fj  # noqa: E402
import emu_scumm  # noqa: E402

path = sys.argv[1]
frames = int(sys.argv[2]) if len(sys.argv) > 2 else 600
out = sys.argv[3] if len(sys.argv) > 3 else "/tmp/fjscumm.ppm"
video = fj.Video(16)
emu = emu_scumm.Emu(path, open(path, "rb").read() if os.path.isfile(path) else b"", video, fj.Config())
ring = array.array("h", bytes(4 * 2 * emu.audio_half()))
emu.set_audio(ring, emu.audio_half())

# Hold right for 40 frames, click A, press C (Escape), then Start + Coin
# (which must not send keys), then Coin alone (F5).
script = {}
for f in range(100, 140):
    script[f] = fj.RIGHT
for f in range(150, 153):
    script[f] = fj.A
script[200] = fj.C
for f in range(300, 310):
    script[f] = fj.START | fj.SELECT
script[400] = fj.SELECT
for n in range(1, frames + 1):
    emu.set_input(script.get(n, 0))
    if not emu.frame():
        print("stopped:", emu.error())
        break
print("frames", n, "samples", emu.samples(), "pointer", emu.x, emu.y, "mem", emu_scumm.scumm.mem())
with open(out, "wb") as f:
    f.write(b"P6 320 240 255\n")
    px = memoryview(video.fb).cast("H")
    f.write(bytes(c for p in px for c in (((p >> 11) & 31) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3)))
print("wrote", out)
