#!/bin/sh
# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
#
# Fetch the free test programs into testroms/ (gitignored, never committed).
# Klaus Dormann's CPU tests (GPL-3, test data only, not linked into anything)
# and inufuto's Supervision games (free downloads from the author's site,
# http://inufuto.web.fc2.com/8bit/, freeware with no written licence; see
# ../NOTES.md). No commercial ROMs. The Supervision has no BIOS.
set -e
cd "$(dirname "$0")"
mkdir -p testroms/inufuto
cd testroms

K=https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files
[ -f 6502_functional_test.bin ] || curl -sfLO $K/6502_functional_test.bin
[ -f 65C02_extended_opcodes_test.bin ] || curl -sfLO $K/65C02_extended_opcodes_test.bin

cd inufuto
I=http://inufuto.web.fc2.com/8bit/svision
for g in mazy neuras ruptus battlot cavit impetus bootskell aerial lift ascend cacorm guntus \
         cracky hopman yewdow antiair osotos mieyen mazy2 svellas awass sword uchotos; do
    if [ ! -f $g.bin ]; then
        curl -sfL -o ${g}_bin.zip $I/${g}_bin.zip
        unzip -oq ${g}_bin.zip
    fi
done
echo "testroms ready"
