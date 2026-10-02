# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Fruit Jam Arcade: a console launcher for CircuitPython.

The SD card is the cartridge, as in Adafruit Arcade Machines:

  /sd/cart/   your ROMs (.nes .gb .gbc .a26 .bin .a78 .lnx .o .sms .gg .sg .col .ch8
              .rom .sv), plus the .sav and .fruitjam.cfg files the games write
  /emus/      on CIRCUITPY: the emulators (*.mpy), their adapters (emu_*.py) and
              fjarcade.py

One ROM in /cart boots straight into its console. With more than one, a menu
lists the consoles, then that console's games: Up/Down to choose, Left/Right a
page at a time, A or Start to pick, B to go back to the consoles. Hold
Start + Coin for a second to come back to the menu.

Error screens: red = no SD card, yellow = no ROM in /cart, magenta = the ROM
was rejected. The reason is printed on serial every 2 seconds.
"""

import os
import sys
import time

sys.path.insert(0, "/emus")


def make_framebuffer():
    """The one 320x240 picodvi framebuffer every screen here draws into.

    Made first, before the card is mounted and the ROM list is built: it needs
    150 KB of contiguous internal SRAM, and later allocations can leave too
    little. Returns (framebuffer, bits per pixel), or (None, 0).
    """
    import board
    import displayio
    import picodvi

    pins = dict(
        clk_dp=board.CKP, clk_dn=board.CKN, red_dp=board.D0P, red_dn=board.D0N,
        green_dp=board.D1P, green_dn=board.D1N, blue_dp=board.D2P, blue_dn=board.D2N,
    )
    displayio.release_displays()
    for depth in (16, 8):
        try:
            return picodvi.Framebuffer(320, 240, color_depth=depth, **pins), depth
        except (MemoryError, ValueError) as e:
            print("%d-bit framebuffer failed: %s" % (depth, e))
            # A failed Framebuffer() keeps CircuitPython's only display slot
            # taken; release it or the next try says "Too many display busses".
            displayio.release_displays()
    return None, 0


FB = make_framebuffer()


def bare_error(colour, reason):
    """Red/yellow screen for problems found before fjarcade is imported."""
    fb, bpp = FB
    if fb is not None:
        if bpp == 8:
            colour = ((colour >> 8) & 0xE0) | ((colour >> 6) & 0x1C) | ((colour >> 3) & 0x03)
        buf = memoryview(fb)
        for i in range(len(buf)):
            buf[i] = colour
    while True:
        print("ERROR:", reason)
        time.sleep(2)


def mount_sd():
    """Mount the card at /sd unless CircuitPython already did.

    CircuitPython mounts it at boot unless settings.toml has
    CIRCUITPY_SDCARD_USB = false (which also keeps it writable while USB is
    connected). In that case mount it here over SPI. Returns True when a card is
    mounted at /sd.
    """
    import storage

    try:
        storage.getmount("/sd")
        return True  # CircuitPython mounted it
    except OSError:
        pass  # /sd is just a folder on CIRCUITPY
    try:
        import board
        import busio
        import sdcardio

        spi = busio.SPI(board.SD_SCK, board.SD_MOSI, board.SD_MISO)
        sd = sdcardio.SDCard(spi, board.SD_CS, baudrate=25_000_000)
        storage.mount(storage.VfsFat(sd), "/sd")
        print("SD card mounted at /sd")
        return True
    except Exception as e:  # no card, or it would not mount
        print("SD card not mounted:", e)
        return False


try:
    os.listdir("/emus")
except OSError:
    bare_error(0xFFE0, "no /emus folder on CIRCUITPY; reinstall Fruit Jam Arcade")
if not mount_sd():
    bare_error(0xF800, "no SD card (or it would not mount)")

import fjarcade as fj  # noqa: E402  (needs /emus on sys.path first)


def run_rom(path, mod_name):
    print("cartridge", path, "->", mod_name)
    video = fj.Video(*FB)
    pad = fj.Pad()
    cfg = fj.Config(path)
    try:
        mod = __import__(mod_name)
        emu = mod.Emu(path, fj.read_rom(path), video, cfg)
    except fj.RomError as e:
        fj.error_screen(video, fj.MAGENTA, str(e), pad)
    except (ImportError, MemoryError) as e:
        fj.error_screen(video, fj.MAGENTA, "%s: %s" % (mod_name, e), pad)
    result = fj.run(emu, video, pad, cfg, fj.SYSTEM_NAMES.get(mod_name, mod_name))
    if result:
        fj.error_screen(video, fj.MAGENTA, result, pad)
    fj.back_to_menu()


def menu(roms, last):
    """Pick a console, then one of its games. Returns the chosen path.

    With games for only one console on the card the console list is skipped.
    """
    import board
    import displayio
    import framebufferio
    import terminalio
    from adafruit_display_text import label

    video = fj.Video(*FB)
    display = framebufferio.FramebufferDisplay(video.fb, auto_refresh=False)
    root = displayio.Group()
    display.root_group = root
    title = label.Label(terminalio.FONT, text="", color=0xFFCC00, x=8, y=10, scale=2)
    root.append(title)
    rows = 15
    lines = []
    for i in range(rows):
        lb = label.Label(terminalio.FONT, text="", color=0xC0C0C0, x=8, y=40 + i * 12)
        root.append(lb)
        lines.append(lb)
    hint = label.Label(terminalio.FONT, text="", color=0x808080, x=8, y=230)
    root.append(hint)
    pad = fj.Pad()
    state = {"prev": pad.poll(), "shot": False}  # buttons already held do not count

    def choose(heading, texts, sel, back):
        """Scrolling list. Returns the chosen index, or None for B when back is allowed."""
        title.text = heading
        if back:
            hint.text = "Up/Down  Left/Right page  A play  B back"
        else:
            hint.text = "Up/Down choose  A or Start pick"
        top = max(0, sel - rows + 1)
        repeat_at = 0
        shown = None
        while True:
            now = time.monotonic_ns()
            m = pad.poll()
            pad.actions.clear()
            pressed = m & ~state["prev"]
            state["prev"] = m
            step = 0
            if pressed & fj.UP or (m & fj.UP and now >= repeat_at):
                step = -1
            elif pressed & fj.DOWN or (m & fj.DOWN and now >= repeat_at):
                step = 1
            elif pressed & fj.LEFT or (m & fj.LEFT and now >= repeat_at):
                step = -rows
            elif pressed & fj.RIGHT or (m & fj.RIGHT and now >= repeat_at):
                step = rows
            if step:
                first = pressed & (fj.UP | fj.DOWN | fj.LEFT | fj.RIGHT)
                repeat_at = now + (400_000_000 if first else 80_000_000)
                if step in (-1, 1):
                    sel = (sel + step) % len(texts)  # Up/Down wrap
                else:
                    sel = min(max(sel + step, 0), len(texts) - 1)  # a page, stops at the ends
            if pressed & (fj.A | fj.START):
                return sel
            if back and pressed & fj.B:
                return None
            if sel < top:
                top = sel
            elif sel >= top + rows:
                top = sel - rows + 1
            if (sel, top, heading) != shown:
                shown = (sel, top, heading)
                for i, lb in enumerate(lines):
                    j = top + i
                    if j < len(texts):
                        lb.text = ("> " if j == sel else "  ") + texts[j]
                        lb.color = 0xFFFFFF if j == sel else 0xA0A0A0
                    else:
                        lb.text = ""
                display.refresh()
                if fj.screenshot_at() and not state["shot"]:
                    state["shot"] = True
                    video.screenshot()
            time.sleep(0.016)

    groups = {}
    for p, mod in roms:
        groups.setdefault(mod, []).append(p)
    systems = sorted(groups, key=lambda m: fj.SYSTEM_NAMES.get(m, m).lower())
    s = 0
    g = 0
    for i, mod in enumerate(systems):
        if last in groups[mod]:
            s, g = i, groups[mod].index(last)
    while True:
        if len(systems) > 1:
            texts = ["%-14s %5d" % (fj.SYSTEM_NAMES.get(m, m), len(groups[m])) for m in systems]
            pick = choose("FRUIT JAM ARCADE", texts, s, False)
            if pick != s:
                g = 0
            s = pick
        mod = systems[s]
        games = groups[mod]
        name = fj.SYSTEM_NAMES.get(mod, mod)
        pick = choose(name.upper() if len(systems) > 1 else "FRUIT JAM ARCADE",
                      [p.rsplit("/", 1)[-1][:48] for p in games], g, len(systems) > 1)
        if pick is not None:
            pad.deinit()
            return games[pick]


# ---- boot ------------------------------------------------------------------------
payload = fj.handoff_take()
try:
    roms = fj.list_roms()
except OSError:
    roms = []
if not roms:
    fj.error_screen(fj.Video(*FB), fj.YELLOW, "no ROM in /sd/cart")

want_menu = payload == fj.NVM_MENU
target = None
if payload and not want_menu:
    p = payload.decode()
    for path, mod in roms:
        if path == p:
            target = (path, mod)
if target is None and len(roms) == 1 and not want_menu:
    target = roms[0]

if target is not None:
    run_rom(*target)
else:
    last = fj.last_take()
    choice = menu(roms, last)
    fj.last_write(choice)
    fj.launch(choice)
