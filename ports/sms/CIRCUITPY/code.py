# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Play one Master System, Game Gear or SG-1000 cartridge on a Fruit Jam.

One ROM per SD card. The first .sms, .gg or .sg file found on /sd (or on
CIRCUITPY) boots at power on. The extension picks the system. No menu.

Display: 320x240 RGB565 picodvi framebuffer, which the HSTX doubles to 640x480.
Master System and SG-1000 (256x192) are drawn 1:1, centered. Game Gear
(160x144) is doubled horizontally and scaled 1.5x vertically to 320x216,
centered. A 2x picture would be 320x288, taller than the buffer.
The emulator writes straight into the framebuffer, no displayio refresh.

Sound: the TLV320 DAC through adafruit_fruitjam.Peripherals. The emulator writes
each frame's samples into a looping double-buffered RawSample, one half ahead
of playback. Frames are paced to exactly one audio frame (368 samples at
22050 Hz = 59.92 Hz, the NTSC frame rate). If the DAC or library is missing it
runs silent.

Controls: a USB gamepad (relic_usb_host_gamepad) and/or the three buttons:
  BUTTON1 left, BUTTON2 right, BUTTON3 button 1
  BUTTON1+BUTTON2 Pause (Start on Game Gear)
  BUTTON1+BUTTON3 down, BUTTON2+BUTTON3 button 2

Saves: games with battery RAM save to <rom>.sav next to the ROM, every 30
seconds when it changed, and on exit.

Serial prints fps every 5 seconds. Set SCREENSHOT_AT to a frame number to dump
the framebuffer over serial once (tools/fb2png.py turns it into a PNG).
"""

import array
import os
import time

import board
import displayio
import keypad
import picodvi
import sms

SCREENSHOT_AT = 0  # frame number to print the framebuffer at, 0 = off
SOUND = True
AUDIO_OUTPUT = "headphone"  # or "speaker"
VOLUME = 0.5
HALF_FRAMES = 4  # audio ring half = 4 frames (~67 ms); latency ~1 half
SAVE_EVERY_NS = 30_000_000_000
SYSTEMS = {".sms": sms.SYSTEM_SMS, ".gg": sms.SYSTEM_GG, ".sg": sms.SYSTEM_SG}


def find_rom():
    for d in ("/sd", "/"):
        try:
            names = sorted(os.listdir(d))
        except OSError:
            continue
        for n in names:
            ext = n[n.rfind("."):].lower() if "." in n else ""
            if ext in SYSTEMS and not n.startswith("."):
                return d.rstrip("/") + "/" + n, SYSTEMS[ext]
    raise OSError("no .sms, .gg or .sg cartridge on /sd or CIRCUITPY")


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
path, system = find_rom()
with open(path, "rb") as f:
    rom = f.read()
print("cartridge", path, len(rom), "bytes")

st = bytearray(sms.STATE_SIZE)
if sms.init(st, rom, system):
    raise RuntimeError("ROM too small")
system, mapper, rom_size = sms.info(st)
print("system", ("SMS", "Game Gear", "SG-1000")[system],
      "mapper", ("none", "Sega", "Codemasters")[mapper])

save_path = path.rsplit(".", 1)[0] + ".sav"
cart = None
saved = None
if sms.save_size(st):
    cart = bytearray(sms.save_size(st))
    try:
        with open(save_path, "rb") as f:
            f.readinto(cart)
        print("loaded", save_path)
    except OSError:
        pass
    sms.set_cart_ram(st, cart)
    saved = bytes(cart)


def write_save():
    global saved
    if cart is None or not sms.cart_ram_used(st) or cart == saved:
        return
    try:
        with open(save_path, "wb") as f:
            f.write(cart)
        saved = bytes(cart)
        print("saved", save_path)
    except OSError as e:
        print("save failed", e)


sms.set_output(st, fb, FB_W, FB_H, BPP)

# ---- sound ---------------------------------------------------------------------
RATE = sms.AUDIO_RATE
PER_FRAME = sms.AUDIO_SAMPLES_PER_FRAME
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

gamepad = None
pad_mask = 0
PAD_MAP = {}
try:
    import relic_usb_host_gamepad as rg

    gamepad = rg.Gamepad()
    PAD_MAP = {
        rg.BUTTON_UP: sms.JOYPAD_UP,
        rg.BUTTON_JOYSTICK_UP: sms.JOYPAD_UP,
        rg.BUTTON_DOWN: sms.JOYPAD_DOWN,
        rg.BUTTON_JOYSTICK_DOWN: sms.JOYPAD_DOWN,
        rg.BUTTON_LEFT: sms.JOYPAD_LEFT,
        rg.BUTTON_JOYSTICK_LEFT: sms.JOYPAD_LEFT,
        rg.BUTTON_RIGHT: sms.JOYPAD_RIGHT,
        rg.BUTTON_JOYSTICK_RIGHT: sms.JOYPAD_RIGHT,
        rg.BUTTON_B: sms.JOYPAD_1,  # left face button, like the SMS pad
        rg.BUTTON_A: sms.JOYPAD_2,
        rg.BUTTON_Y: sms.JOYPAD_1,
        rg.BUTTON_X: sms.JOYPAD_2,
        rg.BUTTON_START: sms.JOYPAD_START,
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
        m = sms.JOYPAD_START
    elif b1 and b3:
        m = sms.JOYPAD_DOWN
    elif b2 and b3:
        m = sms.JOYPAD_2
    else:
        m = 0
        if b1:
            m |= sms.JOYPAD_LEFT
        if b2:
            m |= sms.JOYPAD_RIGHT
        if b3:
            m |= sms.JOYPAD_1
    m |= pad_mask
    # A real pad cannot press opposite directions; some games break on it.
    if m & sms.JOYPAD_LEFT and m & sms.JOYPAD_RIGHT:
        m &= ~(sms.JOYPAD_LEFT | sms.JOYPAD_RIGHT)
    if m & sms.JOYPAD_UP and m & sms.JOYPAD_DOWN:
        m &= ~(sms.JOYPAD_UP | sms.JOYPAD_DOWN)
    return m


# ---- run -----------------------------------------------------------------------
t_play = time.monotonic_ns()
if ring is not None:
    audio.play(sample, loop=True)
    t_play = time.monotonic_ns()
    sms.set_audio(st, ring, HALF_FRAMES * PER_FRAME)  # one half ahead

frames = 0
total = 0
t_emu = 0  # ns inside run_frames
t_input = 0  # ns reading buttons and gamepad
t_report = t_play
t_save = t_play
next_frame = t_play
try:
    while True:
        t0 = time.monotonic_ns()
        sms.set_joypad(st, joypad())
        t1 = time.monotonic_ns()
        sms.run_frames(st, 1)
        now = time.monotonic_ns()
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
        if cart is not None and now - t_save >= SAVE_EVERY_NS:
            t_save = now
            write_save()
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
                sms.set_audio(st, ring, start)
finally:
    if audio is not None:
        audio.stop()
    write_save()
