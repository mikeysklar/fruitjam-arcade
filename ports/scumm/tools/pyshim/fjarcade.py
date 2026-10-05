# SPDX-FileCopyrightText: 2026 Mikey Sklar
# SPDX-License-Identifier: GPL-3.0-or-later
"""The parts of the launcher's fjarcade.py that emu_scumm.py uses, for
running the adapter on CPython (tools/pyshim/test_adapter.py)."""

UP = 1 << 0
DOWN = 1 << 1
LEFT = 1 << 2
RIGHT = 1 << 3
A = 1 << 4
B = 1 << 5
C = 1 << 6
START = 1 << 7
START2 = 1 << 8
SELECT = 1 << 9
ACT_STRETCH = "stretch"
ACT_ROTATE = "rotate"
ACT_PALETTE = "palette"
SAVES_FALLBACK = "/tmp/fjscumm-saves"


class RomError(Exception):
    pass


class Saver:
    def __init__(self, path, size):
        self.cart = None

    def tick(self, now):
        pass

    def flush(self):
        pass


class Config:
    def __init__(self, values=None):
        self.values = dict(values or {})

    def get(self, k, d=None):
        return self.values.get(k, d)

    def get_int(self, k, d=0):
        try:
            return int(self.values[k])
        except (KeyError, ValueError):
            return d

    def get_bool(self, k, d=False):
        v = self.values.get(k)
        return d if v is None else v.lower() in ("1", "true", "yes", "on")

    def set(self, k, v):
        self.values[k] = str(v)


class Video:
    W = 320
    H = 240

    def __init__(self, bpp=16):
        self.bpp = bpp
        self.fb = bytearray(self.W * self.H * bpp // 8)

    def origin(self, w, h):
        return (self.W - w) // 2, (self.H - h) // 2
