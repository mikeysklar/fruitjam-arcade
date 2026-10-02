# Atari 2600 port notes

Unattended port, 2026-09-29. Nothing here has run on a Fruit Jam yet.

## Core choice

No permissively licensed 2600 core in C turned up that was small and complete. Stella, z26,
Javatari and 8bitworkshop are GPL/AGPL. ruby2600 (MIT) is Ruby and has no audio. So the whole
core was written here, MIT like the rest of the repo:

- `src/cpu6502.h`: NMOS 6502 as a code fragment included into a run loop, with the bus
  supplied as macros. Documented opcodes, NMOS decimal mode, and the common undocumented
  opcodes (LAX, SAX, DCP, ISB, SLO, RLA, SRE, RRA, ANC, ALR, ARR, SBX, NOPs, SBC $EB). The
  unstable ones (XAA, LAS, SHA, SHX, SHY, TAS) use the usual approximations. JAM stops the core.
  No IRQ/NMI, since the 6507 has no pins for them (BRK still works).
- `src/atari2600.c`: TIA video and audio, RIOT, bankswitching, module glue.
- `src/palette.h`: NTSC palette computed by `src/gen_palette.py` from a YUV model, so no
  palette data was copied from another emulator.

## Decisions made without asking

- Every bus access costs one cycle and internal cycles are counted, so a store lands on the
  right cycle. Dummy reads and writes are counted but not put on the bus (no side effects).
- TIA renders lazily. A register write takes effect at the color clock at the end of the write
  cycle. No per-register extra delays (real TIA has 1-2 clock delays for some registers).
- RESPx: in HBLANK the object goes to pixel 3 (players) or 2 (missiles, ball); otherwise to
  beam + 5 (players) or beam + 4 (missiles, ball). Double and quad players start 1 pixel later.
- HMOVE applies all motion at once when written. Written during HBLANK it blanks the first 8
  pixels of that line (the "HMOVE bars").
- RESMPx: missile hidden while set; on release it is centered on the player (+3, +6 or +10).
- Priority: P0/M0 > P1/M1 > PF/BL > BK, or PF/BL first with CTRLPF bit 2. Score mode colors
  the playfield with COLUP0/COLUP1 per half, and is ignored when PF priority is set.
- Frame boundary: VSYNC switching on. Line 0 is the line where VSYNC switches off. After 400
  lines without VSYNC a frame is forced.
- Display window: scanlines 12 to 251 after VSYNC (240 rows, `TOP` in code.py). Measured on 11
  homebrews, VBLANK is off between lines 28 and 237, so this shows all of them.
- Framebuffer: 320x240, each TIA pixel written as a pixel pair (RGB565 as one 32-bit store).
- RIOT timer: reads N-1 on the cycle after the write, counts down once per interval, and after
  passing 0 wraps to $FF and counts every cycle. TIMINT bit 7 is set from then until INTIM is
  read. PA7 edge detect is not done.
- TIA reads: collision bits in D7/D6, the rest 0 (real hardware leaves the last bus value there).
  Paddle inputs read 0. INPT5 always reads "not pressed". The INPT4 latch mode is ignored.
- Audio: channel waveforms follow the AUDC table (poly4, poly5, poly9, div 2/6/31/93 and
  combinations) implemented from the documented behavior, not from another emulator's code.
  Two TIA audio clocks per scanline, mono, box-filtered down to 22050 Hz, 0..30000 (no negative
  values, so silence is 0 and there is no pop at start).
- Pacing: code.py paces frames by audio samples made (`atari2600.samples()`), not a fixed
  60 Hz, so games with odd line counts, and PAL games, run at their own rate and stay locked to
  the DAC.
- Bankswitching: 2K and 4K are mirrored. 8K: E0 if an E0 hotspot pattern is found, else 3F if
  `STA $3F` appears twice and no F8 hotspot access is found, else FE if an Activision JSR
  pattern is found, else F8. 16K is F6, 32K is F4 (3F if its pattern wins). Superchip when the
  first 256 bytes of every 4K bank are one value. F8/F6/F4 start in the last bank. The byte
  patterns are the ones commonly used by 2600 cart detectors.
- DPC+/CDF (ARM Harmony) carts are detected by their "DPC+" / "CDF" tags and refused with a
  clear error (init returns 2), rather than running garbage. DK Arcade 2600 is one of these.
- Onboard buttons: B1 left, B2 right, B3 fire, B1+B2 reset, B2+B3 up, B1+B3 down, all three
  select. Up/down need a chord because there are only three buttons. Diagonals and moving
  while firing are not possible without a gamepad.
- Gamepad: Start = reset, Select = select, L1/R1 toggle difficulties, L2/R2/Home toggle B&W.

## What was verified (host only)

`make test` in `src/` runs all of this with the system `cc`:

- Own CPU checks (`host_cpu_test`): LAX, SAX, DCP, ISB, SLO, RRA, SBX, ANC, ALR results; cycle
  counts for page-crossing reads, stores, RMW abs,X, (zp),Y, NOP abs,X, JSR, a taken branch
  across a page; BCD ADC and SBC. PASS.
- Klaus Dormann's 6502_functional_test (downloaded, not committed): reaches the success trap
  at $3469 after 30,646,176 instructions / 96,241,367 cycles. PASS. This covers all documented
  opcodes, flags and decimal mode.
- Own test cart `src/testrom/testrom.bin`, built by `make_testrom.py` (committed, all my code):
  pixel checks on the RGB565 framebuffer for playfield blocks (reflected), P0/P1/M0/BL positions
  predicted from cycle counts (39, 50 after HMOVE, 56, 65), sprite pattern $C3, M0-over-P1
  priority, a mid-line COLUBK change at pixel 52, collision latches (the next frame's background
  depends on P0-PF and M0-P1), the fire button, the RIOT timer (overscan), and a 981 Hz square
  wave (AUDC 4, AUDF 15) measured at 971.7 Hz with the right amplitude. Plus a CRC lock of
  frames 4 and 8. PASS.
- Synthetic bankswitch carts from `make_banktests.py`: F8, F6+Superchip RAM (write/read ports),
  F4, E0, 3F and FE (JSR/RTS through $01FE) each detected correctly and landing in the right
  bank. PASS.
- 11 freely distributed homebrews from github.com/retrobrews/atari2600-games (2K, 4K, F6, F4;
  downloaded to `src/testroms/`, not committed) run 1200 frames with no JAM, and screenshots
  look right by eye: Thrust, Jammed, Halo 2600, Three.s, Anguna, Fishy, Stardust, Evil Magician
  Returns, Kelly Kangaroo (its 6-digit score kernel draws cleanly), Turtle Bay, Solar Plexus.
  They were not compared against a reference emulator frame by frame.
- `atari2600.mpy` builds clean (51 KB, armv7emsp, no undefined symbols besides the natmod
  runtime, bss 0, the natmod linker accepted it). Same mpy header bytes as the working peanutgb.mpy.
- `code.py` passes `python3 -m py_compile`.

Not verified: loading the .mpy on a board, the Python bindings at runtime, HDMI output, DAC
audio, the keypad chords, the gamepad, and speed. No commercial ROMs were tested.

## Speed estimate

No board here. As a rough calibration, the root Peanut-GB host build runs Tetris at about 11,600
fps on this Mac, against 62.6 fps emulation-only on the Fruit Jam (a ratio of about 185). This
core, drawing into an RGB565 framebuffer, runs the homebrews at 19,000 to 23,000 host fps,
which by the same ratio is about 100 to 120 fps on the board. That is a guess across very
different CPUs. The CPU loop is a switch per opcode with inline ROM/RAM access and a function
call for every TIA/RIOT access, which may cost more on a Cortex-M33 than the ratio suggests. So 60 fps is likely but not
certain. Games with heavy mid-line TIA writes cost more than average.

If it is slow: build with `OPT="-O2"` (no unrolling, smaller code), use the RGB332 fallback, or
turn sound off to see how much the audio path costs.

## Timing accuracy risks

- Per-register TIA write delays are not modelled. Games that rewrite PF or GRP within a pixel
  or two of where it is drawn (asymmetric playfields at the exact edge, some 48-pixel kernels)
  can be off by 1-4 pixels.
- HMOVE is instantaneous. Mid-line HMOVE tricks (Cosmic Ark stars, some Activision effects),
  changing HMxx within 24 cycles after HMOVE, and RESPx during the HMOVE blank are not right.
- RESPx offsets come from the documented +5/+4 rule, checked only against my own test cart.
- Dummy reads are not put on the bus, so RMW on TIA/RIOT registers and reads with side
  effects on dummy cycles differ from hardware. Rarely matters.
- Undriven TIA data bits read 0.
- The palette is an approximation. Colors will be close, not exact.
- Audio waveforms 2, 7, 14 and 15 are the least certain.

## Open questions

- Should the display show 160x240 at 2x wide only (current), or scale vertically too? There is
  no room for 2x vertical in 240 rows.
- Is `B1+B2 = Game Reset` the right chord, given most games need reset to start?
- Which `.bin` should win if CIRCUITPY has other `.bin` files (for example firmware)? Right
  now the first by name wins.
