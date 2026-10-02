# CircuitPython CHIP-8

CHIP-8 and SUPER-CHIP programs on an Adafruit Fruit Jam, running under stock CircuitPython. The
interpreter is written for this port in C and compiled into a native `.mpy` module.

## What you need

| part | notes |
|---|---|
| Adafruit Fruit Jam | RP2350 |
| CircuitPython 11.0.0-alpha | the `.mpy` is built for this version |
| HDMI monitor | 64x32 shows at 5x, 128x64 at 2x |
| headphones or speaker | optional, for the beeper |
| USB gamepad | optional, the 3 onboard buttons work too |
| a `.ch8` or `.sc8` program | not included |

## Setup

Back up your `CIRCUITPY` drive first. `code.py` will be replaced.

```sh
git clone https://github.com/mikeysklar/fruitjam-arcade
cp -r fruitjam-arcade/ports/chip8/CIRCUITPY/* /Volumes/CIRCUITPY/
cp Pong.ch8 /Volumes/CIRCUITPY/
```

Optional, for sound and gamepad:

```sh
circup install adafruit_fruitjam relic_usb_host_gamepad
```

The board resets and the program boots.

- The first `.ch8` or `.sc8` file found on the SD card (`/sd`), then on `CIRCUITPY`, is loaded.
- `.ch8` runs with classic CHIP-8 quirks at 11 instructions per frame.
- `.sc8` runs with SUPER-CHIP (modern) quirks at 30 instructions per frame.
- Serial console prints fps every 5 seconds.

## Controls

CHIP-8 has a 16-key hex keypad. Buttons and the gamepad produce logical controls, and `KEYMAP`
in `code.py` turns them into hex keys. The defaults follow the keys most games use (on a PC
keyboard, W A S D E Q map to 5 7 8 9 6 4).

| Fruit Jam | gamepad | control | default hex key |
|---|---|---|---|
| Button 1 + 2 | D-pad / stick up | up | 5 |
| Button 1 + 3 | D-pad / stick down | down | 8 |
| Button 1 | D-pad / stick left | left | 7 |
| Button 2 | D-pad / stick right | right | 9 |
| Button 3 | A | a | 6 |
| Button 2 + 3 | B | b | 4 |
| | X | x | 1 |
| | Y | y | 2 |
| | L1 | l1 | A |
| | R1 | r1 | B |
| | Select | select | E |
| | Start | start | F |

Games disagree on keys. Add an entry to `ROMS` in `code.py`, keyed by the file name without
extension, to change keys, quirks or speed for one program:

```python
ROMS = {
    "pong": {"keys": {"up": 0x1, "down": 0x4}},
    "mygame": {"keys": {"a": 0x5}, "quirks": chip8.QUIRKS_SCHIP, "ipf": 20},
}
```

`code.py` ships with entries for a few classic public domain games (Pong, Tetris, Brix,
Invaders, Tank, Blinky).

## Quirks

CHIP-8 interpreters differ in small ways that break some games. Each is a bit you can combine:

| constant | on | `QUIRKS_CHIP8` | `QUIRKS_SCHIP` |
|---|---|---|---|
| `QUIRK_VF_RESET` | `8XY1/2/3` clear VF | yes | no |
| `QUIRK_MEMORY` | `FX55/FX65` advance I | yes | no |
| `QUIRK_DISPLAY_WAIT` | a sprite draw ends the frame | yes | no |
| `QUIRK_CLIPPING` | sprites clip at the edges | yes | yes |
| `QUIRK_SHIFTING` | `8XY6/8XYE` shift VX, ignore VY | no | yes |
| `QUIRK_JUMPING` | `BXNN` jumps to XNN + VX | no | yes |

If a game draws garbage or runs too fast or slow, try the other preset or a different `ipf`.

## Build the `.mpy` yourself

Same toolchain as the Game Boy build in the root README, then:

```sh
cd fruitjam-arcade/ports/chip8/src
make MPY_DIR=../../../../circuitpython ARCH=armv7emsp
cp chip8.mpy ../CIRCUITPY/lib/
```

Host test (downloads the GPL test ROMs into the gitignored `src/testroms/`, never commit them):

```sh
cd fruitjam-arcade/ports/chip8/src
mkdir -p testroms
for f in 1-chip8-logo 2-ibm-logo 3-corax+ 4-flags 5-quirks 6-keypad 7-beep 8-scrolling; do
  curl -sfL -o "testroms/$f.ch8" "https://github.com/Timendus/chip8-test-suite/raw/main/bin/$f.ch8"
done
make host_test && ./host_test --suite testroms
```

## How it works

- `src/chip8.c` is the whole interpreter plus the CircuitPython bindings.
- The screen is kept at 128x64. A lores pixel is a 2x2 block, so mode switches and scrolls
  need no conversion.
- At the end of each 60 Hz frame, if the screen changed, it is drawn straight into a
  `picodvi.Framebuffer` at the largest integer scale that fits. No displayio refresh.
- The beeper is a square wave into a looping `audiocore.RawSample` on the TLV320 DAC.
- All buffers are Python-owned. The native code allocates nothing.

## Known issues

- Not yet run on hardware. The host test passes, see `NOTES.md`.
- XO-CHIP is not supported.
- `0NNN` machine code calls stop the program with an error.
- CHIP-8 games flicker, as they did on the real machine. There is no flicker reduction.

## License

MIT. The interpreter is original code for this project. No programs are included.
