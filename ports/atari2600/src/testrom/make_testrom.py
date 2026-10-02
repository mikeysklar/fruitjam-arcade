# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Build testrom.bin, a 4K Atari 2600 test cartridge, with a tiny assembler.

The ROM draws a fixed picture whose pixels can be predicted from cycle
counts, so host_main.c can check them:

  playfield  PF0=$10 PF1=$01 PF2=$80, reflected: blocks 0, 11, 19 and mirrors
             -> pixels 0-3, 44-47, 76-83, 112-115, 156-159
  RESP0 ends on cycle 34 after WSYNC -> P0 at pixel 34*3-68+5 = 39
  RESP1 ends on cycle 37             -> P1 at 48, then HMOVE with HMP1=$E0
                                        moves it right 2 -> 50
  RESM0 ends on cycle 40             -> M0 at 40*3-68+4 = 56
  RESBL ends on cycle 43             -> BL at 43*3-68+4 = 65, 4 wide
  section B (20 lines): GRP0=GRP1=$C3, M0 and BL on
  section C (10 lines): COLUBK written by an STA ending on cycle 40 -> the
             new color starts at pixel 40*3-68 = 52
  collisions: P0 hits PF (pixels 45-46) and M0 hits P1 (pixel 56); the next
             frame's background is $84 only if both latches were set
  fire (INPT4) switches COLUP0 from $44 to $1E
  VBLANK is 37 lines counted with WSYNC, overscan waits on TIM64T

Run: python3 make_testrom.py testrom.bin
"""
import sys

# fmt: off
OPS = {
    # mnemonic: {mode: opcode}
    "LDA": {"imm": 0xA9, "zp": 0xA5, "zpx": 0xB5, "abs": 0xAD, "abx": 0xBD, "aby": 0xB9},
    "LDX": {"imm": 0xA2, "zp": 0xA6, "abs": 0xAE},
    "LDY": {"imm": 0xA0, "zp": 0xA4, "abs": 0xAC},
    "STA": {"zp": 0x85, "zpx": 0x95, "abs": 0x8D, "abx": 0x9D},
    "STX": {"zp": 0x86, "abs": 0x8E},
    "STY": {"zp": 0x84, "abs": 0x8C},
    "AND": {"imm": 0x29, "zp": 0x25},
    "ORA": {"imm": 0x09, "zp": 0x05},
    "CMP": {"imm": 0xC9, "zp": 0xC5},
    "CPY": {"imm": 0xC0},
    "CPX": {"imm": 0xE0},
    "BIT": {"zp": 0x24, "abs": 0x2C},
    "INC": {"zp": 0xE6},
    "DEC": {"zp": 0xC6},
    "JMP": {"abs": 0x4C},
    "JSR": {"abs": 0x20},
    "SAX": {"zp": 0x87},     # undocumented, used on purpose
    "LAX": {"zp": 0xA7},     # undocumented
    "DCP": {"zp": 0xC7},     # undocumented
}
IMPLIED = {
    "SEI": 0x78, "CLD": 0xD8, "TXS": 0x9A, "TXA": 0x8A, "TAY": 0xA8, "TAX": 0xAA,
    "TYA": 0x98, "INX": 0xE8, "DEX": 0xCA, "INY": 0xC8, "DEY": 0x88, "NOP": 0xEA,
    "RTS": 0x60, "CLC": 0x18, "SEC": 0x38, "PHA": 0x48, "PLA": 0x68,
}
BRANCH = {"BPL": 0x10, "BMI": 0x30, "BNE": 0xD0, "BEQ": 0xF0, "BCC": 0x90, "BCS": 0xB0}
# fmt: on

TIA = dict(
    VSYNC=0x00, VBLANK=0x01, WSYNC=0x02, NUSIZ0=0x04, NUSIZ1=0x05, COLUP0=0x06, COLUP1=0x07,
    COLUPF=0x08, COLUBK=0x09, CTRLPF=0x0A, PF0=0x0D, PF1=0x0E, PF2=0x0F, RESP0=0x10,
    RESP1=0x11, RESM0=0x12, RESM1=0x13, RESBL=0x14, AUDC0=0x15, AUDF0=0x17, AUDV0=0x19,
    GRP0=0x1B, GRP1=0x1C, ENAM0=0x1D, ENAM1=0x1E, ENABL=0x1F, HMP0=0x20, HMP1=0x21,
    HMOVE=0x2A, HMCLR=0x2B, CXCLR=0x2C,
    CXM0P=0x00, CXP0FB=0x02, INPT4=0x0C,
    INTIM=0x284, TIM64T=0x296, SWCHB=0x282,
    FLAGS=0x80, TMP=0x81, FRAME=0x82,
)

SRC = """
start:  SEI
        CLD
        LDX #$FF
        TXS
        LDA #0
        TAX
clear:  STA $00,X           ; TIA registers and RAM
        INX
        BNE clear

        LDA #$1E
        STA COLUPF
        LDA #$C8
        STA COLUP1
        LDA #$21            ; reflect, ball 4 wide
        STA CTRLPF
        LDA #$10
        STA PF0
        LDA #$01
        STA PF1
        LDA #$80
        STA PF2
        LDA #4
        STA AUDC0
        LDA #15
        STA AUDF0
        LDA #8
        STA AUDV0

        STA WSYNC           ; cycle 0
        LDX #6              ; 2
pos:    DEX                 ; 5 per loop, 4 on the last
        BNE pos             ; -> 31
        STA RESP0           ; ends 34
        STA RESP1           ; ends 37
        STA RESM0           ; ends 40
        STA RESBL           ; ends 43
        LDA #$E0
        STA HMP1
        STA WSYNC
        STA HMOVE           ; in hblank: P1 moves right 2
        LDA #0
        STA WSYNC
        STA HMCLR

frame:  LDA #2
        STA VBLANK
        STA VSYNC
        STA WSYNC
        STA WSYNC
        STA WSYNC
        LDA #0
        STA VSYNC           ; this line is line 0

        LDA #$44            ; P0 color from fire button
        BIT INPT4
        BMI nofire
        LDA #$1E
nofire: STA COLUP0
        LDA #0              ; background from last frame's collisions
        LDX FLAGS
        CPX #$C0
        BNE bkset
        LDA #$84
bkset:  STA COLUBK
        STA CXCLR
        INC FRAME

        LDX #37
vbl:    STA WSYNC
        DEX
        BNE vbl
        LDA #0
        STA VBLANK

        LDY #100            ; section A: playfield only
secA:   STA WSYNC
        DEY
        BNE secA

        LDY #20             ; section B: objects on
secB:   STA WSYNC
        LDA #$C3
        STA GRP0
        STA GRP1
        LDA #2
        STA ENAM0
        STA ENABL
        DEY
        BNE secB
        STA WSYNC
        LDA #0
        STA GRP0
        STA GRP1
        STA ENAM0
        STA ENABL

        LDY #10             ; section C: mid-line background change
secC:   STA WSYNC           ; 0
        LDA COLUBK_SAVE     ; 3 (zp)   dummy load, keeps timing simple
        LDX #3              ; 5
dly:    DEX                 ; 5 per loop, 4 on the last
        BNE dly             ; -> 19
        NOP                 ; 21
        NOP                 ; 23
        NOP                 ; 25
        NOP                 ; 27
        NOP                 ; 29
        NOP                 ; 31
        NOP                 ; 33
        NOP                 ; 35
        LDA #$36            ; 37
        STA COLUBK          ; ends 40
        STA WSYNC
        STX TMP             ; X = 0
        LDA #0
        STA COLUBK
        DEY
        BNE secC

        LDY #61             ; section D: playfield only
secD:   STA WSYNC
        DEY
        BNE secD

        LDA CXM0P           ; bit 7: M0-P1
        AND #$80
        STA FLAGS
        LDA CXP0FB          ; bit 7: P0-PF
        AND #$80
        LSR
        ORA FLAGS
        STA FLAGS           ; $C0 when both collided
        LDA #2
        STA VBLANK
        LDA #35             ; overscan: 35 * 64 cycles
        STA TIM64T
ovs:    LDA INTIM
        BNE ovs
        STA WSYNC
        JMP frame
"""


def assemble(src, org=0xF000):
    rom = bytearray([0xFF] * 4096)
    out = asm(src, org)
    rom[: len(out)] = out
    rom[0xFFC:0xFFE] = bytes([org & 0xFF, org >> 8])  # reset vector
    rom[0xFFE:0x1000] = bytes([org & 0xFF, org >> 8])
    return bytes(rom)


def asm(src, org, extra=None):
    """Assemble src at org, return the bytes."""
    syms = dict(TIA)
    syms.update(extra or {})
    syms["COLUBK_SAVE"] = 0x83
    lines = []
    for raw in src.splitlines():
        s = raw.split(";")[0].rstrip()
        if not s.strip():
            continue
        label = None
        if not s[0].isspace() and ":" in s.split(None, 1)[0]:
            label, _, s = s.partition(":")
        lines.append((label, s.strip()))

    def val(tok):
        tok = tok.strip()
        if tok.startswith("$"):
            return int(tok[1:], 16)
        if tok[0].isdigit():
            return int(tok)
        return syms[tok]

    def encode(pc, text, final):
        if not text:
            return b""
        parts = text.split(None, 1)
        mn = parts[0].upper()
        arg = parts[1].strip() if len(parts) > 1 else ""
        if mn == "LSR" and not arg:
            return bytes([0x4A])
        if mn in IMPLIED:
            return bytes([IMPLIED[mn]])
        if mn in BRANCH:
            target = val(arg) if final else pc
            off = target - (pc + 2)
            if final and not -128 <= off <= 127:
                raise ValueError("branch out of range: " + text)
            return bytes([BRANCH[mn], off & 0xFF])
        modes = OPS[mn]
        if arg.startswith("#"):
            return bytes([modes["imm"], val(arg[1:]) & 0xFF])
        idx = None
        if "," in arg:
            arg, idx = [a.strip() for a in arg.split(",")]
            idx = idx.upper()
        try:
            v = val(arg)
        except KeyError:
            if final:
                raise
            v = 0xFFFF  # forward label: assume absolute
        if v < 0x100 and (("zp" if idx is None else "zp" + idx.lower()) in modes):
            mode = "zp" if idx is None else "zp" + idx.lower()
            return bytes([modes[mode], v])
        mode = "abs" if idx is None else "ab" + idx.lower()
        return bytes([modes[mode], v & 0xFF, (v >> 8) & 0xFF])

    for final in (False, True):
        pc = org
        out = bytearray()
        for label, text in lines:
            if label:
                syms[label] = pc
            b = encode(pc, text, final)
            out += b
            pc += len(b)
    return bytes(out)


if __name__ == "__main__":
    data = assemble(SRC)
    with open(sys.argv[1] if len(sys.argv) > 1 else "testrom.bin", "wb") as f:
        f.write(data)
