# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""CHIP-8 / SUPER-CHIP adapter for the launcher (chip8.mpy).

.ch8 runs with classic CHIP-8 quirks at 11 instructions per frame, .sc8 with
SUPER-CHIP quirks at 30. The interpreter centres and scales the picture
itself (64x32 at 5x, hires 128x64 at 2x) and only redraws changed frames.

CHIP-8 has a 16-key hex keypad. Each pad button is a control, and the control
picks a hex key. Defaults follow the keys most games use (on a PC keyboard,
W A S D E Q map to 5 7 8 9 6 4):

  pad button     control   default hex key
  Up             up        5
  Down           down      8
  Left           left      7
  Right          right     9
  A (D10)        a         6
  B (A2)         b         4
  C (A1)         c         1
  Start (D6)     start     F
  Start 2 (D7)   start2    2
  Coin (A5)      select    E

  hex keypad:  1 2 3 C
               4 5 6 D
               7 8 9 E
               A 0 B F

Games disagree on keys. ROMS below holds known fixes by file name. A game's
<rom>.fruitjam.cfg can override keys, quirks and speed on top of that:

  keys = up:1 down:4 a:5       control:hex pairs, space or comma separated
  quirks = schip               chip8, schip, or a number (0x2f) of QUIRK_* bits
  ipf = 20                     instructions per frame

Onboard buttons: Button 3 (palette) cycles the colour pairs in PALETTES and
stores the choice as `palette` in the cfg. Stretch and rotate do nothing.

SUPER-CHIP 00FD (exit) ends the program on purpose: exited() tells
fjarcade.run() to go straight back to the menu, no error screen.
"""

import os

import chip8
import fjarcade as fj

IPF_CHIP8 = 11
IPF_SCHIP = 30
BEEP_HZ = 440
BEEP_LEVEL = 4000  # 0..32767

# Control -> hex key, in fjarcade bit order.
CONTROLS = (
    ("up", fj.UP, 0x5),
    ("down", fj.DOWN, 0x8),
    ("left", fj.LEFT, 0x7),
    ("right", fj.RIGHT, 0x9),
    ("a", fj.A, 0x6),
    ("b", fj.B, 0x4),
    ("c", fj.C, 0x1),
    ("start", fj.START, 0xF),
    ("start2", fj.START2, 0x2),
    ("select", fj.SELECT, 0xE),
)

# Per ROM settings, keyed by the file name without extension, lower case.
# "keys" entries replace the matching defaults; "quirks" and "ipf" replace
# the defaults for the extension. Examples from the classic public domain
# games; check them against the game's own instructions.
ROMS = {
    "pong": {"keys": {"up": 0x1, "down": 0x4}},
    "pong2": {"keys": {"up": 0x1, "down": 0x4}},
    "tetris": {"keys": {"left": 0x5, "right": 0x6, "a": 0x4, "down": 0x7}},
    "brix": {"keys": {"left": 0x4, "right": 0x6}},
    "breakout": {"keys": {"left": 0x4, "right": 0x6}},
    "invaders": {"keys": {"left": 0x4, "right": 0x6, "a": 0x5}},
    "tank": {"keys": {"up": 0x2, "down": 0x8, "left": 0x4, "right": 0x6, "a": 0x5}},
    "blinky": {"keys": {"up": 0x3, "down": 0x6, "left": 0x7, "right": 0x8}},
    # "somegame": {"quirks": chip8.QUIRKS_SCHIP, "ipf": 30},
}

# Button 3 cycles these (lit, unlit) colours, RGB888.
PALETTES = (
    ((0xFF, 0xFF, 0xFF), (0x00, 0x00, 0x00)),  # white on black
    ((0x33, 0xFF, 0x66), (0x00, 0x18, 0x00)),  # green phosphor
    ((0xFF, 0xB0, 0x00), (0x20, 0x10, 0x00)),  # amber
    ((0x0F, 0x38, 0x0F), (0x9B, 0xBC, 0x0F)),  # LCD green
    ((0xFF, 0xFF, 0xFF), (0x00, 0x30, 0xA0)),  # white on blue
)


def _parse_keys(text):
    """'up:1 down:4' -> {"up": 1, "down": 4}. Bad pairs are skipped."""
    out = {}
    for pair in text.replace(",", " ").split():
        if ":" not in pair:
            continue
        name, key = pair.split(":", 1)
        try:
            k = int(key, 16)
        except ValueError:
            continue
        if 0 <= k <= 0xF:
            out[name.strip().lower()] = k
    return out


def _parse_quirks(text, default):
    text = text.strip().lower()
    if text == "chip8":
        return chip8.QUIRKS_CHIP8
    if text == "schip":
        return chip8.QUIRKS_SCHIP
    try:
        return int(text, 0) & 0x3F
    except ValueError:
        return default


class Emu:
    AUDIO_RATE = chip8.AUDIO_RATE
    AUDIO_CHANNELS = 2

    def __init__(self, path, rom, video, cfg):
        name = path.rsplit("/", 1)[-1].rsplit(".", 1)
        schip = len(name) > 1 and name[1].lower() == "sc8"
        opts = ROMS.get(name[0].lower(), {})
        quirks = opts.get("quirks", chip8.QUIRKS_SCHIP if schip else chip8.QUIRKS_CHIP8)
        ipf = opts.get("ipf", IPF_SCHIP if schip else IPF_CHIP8)
        if cfg.get("quirks") is not None:
            quirks = _parse_quirks(cfg.get("quirks"), quirks)
        ipf = min(max(cfg.get_int("ipf", ipf), 1), 100000)
        keymap = {c: k for c, _, k in CONTROLS}
        keymap.update(opts.get("keys", {}))
        if cfg.get("keys"):
            keymap.update(_parse_keys(cfg.get("keys")))
        # (fjarcade bit, hex key bit) pairs for set_input.
        self.map = tuple((bit, 1 << keymap[c]) for c, bit, _ in CONTROLS)
        print("SUPER-CHIP" if schip else "CHIP-8", "quirks 0x%02x ipf %d" % (quirks, ipf))

        self.st = bytearray(chip8.STATE_SIZE)
        seed = int.from_bytes(os.urandom(4), "little") | 1
        if chip8.init(self.st, rom, seed):
            raise fj.RomError("program too big (%d bytes, max 3584)" % len(rom))
        chip8.config(self.st, quirks, ipf)
        chip8.set_beep(self.st, BEEP_HZ, BEEP_LEVEL)
        # The core centres the picture in the whole framebuffer itself.
        chip8.set_output(self.st, video.fb, video.W, video.H, video.bpp)
        self.bpp = video.bpp
        self.palette = cfg.get_int("palette", 0) % len(PALETTES)
        self._apply_palette()
        self.saver = fj.Saver(path, 0)  # CHIP-8 has no save RAM
        self.frames = 0

    def _pixel(self, rgb):
        r, g, b = rgb
        if self.bpp == 16:
            return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        return (r & 0xE0) | ((g & 0xE0) >> 3) | (b >> 6)

    def _apply_palette(self):
        fg, bg = PALETTES[self.palette]
        chip8.set_colors(self.st, self._pixel(fg), self._pixel(bg))

    def audio_half(self):
        return 4 * chip8.AUDIO_SAMPLES_PER_FRAME

    def set_audio(self, ring, start):
        chip8.set_audio(self.st, ring, start)

    def samples(self):
        return self.frames * chip8.AUDIO_SAMPLES_PER_FRAME

    def set_input(self, m):
        k = 0
        for bit, key in self.map:
            if m & bit:
                k |= key
        chip8.set_keys(self.st, k)

    def frame(self):
        if chip8.run_frames(self.st, 1) != 1:
            return False
        self.frames += 1
        return True

    def exited(self):
        # SUPER-CHIP 00FD ends the program on purpose; fjarcade.run() goes back to the menu.
        return chip8.error(self.st)[0] == chip8.ERR_EXIT

    def error(self):
        code, addr = chip8.error(self.st)
        if code == chip8.ERR_EXIT:
            return "program exited"
        if code == chip8.ERR_STACK:
            return "call stack error at 0x%03x" % addr
        return "unknown instruction at 0x%03x" % addr

    def action(self, name, cfg):
        if name == fj.ACT_PALETTE:
            self.palette = (self.palette + 1) % len(PALETTES)
            self._apply_palette()
            cfg.set("palette", self.palette)
            print("CHIP-8 palette", self.palette)
        else:
            print("CHIP-8: %s not supported" % name)
