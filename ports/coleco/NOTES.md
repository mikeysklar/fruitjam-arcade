# ColecoVision port notes

## Decisions made without asking

- **Wrote the core.** Every maintained ColecoVision emulator I found is GPL (Gearcoleco,
  ColEm is non-commercial only, MAME is GPL-2.0 as a whole and far too large). `src/z80.h` (Z80),
  `src/cv.h` (VDP, PSG, memory, I/O) and `src/gen_ops.py` are new MIT code for this repo.
  The Z80 is self-contained; I did not look at the one being written in `../sms`.
- **Open BIOS written from scratch.** I looked for a permissive replacement BIOS and found
  none. The only open one, `minbios.asm` in 8bitworkshop, is GPL-3 and follows the layout of
  the Coleco ROM. So `src/gen_bios.py` builds a tiny MIT BIOS: reset (DI, SP=$73FF, mute PSG,
  VDP off, jump through $800A if the header is AA55 or 55AA, no title screen), RST and NMI
  vectors to the cartridge's $800C-$8021 jump table, $0069 = 60 (NTSC), a font pointer at
  $006C (CVBasic reads it), and the six simplest OS7 entry points (FILL_VRAM, WRITE_VRAM,
  READ_VRAM, WRITE_REGISTER, READ_REGISTER, TURN_OFF_SOUND) implemented from their documented
  register interface. All other OS7 jump table entries just return. The font is font8x8
  (public domain). The real BIOS can be supplied as `coleco.rom` and is used when present.
- **Timing:** 228 CPU cycles per line, 262 lines, 59736 cycles a frame (59.92 Hz). No M1 wait
  states. Each visible line is drawn at the start of its slice. The VDP frame flag goes up at
  line 192; NMI is edge triggered from (flag AND IE), so turning IE on while the flag is set
  gives an NMI, as on hardware.
- **Z80 interrupt checks** happen before every instruction (one flag test). IRQ/IM 0/1/2 are
  implemented but nothing on the Coleco drives IRQ (spinner not emulated).
- **VDP details:** Graphics II uses MAME's masking (pattern mask includes the low color mask
  bits). Sprite Y above $E0 wraps to negative. Collision counts transparent sprite pixels,
  only on screen. With no 5th sprite the status low bits hold the index where the scan
  stopped. Mode combos: Text wins over Multicolor wins over Graphics II. The 4/16 KB bit is
  ignored (always 16 KB). The first control byte also sets the low address byte.
- **PSG:** 15-bit LFSR with taps 0x0003 (SN76489AN), period 0 acts as 0x400, periods 1-5 are
  a steady level (lets sample playback tricks work). Mono 22050 Hz, 368 samples a frame,
  point sampled, duplicated to stereo, rendered up to the current cycle on every PSG write.
- **Picture** 1x at (32, 24). The rest of the 320x240 framebuffer is painted in the backdrop
  color, redrawn only when the color changes.
- **Cartridges:** code.py pads the file with 0xFF to a multiple of 8 KB (16 KB above 32 KB).
  Up to 32 KB is mirrored into $8000-$FFFF. Above 32 KB is treated as MegaCart: last 16 KB
  bank fixed at $8000, reads of $FFC0-$FFFF switch $C000. `.col` only, since `coleco.rom` is
  the BIOS name.
- **Controls:** gamepad Start = keypad 1, Select = 2, X = *, Y = #; Select acts as a shift for
  the other keys (see README). Opposite joystick directions are masked. Tables live at the
  top of `code.py`. Controller 2 exists in the API (`set_input(st, m1, m2)`) but code.py only
  drives controller 1.
- **No save support** (the ColecoVision has none).

## Verified (host, `src/run_tests.sh`, all pass)

- `zexdoc` 67/67 OK and `zexall` 67/67 OK (undocumented flags and MEMPTR included), CP/M
  harness in `src/host_zex.c`, about 14 s each on an Apple Silicon Mac.
- `host_coleco selftest`: 16 checks on a hand-built VRAM: 5th sprite flag and number, only 4
  sprites drawn, collision (also with color 0 sprites), status read clears flags, EC shift,
  magnification, Y=255, NMI edge rules, keypad and joystick port values.
- Test carts, all booted with the built-in open BIOS, screenshots checked by eye, then pinned
  as frame CRCs:
  - CVBasic examples (Oscar Toledo G., BSD-2-Clause, built from source by
    `get_testroms.sh`): Viboritas (Graphics II game, sprites, text), Space Attack (Graphics I,
    sprites, moves left under scripted input), Brinquitos, oscar (full-screen Graphics II
    bitmap), bank (128 KB MegaCart, image from bank 2 while music plays from bank 1), test1,
    controller (joystick, both fires and keypad 0 light up as scripted).
  - `src/testcarts/modes.asm` (mine): Text mode with the BIOS font, then Multicolor.
  - Music cart: PSG output plays after a scripted button press, tone frequency formula
    checked (f = 3579545 / (32 x period)). Not listened to.
- `tools/fb2png.py` decodes the host framebuffer dump (same BEGIN_FB format as code.py) and
  the exact code.py chunking round trip gives an identical PNG.
- `.mpy` builds clean, bss 0, no undefined symbols beyond the dynruntime ones, 59 KB.
- `code.py` passes `python3 -m py_compile`.

## Not verified

- Nothing has run on a Fruit Jam.
- No commercial cartridge and no real BIOS was tried (neither is available to me, and I did
  not download them). With the real BIOS, the OS7 routines and title screen run as normal
  Z80 code, so they should work, but that is untested.
- Mid-scanline VDP tricks, exact VDP access timing (real VDP drops writes that come too fast;
  here all writes land), sprite overflow timing within a line.
- Audio was checked for levels and pitch only.

## Test ROM for the hardware screenshot

Recommended: **Viboritas** from the CVBasic examples, BSD-2-Clause, by Oscar Toledo G.
Source: https://github.com/nanochess/CVBasic `examples/viboritas.bas`. It is not a download;
`src/get_testroms.sh` builds it (clones CVBasic and gasm80, both BSD-2). A built copy is in
the gitignored `src/testroms/viboritas.col` (Space Attack is next to it too).

Copy `viboritas.col` to the CIRCUITPY root (or /sd), set `SCREENSHOT_AT = 600` in code.py.
The host PNG of the same frame is `images/viboritas_600.png`; with no input pressed the board
should give the same picture (RAM starts zeroed, so the run is deterministic). Frame CRC of
the 256x192 area is c887bdbf.

## Speed estimate

No board, so instruction counts come from `src/armbench/` (copied from `nes/src/armbench`):
the same C built for Cortex-M4 with the `.mpy` flags, run under Unicorn, counting Thumb-2
instructions on every 10th frame. Calibration is the nes agent's: peanutgb Tetris runs 1.49 M
instructions/frame under this bench and 62.6 fps on the board, so 1.61 cycles/instruction.
(No Game Boy ROM here to rerun that point.)

| workload | instr/frame | est. fps |
|---|---|---|
| Space Attack, scripted play, mean | 0.71 M | ~130 |
| Viboritas, mean | 0.74 M | ~126 |
| oscar (Z80 busy-waits all frame), mean | 1.02 M | ~91 |
| worst sampled frame, any cart | 1.08 M | ~86 |

Split for a busy cart: line render + framebuffer write ~60%, Z80 ~35%, PSG ~4%. The nes port
was estimated at 67 fps and measured ~85 uncapped, so the bench is if anything pessimistic.
Full speed (59.9 fps) should have a wide margin; speed risk is low.

To reproduce: `pip install unicorn capstone pyelftools`, then in `src/armbench`,
`./build.sh && python3 bench.py coleco ../testroms/oscar.col 600`.

## Open questions / to do on hardware

- Measure real fps (serial prints it). Take the SCREENSHOT_AT capture and compare with
  `images/viboritas_600.png`.
- Listen to audio; tune `VOLUME`.
- Try a commercial cartridge with a user-supplied `coleco.rom`.
- The Select-as-shift keypad chords send a brief "2" when Select goes down before the other
  button. Harmless in the games I know; say if it matters.
