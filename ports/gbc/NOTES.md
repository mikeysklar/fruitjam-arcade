# Game Boy Color port: notes

Unattended work, 2026-09-29. No board attached, so nothing here has run on a Fruit Jam.

## Core choice

- **tvecera/Peanut-GB, branch `gbc-rtc-fix`, commit `a7919bb`** (2023-07-24), `peanut_gb.h`
  copied to `src/peanut_gb_cgb.h` and patched. It is a GitHub fork of deltabeard/Peanut-GB and
  keeps the MIT header (Mahyar Koshkouei, SameBoy parts by Lior Halphon). The fork repo has no
  LICENSE file of its own; the file header is the licence I relied on.
- **Not pico-peanutGB's copy.** The root `src/peanutgb.c` and an earlier experiment in
  `impossible-gameboy/turbo` used pico-peanutGB's `peanut_gb.h`. That repo's LICENSE is
  **GPLv3**, and its copy has ~100 lines of its own changes (RAM placement, an RGB444 palette
  copy, qsort replaced by an insertion sort). To keep this subdir MIT-clean I started over from
  tvecera's file and wrote the equivalent changes myself.
- The fork already had: VRAM bank 1, WRAM banks 1-7, BG/OBJ palette RAM, BG attributes
  (palette, bank, flips, priority), OBJ bank/palette, GDMA and HDMA, KEY1 double speed via
  STOP, and CGB mode detection from header `0x143`. No CGB code written from scratch.

## Changes to the fork (`src/peanut_gb_cgb.patch`, 81 lines)

1. **LCD-off frame end.** The fork looped forever inside `gb_run_frame()` while the LCD was off.
   Ported upstream's `lcd_off_count`: a frame ends after 70224 cycles with the LCD off.
2. **HALT across frames.** The HALT fast-forward loop only exited on an enabled interrupt, so a
   game halted with only joypad/timer/serial interrupts enabled never returned. It now returns at
   the frame end and the next call resumes halting (without waking the CPU).
3. **HALT step size.** While halted, the fork reused the last step size outside a few mode
   changes. In VBlank that overshot line 153 into line 1, so line 0 was never drawn (cgb-acid2's
   top row was white). Now every halted iteration steps to the next LCD event.
4. **RGB565 palette copy** (`fixPalette565`), updated on each palette write, so drawing is a
   lookup. Palettes start white like the palette RAM the fork already initialises.
5. **`__gb_read` returns 0xFF after an invalid-read error** instead of falling off the end.

Wrapper side (`src/peanutgbc.c`):

- `PGB_UNREACHABLE()` is defined empty. The core marks the code after its error callback
  unreachable, but our callback returns, so on ARM that was undefined behaviour (clang on the
  host traps there). The error callback also sets `gb_frame` so `run_frames()` stops right away.
  **The root `src/peanutgb.c` has the same latent issue** with upstream Peanut-GB; I did not
  touch it (outside this subdir).
- `qsort` (used for DMG-mode sprite priority) is mapped to a small insertion sort; natmods cannot
  link libc `qsort`.
- Save size uses upstream's table (adds code 5 = 64 KiB, rejects unknown codes); the fork's
  version read past its table.
- The MBC3 RTC ticks once per 60 frames (0.5% slow). Not persisted.

## Host test results (what passed)

`src/run_host_tests.sh` builds `host_test` with the system `cc` and checks, pixel for pixel
against the reference PNGs shipped with each test ROM (`check_png.py`, stdlib only):

| ROM | mode | frames | result |
|---|---|---|---|
| cgb-acid2 (Matt Currie, MIT) | CGB | 120 | PASS, pixel-exact, read back from the RGB565 framebuffer |
| dmg-acid2 (Matt Currie, MIT) | DMG | 120 | PASS, pixel-exact vs `dmg-acid2-dmg.png` |
| blargg cpu_instrs | CGB (header 0x80) | 4000 | PASS, pixel-exact vs `cpu_instrs-dmg-cgb.png` |

Before the fixes: cgb-acid2 failed on 66 pixels (all in row 0); dmg-acid2 and cpu_instrs
already passed.

Other checks:

- Robustness sweep, every `.gb`/`.gbc` in the c-sp test ROM pack v7.0 (4510 ROMs, 300 frames,
  5 s limit each): **0 hangs, 0 crashes**. 4256 ran clean; 254 stopped with a clean core
  error (invalid opcode or HALT with IE=0), which is how many mooneye-style tests end. The
  earlier pico-peanutGB-based build hung on 95 of 106 CGB-flagged ROMs.
- Tetris (DMG) 1800 frames with the scripted input: audio checksum `ec5b2f78` matches the
  root port exactly. The frame CRC differs (`e9b8212f` vs root `427b4e9b`) because the fork's
  timing gives different piece RNG; the screenshot shows normal gameplay.
- `.mpy` builds clean with `arm-none-eabi-gcc` 14.3, `-O2 -funroll-loops`: 30,321 bytes, text
  29,976, bss 0, only `mp_fun_table` / natmod symbols undefined (no libgcc, no soft-double,
  no malloc).
- `python3 -m py_compile CIRCUITPY/code.py` passes.

Not verified: anything on hardware, double speed timing correctness (no pass/fail ROM for it
that a scanline core can pass), HDMA with real games, RGB332 colour path (only RGB565 was
read back in the host test), a real commercial CGB game.

Test ROMs: `src/testroms/` (gitignored), from
`https://github.com/c-sp/game-boy-test-roms/releases/download/v7.0/game-boy-test-roms-v7.0.zip`.

## Speed risk

- **Double speed is the main risk.** In double speed the CPU executes twice the instructions per
  frame while the PPU work stays the same. The root DMG port spends ~16 ms/frame emulating on
  the board (62.6 fps). If most of that is CPU, double speed games land around 26-30 ms/frame,
  i.e. roughly 30-40 fps before the live-play overhead the root README reports. Estimate only.
- Normal-speed CGB games: the colour draw path is a table lookup per pixel, the same cost as
  the DMG path. The fork's line renderer does a bit more work in CGB mode (attributes, banks).
  On the host (arm64 Mac, no drawing) the fork build runs Tetris ~7% slower than the root build
  (11.1k vs 11.9k fps).
- State is ~75 KB (was ~25 KB for DMG). If the CircuitPython heap puts it in PSRAM, WRAM/VRAM
  access gets slower. Worth checking on the board.

## Decisions made without asking

- ROM order: per location, `.gbc` before `.gb`; `/sd` before `CIRCUITPY`.
- Module is `peanutgbc` (built from `peanutgbc.c`), API identical to `peanutgb` plus
  `is_cgb(st)`. In CGB mode `frame_crc()` hashes palette indices (0..63), not shades.
- Kept the fork's scanline timing model (HBlank first, then OAM, then draw) instead of porting
  upstream's newer mode order. Enough for cgb-acid2; changing it would be a bigger rewrite.
- Did not add power-off saves; same `.sav`-on-exit behaviour as the root port.

## For the user on hardware

1. Copy `gbc/CIRCUITPY/*` to the board, add a `.gbc`.
2. Check the serial console says `mode CGB` and look at fps for a normal-speed game and a
   double-speed game (many later GBC-only titles switch to double speed).
3. If memory errors appear at `bytearray(peanutgbc.STATE_SIZE)`, the 75 KB state did not fit.
