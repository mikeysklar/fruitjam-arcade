# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Atari 7800 adapter for the launcher (atari7800.mpy). See emu_nes.py for the contract.

No BIOS: the cartridge starts at its reset vector. The core draws the whole
320x240 framebuffer itself (160 modes 2x wide, 320 modes 1:1), 240 lines from
scanline `top` (cfg, default 17; PAL carts move it down 25 more). Sound is
TIA plus POKEY when the header asks for one, mono. samples() is the core's
own running count, so PAL carts pace at 50 Hz on their own.

Pad:

  pad              7800
  ---------------  ---------------------------------
  D-pad            joystick
  A (D10)          left button (button 1)
  B (A2)           right button (button 2)
  C (A1)           right button (button 2)
  Start (D6)       Reset
  Start2 (D7)      Pause
  Coin (A5)        Select

Difficulty switches come from the .fruitjam.cfg: left_difficulty and
right_difficulty = A or B (default B). The onboard buttons (stretch, rotate,
palette) are not supported. Cart RAM is not battery backed, so no saves.
"""

import fjarcade as fj
import atari7800


class Emu:
    AUDIO_RATE = atari7800.AUDIO_RATE
    AUDIO_CHANNELS = 1

    def __init__(self, path, rom, video, cfg):
        self.rom = rom  # the core keeps a pointer; keep it alive
        self.st = bytearray(atari7800.STATE_SIZE)
        err = atari7800.init(self.st, rom)
        if err == 2:
            raise fj.RomError("7800 mapper not supported (Activision, Absolute, banksets, ...)")
        if err:
            raise fj.RomError("unsupported 7800 cartridge size %d" % len(rom))
        mapper, rom_bytes, pokey, pal = atari7800.info(self.st)
        print("7800 mapper %s, %d KB ROM%s%s" % (
            mapper, rom_bytes // 1024, ", POKEY at $%X" % pokey if pokey else "",
            ", PAL" if pal else ""))
        self.saver = fj.Saver(path, 0)
        top = cfg.get_int("top", 17)
        if pal:
            top += 25  # PAL MARIA displays 292 lines; show the middle 240
        atari7800.set_output(self.st, video.fb, video.W, video.H, video.bpp, top)
        self.switches = 0
        if cfg.get("left_difficulty", "B").upper() == "A":
            self.switches |= atari7800.INPUT_LEFT_A
        if cfg.get("right_difficulty", "B").upper() == "A":
            self.switches |= atari7800.INPUT_RIGHT_A

    def audio_half(self):
        return 4 * 368  # ~67 ms at 22050 Hz, as in the port

    def set_audio(self, ring, start):
        atari7800.set_audio(self.st, ring, start)

    def samples(self):
        return atari7800.samples(self.st)

    def set_input(self, m):
        j = self.switches
        if m & fj.UP:
            j |= atari7800.INPUT_UP
        if m & fj.DOWN:
            j |= atari7800.INPUT_DOWN
        if m & fj.LEFT:
            j |= atari7800.INPUT_LEFT
        if m & fj.RIGHT:
            j |= atari7800.INPUT_RIGHT
        if m & fj.A:
            j |= atari7800.INPUT_FIRE1
        if m & (fj.B | fj.C):
            j |= atari7800.INPUT_FIRE2
        if m & fj.START:
            j |= atari7800.INPUT_RESET
        if m & fj.START2:
            j |= atari7800.INPUT_PAUSE
        if m & fj.SELECT:
            j |= atari7800.INPUT_SELECT
        atari7800.set_input(self.st, j)

    def frame(self):
        return atari7800.run_frames(self.st, 1) == 1

    def error(self):
        pc = atari7800.error(self.st)
        return "CPU jam at 0x%04X" % pc if pc is not None else "CPU stopped"

    def action(self, name, cfg):
        # atari7800.mpy has no stretch, rotation or palette switch.
        print("7800: %s not supported" % name)
