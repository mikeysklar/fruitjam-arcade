19:49 started; read briefs, nes/ coleco/ atari2600/ templates
20:00 core plan: own MIT code; MARIA semantics from MAME maria.cpp (BSD-3); test carts: 7800basic samples (CC0 runtime) built with wasm 7800basic
20:05 first light: MARIA+CPU+RIOT+TIA+POKEY written; helloworld and simple render on host
20:11 atari7800.mpy builds clean (49.7 KB, bss 0); 34 7800basic sample carts render on host; TIA and POKEY tones verified
20:19 host tests: Klaus PASS, own testcart 138 pixel checks (160A/B, 320A/B/C/D, indirect 1/2 byte, holey 8/16, kangaroo, DLI) PASS, 14 pinned sample CRCs PASS; eightysix est ~70 fps on board
20:23 done: README, NOTES, code.py (SCREENSHOT_AT), tools/fb2png.py, get_testroms.sh, run_tests.sh ALL PASS; recommended ROM src/testroms/eightysix.a78
