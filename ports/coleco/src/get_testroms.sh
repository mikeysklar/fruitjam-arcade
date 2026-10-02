#!/bin/bash
# Fetch and build the host test inputs into testroms/ (gitignored, never commit).
#  - zexdoc/zexall .com (Frank Cringle's Z80 exercisers) from the z80emu repo
#  - CVBasic and gasm80 (Oscar Toledo G., BSD-2-Clause), built from source,
#    then the CVBasic example programs compiled to .col cartridges
#  - testcarts/modes.asm (this repo) assembled to modes.col
set -e
cd "$(dirname "$0")"
mkdir -p testroms/zex testroms/tools
for f in zexdoc zexall; do
    curl -sfL -o testroms/zex/$f.com https://raw.githubusercontent.com/anotherlin/z80emu/master/testfiles/$f.com
done
cd testroms/tools
[ -d CVBasic ] || git clone -q --depth 1 https://github.com/nanochess/CVBasic
[ -d gasm80 ] || git clone -q --depth 1 https://github.com/nanochess/gasm80
cc -O2 -o gasm80/gasm80 gasm80/gasm80.c
(cd CVBasic && cc -O2 -o cvbasic cvbasic.c node.c driver.c cpuz80.c cpu6502.c cpu9900.c)
cd CVBasic
for g in space_attack viboritas brinquitos oscar bank test1 music controller; do
    ./cvbasic examples/$g.bas ../$g.asm > /dev/null
    ../gasm80/gasm80 ../$g.asm -o ../../$g.col > /dev/null
done
cd ../../..
testroms/tools/gasm80/gasm80 testcarts/modes.asm -o testroms/modes.col > /dev/null
ls testroms
