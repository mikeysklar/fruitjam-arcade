# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Build testcart.a78, a 32K MARIA test cartridge checked by run_tests.sh.

    python3 make_testcart.py testcart.a78

One 16 line zone per test, each with a DLI that sets CTRL and BACKGRND for
the zone from a table. The expected pixels are worked out by hand from the
MARIA documentation (see the comments) and written to testcart.expect,
which host_main --check reads.

Memory: $8000-$9FFF is 32 copies of one 256 byte graphics page, so every
zone line (page hi + offset) sees the same bytes, and the holey DMA zones
can use hi=$88. $A000 holds the display lists, char map and DLL, $F000 the
code.
"""
import sys

CTRL, BACKGRND, DPPH, DPPL, CHARBASE, MSTAT = 0x3C, 0x20, 0x2C, 0x30, 0x34, 0x28
SWCHA, INPT1 = 0x280, 0x09
AUDC0, AUDF0, AUDV0 = 0x15, 0x17, 0x19


def PC(p, c):
    return 0x20 + p * 4 + c


def color(p, c):   # value written to P<p>C<c>: hue p+1, luminance by c
    return ((p + 1) << 4) | (c * 4 + 2)


# ---- tiny assembler ---------------------------------------------------------
OPS = {  # mnemonic mode -> opcode; modes: imp imm zp abs absx rel
    ("sei", "imp"): 0x78, ("cld", "imp"): 0xD8, ("txs", "imp"): 0x9A, ("tax", "imp"): 0xAA,
    ("txa", "imp"): 0x8A, ("pha", "imp"): 0x48, ("pla", "imp"): 0x68, ("rti", "imp"): 0x40,
    ("ldx", "imm"): 0xA2, ("lda", "imm"): 0xA9, ("sta", "zp"): 0x85, ("sta", "abs"): 0x8D,
    ("lda", "zp"): 0xA5, ("lda", "abs"): 0xAD, ("lda", "absx"): 0xBD, ("ldx", "zp"): 0xA6,
    ("inc", "zp"): 0xE6, ("bit", "zp"): 0x24, ("bpl", "rel"): 0x10, ("bmi", "rel"): 0x30,
    ("jmp", "abs"): 0x4C,
}
SIZE = {"imp": 1, "imm": 2, "zp": 2, "abs": 3, "absx": 3, "rel": 2}


def assemble(org, prog):
    labels, pc = {}, org
    for item in prog:
        if isinstance(item, str):
            labels[item] = pc
        else:
            pc += SIZE[item[1]]
    out = bytearray()
    pc = org
    for item in prog:
        if isinstance(item, str):
            continue
        op, mode = item[0], item[1]
        arg = item[2] if len(item) > 2 else None
        if isinstance(arg, str):
            arg = labels[arg]
        out.append(OPS[(op, mode)])
        if mode == "rel":
            out.append((arg - (pc + 2)) & 0xFF)
        elif SIZE[mode] == 2:
            out.append(arg & 0xFF)
        elif SIZE[mode] == 3:
            out += bytes((arg & 0xFF, arg >> 8))
        pc += SIZE[mode]
    return out, labels


# ---- graphics page ($8000-$9FFF, all pages identical) --------------------------
page = bytearray(256)
page[0x00:0x02] = bytes((0x1B, 0xE4))   # 160A: colors 0 1 2 3 | 3 2 1 0
page[0x10] = 0x9C                         # 160B / 320B / 320C / 320D test byte
page[0x20] = 0xA5                         # 320A: 1010 0101
page[0x40:0x43] = bytes((0xFF, 0x55, 0xAA))   # char graphics

# ---- zones ------------------------------------------------------------------------
# Each zone: (ctrl, backgrnd, dll flags, dll hi of graphics use, display list bytes)
# CTRL: $40 = DMA on, +3 read mode 320A/C, +2 read mode 320B/D, +$10 two byte
# chars, +4 kangaroo.
def hdr4(lo, pal, width, hi, hpos):
    return bytes((lo, (pal << 5) | ((-width) & 0x1F), hi, hpos))


def hdr5(lo, wm, ind, pal, width, hi, hpos):
    return bytes((lo, (wm << 7) | 0x40 | (ind << 5), hi, (pal << 5) | ((-width) & 0x1F), hpos))


zones = [
    # 0: blank
    (0x40, 0x00, 0, b""),
    # 1: 160A direct, two objects; the second overlaps with a transparent cell
    (0x40, 0x00, 0, hdr5(0x00, 0, 0, 1, 2, 0x80, 10) + hdr4(0x00, 2, 1, 0x80, 12)),
    # 2: 160B
    (0x40, 0x00, 0, hdr5(0x10, 1, 0, 4, 1, 0x80, 20)),
    # 3: 320A (read mode 3, write mode 0)
    (0x43, 0x00, 0, hdr5(0x20, 0, 0, 1, 1, 0x80, 30)),
    # 4: 320B (read mode 2, write mode 1)
    (0x42, 0x00, 0, hdr5(0x10, 1, 0, 4, 1, 0x80, 30)),
    # 5: 320C (read mode 3, write mode 1)
    (0x43, 0x00, 0, hdr5(0x10, 1, 0, 4, 1, 0x80, 30)),
    # 6: 320D (read mode 2, write mode 0)
    (0x42, 0x00, 0, hdr5(0x10, 0, 0, 5, 1, 0x80, 30)),
    # 7: indirect, one byte chars from the char map at $A000
    (0x40, 0x00, 0, hdr5(0x00, 0, 1, 3, 2, 0xA0, 40)),
    # 8: indirect, two byte chars
    (0x50, 0x00, 0, hdr5(0x00, 0, 1, 3, 2, 0xA0, 40)),
    # 9: holey DMA 16 (DLL bit 6), graphics hi = $88
    (0x40, 0x00, 0x40, hdr5(0x00, 0, 0, 1, 2, 0x88, 10)),
    # 10: holey DMA 8 (DLL bit 5)
    (0x40, 0x00, 0x20, hdr5(0x00, 0, 0, 1, 2, 0x88, 10)),
    # 11: kangaroo: $FF under $1B at the same place
    (0x44, 0x00, 0, hdr5(0x40, 0, 0, 1, 1, 0x80, 60) + hdr4(0x00, 1, 1, 0x80, 60)),
    # 12: background color change by DLI, no objects
    (0x40, 0xD4, 0, b""),
    # 13: palette 0 shows SWCHA (C1) and INPT1 (C2), set in VBLANK
    (0x40, 0x00, 0, hdr4(0x00, 0, 1, 0x80, 70)),
]
FILL = 3   # blank zones after the tests

data = bytearray(b"\xff" * 0x1000)   # $A000-$AFFF
data[0x000:0x002] = bytes((0x40, 0x41))   # char map
dl_addr, dll = 0x100, bytearray()
for k, (ctrl, bg, flags, dl) in enumerate(zones + [(0x40, 0x00, 0, b"")] * FILL):
    a = 0xA000 + dl_addr
    data[dl_addr:dl_addr + len(dl) + 2] = dl + b"\x00\x00"
    dl_addr += len(dl) + 2
    dll += bytes((0x80 | flags | 15, a >> 8, a & 0xFF))
DLL = 0xA000 + 0x800
data[0x800:0x800 + len(dll)] = dll
ctrl_tab = bytes(z[0] for z in zones) + b"\x40" * FILL
bg_tab = bytes(z[1] for z in zones) + b"\x00" * FILL
data[0x900:0x900 + len(ctrl_tab)] = ctrl_tab
data[0xA00:0xA00 + len(bg_tab)] = bg_tab

# ---- code ---------------------------------------------------------------------------
K = 0x40   # zone counter (zero page RAM)
prog = [
    "reset", ("sei", "imp"), ("cld", "imp"), ("ldx", "imm", 0xFF), ("txs", "imp"),
    ("lda", "imm", 0x7F), ("sta", "zp", CTRL),
    ("lda", "imm", DLL >> 8), ("sta", "zp", DPPH), ("lda", "imm", DLL & 0xFF), ("sta", "zp", DPPL),
    ("lda", "imm", 0x80), ("sta", "zp", CHARBASE),
    ("lda", "imm", 4), ("sta", "zp", AUDC0), ("lda", "imm", 15), ("sta", "zp", AUDF0),
    ("lda", "imm", 8), ("sta", "zp", AUDV0),
]
for p in range(8):
    for c in (1, 2, 3):
        prog += [("lda", "imm", color(p, c)), ("sta", "zp", PC(p, c))]
prog += [
    ("lda", "imm", 0x40), ("sta", "zp", CTRL),
    "frame", ("bit", "zp", MSTAT), ("bpl", "rel", "frame"),   # wait for VBLANK
    ("lda", "imm", 0), ("sta", "zp", K),
    ("lda", "abs", SWCHA), ("sta", "zp", PC(0, 1)),
    ("lda", "zp", INPT1), ("sta", "zp", PC(0, 2)),
    "vb", ("bit", "zp", MSTAT), ("bmi", "rel", "vb"),        # wait for the display
    ("jmp", "abs", "frame"),
    "nmi", ("pha", "imp"), ("txa", "imp"), ("pha", "imp"),
    ("ldx", "zp", K), ("lda", "absx", 0xA900), ("sta", "zp", CTRL),
    ("lda", "absx", 0xAA00), ("sta", "zp", BACKGRND), ("inc", "zp", K),
    ("pla", "imp"), ("tax", "imp"), ("pla", "imp"), ("rti", "imp"),
]
code, labels = assemble(0xF000, prog)

rom = bytearray(b"\xff" * 0x8000)
for i in range(32):
    rom[i * 256:(i + 1) * 256] = page
rom[0x2000:0x3000] = data
rom[0x7000:0x7000 + len(code)] = code
rom[0x7FFA:0x8000] = bytes((labels["nmi"] & 0xFF, labels["nmi"] >> 8,
                            labels["reset"] & 0xFF, labels["reset"] >> 8,
                            labels["nmi"] & 0xFF, labels["nmi"] >> 8))

hdr = bytearray(128)
hdr[0] = 3
hdr[1:10] = b"ATARI7800"
hdr[17:17 + 13] = b"MARIA testcart"[:13]
hdr[49:53] = len(rom).to_bytes(4, "big")
hdr[53:55] = b"\x00\x00"
hdr[55] = hdr[56] = 1
hdr[100:128] = b"ACTUAL CART DATA STARTS HERE"

# ---- expected pixels: (zone, row in zone, x in 320 pixels, color byte or "bg") ------
# 160 modes: cell n covers pixels 2n and 2n+1.
exp = []
def cells(z, row, first, colors):
    for i, col in enumerate(colors):
        exp.append((z, row, 2 * (first + i), col))
        exp.append((z, row, 2 * (first + i) + 1, col))

# 1: $1B -> 0 1 2 3, $E4 -> 3 2 1 0 at cell 10 in palette 1; then $1B at cell
# 12 in palette 2: cell 12 keeps P1C2 (color 0 is transparent), 13-15 P2C1-3.
cells(1, 8, 9, ["bg", "bg", color(1, 1), color(1, 2), color(2, 1), color(2, 2), color(2, 3),
                color(1, 1), "bg"])
# 2: 160B $9C = 1001 1100: cell 0 color D7D6=2, palette P2|D3D2 = 4|3 = 7;
# cell 1 color D5D4=1, palette 4|D1D0 = 4.
cells(2, 8, 20, [color(7, 2), color(4, 1), "bg"])
# 3: 320A $A5 at cell 30: pixels 1 0 1 0 0 1 0 1 in P1C2.
for i, b in enumerate((1, 0, 1, 0, 0, 1, 0, 1)):
    exp.append((3, 8, 60 + i, color(1, 2) if b else "bg"))
# 4: 320B $9C: pixel pairs (D7,D3)(D6,D2)(D5,D1)(D4,D0) = 3 1 0 2, palette 4.
for i, c in enumerate((3, 1, 0, 2)):
    exp.append((4, 8, 60 + i, color(4, c) if c else "bg"))
# 5: 320C $9C: D7-D4 = 1 0 0 1 enable color 2; palettes 4|D3D2 = 7 for the
# first two pixels, 4|D1D0 = 4 for the last two.
for i, c in enumerate((color(7, 2), "bg", "bg", color(4, 2))):
    exp.append((5, 8, 60 + i, c))
# 6: 320D $9C with header palette 5 (P2=1, P1P0=01), as MAME renders it: four
# cells 2 1 3 0 in palette 5 read back in 320 mode as pixel pairs.
# cell 0x16 -> (0x12, 0x11), 0x15 -> (0x10, 0x13), 0x17 -> (0x12, 0x13); 0x14
# has color 0, so it is transparent and the cell stays 0.
for i, s in enumerate((0x12, 0x11, 0x10, 0x13, 0x12, 0x13, 0x00, 0x00)):
    exp.append((6, 8, 60 + i, color(4, s & 3) if s & 3 else "bg"))
# 7: chars $40 ($FF) and $41 ($55) at cell 40 in palette 3.
cells(7, 8, 40, [color(3, 3)] * 4 + [color(3, 1)] * 4 + ["bg"])
# 8: two byte chars: $40 -> $FF $55, $41 -> $55 $AA.
cells(8, 8, 40, [color(3, 3)] * 4 + [color(3, 1)] * 8 + [color(3, 2)] * 4 + ["bg"])
# 9: holey 16, hi $88: rows 0-7 read pages $97-$90 (A12 set) -> holes.
cells(9, 2, 11, ["bg"] * 3)
cells(9, 12, 11, [color(1, 1), color(1, 2), color(1, 3)])
# 10: holey 8: rows 8-15 read pages $8F-$88 (A11 set) -> holes.
cells(10, 2, 11, [color(1, 1), color(1, 2), color(1, 3)])
cells(10, 12, 11, ["bg"] * 3)
# 11: kangaroo: the second object's color 0 cell overwrites with background.
cells(11, 8, 60, ["bg", color(1, 1), color(1, 2), color(1, 3)])
# 12: background from the DLI.
exp.append((12, 8, 100, 0xD4))
exp.append((12, 8, 0, 0xD4))
# 13: SWCHA and INPT1 through palette 0 (no input: $FF and $00).
cells(13, 8, 71, ["swcha", "inpt1"])

out = sys.argv[1]
with open(out, "wb") as f:
    f.write(hdr + rom)
with open(out.rsplit(".", 1)[0] + ".expect", "w") as f:
    for z, row, x, c in exp:
        f.write("%d %d %d %s\n" % (z, row, x, c if isinstance(c, str) else "%02x" % c))
print("wrote", out, len(exp), "pixel checks")
