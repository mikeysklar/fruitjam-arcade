#!/bin/sh
# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
#
# Host correctness gate. make -f Makefile.host first; ./get_testroms.sh fetches
# the free test ROMs into testroms/ (missing ones are skipped, not failed).
# Frame CRCs were checked by eye against screenshots and against Handy.
cd "$(dirname "$0")"
fail=0
skip=0
T=testroms
P=testroms/pd

ok() { echo "PASS $1"; }
bad() { echo "FAIL $1: $2"; fail=1; }

cpu() {  # name mode file success_pc (Rockwell ops on: the Klaus test uses them)
    if [ ! -f "$3" ]; then echo "SKIP $1 (no $3)"; skip=1; return; fi
    out=$(./host_cpu_test rockwell "$3" "$4" 2>&1)
    case "$out" in *PASS*) ok "$1" ;; *) bad "$1" "$out" ;; esac
}

frame() {  # name file frames crc [script]
    if [ ! -f "$2" ]; then echo "SKIP $1 (no $2)"; skip=1; return; fi
    out=$(./host_lynx run "$2" "$3" - "$5" | tail -1)
    crc=$(echo "$out" | sed -n 's/.*crc=\([0-9a-f]*\).*/\1/p')
    [ "$crc" = "$4" ] && ok "$1 crc=$crc" || bad "$1" "crc=$crc want $4"
}

cpu "Klaus 6502 functional" rockwell $T/6502_functional_test.bin 3469
cpu "Klaus 65C02 extended" rockwell $T/65C02_extended_opcodes_test.bin 24F1
out=$(./host_cpu_test sc02 2>&1)
case "$out" in *PASS*) ok "65SC02 NOP sizes" ;; *) bad "65SC02 NOP sizes" "$out" ;; esac

# Our own cart (always present): sprites, collision, Suzy math, audio.
out=$(LYNX_DUMP=F0 LYNX_DUMPN=1 ./host_lynx run testcarts/lynxtest.lnx 300 -)
echo "$out" | grep -q "BOOT kind=hle" && ok "lynxtest HLE boot" || bad "lynxtest boot" "$(echo "$out" | head -1)"
echo "$out" | grep -q "^00F0: 06 06 03 01 A5" && ok "lynxtest Suzy math" || bad "lynxtest math" "$(echo "$out" | grep 00F0)"
echo "$out" | grep -q "crc=04882926" && ok "lynxtest frame" || bad "lynxtest frame" "$(echo "$out" | grep BENCH)"
peak=$(echo "$out" | sed -n 's/.*peak=\([0-9]*\).*/\1/p')
[ "${peak:-0}" -gt 2000 ] && ok "lynxtest audio peak=$peak" || bad "lynxtest audio" "peak=$peak"

frame "T_TRIS 300" $P/demo-collection/T_TRIS.O 300 0c5d76e7
frame "T_TRIS gameplay" $P/demo-collection/T_TRIS.O 2400 6e12828a \
    "0:0,600:1,610:0,700:1,710:0,1100:1,1110:0,1300:20,1310:0,1400:10,1410:0,1500:1,1505:0,1600:40,1700:0"
frame "catkanoid title" $T/catkanoid.lnx 300 ebb6d700
frame "DUNGEON" $P/the-dungeon-v0-11/DUNGEON.O 300 a7cb094c
frame "DEMO0006" $P/demo-collection/DEMO0006.O 300 a91bdd7c
frame "WUERFEL3" $P/demo-collection/WUERFEL3.O 300 b4b15f8a
frame "LR" $P/demo-collection/LR.O 300 816e8b51
frame "DRAWTEST" $P/demo-collection/DRAWTEST.O 300 6873b07d
frame "befok" $P/befok/befok.com 300 f7f5788f
frame "mydemo" $P/jumtro/mydemo.o 300 67b52671
frame "ssprint" $P/pcx-to-lynx-spr-test/ssprint.o 300 147ada26
frame "RAYCAST" $P/a-corridor/develop/LYNX/RAYCAST/STEP1/RAYCAST.O 300 ea2d58cf

python3 -m py_compile ../CIRCUITPY/code.py && ok "code.py compiles" || bad "code.py" "py_compile"

if [ $fail -ne 0 ]; then echo "SOME FAILED"; exit 1; fi
[ $skip -ne 0 ] && echo "ALL PASS (some skipped)" || echo "ALL PASS"
