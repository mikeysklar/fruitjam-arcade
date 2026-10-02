# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Generate nes_ops.inc: the 6502 opcode switch used by nescore.h.

Run: python3 gen_ops.py > nes_ops.inc

Each case computes an effective address `ea` for its addressing mode, then runs
the operation body. Cycle counts are the standard NMOS 6502 table; read
instructions add one cycle when an indexed address crosses a page.
"""

CYCLES = [
    7, 6, 0, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,
    2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 0, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,
    2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 0, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,
    2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 0, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,
    2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
    2, 6, 0, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
    2, 5, 0, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
    2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
    2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
]

# Addressing modes. "r" variants add the page-cross cycle (read instructions).
MODES = {
    "imp": "",
    "acc": "",
    "imm": "ea = pc++;",
    "zp": "ea = FETCH(pc); pc++;",
    "zpx": "ea = (uint8_t)(FETCH(pc) + x); pc++;",
    "zpy": "ea = (uint8_t)(FETCH(pc) + y); pc++;",
    "abs": "ea = FETCH16(pc); pc += 2;",
    "abx": "ea = (uint16_t)(FETCH16(pc) + x); pc += 2;",
    "aby": "ea = (uint16_t)(FETCH16(pc) + y); pc += 2;",
    "abxr": "{ uint16_t b = FETCH16(pc); pc += 2; ea = (uint16_t)(b + x); cyc += ((b ^ ea) >> 8) & 1; }",
    "abyr": "{ uint16_t b = FETCH16(pc); pc += 2; ea = (uint16_t)(b + y); cyc += ((b ^ ea) >> 8) & 1; }",
    "izx": "{ uint8_t z = (uint8_t)(FETCH(pc) + x); pc++; ea = ram[z] | (ram[(uint8_t)(z + 1)] << 8); }",
    "izy": "{ uint8_t z = FETCH(pc); pc++; ea = (uint16_t)((ram[z] | (ram[(uint8_t)(z + 1)] << 8)) + y); }",
    "izyr": "{ uint8_t z = FETCH(pc); pc++; uint16_t b = ram[z] | (ram[(uint8_t)(z + 1)] << 8); "
            "ea = (uint16_t)(b + y); cyc += ((b ^ ea) >> 8) & 1; }",
    "rel": "",
    "ind": "",
}

# Operation bodies. `m` is the operand read from ea (for read ops).
READ = {
    "LDA": "a = m; NZ(a);",
    "LDX": "x = m; NZ(x);",
    "LDY": "y = m; NZ(y);",
    "LAX": "a = x = m; NZ(a);",
    "AND": "a &= m; NZ(a);",
    "ORA": "a |= m; NZ(a);",
    "EOR": "a ^= m; NZ(a);",
    "ADC": "ADC(m);",
    "SBC": "ADC(m ^ 0xFF);",
    "CMP": "CMP(a, m);",
    "CPX": "CMP(x, m);",
    "CPY": "CMP(y, m);",
    "BIT": "zr = a & m; nr = m; fv = (m >> 6) & 1;",
    "NOP": "(void)m;",
    "ANC": "a &= m; NZ(a); fc = a >> 7;",
    "ALR": "a &= m; fc = a & 1; a >>= 1; NZ(a);",
    "ARR": "a &= m; a = (uint8_t)((a >> 1) | (fc << 7)); NZ(a); fc = (a >> 6) & 1; fv = ((a >> 6) ^ (a >> 5)) & 1;",
    "AXS": "{ unsigned t = (a & x) - m; fc = t < 0x100; x = (uint8_t)t; NZ(x); }",
    "LAS": "a = x = s = m & s; NZ(a);",
}
# Read-modify-write: body transforms m, result written back.
RMW = {
    "ASL": "fc = m >> 7; m <<= 1; NZ(m);",
    "LSR": "fc = m & 1; m >>= 1; NZ(m);",
    "ROL": "{ uint8_t c = fc; fc = m >> 7; m = (uint8_t)((m << 1) | c); } NZ(m);",
    "ROR": "{ uint8_t c = fc; fc = m & 1; m = (uint8_t)((m >> 1) | (c << 7)); } NZ(m);",
    "INC": "m++; NZ(m);",
    "DEC": "m--; NZ(m);",
    "SLO": "fc = m >> 7; m <<= 1; a |= m; NZ(a);",
    "RLA": "{ uint8_t c = fc; fc = m >> 7; m = (uint8_t)((m << 1) | c); } a &= m; NZ(a);",
    "SRE": "fc = m & 1; m >>= 1; a ^= m; NZ(a);",
    "RRA": "{ uint8_t c = fc; fc = m & 1; m = (uint8_t)((m >> 1) | (c << 7)); } ADC(m);",
    "DCP": "m--; CMP(a, m);",
    "ISB": "m++; ADC(m ^ 0xFF);",
}
STORE = {
    "STA": "a",
    "STX": "x",
    "STY": "y",
    "SAX": "a & x",
}
IMPLIED = {
    "TAX": "x = a; NZ(x);",
    "TAY": "y = a; NZ(y);",
    "TXA": "a = x; NZ(a);",
    "TYA": "a = y; NZ(a);",
    "TSX": "x = s; NZ(x);",
    "TXS": "s = x;",
    "INX": "x++; NZ(x);",
    "INY": "y++; NZ(y);",
    "DEX": "x--; NZ(x);",
    "DEY": "y--; NZ(y);",
    "CLC": "fc = 0;",
    "SEC": "fc = 1;",
    "CLI": "fi = 0; poll = 1;",
    "SEI": "fi = 1;",
    "CLV": "fv = 0;",
    "CLD": "fd = 0;",
    "SED": "fd = 1;",
    "NOP": "",
    "PHA": "PUSH(a);",
    "PHP": "PUSH(GETP() | 0x10);",
    "PLA": "a = PULL(); NZ(a);",
    "PLP": "SETP(PULL()); poll = 1;",
    "RTS": "pc = PULL(); pc |= PULL() << 8; pc++;",
    "RTI": "SETP(PULL()); poll = 1; pc = PULL(); pc |= PULL() << 8;",
    "BRK": "pc++; PUSH(pc >> 8); PUSH(pc & 0xFF); PUSH(GETP() | 0x10); fi = 1; pc = RD16(0xFFFE);",
}
BRANCH = {
    0x10: "!(nr & 0x80)", 0x30: "(nr & 0x80)", 0x50: "!fv", 0x70: "fv",
    0x90: "!fc", 0xB0: "fc", 0xD0: "zr", 0xF0: "!zr",
}

ops = {}


def op(code, name, mode):
    assert code not in ops, hex(code)
    ops[code] = (name, mode)


# cc=01 ALU group
alu = ["ORA", "AND", "EOR", "ADC", "STA", "LDA", "CMP", "SBC"]
alu_modes = ["izx", "zp", "imm", "abs", "izy", "zpx", "aby", "abx"]
for a, name in enumerate(alu):
    for b, mode in enumerate(alu_modes):
        code = (a << 5) | (b << 2) | 1
        if name == "STA" and mode == "imm":
            op(code, "NOP", "imm")
        else:
            op(code, name, mode)
# cc=11 unofficial group, same modes
ill = ["SLO", "RLA", "SRE", "RRA", "SAX", "LAX", "DCP", "ISB"]
for a, name in enumerate(ill):
    for b, mode in enumerate(alu_modes):
        code = (a << 5) | (b << 2) | 3
        if mode == "imm":
            continue
        if name == "LAX" and b == 6:
            continue  # 0xBB is LAS
        if name in ("SAX", "LAX"):
            mode = {"zpx": "zpy", "abx": "aby"}.get(mode, mode)
        if name == "SAX" and mode in ("izy", "aby"):
            continue  # AHX / TAS family, unstable
        op(code, name, mode)
op(0x0B, "ANC", "imm")
op(0x2B, "ANC", "imm")
op(0x4B, "ALR", "imm")
op(0x6B, "ARR", "imm")
op(0xCB, "AXS", "imm")
op(0xEB, "SBC", "imm")
op(0xBB, "LAS", "aby")
# Unstable stores (value ANDed with the high address byte + 1) and magic-constant
# ops, done the common way so a stray one does not jam the CPU.
for code, name, mode in [(0x9C, "SHY", "abx"), (0x9E, "SHX", "aby"), (0x9F, "AHX", "aby"),
                         (0x93, "AHX", "izy"), (0x9B, "TAS", "aby"), (0xAB, "LXA", "imm"),
                         (0x8B, "XAA", "imm")]:
    op(code, name, mode)
# cc=10 shift group
for a, name in enumerate(["ASL", "ROL", "LSR", "ROR"]):
    op((a << 5) | 0x06, name, "zp")
    op((a << 5) | 0x0A, name, "acc")
    op((a << 5) | 0x0E, name, "abs")
    op((a << 5) | 0x16, name, "zpx")
    op((a << 5) | 0x1E, name, "abx")
for a, name in [(6, "DEC"), (7, "INC")]:
    op((a << 5) | 0x06, name, "zp")
    op((a << 5) | 0x0E, name, "abs")
    op((a << 5) | 0x16, name, "zpx")
    op((a << 5) | 0x1E, name, "abx")
op(0x86, "STX", "zp")
op(0x8E, "STX", "abs")
op(0x96, "STX", "zpy")
op(0xA2, "LDX", "imm")
op(0xA6, "LDX", "zp")
op(0xAE, "LDX", "abs")
op(0xB6, "LDX", "zpy")
op(0xBE, "LDX", "aby")
op(0x84, "STY", "zp")
op(0x8C, "STY", "abs")
op(0x94, "STY", "zpx")
op(0xA0, "LDY", "imm")
op(0xA4, "LDY", "zp")
op(0xAC, "LDY", "abs")
op(0xB4, "LDY", "zpx")
op(0xBC, "LDY", "abx")
op(0xC0, "CPY", "imm")
op(0xC4, "CPY", "zp")
op(0xCC, "CPY", "abs")
op(0xE0, "CPX", "imm")
op(0xE4, "CPX", "zp")
op(0xEC, "CPX", "abs")
op(0x24, "BIT", "zp")
op(0x2C, "BIT", "abs")
for code, name in [
    (0xAA, "TAX"), (0xA8, "TAY"), (0x8A, "TXA"), (0x98, "TYA"), (0xBA, "TSX"), (0x9A, "TXS"),
    (0xE8, "INX"), (0xC8, "INY"), (0xCA, "DEX"), (0x88, "DEY"), (0x18, "CLC"), (0x38, "SEC"),
    (0x58, "CLI"), (0x78, "SEI"), (0xB8, "CLV"), (0xD8, "CLD"), (0xF8, "SED"), (0xEA, "NOP"),
    (0x48, "PHA"), (0x08, "PHP"), (0x68, "PLA"), (0x28, "PLP"), (0x60, "RTS"), (0x40, "RTI"),
    (0x00, "BRK"),
]:
    op(code, name, "imp")
for code in (0x1A, 0x3A, 0x5A, 0x7A, 0xDA, 0xFA):
    op(code, "NOP", "imp")
for code in (0x80, 0x82, 0x89, 0xC2, 0xE2):
    if code not in ops:
        op(code, "NOP", "imm")
for code in (0x04, 0x44, 0x64):
    op(code, "NOP", "zp")
for code in (0x14, 0x34, 0x54, 0x74, 0xD4, 0xF4):
    op(code, "NOP", "zpx")
op(0x0C, "NOP", "abs")
for code in (0x1C, 0x3C, 0x5C, 0x7C, 0xDC, 0xFC):
    op(code, "NOP", "abx")
for code in BRANCH:
    op(code, "BR", "rel")
op(0x4C, "JMP", "abs")
op(0x6C, "JMP", "ind")
op(0x20, "JSR", "abs")


UNSTABLE = {
    "SHY": "y", "SHX": "x", "AHX": "a & x", "TAS": "(s = a & x)",
}


def body(code, name, mode):
    m = MODES[mode]
    if name in UNSTABLE:
        base = {"abx": "FETCH16(pc); pc += 2", "aby": "FETCH16(pc); pc += 2",
                "izy": "ram[FETCH(pc)] | (ram[(uint8_t)(FETCH(pc) + 1)] << 8); pc++"}[mode]
        idx = "x" if mode == "abx" else "y"
        return ("{ uint16_t b = %s; ea = (uint16_t)(b + %s); uint8_t v = (%s) & ((b >> 8) + 1); "
                "if ((b ^ ea) & 0x100) ea = (ea & 0xFF) | (v << 8); WR(ea, v); }" % (base, idx, UNSTABLE[name]))
    if name == "LXA":
        return "a = x = FETCH(pc); pc++; NZ(a);"
    if name == "XAA":
        return "a = (a | 0xEE) & x & FETCH(pc); pc++; NZ(a);"
    if name == "BR":
        return ("{ int8_t d = (int8_t)FETCH(pc); pc++; if (%s) { uint16_t t = (uint16_t)(pc + d); "
                "cyc += 1 + (((pc ^ t) >> 8) & 1); pc = t; } }" % BRANCH[code])
    if name == "JMP" and mode == "abs":
        return "pc = FETCH16(pc);"
    if name == "JMP":
        return ("{ uint16_t p = FETCH16(pc); pc = RD(p) | (RD((p & 0xFF00) | ((p + 1) & 0xFF)) << 8); }")
    if name == "JSR":
        return "{ uint16_t t = FETCH16(pc); pc++; PUSH(pc >> 8); PUSH(pc & 0xFF); pc = t; }"
    if mode == "imp":
        return IMPLIED[name]
    if mode == "acc":
        return "{ uint8_t m = a; %s a = m; }" % RMW[name]
    if name in STORE:
        return "%s WR(ea, %s);" % (m, STORE[name])
    if name in RMW:
        return "%s { uint8_t m = RD(ea); %s WR(ea, m); }" % (m, RMW[name])
    if name in READ:
        if mode in ("abx", "aby", "izy"):
            m = MODES[mode + "r"]
        if mode == "imm":
            return "{ uint8_t m = FETCH(pc); pc++; %s }" % READ[name]
        return "%s { uint8_t m = RD(ea); %s }" % (m, READ[name])
    raise ValueError(name)


print("// Generated by gen_ops.py. Do not edit.")
for code in range(256):
    if code not in ops:
        continue
    name, mode = ops[code]
    assert CYCLES[code], hex(code)
    print("case 0x%02X: cyc += %d; %s break; // %s %s" % (code, CYCLES[code], body(code, name, mode), name, mode))
print("default: goto jam;")
