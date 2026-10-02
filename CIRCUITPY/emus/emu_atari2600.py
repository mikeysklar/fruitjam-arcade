# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Atari 2600 adapter for the launcher (atari2600.mpy). See emu_nes.py for the contract.

The core draws the whole 320x240 framebuffer itself: each 160 pixel TIA line
2x wide, 240 of the ~262 NTSC lines from scanline `top` (cfg, default 12).
Sound is mono, and frame length varies by game, so samples() is the core's
own running count rather than frames times a constant.

Pad (one fire button, the rest are console switches):

  pad              2600
  ---------------  ---------------------------------
  D-pad            joystick
  A (D10)          fire
  B (A2)           fire
  C (A1)           right difficulty, toggles A/B
  Start (D6)       Game Reset
  Start2 (D7)      left difficulty, toggles A/B
  Coin (A5)        Game Select

Onboard Button 3 (palette) toggles the colour / B&W switch. Switch states are
kept in the .fruitjam.cfg (left_difficulty, right_difficulty = A or B,
bw = 0/1) and set again at the next start. Stretch and rotate are not supported.
No cartridge has battery RAM, so there are no saves.
"""

import fjarcade as fj
import atari2600


class Emu:
    AUDIO_RATE = atari2600.AUDIO_RATE
    AUDIO_CHANNELS = 1

    def __init__(self, path, rom, video, cfg):
        self.rom = rom  # the core keeps a pointer; keep it alive
        self.st = bytearray(atari2600.STATE_SIZE)
        err = atari2600.init(self.st, rom)
        if err == 2:
            raise fj.RomError("DPC+/CDF (ARM) cartridges are not supported")
        if err:
            raise fj.RomError("unsupported 2600 cartridge size %d" % len(rom))
        print("2600 bankswitching", atari2600.mapper(self.st))
        self.saver = fj.Saver(path, 0)
        atari2600.set_output(self.st, video.fb, video.W, video.H, video.bpp,
                             cfg.get_int("top", 12))
        self.cfg = cfg
        self.switches = 0
        if cfg.get("left_difficulty", "B").upper() == "A":
            self.switches |= atari2600.INPUT_LEFT_A
        if cfg.get("right_difficulty", "B").upper() == "A":
            self.switches |= atari2600.INPUT_RIGHT_A
        if cfg.get_bool("bw", False):
            self.switches |= atari2600.INPUT_BW
        self.prev = 0

    def audio_half(self):
        return 4 * 368  # ~67 ms at 22050 Hz, as in the port

    def set_audio(self, ring, start):
        atari2600.set_audio(self.st, ring, start)

    def samples(self):
        return atari2600.samples(self.st)

    def _toggle(self, bit, key):
        self.switches ^= bit
        self.cfg.set(key, "A" if self.switches & bit else "B")
        print("2600 %s %s" % (key, self.cfg.get(key)))

    def set_input(self, m):
        pressed = m & ~self.prev
        self.prev = m
        if pressed & fj.START2:
            self._toggle(atari2600.INPUT_LEFT_A, "left_difficulty")
        if pressed & fj.C:
            self._toggle(atari2600.INPUT_RIGHT_A, "right_difficulty")
        j = self.switches
        if m & fj.UP:
            j |= atari2600.INPUT_UP
        if m & fj.DOWN:
            j |= atari2600.INPUT_DOWN
        if m & fj.LEFT:
            j |= atari2600.INPUT_LEFT
        if m & fj.RIGHT:
            j |= atari2600.INPUT_RIGHT
        if m & (fj.A | fj.B):
            j |= atari2600.INPUT_FIRE
        if m & fj.START:
            j |= atari2600.INPUT_RESET
        if m & fj.SELECT:
            j |= atari2600.INPUT_SELECT
        atari2600.set_input(self.st, j)

    def frame(self):
        return atari2600.run_frames(self.st, 1) == 1

    def error(self):
        pc = atari2600.error(self.st)
        return "CPU jam at 0x%04X" % pc if pc is not None else "CPU stopped"

    def action(self, name, cfg):
        if name == fj.ACT_PALETTE:
            self.switches ^= atari2600.INPUT_BW
            bw = 1 if self.switches & atari2600.INPUT_BW else 0
            cfg.set("bw", bw)
            print("2600:", "B&W" if bw else "colour")
        else:
            print("2600: %s not supported" % name)
