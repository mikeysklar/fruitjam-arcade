# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Build tiny bankswitching test carts, one per scheme, into a directory.

Each cart starts in its power-on bank, switches the way the scheme does, and
the code it lands on sets COLUBK to a value unique to the target bank before
running a plain VSYNC frame loop. The byte patterns the detector looks for
(E0 hotspot stores, STA $3F, the Activision JSR $D000) are in the code itself.
host_main --banks checks the detected scheme and the background color.

Run: python3 make_banktests.py outdir
"""
import os
import sys

from make_testrom import asm

FRAME = """
frame:  LDA #2
        STA VSYNC
        STA WSYNC
        STA WSYNC
        STA WSYNC
        LDA #0
        STA VSYNC
        LDX #250
fl:     STA WSYNC
        DEX
        BNE fl
        JMP frame
"""


def put(rom, off, data):
    rom[off:off + len(data)] = data


def vectors(rom, bank_off, addr):
    rom[bank_off + 0xFFC] = addr & 0xFF
    rom[bank_off + 0xFFD] = addr >> 8


def f_scheme(nbanks, hot, target, color, sc=False):
    """F8/F6/F4: every bank starts with the same switch instruction; the
    code after it differs per bank so the landing bank shows its color."""
    rom = bytearray([0xFF] * (4096 * nbanks))
    base = 0xF100 if sc else 0xF000
    for b in range(nbanks):
        off = b * 4096
        if sc:
            put(rom, off, bytes(256))           # Superchip RAM area, one fill value
        put(rom, off + (base & 0xFFF), asm("LDA $%04X" % (hot + target), base))
        if sc:
            # Write $5A through the write port, read it back through the read
            # port and use it as the color.
            body = "LDA #$5A\nSTA $1000\nLDA #0\nLDA $1080\nSTA COLUBK\n" + FRAME
        else:
            body = "LDA #$%02X\nSTA COLUBK\n" % (color if b == target else 0x0E) + FRAME
        put(rom, off + (base & 0xFFF) + 3, asm(body, base + 3))
        vectors(rom, off, base)
    return bytes(rom)


def e0():
    # 8 x 1K banks; the last slice ($FC00) is fixed to bank 7.
    rom = bytearray([0xFF] * 8192)
    code = asm("""
        STA $1FE0
        STA $1FE5
        JMP $F000
    """, 0xFC00)
    put(rom, 7 * 1024, code)
    put(rom, 5 * 1024, asm("LDA #$96\nSTA COLUBK\n" + FRAME, 0xF000))
    put(rom, 0 * 1024, asm("LDA #$0E\nSTA COLUBK\n" + FRAME, 0xF000))
    vectors(rom, 4096, 0xFC00)
    return bytes(rom)


def t3f():
    # 4 x 2K banks; $F800-$FFFF fixed to the last one.
    rom = bytearray([0xFF] * 8192)
    code = asm("""
        LDA #2
        STA $3F
        STA $3F
        JMP $F000
    """, 0xF800)
    put(rom, 3 * 2048, code)
    for b in range(3):
        put(rom, b * 2048, asm("LDA #$%02X\nSTA COLUBK\n" % (0x66 if b == 2 else 0x0E) + FRAME, 0xF000))
    vectors(rom, 4096, 0xF800)
    return bytes(rom)


def fe():
    # Activision: JSR $D000 with the stack at $FF lands in the other bank,
    # RTS through $01FE/$01FF comes back.
    rom = bytearray([0xFF] * 8192)
    put(rom, 0, asm("""
        LDX #$FF
        TXS
        JSR $D000
        DEC $C5
        STA COLUBK
    """ + FRAME, 0xF000))
    put(rom, 4096, asm("LDA #$38\nRTS\n", 0xD000))
    vectors(rom, 0, 0xF000)
    vectors(rom, 4096, 0xF000)
    return bytes(rom)


CARTS = {
    # name: (bytes, expected mapper, expected COLUBK)
    "bank_f8.bin": (lambda: f_scheme(2, 0x1FF8, 0, 0x44), "F8", 0x44),
    "bank_f6sc.bin": (lambda: f_scheme(4, 0x1FF6, 1, 0, sc=True), "F6SC", 0x5A),
    "bank_f4.bin": (lambda: f_scheme(8, 0x1FF4, 2, 0x26), "F4", 0x26),
    "bank_e0.bin": (e0, "E0", 0x96),
    "bank_3f.bin": (t3f, "3F", 0x66),
    "bank_fe.bin": (fe, "FE", 0x38),
}

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "banks.txt"), "w") as f:
        for name, (make, mapper, color) in CARTS.items():
            with open(os.path.join(out, name), "wb") as g:
                g.write(make())
            f.write("%s %s %d\n" % (name, mapper, color))
