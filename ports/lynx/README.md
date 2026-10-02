# CircuitPython Atari Lynx

Atari Lynx games on an Adafruit Fruit Jam, running under stock CircuitPython. The emulator is a
C port of parts of [Handy](https://handy.sourceforge.net/) (zlib) plus a 65C02 core written for
this repo (MIT), compiled into a native `.mpy` module like the Game Boy port in the parent
directory.

Not yet tested on hardware. See [NOTES.md](NOTES.md) for what has been verified.

## What you need

| part | notes |
|---|---|
| Adafruit Fruit Jam | RP2350 |
| CircuitPython 11.0.0-alpha | the `.mpy` is built for this version |
| HDMI monitor | 160x102 picture at 2x (320x204), centered |
| headphones or speaker | optional |
| USB gamepad | optional, the 3 onboard buttons work too |
| a `.lnx` cart image or `.o` homebrew | not included |
| `lynxboot.img` | optional, the real boot ROM, not included and not needed |

## Setup

Back up your `CIRCUITPY` drive first. `code.py` will be replaced.

```sh
git clone https://github.com/mikeysklar/fruitjam-arcade
cp -r fruitjam-arcade/ports/lynx/CIRCUITPY/* /Volumes/CIRCUITPY/
cp MyGame.lnx /Volumes/CIRCUITPY/
```

Optional, for sound and gamepad:

```sh
circup install adafruit_fruitjam relic_usb_host_gamepad
```

The board resets and the game boots.

- The first `.lnx`, `.lyx` or `.o` file found on the SD card (`/sd`), then on `CIRCUITPY`, is
  loaded.
- `.lnx` carts boot through a built in replacement for the boot ROM. If `lynxboot.img` is on
  `/sd` or `CIRCUITPY` it is used instead. `.o` files (BS93 homebrew) load straight into RAM.
  Serial says which one happened.
- Serial console prints fps every 5 seconds.

A free game to try: T_TRIS.O from the
[Demo Collection](https://www.zophar.net/pdroms/lynx/demo-collection.html) on zophar.net. Our own
test cart is `src/testcarts/lynxtest.lnx`.

## Controls

| Fruit Jam | Lynx |
|---|---|
| Button 1 | Left |
| Button 2 | Right |
| Button 3 | A |
| Button 1 + 2 | Pause |
| Button 1 + 3 | Down |
| Button 2 + 3 | B |
| USB gamepad | D-pad, A, B, X = Option 1, Y = Option 2, Start = Pause, Select = Option 1 |

## Speed

Estimated, not measured on the board yet. Instruction counts come from running the Cortex-M
build under Unicorn, scaled by the cycles per instruction the Game Boy port shows on real
hardware (see NOTES.md).

```
Lynx core on a Fruit Jam @ 150 MHz (estimate)
+-----------------------------------+-------------+--------+--------+
| test                              | instr/frame | fps    | needs  |
+-----------------------------------+-------------+--------+--------+
| lynxtest (sprites, audio)         | 1.13 M      | ~83    | 60     |
| T_TRIS                            | 1.07 M      | ~87    | 75     |
| The Dungeon                       | 0.77 M      | ~121   | 60     |
| Catkanoid title                   | 1.24 M      | ~75    | 60     |
| DEMO0006 (heavy sprites), mean    | 1.30 M      | ~72    | 60     |
| DEMO0006, worst frame             | 2.48 M      | ~38    | 60     |
+-----------------------------------+-------------+--------+--------+
```

## Build the `.mpy` yourself

Same toolchain as the parent project:

```sh
cd fruitjam-arcade/ports/lynx/src
make MPY_DIR=../../../../circuitpython ARCH=armv7emsp
cp lynx.mpy ../CIRCUITPY/lib/
```

Host tests (any C compiler, downloads free test programs):

```sh
cd fruitjam-arcade/ports/lynx/src
make -f Makefile.host
./get_testroms.sh
./run_tests.sh
```

## How it works

- `src/lynxcore.h` is the emulator: Mikey (timers, interrupts, display DMA, palette, 4 audio
  channels, a minimal serial port), Suzy (sprite engine, collision, math unit, joystick) and
  the cart shifter. `src/cpu65c02.h` is the CPU. `src/lynx.c` wraps it as a native module.
- The boot ROM is replaced by a small HLE: it decrypts the cart's loader with the published
  RSA key and jumps to it, the same way Handy does without a boot ROM.
- Everything runs on one 16 MHz tick clock like Handy. Timers, display lines and audio are
  only updated when their next event is due, not every instruction.
- The emulator draws each line at 2x straight into a `picodvi.Framebuffer`. No displayio
  refresh.
- Sound goes into a looping stereo `audiocore.RawSample` on the Fruit Jam's TLV320 DAC.
  Frames are paced by the samples each frame made, so games at 50, 60 or 75 Hz run at their
  own speed.
- All buffers are Python-owned. The native code allocates nothing.

## Known issues

- Catkanoid hangs when a game starts (the title screen works). Handy without a boot ROM hangs
  the same way, so this may be the HLE boot rather than the core.
- No EEPROM saves. Games that look for one see a blank chip.
- Games made for a sideways Lynx are shown unrotated. The rotation flag is printed on serial.
- No ComLynx (multiplayer cable). The serial port only loops back to itself.
- Heavy sprite scenes may drop below full speed (see Speed).

## License

The core (`src/lynxcore.h`) is an altered version of parts of Handy by K. Wilkins, zlib
license, see `src/LICENSE.handy`. Everything else is MIT. No ROMs and no boot ROM are included.
