# Status

Updated 2026-10-03.

## Done

- The engine subset compiles warning-clean enough, both for the host
  (clang) and as an ARM native module (arm-none-eabi-gcc 14.3,
  armv7emsp).
- The host harness runs the five free demos and renders correctly:
  - Loom EGA long and short demos
  - Monkey Island EGA demo
  - Indy3 EGA demo
  - Passport
  The output covers text, palettes, the cursor and the inventory. PC
  speaker music plays in the Indy3 and Loom demos.
- Clicking and walking work in the interactive Indy3 demo. A save to
  slot 1 and a load of it work.
- The demos run clean under AddressSanitizer. One upstream out-of-bounds
  table read in Player_V2 is now clamped.
- The allocator and printf have a host unit test (`make -f Makefile.host
  test`).
- `scumm.mpy` builds with stock `mpy_ld.py` (about 260 KB).
- Loom.exe index scan was tested with a synthetic exe that has the index
  at an unknown offset.
- The ARM code runs correctly under emulation (Unicorn, Cortex-M33 model)
  and renders the same frames as the host:
  - `tools/armemu/run.py` runs a bare-metal link of the same objects.
  - `tools/armemu/mpyrun.py` links `prelink.o` with CircuitPython's own
    `mpy_ld.py`, applies the .mpy relocations as MicroPython does at
    import, and runs that image.
  - Both exercise the coroutine switch (including MSPLIM), setjmp, the mini
    libc and the allocator.
  - The engine stack peaks at 4.8 KB on ARM.
- Rough speed from the emulator's instruction counts: 50-110k instructions
  per 1/60 s frame on average, 2-5M on room changes.
- `emu_scumm.py` is written to the launcher's adapter contract.
- AdLib music (opt-in, `music = adlib`) for the DOS Loom EGA and Indy3,
  through ScummVM's Player_AD and MAME OPL2. It runs at 11025 Hz with
  medium-quality tables, costs about 150k instructions a frame, and adds
  20 KB to the module. Building the OPL tables at start takes about 60M
  instructions (soft double, roughly half a second).

## On the board (Fruit Jam, stock CP 11.0.0-alpha.1)

- `import scumm` takes 210 ms. `Emu()` takes 330 ms (detection, MD5, the
  2 MB arena), leaving 5.8 MB of GC heap free.
- Loom demo frames match the host's exactly (screenshot at frame 1200).
- Started from the launcher menu (the board's fjarcade.py with the
  two-line change), it runs at a steady 60 fps with the DAC on, 2.5-5 ms
  emu+input per frame.

| demo (3000 frames, from REPL, no pacing) | avg ms/frame | worst after start | frames over 16.7 ms |
|---|---|---|---|
| Loom EGA long demo | 2.9 | 129 | 109 |
| Indy3 EGA demo | 1.7 | 83 | 23 |
| Monkey Island EGA demo | 1.4 | 108 | 44 |
| Passport | 1.6 | 116 | 5 |

- The slow frames are room changes. The launcher re-anchors its audio
  pacing after them.
- Start-up frame (first room load): 220 ms from CIRCUITPY, 340 ms from SD.
- AdLib (`music = adlib`, Loom demo through the launcher): 5.6-11 ms a
  frame, mostly 60 fps, a few 5 s windows at 50 fps.
- Quick save and load work. A save into the game folder on SD takes
  about 0.24 s. With the game on CIRCUITPY, which is read-only to code
  while USB is mounted, the save falls back to `/saves/<folder>/` and
  takes about 1.3 s (internal flash).
- Arena peak 730 KB, engine stack 5 KB.

## Mikey's Loom (DOS EGA floppy) on the board

- 00.LFL md5 37f56ceb13e401a7ac7d9e6b37fecaf7, ScummVM's "loom EGA,
  English, DOS". It is copied to /sd/scumm/loom (78 files, every one
  sha256-checked) with the marker /sd/cart/loom.scumm.
- Copy protection: the Book of Patterns screen is bypassed by default
  (ScummVM's room 69 script 201 -> 205 patch). With `copy_protection =
  true` the draft question appears instead, so the bypass is what skips it.
- From the launcher, in the first room: a steady 60 fps, 2.1 ms
  emu+input per frame.
- Scripted run from the REPL (title, Standard, intro, first room, a
  walk), 6000 frames:
  - engine average 2.2 ms per frame
  - worst 222 ms, the load after choosing the proficiency level
  - 106 frames over 16.7 ms, all scene changes
  - arena peak 650 KB, stack 4.7 KB
- Game tick rate: in play Loom sets VAR_TIMER_NEXT = 6, so scripts and
  screen updates run at 10 Hz (6 jiffies of 60 Hz). Parts of the title
  and intro run at 1-2 jiffies. The display is paced at 60 fps.

## Loom PC Engine CD (English), host only

- Detection: `00.LFL` md5 0aa050f4ad79402fbe9c4f78fb8ac494 ("loom
  PC-Engine EN"). The Japanese release is left out (needs a kanji font).
- Proficiency menu, Lucasfilm logo, title, intro and the first room
  render, Bobbin walks and is masked by the rocks. Pacing matches DOS EGA
  frame for frame. Same frames in the ARM build under armemu.
- CD music plays from `TrackNN.wav` (IMA ADPCM, 22050 Hz mono, 13 MB for
  tracks 3-21). Effects through Player_PCE.
- Cost against DOS EGA, same 3000 frames: arena peak 1.07 MB (EGA 0.65
  MB), armemu ~105k instructions a frame on average (EGA 82k) and 4.9M
  worst at a room load (EGA 2.1M). scumm.mpy 292 KB (was 282 KB).
- Not checked: the distaff (its tiles load at start, but Bobbin doesn't
  have it yet in the first room), later rooms, saves.

On the board (2026-10-03), files in `/sd/scumm/loompce/` (77 LFL + 19
WAV, sha256-checked), marker `/sd/cart/loompce.scumm`:

- Launcher: starts, 60.0 fps at the proficiency menu, 2.2-2.4 ms
  emu+input. DOS EGA Loom with the same scumm.mpy: 60.0 fps, 2.0 ms.
- Scripted paced run (`tools/board_paced.py`, Standard picked at frame
  200, 9000 frames with real audio):
  - first room: 3.4 ms average per frame, 11-13 ms worst
  - room changes and scene loads: 90-148 ms single frames; first frame
    344 ms
  - the intro's pan up the mountains (about frames 1900-1950): 53 ms a
    frame for about a second (DOS EGA: 33 ms there)
  - arena peak 1,067,048 bytes, stack 4.8 KB, gc.mem_free 5.4 MB
- CD music plays (non-zero samples in the DAC ring during the title).
  The only gaps in steady play are the Python GC: `gc.collect()` takes
  ~130 ms in the launcher, outside the engine, every 8-20 s depending
  on how much the Python loop allocates. DOS EGA shows the same stalls.

## Not verified yet

- Loom VGA talkie and CDDA.SOU audio (no files).

## Missing

- USB mouse and keyboard. Only the arcade pad and the USB gamepad
  (through the launcher) are supported.
- Typing save game names. On the game's save screen this needs a
  keyboard. A save/load slot API exists (`scumm.save(n)`,
  `scumm.load(n)`) but is not bound to a button yet.
- Copy-protection screens (Loom EGA has one; the CD version does not).
- Non-English Loom CD index MD5s beyond those in ScummVM 2.2.0.
