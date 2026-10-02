# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""NES adapter for the launcher (nes.mpy). Reference for the other emu_*.py.

Adapter contract, used by fjarcade.run():
  Emu(path, rom, video, cfg)   load the ROM, draw into video.fb; raise
                               fjarcade.RomError for a ROM the core rejects
  AUDIO_RATE, AUDIO_CHANNELS   what the core writes into the ring
  audio_half()                 ring half length in sample frames
  set_audio(ring, start)       core writes from `start` (sample frames)
  samples()                    running count of sample frames made
  set_input(mask)              fjarcade logical bitmask -> core input
  frame()                      run one frame, False if the core stopped
  error()                      why it stopped
  action(name, cfg)            onboard Button 1-3 (ACT_*), may be a no-op
  saver                        fjarcade.Saver (cart may be None)

Pad: D-pad, A = A (D10), B = B (A2), Start = D6, Select = Coin (A5).
The third button (A1) is a turbo-free second B, handy on arcade panels.
"""

import fjarcade as fj
import nes


class Emu:
    AUDIO_RATE = nes.AUDIO_RATE
    AUDIO_CHANNELS = 2

    def __init__(self, path, rom, video, cfg):
        self.rom = rom  # the core keeps a pointer; keep it alive
        self.st = bytearray(nes.STATE_SIZE)
        err = nes.init(self.st, rom)
        if err == 2:
            raise fj.RomError("NES mapper %d not supported" % nes.info(self.st)[0])
        if err:
            raise fj.RomError("not an iNES file (error %d)" % err)
        mapper, prg, chr_size, battery = nes.info(self.st)
        print("NES mapper", mapper, "PRG", prg, "CHR", chr_size or "RAM",
              "battery" if battery else "")
        self.saver = fj.Saver(path, nes.save_size(self.st))
        if self.saver.cart is not None:
            nes.set_cart_ram(self.st, self.saver.cart)
        x0, y0 = video.origin(nes.WIDTH, nes.HEIGHT)
        nes.set_output(self.st, video.fb, video.W, video.bpp, x0, y0)
        self.frames = 0

    def audio_half(self):
        return 4 * nes.AUDIO_SAMPLES_PER_FRAME

    def set_audio(self, ring, start):
        nes.set_audio(self.st, ring, start)

    def samples(self):
        return self.frames * nes.AUDIO_SAMPLES_PER_FRAME

    def set_input(self, m):
        j = 0
        if m & fj.UP:
            j |= nes.JOYPAD_UP
        if m & fj.DOWN:
            j |= nes.JOYPAD_DOWN
        if m & fj.LEFT:
            j |= nes.JOYPAD_LEFT
        if m & fj.RIGHT:
            j |= nes.JOYPAD_RIGHT
        if m & fj.A:
            j |= nes.JOYPAD_A
        if m & (fj.B | fj.C):
            j |= nes.JOYPAD_B
        if m & (fj.START | fj.START2):
            j |= nes.JOYPAD_START
        if m & fj.SELECT:
            j |= nes.JOYPAD_SELECT
        nes.set_joypad(self.st, j)

    def frame(self):
        if nes.run_frames(self.st, 1) != 1:
            return False
        self.frames += 1
        return True

    def error(self):
        return "CPU jam %s" % (nes.error(self.st),)

    def action(self, name, cfg):
        # nes.mpy has no stretch, rotation or palette switch yet.
        print("NES: %s not supported" % name)
