# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""MSX1 adapter for the launcher (msx.mpy). Cartridge ROMs: .rom .mx1 .mx2.

BIOS: C-BIOS 0.29a is built into msx.mpy (open source, BSD-style license,
see msx/src/cbios/LICENSE.txt). It runs cartridge games; it has no MSX-BASIC,
so BASIC-only cartridges stop at its "no cartridge" style message. Put a dump
of a real 32 KB MSX1 BIOS in /sd/cart as msx.rom and it is used instead.

Mappers: plain ROMs up to 64 KB (placed from their "AB" header), and the
MegaROM mappers Konami, Konami SCC (with SCC sound), ASCII8 and ASCII16,
guessed from the ROM like openMSX does. A wrong guess can be fixed per game in
/sd/cart/<rom name>.fruitjam.cfg with one of:

  mapper = plain | konami | scc | ascii8 | ascii16

The 256x192 picture is drawn 1:1 in the middle; the border shows the VDP
backdrop colour like a real TV.

Pad: joystick port 1 plus the keys games ask for most. C (A1) is a shift:
held with another button it gives the second column. Tapped on its own it is N.

  Button         alone                        with C held
  D-pad          joystick 1 + cursor keys     Up 1, Down 2, Left 3, Right 4
  A (D10)        trigger A + Space            F2
  B (A2)         trigger B + M                F3
  Start (D6)     F1                           F4
  Start2 (D7)    Return                       F5
  Coin (A5)      1                            Esc
  C (A1)         N                            (shift)

Space or trigger A starts most games, F1 pauses Konami games, Return and 1/2
pick menu items. Start + Coin held for a second goes back to the menu (the
game briefly sees F1 + 1 while you hold them).

More per-game settings in the .fruitjam.cfg:

  dpad = both | joy | keys     where the D-pad goes (default both)
  keys = A:SPACE B:M START:F1 START2:RETURN SELECT:1 C:N
                               replace the plain-layer key of any of those
                               buttons; key names are in KEY_NAMES below,
                               letters and digits by themselves
"""

import fjarcade as fj
import msx

BIOS_NAME = "msx.rom"

MAPPERS = {
    "plain": msx.MAPPER_PLAIN, "konami": msx.MAPPER_KONAMI,
    "scc": msx.MAPPER_KONAMI_SCC, "konami_scc": msx.MAPPER_KONAMI_SCC,
    "ascii8": msx.MAPPER_ASCII8, "ascii16": msx.MAPPER_ASCII16,
}
MAPPER_NAMES = ("plain", "konami", "konami scc", "ascii8", "ascii16")

# Keyboard matrix (row, bit), international layout. Letters and digits are
# worked out in key().
KEY_NAMES = {
    "SHIFT": (6, 0), "CTRL": (6, 1), "GRAPH": (6, 2), "CAPS": (6, 3), "CODE": (6, 4),
    "F1": (6, 5), "F2": (6, 6), "F3": (6, 7), "F4": (7, 0), "F5": (7, 1),
    "ESC": (7, 2), "TAB": (7, 3), "STOP": (7, 4), "BS": (7, 5), "SELECT": (7, 6),
    "RETURN": (7, 7), "SPACE": (8, 0), "HOME": (8, 1), "INS": (8, 2), "DEL": (8, 3),
    "LEFT": (8, 4), "UP": (8, 5), "DOWN": (8, 6), "RIGHT": (8, 7),
}


def key(name):
    """Matrix (row, bit) of a key name, a letter or a digit."""
    name = name.upper()
    if name in KEY_NAMES:
        return KEY_NAMES[name]
    if len(name) == 1 and "0" <= name <= "9":
        return divmod(ord(name) - ord("0"), 8)
    if len(name) == 1 and "A" <= name <= "Z":
        return divmod(22 + ord(name) - ord("A"), 8)
    raise ValueError("unknown MSX key " + name)


BUTTONS = {"UP": fj.UP, "DOWN": fj.DOWN, "LEFT": fj.LEFT, "RIGHT": fj.RIGHT,
           "A": fj.A, "B": fj.B, "C": fj.C, "START": fj.START,
           "START2": fj.START2, "SELECT": fj.SELECT}

# fjarcade bit -> joystick bits, C not held.
JOY = ((fj.UP, msx.UP), (fj.DOWN, msx.DOWN), (fj.LEFT, msx.LEFT),
       (fj.RIGHT, msx.RIGHT), (fj.A, msx.TRIG_A), (fj.B, msx.TRIG_B))
# fjarcade bit -> key, C not held. C itself is the key sent when C is alone.
PLAIN = {fj.UP: "UP", fj.DOWN: "DOWN", fj.LEFT: "LEFT", fj.RIGHT: "RIGHT",
         fj.A: "SPACE", fj.B: "M", fj.START: "F1", fj.START2: "RETURN",
         fj.SELECT: "1", fj.C: "N"}
# With C held: the second column.
SHIFTED = {fj.UP: "1", fj.DOWN: "2", fj.LEFT: "3", fj.RIGHT: "4",
           fj.A: "F2", fj.B: "F3", fj.START: "F4", fj.START2: "F5",
           fj.SELECT: "ESC"}
DPAD = fj.UP | fj.DOWN | fj.LEFT | fj.RIGHT


def load_bios(path):
    bios_path = path.rsplit("/", 1)[0] + "/" + BIOS_NAME
    try:
        with open(bios_path, "rb") as f:
            bios = f.read()
    except OSError:
        print("BIOS: built-in C-BIOS 0.29a")
        return None
    if len(bios) != 32768:
        print("ignoring", bios_path, "(not 32768 bytes)")
        return None
    print("BIOS", bios_path)
    return bios


class Emu:
    AUDIO_RATE = msx.AUDIO_RATE
    AUDIO_CHANNELS = 2  # the core writes its mono samples to both channels

    def __init__(self, path, rom, video, cfg):
        size = len(rom)
        if size % 8192:  # whole 8 KB banks
            rom = bytes(rom) + b"\xff" * (8192 - size % 8192)
        self.rom = rom  # the core keeps a pointer; keep it alive
        self.bios = load_bios(path)
        mapper = -1
        name = cfg.get("mapper")
        if name:
            mapper = MAPPERS.get(name.lower().replace("-", "_").replace(" ", "_"), -1)
            if mapper < 0:
                print("unknown mapper", name, "- guessing")
        forced = mapper >= 0
        self.st = bytearray(msx.STATE_SIZE)
        err = msx.init(self.st, self.rom, self.bios, mapper)
        if err == 1:
            raise fj.RomError("MSX: bad ROM size %d" % size)
        if err:
            raise fj.RomError("MSX: a plain ROM over 64 KB needs a mapper (%d bytes)" % size)
        mapper, base = msx.info(self.st)
        print("MSX mapper", MAPPER_NAMES[mapper],
              "at 0x%04x" % base if mapper == msx.MAPPER_PLAIN else "",
              "(cfg)" if forced else "(guessed)")
        self.saver = fj.Saver(path, 0)  # SRAM carts (ASCII8/16 + SRAM) not emulated
        x0, y0 = video.origin(msx.WIDTH, msx.HEIGHT)
        msx.set_output(self.st, video.fb, video.W, video.bpp, x0, y0)
        self.frames = 0
        self.plain = dict(PLAIN)
        for item in (cfg.get("keys") or "").split():
            try:
                b, k = item.split(":")
                key(k)
                self.plain[BUTTONS[b.upper()]] = k
            except (ValueError, KeyError):
                print("bad keys entry", item)
        dpad = (cfg.get("dpad") or "both").lower()
        self.dpad_joy = dpad != "keys"
        self.dpad_keys = dpad != "joy"
        self.cache = {}

    def _inputs(self, m):
        """(joystick bits, 11-byte key matrix) for fjarcade mask m."""
        joy = 0
        names = []
        if m & fj.C:
            others = m & ~fj.C
            if others:
                names = [k for b, k in SHIFTED.items() if others & b]
            else:
                names = [self.plain[fj.C]]
        else:
            for b, j in JOY:
                if m & b and (self.dpad_joy or not b & DPAD):
                    joy |= j
            for b, k in self.plain.items():
                if m & b and b != fj.C and (self.dpad_keys or not b & DPAD):
                    names.append(k)
        mat = bytearray(11)
        for k in names:
            row, bit = key(k)
            mat[row] |= 1 << bit
        return joy, bytes(mat)

    def audio_half(self):
        return 4 * msx.AUDIO_SAMPLES_PER_FRAME

    def set_audio(self, ring, start):
        msx.set_audio(self.st, ring, start)

    def samples(self):
        return self.frames * msx.AUDIO_SAMPLES_PER_FRAME

    def set_input(self, m):
        v = self.cache.get(m)
        if v is None:
            v = self.cache[m] = self._inputs(m)
        msx.set_input(self.st, v[0])
        msx.set_keys(self.st, v[1])

    def frame(self):
        if msx.run_frames(self.st, 1) != 1:
            return False
        self.frames += 1
        return True

    def error(self):
        return "core stopped"  # msx.mpy always runs the frames it is asked for

    def action(self, name, cfg):
        # msx.mpy draws 1:1 with the fixed TMS9918 palette.
        print("MSX: %s not supported" % name)
