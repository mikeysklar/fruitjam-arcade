# Fruit Jam Arcade

Retro consoles on an [Adafruit Fruit Jam](https://www.adafruit.com/product/6200), running on
CircuitPython. Put games on a microSD card, pick one from the menu, play with John Park's
arcade pad or a USB gamepad.

| Console | ROM files | fps on the Fruit Jam |
|---|---|---|
| NES | `.nes` | 60.1 |
| Game Boy, Game Boy Color | `.gb` `.gbc` | 59.7 |
| Atari 2600 | `.a26` `.bin` | 60.0 |
| Atari 7800 | `.a78` | 60.0 |
| Atari Lynx | `.lnx` `.lyx` `.o` | 75.0 |
| Master System, Game Gear, SG-1000 | `.sms` `.gg` `.sg` | 60.0 |
| ColecoVision | `.col` | 60.0 |
| MSX | `.rom` `.mx1` `.mx2` | 59.9 |
| Watara Supervision | `.sv` | 61.0 |
| CHIP-8, SUPER-CHIP | `.ch8` `.sc8` | 60.1 |

No games are included.

## Install

Download from the [latest release](https://github.com/mikeysklar/fruitjam-arcade/releases/latest).

**Already running CircuitPython 11?** Copy `fruitjam-arcade-code.py` to the CIRCUITPY drive
as `code.py`. The drive disappears while it unpacks, then the board restarts.

**Anything else** (new board, older CircuitPython): hold BOOT, press RESET, and copy
`fruitjam-arcade.uf2` to the `RP2350` drive. This installs CircuitPython 11.0.0-alpha.1 and
**replaces everything on CIRCUITPY**.

## Games

1. Format a microSD card as FAT32 or exFAT.
2. Make a folder called `cart` and copy your games into it.
3. Put the card in the Fruit Jam and power up.

Supervision games often end in `.bin`; rename them to `.sv` (`.bin` means Atari 2600 here).

Optional: add this line to `settings.toml` on CIRCUITPY so games can save while the board is
plugged into a computer.

```toml
CIRCUITPY_SDCARD_USB = false
```

## Play

| Button | Menu | In a game |
|---|---|---|
| D-pad | choose (Left/Right a page at a time) | D-pad / joystick |
| A or Start | pick | A / fire / Start |
| B | back to the console list | B / second fire |
| Start + Coin, hold 1 s | | back to the menu |

Each console's exact button map is at the top of its adapter, `CIRCUITPY/emus/emu_<console>.py`.
More detail: [docs/launcher.md](docs/launcher.md).

## Build from source

```sh
pip install 'folder2uf2>=0.1.2' circup
tools/build.sh                              # release files in dist/
MPY_DIR=../circuitpython tools/build.sh     # also rebuild the emulators
```

The emulator sources are in `ports/<console>/`.

## License

MIT. Some emulators include code or data under other permissive licenses; see
[THIRD_PARTY.md](THIRD_PARTY.md).
