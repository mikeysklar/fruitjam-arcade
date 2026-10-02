# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Compare a host_test screenshot with a test ROM's reference PNG, pixel for pixel.

    python3 check_png.py shot.ppm|shot.pgm reference.png

CGB frames (P6, 5-bit channels) are widened with (x << 3) | (x >> 2), the formula
cgb-acid2 asks for. DMG frames (P5, 0..3, 3 = white) are compared as 4 greys.
Stdlib only (zlib), so it runs anywhere python3 does.
"""

import struct
import sys
import zlib


def read_png(path):
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, plte = 8, b"", None
    while pos < len(d):
        n, kind = struct.unpack(">I4s", d[pos : pos + 8])
        body = d[pos + 8 : pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            plte = [tuple(body[i : i + 3]) for i in range(0, n, 3)]
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    assert interlace == 0, "interlaced PNG"
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, chans * depth // 8)
    stride = (w * chans * depth + 7) // 8
    raw = zlib.decompress(idat)
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(line)
        prev = line
    px = []
    for line in rows:
        if depth < 8:
            per = 8 // depth
            vals = [(line[x // per] >> (8 - depth * (x % per + 1))) & ((1 << depth) - 1) for x in range(w)]
        else:
            vals = None
        for x in range(w):
            if ctype == 3:
                px.append(plte[vals[x]])
            elif ctype == 0:
                v = vals[x] if vals else line[x]
                px.append((v, v, v))
            else:
                px.append(tuple(line[x * chans : x * chans + 3]))
    return w, h, depth, ctype, px


def read_pnm(path):
    d = open(path, "rb").read()
    parts = d.split(maxsplit=4)
    kind, w, h, maxv, data = parts[0], int(parts[1]), int(parts[2]), int(parts[3]), parts[4]
    if kind == b"P6":
        px = [tuple(data[i : i + 3]) for i in range(0, w * h * 3, 3)]
        px = [tuple((c << 3) | (c >> 2) for c in p) for p in px]
        return w, h, "rgb", px
    return w, h, "grey", list(data[: w * h])


def main():
    w, h, kind, shot = read_pnm(sys.argv[1])
    rw, rh, depth, ctype, ref = read_png(sys.argv[2])
    assert (w, h) == (rw, rh), "size mismatch"
    if kind == "grey":
        # Reference greys: 2-bit PNG (0..3) or 8-bit; map both to 0..3, 3 = white.
        ref = [p[0] if depth == 2 else p[0] * 3 // 255 for p in ref]
    bad = [i for i in range(w * h) if shot[i] != ref[i]]
    if bad:
        i = bad[0]
        print(f"FAIL {len(bad)} of {w * h} pixels differ, first at ({i % w},{i // w}): got {shot[i]} want {ref[i]}")
        return 1
    print(f"PASS {w}x{h} pixel-exact vs {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
