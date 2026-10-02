# usage: fb2png.py fb.bin out.png [w h]  (raw RGB565 little endian from the sim)
import struct, sys, zlib
d = open(sys.argv[1], "rb").read()
w, h = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (320, 240)
rows = []
for y in range(h):
    r = bytearray([0])
    for x in range(w):
        (p,) = struct.unpack_from("<H", d, 2 * (y * w + x))
        r += bytes(((p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3))
    rows.append(bytes(r))
def chunk(t, b):
    return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b"")
open(sys.argv[2], "wb").write(png)
