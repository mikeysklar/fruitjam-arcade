#!/bin/bash
# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
# Fetch and build the host test inputs into testroms/ (gitignored, never commit).
#  - zexdoc/zexall .com (Frank Cringle's Z80 exercisers) from the z80emu repo
#  - CVBasic and gasm80 (Oscar Toledo G., BSD-2-Clause), built from source,
#    then CVBasic example programs compiled for MSX: plain 8 KB ROMs, and the
#    bank switching example as a 128 KB ASCII16 and a 128 KB Konami MegaROM
#    (padded to 128 KB, the size a real MegaROM has, so the mapper guess sees it)
#  - World Rally demo (48 KB plain ROM, $0000-$BFFF) and the Justin Cyr
#    portraits slideshow (256 KB ASCII8 MegaROM) by theNestruo, both GPL-3.0,
#    from their GitHub repos, which ship the .rom files
set -e
cd "$(dirname "$0")"
mkdir -p testroms/zex testroms/tools
for f in zexdoc zexall; do
    curl -sfL -o testroms/zex/$f.com https://raw.githubusercontent.com/anotherlin/z80emu/master/testfiles/$f.com
done
curl -sfL -o testroms/wrally_rally-demo.rom https://raw.githubusercontent.com/theNestruo/msx-wrally/master/rally-demo.rom
curl -sfL -o testroms/portraits.rom https://raw.githubusercontent.com/theNestruo/msx-justincyr-female-portraits/master/justincyr-female-portraits.rom
cd testroms/tools
[ -d CVBasic ] || git clone -q --depth 1 https://github.com/nanochess/CVBasic
[ -d gasm80 ] || git clone -q --depth 1 https://github.com/nanochess/gasm80
cc -O2 -o gasm80/gasm80 gasm80/gasm80.c
(cd CVBasic && cc -O2 -o cvbasic cvbasic.c node.c driver.c cpuz80.c cpu6502.c cpu9900.c)
cd CVBasic
for g in viboritas space_attack brinquitos oscar test1 controller; do
    ./cvbasic --msx examples/$g.bas ../$g.asm > /dev/null
    ../gasm80/gasm80 ../$g.asm -o ../../$g.rom > /dev/null
done
./cvbasic --msx examples/bank.bas ../bank.asm > /dev/null
../gasm80/gasm80 ../bank.asm -o ../bank.rom > /dev/null
./cvbasic --msx -konami examples/bank.bas ../bank_k.asm > /dev/null
../gasm80/gasm80 ../bank_k.asm -o ../bank_k.rom > /dev/null
cd ../../..
python3 - <<'PY'
for src, dst in (("bank.rom", "bank128.rom"), ("bank_k.rom", "bank_konami128.rom")):
    d = open("testroms/tools/" + src, "rb").read()
    open("testroms/" + dst, "wb").write(d + b"\xff" * (131072 - len(d)))
PY
ls testroms
