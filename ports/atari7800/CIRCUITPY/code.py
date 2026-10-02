# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Play one Atari 7800 cartridge on a Fruit Jam with CircuitPython + atari7800.mpy.

One ROM per SD card: the first .a78 file found on /sd (or on CIRCUITPY) boots
at power on; if there is none, the first headerless .bin. No menu, no BIOS
(the cartridge starts at its reset vector, as emulators that skip the BIOS do).

Display: 320x240 RGB565 picodvi framebuffer, which the HSTX doubles to 640x480.
MARIA's 160 pixel modes are drawn 2x wide, 320 modes 1:1. 240 of the 243
lines MARIA can display are shown, starting at scanline TOP.

Sound: TIA (and POKEY, when the cart header asks for one) through the TLV320
DAC via adafruit_fruitjam.Peripherals. The emulator appends mono samples to a
looping double-buffered RawSample, one half ahead of playback. Frames are
paced by the number of samples made. Without the DAC or library it runs silent.

Controls: a USB gamepad (relic_usb_host_gamepad) and/or the three buttons:
  BUTTON1 left, BUTTON2 right, BUTTON3 fire (left button)
  BUTTON1+BUTTON2 reset, BUTTON2+BUTTON3 up, BUTTON1+BUTTON3 down,
  all three: pause
  gamepad: d-pad/stick, A/X left button, B/Y right button, Start reset,
  Select select, L1/R1 pause, L2/R2 toggle left/right difficulty

Serial prints fps every 5 seconds.
"""

import array
import os
import sys
import time

import atari7800
import binascii
import board
import displayio
import keypad
import picodvi

SOUND = True
AUDIO_OUTPUT = "headphone"  # or "speaker"
VOLUME = 0.5
TOP = 17  # first scanline shown on screen row 0 (MARIA displays lines 17-257)
LEFT_DIFFICULTY_A = False  # console switches at power on
RIGHT_DIFFICULTY_A = False
HALF = 4 * 368  # audio ring half, ~4 frames (~67 ms); latency ~1 half
SCREENSHOT_AT = 0  # >0: dump the framebuffer over serial after this many frames


def find_rom():
    for exts in ((".a78",), (".bin",)):
        for d in ("/sd", "/"):
            try:
                names = sorted(os.listdir(d))
            except OSError:
                continue
            for n in names:
                if n.lower().endswith(exts) and not n.startswith("."):
                    return d.rstrip("/") + "/" + n
    raise OSError("no .a78 or .bin cartridge on /sd or CIRCUITPY")


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

# ---- cartridge -----------------------------------------------------------------
path = find_rom()
with open(path, "rb") as f:
    rom = f.read()
print("cartridge", path, len(rom), "bytes")

st = bytearray(atari7800.STATE_SIZE)
err = atari7800.init(st, rom)
if err == 2:
    raise RuntimeError("unsupported cartridge mapper (Activision, Absolute, banksets, ...)")
if err:
    raise RuntimeError("unsupported cartridge size %d" % len(rom))
mapper, rom_bytes, pokey, pal = atari7800.info(st)
print("mapper %s, %d KB ROM%s%s" % (
    mapper, rom_bytes // 1024, ", POKEY at $%X" % pokey if pokey else "", ", PAL" if pal else ""))

if pal:
    TOP += 25  # PAL MARIA displays 292 lines; show the middle 240
atari7800.set_output(st, fb, FB_W, FB_H, BPP, TOP)

# ---- sound ---------------------------------------------------------------------
RATE = atari7800.AUDIO_RATE
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
    ring = array.array("h", bytes(2 * 2 * HALF))
    sample = audiocore.RawSample(ring, channel_count=1, sample_rate=RATE, single_buffer=False)
except Exception as e:  # no DAC, no library, wrong board: play silent
    print("sound off:", e)
    ring = None

RING_LEN = 2 * HALF  # samples in the ring

# ---- controls ------------------------------------------------------------------
UP, DOWN = atari7800.INPUT_UP, atari7800.INPUT_DOWN
LEFT, RIGHT = atari7800.INPUT_LEFT, atari7800.INPUT_RIGHT
FIRE1, FIRE2 = atari7800.INPUT_FIRE1, atari7800.INPUT_FIRE2
RESET, SELECT, PAUSE = atari7800.INPUT_RESET, atari7800.INPUT_SELECT, atari7800.INPUT_PAUSE
LEFT_A, RIGHT_A = atari7800.INPUT_LEFT_A, atari7800.INPUT_RIGHT_A

keys = keypad.Keys((board.BUTTON1, board.BUTTON2, board.BUTTON3), value_when_pressed=False, pull=True)
held = [False, False, False]

# Difficulty switches stay set; L2/R2 on the gamepad toggle them.
switches = (LEFT_A if LEFT_DIFFICULTY_A else 0) | (RIGHT_A if RIGHT_DIFFICULTY_A else 0)

gamepad = None
pad_mask = 0
PAD_MAP = {}
PAD_TOGGLE = {}
try:
    import relic_usb_host_gamepad as rg

    gamepad = rg.Gamepad()
    PAD_MAP = {
        rg.BUTTON_UP: UP,
        rg.BUTTON_JOYSTICK_UP: UP,
        rg.BUTTON_DOWN: DOWN,
        rg.BUTTON_JOYSTICK_DOWN: DOWN,
        rg.BUTTON_LEFT: LEFT,
        rg.BUTTON_JOYSTICK_LEFT: LEFT,
        rg.BUTTON_RIGHT: RIGHT,
        rg.BUTTON_JOYSTICK_RIGHT: RIGHT,
        rg.BUTTON_A: FIRE1,
        rg.BUTTON_X: FIRE1,
        rg.BUTTON_B: FIRE2,
        rg.BUTTON_Y: FIRE2,
        rg.BUTTON_START: RESET,
        rg.BUTTON_SELECT: SELECT,
        rg.BUTTON_L1: PAUSE,
        rg.BUTTON_R1: PAUSE,
    }
    PAD_TOGGLE = {
        rg.BUTTON_L2: LEFT_A,
        rg.BUTTON_R2: RIGHT_A,
    }
except Exception as e:
    print("gamepad off:", e)
    gamepad = None


def controls():
    global pad_mask, switches
    ev = keys.events.get()
    while ev:
        held[ev.key_number] = ev.pressed
        ev = keys.events.get()
    if gamepad is not None and gamepad.update():
        for e in gamepad.events:
            bit = PAD_MAP.get(e.key_number, 0)
            if e.pressed:
                pad_mask |= bit
                t = PAD_TOGGLE.get(e.key_number, 0)
                if t:
                    switches ^= t
                    print("difficulty: left %s, right %s" % (
                        "A" if switches & LEFT_A else "B",
                        "A" if switches & RIGHT_A else "B"))
            else:
                pad_mask &= ~bit
    b1, b2, b3 = held
    if b1 and b2 and b3:
        m = PAUSE
    elif b1 and b2:
        m = RESET
    elif b2 and b3:
        m = UP
    elif b1 and b3:
        m = DOWN
    else:
        m = 0
        if b1:
            m |= LEFT
        if b2:
            m |= RIGHT
        if b3:
            m |= FIRE1
    return m | pad_mask | switches


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
    atari7800.set_audio(st, ring, HALF)  # one half ahead
t_anchor = t_play  # pacing: samples made since s0 are due from t_anchor
s0 = atari7800.samples(st)

frames = 0
total = 0
t_emu = 0  # ns inside run_frames
t_input = 0  # ns reading buttons and gamepad
t_report = t_play
try:
    while True:
        t0 = time.monotonic_ns()
        atari7800.set_input(st, controls())
        t1 = time.monotonic_ns()
        if atari7800.run_frames(st, 1) != 1:
            print("CPU jammed at", hex(atari7800.error(st)))
            break
        now = time.monotonic_ns()
        t_input += t1 - t0
        t_emu += now - t1
        frames += 1
        total += 1
        if total == SCREENSHOT_AT:
            screenshot()
            now = time.monotonic_ns()
        if now - t_report >= 5_000_000_000:
            print("fps %.1f  emu %.2f ms  input %.2f ms  per frame" % (
                frames * 1e9 / (now - t_report), t_emu / frames / 1e6, t_input / frames / 1e6))
            frames = 0
            t_emu = t_input = 0
            t_report = now
        # Pace by audio made: the frame is due when the DAC has played it.
        made = atari7800.samples(st) - s0
        due = t_anchor + made * 1_000_000_000 // RATE
        if due > now:
            time.sleep((due - now) / 1e9)
        elif now - due > HALF * 1_000_000_000 // RATE:
            # Running slow, more than a half behind the DAC. Re-anchor pacing
            # and move the write point back one half ahead of the estimated
            # play position so stale audio is not replayed as noise.
            if ring is not None:
                played = (now - t_play) * RATE // 1_000_000_000
                atari7800.set_audio(st, ring, (played + HALF) % RING_LEN)
            t_anchor = now
            s0 = atari7800.samples(st)
finally:
    if audio is not None:
        audio.stop()
