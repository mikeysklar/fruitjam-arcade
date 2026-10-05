# SPDX-FileCopyrightText: 2026 Mikey Sklar
# SPDX-License-Identifier: GPL-3.0-or-later
"""Launch a game through the Fruit Jam launcher and record the serial output.

    python3 tools/launchcap.py /dev/cu.usbmodemXXXX /sd/cart/game.scumm out.txt [seconds]

Calls fjarcade.launch(path) from the raw REPL (nvm handoff + reload), then
records serial for `seconds`. With FJ_SCREENSHOT set in settings.toml the
capture holds a BEGIN_FB block for tools/fb2png.py.
"""
import sys
import time

import serial

tty, path, outp = sys.argv[1:4]
secs = float(sys.argv[4]) if len(sys.argv) > 4 else 40
s = serial.Serial(tty, 115200, timeout=0.2, write_timeout=10)
for _ in range(5):
    s.write(b"\r\x03\x03")
    time.sleep(1)
    if b">>> " in s.read(65536):
        break
    s.write(b"\r\n")
    time.sleep(1)
    s.read(65536)
s.write(b"\x01")
time.sleep(0.5)
s.read(65536)
s.write(("import sys\nsys.path.insert(0, '/emus')\nimport fjarcade as fj\nfj.launch(%r)\n" % path).encode() + b"\x04")
time.sleep(3)
s.write(b"\x02")  # leave the raw REPL, then soft reboot into code.py
time.sleep(1)
s.write(b"\x04")
out = bytearray()
t = time.time()
while time.time() - t < secs:
    try:
        out += s.read(8192)
    except serial.SerialException:
        time.sleep(0.2)
open(outp, "wb").write(out)
print("got %d bytes%s" % (len(out), ", END_FB" if b"END_FB" in out else ""))
