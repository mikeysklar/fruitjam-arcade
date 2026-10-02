# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Turn a framebuffer dump from code.py's serial output into a PNG.

    python3 fb2png.py capture.txt out.png

The capture holds a block
    BEGIN_FB <width> <height> <bpp>
    <base64 of the framebuffer bytes, any line length>
    END_FB
anywhere in it (other serial lines are ignored). bpp 16 is RGB565 little
endian, bpp 8 is RGB332. Standard library only.
"""
import base64
import struct
import sys
import zlib


def read_block(path):
    lines = open(path, "r", errors="replace").read().splitlines()
    for i, line in enumerate(lines):
        if line.strip().startswith("BEGIN_FB"):
            w, h, bpp = (int(v) for v in line.split()[1:4])
            data = []
            for body in lines[i + 1:]:
                if body.strip() == "END_FB":
                    return w, h, bpp, base64.b64decode("".join(data))
                data.append(body.strip())
            raise SystemExit("END_FB missing")
    raise SystemExit("BEGIN_FB not found")


def to_rgb(w, h, bpp, raw):
    need = w * h * bpp // 8
    if len(raw) < need:
        raise SystemExit("short framebuffer: %d of %d bytes" % (len(raw), need))
    rows = []
    for y in range(h):
        row = bytearray(b"\x00")  # PNG filter type 0
        for x in range(w):
            if bpp == 16:
                c = raw[2 * (y * w + x)] | (raw[2 * (y * w + x) + 1] << 8)
                r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
                row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
            else:
                c = raw[y * w + x]
                r, g, b = c >> 5, (c >> 2) & 7, c & 3
                row += bytes((r * 255 // 7, g * 255 // 7, b * 255 // 3))
        rows.append(bytes(row))
    return b"".join(rows)


def write_png(path, w, h, rgb):
    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rgb, 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    w, h, bpp, raw = read_block(sys.argv[1])
    write_png(sys.argv[2], w, h, to_rgb(w, h, bpp, raw))
    print("wrote %s (%dx%d, %d bpp)" % (sys.argv[2], w, h, bpp))
