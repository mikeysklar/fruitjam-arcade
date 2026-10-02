#!/bin/sh
# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
#
# Fetch the free test programs into testroms/ (gitignored, never committed).
# Klaus Dormann's CPU tests (GPL-3, test data only, not linked into anything),
# Lynx public domain demos from zophar.net, and Catkanoid (free download from
# its author). No commercial ROMs and no boot ROM.
set -e
cd "$(dirname "$0")"
mkdir -p testroms/pd
cd testroms

K=https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files
[ -f 6502_functional_test.bin ] || curl -sLO $K/6502_functional_test.bin
[ -f 65C02_extended_opcodes_test.bin ] || curl -sLO $K/65C02_extended_opcodes_test.bin

[ -f catkanoid.lnx ] || curl -sLO http://sebastianmihai.com/downloads/catkanoid/catkanoid.lnx

cd pd
for p in a-corridor:11846 sprite-demos:11845 the-dungeon-v0-11:11847 jumtro:11850 \
         demo-collection:11849 befok:11853 pcx-to-lynx-spr-test:11855; do
    name=${p%%:*}
    id=${p##*:}
    if [ ! -d "$name" ]; then
        curl -sL "https://www.zophar.net/download_file/$id" -o "$name.zip"
        mkdir -p "$name"
        (cd "$name" && unzip -oq "../$name.zip")
    fi
done
echo "testroms ready"
