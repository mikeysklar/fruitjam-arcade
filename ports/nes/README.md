# CircuitPython NES

NES games on an Adafruit Fruit Jam, running under stock CircuitPython. The emulator is a small
NES core written for this project (MIT), compiled into a native `.mpy` module like the Game Boy
port in the parent directory.

Not yet tested on hardware. See [NOTES.md](NOTES.md) for what has been verified.

## What you need

| part | notes |
|---|---|
| Adafruit Fruit Jam | RP2350 |
| CircuitPython 11.0.0-alpha | the `.mpy` is built for this version |
| HDMI monitor | 256x240 picture, doubled to 640x480 by the HDMI output |
| headphones or speaker | optional |
| USB gamepad | optional, the 3 onboard buttons work too |
| a `.nes` ROM | iNES format, not included |

## Setup

Back up your `CIRCUITPY` drive first. `code.py` will be replaced.

```sh
git clone https://github.com/mikeysklar/fruitjam-arcade
cp -r fruitjam-arcade/ports/nes/CIRCUITPY/* /Volumes/CIRCUITPY/
cp MyGame.nes /Volumes/CIRCUITPY/
```

Optional, for sound and gamepad:

```sh
circup install adafruit_fruitjam relic_usb_host_gamepad
```

The board resets and the game boots.

- The first `.nes` file found on the SD card (`/sd`), then on `CIRCUITPY`, is loaded.
- Battery saves go to `MyGame.sav` next to the ROM, every 30 s when changed and on exit.
  Saving to `CIRCUITPY` needs it writable from code (a `boot.py` remount), `/sd` just works.
- Serial console prints fps every 5 seconds.

## Supported cartridges

| mapper | name | example games |
|---|---|---|
| 0 | NROM | Super Mario Bros., Donkey Kong |
| 1 | MMC1 (incl. 512 KB SUROM) | Zelda, Metroid, Mega Man 2 |
| 2 | UxROM | Mega Man, Castlevania, Contra |
| 3 | CNROM | Solomon's Key, Arkanoid |
| 4 | MMC3 | Super Mario Bros. 3, Kirby's Adventure |
| 7 | AxROM | Battletoads, Wizards & Warriors |
| 66 | GxROM | Super Mario Bros. + Duck Hunt |

NTSC only. Other mappers stop at boot with "mapper N not supported".

## Controls

| Fruit Jam | NES |
|---|---|
| Button 1 | Left |
| Button 2 | Right |
| Button 3 | A |
| Button 1 + 2 | Start |
| Button 1 + 3 | Down |
| USB gamepad | D-pad, A, B, Start, Select |

## Speed

Estimated, not measured on the board yet. Instruction counts come from running the Cortex-M
build under Unicorn, scaled by the cycles per instruction the Game Boy port shows on real
hardware (see NOTES.md).

```
NES core on a Fruit Jam @ 150 MHz (estimate)
+-----------------------------------+-------------+--------+
| test                              | instr/frame | fps    |
+-----------------------------------+-------------+--------+
| Peanut-GB Tetris (measured 62.6)  | 1.49 M      | 62.6   |
| NES Blade Buster, draw RGB565     | 1.38 M      | ~67    |
| NES Blade Buster, worst frame     | 1.44 M      | ~65    |
+-----------------------------------+-------------+--------+
NES needs 60.1 fps for full speed.
```

## Build the `.mpy` yourself

Same toolchain as the parent project:

```sh
cd fruitjam-arcade/ports/nes/src
make MPY_DIR=../../../../circuitpython ARCH=armv7emsp
cp nes.mpy ../CIRCUITPY/lib/
```

Host tests (any C compiler, downloads free test ROMs):

```sh
cd fruitjam-arcade/ports/nes/src
./get_testroms.sh
./run_tests.sh
```

## How it works

- `src/nescore.h` is the emulator: 6502 CPU, scanline PPU, APU, mappers. `src/nes.c` wraps it
  as a CircuitPython native module.
- The PPU draws each scanline in one go, then the CPU runs through that line. This is much
  cheaper than a dot-by-dot PPU and works for most games (see Known issues).
- The emulator draws each line straight into a `picodvi.Framebuffer`. No displayio refresh.
- Sound (2 pulse, triangle, noise, DMC) goes into a looping `audiocore.RawSample` on the Fruit
  Jam's TLV320 DAC.
- All buffers are Python-owned. The native code allocates nothing.

## Known issues

- Games that change scroll or palette in the middle of a scanline show the change one line
  late. Status bars split with sprite 0 or MMC3 IRQs work.
- MMC3 IRQ is clocked once per line, so games relying on odd PPU address patterns may glitch.
- No PAL games, no Famicom Disk System, no second controller.
- The picture is 1x (256x240) with black bars; 2x does not fit in SRAM.

## License

MIT. The core is original code for this project. The NES color palette values are from agnes
by Krzysztof Gabis (MIT, `src/LICENSE.agnes`). No ROMs are included.
