# SPDX-FileCopyrightText: 2026 Mikey Sklar
# SPDX-License-Identifier: GPL-3.0-or-later
# (from the fruitjam-arcade board tools)
# usage: sdpush.py <tty> <local dir> <dest dir on board> [files...]
# Mounts the SD card over SPI in the REPL, then writes each file in base64 chunks through the
# raw REPL and checks size + sha256 on the board.
import base64, hashlib, os, serial, sys, time

tty, src, dest = sys.argv[1:4]
names = sys.argv[4:] or sorted(n for n in os.listdir(src) if os.path.isfile(os.path.join(src, n)))
s = serial.Serial(tty, 115200, timeout=0.5, write_timeout=10)


def raw(code, wait=20):
    s.write(code.encode() + b"\x04")
    out = b""; t = time.time()
    while time.time() - t < wait:
        out += s.read(4096)
        if out.count(b"\x04") >= 2:
            break
    o = out.decode(errors="replace")
    body, _, err = o.partition("\x04")
    err = err.split("\x04")[0].strip()
    if err:
        raise RuntimeError(err)
    return body.lstrip("OK").strip()


s.write(b"\r\x03\x03"); time.sleep(1); s.read(65536)
s.write(b"\r\x01"); time.sleep(0.5); s.read(65536)
print(raw("""
import os, storage, board, busio, sdcardio, binascii, hashlib
try:
    storage.getmount('/sd')
except OSError:
    _spi = busio.SPI(board.SD_SCK, board.SD_MOSI, board.SD_MISO)
    _sd = sdcardio.SDCard(_spi, board.SD_CS, baudrate=25_000_000)
    storage.mount(storage.VfsFat(_sd), '/sd')
def _mk(p):
    cur = ''
    for part in p.strip('/').split('/'):
        cur += '/' + part
        try:
            os.mkdir(cur)
        except OSError:
            pass
_mk(%r)
print('mounted', os.statvfs('/sd')[0] * os.statvfs('/sd')[2] // (1 << 30), 'GiB')
""" % dest))
CH = 3 * 1024
t0 = time.time(); total = 0
for n in names:
    data = open(os.path.join(src, n), "rb").read()
    path = dest.rstrip("/") + "/" + n
    raw("_f = open(%r, 'wb')" % path)
    for i in range(0, len(data), CH):
        b = base64.b64encode(data[i:i + CH]).decode()
        raw("_f.write(binascii.a2b_base64(%r))" % b)
    raw("_f.close()")
    got = raw("""
_h = hashlib.new('sha256')
with open(%r, 'rb') as _g:
    while True:
        _c = _g.read(4096)
        if not _c:
            break
        _h.update(_c)
print(os.stat(%r)[6], binascii.hexlify(_h.digest()).decode())
""" % (path, path))
    want = "%d %s" % (len(data), hashlib.sha256(data).hexdigest())
    print("%-24s %s" % (n, "OK" if got == want else "MISMATCH got %s want %s" % (got, want)))
    total += len(data)
print("%d bytes in %.0f s" % (total, time.time() - t0))
s.write(b"\x02")
