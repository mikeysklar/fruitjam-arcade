# SPDX-FileCopyrightText: 2026 Mikey Sklar
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the ARM (Cortex-M33) build of the core under Unicorn on a game folder.

Checks the code the board will run (Thumb-2 code generation, the coroutine
switch with MSPLIM, setjmp, the mini libc and the arena allocator) and
counts instructions per frame as a rough speed estimate.

    make -C tools/armemu      (or: make armemu)
    python3 tools/armemu/run.py GAMEDIR [frames] [shot_frame,...]

Needs: unicorn, pyelftools.
"""
import os
import struct
import sys
import zlib

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_HOOK_BLOCK, \
    UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, \
    UC_ARM_REG_LR, UC_ARM_REG_PC, UC_CPU_ARM_CORTEX_M33

HERE = os.path.dirname(os.path.abspath(__file__))
ELF = os.path.join(HERE, "..", "..", "build", "armemu", "armtest.elf")
HOST = 0x0F000000
RAM = 0x20000000
RAM_SIZE = 16 << 20
PPB = 0xE000E000


def png(path, fb, w=320, h=240):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for p in struct.unpack_from("<%dH" % w, fb, y * w * 2):
            raw += bytes((((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def main():
    game = sys.argv[1]
    frames = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    shots = [int(x) for x in sys.argv[3].split(",")] if len(sys.argv) > 3 else [frames]
    outdir = os.environ.get("OUT", "/tmp")

    elf = ELFFile(open(ELF, "rb"))
    syms = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()}
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS, cpu=UC_CPU_ARM_CORTEX_M33)
    uc.mem_map(RAM, RAM_SIZE, UC_PROT_ALL)
    uc.mem_map(HOST, 0x1000, UC_PROT_ALL)
    uc.mem_write(HOST, b"\x70\x47" * 0x800)  # bx lr
    uc.mem_map(PPB, 0x1000, UC_PROT_ALL)
    uc.mem_write(0xE000ED00, struct.pack("<I", 0x410FD210))  # CPUID: Cortex-M33
    for seg in elf.iter_segments():
        if seg["p_type"] == "PT_LOAD" and seg["p_filesz"]:
            uc.mem_write(seg["p_paddr"], seg.data())
    ctl = syms["armtest_ctl"]
    fb = syms["armtest_fb"]
    ring = syms["armtest_ring"]

    names = {n.lower(): n for n in os.listdir(game)}
    files = {}
    state = {"frame": 0, "insns": 0, "frame_insns": [], "dead": False, "ring_pos": 0}

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
        n = (addr - HOST) // 4
        if (addr - HOST) % 4:
            return
        r = [uc.reg_read(x) for x in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        ret = 0
        if n == 0:  # open
            name, write = cstr(r[1]), r[2]
            p = os.path.join(game, names.get(name.lower(), name))
            try:
                f = open(p, "wb" if write else "rb")
                h = 0
                while h in files:
                    h += 1
                files[h] = f
                ret = h
                if write:
                    names[name.lower()] = name
            except OSError:
                ret = -1
        elif n == 1:  # read
            data = files[r[1]].read(r[3])
            uc.mem_write(r[2], data)
            ret = len(data)
        elif n == 2:  # write
            ret = files[r[1]].write(bytes(uc.mem_read(r[2], r[3])))
        elif n == 3:  # seek
            files[r[1]].seek(r[2])
        elif n == 4:  # size
            f = files[r[1]]
            cur = f.tell()
            f.seek(0, 2)
            ret = f.tell()
            f.seek(cur)
        elif n == 5:  # close
            files.pop(r[1]).close()
        elif n == 6:  # log
            print(cstr(r[1]).rstrip())
        elif n == 7:  # frame(alive, init_err)
            if r[1]:
                print("init failed", r[1])
                uc.emu_stop()
                return
            f = state["frame"]
            if f:
                state["frame_insns"].append(state["insns"])
            state["insns"] = 0
            if f in shots:
                p = os.path.join(outdir, "arm%05d.png" % f)
                png(p, bytes(uc.mem_read(fb, 320 * 240 * 2)))
                print("wrote", p)
            if not r[0]:
                print("engine stopped at frame", f)
                state["dead"] = True
                uc.emu_stop()
                return
            if f >= frames:
                uc.emu_stop()
                return
            state["frame"] = f + 1
            x, y, b = 160, 100, 0
            uc.mem_write(ctl, struct.pack("<7i", x, y, b, 0, 0, 0, 0))
        uc.reg_write(UC_ARM_REG_R0, ret & 0xffffffff)

    def block(uc, addr, size, _):
        state["insns"] += size

    def unmapped(uc, access, addr, size, value, _):
        print("unmapped access at %08x (pc %08x)" % (addr, uc.reg_read(UC_ARM_REG_PC)))
        return False

    uc.hook_add(UC_HOOK_CODE, host, begin=HOST, end=HOST + 0xfff)
    uc.hook_add(UC_HOOK_BLOCK, block)
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, unmapped)
    uc.reg_write(UC_ARM_REG_SP, RAM + RAM_SIZE - 16)
    uc.reg_write(UC_ARM_REG_LR, HOST + 0x800 | 1)
    try:
        uc.emu_start(syms["armtest_main"] | 1, HOST + 0x800)
    except UcError as e:
        print("emulation error:", e, "pc %08x lr %08x" % (uc.reg_read(UC_ARM_REG_PC), uc.reg_read(UC_ARM_REG_LR)))
        sys.exit(1)
    fi = state["frame_insns"]
    if fi:
        # The block hook counts bytes of code; Thumb-2 averages ~3 bytes an instruction.
        avg = sum(fi) / len(fi) / 3
        srt = sorted(fi)
        top = sorted(range(len(fi)), key=lambda i: -fi[i])[:5]
        print("frames %d  ~k instructions/frame: avg %.0f  p50 %.0f  p95 %.0f  max %.0f" % (
            len(fi), avg / 1000, srt[len(srt) // 2] / 3000, srt[len(srt) * 95 // 100] / 3000, srt[-1] / 3000))
        print("heaviest frames:", ", ".join("%d (%.0fk)" % (i + 1, fi[i] / 3000) for i in top))


if __name__ == "__main__":
    main()
