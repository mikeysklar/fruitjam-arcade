#!/bin/bash
# Fetch the freely distributable test ROMs used by run_tests.sh into testroms/
# (gitignored). Source: the nes-test-roms collection, pinned commit.
set -e
cd "$(dirname "$0")"
COMMIT=95d8f621ae55cee0d09b91519a8989ae0e64753b
TMP=$(mktemp -d)
git clone -q https://github.com/christopherpow/nes-test-roms "$TMP/r"
git -C "$TMP/r" checkout -q $COMMIT
R=$TMP/r
mkdir -p testroms
cp $R/other/nestest.nes $R/other/nestest.log $R/other/BladeBuster.nes \
   $R/other/quantum_disco_brothers_by_wAMMA.nes $R/other/oam3.nes \
   $R/instr_test-v5/official_only.nes $R/instr_test-v5/all_instrs.nes \
   $R/instr_timing/instr_timing.nes $R/oam_read/oam_read.nes \
   $R/cpu_timing_test6/cpu_timing_test.nes $R/branch_timing_tests/*.nes \
   $R/spritecans-2011/spritecans.nes $R/240pee/240pee.nes testroms/
cp -r $R/sprite_hit_tests_2005.10.05 testroms/
mkdir -p testroms/apu_test testroms/mmc3_test_2
cp $R/apu_test/rom_singles/*.nes testroms/apu_test/
cp $R/mmc3_test_2/rom_singles/*.nes testroms/mmc3_test_2/
rm -rf "$TMP"
echo "test ROMs in $(pwd)/testroms"
