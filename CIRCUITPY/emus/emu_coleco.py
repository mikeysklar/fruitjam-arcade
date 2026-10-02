# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""ColecoVision adapter for the launcher (coleco.mpy).

BIOS: a small open BIOS is built in. It boots cartridges that drive the
hardware themselves (most homebrew, e.g. anything made with CVBasic) but has
no title screen and none of the OS7 library commercial cartridges call. Put a
dump of a real 8 KB BIOS in /sd/cart as coleco.rom and it is used instead.

The 256x192 picture is drawn 1:1 in the middle; the border shows the VDP
backdrop colour like a real TV.

Pad: the controller has a joystick, two fire buttons and a 12-key keypad.
C (A1) is a shift: held with another button it gives the second column.
Tapped on its own it is #.

  Button         alone          with C held
  D-pad          joystick       Up 3, Right 4, Down 5, Left 6
  A (D10)        left fire      7
  B (A2)         right fire     8
  Start (D6)     keypad 1       9
  Start2 (D7)    keypad 2       0
  Coin (A5)      keypad *       *
  C (A1)         keypad #       (shift)

Most games start with 1 (one player) or 2 and use * / # to restart. If
several keypad keys are held the lowest wins, like the real controller.
Start + Coin held for a second still goes back to the menu.
"""

import fjarcade as fj
import coleco

BIOS_NAME = "coleco.rom"

# fjarcade bit -> coleco bits, C not held.
PLAIN = (
    (fj.UP, coleco.UP), (fj.DOWN, coleco.DOWN),
    (fj.LEFT, coleco.LEFT), (fj.RIGHT, coleco.RIGHT),
    (fj.A, coleco.FIRE_L), (fj.B, coleco.FIRE_R),
    (fj.START, coleco.KEY_1), (fj.START2, coleco.KEY_2),
    (fj.SELECT, coleco.KEY_STAR),
)
# With C held: the second column of the keypad.
SHIFTED = (
    (fj.UP, coleco.KEY_3), (fj.RIGHT, coleco.KEY_4),
    (fj.DOWN, coleco.KEY_5), (fj.LEFT, coleco.KEY_6),
    (fj.A, coleco.KEY_7), (fj.B, coleco.KEY_8),
    (fj.START, coleco.KEY_9), (fj.START2, coleco.KEY_0),
    (fj.SELECT, coleco.KEY_STAR),
)


def load_bios(path):
    bios_path = path.rsplit("/", 1)[0] + "/" + BIOS_NAME
    try:
        with open(bios_path, "rb") as f:
            bios = f.read()
    except OSError:
        print("BIOS: built-in open BIOS (homebrew only)")
        return None
    if len(bios) != 8192:
        print("ignoring", bios_path, "(not 8192 bytes)")
        return None
    print("BIOS", bios_path)
    return bios


class Emu:
    AUDIO_RATE = coleco.AUDIO_RATE
    AUDIO_CHANNELS = 2  # the core writes its mono samples to both channels

    def __init__(self, path, rom, video, cfg):
        size = len(rom)
        # Pad short dumps to whole banks: 8 KB up to 32 KB, 16 KB above.
        unit = 16384 if size > 32768 else 8192
        if size % unit:
            rom = bytes(rom) + b"\xff" * (unit - size % unit)
        self.rom = rom  # the core keeps a pointer; keep it alive
        self.bios = load_bios(path)
        self.st = bytearray(coleco.STATE_SIZE)
        if coleco.init(self.st, self.rom, self.bios):
            raise fj.RomError("ColecoVision: bad cartridge size %d" % size)
        if not coleco.header_ok(self.st):
            print("warning: no AA55/55AA cartridge header, it may not boot")
        self.saver = fj.Saver(path, 0)  # no battery RAM on ColecoVision carts
        x0, y0 = video.origin(coleco.WIDTH, coleco.HEIGHT)
        coleco.set_output(self.st, video.fb, video.W, video.bpp, x0, y0)
        self.frames = 0

    def audio_half(self):
        return 4 * coleco.AUDIO_SAMPLES_PER_FRAME

    def set_audio(self, ring, start):
        coleco.set_audio(self.st, ring, start)

    def samples(self):
        return self.frames * coleco.AUDIO_SAMPLES_PER_FRAME

    def set_input(self, m):
        j = 0
        if m & fj.C:
            others = m & ~fj.C
            if others:
                for bit, cv in SHIFTED:
                    if others & bit:
                        j |= cv
            else:
                j = coleco.KEY_HASH
        else:
            for bit, cv in PLAIN:
                if m & bit:
                    j |= cv
        coleco.set_input(self.st, j)

    def frame(self):
        if coleco.run_frames(self.st, 1) != 1:
            return False
        self.frames += 1
        return True

    def error(self):
        return "core stopped"  # coleco.mpy always runs the frames it is asked for

    def action(self, name, cfg):
        # coleco.mpy draws 1:1 with the fixed TMS9918 palette.
        print("ColecoVision: %s not supported" % name)
