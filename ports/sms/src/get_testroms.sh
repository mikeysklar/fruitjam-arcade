#!/bin/bash
# Fetch the test programs used by run_tests.sh into zex/ and testroms/ (both
# gitignored). Nothing here is redistributed by this repo.
#  - zexdoc/zexall: Frank Cringle's Z80 exercisers (CP/M .com), from the
#    z80emu repository, pinned commit.
#  - Homebrew from SMS Power (https://www.smspower.org/Homebrew/). See NOTES.md
#    for each one's distribution terms.
set -e
cd "$(dirname "$0")"
Z=https://raw.githubusercontent.com/anotherlin/z80emu/1c418fa0d719abab9273131113defbe276101d95/testfiles
mkdir -p zex testroms
curl -sfL -o zex/zexdoc.com $Z/zexdoc.com
curl -sfL -o zex/zexall.com $Z/zexall.com
H=https://www.smspower.org/uploads/Homebrew
for z in SwordWork-SMS-1.0 SwordWork-SG-1.0 SwordWorkMicro-GG-1.0 NyanCat-SG-0.01 \
         Structris-SG-Updated AstroClimber-SMS-1.2.1 Tetris4GG-GG-1.0; do
    d=${z%-*}
    curl -sfL -o testroms/$z.zip $H/$z.zip
    rm -rf testroms/$d
    unzip -q -o testroms/$z.zip -d testroms/$d
done
echo "test programs in $(pwd)/zex and $(pwd)/testroms"
