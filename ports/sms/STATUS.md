19:02 started: read briefs and nes/ template; writing own Z80 + VDP core (MIT)
19:08 test ROMs fetched (zexdoc/zexall, SMS Power homebrew); writing Z80 generator
19:17 zexdoc: all 67 tests OK on host
19:21 zexall passes too; SwordWork title screens render on SMS, GG, SG (fixed DI/EI swap)
19:28 sms.mpy builds (72 KB); armbench: 0.84-1.0 M instr/frame mean, 1.13 M worst (~80-90 fps est.)
19:34 host gate ALL PASS (zexdoc, zexall, PSG, mappers, 11 pinned frame CRCs); code.py smoke-tested under CPython stubs; writing README/NOTES
19:36 DONE: README/NOTES written, sms.mpy 77 KB in CIRCUITPY/lib, run_tests.sh ALL PASS
