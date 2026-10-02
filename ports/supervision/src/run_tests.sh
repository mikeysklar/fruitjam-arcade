#!/bin/sh
# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
#
# Host correctness gate. make -f Makefile.host first; ./get_testroms.sh fetches
# the free test programs into testroms/ (missing ones are skipped, not failed).
# Frame CRCs were checked by eye against screenshots (shots/, host_sv's fbdump).
cd "$(dirname "$0")"
fail=0
skip=0
T=testroms
I=testroms/inufuto
START="0:0,300:80,310:0,400:20,410:0"  # Start on the title, then A

ok() { echo "PASS $1"; }
bad() { echo "FAIL $1: $2"; fail=1; }

cpu() {  # name file success_pc (Rockwell ops on, as on the Supervision's 65C02)
    if [ ! -f "$2" ]; then echo "SKIP $1 (no $2)"; skip=1; return; fi
    out=$(./host_cpu_test rockwell "$2" "$3" 2>&1)
    case "$out" in *PASS*) ok "$1" ;; *) bad "$1" "$out" ;; esac
}

frame() {  # name file frames crc [script]
    if [ ! -f "$2" ]; then echo "SKIP $1 (no $2)"; skip=1; return; fi
    out=$(./host_sv run "$2" "$3" - "$5" | tail -1)
    crc=$(echo "$out" | sed -n 's/.*crc=\([0-9a-f]*\).*/\1/p')
    [ "$crc" = "$4" ] && ok "$1 crc=$crc" || bad "$1" "crc=$crc want $4"
}

cpu "Klaus 6502 functional" $T/6502_functional_test.bin 3469
cpu "Klaus 65C02 extended" $T/65C02_extended_opcodes_test.bin 24F1

# Our own cart (always present): banking, VRAM DMA, timer, NMI, audio DMA IRQ.
out=$(SV_DUMP=0 ./host_sv run testcart/svtest.sv 300 -)
echo "$out" | grep -q "^0000: .. .. .. B0 B1 B2 B3 00 00 B0 85 .. 5A" && ok "svtest start-up checks" \
    || bad "svtest checks" "$(echo "$out" | grep ^0000)"
nmi=$(echo "$out" | sed -n 's/^0000: \(..\) \(..\) \(..\).*/\1 \2 \3/p')
set -- $nmi
[ "$1" = 2A ] && ok "svtest NMI count" || bad "svtest NMI" "$1"
[ "$2" = 2B ] && ok "svtest timer IRQ count" || bad "svtest timer" "$2"
[ "$3" = 12 ] && ok "svtest audio DMA IRQ count" || bad "svtest audio DMA" "$3"
echo "$out" | grep -q "crc=92f916d8" && ok "svtest frame" || bad "svtest frame" "$(echo "$out" | grep BENCH)"
peak=$(echo "$out" | sed -n 's/.*peak=\([0-9]*\).*/\1/p')
[ "${peak:-0}" -gt 10000 ] && ok "svtest audio peak=$peak" || bad "svtest audio" "peak=$peak"
pad=$(SV_DUMP=0 ./host_sv run testcart/svtest.sv 20 - "0:A5" | sed -n 's/^0000: .. .. .. .. .. .. .. \(..\).*/\1/p')
[ "$pad" = A5 ] && ok "svtest joypad" || bad "svtest joypad" "$pad"

frame "ascend title" $I/ascend.bin 300 0cf2464b
frame "aerial play" $I/aerial.bin 600 726a555e "$START"
frame "antiair play" $I/antiair.bin 600 60a35409 "$START"
frame "ascend play" $I/ascend.bin 600 5350cf3b "$START"
frame "awass play" $I/awass.bin 600 c0a98560 "$START"
frame "battlot play" $I/battlot.bin 600 c49e5076 "$START"
frame "bootskell play" $I/bootskell.bin 600 b5ef1705 "$START"
frame "cacorm play" $I/cacorm.bin 600 d45a3593 "$START"
frame "cavit play" $I/cavit.bin 600 f21521cc "$START"
frame "cracky play" $I/cracky.bin 600 325e651a "$START"
frame "guntus play" $I/guntus.bin 600 fb5cc895 "$START"
frame "hopman play" $I/hopman.bin 600 5295d619 "$START"
frame "impetus play" $I/impetus.bin 600 62ab5c39 "$START"
frame "lift play" $I/lift.bin 600 78d1cb6c "$START"
frame "mazy play" $I/mazy.bin 600 d7a542dc "$START"
frame "mazy2 play" $I/mazy2.bin 600 b3260268 "$START"
frame "mieyen play" $I/mieyen.bin 600 9a7ab0e9 "$START"
frame "neuras play" $I/neuras.bin 600 76526121 "$START"
frame "osotos play" $I/osotos.bin 600 48c2c9ca "$START"
frame "ruptus play" $I/ruptus.bin 600 90b898b5 "$START"
frame "svellas play" $I/svellas.bin 600 b9abc977 "$START"
frame "sword play" $I/sword.bin 600 3ab37c56 "$START"
frame "uchotos play" $I/uchotos.bin 600 7f357f59 "$START"
frame "yewdow play" $I/yewdow.bin 600 39b58f5e "$START"
# The D-pad reaches the game: holding Right from frame 420 moves the player.
frame "sword right" $I/sword.bin 600 bddeb663 "$START,420:01"

python3 -m py_compile ../../launcher/sd/emus/emu_supervision.py && ok "emu_supervision.py compiles" \
    || bad "emu_supervision.py" "py_compile"

if [ $fail -ne 0 ]; then echo "SOME FAILED"; exit 1; fi
[ $skip -ne 0 ] && echo "ALL PASS (some skipped)" || echo "ALL PASS"
