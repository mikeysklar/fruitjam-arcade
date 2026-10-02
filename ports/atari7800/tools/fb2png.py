# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Turn a serial capture with a BEGIN_FB block into a PNG. Standard library only.

    python3 fb2png.py capture.txt out.png

code.py prints, when SCREENSHOT_AT is set:
    BEGIN_FB <width> <height> <bpp>
    <base64 of the framebuffer bytes, 76 chars per line>
    END_FB
bpp 16 is RGB565 little endian, bpp 8 is RGB332.
"""
import binascii
import struct
import sys
import zlib


def read_block(path):
    lines = open(path, "r", errors="replace").read().splitlines()
    for i, line in enumerate(lines):
        line = line.strip()
        if line.startswith("BEGIN_FB"):
            _, w, h, bpp = line.split()[:4]
            data = []
            for body in lines[i + 1:]:
                body = body.strip()
                if body.startswith("END_FB"):
                    break
                data.append(body)
            return int(w), int(h), int(bpp), binascii.a2b_base64("".join(data))
    raise SystemExit("no BEGIN_FB block in " + path)


def to_rgb(w, h, bpp, raw):
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            if bpp == 16:
                i = (y * w + x) * 2
                c = raw[i] | (raw[i + 1] << 8)
                r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
                row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
            else:
                c = raw[y * w + x]
                r, g, b = (c >> 5) & 7, (c >> 2) & 7, c & 3
                row += bytes((r * 255 // 7, g * 255 // 7, b * 255 // 3))
        rows.append(bytes(row))
    return rows


def write_png(path, w, h, rows):
    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + r for r in rows)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    w, h, bpp, raw = read_block(sys.argv[1])
    need = w * h * bpp // 8
    if len(raw) < need:
        raise SystemExit("block has %d bytes, need %d" % (len(raw), need))
    write_png(sys.argv[2], w, h, to_rgb(w, h, bpp, raw))
    print("wrote %s (%dx%d, %d bpp)" % (sys.argv[2], w, h, bpp))


main()
