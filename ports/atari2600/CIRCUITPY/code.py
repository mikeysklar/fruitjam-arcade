# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Play one Atari 2600 cartridge on a Fruit Jam with CircuitPython + atari2600.mpy.

One ROM per SD card: the first .a26 or .bin file found on /sd (or on
CIRCUITPY) boots at power on. No menu.

Display: 320x240 RGB565 picodvi framebuffer, which the HSTX doubles to 640x480.
Each 160 pixel TIA line is drawn 2x wide, 240 of the ~262 NTSC lines are shown.

Sound: the TLV320 DAC through adafruit_fruitjam.Peripherals. The emulator
appends mono samples to a looping double-buffered RawSample, one half ahead of
playback. Frames are paced by the number of samples made, so production matches
the DAC. If the DAC or library is missing it runs silent.

Controls: a USB gamepad (relic_usb_host_gamepad) and/or the three buttons:
  BUTTON1 left, BUTTON2 right, BUTTON3 fire
  BUTTON1+BUTTON2 game reset, BUTTON2+BUTTON3 up, BUTTON1+BUTTON3 down,
  all three: game select
  gamepad: d-pad/stick, A/B/X/Y fire, Start reset, Select select,
  L1/R1 toggle left/right difficulty, L2/R2/Home toggle color/B&W

Serial prints fps every 5 seconds.
"""

import array
import os
import time

import atari2600
import board
import displayio
import keypad
import picodvi

SOUND = True
AUDIO_OUTPUT = "headphone"  # or "speaker"
VOLUME = 0.5
TOP = 12  # first scanline after VSYNC shown on screen row 0
LEFT_DIFFICULTY_A = False  # console switches at power on
RIGHT_DIFFICULTY_A = False
BLACK_AND_WHITE = False
HALF = 4 * 368  # audio ring half, ~4 frames (~67 ms); latency ~1 half


def find_rom():
    for d in ("/sd", "/"):
        try:
            names = sorted(os.listdir(d))
        except OSError:
            continue
        for n in names:
            low = n.lower()
            if (low.endswith(".a26") or low.endswith(".bin")) and not n.startswith("."):
                return d.rstrip("/") + "/" + n
    raise OSError("no .a26 or .bin cartridge on /sd or CIRCUITPY")


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
buf = memoryview(fb)  # item size follows the color depth
for i in range(len(buf)):
    buf[i] = 0
del buf

# ---- cartridge -----------------------------------------------------------------
path = find_rom()
with open(path, "rb") as f:
    rom = f.read()
print("cartridge", path, len(rom), "bytes")

st = bytearray(atari2600.STATE_SIZE)
err = atari2600.init(st, rom)
if err == 2:
    raise RuntimeError("DPC+/CDF (ARM) cartridges are not supported")
if err:
    raise RuntimeError("unsupported cartridge size %d" % len(rom))
print("bankswitching", atari2600.mapper(st))

atari2600.set_output(st, fb, FB_W, FB_H, BPP, TOP)

# ---- sound ---------------------------------------------------------------------
RATE = atari2600.AUDIO_RATE
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
UP, DOWN = atari2600.INPUT_UP, atari2600.INPUT_DOWN
LEFT, RIGHT = atari2600.INPUT_LEFT, atari2600.INPUT_RIGHT
FIRE, RESET, SELECT = atari2600.INPUT_FIRE, atari2600.INPUT_RESET, atari2600.INPUT_SELECT
LEFT_A, RIGHT_A, BW = atari2600.INPUT_LEFT_A, atari2600.INPUT_RIGHT_A, atari2600.INPUT_BW

keys = keypad.Keys((board.BUTTON1, board.BUTTON2, board.BUTTON3), value_when_pressed=False, pull=True)
held = [False, False, False]

# Switch states that stay set (difficulty, color/B&W), toggled from the gamepad.
switches = (LEFT_A if LEFT_DIFFICULTY_A else 0) | (RIGHT_A if RIGHT_DIFFICULTY_A else 0)
switches |= BW if BLACK_AND_WHITE else 0

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
        rg.BUTTON_A: FIRE,
        rg.BUTTON_B: FIRE,
        rg.BUTTON_X: FIRE,
        rg.BUTTON_Y: FIRE,
        rg.BUTTON_START: RESET,
        rg.BUTTON_SELECT: SELECT,
    }
    PAD_TOGGLE = {
        rg.BUTTON_L1: LEFT_A,
        rg.BUTTON_R1: RIGHT_A,
        rg.BUTTON_L2: BW,
        rg.BUTTON_R2: BW,
        rg.BUTTON_HOME: BW,
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
                    print("switches: left %s, right %s, %s" % (
                        "A" if switches & LEFT_A else "B",
                        "A" if switches & RIGHT_A else "B",
                        "B&W" if switches & BW else "color"))
            else:
                pad_mask &= ~bit
    b1, b2, b3 = held
    if b1 and b2 and b3:
        m = SELECT
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
            m |= FIRE
    return m | pad_mask | switches


# ---- run -----------------------------------------------------------------------
t_play = time.monotonic_ns()
if ring is not None:
    audio.play(sample, loop=True)
    t_play = time.monotonic_ns()
    atari2600.set_audio(st, ring, HALF)  # one half ahead
t_anchor = t_play  # pacing: samples made since s0 are due from t_anchor
s0 = atari2600.samples(st)

frames = 0
t_emu = 0  # ns inside run_frames
t_input = 0  # ns reading buttons and gamepad
t_report = t_play
try:
    while True:
        t0 = time.monotonic_ns()
        atari2600.set_input(st, controls())
        t1 = time.monotonic_ns()
        if atari2600.run_frames(st, 1) != 1:
            print("CPU jammed at", hex(atari2600.error(st)))
            break
        now = time.monotonic_ns()
        t_input += t1 - t0
        t_emu += now - t1
        frames += 1
        if now - t_report >= 5_000_000_000:
            print("fps %.1f  emu %.2f ms  input %.2f ms  per frame" % (
                frames * 1e9 / (now - t_report), t_emu / frames / 1e6, t_input / frames / 1e6))
            frames = 0
            t_emu = t_input = 0
            t_report = now
        # Pace by audio made: the frame is due when the DAC has played it.
        made = atari2600.samples(st) - s0
        due = t_anchor + made * 1_000_000_000 // RATE
        if due > now:
            time.sleep((due - now) / 1e9)
        elif now - due > HALF * 1_000_000_000 // RATE:
            # Running slow, more than a half behind the DAC. Re-anchor pacing
            # and move the write point back one half ahead of the estimated
            # play position so stale audio is not replayed as noise.
            if ring is not None:
                played = (now - t_play) * RATE // 1_000_000_000
                atari2600.set_audio(st, ring, (played + HALF) % RING_LEN)
            t_anchor = now
            s0 = atari2600.samples(st)
finally:
    if audio is not None:
        audio.stop()
