# NES port notes

## Decisions made without asking

- **Wrote the core instead of using agnes.** agnes (kgabis, MIT) was the obvious permissive
  candidate, but its PPU works dot by dot (89,342 steps a frame) and it has no APU and no
  CNROM. pico-infonesPlus/InfoNES and nofrendo are GPL/LGPL, so they were ruled out.
  `src/nescore.h` is new MIT code for this repo: switch-dispatched 6502 (opcode cases generated
  by `gen_ops.py`), scanline PPU, APU, mappers 0/1/2/3/4/7/66. The only borrowed data is the
  64-color palette from agnes (MIT, credited, `src/LICENSE.agnes`).
- **Scanline PPU.** Each visible line is drawn at dot 0 from the current scroll state, then the
  CPU runs to dot 257 (vertical increment and horizontal copy), dot 260 (MMC3 IRQ clock) and
  the end of the line. Sprite 0 hit is found while drawing and the flag is raised when the CPU
  reaches that dot. Mid-line register writes take effect on the next line.
- **1x picture, centered.** 256x240 goes into the 320x240 framebuffer at x=32, y=0. 2x would
  need 512x480, which does not fit in SRAM.
- **Audio.** Mono samples at 22050 Hz, 367 per frame, duplicated to stereo into the same kind
  of ring as the parent project. 367 samples is 60.08 fps, 0.03% slower than NTSC. Channels are
  point-sampled (no band limiting), mixed with the nesdev nonlinear tables, DC removed with a
  slow tracker. DMC sample fetches do not stall the CPU.
- **Mapper extras.** Added AxROM (7) and GxROM (66) since they are a few lines each. MMC1
  supports 512 KB SUROM. MMC3 PRG RAM protect is ignored (RAM always on).
- **PRG RAM** at $6000 is always present (8 KB inside the state buffer). For battery carts
  `code.py` passes its own 8 KB bytearray so it can be saved.
- **Saves every 30 s when changed**, and on exit. The parent `code.py` only saves on exit;
  NES games save at any time and people power off without Ctrl-C.
- **Controls** copy the parent `code.py` exactly (same chords). Opposite d-pad directions are
  masked since some NES games break on Left+Right.
- **Unofficial opcodes** are implemented, including the unstable SHY/SHX/AHX/TAS/XAA/LXA
  done the common way, so nothing jams except the real KIL opcodes.
- **Interrupt polling.** The CPU only rechecks NMI/IRQ after I/O, CLI/PLP/RTI, and at the start
  of each run slice (interrupt sources can only change then). Saves about 2% of total instructions.

## Verified (host, `src/run_tests.sh`, all pass)

- `nestest.nes` automation mode from $C000: all 8991 log lines match on PC, A, X, Y, P, SP
  and CPU cycle count; $02/$03 are 00.
- blargg `official_only`, `all_instrs` (MMC1, all 16 groups incl. unofficial), `instr_timing`,
  `oam_read`, `apu_test` 1-3 (length counter, length table, frame IRQ flag): pass via the
  $6000 protocol.
- blargg sprite 0 hit tests 01-11, `cpu_timing_test6`, branch timing 1-3: "PASSED" on screen,
  checked by eye, then pinned as frame CRCs.
- Frame CRCs pinned after checking screenshots: nestest menu, spritecans (NROM, many
  sprites), 240pee (UxROM + CHR RAM), quantum_disco_brothers (CNROM), oam3 (AxROM), and
  Blade Buster in game at frame 1800 (MMC3, IRQ status bar, scrolling).
- `.mpy` builds clean with `-Werror`, no undefined symbols beyond the dynruntime ones, no
  writable static data (bss 0), 65 KB.
- `code.py` passes `python3 -m py_compile`.

## Not verified

- Nothing has run on a Fruit Jam. The Python API, framebuffer output, audio ring and save
  path follow the parent project but are untested on hardware.
- Audio was only checked for levels (it plays, peaks ~16k), not listened to.
- Known test failures, all needing cycle-exact behavior the scanline design skips:
  `apu_test` 4-8 (jitter, length/IRQ timing, DMC), `mmc3_test_2` (A12 clocking via
  $2006, exact IRQ timing), `ppu_vbl_nmi`, `cpu_interrupts_v2`, dummy read/write tests,
  `ppu_open_bus`, `ppu_read_buffer`.
- No commercial game was tested (none available). Games known to need mid-scanline effects
  or exact MMC3 timing may glitch.

## Speed estimate

No board, so instruction counts come from `src/armbench/`: the same C built for Cortex-M4
with the same flags as the `.mpy`, run under Unicorn with a block hook counting Thumb-2
instructions on every 10th frame. Calibration: the parent project's `peanutgb.c` with Tetris
under the same bench is 1.49 M instructions/frame, and the board runs it at 62.6 fps
(README), which is 2.40 M cycles, so 1.61 cycles per instruction on this setup.

| workload | instr/frame | est. cycles | est. fps |
|---|---|---|---|
| Blade Buster in game, RGB565 output, mean | 1.38 M | 2.22 M | 67 |
| same, worst sampled frame | 1.44 M | 2.32 M | 65 |

Split: PPU render ~47%, CPU ~31%, framebuffer write ~15%, APU ~6%. So full speed (60.1 fps)
looks reachable with ~10% margin, but the parent port's live play ran ~20% below its bench
for an unknown reason; if that repeats, expect ~50-55 fps. Host: ~90 us/frame on an Apple
Silicon Mac without CRC.

To reproduce: `pip install unicorn capstone pyelftools`, then in `src/armbench`,
`./build.sh && python3 bench.py nes ../testroms/BladeBuster.nes 900`.

## Open questions / to do on hardware

- Measure real fps (serial prints it). If short, cheap options: skip the framebuffer write on
  alternate frames (still render for sprite 0), or detect idle loops.
- Check the state buffer and ROM land in SRAM vs PSRAM and whether it matters.
- Listen to audio; tune `VOLUME` and the DC tracker if there are pops.
- Check a battery save game (e.g. Zelda) writes and reloads `.sav`.
