# Atari Lynx port notes

## Decisions made without asking

- **Ported Handy instead of writing a core from scratch.** Handy (K. Wilkins) is zlib
  licensed (checked: the header in every libretro-handy source file). Mednafen's Lynx core
  is GPL and was avoided. `src/lynxcore.h` is a C rewrite of Handy's Suzy (sprites,
  collision, math), Mikey (timers, display DMA, palette, registers) and cart shifter, as one
  struct with no globals, statics or allocation. It is marked as an altered version, as
  zlib requires (`src/LICENSE.handy`).
- **Own 65C02.** `src/cpu65c02.h` (MIT) extends the MIT 6502 core from `atari2600/` with the
  65C02 instructions. The Lynx CPU is a 65SC02, so the Rockwell bit ops (RMB/SMB/BBR/BBS)
  are 1-byte, 1-cycle NOPs like Handy (`CPU_ROCKWELL=0`). WAI and STP work.
- **HLE boot.** The boot ROM is copyrighted and was never downloaded. Without
  `lynxboot.img`, `lynx_init` decrypts the cart's first loader frame itself (RSA,
  c^3 mod the published 51 byte modulus, written from the published algorithm), sets up
  Mikey/Suzy like the ROM leaves them, and jumps to $0200. The ROM entry points games call
  later (FE00, FE19, FE4A, FF80) are trapped and emulated. With `lynxboot.img` the real ROM
  runs instead. Formats: `.lnx` (LYNX header), BS93 `.o` homebrew (loaded to its address),
  and headerless images (treated as a cart). `lynx.info()` and serial report which path ran.
- **Timing model is Handy's.** 16 MHz ticks, a CPU cycle costs 5 ticks minus 4 per
  instruction, display DMA steals 720 ticks per line, sprite painting sleeps the CPU 3 ticks
  per Suzy bus access. Timers, display and audio only update when their next event is due.
  Branch and page crossing cycles are counted, so some demos run a little slower than Handy.
- **Audio.** 4 LFSR channels, advanced lazily at each output sample (22050 Hz) and box
  filtered over the sample period instead of Handy's per-event blip_buf. Stereo through
  MSTEREO/MPAN/ATTEN, DC removed, 16-bit. Samples per frame follow the game's frame rate
  (50/60/75 Hz), and `code.py` paces by samples, not by frames.
- **Display.** 2x (320x204) centered in the 320x240 framebuffer at y=18, written straight
  from display DMA, one pass per line for both rows. RGB565 or RGB332 if 16-bit does not fit.
- **Rotation is only logged.** The .lnx rotation byte is reported by `lynx.info()` and
  printed. Rotating a 2x picture does not fit the 320x240 framebuffer (204 wide becomes 320
  tall).
- **Writes under the ROM and vectors go to RAM**, as on hardware, so games that switch
  MAPCTL later see what they wrote.
- **Minimal UART.** SERCTL/SERDAT with TX timing from timer 4 and loopback (a Lynx with no
  cable hears its own bytes). T_TRIS waits on the TX interrupt. No ComLynx.
- **No EEPROM.** IODAT bit 4 reads high when it is an input, so games see a blank chip
  (all FFFF) instead of hanging. No saves, no AUDIN, no bank 1 carts.
- **Timer 6** is emulated (not linked in any chain); T_TRIS uses it as a timeout.
- **Default test ROM not shipped in CIRCUITPY.** `code.py` boots the first file by name, so
  a bundled cart could win over the user's game. `src/testcarts/lynxtest.lnx` is there to
  copy by hand.
- **Controls** follow the parent chords, with Button 2 + 3 as B since the Lynx has two
  fire buttons. Opposite directions are masked. Lefthanded mode (SPRSYS) swaps the
  directions like the hardware does.

## Recommended test ROM

T_TRIS.O (a Tetris clone, public domain) from the Demo Collection:
https://www.zophar.net/pdroms/lynx/demo-collection.html (direct zip:
https://www.zophar.net/download_file/11849). `src/get_testroms.sh` fetches it into
`src/testroms/pd/demo-collection/`. It boots as BS93, runs at 75 Hz, uses sprites, timers,
UART interrupts and sound, and is playable on the host (a scripted 2400 frame game is pinned
in `run_tests.sh`). Our own cart `src/testcarts/lynxtest.lnx` (source `lynxtest.c`, MIT, build with
`cl65 -t lynx -C lynx-coll.cfg -O -o lynxtest.lnx lynxtest.c`) tests the HLE boot path.

## Verified (host, `src/run_tests.sh`, all pass)

- Klaus Dormann's 6502 functional test (success trap $3469) and 65C02 extended opcodes test
  ($24F1). A separate check confirms all 32 Rockwell opcodes are 1-byte NOPs in 65SC02 mode.
- lynxtest.lnx (cc65 2.19, HLE boot): TGI drawing; literal, packed, 1bpp, 2x, flipped,
  stretched and tilted sprites; a collision pair read back through the depository; Suzy
  multiply/divide results stored to $00F0 and checked (06 06 03 01 A5); an 874 Hz square
  wave measured at 876 Hz.
- Pixel cross-check against Handy (libretro-handy built locally, 60 Hz, 300 frames,
  160x102 compared): lynxtest, The Dungeon, WUERFEL3, CHECK_EE, LR, DRAWTEST, befok, mydemo,
  ssprint, RAYCAST match exactly. DEMO0006, FONTTST2, T_TRIS, BOING, APFEL, CIR_TUNN differ
  only by animation or palette phase (checked by eye). I2C_TEST differs because it reads
  the missing EEPROM as FFFF where Handy emulates one.
- Frame CRCs pinned after checking screenshots: lynxtest, T_TRIS (title and a 2400 frame
  scripted game), Catkanoid title, The Dungeon, DEMO0006, WUERFEL3, LR, DRAWTEST, befok,
  mydemo, ssprint, RAYCAST.
- `.mpy` builds clean with `-Werror`, bss 0, only const function objects in
  `.data.rel.ro`, 51 KB.
- `code.py` passes `python3 -m py_compile`, and runs under CPython with stub modules
  (display, audio, keypad) through boot, pacing and screenshot.
- `tools/fb2png.py` turns the host framebuffer dump (same format as the serial screenshot)
  into PNGs.

## Not verified

- Nothing has run on a Fruit Jam. API, framebuffer output, audio ring and pacing follow the
  nes/sms ports but are untested on hardware.
- No commercial carts tested (none available). Only cc65-built and public domain programs.
- The real boot ROM path (`lynxboot.img`) is written but untested, since no ROM was used.
- Audio checked by level and frequency only (lynxtest peak ~3300 at 876 Hz, T_TRIS ~960);
  not listened to.
- Catkanoid hangs when a game starts: an interrupt storm while MAPCTL maps the vectors to
  RAM. Handy with HLE boot hangs the same way, so it is likely the HLE, not Suzy/Mikey.

## Speed estimate

Instruction counts from `src/armbench/` (Cortex-M4 build, same flags as the `.mpy`, Unicorn,
every 10th frame counted). Calibration from the parent project: 1.61 cycles per instruction.

| workload | instr/frame | est. fps @ 150 MHz | needs |
|---|---|---|---|
| lynxtest | 1.13 M | 83 | 60 |
| T_TRIS | 1.07 M | 87 | 75 |
| The Dungeon | 0.77 M | 121 | 60 |
| Catkanoid title | 1.24 M | 75 | 60 |
| DEMO0006 mean | 1.30 M | 72 | 60 |
| DEMO0006 worst | 2.48 M | 38 | 60 |

Split on lynxtest: CPU loop ~41%, I/O writes incl. sprite painting ~32%, display ~12%,
audio ~9%. Most games should hold full speed with 15-40% margin, less if live play loses
~20% like the parent port did. Heavy sprite frames can drop to ~40 fps.

To reproduce: `pip install unicorn capstone pyelftools`, then in `src/armbench`,
`./build.sh && python3 bench.py lynx ../testroms/pd/demo-collection/T_TRIS.O 900`.

## Open questions / to do on hardware

- Measure fps on T_TRIS (75 Hz, needs the most headroom) and lynxtest.
- Take a `SCREENSHOT_AT = 300` dump and compare with a host dump
  (`./host_lynx run T_TRIS.O 300 fb.txt`, then `tools/fb2png.py`).
- Listen to audio, tune `VOLUME`.
- Check `lynx.STATE_SIZE` (66,800 bytes) lands in SRAM and whether moving it matters.
- If someone has a boot ROM, try `lynxboot.img` and see whether Catkanoid gets past the title.
