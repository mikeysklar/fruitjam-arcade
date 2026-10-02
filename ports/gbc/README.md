# CircuitPython Game Boy Color

Game Boy Color games on an Adafruit Fruit Jam, running under stock CircuitPython. The emulator
is [Peanut-GB](https://github.com/deltabeard/Peanut-GB) in
[tvecera's CGB fork](https://github.com/tvecera/Peanut-GB/tree/gbc-rtc-fix), compiled into a
native `.mpy` module called `peanutgbc`. It sits next to the root `peanutgb` module and does not
replace it.

Plain Game Boy (`.gb`) carts still work. They run in DMG mode with greys.

## What you need

| part | notes |
|---|---|
| Adafruit Fruit Jam | RP2350 |
| CircuitPython 11.0.0-alpha | the `.mpy` is built for this version |
| HDMI monitor | game shows at 2x |
| headphones or speaker | optional |
| USB gamepad | optional, the 3 onboard buttons work too |
| a `.gbc` or `.gb` ROM | not included |

## Setup

Back up your `CIRCUITPY` drive first. `code.py` will be replaced.

```sh
git clone https://github.com/mikeysklar/fruitjam-arcade
cp -r fruitjam-arcade/ports/gbc/CIRCUITPY/* /Volumes/CIRCUITPY/
cp MyGame.gbc /Volumes/CIRCUITPY/
```

Optional, for sound and gamepad:

```sh
circup install adafruit_fruitjam relic_usb_host_gamepad
```

The board resets and the game boots.

- ROM choice: first `.gbc` on the SD card (`/sd`), else first `.gb` there, then the same order
  on `CIRCUITPY`.
- Saves go to `MyGame.sav` next to the ROM, written when `code.py` exits (same as the root port).
- Serial console prints `mode CGB` or `mode DMG` at start, then fps every 5 seconds.

## Controls

| Fruit Jam | Game Boy |
|---|---|
| Button 1 | Left |
| Button 2 | Right |
| Button 3 | A |
| Button 1 + 2 | Start |
| Button 1 + 3 | Down |
| USB gamepad | D-pad, A, B, Start, Select |

## Speed

Not measured on a board yet. The root DMG port reaches 58 fps with drawing and 43-48 fps in
live play at 150 MHz. Colour games in normal speed should be close to that. Games that switch
the CPU to double speed run twice the CPU work per frame; expect roughly 30-40 fps there.

## Build the `.mpy` yourself

Same toolchain as the root README.

```sh
cd fruitjam-arcade/ports/gbc/src
make MPY_DIR=../../../../circuitpython ARCH=armv7emsp
cp peanutgbc.mpy ../CIRCUITPY/lib/
```

Host test (no board, no `MPY_DIR`), after downloading the test ROMs listed in `NOTES.md`:

```sh
./run_host_tests.sh
```

It checks cgb-acid2, dmg-acid2 and blargg `cpu_instrs` pixel for pixel against their
reference images.

## How it works

- `src/peanutgbc.c` wraps the CGB core and minigb_apu (sound) as a CircuitPython native module.
  Same API as the root `peanutgb`, plus `is_cgb(st)`.
- `src/peanut_gb_cgb.h` is tvecera's fork with a few timing fixes, listed in
  `src/peanut_gb_cgb.patch`.
- Colour mode is picked from the cartridge header (byte `0x143`, bit 7).
- The core keeps an RGB565 copy of both colour palettes, updated on each palette write, so each
  pixel is one table lookup into the `picodvi.Framebuffer`. RGB332 is the fallback.
- The state buffer is about 75 KB (32 KB WRAM and 16 KB VRAM for colour mode). All buffers are
  Python-owned. The native code allocates nothing.

## Known issues

- Not tested on hardware yet.
- Double speed games will likely run slow.
- The MBC3 clock (Pokemon Gold/Silver/Crystal) ticks while playing but is not saved. It starts
  from zero at each boot.
- The fork's STAT mode order and HDMA timing are approximate (scanline renderer). Games that
  depend on exact mid-line timing may glitch.

## License

MIT. Peanut-GB (Mahyar Koshkouei, with parts from SameBoy by Lior Halphon) is MIT, see the
header of `src/peanut_gb_cgb.h`. The CGB support comes from Tomáš Večeřa's GitHub fork of
Peanut-GB, which keeps that MIT header. minigb_apu (Alex Baines, Mahyar Koshkouei) is MIT, see `src/LICENSE.minigb_apu`. No ROMs are included.
