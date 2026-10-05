# SPDX-FileCopyrightText: 2026 Mikey Sklar
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run a Python file on the board through the raw REPL and stream its output.

    python3 tools/rawrun.py /dev/cu.usbmodemXXXX file.py [seconds] [NAME=value ...]

NAME=value pairs are prepended as assignments (e.g. GAME="/scumm/loomdemo").
Output is printed as it arrives and also saved to /tmp/rawrun.out.
"""
import sys
import time

import serial

tty, path = sys.argv[1], sys.argv[2]
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 60
pre = "".join("%s = %s\n" % tuple(a.split("=", 1)) for a in sys.argv[4:])
src = pre + open(path).read()
s = serial.Serial(tty, 115200, timeout=0.2, write_timeout=10)


def wait_for(token, secs):
    buf = bytearray()
    t = time.time()
    while time.time() - t < secs:
        buf += s.read(4096)
        if token in buf:
            return True
    return False


for attempt in range(5):
    s.write(b"\r\x03\x03")
    if wait_for(b">>> ", 3):
        break
    s.write(b"\r\n")  # "Press any key to enter the REPL"
    if wait_for(b">>> ", 3):
        break
else:
    sys.exit("no REPL prompt")
s.write(b"\x01")
if not wait_for(b"raw REPL; CTRL-B to exit\r\n>", 3):
    sys.exit("no raw REPL")
for i in range(0, len(src), 256):
    s.write(src[i:i + 256].encode())
    time.sleep(0.01)
s.write(b"\x04")
out = bytearray()
t = time.time()
while time.time() - t < secs:
    b = s.read(4096)
    if b:
        out += b
        sys.stdout.write(b.decode(errors="replace").replace("\x04", "\n[EOT]\n"))
        sys.stdout.flush()
        if out.count(b"\x04") >= 2:
            break
s.write(b"\x03\x02")
open("/tmp/rawrun.out", "wb").write(out)
