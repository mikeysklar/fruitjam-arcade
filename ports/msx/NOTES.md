<!--
SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
SPDX-License-Identifier: MIT
-->

# MSX port notes

## Decisions made without asking

- **Core is new MIT code, built from the coleco port.** `src/z80.h`, `src/z80_ops.inc`,
  `src/gen_ops.py` and `src/host_zex.c` are copies of `../coleco/src` (self-contained module,
  duplication accepted). The VDP code in `src/msx.h` is the coleco VDP with the interrupt
  moved from NMI to INT. AY-3-8910, SCC, PPI, slots and mappers are new.
- **BIOS: C-BIOS 0.29a built in.** License read from `doc/cbios.txt` in the official
  `cbios-0.29a.zip` (sourceforge): BSD-2-clause style, "Redistributions in binary form must
  reproduce the above copyright notice ... in the documentation and/or other materials".
  So: the two unmodified ROMs (`cbios_main_msx1.rom`, `cbios_logo_msx1.rom`, sha1 matching
  openMSX's C-BIOS_MSX1 machine) are in `src/cbios/` with `LICENSE.txt` (the notice) and a
  README saying they are not MIT. `gen_cbios.py` turns them into `src/cbios.h`, whose header
  comment says the same. README.md and the adapter docstring point at the license.
  The logo ROM costs 16 KB of .mpy and a 2 s logo (C-BIOS waits 120 frames either way).
  A user `msx.rom` (32 KB) replaces both.
- **Machine layout = openMSX's C-BIOS_MSX1**: slot 0 BIOS + logo, slot 1 cartridge, slot 2
  empty, slot 3 64 KB RAM, no expanded (secondary) slots. Secondary slot registers are not
  emulated because nothing in this layout is expanded; C-BIOS and real MSX1 BIOSes probe for
  expansion and work without it. Most MSX1 machines had none either.
- **Timing:** 228 cycles a line, 262 lines (59.92 Hz), like coleco. The MSX's one wait state
  per M1 cycle is added after each line from the R register count (`msx_run_frame`); a line
  with LD R,A in it gets no correction.
- **Mappers:** plain (placement from the "AB" header: 48/64 KB images with the header at
  $4000 start at $0000; up to 16 KB is mirrored over its 16 KB page and goes to $8000 when
  its start address is there), Konami (4000 fixed, 6000/8000/A000 registers), Konami SCC
  (5000/7000/9000/B000, SCC enabled by bank $3F in the $9000 register), ASCII8
  (6000/6800/7000/7800), ASCII16 (6000-67FF / 7000-77FF). Initial banks: Konami 0-3, ASCII 0.
  Bank numbers wrap modulo the ROM size, so non power of two dumps work.
- **Mapper guess** = openMSX's `guessRomType` heuristic (counts `LD (nnnn),A` = `32 lo hi` to
  each register address; ASCII8 gets -1; ties go to ASCII16, then Konami SCC, Konami,
  ASCII8). Up to 64 KB is plain. No database. Per-game override `mapper =` in the cfg.
  Known miss: the portraits slideshow (ASCII8, one bank write) is guessed ASCII16; its cfg on
  the card sets ascii8.
- **AY-3-8910:** clock 1.7897725 MHz; tone half period = 8 x period clocks, noise 17-bit LFSR
  (taps 0 and 3) shifted every 16 x period clocks, envelope 16 steps of 16 x period clocks,
  all 8 shapes. Tone periods under 5 are a steady level (sample playback with the mixer off
  works). Register read masks as on the chip. Port A = joystick (port picked by R15 bit 6),
  active low; bit 6 = 1, bit 7 (tape) = 0. 16 level log volume table (AY, not the 32-step
  YM2149 envelope). Output is one-sided, so a one-pole DC blocker follows the mixer.
- **SCC:** waveform RAM $9800-$987F (channel 5 shares channel 4's wave, plain SCC), periods,
  volumes, enable; $9880-$989F mirrors; deformation register ignored; reads of waveform RAM
  work, other registers read $FF. Periods under 9 silence the channel.
- **Key click** (PPI C bit 7) is a level in the mix. Cassette and CAPS LED are ignored.
- **Audio** mono 22050 Hz, 368 samples a frame, point sampled, rendered up to the current
  cycle on every PSG, SCC and click write, duplicated to stereo.
- **Picture** 1x at (32, 24); border in the backdrop color, redrawn on change (same as coleco).
- **Pad mapping** (adapter): joystick 1 and cursor keys on the D-pad, trigger A + Space,
  trigger B + M, F1, Return, 1, N, and a C-shift layer for 1-4, F2-F5, Esc. Pressing joystick
  and the matching key together has not upset any test ROM; `dpad =` in the cfg separates
  them if a game minds. Inputs are cached per pad mask, so set_input costs two native calls.
- **No save support:** SRAM cartridges are not emulated (`fj.Saver(path, 0)`).

## Verified

Host (`src/run_tests.sh`, all pass, Apple Silicon Mac):

- `zexdoc` 67/67 and `zexall` 67/67 OK (same core as coleco).
- `host_msx selftest`, 42 checks: VDP sprites/collision/status, INT follows frame flag and
  IE; slot map (BIOS, logo, empty page, RAM, slot register read back); PPI keyboard row read,
  bit set/reset; PSG R14 joystick ports 1 and 2, register masks; Konami, Konami SCC
  (incl. SCC enable/disable, wave read, period), ASCII8 and ASCII16 bank switching on a
  numbered 256 KB ROM, bank wrap; mapper guesses for each type; plain ROM placement;
  AY tone at 440 Hz (441 edges/s), SCC channel at 440 Hz (439), envelope sawtooth rate.
- ROMs booted through C-BIOS, screenshots looked at, then pinned as frame CRCs:
  CVBasic examples built for MSX (Viboritas, Space Attack, Brinquitos, oscar, test1,
  controller with joystick + triggers + key 1 lighting up as scripted; joystick left and
  cursor left move Space Attack's ship identically), CVBasic bank example as 128 KB ASCII16
  and 128 KB Konami MegaROMs (both guessed right, same picture as the coleco bank cart),
  World Rally demo (48 KB plain from $0000, intro scene and PSG music), portraits slideshow
  (256 KB ASCII8 with override).

Board (Fruit Jam, launcher, C-BIOS, CircuitPython 11.0.0-alpha.1):

- `msx_wrally_demo.rom`, `msx_viboritas.rom`, `msx_portraits.rom` (+ cfg `mapper = ascii8`),
  `msx_cvbasic_bank_konami.rom` all run at 59.9 fps through the launcher; the adapter's
  mapper print says plain at $0000, plain at $4000, ascii8 (cfg), konami (guessed).
- Screenshots at frame 600 (`board-wrally.png`, `board-viboritas.png`,
  `board-portraits.png`) are byte-identical to the host framebuffer at frame 600.
- emu+input per frame: World Rally 6.9-9.6 ms, Viboritas 7.9-8.3, portraits 5.4-6.4,
  bank Konami (Z80 busy-waits all frame) 8.6-11.8. 16.7 ms is the budget.
- `msx.mpy` 99,097 bytes (text 98,744: ~48 KB C-BIOS + ~50 KB code), bss 0.
- The DAC came up (no "sound off" line).

## Not verified

- Nobody listened to the board's audio. AY/SCC pitch and envelope rate are checked on host.
- Pad buttons on the board (no one to press them); host scripts exercise the same paths.
- No commercial cartridge and no real MSX BIOS were tried (not available, not downloaded).
  Konami SCC was only tested with the selftest's synthetic ROM, not a real SCC game.
- The standalone `CIRCUITPY/code.py` has not run on the board (the board's CIRCUITPY
  code.py is the launcher and was not touched). It passes `py_compile`.
- Mid-scanline VDP tricks, VDP access-speed limits, sprite timing within a line.

## Test ROMs (none committed; `src/get_testroms.sh` fetches/builds them)

- CVBasic examples by Oscar Toledo G., BSD-2-Clause, compiled for MSX with `cvbasic --msx`
  (and `-konami` for the Konami bank build), assembled with gasm80 (BSD-2).
- World Rally demo, theNestruo, GPL-3.0, `rally-demo.rom` from
  https://github.com/theNestruo/msx-wrally (48 KB plain).
- Justin Cyr female portraits slideshow, theNestruo, GPL-3.0,
  https://github.com/theNestruo/msx-justincyr-female-portraits (256 KB ASCII8).

Left on the SD card: `/sd/cart/msx_wrally_demo.rom`, `msx_viboritas.rom`,
`msx_portraits.rom`, `msx_portraits.fruitjam.cfg`, `msx_cvbasic_bank_konami.rom`.

## Board testing notes

- Launching from the REPL with `fj.launch()` (soft reload) after the menu had run gave
  "RuntimeError: Too many display busses" in `fjarcade.Video()` every time, on a board fresh
  from `microcontroller.reset()` too: the menu's picodvi framebuffer survives into the REPL
  and the soft reload. What works: write the handoff with `fj.handoff_write(path)` from the
  raw REPL, then `microcontroller.reset()`; nvm survives the reset and code.py starts the
  game on a clean display. That is what the screenshots used.

## To do

- Listen to audio on the board; tune AY/SCC/click levels.
- Try real Konami SCC games and a user BIOS.
- SRAM mappers (ASCII8/16 + SRAM, Hydlide 2, Xanadu), Konami SCC+ (Snatcher sound cart),
  ROM database by CRC for mapper-guess misses.
