# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Play one Atari Lynx cartridge or homebrew program on a Fruit Jam.

One game per SD card. The first .lnx, .lyx or .o file found on /sd (or on
CIRCUITPY) boots at power on. No menu.

Boot: .lnx carts start through a built in replacement for the Lynx boot ROM
that decrypts the cart's loader (HLE). If lynxboot.img (the real 512 byte
boot ROM, not included) is on /sd or CIRCUITPY it is used instead. .o files
(BS93 homebrew) are loaded straight into RAM. Serial says which happened.

Display: 320x240 RGB565 picodvi framebuffer, which the HSTX doubles to 640x480.
The 160x102 Lynx screen is drawn 2x, 320x204, centered. Games that expect the
Lynx held sideways (rotation flag in the .lnx header) are shown unrotated.

Sound: the TLV320 DAC through adafruit_fruitjam.Peripherals, stereo. The
emulator writes samples into a looping double-buffered RawSample one half
ahead of playback, and frames are paced by the samples each frame made, so
games running at 50, 60 or 75 Hz all play at their own speed. If the DAC or
library is missing it runs silent.

Controls: a USB gamepad (relic_usb_host_gamepad) and/or the three buttons:
  BUTTON1 left, BUTTON2 right, BUTTON3 A
  BUTTON1+BUTTON2 Pause, BUTTON1+BUTTON3 down, BUTTON2+BUTTON3 B
Gamepad: d-pad, A, B, X = Option 1, Y = Option 2, Start = Pause,
Select = Option 1.

Serial prints fps every 5 seconds. Set SCREENSHOT_AT to a frame number to dump
the framebuffer over serial once (tools/fb2png.py turns it into a PNG).
"""

import array
import os
import time

import board
import displayio
import keypad
import lynx
import picodvi

SCREENSHOT_AT = 0  # frame number to print the framebuffer at, 0 = off
SOUND = True
AUDIO_OUTPUT = "headphone"  # or "speaker"
VOLUME = 0.5
HALF_SAMPLES = 1600  # audio ring half (~73 ms); latency ~1 half
EXTS = (".lnx", ".lyx", ".o")


def find_file(match):
    for d in ("/sd", "/"):
        try:
            names = sorted(os.listdir(d))
        except OSError:
            continue
        for n in names:
            if not n.startswith(".") and match(n.lower()):
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
buf = memoryview(fb)  # item size follows the color depth
for i in range(len(buf)):
    buf[i] = 0
del buf


def screenshot():
    """Print the framebuffer as BEGIN_FB w h bpp / base64 lines / END_FB."""
    import binascii

    mv = memoryview(fb)
    item = BPP // 8
    step = 57 * 20 // item  # items per chunk: 20 lines of 76 base64 chars
    print("BEGIN_FB %d %d %d" % (FB_W, FB_H, BPP))
    for i in range(0, len(mv), step):
        text = binascii.b2a_base64(bytes(mv[i:i + step])).rstrip(b"\n")
        for j in range(0, len(text), 76):
            print(text[j:j + 76].decode())
    print("END_FB")


# ---- cartridge -----------------------------------------------------------------
path = find_file(lambda n: n[n.rfind("."):] in EXTS if "." in n else False)
if path is None:
    raise OSError("no .lnx, .lyx or .o file on /sd or CIRCUITPY")
with open(path, "rb") as f:
    rom = f.read()
print("cartridge", path, len(rom), "bytes")

boot = None
boot_path = find_file(lambda n: n == "lynxboot.img")
if boot_path:
    with open(boot_path, "rb") as f:
        boot = f.read(512)
    print("boot ROM", boot_path)

st = bytearray(lynx.STATE_SIZE)
if lynx.init(st, rom, boot):
    raise RuntimeError("not a Lynx image (too small)")
kind, rotation, addr, size, page = lynx.info(st)
if kind == lynx.BOOT_HLE:
    print("boot: HLE decrypted %d loader blocks to $%04X, page size %d" % (size, addr, page))
elif kind == lynx.BOOT_BS93:
    print("boot: BS93 homebrew, %d bytes loaded at $%04X" % (size, addr))
else:
    print("boot: real boot ROM, page size %d" % page)
if rotation:
    print("rotation flag", ("none", "left", "right")[rotation % 3], "(shown unrotated)")

lynx.set_output(st, fb, FB_W, BPP, 0, (FB_H - 2 * lynx.HEIGHT) // 2)

# ---- sound ---------------------------------------------------------------------
RATE = lynx.AUDIO_RATE
RING_FRAMES = 2 * HALF_SAMPLES  # stereo frames in the ring
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
    ring = array.array("h", bytes(2 * RING_FRAMES * 2))
    sample = audiocore.RawSample(ring, channel_count=2, sample_rate=RATE, single_buffer=False)
except Exception as e:  # no DAC, no library, wrong board: play silent
    print("sound off:", e)
    ring = None

# ---- controls ------------------------------------------------------------------
keys = keypad.Keys((board.BUTTON1, board.BUTTON2, board.BUTTON3), value_when_pressed=False, pull=True)
held = [False, False, False]

gamepad = None
pad_mask = 0
PAD_MAP = {}
try:
    import relic_usb_host_gamepad as rg

    gamepad = rg.Gamepad()
    PAD_MAP = {
        rg.BUTTON_UP: lynx.JOYPAD_UP,
        rg.BUTTON_JOYSTICK_UP: lynx.JOYPAD_UP,
        rg.BUTTON_DOWN: lynx.JOYPAD_DOWN,
        rg.BUTTON_JOYSTICK_DOWN: lynx.JOYPAD_DOWN,
        rg.BUTTON_LEFT: lynx.JOYPAD_LEFT,
        rg.BUTTON_JOYSTICK_LEFT: lynx.JOYPAD_LEFT,
        rg.BUTTON_RIGHT: lynx.JOYPAD_RIGHT,
        rg.BUTTON_JOYSTICK_RIGHT: lynx.JOYPAD_RIGHT,
        rg.BUTTON_A: lynx.JOYPAD_A,
        rg.BUTTON_B: lynx.JOYPAD_B,
        rg.BUTTON_X: lynx.JOYPAD_OPT1,
        rg.BUTTON_Y: lynx.JOYPAD_OPT2,
        rg.BUTTON_START: lynx.JOYPAD_PAUSE,
        rg.BUTTON_SELECT: lynx.JOYPAD_OPT1,
    }
except Exception as e:
    print("gamepad off:", e)
    gamepad = None


def joypad():
    global pad_mask
    ev = keys.events.get()
    while ev:
        held[ev.key_number] = ev.pressed
        ev = keys.events.get()
    if gamepad is not None and gamepad.update():
        for e in gamepad.events:
            bit = PAD_MAP.get(e.key_number, 0)
            if e.pressed:
                pad_mask |= bit
            else:
                pad_mask &= ~bit
    b1, b2, b3 = held
    if b1 and b2:
        m = lynx.JOYPAD_PAUSE
    elif b1 and b3:
        m = lynx.JOYPAD_DOWN
    elif b2 and b3:
        m = lynx.JOYPAD_B
    else:
        m = 0
        if b1:
            m |= lynx.JOYPAD_LEFT
        if b2:
            m |= lynx.JOYPAD_RIGHT
        if b3:
            m |= lynx.JOYPAD_A
    m |= pad_mask
    # A real pad cannot press opposite directions; some games break on it.
    if m & lynx.JOYPAD_LEFT and m & lynx.JOYPAD_RIGHT:
        m &= ~(lynx.JOYPAD_LEFT | lynx.JOYPAD_RIGHT)
    if m & lynx.JOYPAD_UP and m & lynx.JOYPAD_DOWN:
        m &= ~(lynx.JOYPAD_UP | lynx.JOYPAD_DOWN)
    return m


# ---- run -----------------------------------------------------------------------
t_play = time.monotonic_ns()
if ring is not None:
    audio.play(sample, loop=True)
    t_play = time.monotonic_ns()
    lynx.set_audio(st, ring, HALF_SAMPLES)  # one half ahead

frames = 0
total = 0
t_emu = 0  # ns inside run_frames
t_input = 0  # ns reading buttons and gamepad
t_report = t_play
emu_samples = 0  # samples made since t_play: the emulated clock
try:
    while True:
        t0 = time.monotonic_ns()
        lynx.set_joypad(st, joypad())
        t1 = time.monotonic_ns()
        lynx.run_frames(st, 1)
        now = time.monotonic_ns()
        emu_samples += lynx.samples(st)
        t_input += t1 - t0
        t_emu += now - t1
        frames += 1
        total += 1
        if total == SCREENSHOT_AT:
            screenshot()  # takes a while; the slow path below re-syncs audio
            now = time.monotonic_ns()
        if now - t_report >= 5_000_000_000:
            print("fps %.1f  emu %.2f ms  input %.2f ms  per frame" % (
                frames * 1e9 / (now - t_report), t_emu / frames / 1e6, t_input / frames / 1e6))
            frames = 0
            t_emu = t_input = 0
            t_report = now
        # Pace to emulated time: the samples made so far at RATE.
        due = t_play + emu_samples * 1_000_000_000 // RATE
        if due > now:
            time.sleep((due - now) / 1e9)
        elif now - due > HALF_SAMPLES * 1_000_000_000 // RATE:
            # More than a half behind the DAC: re-anchor the clock and move
            # the write point one half ahead of the play position, so stale
            # audio is not replayed as noise.
            played = (now - t_play) * RATE // 1_000_000_000
            emu_samples = played
            if ring is not None:
                lynx.set_audio(st, ring, (played + HALF_SAMPLES) % RING_FRAMES)
finally:
    if audio is not None:
        audio.stop()
