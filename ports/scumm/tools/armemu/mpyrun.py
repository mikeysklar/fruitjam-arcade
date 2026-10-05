# SPDX-FileCopyrightText: 2026 Mikey Sklar
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the code image of scumm.mpy under Unicorn (Cortex-M33).

Links build/armv7emsp/prelink.o with CircuitPython's own mpy_ld.py exactly
as `make` does, applies the .mpy relocations the way MicroPython's
mp_native_relocate() does at import time, and calls the fj_core_* entry
points in the relocated image. memset/memmove come through a fake
mp_fun_table. This checks the partial link + mpy_ld packaging without a
board (only modscumm.c, the MicroPython glue, is not exercised).

    MPY_DIR=/path/to/circuitpython [MUSIC=adlib] [CLICK=F:X,Y] python3 tools/armemu/mpyrun.py GAMEDIR [frames] [shots]

CLICK holds the left button at X,Y (game coordinates) for frames F..F+2.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, "..", "..")
sys.path.insert(0, os.path.join(os.environ["MPY_DIR"], "tools"))
import mpy_ld  # noqa: E402

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_HOOK_BLOCK, \
    UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL  # noqa: E402
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, \
    UC_ARM_REG_LR, UC_ARM_REG_PC, UC_CPU_ARM_CORTEX_M33  # noqa: E402

from run import png  # noqa: E402

TEXT = 0x30000000
HOST = 0x0F000000
RAM = 0x20000000
RAM_SIZE = 8 << 20
STOP = HOST + 0x800
FUN_TABLE = HOST + 0x400  # fake mp_fun_table (words)


def link():
    env = mpy_ld.LinkEnv("armv7emsp")
    with open(os.path.join(TOP, "build", "armv7emsp", "prelink.o"), "rb") as f:
        mpy_ld.load_object_file(env, f, "prelink.o")
    nq = 0
    with open(os.path.join(TOP, "build", "armv7emsp", "scumm.config.h")) as f:
        nq = sum(1 for l in f if l.startswith("#define MP_QSTR_") and "mp_native_" in l)
    mpy_ld.link_objects(env, nq)
    return env


def main():
    game = sys.argv[1]
    frames = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    shots = [int(x) for x in sys.argv[3].split(",")] if len(sys.argv) > 3 else [frames]
    outdir = os.environ.get("OUT", "/tmp")
    mpy_ld.log_level = 0
    env = link()
    text = bytearray(env.full_text)
    assert not env.full_bss and not env.full_rodata
    print("text %d bytes, %d relocations, %d GOT entries" % (len(text), len(env.mpy_relocs), len(env.got_entries)))

    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS, cpu=UC_CPU_ARM_CORTEX_M33)
    uc.mem_map(TEXT, (len(text) + 0xfff) & ~0xfff, UC_PROT_ALL)
    uc.mem_map(RAM, RAM_SIZE, UC_PROT_ALL)
    uc.mem_map(HOST, 0x1000, UC_PROT_ALL)
    uc.mem_write(HOST, b"\x70\x47" * 0x200)  # bx lr at each host call
    uc.mem_map(0xE000E000, 0x1000, UC_PROT_ALL)
    uc.mem_write(0xE000ED00, struct.pack("<I", 0x410FD210))
    # fake mp_fun_table: every entry is a host call that reports its index,
    # except memset (50) and memmove (51)
    for i in range(128):
        uc.mem_write(FUN_TABLE + 4 * i, struct.pack("<I", HOST + 0x100 + 4 * i + 1))

    # mp_native_relocate
    bad = 0
    for base, addr, kind in env.mpy_relocs:
        assert base == ".text"
        (w,) = struct.unpack_from("<I", text, addr)
        if isinstance(kind, str) and kind.startswith((".text", ".rodata", ".data.rel.ro")):
            w += TEXT
        elif kind == "mp_fun_table":
            w += FUN_TABLE
        elif kind in ("mp_native_qstr_table", "mp_native_obj_table"):
            w += RAM  # not used by the engine
        elif isinstance(kind, int):
            w += HOST + 0x100 + 4 * kind + 1
        else:
            bad += 1
        struct.pack_into("<I", text, addr, w & 0xffffffff)
    assert not bad
    uc.mem_write(TEXT, bytes(text))

    def sym(name):
        s = env.find_sym(name)
        return TEXT + s.section.addr + s["st_value"]

    # RAM layout: arena, framebuffer, ring, io table, strings, stack at top
    arena, arena_size = RAM, 2 << 20
    fb = RAM + arena_size
    ring = fb + 320 * 240 * 2
    io = ring + 4096 * 4
    uc.mem_write(io, struct.pack("<8I", *[HOST + 4 * i + 1 for i in range(7)] + [0]))

    names = {n.lower(): n for n in os.listdir(game)}
    files = {}
    st = {"insns": 0, "calls": {}}

    def cstr(addr):
        out = bytearray()
        while True:
            b = uc.mem_read(addr, 64)
            i = b.find(0)
            if i >= 0:
                return bytes(out + b[:i]).decode()
            out += b
            addr += 64

    def host(uc, addr, size, _):
        off = addr - HOST
        if off % 4 or off >= 0x800:
            return
        r = [uc.reg_read(x) for x in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        ret = 0
        if off >= 0x100:
            idx = (off - 0x100) // 4
            st["calls"][idx] = st["calls"].get(idx, 0) + 1
            if idx == 50:  # memset(d, c, n)
                uc.mem_write(r[0], bytes([r[1] & 0xff]) * r[2])
            elif idx == 51:  # memmove(d, s, n)
                uc.mem_write(r[0], bytes(uc.mem_read(r[1], r[2])))
            else:
                print("unexpected mp_fun_table call", idx)
                uc.emu_stop()
            ret = r[0]
        else:
            n = off // 4
            if n == 0:
                name, write = cstr(r[1]), r[2]
                try:
                    f = open(os.path.join(game, names.get(name.lower(), name)), "wb" if write else "rb")
                    h = 0
                    while h in files:
                        h += 1
                    files[h] = f
                    ret = h
                except OSError:
                    ret = -1
            elif n == 1:
                data = files[r[1]].read(r[3])
                uc.mem_write(r[2], data)
                ret = len(data)
            elif n == 2:
                ret = files[r[1]].write(bytes(uc.mem_read(r[2], r[3])))
            elif n == 3:
                files[r[1]].seek(r[2])
            elif n == 4:
                f = files[r[1]]
                cur = f.tell()
                f.seek(0, 2)
                ret = f.tell()
                f.seek(cur)
            elif n == 5:
                files.pop(r[1]).close()
            elif n == 6:
                print(cstr(r[1]).rstrip())
        uc.reg_write(UC_ARM_REG_R0, ret & 0xffffffff)

    def block(uc, addr, size, _):
        st["insns"] += size

    def unmapped(uc, access, addr, size, value, _):
        print("unmapped access at %08x (pc %08x)" % (addr, uc.reg_read(UC_ARM_REG_PC)))
        return False

    uc.hook_add(UC_HOOK_CODE, host, begin=HOST, end=HOST + 0x7ff)
    uc.hook_add(UC_HOOK_BLOCK, block, begin=TEXT, end=TEXT + len(text))
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, unmapped)

    def call(name, *args):
        for reg, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            uc.reg_write(reg, v & 0xffffffff)
        uc.reg_write(UC_ARM_REG_SP, RAM + RAM_SIZE - 64)
        uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        try:
            uc.emu_start(sym(name), STOP)
        except UcError as e:
            print("emulation error in %s: %s pc %08x" % (name, e, uc.reg_read(UC_ARM_REG_PC)))
            sys.exit(1)
        r0 = uc.reg_read(UC_ARM_REG_R0)
        return r0 - (1 << 32) if r0 & 0x80000000 else r0

    err = call("fj_core_init", arena, arena_size, io)
    print("fj_core_init ->", err, cstr(call("fj_core_game")))
    if err:
        print(cstr(call("fj_core_error")))
        sys.exit(1)
    music = os.environ.get("MUSIC")
    if music:
        strs = io + 64
        uc.mem_write(strs, b"music\0" + music.encode() + b"\0")
        call("fj_core_config", strs, strs + 6)
    call("fj_core_set_output", fb, 320, 240, 16)  # x0, y0 stay 0 (5th/6th args on the stack)
    call("fj_core_set_audio", ring, 4096, 0)
    click = None
    if os.environ.get("CLICK"):
        cf, cxy = os.environ["CLICK"].split(":")
        click = (int(cf),) + tuple(int(v) for v in cxy.split(","))
    per = []
    for f in range(1, frames + 1):
        st["insns"] = 0
        if click and click[0] <= f < click[0] + 3:
            call("fj_core_input", click[1], click[2], 1)
        else:
            call("fj_core_input", 160, 100, 0)
        alive = call("fj_core_frame")
        per.append(st["insns"] / 3)
        if f in shots:
            p = os.path.join(outdir, "mpy%05d.png" % f)
            png(p, bytes(uc.mem_read(fb, 320 * 240 * 2)))
            print("wrote", p)
        if not alive:
            print("engine stopped at frame", f, cstr(call("fj_core_error")))
            break
    print("frames %d, ~%.0fk instructions/frame avg, %.0fk max; arena peak %d; engine stack %d" % (
        len(per), sum(per) / len(per) / 1000, max(per) / 1000, call("fj_core_mem_peak"),
        call("fj_core_stack_used")))
    print("mp_fun_table calls:", st["calls"])


main()
