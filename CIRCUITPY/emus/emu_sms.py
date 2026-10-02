# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Sega Master System, Game Gear and SG-1000 adapter for the launcher (sms.mpy).

The extension picks the system: .sms Master System, .gg Game Gear, .sg SG-1000.
The core lays the picture out from the framebuffer size: Master System and
SG-1000 (256x192) 1:1 and centred, Game Gear (160x144) doubled across and
1.5x down to 320x216.

Pad:
  D-pad          joystick
  A (D10)        button 1
  B (A2)         button 2
  C (A1)         button 2 (duplicate)
  Start (D6)     Pause on Master System and SG-1000, Start on Game Gear
  Start2 (D7)    same as Start
  Coin (A5)      not used (no Select on Sega pads)

Saves: mapped carts get 32 KB of cart RAM through fjarcade.Saver. Games that
never touch it leave it unchanged, so no .sav is written for them.
"""

import fjarcade as fj
import sms

SYSTEMS = {".sms": sms.SYSTEM_SMS, ".gg": sms.SYSTEM_GG, ".sg": sms.SYSTEM_SG}
SYSTEM_NAMES = ("Master System", "Game Gear", "SG-1000")
MAPPER_NAMES = ("none", "Sega", "Codemasters")


class Emu:
    AUDIO_RATE = sms.AUDIO_RATE
    AUDIO_CHANNELS = 2  # the core writes its mono samples to both channels

    def __init__(self, path, rom, video, cfg):
        self.rom = rom  # the core keeps a pointer; keep it alive
        self.st = bytearray(sms.STATE_SIZE)
        system = SYSTEMS.get(fj.ext_of(path), sms.SYSTEM_SMS)
        if sms.init(self.st, rom, system):
            raise fj.RomError("Sega ROM too small (%d bytes)" % len(rom))
        system, mapper, rom_size = sms.info(self.st)
        print("Sega", SYSTEM_NAMES[system], "mapper", MAPPER_NAMES[mapper], "ROM", rom_size)
        self.saver = fj.Saver(path, sms.save_size(self.st))
        if self.saver.cart is not None:
            sms.set_cart_ram(self.st, self.saver.cart)
        sms.set_output(self.st, video.fb, video.W, video.H, video.bpp)
        self.frames = 0

    def audio_half(self):
        return 4 * sms.AUDIO_SAMPLES_PER_FRAME

    def set_audio(self, ring, start):
        sms.set_audio(self.st, ring, start)

    def samples(self):
        return self.frames * sms.AUDIO_SAMPLES_PER_FRAME

    def set_input(self, m):
        j = 0
        if m & fj.UP:
            j |= sms.JOYPAD_UP
        if m & fj.DOWN:
            j |= sms.JOYPAD_DOWN
        if m & fj.LEFT:
            j |= sms.JOYPAD_LEFT
        if m & fj.RIGHT:
            j |= sms.JOYPAD_RIGHT
        if m & fj.A:
            j |= sms.JOYPAD_1
        if m & (fj.B | fj.C):
            j |= sms.JOYPAD_2
        if m & (fj.START | fj.START2):
            j |= sms.JOYPAD_START
        sms.set_joypad(self.st, j)

    def frame(self):
        if sms.run_frames(self.st, 1) != 1:
            return False
        self.frames += 1
        return True

    def error(self):
        return "core stopped"  # sms.mpy always runs the frames it is asked for

    def action(self, name, cfg):
        # sms.mpy picks its own layout and has no palette switch.
        print("Sega: %s not supported" % name)
