# Third-party code and data

Everything in this repository is MIT (see `LICENSE`) except the files below, which keep
their own permissive licenses. Each license text sits next to the files it covers.

| Port | Files | From | License |
|---|---|---|---|
| Game Boy | `ports/gbc/src/peanut_gb_cgb.h` (plus our `peanut_gb_cgb.patch`) | [Peanut-GB](https://github.com/deltabeard/Peanut-GB), Game Boy Color fork by tvecera | MIT |
| Game Boy | `ports/gbc/src/minigb_apu.c`, `minigb_apu.h` | [minigb_apu](https://github.com/baines/MiniGBS) | MIT, `LICENSE.minigb_apu` |
| NES | palette data in `ports/nes/src` | [agnes](https://github.com/kgabis/agnes) | MIT, `LICENSE.agnes` |
| Atari 7800 | `ports/atari7800/src/palette.h` (color values only) | [MAME](https://github.com/mamedev/mame) | BSD-3-Clause, `LICENSE.mame` |
| Atari Lynx | Suzy and Mikey chip code in `ports/lynx/src` (marked as altered) | [Handy](https://handy.sourceforge.net/) by K. Wilkins | zlib, `LICENSE.handy` |
| ColecoVision | 8x8 font in `ports/coleco/src` | [font8x8](https://github.com/dhepper/font8x8) | public domain, `LICENSE.font8x8` |
| MSX | `ports/msx/src/cbios/*.rom` (built into `msx.mpy`) | [C-BIOS](http://cbios.sourceforge.net/) 0.29a | BSD-style, `cbios/LICENSE.txt` |

The libraries in the release's `lib/` folder come from the
[Adafruit CircuitPython Bundle](https://github.com/adafruit/Adafruit_CircuitPython_Bundle) and the
[Community Bundle](https://github.com/adafruit/CircuitPython_Community_Bundle), under their own
(MIT) licenses. The `.uf2` release includes [CircuitPython](https://github.com/adafruit/circuitpython)
(MIT).
