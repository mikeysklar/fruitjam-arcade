# Third-party code and data

Everything in this repository is MIT (see `LICENSE`) except the files below, which keep
their own licenses. Each license text sits next to the files it covers.

**The SCUMM engine is GPL.** `ports/scumm/` is a trimmed copy of the
[ScummVM](https://www.scummvm.org/) 2.2.0 SCUMM engine plus our backend, distributed under
GPL-3.0-or-later (`ports/scumm/LICENSE`; ScummVM's files are "GPL version 2 or later" and keep
their headers, credits in `ports/scumm/AUTHORS` and `COPYRIGHT`). It builds `scumm.mpy`, which
the release puts in `/emus`, so the release files include GPL-licensed code; its source is this
folder. No game data is included or distributed.

| Port | Files | From | License |
|---|---|---|---|
| Game Boy | `ports/gbc/src/peanut_gb_cgb.h` (plus our `peanut_gb_cgb.patch`) | [Peanut-GB](https://github.com/deltabeard/Peanut-GB), Game Boy Color fork by tvecera | MIT |
| Game Boy | `ports/gbc/src/minigb_apu.c`, `minigb_apu.h` | [minigb_apu](https://github.com/baines/MiniGBS) | MIT, `LICENSE.minigb_apu` |
| NES | palette data in `ports/nes/src` | [agnes](https://github.com/kgabis/agnes) | MIT, `LICENSE.agnes` |
| Atari 7800 | `ports/atari7800/src/palette.h` (color values only) | [MAME](https://github.com/mamedev/mame) | BSD-3-Clause, `LICENSE.mame` |
| Atari Lynx | Suzy and Mikey chip code in `ports/lynx/src` (marked as altered) | [Handy](https://handy.sourceforge.net/) by K. Wilkins | zlib, `LICENSE.handy` |
| ColecoVision | 8x8 font in `ports/coleco/src` | [font8x8](https://github.com/dhepper/font8x8) | public domain, `LICENSE.font8x8` |
| MSX | `ports/msx/src/cbios/*.rom` (built into `msx.mpy`) | [C-BIOS](http://cbios.sourceforge.net/) 0.29a | BSD-style, `cbios/LICENSE.txt` |
| SCUMM | `ports/scumm/` (built into `scumm.mpy`), and `CIRCUITPY/emus/emu_scumm.py` | [ScummVM](https://github.com/scummvm/scummvm) 2.2.0, `engines/scumm` | GPL-3.0-or-later, `ports/scumm/LICENSE` |

The libraries in the release's `lib/` folder come from the
[Adafruit CircuitPython Bundle](https://github.com/adafruit/Adafruit_CircuitPython_Bundle) and the
[Community Bundle](https://github.com/adafruit/CircuitPython_Community_Bundle), under their own
(MIT) licenses. The `.uf2` release includes [CircuitPython](https://github.com/adafruit/circuitpython)
(MIT).
