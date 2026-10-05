# SPDX-FileCopyrightText: 2026 Mikey Sklar
# SPDX-License-Identifier: GPL-3.0-or-later
"""Paced on-board run of a SCUMM game through the launcher's own pieces.

Like fjarcade.run() (audio-paced, real DAC output), but scripted: CLICKS
press the left button at a game position on given frames, and it reports
per 5 s window the fps, emu ms average and worst, and how often the loop
fell more than half an audio buffer behind the DAC (an audible dropout;
fjarcade re-anchors then). SHOTS dump the screen as BEGIN_FB blocks
(tools/fb2png.py); the window holding a shot is slow because of it.

Run from the raw REPL (tools/rawrun.py) after stopping the launcher. Set
CART, FRAMES, CLICKS, SHOTS as globals first to override the defaults.
"""
import gc
import sys
import time

sys.path.insert(0, "/emus")

CART = globals().get("CART", "/sd/cart/loompce.scumm")
FRAMES = globals().get("FRAMES", 9000)
CLICKS = globals().get("CLICKS", {200: (160, 75)})  # frame -> (x, y)
SHOTS = globals().get("SHOTS", (1080, 2880))
VERBOSE = globals().get("VERBOSE", False)  # print each time it falls behind

import board  # noqa: E402
import displayio  # noqa: E402
import picodvi  # noqa: E402

displayio.release_displays()
fb = picodvi.Framebuffer(
    320, 240, color_depth=16, clk_dp=board.CKP, clk_dn=board.CKN, red_dp=board.D0P,
    red_dn=board.D0N, green_dp=board.D1P, green_dn=board.D1N, blue_dp=board.D2P,
    blue_dn=board.D2N)

import fjarcade as fj  # noqa: E402
import scumm  # noqa: E402
import emu_scumm  # noqa: E402

video = fj.Video(fb, 16)
pad = fj.Pad()
cfg = fj.Config(CART)
gc.collect()
emu = emu_scumm.Emu(CART, fj.read_rom(CART), video, cfg)
gc.collect()
print("gc free after init", gc.mem_free())

pad.release_onboard()
audio = fj.Audio(emu.AUDIO_RATE, emu.AUDIO_CHANNELS, emu.audio_half(), cfg)
pad.claim_onboard()
audio.start()
if audio.ring is not None:
    emu.set_audio(audio.ring, audio.half)
t_anchor = audio.t_play
s0 = emu.samples()
half_ns = audio.half * 1_000_000_000 // audio.rate

frames = 0
t_emu = 0
worst = 0
worst_all = 0
worst_at = 0
behind = 0
behind_all = 0
t_report = t_anchor
held = 0
t_prev = time.monotonic_ns()
gap = 0  # ns from the previous frame's start to this one's
try:
    for total in range(1, FRAMES + 1):
        t0 = time.monotonic_ns()
        gap = t0 - t_prev
        t_prev = t0
        if total in CLICKS:
            held = 3
            cx, cy = CLICKS[total]
        if held:
            scumm.input(cx, cy, scumm.BTN_LEFT)
            held -= 1
        else:
            emu.set_input(0)
        if not emu.frame():
            print("stopped:", emu.error())
            break
        now = time.monotonic_ns()
        dt = now - t0
        t_emu += dt
        if dt > worst:
            worst = dt
        if dt > worst_all:
            worst_all = dt
            worst_at = total
        frames += 1
        if total in SHOTS:
            print("shot at frame", total)
            video.screenshot()
            now = time.monotonic_ns()
        if now - t_report >= 5_000_000_000:
            used, peak, stack = scumm.mem()
            print("f%d fps %.1f  emu %.2f ms avg %.1f worst  behind %d  arena %d peak %d  gc %d" % (
                total, frames * 1e9 / (now - t_report), t_emu / frames / 1e6, worst / 1e6,
                behind, used, peak, gc.mem_free()))
            frames = t_emu = worst = behind = 0
            t_report = now
        due = t_anchor + (emu.samples() - s0) * 1_000_000_000 // audio.rate
        if due > now:
            time.sleep((due - now) / 1e9)
        elif now - due > half_ns:
            if VERBOSE:
                print("  behind at f%d: %.1f ms late, frame %.1f ms, since last frame %.1f ms" % (
                    total, (now - due) / 1e6, dt / 1e6, gap / 1e6))
            behind += 1
            behind_all += 1
            if audio.ring is not None:
                emu.set_audio(audio.ring, audio.resync_point(now))
            t_anchor = now
            s0 = emu.samples()
finally:
    audio.stop()
used, peak, stack = scumm.mem()
print("done: worst frame %.1f ms at %d, fell behind %d times, arena peak %d, stack %d, gc free %d" % (
    worst_all / 1e6, worst_at, behind_all, peak, stack, gc.mem_free()))
