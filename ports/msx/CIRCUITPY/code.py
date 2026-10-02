# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Play one MSX1 cartridge on a Fruit Jam with CircuitPython + msx.mpy.

One cartridge per SD card. The first .rom/.mx1/.mx2 file found on /sd (or on
CIRCUITPY) boots at power on. No menu; the launcher in ../launcher has one.

BIOS: C-BIOS 0.29a is built into msx.mpy (open source, BSD-style license, see
src/cbios/LICENSE.txt). It runs cartridge games but has no MSX-BASIC. A dump of
a real 32 KB MSX1 BIOS next to the cartridge as msx.rom is used instead.

Mapper: guessed from the ROM (plain, Konami, Konami SCC, ASCII8, ASCII16).
Set MAPPER below to force one.

Display: 320x240 RGB565 picodvi framebuffer, which the HSTX doubles to 640x480.
The 256x192 picture is drawn 1:1 in the middle; the border shows the VDP
backdrop color like a real TV.

Sound: AY-3-8910 (+ SCC on Konami SCC carts, + key click) through the TLV320
DAC, double-buffered RawSample ring. 368 samples at 22050 Hz per frame.

Controls: USB gamepad and the Fruit Jam buttons, see CONTROLS below. Serial
prints fps every 5 seconds.
"""

import array
import binascii
import os
import sys
import time

import board
import displayio
import keypad
import msx
import picodvi

SOUND = True
AUDIO_OUTPUT = "headphone"  # or "speaker"
VOLUME = 0.5
HALF_FRAMES = 4  # audio ring half = 4 frames (~67 ms); latency ~1 half
SCREENSHOT_AT = 0  # >0: dump the framebuffer over serial after this many frames
MAPPER = None  # None = guess, or "plain" "konami" "scc" "ascii8" "ascii16"

# ---- CONTROLS ------------------------------------------------------------------
# Each button gives joystick 1 bits (msx constants UP DOWN LEFT RIGHT TRIG_A
# TRIG_B) and/or keyboard keys (names in KEYS below, or a letter or digit).
# USB gamepad buttons (relic_usb_host_gamepad names without BUTTON_).
PAD_MAP = {
    "UP": ("UP", "UP"), "DOWN": ("DOWN", "DOWN"),
    "LEFT": ("LEFT", "LEFT"), "RIGHT": ("RIGHT", "RIGHT"),
    "JOYSTICK_UP": ("UP", "UP"), "JOYSTICK_DOWN": ("DOWN", "DOWN"),
    "JOYSTICK_LEFT": ("LEFT", "LEFT"), "JOYSTICK_RIGHT": ("RIGHT", "RIGHT"),
    "A": ("TRIG_A", "SPACE"),
    "B": ("TRIG_B", "M"),
    "X": (None, "N"),
    "Y": (None, "RETURN"),
    "START": (None, "F1"),
    "SELECT": (None, "1"),
}
# Fruit Jam buttons: BUTTON1, BUTTON2, BUTTON3.
BUTTONS = ((None, "SPACE"), (None, "RETURN"), (None, "F1"))

KEYS = {
    "SHIFT": (6, 0), "CTRL": (6, 1), "GRAPH": (6, 2), "CAPS": (6, 3), "CODE": (6, 4),
    "F1": (6, 5), "F2": (6, 6), "F3": (6, 7), "F4": (7, 0), "F5": (7, 1),
    "ESC": (7, 2), "TAB": (7, 3), "STOP": (7, 4), "BS": (7, 5), "SELECT": (7, 6),
    "RETURN": (7, 7), "SPACE": (8, 0), "HOME": (8, 1), "INS": (8, 2), "DEL": (8, 3),
    "LEFT": (8, 4), "UP": (8, 5), "DOWN": (8, 6), "RIGHT": (8, 7),
}
MAPPERS = {"plain": 0, "konami": 1, "scc": 2, "ascii8": 3, "ascii16": 4}


def key(name):
    if name in KEYS:
        return KEYS[name]
    if "0" <= name <= "9":
        return divmod(ord(name) - ord("0"), 8)
    return divmod(22 + ord(name) - ord("A"), 8)


def compile_map(entry):
    joy, k = entry
    return (getattr(msx, joy) if joy else 0), (key(k) if k else None)


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
try:  # the SD card, if the board did not mount it already
    import busio
    import sdcardio
    import storage

    try:
        storage.getmount("/sd")
    except OSError:
        spi = busio.SPI(board.SD_SCK, board.SD_MOSI, board.SD_MISO)
        storage.mount(storage.VfsFat(sdcardio.SDCard(spi, board.SD_CS)), "/sd")
except Exception as e:
    print("no SD card:", e)

path = find_file((".rom", ".mx1", ".mx2"), skip=("msx.rom",))
if path is None:
    raise OSError("no .rom cartridge on /sd or CIRCUITPY")
with open(path, "rb") as f:
    rom = bytearray(f.read())
size = len(rom)
if size % 8192:
    rom.extend(b"\xff" * (8192 - size % 8192))
print("cartridge", path, size, "bytes")

bios = None
bios_path = find_file(("msx.rom",))
if bios_path is not None:
    with open(bios_path, "rb") as f:
        bios = f.read()
    if len(bios) != 32768:
        print("ignoring", bios_path, "(not 32768 bytes)")
        bios = None
    else:
        print("BIOS", bios_path)
if bios is None:
    print("BIOS: built-in C-BIOS 0.29a")

st = bytearray(msx.STATE_SIZE)
err = msx.init(st, rom, bios, MAPPERS[MAPPER] if MAPPER else -1)
if err:
    raise RuntimeError("cartridge rejected (%d), size %d" % (err, size))
print("mapper", ("plain", "konami", "konami scc", "ascii8", "ascii16")[msx.info(st)[0]])
msx.set_output(st, fb, FB_W, BPP, (FB_W - msx.WIDTH) // 2, (FB_H - msx.HEIGHT) // 2)

# ---- sound ---------------------------------------------------------------------
RATE = msx.AUDIO_RATE
PER_FRAME = msx.AUDIO_SAMPLES_PER_FRAME
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
BUTTON_MAP = [compile_map(e) for e in BUTTONS]

gamepad = None
pad_held = set()
PAD = {}
try:
    import relic_usb_host_gamepad as rg

    gamepad = rg.Gamepad()
    for k, v in PAD_MAP.items():
        PAD[getattr(rg, "BUTTON_" + k)] = compile_map(v)
except Exception as e:
    print("gamepad off:", e)
    gamepad = None

matrix = bytearray(11)


def controller():
    ev = keys.events.get()
    while ev:
        held[ev.key_number] = ev.pressed
        ev = keys.events.get()
    if gamepad is not None:
        if gamepad.update():
            for e in gamepad.events:
                if e.pressed:
                    pad_held.add(e.key_number)
                else:
                    pad_held.discard(e.key_number)
    joy = 0
    for i in range(11):
        matrix[i] = 0
    active = [BUTTON_MAP[i] for i in range(3) if held[i]]
    active += [PAD[k] for k in pad_held if k in PAD]
    for j, k in active:
        joy |= j
        if k is not None:
            matrix[k[0]] |= 1 << k[1]
    # The joystick cannot press opposite directions.
    if joy & msx.LEFT and joy & msx.RIGHT:
        joy &= ~(msx.LEFT | msx.RIGHT)
    if joy & msx.UP and joy & msx.DOWN:
        joy &= ~(msx.UP | msx.DOWN)
    return joy


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
    msx.set_audio(st, ring, HALF_FRAMES * PER_FRAME)  # one half ahead

frames = 0
total = 0
t_emu = 0  # ns inside run_frames
t_input = 0  # ns reading buttons and gamepad
t_report = t_play
next_frame = t_play
try:
    while True:
        t0 = time.monotonic_ns()
        msx.set_input(st, controller())
        msx.set_keys(st, matrix)
        t1 = time.monotonic_ns()
        msx.run_frames(st, 1)
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
                msx.set_audio(st, ring, start)
finally:
    if audio is not None:
        audio.stop()
