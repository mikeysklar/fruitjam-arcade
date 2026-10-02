12:45 started: read lynx template, launcher contract, Kevin Horton's notes (Unlicense), MAME svision as behaviour reference
13:00 svcore.h + supervision.c written; host harness boots all 23 inufuto games to gameplay first try
13:05 supervision.mpy builds (76 KB); on the board sword.sv runs 61 fps at 14.3 ms/frame, board screenshot OK
13:30 slimmed the CPU memory path and redraw only changed lines: 44.5 KB .mpy, 10.3 ms/frame on the board
13:35 own test cart svtest.sv (ca65): banking, VRAM DMA, timer, NMI, audio DMA IRQ, joypad; run_tests.sh ALL PASS (Klaus x2, svtest, 24 frame CRCs)
14:05 board: svtest 61 fps 12.1 ms, board-svtest.png matches host; scripted SwordWork gameplay CRC on board == host
14:15 DONE: README, NOTES, STATUS, emu_supervision.py; sword.sv and svtest.sv left on the card
