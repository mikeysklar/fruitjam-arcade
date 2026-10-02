# CircuitPython Atari 7800

Atari 7800 games on an Adafruit Fruit Jam, running under stock CircuitPython. The emulator
(6502C "Sally", MARIA, TIA sound, RIOT, optional POKEY) was written for this project and is
compiled into a native `.mpy` module.

Not yet tested on hardware. See [NOTES.md](NOTES.md) for what has been verified.

## What you need

| part | notes |
|---|---|
| Adafruit Fruit Jam | RP2350 |
| CircuitPython 11.0.0-alpha | the `.mpy` is built for this version |
| HDMI monitor | 320x240, 160 pixel modes shown 2x wide |
| headphones or speaker | optional |
| USB gamepad | optional, the 3 onboard buttons work too |
| a `.a78` ROM | not included; no BIOS needed |

## Setup

Back up your `CIRCUITPY` drive first. `code.py` will be replaced.

```sh
git clone https://github.com/mikeysklar/fruitjam-arcade
cp -r fruitjam-arcade/ports/atari7800/CIRCUITPY/* /Volumes/CIRCUITPY/
cp MyGame.a78 /Volumes/CIRCUITPY/
```

Optional, for sound and gamepad:

```sh
circup install adafruit_fruitjam relic_usb_host_gamepad
```

The board resets and the game boots.

- The first `.a78` file found on the SD card (`/sd`), then on `CIRCUITPY`, is loaded. If there
  is none, the first headerless `.bin` is used, with the mapper guessed from its size.
- The serial console prints the mapper, POKEY location, and fps every 5 seconds.
- There is no BIOS and no Atari logo screen. The cartridge starts right away.

## Controls

| Fruit Jam | Atari 7800 |
|---|---|
| Button 1 | Left |
| Button 2 | Right |
| Button 3 | Fire (left button) |
| Button 1 + 2 | Reset |
| Button 2 + 3 | Up |
| Button 1 + 3 | Down |
| Button 1 + 2 + 3 | Pause |

| USB gamepad | Atari 7800 |
|---|---|
| D-pad or left stick | Joystick |
| A, X | Left button (button 1) |
| B, Y | Right button (button 2) |
| Start | Reset |
| Select | Select |
| L1, R1 | Pause |
| L2 | Toggle left difficulty (A/B) |
| R2 | Toggle right difficulty (A/B) |

Power-on difficulty switches are set at the top of `code.py`. Both default to B.

## Cartridges

| `.a78` header type | support |
|---|---|
| plain 16K / 32K / 48K | yes |
| SuperGame (128K and up) | yes, bank at $8000, last bank at $C000, second to last at $4000 |
| SuperGame + RAM at $4000 | yes |
| SuperGame 9 bank (144K, bank 0 at $4000) | yes |
| RAM at $4000 with A8 mirror | yes |
| POKEY at $4000 or $0450 | yes |
| VersaBoard | treated as SuperGame + 16K RAM |
| Activision, Absolute, banksets, Souper, XM, YM2151 | no, init reports an error |

## Build the `.mpy` yourself

Same toolchain as the Game Boy build in the root README:

```sh
cd fruitjam-arcade/ports/atari7800/src
make MPY_DIR=../../../../circuitpython ARCH=armv7emsp
cp atari7800.mpy ../CIRCUITPY/lib/
```

Host tests (system `cc`; the first run downloads wasmtime and 7800basic and builds the
7800basic sample programs as test cartridges):

```sh
make test
```

## How it works

- `src/cpu6502.h` is the NMOS 6502 core shared with `../atari2600` (a code fragment that
  counts every bus cycle). Time is kept in MARIA clocks: 4 per CPU cycle, 6 for TIA and
  RIOT accesses, 454 per scanline, 263 lines per NTSC frame (313 PAL).
- `src/atari7800.c` has MARIA, TIA sound, RIOT, POKEY, the cartridge mappers and the module
  glue. On each scanline MARIA reads the display list for the current zone and builds a
  160 cell line RAM, holding the CPU for the DMA time; the line RAM built on the previous line
  is drawn straight into the `picodvi.Framebuffer` row. No displayio refresh.
- Display list interrupts are NMIs raised when MARIA fetches a DLL entry with the DLI bit.
  WSYNC holds the CPU to the end of the line. MSTAT reports VBLANK outside lines 16-257.
- Screen row 0 is scanline 17 (`TOP` in `code.py`), so 240 of MARIA's 241 visible lines show.
- Sound: two TIA channels clocked twice per scanline, plus POKEY if the header asks for one,
  mixed to mono and resampled to 22050 Hz into a looping `audiocore.RawSample` on the TLV320
  DAC. Frames are paced by the samples made.
- All buffers are Python-owned. The native code allocates nothing.

## Known issues

- Speed on the board is not measured yet. Estimates are 70 to 100 fps; see NOTES.md.
- MARIA is line based: palette and CTRL writes take effect for a whole line, not mid-line.
- No BIOS, so games that depend on BIOS side effects may not start. 2600 carts are not
  supported (no 2600 mode).
- PAL carts run at PAL speed but use the NTSC palette.
- Only joystick port 1 with two buttons. No paddles, keypads, light gun, trackball,
  High Score Cartridge, SaveKey or AtariVox.

## License

MIT for everything in this directory, except `src/palette.h`, which holds MAME's Atari 7800
NTSC colors (BSD-3-Clause, see `src/LICENSE.mame`). The MARIA behavior follows MAME's
`maria.cpp` (also BSD-3-Clause), but no MAME code was copied.
