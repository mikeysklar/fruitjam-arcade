# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Generate z80_ops.inc: the Z80 opcode switch bodies used by smscore.h.

Run: python3 gen_z80.py > z80_ops.inc

The generated text is the list of `case` labels for the unprefixed opcode
switch in z80_run(). Prefixes (CB, ED, DD, FD, DDCB, FDCB) are nested switches
inside their prefix case. Each case first adds its T-states to `cyc`, so I/O
sees the time at the end of the instruction.

Register model inside z80_run(): locals a, f (8 bit), bc, de, hl, sp, pc, wz
(16 bit, kept masked). IX/IY and the rest live in the sms_t `n`.
Helpers (RD, WR, FETCH, PUSH, POP, IO_IN, IO_OUT, flag tables sz/szp) are
macros/functions in smscore.h.
"""

out = []


def emit(s=""):
    out.append(s)


R8 = ["B", "C", "D", "E", "H", "L", "(HL)", "A"]


def get8(r, xy=None):
    if r == 0:
        return "(bc >> 8)"
    if r == 1:
        return "(bc & 0xFF)"
    if r == 2:
        return "(de >> 8)"
    if r == 3:
        return "(de & 0xFF)"
    if r == 4:
        return "(n->%s >> 8)" % xy if xy else "(hl >> 8)"
    if r == 5:
        return "(n->%s & 0xFF)" % xy if xy else "(hl & 0xFF)"
    if r == 7:
        return "a"
    raise ValueError


def set8(r, v, xy=None):
    if r == 0:
        return "bc = (bc & 0xFF) | ((%s) << 8);" % v
    if r == 1:
        return "bc = (bc & 0xFF00) | (%s);" % v
    if r == 2:
        return "de = (de & 0xFF) | ((%s) << 8);" % v
    if r == 3:
        return "de = (de & 0xFF00) | (%s);" % v
    if r == 4:
        if xy:
            return "n->%s = (n->%s & 0xFF) | ((%s) << 8);" % (xy, xy, v)
        return "hl = (hl & 0xFF) | ((%s) << 8);" % v
    if r == 5:
        if xy:
            return "n->%s = (n->%s & 0xFF00) | (%s);" % (xy, xy, v)
        return "hl = (hl & 0xFF00) | (%s);" % v
    if r == 7:
        return "a = (%s);" % v
    raise ValueError


# 16-bit register pairs for the dd field: BC DE HL SP (HL replaced by xy).
def get16(p, xy=None):
    return ["bc", "de", ("n->" + xy) if xy else "hl", "sp"][p]


def set16(p, v, xy=None):
    return "%s = (%s) & 0xFFFF;" % (get16(p, xy), v)


# For PUSH/POP: BC DE HL AF.
def getpp(p, xy=None):
    if p == 3:
        return "((a << 8) | f)"
    return get16(p, xy)


def setpp(p, v, xy=None):
    if p == 3:
        return "{ uint32_t _t = (%s); a = _t >> 8; f = _t & 0xFF; }" % v
    return set16(p, v, xy)


COND = ["!(f & 0x40)", "(f & 0x40)", "!(f & 0x01)", "(f & 0x01)",
        "!(f & 0x04)", "(f & 0x04)", "!(f & 0x80)", "(f & 0x80)"]

ALU = ["ADD8", "ADC8", "SUB8", "SBC8", "AND8", "XOR8", "OR8", "CP8"]
ROT = ["RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"]


def case(op, cycles, body):
    emit("case 0x%02X: cyc += %d; %s break;" % (op, cycles, body))


# ---------------------------------------------------------------------------
# Unprefixed opcodes. xy=None means HL; for DD/FD the same generator is used
# with xy="ix"/"iy" for the opcodes that touch H, L, HL or (HL).

def gen_main():
    for op in range(256):
        body = main_op(op)
        if body is None:
            continue
        cycles, code = body
        case(op, cycles, code)


def main_op(op):
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    if x == 1:
        if op == 0x76:
            return 4, "HALT_OP();"
        if y == 6:
            return 7, "WR(hl, %s);" % get8(z)
        if z == 6:
            return 7, set8(y, "RD(hl)")
        if y == z:
            return 4, ""
        return 4, set8(y, get8(z))
    if x == 2:
        if z == 6:
            return 7, "%s(RD(hl));" % ALU[y]
        return 4, "%s(%s);" % (ALU[y], get8(z))
    if x == 0:
        if z == 0:
            if y == 0:
                return 4, ""
            if y == 1:
                return 4, "{ uint32_t _t = n->af2; n->af2 = (a << 8) | f; a = _t >> 8; f = _t & 0xFF; }"
            if y == 2:
                return 8, ("{ int8_t _d = (int8_t)RD(pc); pc = (pc + 1) & 0xFFFF; "
                           "bc = (bc - 0x100) & 0xFFFF; if (bc >> 8) { cyc += 5; pc = (pc + _d) & 0xFFFF; wz = pc; } }")
            if y == 3:
                return 12, "{ int8_t _d = (int8_t)RD(pc); pc = (pc + 1 + _d) & 0xFFFF; wz = pc; }"
            return 7, ("{ int8_t _d = (int8_t)RD(pc); pc = (pc + 1) & 0xFFFF; "
                       "if (%s) { cyc += 5; pc = (pc + _d) & 0xFFFF; wz = pc; } }" % COND[y - 4])
        if z == 1:
            if q == 0:
                return 10, "%s pc = (pc + 2) & 0xFFFF;" % set16(p, "RD16(pc)")
            return 11, "ADD16(hl, %s);" % get16(p)
        if z == 2:
            if op == 0x02:
                return 7, "WR(bc, a); wz = ((bc + 1) & 0xFF) | (a << 8);"
            if op == 0x12:
                return 7, "WR(de, a); wz = ((de + 1) & 0xFF) | (a << 8);"
            if op == 0x22:
                return 16, "ad = RD16(pc); pc = (pc + 2) & 0xFFFF; WR16(ad, hl); wz = (ad + 1) & 0xFFFF;"
            if op == 0x32:
                return 13, ("ad = RD16(pc); pc = (pc + 2) & 0xFFFF; WR(ad, a); "
                            "wz = ((ad + 1) & 0xFF) | (a << 8);")
            if op == 0x0A:
                return 7, "a = RD(bc); wz = (bc + 1) & 0xFFFF;"
            if op == 0x1A:
                return 7, "a = RD(de); wz = (de + 1) & 0xFFFF;"
            if op == 0x2A:
                return 16, "ad = RD16(pc); pc = (pc + 2) & 0xFFFF; hl = RD16(ad); wz = (ad + 1) & 0xFFFF;"
            if op == 0x3A:
                return 13, "ad = RD16(pc); pc = (pc + 2) & 0xFFFF; a = RD(ad); wz = (ad + 1) & 0xFFFF;"
        if z == 3:
            if q == 0:
                return 6, set16(p, get16(p) + " + 1")
            return 6, set16(p, get16(p) + " - 1")
        if z == 4:
            if y == 6:
                return 11, "ad = hl; v = RD(ad); INC8(v); WR(ad, v);"
            return 4, "v = %s; INC8(v); %s" % (get8(y), set8(y, "v"))
        if z == 5:
            if y == 6:
                return 11, "ad = hl; v = RD(ad); DEC8(v); WR(ad, v);"
            return 4, "v = %s; DEC8(v); %s" % (get8(y), set8(y, "v"))
        if z == 6:
            if y == 6:
                return 10, "WR(hl, RD(pc)); pc = (pc + 1) & 0xFFFF;"
            return 7, "%s pc = (pc + 1) & 0xFFFF;" % set8(y, "RD(pc)")
        if z == 7:
            return 4, ["RLCA();", "RRCA();", "RLA();", "RRA();", "DAA();", "CPL();", "SCF();", "CCF();"][y]
    if x == 3:
        if z == 0:
            return 5, "if (%s) { cyc += 6; pc = POP(); wz = pc; }" % COND[y]
        if z == 1:
            if q == 0:
                return 10, setpp(p, "POP()")
            if p == 0:
                return 10, "pc = POP(); wz = pc;"
            if p == 1:
                return 4, ("{ uint32_t _t; _t = bc; bc = n->bc2; n->bc2 = _t; _t = de; de = n->de2; n->de2 = _t; "
                           "_t = hl; hl = n->hl2; n->hl2 = _t; }")
            if p == 2:
                return 4, "pc = hl;"
            return 6, "sp = hl;"
        if z == 2:
            return 10, "wz = RD16(pc); if (%s) pc = wz; else pc = (pc + 2) & 0xFFFF;" % COND[y]
        if z == 3:
            if y == 0:
                return 10, "pc = RD16(pc); wz = pc;"
            if y == 1:
                return None  # CB prefix, handled separately
            if y == 2:
                return 11, ("ad = RD(pc) | (a << 8); pc = (pc + 1) & 0xFFFF; IO_OUT(ad, a); "
                            "wz = ((ad + 1) & 0xFF) | (a << 8);")
            if y == 3:
                return 11, ("ad = RD(pc) | (a << 8); pc = (pc + 1) & 0xFFFF; a = IO_IN(ad); "
                            "wz = (ad + 1) & 0xFFFF;")
            if y == 4:
                return 19, "{ uint32_t _t = RD16(sp); WR16(sp, hl); hl = _t; wz = _t; }"
            if y == 5:
                return 4, "{ uint32_t _t = de; de = hl; hl = _t; }"
            if y == 6:
                return 4, "n->iff1 = n->iff2 = 0;"
            return 4, "n->iff1 = n->iff2 = 1; EI_OP();"
        if z == 4:
            return 10, ("wz = RD16(pc); pc = (pc + 2) & 0xFFFF; "
                        "if (%s) { cyc += 7; PUSH(pc); pc = wz; }" % COND[y])
        if z == 5:
            if q == 0:
                return 11, "PUSH(%s);" % getpp(p)
            if p == 0:
                return 17, "wz = RD16(pc); pc = (pc + 2) & 0xFFFF; PUSH(pc); pc = wz;"
            return None  # DD, ED, FD prefixes
        if z == 6:
            return 7, "%s(RD(pc)); pc = (pc + 1) & 0xFFFF;" % ALU[y]
        if z == 7:
            return 11, "PUSH(pc); pc = 0x%02X; wz = pc;" % (y * 8)
    raise ValueError(op)


# ---------------------------------------------------------------------------
# CB prefix: rotates, BIT, RES, SET on registers and (HL).

def gen_cb():
    for op in range(256):
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        if z == 6:
            if x == 0:
                case(op, 15, "ad = hl; v = RD(ad); %s(v); WR(ad, v);" % ROT[y])
            elif x == 1:
                case(op, 12, "BIT(%d, RD(hl), wz >> 8);" % y)
            elif x == 2:
                case(op, 15, "ad = hl; WR(ad, RD(ad) & 0x%02X);" % (~(1 << y) & 0xFF))
            else:
                case(op, 15, "ad = hl; WR(ad, RD(ad) | 0x%02X);" % (1 << y))
            continue
        r = get8(z)
        if x == 0:
            case(op, 8, "v = %s; %s(v); %s" % (r, ROT[y], set8(z, "v")))
        elif x == 1:
            case(op, 8, "v = %s; BIT(%d, v, v);" % (r, y))
        elif x == 2:
            case(op, 8, set8(z, "%s & 0x%02X" % (r, ~(1 << y) & 0xFF)))
        else:
            case(op, 8, set8(z, "%s | 0x%02X" % (r, 1 << y)))


# DDCB/FDCB: address already in `ad` (and wz). Undocumented register copies.
def gen_xycb():
    for op in range(256):
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        copy = "" if z == 6 else " " + set8(z, "v")
        if x == 0:
            case(op, 23, "v = RD(ad); %s(v); WR(ad, v);%s" % (ROT[y], copy))
        elif x == 1:
            case(op, 20, "BIT(%d, RD(ad), ad >> 8);" % y)
        elif x == 2:
            case(op, 23, "v = RD(ad) & 0x%02X; WR(ad, v);%s" % (~(1 << y) & 0xFF, copy))
        else:
            case(op, 23, "v = RD(ad) | 0x%02X; WR(ad, v);%s" % (1 << y, copy))


# ---------------------------------------------------------------------------
# ED prefix.

def gen_ed():
    for op in range(0x40, 0x80):
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        p, q = y >> 1, y & 1
        if z == 0:
            if y == 6:
                case(op, 12, "v = IO_IN(bc); f = (f & 1) | n->szp[v]; wz = (bc + 1) & 0xFFFF;")
            else:
                case(op, 12, "v = IO_IN(bc); f = (f & 1) | n->szp[v]; %s wz = (bc + 1) & 0xFFFF;"
                     % set8(y, "v"))
        elif z == 1:
            v = "0" if y == 6 else get8(y)
            case(op, 12, "IO_OUT(bc, %s); wz = (bc + 1) & 0xFFFF;" % v)
        elif z == 2:
            if q == 0:
                case(op, 15, "SBC16(%s);" % get16(p))
            else:
                case(op, 15, "ADC16(%s);" % get16(p))
        elif z == 3:
            if q == 0:
                case(op, 20, "ad = RD16(pc); pc = (pc + 2) & 0xFFFF; WR16(ad, %s); wz = (ad + 1) & 0xFFFF;"
                     % get16(p))
            else:
                case(op, 20, "ad = RD16(pc); pc = (pc + 2) & 0xFFFF; %s wz = (ad + 1) & 0xFFFF;"
                     % set16(p, "RD16(ad)"))
        elif z == 4:
            case(op, 8, "v = a; a = 0; SUB8(v);")
        elif z == 5:
            # RETN / RETI: both restore IFF1 from IFF2 here.
            case(op, 14, "n->iff1 = n->iff2; pc = POP(); wz = pc;")
        elif z == 6:
            im = [0, 0, 1, 2, 0, 0, 1, 2][y]
            case(op, 8, "n->im = %d;" % im)
        else:
            body = [
                "n->i = a;",
                "n->r = a; n->r7 = a & 0x80;",
                "a = n->i; f = (f & 1) | n->sz[a] | (n->iff2 << 2);",
                "a = (n->r & 0x7F) | n->r7; f = (f & 1) | n->sz[a] | (n->iff2 << 2);",
                "RRD();",
                "RLD();",
                "",
                "",
            ][y]
            case(op, 9 if y < 4 else (18 if y < 6 else 8), body)
    blocks = {
        0xA0: (16, "LDI_OP(1);"), 0xA8: (16, "LDI_OP(-1);"),
        0xB0: (16, "LDI_OP(1); if (bc) { cyc += 5; pc = (pc - 2) & 0xFFFF; wz = (pc + 1) & 0xFFFF; }"),
        0xB8: (16, "LDI_OP(-1); if (bc) { cyc += 5; pc = (pc - 2) & 0xFFFF; wz = (pc + 1) & 0xFFFF; }"),
        0xA1: (16, "CPI_OP(1);"), 0xA9: (16, "CPI_OP(-1);"),
        0xB1: (16, "CPI_OP(1); if (bc && !(f & 0x40)) { cyc += 5; pc = (pc - 2) & 0xFFFF; wz = (pc + 1) & 0xFFFF; }"),
        0xB9: (16, "CPI_OP(-1); if (bc && !(f & 0x40)) { cyc += 5; pc = (pc - 2) & 0xFFFF; wz = (pc + 1) & 0xFFFF; }"),
        0xA2: (16, "INI_OP(1);"), 0xAA: (16, "INI_OP(-1);"),
        0xB2: (16, "INI_OP(1); if (bc >> 8) { cyc += 5; pc = (pc - 2) & 0xFFFF; }"),
        0xBA: (16, "INI_OP(-1); if (bc >> 8) { cyc += 5; pc = (pc - 2) & 0xFFFF; }"),
        0xA3: (16, "OUTI_OP(1);"), 0xAB: (16, "OUTI_OP(-1);"),
        0xB3: (16, "OUTI_OP(1); if (bc >> 8) { cyc += 5; pc = (pc - 2) & 0xFFFF; }"),
        0xBB: (16, "OUTI_OP(-1); if (bc >> 8) { cyc += 5; pc = (pc - 2) & 0xFFFF; }"),
    }
    for op in sorted(blocks):
        case(op, *blocks[op])
    emit("default: cyc += 8; break;")


# ---------------------------------------------------------------------------
# DD/FD prefix, for index register xy. Opcodes not listed fall back to the
# unprefixed opcode (the caller adds 4 T-states and re-dispatches).

def gen_xy(xy):
    X = "n->" + xy
    dis = "ad = (%s + (int8_t)RD(pc)) & 0xFFFF; pc = (pc + 1) & 0xFFFF; wz = ad;" % X
    for op in range(256):
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        p, q = y >> 1, y & 1
        code = None
        if op == 0x09 or op == 0x19 or op == 0x29 or op == 0x39:
            code = 15, "{ uint32_t _h = %s; ADD16(_h, %s); %s = _h; }" % (X, get16(p, xy), X)
        elif op == 0x21:
            code = 14, "%s = RD16(pc); pc = (pc + 2) & 0xFFFF;" % X
        elif op == 0x22:
            code = 20, "ad = RD16(pc); pc = (pc + 2) & 0xFFFF; WR16(ad, %s); wz = (ad + 1) & 0xFFFF;" % X
        elif op == 0x2A:
            code = 20, "ad = RD16(pc); pc = (pc + 2) & 0xFFFF; %s = RD16(ad); wz = (ad + 1) & 0xFFFF;" % X
        elif op == 0x23:
            code = 10, "%s = (%s + 1) & 0xFFFF;" % (X, X)
        elif op == 0x2B:
            code = 10, "%s = (%s - 1) & 0xFFFF;" % (X, X)
        elif op in (0x24, 0x2C):
            r = y
            code = 8, "v = %s; INC8(v); %s" % (get8(r, xy), set8(r, "v", xy))
        elif op in (0x25, 0x2D):
            r = y
            code = 8, "v = %s; DEC8(v); %s" % (get8(r, xy), set8(r, "v", xy))
        elif op in (0x26, 0x2E):
            code = 11, "%s pc = (pc + 1) & 0xFFFF;" % set8(y, "RD(pc)", xy)
        elif op == 0x34:
            code = 23, "%s v = RD(ad); INC8(v); WR(ad, v);" % dis
        elif op == 0x35:
            code = 23, "%s v = RD(ad); DEC8(v); WR(ad, v);" % dis
        elif op == 0x36:
            code = 19, "%s WR(ad, RD(pc)); pc = (pc + 1) & 0xFFFF;" % dis
        elif x == 1 and op != 0x76:
            if y == 6:
                code = 19, "%s WR(ad, %s);" % (dis, get8(z))
            elif z == 6:
                code = 19, "%s %s" % (dis, set8(y, "RD(ad)"))
            elif y in (4, 5) or z in (4, 5):
                code = 8, set8(y, get8(z, xy), xy)
        elif x == 2:
            if z == 6:
                code = 19, "%s %s(RD(ad));" % (dis, ALU[y])
            elif z in (4, 5):
                code = 8, "%s(%s);" % (ALU[y], get8(z, xy))
        elif op == 0xE1:
            code = 14, "%s = POP();" % X
        elif op == 0xE5:
            code = 15, "PUSH(%s);" % X
        elif op == 0xE3:
            code = 23, "{ uint32_t _t = RD16(sp); WR16(sp, %s); %s = _t; wz = _t; }" % (X, X)
        elif op == 0xE9:
            code = 8, "pc = %s;" % X
        elif op == 0xF9:
            code = 10, "sp = %s;" % X
        elif op == 0xCB:
            emit("case 0xCB: %s op = RD(pc); pc = (pc + 1) & 0xFFFF; switch (op) {" % dis)
            gen_xycb()
            emit("} break;")
            continue
        if code:
            case(op, *code)
    emit("default: cyc += 4; goto dispatch;")


emit("// Generated by gen_z80.py. Do not edit.")
gen_main()
emit("case 0xCB: op = FETCH_M1(); switch (op) {")
gen_cb()
emit("} break;")
emit("case 0xED: op = FETCH_M1(); switch (op) {")
gen_ed()
emit("} break;")
emit("case 0xDD: op = FETCH_M1(); switch (op) {")
gen_xy("ix")
emit("} break;")
emit("case 0xFD: op = FETCH_M1(); switch (op) {")
gen_xy("iy")
emit("} break;")
print("\n".join(out))
