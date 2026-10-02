# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Watara Supervision adapter for the launcher (supervision.mpy). See emu_nes.py
for the contract.

Carts are .sv files. Many Supervision dumps are named .bin, but .bin in /cart
is taken by the Atari 2600, so rename them to .sv. No BIOS is needed.

The 160x160 LCD is drawn 1.5x (240x240, centred) with the in-between pixels
blended, in a green LCD palette. The game runs at the Supervision's 61.04 Hz
NMI rate. Sound is stereo (square 1 right, square 2 left, noise and sample
DMA either side, as on the real unit).

Pad:

  pad              Supervision
  ---------------  ---------------------------------
  D-pad            D-pad
  A (D10)          A
  B (A2)           B
  C (A1)           Select
  Start (D6)       Start
  Start2 (D7)      Select
  Coin (A5)        not used (keeps Start + Coin free to leave the game)

Onboard buttons: Button 1 (stretch) cycles the picture between 1.5x blended,
1.5x sharp and 1x, Button 3 (palette) cycles the colours in PALETTES. Both
are kept in the game's .fruitjam.cfg as `scale` and `palette`. Button 2
(rotate) does nothing. Supervision carts have no save RAM.
"""

import fjarcade as fj
import supervision as sv

# Button 3 cycles these, shade 0 (lightest) to 3, RGB888.
PALETTES = (
    (0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F),  # green LCD
    (0xE0E0E0, 0xB9B9B9, 0x545454, 0x121212),  # grey
    (0xE8D8A0, 0xB89850, 0x705020, 0x281808),  # amber
    (0xD0E8F8, 0x80A8D0, 0x305888, 0x081830),  # blue
    (0x9BBC0F, 0x8BAC0F, 0x306230, 0x0F380F),  # pea soup
)

# Button 1 cycles these picture modes.
MODES = (sv.MODE_SMOOTH, sv.MODE_SHARP, sv.MODE_1X)
MODE_NAMES = ("1.5x blended", "1.5x sharp", "1x")

# fjarcade bit -> Supervision bit
PAD = (
    (fj.UP, sv.JOYPAD_UP),
    (fj.DOWN, sv.JOYPAD_DOWN),
    (fj.LEFT, sv.JOYPAD_LEFT),
    (fj.RIGHT, sv.JOYPAD_RIGHT),
    (fj.A, sv.JOYPAD_A),
    (fj.B, sv.JOYPAD_B),
    (fj.C, sv.JOYPAD_SELECT),
    (fj.START, sv.JOYPAD_START),
    (fj.START2, sv.JOYPAD_SELECT),
)


class Emu:
    AUDIO_RATE = sv.AUDIO_RATE
    AUDIO_CHANNELS = 2

    def __init__(self, path, rom, video, cfg):
        self.rom = rom  # the core keeps a pointer; keep it alive
        self.st = bytearray(sv.STATE_SIZE)
        if sv.init(self.st, rom):
            raise fj.RomError("not a Supervision ROM (%d bytes, want a multiple of 8 KB)" % len(rom))
        size, banks = sv.info(self.st)
        print("Supervision ROM %d KB, %d bank%s" % (size // 1024, banks, "" if banks == 1 else "s"))
        self.video = video
        self.palette = cfg.get_int("palette", 0) % len(PALETTES)
        self.mode = cfg.get_int("scale", 0) % len(MODES)
        self._apply_palette()
        self._apply_mode()
        self.saver = fj.Saver(path, 0)
        self.made = 0

    def _apply_palette(self):
        sv.set_palette(self.st, *PALETTES[self.palette])

    def _apply_mode(self):
        v = self.video
        sv.set_output(self.st, v.fb, v.W, v.H, v.bpp, MODES[self.mode])

    def audio_half(self):
        return 1536  # stereo frames, ~70 ms at 22050 Hz, about 4 frames

    def set_audio(self, ring, start):
        sv.set_audio(self.st, ring, start)

    def samples(self):
        return self.made

    def set_input(self, m):
        j = 0
        for bit, key in PAD:
            if m & bit:
                j |= key
        sv.set_joypad(self.st, j)

    def frame(self):
        # run_frames returns 0 only when the CPU stopped (STP); no tuple per
        # frame, so the GC (about 9 ms on the board) runs less often.
        ok = sv.run_frames(self.st, 1) == 1
        self.made += sv.samples(self.st)
        return ok

    def error(self):
        code, pc = sv.error(self.st)
        if code == sv.ERR_STP:
            return "CPU executed STP at $%04X" % pc
        return "error %d at $%04X" % (code, pc)

    def action(self, name, cfg):
        if name == fj.ACT_PALETTE:
            self.palette = (self.palette + 1) % len(PALETTES)
            self._apply_palette()
            cfg.set("palette", self.palette)
            print("Supervision palette", self.palette)
        elif name == fj.ACT_STRETCH:
            self.mode = (self.mode + 1) % len(MODES)
            self._apply_mode()
            cfg.set("scale", self.mode)
            print("Supervision picture", MODE_NAMES[self.mode])
        else:
            print("Supervision: %s not supported" % name)
