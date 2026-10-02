# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Atari Lynx adapter for the launcher (lynx.mpy). See emu_nes.py for the contract.

.lnx/.lyx carts boot through the core's built in replacement for the Lynx
boot ROM (HLE). If lynxboot.img (the real 512 byte boot ROM, not included)
is in /sd/cart next to the game, it is used instead. .o files (BS93
homebrew) load straight into RAM.

The 160x102 screen is drawn 2x, 320x204, centred. Games made for a sideways
Lynx are shown unrotated. Sound is stereo. Games run at 50, 60 or 75 Hz, so
samples() adds up what each run_frames made (the core only counts per call).

Pad:

  pad              Lynx
  ---------------  ---------------------------------
  D-pad            D-pad
  A (D10)          A
  B (A2)           B
  C (A1)           Option 1
  Start (D6)       Pause
  Start2 (D7)      Option 2
  Coin (A5)        not used (keeps Start + Coin free to leave the game)

Pause + Option 1 restarts most games, Pause + Option 2 flips the screen.
The onboard buttons (stretch, rotate, palette) are not supported: the core
only scales 2x and has no rotation. No cart EEPROM is emulated, so no saves.
"""

import fjarcade as fj
import lynx


class Emu:
    AUDIO_RATE = lynx.AUDIO_RATE
    AUDIO_CHANNELS = 2

    def __init__(self, path, rom, video, cfg):
        self.rom = rom  # the core keeps a pointer; keep it alive
        self.boot = None
        boot_path = path.rsplit("/", 1)[0] + "/lynxboot.img"
        try:
            with open(boot_path, "rb") as f:
                self.boot = f.read(512)
        except OSError:
            pass
        if self.boot is not None and len(self.boot) < 512:
            print("Lynx: %s is short, ignored" % boot_path)
            self.boot = None
        self.st = bytearray(lynx.STATE_SIZE)
        if lynx.init(self.st, rom, self.boot):
            raise fj.RomError("not a Lynx image (too small)")
        kind, rotation, addr, size, page = lynx.info(self.st)
        if kind == lynx.BOOT_HLE:
            print("Lynx boot: HLE decrypted %d loader blocks to $%04X, page size %d" % (
                size, addr, page))
        elif kind == lynx.BOOT_BS93:
            print("Lynx boot: BS93 homebrew, %d bytes loaded at $%04X" % (size, addr))
        else:
            print("Lynx boot: real boot ROM, page size %d" % page)
        if rotation:
            print("Lynx rotation flag", ("none", "left", "right")[rotation % 3],
                  "(shown unrotated)")
        self.saver = fj.Saver(path, 0)
        x0, y0 = video.origin(2 * lynx.WIDTH, 2 * lynx.HEIGHT)
        lynx.set_output(self.st, video.fb, video.W, video.bpp, x0 & ~1, y0)
        self.made = 0

    def audio_half(self):
        return 1600  # stereo frames, ~73 ms at 22050 Hz, as in the port

    def set_audio(self, ring, start):
        lynx.set_audio(self.st, ring, start)

    def samples(self):
        return self.made

    def set_input(self, m):
        j = 0
        if m & fj.UP:
            j |= lynx.JOYPAD_UP
        if m & fj.DOWN:
            j |= lynx.JOYPAD_DOWN
        if m & fj.LEFT:
            j |= lynx.JOYPAD_LEFT
        if m & fj.RIGHT:
            j |= lynx.JOYPAD_RIGHT
        if m & fj.A:
            j |= lynx.JOYPAD_A
        if m & fj.B:
            j |= lynx.JOYPAD_B
        if m & fj.C:
            j |= lynx.JOYPAD_OPT1
        if m & fj.START2:
            j |= lynx.JOYPAD_OPT2
        if m & fj.START:
            j |= lynx.JOYPAD_PAUSE
        lynx.set_joypad(self.st, j)

    def frame(self):
        # run_frames may return 0 when a game stops its display timers; the
        # core still advances time and audio then, so only a CPU stop counts.
        lynx.run_frames(self.st, 1)
        self.made += lynx.samples(self.st)
        return lynx.error(self.st)[0] == 0

    def error(self):
        code, pc = lynx.error(self.st)
        if code == 3:
            return "CPU executed STP at $%04X" % pc
        return "error %d at $%04X" % (code, pc)

    def action(self, name, cfg):
        # lynx.mpy only scales 2x and has no rotation or palette switch.
        print("Lynx: %s not supported" % name)
