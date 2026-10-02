# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Generate z80_ops.inc: the main, DD/FD and CB opcode switches for z80.h.

    python3 gen_ops.py > z80_ops.inc

The DD and FD switches only list opcodes that touch HL, H, L or (HL); any other
opcode after a prefix falls back to the main switch (the prefix is then just
4 T-states), which also handles runs of prefixes.
"""

ALU = ["alu_add", "alu_adc", "alu_sub", "alu_sbc", "alu_and", "alu_xor", "alu_or", "alu_cp"]
ROT = ["RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"]


def regs(px):
    return ["z->bc.b.h", "z->bc.b.l", "z->de.b.h", "z->de.b.l",
            "z->%s.b.h" % px, "z->%s.b.l" % px, None, "z->a"]


REAL = regs("hl")


def rp(p, px):
    return ["z->bc.w", "z->de.w", "z->%s.w" % px, "z->sp"][p]


def mem_addr(px):
    """Code computing `ad` for (HL) or (IX+d), and the extra T-states."""
    if px == "hl":
        return "uint16_t ad = z->hl.w;", 0
    return ("uint16_t ad = (uint16_t)(z->%s.w + (int8_t)RD(z->pc++)); z->wz = ad;" % px), 8


def op_code(op, px):
    """Return (C body, T-states) for opcode op, or None if it needs special casing."""
    x, y, zz = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    R = regs(px)
    pre = px != "hl"
    if x == 0:
        if zz == 0:
            if y == 0:
                return "", 4
            if y == 1:
                return ("uint8_t t = z->a; z->a = z->af_.b.h; z->af_.b.h = t;"
                        " t = z->f; z->f = z->af_.b.l; z->af_.b.l = t;"), 4
            if y == 2:
                return ("int8_t e = RD(z->pc++);"
                        " if (--z->bc.b.h) { z->pc += e; z->wz = z->pc; z->cyc += 5; }"), 8
            if y == 3:
                return "int8_t e = RD(z->pc++); z->pc += e; z->wz = z->pc;", 12
            return ("int8_t e = RD(z->pc++);"
                    " if (%s) { z->pc += e; z->wz = z->pc; z->cyc += 5; }" % cond(y - 4)), 7
        if zz == 1:
            if q == 0:
                return "%s = rd16(z, z->pc); z->pc += 2;" % rp(p, px), 10
            return "%s = add16(z, %s, %s);" % (rp(2, px), rp(2, px), rp(p, px)), 11
        if zz == 2:
            if p == 0 or p == 1:
                r = rp(p, px)
                if q == 0:
                    return ("WR(%s, z->a); z->wz = ((%s + 1) & 0xFF) | (z->a << 8);" % (r, r)), 7
                return "z->a = RD(%s); z->wz = %s + 1;" % (r, r), 7
            if p == 2:
                if q == 0:
                    return ("uint16_t nn = rd16(z, z->pc); z->pc += 2;"
                            " wr16(z, nn, %s); z->wz = nn + 1;" % rp(2, px)), 16
                return ("uint16_t nn = rd16(z, z->pc); z->pc += 2;"
                        " %s = rd16(z, nn); z->wz = nn + 1;" % rp(2, px)), 16
            if q == 0:
                return ("uint16_t nn = rd16(z, z->pc); z->pc += 2;"
                        " WR(nn, z->a); z->wz = ((nn + 1) & 0xFF) | (z->a << 8);"), 13
            return "uint16_t nn = rd16(z, z->pc); z->pc += 2; z->a = RD(nn); z->wz = nn + 1;", 13
        if zz == 3:
            return "%s%s;" % (rp(p, px), "++" if q == 0 else "--"), 6
        if zz in (4, 5):
            fn = "inc8" if zz == 4 else "dec8"
            if y == 6:
                a, extra = mem_addr(px)
                return "%s WR(ad, %s(z, RD(ad)));" % (a, fn), 11 + extra
            return "%s = %s(z, %s);" % (R[y], fn, R[y]), 4
        if zz == 6:
            if y == 6:
                a, extra = mem_addr(px)
                return "%s WR(ad, RD(z->pc++));" % a, 10 + (5 if pre else 0)
            return "%s = RD(z->pc++);" % R[y], 7
        return [
            "z->a = (z->a << 1) | (z->a >> 7);"
            " z->f = (z->f & (Z_SF | Z_ZF | Z_PF)) | (z->a & (Z_YF | Z_XF | Z_CF));",
            "uint8_t c = z->a & 1; z->a = (z->a >> 1) | (z->a << 7);"
            " z->f = (z->f & (Z_SF | Z_ZF | Z_PF)) | (z->a & (Z_YF | Z_XF)) | c;",
            "uint8_t c = z->a >> 7; z->a = (z->a << 1) | (z->f & Z_CF);"
            " z->f = (z->f & (Z_SF | Z_ZF | Z_PF)) | (z->a & (Z_YF | Z_XF)) | c;",
            "uint8_t c = z->a & 1; z->a = (z->a >> 1) | ((z->f & Z_CF) << 7);"
            " z->f = (z->f & (Z_SF | Z_ZF | Z_PF)) | (z->a & (Z_YF | Z_XF)) | c;",
            "z80_daa(z);",
            "z->a ^= 0xFF;"
            " z->f = (z->f & (Z_SF | Z_ZF | Z_PF | Z_CF)) | Z_HF | Z_NF | (z->a & (Z_YF | Z_XF));",
            "z->f = (z->f & (Z_SF | Z_ZF | Z_PF)) | Z_CF | (z->a & (Z_YF | Z_XF));",
            "z->f = ((z->f & (Z_SF | Z_ZF | Z_PF | Z_CF)) | ((z->f & Z_CF) << 4)"
            " | (z->a & (Z_YF | Z_XF))) ^ Z_CF;",
        ][y], 4
    if x == 1:
        if op == 0x76:
            return "z->halted = 1;", 4
        if y == 6:
            a, extra = mem_addr(px)
            return "%s WR(ad, %s);" % (a, REAL[zz]), 7 + extra
        if zz == 6:
            a, extra = mem_addr(px)
            return "%s %s = RD(ad);" % (a, REAL[y]), 7 + extra
        return "%s = %s;" % (R[y], R[zz]), 4
    if x == 2:
        if zz == 6:
            a, extra = mem_addr(px)
            return "%s %s(z, RD(ad));" % (a, ALU[y]), 7 + extra
        return "%s(z, %s);" % (ALU[y], R[zz]), 4
    # x == 3
    if zz == 0:
        return "if (%s) { z->pc = z->wz = pop16(z); z->cyc += 6; }" % cond(y), 5
    if zz == 1:
        if q == 0:
            if p == 3:
                return "uint16_t v = pop16(z); z->a = v >> 8; z->f = v;", 10
            return "%s = pop16(z);" % rp(p, px), 10
        if p == 0:
            return "z->pc = z->wz = pop16(z);", 10
        if p == 1:
            return ("uint16_t t = z->bc.w; z->bc.w = z->bc_.w; z->bc_.w = t;"
                    " t = z->de.w; z->de.w = z->de_.w; z->de_.w = t;"
                    " t = z->hl.w; z->hl.w = z->hl_.w; z->hl_.w = t;"), 4
        if p == 2:
            return "z->pc = %s;" % rp(2, px), 4
        return "z->sp = %s;" % rp(2, px), 6
    if zz == 2:
        return ("uint16_t nn = rd16(z, z->pc); z->pc += 2; z->wz = nn;"
                " if (%s) { z->pc = nn; }" % cond(y)), 10
    if zz == 3:
        if y == 0:
            return "z->pc = z->wz = rd16(z, z->pc);", 10
        if y == 1:
            return None  # CB
        if y == 2:
            return ("uint8_t n = RD(z->pc++); Z80_OUT((z->a << 8) | n, z->a);"
                    " z->wz = ((n + 1) & 0xFF) | (z->a << 8);"), 11
        if y == 3:
            return ("uint16_t port = (z->a << 8) | RD(z->pc++); z->a = Z80_IN(port);"
                    " z->wz = port + 1;"), 11
        if y == 4:
            r = rp(2, px)
            return ("uint16_t v = rd16(z, z->sp); wr16(z, z->sp, %s); %s = v; z->wz = v;"
                    % (r, r)), 19
        if y == 5:
            return "uint16_t t = z->de.w; z->de.w = z->hl.w; z->hl.w = t;", 4
        if y == 6:
            return "z->iff1 = z->iff2 = 0;", 4
        return "z->iff1 = z->iff2 = 1; z->ei_delay = 1;", 4
    if zz == 4:
        return ("uint16_t nn = rd16(z, z->pc); z->pc += 2; z->wz = nn;"
                " if (%s) { push16(z, z->pc); z->pc = nn; z->cyc += 7; }" % cond(y)), 10
    if zz == 5:
        if q == 0:
            if p == 3:
                return "push16(z, (z->a << 8) | z->f);", 11
            return "push16(z, %s);" % rp(p, px), 11
        if p == 0:
            return ("uint16_t nn = rd16(z, z->pc); z->pc += 2; z->wz = nn;"
                    " push16(z, z->pc); z->pc = nn;"), 17
        return None  # DD ED FD
    if zz == 6:
        return "%s(z, RD(z->pc++));" % ALU[y], 7
    return "push16(z, z->pc); z->pc = z->wz = 0x%02X;" % (y * 8), 11


def cond(cc):
    return ["!(z->f & Z_ZF)", "(z->f & Z_ZF)", "!(z->f & Z_CF)", "(z->f & Z_CF)",
            "!(z->f & Z_PF)", "(z->f & Z_PF)", "!(z->f & Z_SF)", "(z->f & Z_SF)"][cc]


def uses_hl(op):
    x, y, zz = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    if op == 0x76 or op == 0xEB:
        return False
    if x == 0:
        if zz == 1:
            return q == 1 or p == 2
        if zz == 2:
            return p == 2
        if zz == 3:
            return p == 2
        if zz in (4, 5, 6):
            return y in (4, 5, 6)
        return False
    if x == 1:
        return y in (4, 5, 6) or zz in (4, 5, 6)
    if x == 2:
        return zz in (4, 5, 6)
    return op in (0xE1, 0xE3, 0xE5, 0xE9, 0xF9, 0xCB)


def case(op, body, t):
    return "        case 0x%02X: { %s z->cyc += %d; break; }" % (op, body, t)


def main():
    out = []
    w = out.append
    w("// Generated by gen_ops.py, do not edit.")
    w("")
    # CB: registers and (HL).
    w("static void z80_cb(z80_t *z) {")
    w("    uint8_t op = RD(z->pc++);")
    w("    z->r++;")
    w("    switch (op) {")
    for op in range(256):
        x, y, zz = op >> 6, (op >> 3) & 7, op & 7
        if zz == 6:
            if x == 0:
                body, t = "uint8_t v = rot8(z, %d, RD(z->hl.w)); WR(z->hl.w, v);" % y, 15
            elif x == 1:
                body, t = "bit8(z, %d, RD(z->hl.w), z->wz >> 8);" % y, 12
            elif x == 2:
                body, t = "WR(z->hl.w, RD(z->hl.w) & 0x%02X);" % (~(1 << y) & 0xFF), 15
            else:
                body, t = "WR(z->hl.w, RD(z->hl.w) | 0x%02X);" % (1 << y), 15
        else:
            r = REAL[zz]
            if x == 0:
                body = "%s = rot8(z, %d, %s);" % (r, y, r)
            elif x == 1:
                body = "bit8(z, %d, %s, %s);" % (y, r, r)
            elif x == 2:
                body = "%s &= 0x%02X;" % (r, ~(1 << y) & 0xFF)
            else:
                body = "%s |= 0x%02X;" % (r, 1 << y)
            t = 8
        w(case(op, body, t))
    w("    }")
    w("}")
    w("")
    w("static void z80_step(z80_t *z) {")
    w("    uint8_t op = RD(z->pc++);")
    w("    z->r++;")
    w("#ifdef Z80_COUNT")
    w("    z->instrs++;")
    w("#endif")
    w("dispatch:")
    w("    switch (op) {")
    for op in range(256):
        if op == 0xCB:
            w("        case 0xCB: z80_cb(z); break;")
            continue
        if op == 0xED:
            w("        case 0xED: z80_ed(z); break;")
            continue
        if op in (0xDD, 0xFD):
            px = "ix" if op == 0xDD else "iy"
            w("        case 0x%02X:" % op)
            w("            z->cyc += 4;")
            w("            op = RD(z->pc++);")
            w("            z->r++;")
            w("            switch (op) {")
            for o2 in range(256):
                if not uses_hl(o2):
                    continue
                if o2 == 0xCB:
                    w("                case 0xCB: { uint16_t ad = (uint16_t)(z->%s.w + "
                      "(int8_t)RD(z->pc++)); z->wz = ad; z80_xycb(z, ad); break; }" % px)
                    continue
                body, t = op_code(o2, px)
                w("        " + case(o2, body, t))
            w("                default: goto dispatch;")
            w("            }")
            w("            break;")
            continue
        body, t = op_code(op, "hl")
        w(case(op, body, t))
    w("    }")
    w("}")
    print("\n".join(out))


main()
