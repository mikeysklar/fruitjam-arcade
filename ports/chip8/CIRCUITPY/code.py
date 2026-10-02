# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Play one CHIP-8 or SUPER-CHIP program on a Fruit Jam with CircuitPython + chip8.mpy.

The first .ch8 or .sc8 file found on /sd (or on CIRCUITPY) boots at power on.
No menu. .ch8 runs with classic CHIP-8 quirks, .sc8 with SUPER-CHIP (modern)
quirks. Both can be overridden per ROM in ROMS below.

Display: 320x240 picodvi framebuffer (RGB565, RGB332 fallback), which the HSTX
doubles to 640x480. The 64x32 screen is drawn at 5x (320x160) and SUPER-CHIP
hires 128x64 at 2x (256x128), centered. The interpreter writes straight into
the framebuffer, and only on frames where the screen changed.

Sound: the TLV320 DAC through adafruit_fruitjam.Peripherals. A square wave
while the sound timer runs, written into a looping double-buffered RawSample
one half ahead of playback. If the DAC or library is missing it runs silent.

Controls: CHIP-8 has a 16-key hex keypad. A USB gamepad (relic_usb_host_gamepad)
and the three buttons produce logical controls ("up", "a", ...) and KEYMAP turns
those into hex keys. See README.md for the default layout.

Serial prints fps every 5 seconds.
"""

import array
import os
import time

import board
import chip8
import displayio
import keypad
import picodvi

SOUND = True
AUDIO_OUTPUT = "headphone"  # or "speaker"
VOLUME = 0.5
BEEP_HZ = 440
BEEP_LEVEL = 4000  # 0..32767
HALF_FRAMES = 4  # audio ring half = 4 frames (~67 ms)
FG = (0xFF, 0xFF, 0xFF)  # lit pixel
BG = (0x00, 0x00, 0x00)

# Instructions per 60 Hz frame. ~11 matches the original COSMAC VIP for most
# CHIP-8 games; SUPER-CHIP games expect more.
IPF_CHIP8 = 11
IPF_SCHIP = 30

# Logical control -> CHIP-8 hex key. The CHIP-8 keypad and the keys most
# games use (W A S D E Q on a PC map to 5 7 8 9 6 4):
#   1 2 3 C
#   4 5 6 D
#   7 8 9 E
#   A 0 B F
KEYMAP = {
    "up": 0x5,
    "down": 0x8,
    "left": 0x7,
    "right": 0x9,
    "a": 0x6,
    "b": 0x4,
    "x": 0x1,
    "y": 0x2,
    "l1": 0xA,
    "r1": 0xB,
    "select": 0xE,
    "start": 0xF,
}

# Per ROM settings, keyed by the file name without extension, lower case.
# "keys" entries replace the matching KEYMAP entries; "quirks" and "ipf"
# replace the defaults. These are examples from the classic public domain
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


def find_rom():
    for d in ("/sd", "/"):
        try:
            names = sorted(os.listdir(d))
        except OSError:
            continue
        for n in names:
            low = n.lower()
            if (low.endswith(".ch8") or low.endswith(".sc8")) and not n.startswith("."):
                return d.rstrip("/") + "/" + n
    raise OSError("no .ch8 or .sc8 program on /sd or CIRCUITPY")


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


def pixel(rgb):
    r, g, b = rgb
    if BPP == 16:
        return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return (r & 0xE0) | ((g & 0xE0) >> 3) | (b >> 6)


# ---- program -------------------------------------------------------------------
path = find_rom()
with open(path, "rb") as f:
    rom = f.read()
print("program", path, len(rom), "bytes")

name = path.rsplit("/", 1)[-1].rsplit(".", 1)
stem = name[0].lower()
schip = len(name) > 1 and name[1].lower() == "sc8"
opts = ROMS.get(stem, {})
quirks = opts.get("quirks", chip8.QUIRKS_SCHIP if schip else chip8.QUIRKS_CHIP8)
ipf = opts.get("ipf", IPF_SCHIP if schip else IPF_CHIP8)
keymap = dict(KEYMAP)
keymap.update(opts.get("keys", {}))
print("quirks 0x%02x  ipf %d" % (quirks, ipf))

st = bytearray(chip8.STATE_SIZE)
seed = int.from_bytes(os.urandom(4), "little") | 1
if chip8.init(st, rom, seed):
    raise RuntimeError("program too big (max 3584 bytes)")
chip8.config(st, quirks, ipf)
chip8.set_colors(st, pixel(FG), pixel(BG))
chip8.set_output(st, fb, FB_W, FB_H, BPP)
chip8.set_beep(st, BEEP_HZ, BEEP_LEVEL)

# ---- sound ---------------------------------------------------------------------
RATE = chip8.AUDIO_RATE
PER_FRAME = chip8.AUDIO_SAMPLES_PER_FRAME
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
# BUTTON1 left, BUTTON2 right, BUTTON3 a; chords: 1+2 up, 1+3 down, 2+3 b.
keys = keypad.Keys((board.BUTTON1, board.BUTTON2, board.BUTTON3), value_when_pressed=False, pull=True)
held = [False, False, False]


def bit(control):
    k = keymap.get(control)
    return 0 if k is None else 1 << k


B_UP, B_DOWN, B_LEFT, B_RIGHT, B_A, B_B = (bit(c) for c in ("up", "down", "left", "right", "a", "b"))

gamepad = None
pad_mask = 0
PAD_MAP = {}
try:
    import relic_usb_host_gamepad as rg

    gamepad = rg.Gamepad()
    PAD_MAP = {
        rg.BUTTON_UP: bit("up"),
        rg.BUTTON_JOYSTICK_UP: bit("up"),
        rg.BUTTON_DOWN: bit("down"),
        rg.BUTTON_JOYSTICK_DOWN: bit("down"),
        rg.BUTTON_LEFT: bit("left"),
        rg.BUTTON_JOYSTICK_LEFT: bit("left"),
        rg.BUTTON_RIGHT: bit("right"),
        rg.BUTTON_JOYSTICK_RIGHT: bit("right"),
        rg.BUTTON_A: bit("a"),
        rg.BUTTON_B: bit("b"),
        rg.BUTTON_X: bit("x"),
        rg.BUTTON_Y: bit("y"),
        rg.BUTTON_L1: bit("l1"),
        rg.BUTTON_R1: bit("r1"),
        rg.BUTTON_START: bit("start"),
        rg.BUTTON_SELECT: bit("select"),
    }
except Exception as e:
    print("gamepad off:", e)
    gamepad = None


def keypad_mask():
    global pad_mask
    ev = keys.events.get()
    while ev:
        held[ev.key_number] = ev.pressed
        ev = keys.events.get()
    if gamepad is not None and gamepad.update():
        for e in gamepad.events:
            b = PAD_MAP.get(e.key_number, 0)
            if e.pressed:
                pad_mask |= b
            else:
                pad_mask &= ~b
    b1, b2, b3 = held
    if b1 and b2:
        m = B_UP
    elif b1 and b3:
        m = B_DOWN
    elif b2 and b3:
        m = B_B
    else:
        m = 0
        if b1:
            m |= B_LEFT
        if b2:
            m |= B_RIGHT
        if b3:
            m |= B_A
    return m | pad_mask


# ---- run -----------------------------------------------------------------------
t_play = time.monotonic_ns()
if ring is not None:
    audio.play(sample, loop=True)
    t_play = time.monotonic_ns()
    chip8.set_audio(st, ring, HALF_FRAMES * PER_FRAME)  # one half ahead

frames = 0
t_emu = 0  # ns inside run_frames
t_input = 0  # ns reading buttons and gamepad
t_report = t_play
next_frame = t_play
try:
    while True:
        t0 = time.monotonic_ns()
        chip8.set_keys(st, keypad_mask())
        t1 = time.monotonic_ns()
        if chip8.run_frames(st, 1) != 1:
            code, addr = chip8.error(st)
            if code == chip8.ERR_EXIT:
                print("program exited")
            else:
                print("interpreter error %d at 0x%03x" % (code, addr))
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
                chip8.set_audio(st, ring, start)
finally:
    if audio is not None:
        audio.stop()
