19:48 started: reading briefs and nes/sms templates
19:56 Handy (zlib) chosen as reference; writing own C port + 65C02 core
20:15 65C02 core passes Klaus 6502 functional + 65C02 extended tests on host
20:15 core boots cc65 .lnx via HLE decrypt; own test cart renders (TGI, sprites, collision, math OK); lynx.mpy builds (37 KB, bss 0)
20:30 cross-checked vs Handy (zlib, built locally as reference): test cart + 12 PD demos match pixel-for-pixel except animation phase; armbench 0.77-1.4 M instr/frame (60 Hz games est. 67-120 fps)
20:44 DONE: tests ALL PASS (Klaus, 65SC02, lynxtest math/audio/CRC, 12 PD CRCs); lynx.mpy 51 KB bss 0 in CIRCUITPY/lib; README, NOTES, get_testroms.sh, run_tests.sh written; armbench 0.77-1.3 M instr/frame mean, worst 2.48 M
