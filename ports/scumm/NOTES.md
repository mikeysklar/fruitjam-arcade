# How fruitjam-scumm works

## Source

- `src/scumm/`: ScummVM 2.2.0 (commit 51bb9305) `engines/scumm`, cut down to
  what v3/v4 PC games use. `unifdef` removed v7/v8, HE, RGB colour, Towns
  dual layer, NES/C64/Apple II, SID and the ARM assembler paths. The
  classes for v0-v2, v6+, HE, NES, C64 and Towns are gone. Version branches
  that are dead at run time but harmless were left in place, so the code
  diffs cleanly against upstream. Changes are marked `fruitjam-scumm:`.
  34k of the engine's 133k lines are kept.
- `src/scumm/fj_detect.cpp`: replaces detection.cpp. It takes the MD5 of
  00.LFL/000.LFL and looks it up in `fj_md5.h`, generated from ScummVM's
  scumm-md5.h by `tools/gen_md5.py`. It also finds the Loom index inside the
  Steam/GOG `Loom.exe`, first at the known offset, then by scanning for a v4
  index block chain of exactly 8307 bytes.
- `src/common`, `src/audio`, `src/graphics`: the ScummVM support code the
  engine uses, copied from 2.2.0 where possible. That includes the full
  mixer and rate converter. OSystem, events, ConfMan, the timer manager
  and save files are small rewrites.
- `src/backend/`: the platform layer (below).
- `src/libc/`: string, ctype and printf functions, setjmp/longjmp, and
  new/delete for the native module, which has no C library.
- `src/modscumm.c`: the MicroPython binding, the only file that sees
  MicroPython headers.

## Execution model

The engine keeps its own blocking main loop (`ScummEngine::go()`), and it
runs on a **coroutine** with a 48 KB stack taken from the arena. On ARM the
switch is a few instructions of assembler that save r4-r11, lr and s16-s31
(`fj_coro.c`). On the M33 the stack limit register (MSPLIM/PSPLIM) is
cleared while the coroutine runs, because its stack sits in PSRAM below
the main stack's limit, and restored afterwards. On the host it is a pthread passed a baton, so the
two sides strictly alternate.

- `OSystem::delayMillis()` ends the frame: the coroutine yields and
  `fj_core_frame()` returns to Python. The clock is virtual. Each
  `fj_core_frame()` advances it by 1/60 s and fires due timers (the Loom
  CD music timer). Game speed therefore follows the launcher's audio
  pacing, not the wall clock.
- **File I/O is a syscall.** The engine fills in a request and yields.
  The host side runs it through Python file methods (`ScummIO` in
  emu_scumm.py) and resumes the engine. MicroPython is never called from
  the coroutine stack, so the VM's stack checks and the GC's stack scan
  never see it.
- Log lines from the coroutine go to a buffer and are printed on the host
  side.
- `error()` on the coroutine parks the engine for good, and
  `fj_core_frame()` then returns False with the message. On the host side
  (inside a mixer callback) it longjmps back out of `fj_core_frame()`.

## Memory

`emu_scumm.py` allocates one 2 MB `bytearray` (the **arena**). Every engine
malloc/new goes to a boundary-tag allocator inside it (`fj_alloc.c`), so
the GC never moves or frees engine memory and nothing in it holds a Python
object. The host harness uses the same allocator, so these figures carry
over.

| | peak arena use, first 50 to 100 s |
|---|---|
| Loom EGA demo | 540 KB |
| Indy3 EGA demo (with save/load) | 560 KB to 750 KB |
| Monkey Island EGA demo | 400 KB |
| Passport | 390 KB |

The engine's resource cache starts expiring at 1.1 MB and frees down to
800 KB (`setHeapThreshold`). ScummVM uses 550/400 KB; the larger cache
means fewer SD reads on room changes. The figures above were measured with
ScummVM's values. The coroutine stack's high-water mark on the host is
5.5 KB (17 KB in an unoptimised ASan build) of the 48 KB.

## Native module packaging (no firmware changes)

mpy_ld only takes text, rodata and bss sections. It refuses initialised
`.data`, refuses PC-relative references into `.bss`, and knows nothing about
C++ constructors. All objects are therefore first **partially linked**
(`ld -r --force-group-allocation --gc-sections`) with `tools/natmod.ld`,
which puts text, rodata, data.rel.ro, data and bss into one `.text`, with
bss as zero bytes and `.init_array` bracketed by two symbols. libgcc is
pulled in at the same step. A native module is loaded into RAM, so data
inside `.text` is writable. Stock `mpy_ld.py` then links the single object:

- 3376 THM_CALL and 313 THM_JUMP24 branches
- 1358 REL32 and 1632 ABS32 relocations (the ABS32 ones are applied at
  load time)
- 447 GOT entries

Thumb function pointers keep bit 0 because every relocation to a function
uses the function's own symbol (checked over all ABS32 relocations).

The result is `scumm.mpy`, about 260 KB. The module has no constructors
today, but `fj_core_init` runs any it gets once the arena exists.

memcpy, memmove and memset come from MicroPython's function table.
Everything else is in `src/libc`.

## Video

The engine draws into an 8-bit 320x200 buffer (the "screen"). Each frame,
the host side converts the rows that changed to the framebuffer through
a 256-entry RGB565 (or RGB332) table. The picture is centred in the
launcher's 320x240 framebuffer with 20-pixel bars. A palette change
redraws everything. The mouse cursor is drawn on top in software, and
the engine's screen shake moves the picture.

Loom for the PC Engine CD is the exception: its art uses more than 256
colours, so ScummVM draws it 16 bits a pixel (`GF_16BIT_COLOR`, upstream
only with `USE_RGB_COLOR`). The engine asks `initGraphics` for RGB565, the
framebuffer's own format, so the screen buffer is 2 bytes a pixel and
dirty rows are copied straight across (RGB332 framebuffers get a
per-pixel conversion). The PC Engine pieces upstream keeps behind
`USE_RGB_COLOR` are restored: `GdiPCEngine` (tile graphics and masks),
`CharsetRendererPCE` (English only; the Japanese release needs the System
Card's kanji font and is left out of detection), `PCEngineCostumeRenderer`
and `Player_PCE`. The game's own font is used, so no BIOS file is needed.

## Audio

The output is 22050 Hz stereo int16, written into the launcher's ring each
frame (367 or 368 samples a frame). Frame pacing comes from
`fj_core_samples()`. ScummVM's mixer and rate converter are used
unchanged, and they run on the host side once per frame.

- EGA games: PC speaker music through Player_V2 (the default), or, for
  the DOS Loom and Indy3, AdLib through Player_AD and ScummVM's MAME OPL2
  emulator (`music = adlib`). The OPL runs at half the output rate, 11025
  Hz, with the "medium quality" tables, and the mixer resamples it. Its
  start-up table build needs sin/pow/log10, which `src/libc/fj_math.c`
  provides. MI1 uses iMuse for AdLib, which is not included, so it gets
  the PC speaker.
- Loom talkie: `Sound::_isLoomSteam` turns on when `CDDA.SOU` is in the
  folder. The CDDA stream (44.1 kHz, 8-bit with a per-block shift) is read
  through the same file syscalls, from the mixer on the host side, and is
  decoded in 512-byte chunks.
- Loom PC Engine: effects come from `Player_PCE` (ScummVM's HuC6280 PSG,
  integer per sample). Music is the CD's audio tracks 3 to 21, which the
  game starts whole (sound IDs 13-32 map to tracks). `tools/pce_tracks.sh`
  converts the ripped tracks with ffmpeg to `TrackNN.wav`: IMA ADPCM,
  22050 Hz mono, 1 KB blocks, 11 KB/s, about 13 MB for all 19 tracks.
  `src/scumm/fj_cdtrack.cpp` decodes one block at a time (bit-exact with
  ffmpeg, `make -f Makefile.host test`), so no resampling is needed and
  the card sees one 1 KB read every 93 ms. Tracks that are missing just
  don't play.

## Input

The adapter turns the arcade pad into a mouse, with an accelerating
pointer, A and B as the buttons, and a few keys. The core turns changes
into ScummVM events. F5 goes to the game's scripts, as on DOS, so the
game's own save/load screen opens. No ScummVM dialogs are built.
