# Launcher notes

Development log. Until the first release the emulators lived on the card in `/sd/emus`;
they now live on CIRCUITPY in `/emus`, and `tools/build.sh` replaces `make_sd.sh`.

Built 2026-09-30 and verified first in CircuitPython's unix port with the real cores, then
on a Fruit Jam (CircuitPython 11.0.0-alpha.1).

## Verified on the board (2026-09-30)

No SD card was in the slot, so `/sd/emus` and `/sd/cart` were staged in the empty `/sd`
mount-point folder on CIRCUITPY. The launcher cannot tell the difference; a real card mounts
over that folder.

| ROM | Console | fps | emu + input ms |
|---|---|---|---|
| BladeBuster.nes | NES | 60.1 | 11.1 |
| cgb-acid2.gbc | Game Boy Color | 59.7 | 14.4 |
| smasteroids.a78 | Atari 7800 | 60.0 | 9.7 |
| sword.gg | Game Gear | 60.0 | 8.0 |
| sword.sms | Master System | 60.0 | 8.7 |
| T_TRIS.O | Lynx | 75.0 | 9.3-10.4 |
| thrust.bin | Atari 2600 | 60.0 | 10.5 |
| viboritas.col | ColecoVision | 60.0 | 6.5 |
| 3-corax+.ch8 | CHIP-8 | 60.1 | 0.5 |

- Native `.mpy` modules import from `/sd/emus` (folder on CIRCUITPY; a real SD VFS still to try).
- Single ROM boots straight in. With nine ROMs the menu renders correctly (board screenshot).
- `fjarcade.launch()` (nvm handoff + `supervisor.reload()`) starts each of the nine consoles;
  each renders correctly in a frame-300 screenshot.
- Sound comes up (after the fix below). Battery save falls back to `/saves` when the drive is
  read-only and is loaded from there next boot.
- Junk `.nes`: magenta path, reason on serial every 2 s.
- Bug found on the board and fixed: `adafruit_fruitjam.Peripherals` claims BUTTON1-3, so with
  `Pad` already holding them sound was off ("BUTTON1 in use"). `run()` now releases the onboard
  keys around audio setup and claims them back.
- Test aid added: `FJ_SCREENSHOT = <frame>` in settings.toml dumps the menu and the game's
  frame over serial in the ports' BEGIN_FB format. Off by default.

Still needs a person at the board: the arcade pad and Start + Coin on real pins, the picture
and sound on a monitor, a real microSD card, red/yellow screens.

## Decisions

- **Model B (hybrid)**, chosen by Mikey: one ROM boots straight in (AAM's model); more than
  one shows a menu; Start + Coin held 1 s returns to it.
- **Handoff by `microcontroller.nvm` + `supervisor.reload()`**, so each game starts on a
  clean heap and the menu's displayio objects never share memory with an emulator. The
  pending pick is cleared when read, so a crashing game does not loop. The last pick is
  kept at nvm offset 256 and only rewritten when it changes.
- **Everything but `code.py` lives on the card** (`/sd/emus`), as asked. `code.py` shows
  red/yellow itself, because `fjarcade` cannot be imported without the card.
- **`.gb` runs on the GBC core** (`peanutgbc`). It passes dmg-acid2 pixel-exact, was faster
  on the board than the root port, and avoids the root `peanut_gb.h` licence question.
- **`.bin` in `/cart` is an Atari 2600 cart.** Only ROMs belong in `/cart`.
- **Saves** follow AAM's "dirty and quiet for ~1 s" rule by comparing the save RAM to a
  snapshot every 250 ms, so the cores needed no dirty flag. AAM rewrites the `.sav` in place
  sector by sector to avoid red lines; this writes the whole file. A save is at most 32 KB.
  Watch for a frame hitch at save time on hardware.
- **Read-only card**: when a computer has the SD card mounted over USB, CircuitPython locks
  it against code writes (`usb_msc_flash.c`, `tud_msc_is_writable_cb`). Saves fall back to
  `/saves` and are read from there first. After any successful save to the card the `/saves`
  copy is deleted, so it never shadows a newer card save (bug found on the board 2026-10-01,
  fixed and checked: boot 1 loaded `/saves` and saved to the card, boot 2 loaded the card,
  `/saves` empty). README recommends `CIRCUITPY_SDCARD_USB = false`.
- **The launcher mounts the card itself** over SPI at 25 MHz when CircuitPython has not
  (`CIRCUITPY_SDCARD_USB = false` stops CircuitPython's own mount). `sdioio` on this Fruit
  Jam fails with SDIO Init Error 0x17 at every clock, and a 1-bit `sdioio` attempt hard
  faulted CircuitPython (worth an upstream issue); SPI reads cleanly at 25 MHz.
- **The framebuffer is made first** (`make_framebuffer()` at the top of `code.py`, passed
  into every `fj.Video`). Found 2026-10-01: after a game and a REPL session, the next
  `picodvi.Framebuffer(color_depth=16)` failed with `MemoryError` (153600 bytes of internal
  SRAM not available once the 1020-ROM list and other allocations came first). picodvi then
  keeps the only display slot (`CIRCUITPY_DISPLAY_LIMIT` 1): the constructor marks it used
  before allocating, and `deinit` is a no-op when `framebuffer == NULL`. So the 8-bit retry
  said "Too many display busses". `make_framebuffer()` calls `displayio.release_displays()`
  after a failed try. Verified on the board: the failing sequence (game, REPL, launch) that
  failed 3 of 3 now runs NES, MSX and Supervision at full speed. The slot leak is a
  CircuitPython bug worth an upstream issue.
- **Onboard buttons** keep AAM's roles. Only CHIP-8 (colours) and the 2600 (B&W switch on
  Button 3) do anything with them; the cores have no stretch or rotate yet.
- **Menu sort is case-insensitive** (found in the sim: `T_TRIS.O` sorted to the top).
- **CHIP-8 programs that end on purpose** (SUPER-CHIP 00FD) go back to the menu, not to
  the magenta screen (`exited()` hook, proposed by the CHIP-8 adapter agent).

## Verified in the unix-port sim (`tools/sim`)

Real launcher, real `fjarcade`, real adapters, real cores built for x64, stand-in modules
for board, keypad, picodvi, nvm and reload.

- All 11 test ROMs boot through the launcher and render correctly (NES, GB, GBC, 2600,
  7800, Lynx, SMS, GG, SG-1000, Coleco, CHIP-8).
- Pad input reaches every core whose test ROM reads input: NES, 7800, SMS, GG, SG-1000,
  2600, Lynx, and Coleco (its controller test cart lights Left, left fire and keypad 1 for
  D8, D10 and D6). corax+ and the acid2 screens ignore input by design.
- Single ROM boots directly; Start + Coin returns to the menu; the menu lists every ROM with
  its console, wraps, opens on the last pick, and A launches.
- A junk `.nes` shows magenta (`0xF81F`) with the reason on serial every 2 s, and
  Start + Coin escapes it.
- `.sav` written on exit and loaded on the next boot (Blade Buster).
- Button 3 on CHIP-8 writes `palette = 1` to the `.fruitjam.cfg` 3 s later.
- `lynxboot.img` that is too short is ignored with a message; BIOS files are hidden from
  the menu.

## Not verified yet

1. A real microSD card (modules and ROMs on the SD VFS rather than a CIRCUITPY folder).
2. The arcade pad on A1-A5 / D6-D10 and Start + Coin back to the menu, on real pins.
3. Picture and sound judged by a person on a monitor and speakers.
4. Saves written to the card itself (needs the card, and `CIRCUITPY_SDCARD_USB = false` or no
   computer mounting it).
5. Red (no card) and yellow (empty /cart) screens.

## Hardware bring-up order

1. `tools/make_sd.sh <dir>`, copy to a FAT32 card, add ROMs to `/cart`, insert.
2. Copy `CIRCUITPY/code.py`, run circup (README step 2).
3. One ROM in `/cart` (Blade Buster): it should boot straight to the NES at 60 fps.
4. Add a second ROM: the menu should appear; pick each, Start + Coin back.
5. Pull the card: red. Empty `/cart`: yellow. A junk `.nes`: magenta.
6. A game with battery RAM: save in game, wait 2 s, power cycle, check it loads.
