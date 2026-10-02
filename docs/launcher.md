<!--
SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
SPDX-License-Identifier: MIT
-->

# Fruit Jam Arcade: how the launcher works

One Fruit Jam, one microSD card, thirteen consoles plus CHIP-8. The SD card is the cartridge,
the same way it is in John Park's
[Adafruit Arcade Machines](https://github.com/adafruit/Adafruit_Arcade_Machines),
so it plays the same: the same arcade pad pins, the same error colours, the same
`.sav` and `.fruitjam.cfg` files next to the ROM. The difference is that it runs on
stock CircuitPython and switches consoles without reflashing.

| Console | ROM files | Emulator |
|---|---|---|
| NES | `.nes` | `nes.mpy` |
| Game Boy, Game Boy Color | `.gb` `.gbc` | `peanutgbc.mpy` |
| Atari 2600 | `.a26` `.bin` | `atari2600.mpy` |
| Atari 7800 | `.a78` | `atari7800.mpy` |
| Atari Lynx | `.lnx` `.lyx` `.o` | `lynx.mpy` |
| Master System, Game Gear, SG-1000 | `.sms` `.gg` `.sg` | `sms.mpy` |
| ColecoVision | `.col` | `coleco.mpy` |
| CHIP-8, SUPER-CHIP | `.ch8` `.sc8` | `chip8.mpy` |
| MSX (MSX1 cartridges) | `.rom` `.mx1` `.mx2` | `msx.mpy` |
| Watara Supervision | `.sv` | `supervision.mpy` |

No ROMs are included.

Supervision dumps often end in `.bin`, which here means an Atari 2600 cart, so
rename them to `.sv`. MSX uses the open C-BIOS built into `msx.mpy` (BSD-style
licence, see `msx/src/cbios/`); it has no MSX-BASIC. Put your own 32 KB BIOS in
`/cart` as `msx.rom` to use it instead.

## Setup

See the [README](../README.md#install): one file installs the launcher, the emulators and
the libraries on CIRCUITPY. The microSD card only holds your games, in `/cart`.

## Playing

- **One ROM** in `/cart`: it boots straight into its console.
- **More than one**: a menu lists the consoles with how many games each has. Pick
  one to see its games. If every ROM is for the same console, the menu goes straight
  to the games.
- **Hold Start + Coin for a second** to come back to the menu from any game, or from
  an error screen. The menu opens on the last game you played.

| Button | Console list | Game list |
|---|---|---|
| Up / Down | Choose | Choose (wraps around) |
| Left / Right | | A page (15 games) at a time |
| A or Start | Show its games | Play |
| B | | Back to the console list |

Master System, Game Gear and SG-1000 share one emulator, so they are listed
together as Sega.

### Arcade pad (AAM header pins)

| Pin | Button | NES, Game Boy | Sega | Atari 2600 | Atari 7800 | Lynx | Coleco |
|---|---|---|---|---|---|---|---|
| A3 A4 D8 D9 | D-pad | D-pad | D-pad | Joystick | Joystick | D-pad | Joystick |
| D10 | A | A | 1 | Fire | Button 1 | A | Left fire |
| A2 | B | B | 2 | Fire | Button 2 | B | Right fire |
| A1 | C | B | 2 | Right difficulty | Button 2 | Option 1 | Shift (#) |
| D6 | Start | Start | Pause / Start | Reset | Reset | Pause | Keypad 1 |
| D7 | Start 2 | Start | Pause / Start | Left difficulty | Pause | Option 2 | Keypad 2 |
| A5 | Coin | Select | | Select | Select | | Keypad * |

CHIP-8 maps the pad to its hex keypad (WASD-style defaults, per-game fixes, and a
`keys =` line in the game's `.fruitjam.cfg`).
Each adapter's docstring (`CIRCUITPY/emus/emu_*.py`) has its exact table, including the
console switches and keypads this summary leaves out. A USB gamepad works at the
same time as the pad.

### Onboard buttons

As in AAM, these are never game buttons: Button 1 stretch, Button 2 rotate,
Button 3 palette. Each console does what its emulator supports and ignores the rest
(CHIP-8 cycles colour pairs on Button 3, for example). Choices are saved to
`/cart/<rom name>.fruitjam.cfg` 3 seconds after the last change.

### Error screens

| Colour | Meaning |
|---|---|
| Red | No SD card, or it would not mount |
| Yellow | No ROM in `/cart` on the card, or no `/emus` folder on CIRCUITPY (reinstall) |
| Magenta | The ROM was rejected: bad header, unsupported mapper, or the emulator failed to load |

The reason is printed on the serial console every 2 seconds.

## Saves

Games with battery RAM save the way the cartridge did: once the game writes its save
RAM and leaves it alone for about a second, it is written to `/cart/<rom name>.sav`,
a standard raw `.sav` that PC emulators also read. There are no save states and no
save button.

## How it works

- `CIRCUITPY/code.py` is the launcher. It puts `/emus` on `sys.path`, lists
  `/sd/cart`, and either runs the one ROM or shows the menu.
- Picking a game stores its path in `microcontroller.nvm` and reloads CircuitPython,
  so every game starts with a clean heap. Start + Coin stores "menu" and reloads.
- `emus/fjarcade.py` is shared: pad, display, audio, saves, settings, the run loop.
  Frames are paced by the audio the emulator has produced, which covers fixed-rate
  cores and variable ones (Atari 2600, Lynx at 75 Hz) the same way.
- `emus/emu_<console>.py` adapts one native module to that run loop.

## Testing without a board

`tools/sim/` runs the real launcher and the real emulator cores in CircuitPython's
unix port, with stand-in modules for the hardware, scripted button presses, and a
framebuffer dump at the end. Build the unix port and x64 versions of the `.mpy`
modules (`make ARCH=x64` in each port's `src/`), then:

```sh
tools/sim/test_all.sh <rom dir> <x64 mpy dir> <out dir>     # every ROM, one at a time
tools/sim/run_sim.sh <root> <script>                          # menu and Start+Coin flows
```

## License

MIT, like the rest of this repo. The emulator cores' licenses are in each port's
directory (Lynx is derived from Handy, zlib).
