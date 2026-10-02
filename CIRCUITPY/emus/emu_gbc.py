# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Game Boy and Game Boy Color adapter for the launcher (peanutgbc.mpy).

Runs both .gb and .gbc files. The core reads the header: colour carts
(0x143 bit 7 set) run in CGB mode, plain Game Boy carts in DMG mode with greys.
The 160x144 picture is drawn 1:1 in the middle; 2x would not fit in 240 lines.

Pad: D-pad, A = A (D10), B = B (A2), Start = D6, Select = Coin (A5).
The third button (A1) is a second B and D7 a second Start.

Battery saves (MBC carts with RAM) go through fjarcade.Saver. The MBC3 clock
runs while playing but is not kept across power cycles.
"""

import fjarcade as fj
import peanutgbc

GB_W = 160
GB_H = 144

INIT_ERRORS = {1: "cartridge type not supported", 2: "bad header checksum"}
CORE_ERRORS = ("unknown error", "invalid opcode", "invalid read", "invalid write",
               "halted forever")


class Emu:
    AUDIO_RATE = peanutgbc.AUDIO_RATE
    AUDIO_CHANNELS = 2

    def __init__(self, path, rom, video, cfg):
        self.rom = rom  # the core keeps a pointer; keep it alive
        self.st = bytearray(peanutgbc.STATE_SIZE)
        err = peanutgbc.init(self.st, rom)
        if err:
            raise fj.RomError("Game Boy: %s" % INIT_ERRORS.get(err, "init error %d" % err))
        try:
            size = peanutgbc.save_size(self.st)
        except ValueError:
            raise fj.RomError("Game Boy: bad cartridge RAM size in header")
        print("Game Boy mode", "CGB" if peanutgbc.is_cgb(self.st) else "DMG",
              "save RAM", size)
        self.saver = fj.Saver(path, size)
        if self.saver.cart is not None:
            peanutgbc.set_cart_ram(self.st, self.saver.cart)
        x0, y0 = video.origin(GB_W, GB_H)
        peanutgbc.set_output(self.st, video.fb, video.W, video.bpp, 1, x0, y0)
        self.frames = 0

    def audio_half(self):
        return 4 * peanutgbc.AUDIO_SAMPLES_PER_FRAME

    def set_audio(self, ring, start):
        peanutgbc.set_audio(self.st, ring, start)

    def samples(self):
        return self.frames * peanutgbc.AUDIO_SAMPLES_PER_FRAME

    def set_input(self, m):
        j = 0
        if m & fj.UP:
            j |= peanutgbc.JOYPAD_UP
        if m & fj.DOWN:
            j |= peanutgbc.JOYPAD_DOWN
        if m & fj.LEFT:
            j |= peanutgbc.JOYPAD_LEFT
        if m & fj.RIGHT:
            j |= peanutgbc.JOYPAD_RIGHT
        if m & fj.A:
            j |= peanutgbc.JOYPAD_A
        if m & (fj.B | fj.C):
            j |= peanutgbc.JOYPAD_B
        if m & (fj.START | fj.START2):
            j |= peanutgbc.JOYPAD_START
        if m & fj.SELECT:
            j |= peanutgbc.JOYPAD_SELECT
        peanutgbc.set_joypad(self.st, j)

    def frame(self):
        if peanutgbc.run_frames(self.st, 1) != 1:
            return False
        self.frames += 1
        return True

    def error(self):
        code, addr = peanutgbc.error(self.st)
        what = CORE_ERRORS[code - 1] if 0 < code <= len(CORE_ERRORS) else "error %d" % code
        return "%s at 0x%04X" % (what, addr)

    def action(self, name, cfg):
        # peanutgbc has a fixed DMG grey palette and integer scale only.
        print("Game Boy: %s not supported" % name)
