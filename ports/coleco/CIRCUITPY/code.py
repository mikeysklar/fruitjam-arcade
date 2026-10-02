# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Play one ColecoVision cartridge on a Fruit Jam with CircuitPython + coleco.mpy.

One cartridge per SD card. The first .col file found on /sd (or on CIRCUITPY)
boots at power on. No menu.

BIOS: a small open BIOS is built in. It boots cartridges that drive the
hardware themselves (most homebrew, e.g. anything made with CVBasic) but has
no title screen and none of the Coleco OS7 sound, controller or graphics
library that commercial cartridges call. If you own a ColecoVision, put a dump
of its 8 KB BIOS next to the cartridge as coleco.rom and it is used instead.

Display: 320x240 RGB565 picodvi framebuffer, which the HSTX doubles to 640x480.
The 256x192 picture is drawn 1:1 in the middle; the border shows the VDP
backdrop color like a real TV.

Sound: SN76489 through the TLV320 DAC, same double-buffered RawSample ring as
the nes/ port. 368 samples at 22050 Hz per frame = 59.92 fps, the real rate.

Controls: see CONTROLS below; edit the tables to taste. Serial prints fps
every 5 seconds.
"""

import array
import os
import sys
import time

import binascii
import board
import coleco
import displayio
import keypad
import picodvi

SOUND = True
AUDIO_OUTPUT = "headphone"  # or "speaker"
VOLUME = 0.5
HALF_FRAMES = 4  # audio ring half = 4 frames (~67 ms); latency ~1 half
SCREENSHOT_AT = 0  # >0: dump the framebuffer over serial after this many frames

# ---- CONTROLS ------------------------------------------------------------------
# ColecoVision controller: joystick, left and right fire, keypad 0-9 * #.
# Names on the right are coleco constants: UP DOWN LEFT RIGHT FIRE_L FIRE_R
# KEY_0..KEY_9 KEY_STAR KEY_HASH.
#
# USB gamepad buttons (relic_usb_host_gamepad names without BUTTON_).
PAD_MAP = {
    "UP": "UP", "DOWN": "DOWN", "LEFT": "LEFT", "RIGHT": "RIGHT",
    "JOYSTICK_UP": "UP", "JOYSTICK_DOWN": "DOWN",
    "JOYSTICK_LEFT": "LEFT", "JOYSTICK_RIGHT": "RIGHT",
    "A": "FIRE_L",
    "B": "FIRE_R",
    "X": "KEY_STAR",
    "Y": "KEY_HASH",
    "START": "KEY_1",
    "SELECT": "KEY_2",
}
# While SELECT is held these replace the normal meaning of a button, and
# SELECT's own key (2) is not sent. Covers the rest of the keypad.
PAD_SHIFT = {
    "START": "KEY_3",
    "A": "KEY_4",
    "B": "KEY_5",
    "X": "KEY_0",
    "Y": "KEY_6",
    "UP": "KEY_7",
    "RIGHT": "KEY_8",
    "DOWN": "KEY_9",
    "LEFT": "KEY_0",
}
# Fruit Jam buttons: single presses, then two-button chords.
BUTTONS = ("LEFT", "RIGHT", "FIRE_L")  # BUTTON1, BUTTON2, BUTTON3
CHORDS = {
    (0, 1): "KEY_1",  # BUTTON1 + BUTTON2
    (0, 2): "KEY_2",  # BUTTON1 + BUTTON3
    (1, 2): "FIRE_R",  # BUTTON2 + BUTTON3
}


def bits(name):
    return getattr(coleco, name)


def find_file(exts, skip=()):
    for d in ("/sd", "/"):
        try:
            names = sorted(os.listdir(d))
        except OSError:
            continue
        for n in names:
            low = n.lower()
            if low.endswith(exts) and not n.startswith(".") and low not in skip:
                return d.rstrip("/") + "/" + n
    return None


# ---- display -------------------------------------------------------------------
displayio.release_displays()
PINS = dict(
    clk_dp=board.CKP, clk_dn=board.CKN, red_dp=board.D0P, red_dn=board.D0N,
    green_dp=board.D1P, green_dn=board.D1N, blue_dp=board.D2P, blue_dn=board.D2N,
)
FB_W, FB_H = 320, 240
try:
    fb = picodvi.Framebuffer(FB_W, FB_H, color_depth=16, **PINS)
    BPP = 16
except (MemoryError, ValueError):
    fb = picodvi.Framebuffer(FB_W, FB_H, color_depth=8, **PINS)
    BPP = 8

# ---- cartridge and BIOS --------------------------------------------------------
path = find_file((".col",))
if path is None:
    raise OSError("no .col cartridge on /sd or CIRCUITPY")
with open(path, "rb") as f:
    rom = bytearray(f.read())
size = len(rom)
unit = 16384 if size > 32768 else 8192
if size % unit:
    rom.extend(b"\xff" * (unit - size % unit))
print("cartridge", path, size, "bytes")

bios = None
bios_path = find_file(("coleco.rom",))
if bios_path is not None:
    with open(bios_path, "rb") as f:
        bios = f.read()
    if len(bios) != 8192:
        print("ignoring", bios_path, "(not 8192 bytes)")
        bios = None
    else:
        print("BIOS", bios_path)
if bios is None:
    print("BIOS: built-in open BIOS (homebrew only)")

st = bytearray(coleco.STATE_SIZE)
if coleco.init(st, rom, bios):
    raise RuntimeError("bad cartridge size %d" % size)
if not coleco.header_ok(st):
    print("warning: no AA55/55AA cartridge header, it may not boot")
coleco.set_output(st, fb, FB_W, BPP, (FB_W - coleco.WIDTH) // 2, (FB_H - coleco.HEIGHT) // 2)

# ---- sound ---------------------------------------------------------------------
RATE = coleco.AUDIO_RATE
PER_FRAME = coleco.AUDIO_SAMPLES_PER_FRAME
FRAME_NS = PER_FRAME * 1_000_000_000 // RATE
ring = None
audio = None
try:
    if not SOUND:
        raise RuntimeError("SOUND = False")
    import audiocore
    import digitalio
    import adafruit_fruitjam

    reset = digitalio.DigitalInOut(board.PERIPH_RESET)
    reset.switch_to_output(False)
    time.sleep(0.1)
    reset.value = True
    periphs = adafruit_fruitjam.Peripherals(
        audio_output=AUDIO_OUTPUT, sample_rate=RATE, bit_depth=16, i2c=board.I2C()
    )
    periphs.volume = VOLUME
    # Peripherals claims BUTTON1-3 as DigitalInOut; hand the pins to keypad.
    for b in getattr(periphs, "_buttons", ()):
        b.deinit()
    audio = periphs.audio
    ring = array.array("h", bytes(2 * HALF_FRAMES * PER_FRAME * 4))
    sample = audiocore.RawSample(ring, channel_count=2, sample_rate=RATE, single_buffer=False)
except Exception as e:  # no DAC, no library, wrong board: play silent
    print("sound off:", e)
    ring = None

RING_FRAMES = 2 * HALF_FRAMES * PER_FRAME  # stereo frames in the ring

# ---- controls ------------------------------------------------------------------
keys = keypad.Keys((board.BUTTON1, board.BUTTON2, board.BUTTON3), value_when_pressed=False, pull=True)
held = [False, False, False]
BUTTON_BITS = [bits(n) for n in BUTTONS]
CHORD_BITS = [(a, b, bits(n)) for (a, b), n in CHORDS.items()]

gamepad = None
pad_held = set()
PAD_BITS = {}
SHIFT_BITS = {}
SELECT = None
try:
    import relic_usb_host_gamepad as rg

    gamepad = rg.Gamepad()
    for k, v in PAD_MAP.items():
        PAD_BITS[getattr(rg, "BUTTON_" + k)] = bits(v)
    for k, v in PAD_SHIFT.items():
        SHIFT_BITS[getattr(rg, "BUTTON_" + k)] = bits(v)
    SELECT = rg.BUTTON_SELECT
except Exception as e:
    print("gamepad off:", e)
    gamepad = None


def controller():
    ev = keys.events.get()
    while ev:
        held[ev.key_number] = ev.pressed
        ev = keys.events.get()
    m = 0
    for a, b, bit in CHORD_BITS:
        if held[a] and held[b]:
            m |= bit
    if not m:
        for i in range(3):
            if held[i]:
                m |= BUTTON_BITS[i]
    if gamepad is not None:
        if gamepad.update():
            for e in gamepad.events:
                if e.pressed:
                    pad_held.add(e.key_number)
                else:
                    pad_held.discard(e.key_number)
        shift = SELECT in pad_held and len(pad_held) > 1
        for k in pad_held:
            if shift:
                if k != SELECT:
                    m |= SHIFT_BITS.get(k, PAD_BITS.get(k, 0))
            else:
                m |= PAD_BITS.get(k, 0)
    # The joystick cannot press opposite directions.
    if m & coleco.LEFT and m & coleco.RIGHT:
        m &= ~(coleco.LEFT | coleco.RIGHT)
    if m & coleco.UP and m & coleco.DOWN:
        m &= ~(coleco.UP | coleco.DOWN)
    return m


def screenshot():
    # 114 bytes per chunk encode to two 76 char lines; only the last chunk
    # is short, so base64 padding only appears at the very end.
    mv = memoryview(fb)  # items are pixels, 1 or 2 bytes
    step = 114 // (BPP // 8)
    sys.stdout.write("BEGIN_FB %d %d %d\n" % (FB_W, FB_H, BPP))
    for i in range(0, len(mv), step):
        s = binascii.b2a_base64(mv[i:i + step]).decode().rstrip()
        sys.stdout.write(s[:76] + "\n")
        if len(s) > 76:
            sys.stdout.write(s[76:] + "\n")
    sys.stdout.write("END_FB\n")


# ---- run -----------------------------------------------------------------------
t_play = time.monotonic_ns()
if ring is not None:
    audio.play(sample, loop=True)
    t_play = time.monotonic_ns()
    coleco.set_audio(st, ring, HALF_FRAMES * PER_FRAME)  # one half ahead

frames = 0
total = 0
t_emu = 0  # ns inside run_frames
t_input = 0  # ns reading buttons and gamepad
t_report = t_play
next_frame = t_play
try:
    while True:
        t0 = time.monotonic_ns()
        coleco.set_input(st, controller())
        t1 = time.monotonic_ns()
        coleco.run_frames(st, 1)
        now = time.monotonic_ns()
        t_input += t1 - t0
        t_emu += now - t1
        frames += 1
        total += 1
        if total == SCREENSHOT_AT:
            screenshot()
            now = next_frame = time.monotonic_ns()
        if now - t_report >= 5_000_000_000:
            print("fps %.1f  emu %.2f ms  input %.2f ms  per frame" % (
                frames * 1e9 / (now - t_report), t_emu / frames / 1e6, t_input / frames / 1e6))
            frames = 0
            t_emu = t_input = 0
            t_report = now
        next_frame += FRAME_NS
        if next_frame > now:
            time.sleep((next_frame - now) / 1e9)
        else:
            # Running slow. Re-anchor pacing and, if we fell more than a half
            # behind the DAC, move the write point back one half ahead of the
            # estimated play position so stale audio is not replayed as noise.
            behind = (now - next_frame) // FRAME_NS
            next_frame = now
            if ring is not None and behind >= HALF_FRAMES:
                played = (now - t_play) * RATE // 1_000_000_000
                start = (played + HALF_FRAMES * PER_FRAME) % RING_FRAMES
                coleco.set_audio(st, ring, start)
finally:
    if audio is not None:
        audio.stop()
