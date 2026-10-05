# SPDX-FileCopyrightText: 2026 Mikey Sklar
# SPDX-License-Identifier: GPL-3.0-or-later
"""SCUMM adapter for the Fruit Jam launcher (scumm.mpy): Loom, Monkey Island
(EGA), Indiana Jones and the Last Crusade, Passport to Adventure.

A SCUMM game is a folder, not a file. The launcher hands this adapter a
path that is one of:
  - a folder holding the game files,
  - a file inside it (000.LFL, 00.LFL, Loom.exe),
  - a small "<name>.scumm" marker file in /sd/cart. The marker holds the
    game folder (absolute, or relative to the marker); when it is empty the
    folder is the marker's own name without ".scumm".

Pad, as a mouse:
  D-pad      move the pointer (it speeds up while held)
  A          left click            B   right click
  C          Escape (skip a cutscene)
  Start      space (pause)         Coin  F5 (the game's save/load screen)
  Start 2    "." (skip the current line of dialogue)
  Start + Coin held 1 s: back to the menu (neither key is sent then)
Onboard: Button 1 = F5, Button 2 = quick save, Button 3 = quick load.
The quick slot is QUICK_SLOT; it shows in the game's own load screen as
"Fruit Jam". The game's save screen wants a typed name, which the pad
cannot do, so quick save is the way to save without a keyboard.
"""

import os

import fjarcade as fj
import scumm

ARENA_BYTES = 2 * 1024 * 1024  # engine heap, coroutine stack and screen

KEY_ESC = (27, 27)
KEY_SPACE = (32, 32)
KEY_F5 = (286, 319)  # ScummVM keycode, ascii value the scripts see
KEY_DOT = (46, 46)

SPEEDS = ((1, 4), (1, 6), (2, 8))  # (start, top) pointer speed in px/frame
QUICK_SLOT = 1


def _is_dir(p):
    try:
        return os.stat(p)[0] & 0x4000 != 0
    except OSError:
        return False


def game_folder(path, rom):
    """Work out the game folder from what the launcher passed."""
    if _is_dir(path):
        return path.rstrip("/")
    head = path.rsplit("/", 1)[0] if "/" in path else "."
    if path.lower().endswith(".scumm"):
        target = bytes(rom or b"").decode().strip()
        if not target:
            return path[:-6]
        if not target.startswith("/"):
            target = head + "/" + target
        return target.rstrip("/")
    return head


class ScummIO:
    """File access for the engine. Names are matched without regard to case.

    Writes (save games) go to the game folder, or to /saves/<folder name>
    when the card is read-only (mounted on a computer), where reads look too.
    """

    def __init__(self, folder):
        self.folder = folder
        self.fallback = fj.SAVES_FALLBACK + "/" + folder.rsplit("/", 1)[-1]
        self.files = {}
        self.sizes = {}
        self.names = {}
        self._scan()

    def _scan(self):
        self.names = {}
        for d in (self.fallback, self.folder):  # the game folder wins
            try:
                for n in os.listdir(d):
                    self.names[n.lower()] = d + "/" + n
            except OSError:
                pass

    def _free(self):
        h = 0
        while h in self.files:
            h += 1
        return h

    def open(self, name, write):
        try:
            if write:
                f = None
                for d in (self.folder, self.fallback):
                    try:
                        f = open(d + "/" + name, "wb")
                        break
                    except OSError:
                        if d == self.fallback:
                            try:
                                os.mkdir(fj.SAVES_FALLBACK)
                            except OSError:
                                pass
                            try:
                                os.mkdir(d)
                                f = open(d + "/" + name, "wb")
                            except OSError:
                                pass
                if f is None:
                    return -1
                self.names[name.lower()] = d + "/" + name
                size = 0
            else:
                p = self.names.get(name.lower())
                if p is None:
                    return -1
                f = open(p, "rb")
                size = os.stat(p)[6]
            h = self._free()
            self.files[h] = f
            self.sizes[h] = size
            return h
        except Exception as e:
            print("scumm: open", name, e)
            return -1

    def read(self, h, buf):
        try:
            return self.files[h].readinto(buf) or 0
        except Exception as e:
            print("scumm: read", e)
            return 0

    def write(self, h, data):
        try:
            n = self.files[h].write(data)
            self.sizes[h] += n
            return n
        except Exception as e:
            print("scumm: write", e)
            return 0

    # None of these may raise: an exception would unwind through the
    # engine's frame with the engine stopped halfway through a request.
    def seek(self, h, pos):
        try:
            self.files[h].seek(pos)
            return 0
        except Exception as e:
            print("scumm: seek", e)
            return -1

    def size(self, h):
        return self.sizes.get(h, 0)

    def close(self, h):
        f = self.files.pop(h, None)
        self.sizes.pop(h, None)
        try:
            if f is not None:
                f.close()
        except OSError:
            pass

    def log(self, msg):
        print("scumm:", msg.rstrip())


class Emu:
    AUDIO_RATE = scumm.AUDIO_RATE
    AUDIO_CHANNELS = 2

    def __init__(self, path, rom, video, cfg):
        folder = game_folder(path, rom)
        if not _is_dir(folder):
            raise fj.RomError("SCUMM: no game folder %s" % folder)
        self.io = ScummIO(folder)
        try:
            self.arena = bytearray(ARENA_BYTES)
        except MemoryError:
            raise fj.RomError("SCUMM: not enough memory for the engine")
        err = scumm.init(self.arena, self.io)
        if err:
            raise fj.RomError("SCUMM: %s" % scumm.error())
        print("SCUMM game", scumm.game(), "in", folder)
        scumm.config("music", cfg.get("music", "pcspk"))
        scumm.config("subtitles", "true" if cfg.get_bool("subtitles", True) else "false")
        scumm.config("talkspeed", str(cfg.get_int("talkspeed", 60)))
        x0, y0 = video.origin(scumm.WIDTH, scumm.HEIGHT)
        scumm.set_output(video.fb, video.W, video.H, video.bpp, x0, y0)
        self.saver = fj.Saver(path, 0)
        self.x = scumm.WIDTH // 2
        self.y = scumm.HEIGHT // 2
        self.held = 0  # frames a direction has been held
        self.speed = cfg.get_int("pointer_speed", 0) % len(SPEEDS)
        self.last = 0
        self.combo = False  # Start and Coin went down together: no keys
        scumm.input(self.x, self.y, 0)
        slot = cfg.get_int("autoload", 0)
        if slot:
            print("SCUMM loading save slot", slot)
            scumm.load(slot)  # done once the engine is up, on frame 2

    def audio_half(self):
        return 4 * scumm.AUDIO_RATE // 60

    def set_audio(self, ring, start):
        scumm.set_audio(ring, start)

    def samples(self):
        return scumm.samples()

    def set_input(self, m):
        d = m & (fj.UP | fj.DOWN | fj.LEFT | fj.RIGHT)
        if d:
            lo, hi = SPEEDS[self.speed]
            step = min(hi, lo + self.held // 8)
            self.held += 1
            if m & fj.LEFT:
                self.x -= step
            if m & fj.RIGHT:
                self.x += step
            if m & fj.UP:
                self.y -= step
            if m & fj.DOWN:
                self.y += step
            self.x = max(0, min(scumm.WIDTH - 1, self.x))
            self.y = max(0, min(scumm.HEIGHT - 1, self.y))
        else:
            self.held = 0
        b = 0
        if m & fj.A:
            b |= scumm.BTN_LEFT
        if m & fj.B:
            b |= scumm.BTN_RIGHT
        scumm.input(self.x, self.y, b)

        # Keys fire on release, so Start + Coin (exit) sends nothing.
        both = fj.START | fj.SELECT
        if m & both == both:
            self.combo = True
        released = self.last & ~m
        if not m & both:
            if not self.combo:
                if released & fj.START:
                    scumm.key(*KEY_SPACE)
                if released & fj.SELECT:
                    scumm.key(*KEY_F5)
            self.combo = False
        pressed = m & ~self.last
        if pressed & fj.C:
            scumm.key(*KEY_ESC)
        if pressed & fj.START2:
            scumm.key(*KEY_DOT)
        self.last = m

    def frame(self):
        return scumm.frame()

    def error(self):
        return scumm.error()

    def exited(self):
        """The player quit from the game's own menu: back to the launcher."""
        return scumm.error() == "game quit"

    def action(self, name, cfg):
        if name == fj.ACT_STRETCH:
            scumm.key(*KEY_F5)
        elif name == fj.ACT_ROTATE:
            print("SCUMM quick save, slot", QUICK_SLOT)
            scumm.save(QUICK_SLOT)
        elif name == fj.ACT_PALETTE:
            print("SCUMM quick load, slot", QUICK_SLOT)
            scumm.load(QUICK_SLOT)
