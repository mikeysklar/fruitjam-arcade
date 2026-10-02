# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Shared pieces for the Fruit Jam console launcher.

Follows the Adafruit Arcade Machines (AAM) conventions so the CircuitPython
consoles feel like John Park's Arduino ones:

- The SD card is the cartridge: ROMs live in /sd/cart.
- Error colours: red = no SD card, yellow = no ROM in /cart,
  magenta = ROM rejected (bad header, wrong console, unsupported mapper).
  The reason is repeated on serial every 2 seconds.
- Battery saves go to /cart/<rom name>.sav once save RAM has been left alone
  for about a second. No save states, no save button.
- Settings per game in /cart/<rom name>.fruitjam.cfg, written 3 s after the
  last change. Plain "key = value" text.
- Onboard buttons are never game inputs: Button 1 stretch, Button 2 rotate,
  Button 3 palette. Each console does what it supports and ignores the rest.
- Arcade pad on the AAM header pins, active low with pull-ups.

An emulator adapter (emu_*.py) exposes a class `Emu` and the runner here
drives it. See emu_nes.py for the reference adapter.
"""

import array
import os
import time

import board
import keypad

# ---- logical buttons (one bitmask for every console) ---------------------------
UP = 1 << 0
DOWN = 1 << 1
LEFT = 1 << 2
RIGHT = 1 << 3
A = 1 << 4  # D10, "shoot"
B = 1 << 5  # A2, action 2
C = 1 << 6  # A1, action 3
START = 1 << 7  # D6, 1P start
START2 = 1 << 8  # D7, 2P start
SELECT = 1 << 9  # A5, coin

# AAM header pins, same order as the bits above.
PAD_PINS = ("A3", "A4", "D8", "D9", "D10", "A2", "A1", "D6", "D7", "A5")

# Onboard buttons: AAM roles.
ACT_STRETCH = "stretch"  # Button 1
ACT_ROTATE = "rotate"  # Button 2
ACT_PALETTE = "palette"  # Button 3

EXIT_COMBO = START | SELECT  # hold to go back to the menu
EXIT_HOLD_NS = 1_000_000_000

RED = 0xF800
YELLOW = 0xFFE0
MAGENTA = 0xF81F

CART_DIR = "/sd/cart"
EMU_DIR = "/emus"
SAVES_FALLBACK = "/saves"

HALF_MS = 67  # audio ring half, ~4 frames at 60 Hz


def screenshot_at():
    """Frame number for a test screenshot, from FJ_SCREENSHOT in settings.toml. 0 = off."""
    try:
        return int(os.getenv("FJ_SCREENSHOT") or 0)
    except ValueError:
        return 0


class RomError(Exception):
    """The ROM was found but rejected. Shown as the magenta screen."""


# ---- video ---------------------------------------------------------------------
class Video:
    """The 320x240 picodvi framebuffer, RGB565 with an RGB332 fallback."""

    W = 320
    H = 240

    def __init__(self, fb, bpp):
        """Wrap the framebuffer code.py made at boot (see make_framebuffer there)."""
        if fb is None:
            raise RuntimeError("no framebuffer (picodvi could not allocate one)")
        self.fb = fb
        self.bpp = bpp
        self.fill(0)

    def fill(self, rgb565):
        buf = memoryview(self.fb)
        if self.bpp == 8:
            rgb565 = ((rgb565 >> 8) & 0xE0) | ((rgb565 >> 6) & 0x1C) | ((rgb565 >> 3) & 0x03)
        for i in range(len(buf)):
            buf[i] = rgb565

    def screenshot(self):
        """Print the framebuffer as BEGIN_FB w h bpp / base64 lines / END_FB.

        Same format as the ports; their tools/fb2png.py turns it into a PNG.
        """
        import binascii

        mv = memoryview(self.fb)
        step = 57 * 20 // (self.bpp // 8)  # items per chunk: 20 lines of 76 base64 chars
        print("BEGIN_FB %d %d %d" % (self.W, self.H, self.bpp))
        for i in range(0, len(mv), step):
            text = binascii.b2a_base64(bytes(mv[i:i + step])).rstrip(b"\n")
            for j in range(0, len(text), 76):
                print(text[j:j + 76].decode())
        print("END_FB")

    def origin(self, w, h):
        """Top-left corner that centres a w x h picture."""
        return (self.W - w) // 2, (self.H - h) // 2


def error_screen(video, colour, reason, pad=None):
    """AAM error screen: fill with the colour, repeat the reason every 2 s.

    Holding Start + Coin still returns to the menu, so a bad ROM picked from
    the menu is not a dead end.
    """
    if video is not None:
        video.fill(colour)
    if pad is None:
        try:
            pad = Pad(usb=False)
        except Exception:
            pass
    t = 0
    while True:
        now = time.monotonic_ns()
        if now - t >= 2_000_000_000:
            print("ERROR:", reason)
            t = now
        if pad is not None:
            pad.poll()
            if pad.exit_requested(now):
                back_to_menu()
        time.sleep(0.02)


# ---- input ---------------------------------------------------------------------
class Pad:
    """Arcade pad on the AAM header, onboard Buttons 1-3, and a USB gamepad.

    poll() returns the logical bitmask. Onboard button presses are queued in
    `actions` (ACT_*), consumed by the caller.
    """

    def __init__(self, usb=True):
        pins = tuple(getattr(board, n) for n in PAD_PINS)
        self.keys = keypad.Keys(pins, value_when_pressed=False, pull=True)
        self.onboard = None
        self.claim_onboard()
        self.gpio = 0
        self.usb_mask = 0
        self.mask = 0
        self.actions = []
        self._exit_since = None
        self.gamepad = None
        self._usb_map = {}
        if usb:
            try:
                import relic_usb_host_gamepad as rg

                self.gamepad = rg.Gamepad()
                self._usb_map = {
                    rg.BUTTON_UP: UP, rg.BUTTON_JOYSTICK_UP: UP,
                    rg.BUTTON_DOWN: DOWN, rg.BUTTON_JOYSTICK_DOWN: DOWN,
                    rg.BUTTON_LEFT: LEFT, rg.BUTTON_JOYSTICK_LEFT: LEFT,
                    rg.BUTTON_RIGHT: RIGHT, rg.BUTTON_JOYSTICK_RIGHT: RIGHT,
                    rg.BUTTON_A: A, rg.BUTTON_B: B,
                    rg.BUTTON_X: C, rg.BUTTON_Y: A,
                    rg.BUTTON_START: START, rg.BUTTON_SELECT: SELECT,
                }
            except Exception as e:
                print("gamepad off:", e)

    def claim_onboard(self):
        self.onboard = keypad.Keys(
            (board.BUTTON1, board.BUTTON2, board.BUTTON3), value_when_pressed=False, pull=True
        )

    def release_onboard(self):
        """Free BUTTON1-3 while adafruit_fruitjam.Peripherals sets up the DAC."""
        if self.onboard is not None:
            self.onboard.deinit()
            self.onboard = None

    def poll(self):
        ev = self.keys.events.get()
        while ev:
            bit = 1 << ev.key_number
            if ev.pressed:
                self.gpio |= bit
            else:
                self.gpio &= ~bit
            ev = self.keys.events.get()
        ev = self.onboard.events.get() if self.onboard else None
        while ev:
            if ev.pressed:
                self.actions.append((ACT_STRETCH, ACT_ROTATE, ACT_PALETTE)[ev.key_number])
            ev = self.onboard.events.get()
        if self.gamepad is not None:
            try:
                if self.gamepad.update():
                    for e in self.gamepad.events:
                        bit = self._usb_map.get(e.key_number, 0)
                        if e.pressed:
                            self.usb_mask |= bit
                        else:
                            self.usb_mask &= ~bit
            except Exception as e:  # unplugged mid-game
                print("gamepad lost:", e)
                self.gamepad = None
                self.usb_mask = 0
        m = self.gpio | self.usb_mask
        # A real pad cannot press opposite directions; some games break on it.
        if m & LEFT and m & RIGHT:
            m &= ~(LEFT | RIGHT)
        if m & UP and m & DOWN:
            m &= ~(UP | DOWN)
        self.mask = m
        return m

    def exit_requested(self, now):
        """True once Start + Coin have been held together for a second."""
        if self.mask & EXIT_COMBO == EXIT_COMBO:
            if self._exit_since is None:
                self._exit_since = now
            return now - self._exit_since >= EXIT_HOLD_NS
        self._exit_since = None
        return False

    def deinit(self):
        self.keys.deinit()
        self.release_onboard()


# ---- audio ---------------------------------------------------------------------
class Audio:
    """TLV320 DAC through adafruit_fruitjam, fed from a looping RawSample ring.

    The ring is `2 * half` sample frames (mono or stereo). The emulator writes
    into it one half ahead of playback. `ring` is None when running silent.
    """

    def __init__(self, rate, channels, half, cfg):
        self.rate = rate
        self.channels = channels
        self.half = half
        self.ring_frames = 2 * half
        self.ring = None
        self.audio = None
        self.sample = None
        self.t_play = time.monotonic_ns()
        if not cfg.get_bool("sound", True):
            print("sound off: cfg")
            return
        try:
            import audiocore
            import digitalio
            import adafruit_fruitjam

            reset = digitalio.DigitalInOut(board.PERIPH_RESET)
            reset.switch_to_output(False)
            time.sleep(0.1)
            reset.value = True
            periphs = adafruit_fruitjam.Peripherals(
                audio_output=cfg.get("audio_output", "headphone"),
                sample_rate=rate, bit_depth=16, i2c=board.I2C(),
            )
            periphs.volume = cfg.get_float("volume", 0.5)
            # Peripherals claims BUTTON1-3 as DigitalInOut; Pad needs them.
            for b in getattr(periphs, "_buttons", ()):
                b.deinit()
            self.audio = periphs.audio
            self.ring = array.array("h", bytes(2 * self.ring_frames * channels))
            self.sample = audiocore.RawSample(
                self.ring, channel_count=channels, sample_rate=rate, single_buffer=False
            )
        except Exception as e:  # no DAC, no library: play silent
            print("sound off:", e)
            self.ring = None

    def start(self):
        if self.ring is not None:
            self.audio.play(self.sample, loop=True)
        self.t_play = time.monotonic_ns()

    def resync_point(self, now):
        """Ring position one half ahead of the estimated play position."""
        played = (now - self.t_play) * self.rate // 1_000_000_000
        return (played + self.half) % self.ring_frames

    def stop(self):
        if self.audio is not None:
            self.audio.stop()


# ---- settings (.fruitjam.cfg) ----------------------------------------------------
class Config:
    """Per-game settings next to the ROM: <rom>.fruitjam.cfg, key = value.

    Written 3 s after the last change, like AAM. Unknown keys are kept.
    """

    DELAY_NS = 3_000_000_000

    def __init__(self, rom_path):
        self.path = rom_path.rsplit(".", 1)[0] + ".fruitjam.cfg"
        self.values = {}
        self._dirty_at = None
        try:
            with open(self.path) as f:
                for line in f:
                    line = line.split("#", 1)[0].strip()
                    if "=" in line:
                        k, v = line.split("=", 1)
                        self.values[k.strip()] = v.strip().strip('"')
        except OSError:
            pass

    def get(self, key, default=None):
        return self.values.get(key, default)

    def get_int(self, key, default=0):
        try:
            return int(self.values[key])
        except (KeyError, ValueError):
            return default

    def get_float(self, key, default=0.0):
        try:
            return float(self.values[key])
        except (KeyError, ValueError):
            return default

    def get_bool(self, key, default=False):
        v = self.values.get(key)
        if v is None:
            return default
        return v.lower() in ("1", "true", "yes", "on")

    def set(self, key, value):
        value = str(value)
        if self.values.get(key) != value:
            self.values[key] = value
            self._dirty_at = time.monotonic_ns()

    def tick(self, now):
        if self._dirty_at is not None and now - self._dirty_at >= self.DELAY_NS:
            self._dirty_at = None
            self.flush()

    def flush(self):
        try:
            with open(self.path, "w") as f:
                for k in sorted(self.values):
                    f.write("%s = %s\n" % (k, self.values[k]))
            print("settings saved", self.path)
        except OSError as e:
            print("settings not saved:", e)


# ---- battery saves (.sav) --------------------------------------------------------
class Saver:
    """Battery RAM to <rom>.sav once it has been left alone for about a second.

    Checked every CHECK_NS by comparing with the last snapshot, so the native
    modules need no dirty flag. If /sd is read-only (a computer has the card
    mounted over USB), falls back to /saves, CircuitPython's code-writable
    partition, and reads from there first next time.
    """

    CHECK_NS = 250_000_000
    QUIET_NS = 1_000_000_000

    def __init__(self, rom_path, size):
        self.cart = bytearray(size) if size else None
        base = rom_path.rsplit("/", 1)[-1].rsplit(".", 1)[0] + ".sav"
        self.path = rom_path.rsplit(".", 1)[0] + ".sav"
        self.fallback = SAVES_FALLBACK + "/" + base
        self._snap = None
        self._saved = None
        self._changed_at = None
        self._t_check = 0
        if self.cart is None:
            return
        from_fallback = False
        # /saves only holds a copy while it is newer than the card's: flush()
        # removes it after every successful write to the card.
        for p in (self.fallback, self.path):
            try:
                with open(p, "rb") as f:
                    f.readinto(self.cart)
                print("loaded", p)
                from_fallback = p == self.fallback
                break
            except OSError:
                pass
        self._snap = bytes(self.cart)
        # Loaded from /saves: leave it marked unsaved so the next flush copies it
        # to the card (and removes the /saves copy), even if the game never writes.
        self._saved = None if from_fallback else self._snap

    def tick(self, now):
        if self.cart is None or now - self._t_check < self.CHECK_NS:
            return
        self._t_check = now
        if self.cart != self._snap:
            self._snap = bytes(self.cart)
            self._changed_at = now
        elif self._changed_at is not None and now - self._changed_at >= self.QUIET_NS:
            self._changed_at = None
            self.flush()

    def flush(self):
        if self.cart is None or self.cart == self._saved:
            return
        for p in (self.path, self.fallback):
            try:
                with open(p, "wb") as f:
                    f.write(self.cart)
                self._saved = bytes(self.cart)
                print("saved", p)
                if p == self.path:
                    # The card copy is now the newest; a /saves copy left from a
                    # read-only time would otherwise be loaded first next boot.
                    try:
                        os.remove(self.fallback)
                    except OSError:
                        pass
                return
            except OSError as e:
                print("save to", p, "failed:", e)


# ---- cartridges ------------------------------------------------------------------
# Extension -> adapter module. .gb goes to the GBC core, which also runs DMG
# games and is MIT-clean. .bin in /cart is taken as an Atari 2600 cart.
SYSTEMS = {
    ".nes": "emu_nes",
    ".gb": "emu_gbc",
    ".gbc": "emu_gbc",
    ".a26": "emu_atari2600",
    ".bin": "emu_atari2600",
    ".a78": "emu_atari7800",
    ".lnx": "emu_lynx",
    ".lyx": "emu_lynx",
    ".o": "emu_lynx",
    ".sms": "emu_sms",
    ".gg": "emu_sms",
    ".sg": "emu_sms",
    ".col": "emu_coleco",
    ".ch8": "emu_chip8",
    ".sc8": "emu_chip8",
    ".rom": "emu_msx",
    ".mx1": "emu_msx",
    ".mx2": "emu_msx",
    ".sv": "emu_supervision",
}

# Files in /cart that are not games (BIOS images the adapters pick up).
NOT_GAMES = ("coleco.rom", "lynxboot.img", "msx.rom")

SYSTEM_NAMES = {
    "emu_nes": "NES", "emu_gbc": "Game Boy", "emu_atari2600": "Atari 2600",
    "emu_atari7800": "Atari 7800", "emu_lynx": "Lynx", "emu_sms": "Sega",
    "emu_coleco": "ColecoVision", "emu_chip8": "CHIP-8",
    "emu_msx": "MSX", "emu_supervision": "Supervision",
}


def ext_of(name):
    i = name.rfind(".")
    return name[i:].lower() if i > 0 else ""


def list_roms(cart_dir=CART_DIR):
    """Sorted list of (path, adapter) for every playable file in /cart."""
    out = []
    for n in sorted(os.listdir(cart_dir), key=lambda n: n.lower()):
        if n.startswith(".") or n.lower() in NOT_GAMES:
            continue
        mod = SYSTEMS.get(ext_of(n))
        if mod:
            out.append((cart_dir + "/" + n, mod))
    return out


def read_rom(path):
    with open(path, "rb") as f:
        return f.read()


# ---- menu handoff ----------------------------------------------------------------
# The chosen ROM survives a reload in microcontroller.nvm: b"FJ" + 1 length
# byte + path. The runner clears it once read, so a crash does not loop.
NVM_MAGIC = b"FJ"
NVM_MENU = b"!menu"


def _nvm():
    import microcontroller

    return microcontroller.nvm


def handoff_write(payload):
    nvm = _nvm()
    data = NVM_MAGIC + bytes((len(payload),)) + payload
    nvm[0:len(data)] = data


def handoff_take():
    """Return and clear the pending payload (bytes), or None."""
    nvm = _nvm()
    if nvm[0:2] != NVM_MAGIC:
        return None
    n = nvm[2]
    payload = bytes(nvm[3:3 + n])
    nvm[0:2] = b"\xff\xff"
    return payload


# The last game picked, so the menu opens on it. Kept further into nvm.
NVM_LAST = 256


def last_write(path):
    nvm = _nvm()
    b = path.encode()[:200]
    data = NVM_MAGIC + bytes((len(b),)) + b
    if bytes(nvm[NVM_LAST:NVM_LAST + len(data)]) != data:  # spare the flash
        nvm[NVM_LAST:NVM_LAST + len(data)] = data


def last_take():
    nvm = _nvm()
    if nvm[NVM_LAST:NVM_LAST + 2] != NVM_MAGIC:
        return None
    n = nvm[NVM_LAST + 2]
    return bytes(nvm[NVM_LAST + 3:NVM_LAST + 3 + n]).decode()


def launch(path):
    import supervisor

    handoff_write(path.encode())
    supervisor.reload()


def back_to_menu():
    import supervisor

    handoff_write(NVM_MENU)
    supervisor.reload()


# ---- runner ----------------------------------------------------------------------
def run(emu, video, pad, cfg, name="emu"):
    """Drive one emulator until Start + Coin is held or the core stops.

    Returns None for Start + Coin, or the core's error text.

    Pacing is by audio produced: a frame is due when the DAC has played the
    samples it made. That covers fixed-rate cores (NES, 60.08 Hz) and cores
    whose frame length varies (Atari 2600, Lynx at 75 Hz) the same way.
    """
    pad.release_onboard()  # Peripherals claims BUTTON1-3 and frees them again
    audio = Audio(emu.AUDIO_RATE, emu.AUDIO_CHANNELS, emu.audio_half(), cfg)
    pad.claim_onboard()
    audio.start()
    if audio.ring is not None:
        emu.set_audio(audio.ring, audio.half)  # one half ahead
    t_anchor = audio.t_play
    s0 = emu.samples()
    half_ns = audio.half * 1_000_000_000 // audio.rate

    frames = 0
    t_emu = 0
    t_report = t_anchor
    result = None
    shot_at = screenshot_at()
    total = 0
    try:
        while True:
            t0 = time.monotonic_ns()
            mask = pad.poll()
            if pad.exit_requested(t0):
                break
            while pad.actions:
                emu.action(pad.actions.pop(0), cfg)
            emu.set_input(mask)
            if not emu.frame():
                if getattr(emu, "exited", None) and emu.exited():
                    break  # the program ended on purpose (SUPER-CHIP 00FD): back to the menu
                result = "%s stopped: %s" % (name, emu.error())
                print(result)
                break
            total += 1
            if total == shot_at:
                video.screenshot()
            now = time.monotonic_ns()
            t_emu += now - t0
            frames += 1
            if now - t_report >= 5_000_000_000:
                print("fps %.1f  emu+input %.2f ms  per frame" % (
                    frames * 1e9 / (now - t_report), t_emu / frames / 1e6))
                frames = 0
                t_emu = 0
                t_report = now
            emu.saver.tick(now)
            cfg.tick(now)
            due = t_anchor + (emu.samples() - s0) * 1_000_000_000 // audio.rate
            if due > now:
                time.sleep((due - now) / 1e9)
            elif now - due > half_ns:
                # Running slow, more than a half behind the DAC: re-anchor and
                # move the write point so stale audio is not replayed as noise.
                if audio.ring is not None:
                    emu.set_audio(audio.ring, audio.resync_point(now))
                t_anchor = now
                s0 = emu.samples()
    finally:
        audio.stop()
        emu.saver.flush()
        if cfg._dirty_at is not None:
            cfg.flush()
    return result
