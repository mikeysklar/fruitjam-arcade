<!--
SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
SPDX-License-Identifier: MIT
-->
12:50 started; read coleco template, launcher contract
12:55 test ROMs: CVBasic examples built for MSX (BSD-2), theNestruo World Rally demo + portraits (GPL-3); C-BIOS 0.29a fetched, license is BSD-2 style, binaries redistributable with notice
13:04 msx.h/msx.c written (Z80 + VDP from coleco, AY, SCC, PPI, slots, 4 mappers + guess); selftest passes; all test ROMs render on host; msx.mpy 99 KB
13:10 emu_msx.py adapter written and pushed to the board with msx.mpy and test ROMs
13:25 board: World Rally demo 59.9 fps, 7-9.5 ms/frame; screenshot frame 600 byte-identical to host
13:35 board: viboritas, portraits (ASCII8 via cfg), CVBasic bank (Konami) all 59.9 fps; screenshots identical to host; run_tests.sh all pass incl. zexall
13:45 README, NOTES, standalone code.py; board back on the launcher menu
