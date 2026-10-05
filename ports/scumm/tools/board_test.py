# SPDX-FileCopyrightText: 2026 Mikey Sklar
# SPDX-License-Identifier: GPL-3.0-or-later
"""On-board smoke test for scumm.mpy, run from the REPL (no launcher change).

Copy scumm.mpy and emu_scumm.py into /emus (next to fjarcade.py), put a game
folder somewhere, edit GAME below and paste or exec this file. It opens the
picodvi framebuffer itself, runs FRAMES frames without pacing, prints the
time per frame and memory, and dumps the screen as BEGIN_FB/END_FB at
SHOT (tools/fb2png.py makes a PNG of it).
"""
import gc
import sys
import time

sys.path.insert(0, "/emus")

GAME = globals().get("GAME", "/sd/scumm/loom-dos-long-demo-en")
FRAMES = globals().get("FRAMES", 1200)
SHOT = globals().get("SHOT", 1200)
CLICKS = globals().get("CLICKS", {})  # frame -> (x, y)
SAVE_AT = globals().get("SAVE_AT", 0)  # quick save to slot 1 at this frame
LOAD_AT = globals().get("LOAD_AT", 0)  # quick load from slot 1 at this frame

import board  # noqa: E402
import displayio  # noqa: E402
import picodvi  # noqa: E402
import storage  # noqa: E402

try:
    storage.getmount("/sd")
except OSError:
    import busio
    import sdcardio

    _spi = busio.SPI(board.SD_SCK, board.SD_MOSI, board.SD_MISO)
    storage.mount(storage.VfsFat(sdcardio.SDCard(_spi, board.SD_CS, baudrate=25_000_000)), "/sd")

displayio.release_displays()
fb = picodvi.Framebuffer(
    320, 240, color_depth=16, clk_dp=board.CKP, clk_dn=board.CKN, red_dp=board.D0P,
    red_dn=board.D0N, green_dp=board.D1P, green_dn=board.D1N, blue_dp=board.D2P, blue_dn=board.D2N,
)

import fjarcade as fj  # noqa: E402

video = fj.Video(fb, 16)
gc.collect()
print("free before import", gc.mem_free())
t = time.monotonic_ns()
import scumm  # noqa: E402

print("import scumm %.0f ms, free %d" % ((time.monotonic_ns() - t) / 1e6, gc.mem_free()))
import emu_scumm  # noqa: E402

t = time.monotonic_ns()
emu = emu_scumm.Emu(GAME, b"", video, fj.Config(GAME + "/x"))
print("Emu() %.0f ms, free %d" % ((time.monotonic_ns() - t) / 1e6, gc.mem_free()))

worst = 0  # frame 1 (start-up loading) is left out
worst_at = 0
late = 0
total = 0
t_all = time.monotonic_ns()
for n in range(1, FRAMES + 1):
    c = CLICKS.get(n)
    if c:
        scumm.input(c[0], c[1], scumm.BTN_LEFT)
    elif n - 3 in CLICKS:
        scumm.input(160, 100, 0)
    if n == SAVE_AT:
        scumm.save(1)
    if n == LOAD_AT:
        scumm.load(1)
    t0 = time.monotonic_ns()
    if not emu.frame():
        print("stopped at frame", n, emu.error())
        break
    dt = time.monotonic_ns() - t0
    total += dt
    if dt > worst and n > 1:
        worst, worst_at = dt, n
    if dt > 16_666_667:
        late += 1
    if n % 300 == 0:
        print("frame %d: avg %.2f ms, worst %.1f ms at %d, mem %s" % (
            n, total / n / 1e6, worst / 1e6, worst_at, scumm.mem()))
    if n == SHOT:
        video.screenshot()
print("%d frames in %.1f s wall; engine avg %.2f ms/frame, worst %.1f ms (frame %d), %d over 16.7 ms" % (
    n, (time.monotonic_ns() - t_all) / 1e9, total / n / 1e6, worst / 1e6, worst_at, late))
print("arena used/peak/stack", scumm.mem(), "gc free", gc.mem_free())
