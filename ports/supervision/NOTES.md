# Watara Supervision port notes

## Decisions made without asking

- **Own core, from the public notes.** `src/svcore.h` (MIT) is written from Kevin Horton's
  "Supervision Reverse Engineering Notes" V4.00 (2011, Unlicense,
  github.com/GrenderG/supervision_reveng_notes). MAME's svision driver (GPL-2.0+, not
  BSD-3 as the brief guessed) was read as a behaviour reference only where the notes are
  silent; no code was taken from it or from Potator.
- **Own 65C02, shared with the Lynx port.** `src/cpu65c02.h` and `src/host_cpu_test.c` are
  copies of the MIT files in `../lynx/src` (only those; nothing Handy-derived). The
  Supervision's CPU is built with the Rockwell/WDC bit opcodes on (`CPU_ROCKWELL 1`), as
  MAME models it (W65C02). inufuto's games never use them.
- **Frame = NMI period.** The NMI comes every 65536 cycles (61.04 Hz) and is not tied to the
  LCD, which scans at 50.8 Hz on its own. One `run_frames(1)` runs one NMI period and takes
  the picture from VRAM at that moment. Games cannot see the LCD position, so nothing
  depends on scanline timing. One CPU cycle is one tick at 4 MHz.
- **Video** follows the notes' scan pseudocode: X/Y scroll, $30-byte lines ($60 when
  LCD_X_Size > $C3), 8 KB wrap, line start $1FE0 wraps to 0, LCD_X_Size below $A0 leaves the
  right columns as they were, display-enable bit blanks the screen. LCD_Y_Size is ignored
  (always 160 lines). The picture is kept as 2 bpp lines (6.4 KB), compared each frame,
  and only changed line pairs are redrawn.
- **Picture modes.** 1.5x (240x240, centred in 320x240) with blended in-between pixels by
  default: each 2x2 source block becomes 3x3, corners copied, edges the average of two,
  the centre the average of four, through a 13-entry colour table. 1.5x sharp (nearest)
  and 1x are on Button 1. RGB565, RGB332 if the 16-bit framebuffer does not fit.
- **Registers.** $2000-$3FFF decodes as $2000-$203F, with the mirrors the notes list
  ($2004-7, $202C-E). Reads return what was written except the joypad, link port, timer,
  IRQ status and the two flag-clearing reads.
- **VRAM DMA** ($2008-$200D) copies (L ? L : 256) * 16 bytes at once and stalls the CPU 6
  cycles per 5 bytes. Address registers are left advanced and the length at 0, as measured
  in the notes. Destinations outside VRAM write WRAM instead of the corruption the notes
  describe.
- **Timer** per the notes: counts down every 256 or 16384 cycles from the write, stays at
  0, writing 0 fires at once (MAME treats 0 as 256; the notes measured hardware). Timer IRQ
  is enabled by system control bit 1, audio DMA IRQ by bit 2 (bit 2 from MAME).
- **Banking.** $8000-$BFFF = bank (system control bits 5-7) modulo the bank count,
  $C000-$FFFF = the last 16 KB. ROMs must be a whole number of 8 KB; an 8 KB ROM is
  mirrored. Reads from $6000-$7FFF return $FF.
- **Sound** per the notes: squares at 125 kHz / (F + 1) with 12.5/25/50/75 % duty and
  length counters on the 65536-cycle prescaler, 15/7-bit noise LFSR with the divisor table,
  audio DMA nibbles every 256 << F cycles with its own bank bits, and the hardware's
  "add and clip at 15" mix per side. Square 1 is right, square 2 left. Generated lazily at
  22050 Hz, box filtered over each output sample. The DACs only go positive, so silence is
  0 and full scale is 30000.
- **Nothing allocates.** The state (WRAM, VRAM, picture lines) is one Python bytearray of
  `STATE_SIZE` bytes; ROM, framebuffer and audio ring are Python objects too.
- **Code size matters on the board.** Native code runs from the heap through the 16 KB
  cache, so only WRAM writes and mapped reads are inline; registers, VRAM writes and open
  bus go through two out-of-line functions. That took the CPU loop from 68 KB to 38 KB and
  the frame from 14.3 ms to 10.3 ms. `-O2` (44.5 KB `.mpy`) was as fast as
  `-O2 -funroll-loops` and faster than `-Os` (11.9 ms).
- **Pad.** Select is on C and Start 2, not on Coin, so Start + Coin stays the launcher's
  exit, as the Lynx adapter does with Option 1.
- **`.sv` only.** `.bin` belongs to the Atari 2600 in the launcher; Supervision `.bin`
  dumps must be renamed `.sv`.
- **No standalone `code.py`.** The launcher is the way to run it.

## Verified

On the host (`src/run_tests.sh`, all pass):

- Klaus Dormann's 6502 functional and 65C02 extended opcode tests.
- Our test cart `src/testcart/svtest.sv` (own, MIT, ca65): bank signatures through the
  $8000 window, VRAM DMA (pattern, registers advanced, length 0 afterwards), timer read-back,
  NMI / timer IRQ / audio DMA IRQ counts after 300 frames, joypad read, sound present.
- All 23 inufuto Supervision games boot, take Start and A, and reach gameplay; frame CRCs
  at frame 600 were checked by eye on screenshots. Holding Right moves the SwordWork player.

On the Fruit Jam (UID 07A038438B86EED5, CircuitPython 11.0.0-alpha.1, launcher from the SD
card, 2026-10-01):

- `sword.sv` and `svtest.sv` start through the launcher handoff and run at 61.0 fps,
  10.3 and 12.1 ms per frame (emu + input).
- Board screenshots `board-sword.png` (frame 300, title) and `board-svtest.png` (frame
  300) render correctly; svtest's counters on the board are the same as on the host.
- SwordWork gameplay driven by a scripted pad from the REPL for 900 frames gives the same
  frame CRC as the host (7013edf6): the core is bit-exact on the board. 10.05 ms average,
  18.4 ms worst frame.

## Not verified

1. Sound judged by ear (host WAV checks only: the sample DMA sawtooth is at the expected
   122 Hz, levels and stereo placement as designed).
2. The arcade pad and the onboard buttons on real pins (the adapter mapping is covered by
   the launcher's own tests elsewhere; Start reached the core through the host script only).
3. Commercial games (none used). Games that rely on LCD_Y_Size tricks, mid-frame register
   changes, the link port or VRAM DMA to non-VRAM addresses will differ.
4. RGB332 output (code path exists, the board always got the 16-bit framebuffer).
5. Exact CPU cycle costs of the audio DMA fetch and the start delay of a triggered sample.

## Test programs

- inufuto's Supervision games (`src/get_testroms.sh`, into gitignored `src/testroms/`):
  free downloads from the author's site, http://inufuto.web.fc2.com/8bit/ (each game page
  links `../svision/<game>_bin.zip`). No written licence; treated as freeware, as the SMS
  port does for SwordWork. Not redistributed here.
- Klaus Dormann's CPU tests: GPL-3, test data only, downloaded at test time.
- `src/testcart/svtest.sv`: ours, MIT, built from `svtest.s`.

## Board workflow notes

- Handing off to a game from the REPL with a soft reload (raw REPL `fj.launch()`, then
  Ctrl-D) kept failing with "Too many display busses" in `fjarcade.Video` once the REPL had
  been entered; the 16-bit framebuffer attempt fails and its bus slot stays taken for the
  8-bit retry. Writing the handoff with `fj.handoff_write()` and `microcontroller.reset()`
  works every time. Users never go through the REPL, so the launcher itself is fine.
- One hard reset straight after an SD write session gave a one-off `SyntaxError` importing
  `fjarcade.py` (an SD read glitch on the first access); the file on the card was intact
  and the next boot was fine. The launch script unmounts `/sd` before resetting.
