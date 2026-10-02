# CHIP-8 port notes

## Core and license

- Wrote the interpreter from scratch (`src/chip8.c`, MIT). No third-party code. The fonts are
  the standard CHIP-8 and SUPER-CHIP glyph bytes.
- The Timendus test ROMs are GPL-3.0. They are only downloaded into the gitignored
  `src/testroms/` for the host test and are never committed.

## Decisions made without asking

- Instruction set: CHIP-8 plus SUPER-CHIP 1.1 (`00CN`, `00FB`, `00FC`, `00FD`, `00FE`, `00FF`,
  `DXY0`, `FX30`, `FX75`, `FX85`). The SUPER-CHIP opcodes are always on; they do not clash with
  CHIP-8 programs. XO-CHIP is not implemented.
- SUPER-CHIP behavior follows the "modern" variant (Octo / Timendus option 2): lores scroll moves
  whole lores pixels, `DXY0` draws 16x16 in both modes, VF is 1 on any collision (not the
  legacy row count), mode switches do not clear the screen.
- `0NNN` (machine code call) stops with `ERR_OPCODE`. So does `0000`, which catches programs
  running off into empty memory.
- `FX0A` waits for a key to go down and come back up (COSMAC VIP). While blocked the frame ends
  early; timers keep counting.
- Display wait quirk is implemented as "a sprite draw ends the frame's instructions", the usual
  approach.
- Timers tick after the frame's instructions and after the audio for that frame is generated.
- Audio: 22050 Hz stereo int16 like the Game Boy port (a rate known to work with the Fruit Jam
  DAC setup), 367 samples per frame, so frames are paced at 60.08 Hz. Beep is a 440 Hz square
  wave, level 4000, both settable with `set_beep`.
- Scaling: lores 64x32 at 5x (320x160), hires 128x64 at 2x (256x128), centered. The screen is
  only redrawn on frames where it changed; switching modes clears the border first.
- ROM choice: first `.ch8` or `.sc8`. `.sc8` picks SUPER-CHIP quirks and 30 ipf. The brief only
  said `.ch8`; `.sc8` is a common extension for SUPER-CHIP programs so I added it.
- Default keymap follows the WASD/QE convention (5 7 8 9 / 6 4) most modern CHIP-8 games use.
  Onboard buttons: B1 left, B2 right, B3 a, chords B1+B2 up, B1+B3 down, B2+B3 b.
- The per-game `ROMS` entries in `code.py` (Pong, Tetris, Brix, Invaders, Tank, Blinky) come
  from the commonly documented key layouts of those games. Not checked against the games.
- RNG is xorshift32, seeded from `os.urandom` in `code.py`.

## What is verified

Host test (`make host_test && ./host_test --suite testroms`), all PASS:

| ROM | setup | result checked by eye, then CRC locked |
|---|---|---|
| 1-chip8-logo | CHIP-8 | logo drawn correctly |
| 2-ibm-logo | CHIP-8 | IBM logo drawn correctly |
| 3-corax+ | CHIP-8 | 22 checkmarks, 0 crosses |
| 4-flags | CHIP-8 | 47 checkmarks, 0 crosses |
| 5-quirks | platform 1, `QUIRKS_CHIP8`, 30 ipf | all six quirks ON/OFF as expected, all checkmarks |
| 5-quirks | platform 2, `QUIRKS_SCHIP`, 30 ipf | vF reset OFF, memory OFF, display wait NONE, clipping BOTH, shifting ON, jumping ON, all checkmarks |
| 6-keypad | platform 3 (`FX0A`), key 5 held frames 60-90 | "ALL GOOD" with checkmark |
| 8-scrolling | platform 1 (SCHIP lores modern) | all arrows in their boxes |
| 8-scrolling | platform 3 (SCHIP hires) | all arrows in their boxes |
| 7-beep | CHIP-8, 600 frames | sound timer active in 282 frames (SOS pattern) |

Also checked:
- `6-keypad` platform 1 (`EX9E`) with key 6 held shows only key 6 lit, and platform 2 (`EXA1`)
  shows every key but 6 lit (by eye only, not in the suite).
- 8-scrolling platform 2 (SCHIP lores legacy) fails, as expected: lores scrolling is modern.
- Every suite run also renders into a 320x240 RGB565 buffer and checks each logical pixel
  against the display, so the scaler and centering are exercised.
- `.mpy` builds clean with the brief's command: 4710 bytes, text 4316, bss 0, 2 GOT entries,
  no unresolved symbols beyond the dynruntime ones.
- `python3 -m py_compile CIRCUITPY/code.py` passes.

## Not verified

- Nothing has run on a Fruit Jam. The Python bindings (`mod_*` functions) were not exercised on
  any VM; no unix port build was available in `cp-lookup` and I did not build one there.
- The RGB332 path, audio through the real DAC, the gamepad and the button chords.
- Real games. Only the test suite.

## Speed

Should be a non-issue. 11 instructions per frame is trivial; the only real cost is a redraw of
320x160 RGB565 pixels (about 50k stores plus row copies) on frames where the screen changed,
well under 1 ms of the 16.6 ms frame at 150 MHz. The host runs the full suite in milliseconds.

## Open questions

- Should there be flicker reduction (e.g. OR the last two frames before drawing)? Many CHIP-8
  games flicker badly on a sharp HDMI picture.
- Is 5x lores with a black border fine, or should lores stretch to 320x240 (5x7.5, non-integer)?
