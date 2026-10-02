#!/bin/sh
# Host correctness gate for peanutgbc. Needs the test ROMs in testroms/ (see
# NOTES.md for the download line). Exits non-zero on the first failure.
set -e
cd "$(dirname "$0")"
make -s host_test
out=${TMPDIR:-/tmp}/peanutgbc-host
mkdir -p "$out"
R=testroms

echo "cgb-acid2 (CGB mode, via the RGB565 framebuffer)"
./host_test $R/cgb-acid2/cgb-acid2.gbc 120 "$out/cgb-acid2.ppm" | grep -q "CGB mode frame written"
python3 check_png.py "$out/cgb-acid2.ppm" $R/cgb-acid2/cgb-acid2.png

echo "dmg-acid2 (DMG mode)"
./host_test $R/dmg-acid2/dmg-acid2.gb 120 "$out/dmg-acid2.pgm" > /dev/null
python3 check_png.py "$out/dmg-acid2.pgm" $R/dmg-acid2/dmg-acid2-dmg.png

echo "blargg cpu_instrs (CGB-compatible header, runs in CGB mode)"
./host_test $R/blargg/cpu_instrs/cpu_instrs.gb 4000 "$out/cpu_instrs.ppm" > /dev/null
python3 check_png.py "$out/cpu_instrs.ppm" $R/blargg/cpu_instrs/cpu_instrs-dmg-cgb.png

echo "all host checks passed"
