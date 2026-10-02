# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Count Thumb-2 instructions per emulated frame under Unicorn.

    python3 bench.py sms ../testroms/game.sms 1800 [script]
    python3 bench.py gb Tetris.gb 1800        # calibration, parent project

script is the host_sms one: "from-to:hexmask,..." held on frames [from, to).

Every 10th frame is counted (block hook + capstone); the rest run unhooked.
Needs: pip install unicorn capstone pyelftools
"""
import sys
from capstone import CS_ARCH_ARM, CS_MODE_MCLASS, CS_MODE_THUMB, Cs
from elftools.elf.elffile import ELFFile
from unicorn import UC_ARCH_ARM, UC_HOOK_BLOCK, UC_MODE_MCLASS, UC_MODE_THUMB, Uc
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP

kind, rom_path, frames = sys.argv[1], sys.argv[2], int(sys.argv[3])
elf = ELFFile(open("bench_%s.elf" % kind, "rb"))
uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.mem_map(0x10000000, 1 << 20)
uc.mem_map(0x20000000, 512 << 10)
uc.mem_map(0x11000000, 8 << 20)
uc.mem_map(0x0FFFF000, 0x1000)
for seg in elf.iter_segments():
    if seg["p_type"] == "PT_LOAD" and seg["p_filesz"]:
        uc.mem_write(seg["p_paddr"], seg.data())
entry = elf.get_section_by_name(".symtab").get_symbol_by_name("bench_entry")[0]["st_value"]
rom = open(rom_path, "rb").read()
uc.mem_write(0x11000000, rom)
RET = 0x0FFFF000


def call(cmd, arg):
    uc.reg_write(UC_ARM_REG_R0, cmd)
    uc.reg_write(UC_ARM_REG_R1, 0x11000000)
    uc.reg_write(UC_ARM_REG_R2, arg)
    uc.reg_write(UC_ARM_REG_SP, 0x20080000)
    uc.reg_write(UC_ARM_REG_LR, RET | 1)
    uc.emu_start(entry | 1, RET)
    return uc.reg_read(UC_ARM_REG_R0)


syms = sorted((sym["st_value"] & ~1, sym.name) for sym in elf.get_section_by_name(".symtab").iter_symbols()
              if sym["st_info"]["type"] == "STT_FUNC")
prof = {}
blocks = {}


def func_of(addr):
    lo, hi = 0, len(syms) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if syms[mid][0] <= addr:
            lo = mid
        else:
            hi = mid - 1
    return syms[lo][1]


cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
cache = {}
count = [0]


def on_block(uc, addr, size, _):
    k = (addr, size)
    n = cache.get(k)
    if n is None:
        n = cache[k] = (sum(1 for _ in cs.disasm_lite(bytes(uc.mem_read(addr, size)), addr)), func_of(addr))
    count[0] += n[0]
    prof[n[1]] = prof.get(n[1], 0) + n[0]
    blocks[addr] = blocks.get(addr, 0) + n[0]


SCRIPT = [a for a in sys.argv[4:] if a != "-v"]
SCRIPT = [(int(r.split(":")[0].split("-")[0]), int(r.split(":")[0].split("-")[1]), int(r.split(":")[1], 16))
          for r in SCRIPT[0].split(",")] if SCRIPT and SCRIPT[0] else []


def pad_sms(f):
    p = 0
    for a, b, m in SCRIPT:
        if a <= f < b:
            p |= m
    return p


def pad_gb(f):  # parent host_main.c tetrisplayer script, peanut mask (1 = pressed)
    for at, hold, m in [(600, 6, 0x08), (720, 6, 0x08), (840, 6, 0x08), (960, 6, 0x01),
                        (1020, 12, 0x20), (1080, 12, 0x10), (1140, 60, 0x80)]:
        if at <= f < at + hold:
            return m
    return 0


if kind == "sms":
    system = {".gg": 1, ".sg": 2}.get(rom_path[rom_path.rfind("."):].lower(), 0)
    print("init", call(0, len(rom) | (system << 28)))
else:
    print("init", call(0, len(rom)))
pad = pad_sms if kind == "sms" else pad_gb
samples = []
for f in range(frames):
    counted = f % 10 == 9
    if counted:
        h = uc.hook_add(UC_HOOK_BLOCK, on_block)
        uc.ctl_flush_tb()  # translated blocks only see hooks present when built
        count[0] = 0
    err = call(1, pad(f))
    if counted:
        uc.hook_del(h)
        uc.ctl_flush_tb()
        samples.append(count[0])
    if err:
        print("core error", err, "frame", f)
        break
    if (f + 1) % 300 == 0:
        print("frame %d: last %d, mean %.0f instrs/frame" % (f + 1, samples[-1], sum(samples) / len(samples)))
        sys.stdout.flush()
tot = sum(prof.values())
for name, c in sorted(prof.items(), key=lambda kv: -kv[1])[:12]:
    print("  %-24s %5.1f%%" % (name, 100.0 * c / tot))
if "-v" in sys.argv:
    for addr, c in sorted(blocks.items(), key=lambda kv: -kv[1])[:40]:
        print("  0x%08x %5.1f%%" % (addr, 100.0 * c / tot))
print("RESULT %s mean=%.0f max=%d instrs/frame over %d sampled frames" % (
    kind, sum(samples) / len(samples), max(samples), len(samples)))
