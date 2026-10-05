# SCUMM engine (LucasArts adventures)

LucasArts SCUMM v3/v4 adventures on the Fruit Jam under CircuitPython, as a native module
(`scumm.mpy`) for the Fruit Jam Arcade launcher. The engine is a trimmed copy of ScummVM
2.2.0's SCUMM engine. **This folder is GPL-3.0-or-later** (`LICENSE`), unlike the rest of the
repo. ScummVM's files are "GPL version 2 or later", which allows that; their headers are kept,
and `AUTHORS` and `COPYRIGHT` come from ScummVM.

| Game | SCUMM | On the Fruit Jam (stock 150 MHz) |
|---|---|---|
| Loom, DOS EGA (floppy) | v3 | 60 fps, about 2 ms per frame, PC speaker or AdLib music |
| Loom, TurboGrafx-16 / PC Engine CD (English) | v3 | 60 fps, 2-3.5 ms per frame, CD music and PSG effects |
| Loom EGA, Monkey Island 1 EGA, Indy3 EGA, Passport demos | v3/v4 | run (1.4-2.9 ms per frame) |
| Loom VGA talkie (GOG/Steam/DOS CD) | v4 | detection and CD audio written, not tried with real files |

No game data is in this repo, and none may be added. Use your own copy. `./get_testdata.sh`
fetches ScummVM's free demos into `testdata/` (gitignored) for testing.

## Adding a game

The launcher already routes `.scumm` files to this engine. Each game is a folder on the SD
card plus a marker file in `/sd/cart`:

1. Copy the game files into a folder, for example `/sd/scumm/loom/`.
2. Make a text file in `/sd/cart` whose name is what the menu should show, for example
   `/sd/cart/Loom (DOS EGA).scumm`, holding the folder path (`/sd/scumm/loom`). An empty marker
   means the folder next to it with the marker's name.

### Loom, DOS EGA

Copy `00.LFL` to `99.LFL`. The Book of Patterns copy-protection screen is skipped, as in
ScummVM.

### Loom, TurboGrafx-16 / PC Engine CD (English)

Copy `00.LFL` to `99.LFL`, and convert the CD audio tracks (any format ffmpeg reads) into the
same folder on your computer before copying:

```sh
tools/pce_tracks.sh /path/to/rip /path/to/loom-pce    # Track03.wav .. Track21.wav, ~13 MB
```

Without the `.wav` files the game runs silent apart from sound effects. The Japanese release
is not supported (it needs the console's kanji font).

### Loom from GOG or Steam (untested)

Copy `Loom.exe` (the 000.LFL index is read out of it) or a plain `000.LFL`, `901.LFL` to
`904.LFL`, every `DISK??.LEC`, and `CDDA.SOU` (music and voices, about 280 MB).

## Controls

| input | does |
|---|---|
| D-pad | move the pointer, faster the longer it is held |
| A | left click |
| B | right click |
| C | Escape (skip a cutscene) |
| Start | space (pause) |
| Start 2 | "." (skip the current line of dialogue) |
| Coin | F5, the game's own save/load screen |
| Start + Coin, 1 s | back to the menu |
| Button 1 | F5 |
| Button 2 | quick save (slot 1, listed as "Fruit Jam" in the game's load screen) |
| Button 3 | quick load (slot 1) |

The game's own save screen wants a typed name, so without a keyboard use
quick save. Its load screen works with the pad.

Per-game settings in `/sd/cart/<name>.fruitjam.cfg`: `music = pcspk | adlib | none`
(AdLib is for the DOS EGA Loom and Indy3 and costs about 9M instructions a
second; the Loom talkie uses its CD audio whatever this says),
`subtitles = true | false`, `talkspeed = 0..255`, `pointer_speed = 0..2`,
`autoload = N` (start from save slot N),
plus the launcher's `sound`, `volume` and `audio_output`.

Saves are the game's own (`loom.s01`, ...), written in the game folder, or
in `/saves/<folder>/` when the card is read-only.

## Build

Needs arm-none-eabi-gcc (14.x tested), a CircuitPython checkout for
`py/dynruntime.mk` and `tools/mpy_ld.py`, and pyelftools.

```sh
make MPY_DIR=/path/to/circuitpython          # scumm.mpy (ARCH=armv7emsp)
make -f Makefile.host                        # scumm_host, the desktop harness
make -f Makefile.host test                   # allocator, printf, math, CDDA and CD track decoder tests
./get_testdata.sh
./scumm_host testdata/loom-dos-long-demo-en -n 1200 -s 300,1200 -o /tmp
```

`scumm_host GAMEDIR` runs a game headless through the same core API as the
board: `-n` frames, `-s` screenshot frames (PNG), `-e N` a shot every N
frames, `-c F:X,Y[:r]` click, `-k F:KEY` key, `-S F:SLOT` / `-L F:SLOT`
save and load, `-w out.wav` audio, `-a KB` arena size.

`tools/armemu/` runs the ARM build under the Unicorn emulator, as a
bare-metal ELF (`run.py`) or as the relocated scumm.mpy image
(`mpyrun.py`). Install unicorn and pyelftools for it.

`tools/pyshim/` runs `emu_scumm.py` on CPython over the host core
(`make -f Makefile.host adapter-test`).

`tools/fb2png.py` turns a board screenshot (`FJ_SCREENSHOT` in
settings.toml) into a PNG.

See NOTES.md for how it works and STATUS.md for where it stands.
