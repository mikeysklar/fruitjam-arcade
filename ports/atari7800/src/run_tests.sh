#!/bin/bash
# Host correctness gate. Needs testroms/ (./get_testroms.sh) and a C compiler.
# Frame CRCs are CRC-32 of the 320x240 window of 7800 color bytes of the last
# frame; each was checked by eye (screenshot) once, then pinned here.
cd "$(dirname "$0")"
make -s host || exit 1
fail=0
check() {  # name, expected, actual
    if [ "$2" == "$3" ]; then echo "PASS $1"; else echo "FAIL $1: want $2 got $3"; fail=1; fi
}

out=$(./host_cpu_test testroms/6502_functional_test.bin | tail -1)
echo "$out"
echo "$out" | grep -q PASS || fail=1
./host_cpu_test | tail -1 | grep -q PASS && echo "PASS own CPU checks" || { echo "FAIL own CPU checks"; fail=1; }

./host_main --check testcart/testcart.a78 testcart/testcart.expect || fail=1

bench() {  # rom frames [inputs]
    ./host_main testroms/$1 $2 ${3:+-i $3}
}
crc() { bench "$@" | sed -n 's/.*crc=\([0-9a-f]*\).*/\1/p'; }
peak() { bench "$@" | sed -n 's/.*peak=\([0-9]*\).*/\1/p'; }

check "helloworld (320A text) frame 300" 8402c5c2 $(crc helloworld.a78 300)
check "fontcheck (320A, char mode) frame 600" ab6e9d06 $(crc fontcheck.a78 600)
check "splitmodedemotop (DLI 320A/160A split) frame 600" 63b2a436 $(crc splitmodedemotop.a78 600)
check "screensafe (224 line zone grid) frame 600" 2d118ff5 $(crc screensafe.a78 600)
check "banner (big 160A banner) frame 600" 634eaeb9 $(crc banner.a78 600)
check "simple (joystick moves sprite) frame 200" b92bcbe5 $(crc simple.a78 200 1-100:a)
check "smasteroids (48K, gameplay with input) frame 900" e2abc55f \
    $(crc smasteroids.a78 900 60-65:10,200-400:14,300-700:1,500-900:10)
check "doublebuffer (SG+RAM) frame 600" 9fcdd841 $(crc doublebuffer.a78 600)
check "onehundredandone (SG+RAM, 101 sprites) frame 1200" a477a8a5 $(crc onehundredandone.a78 1200)
check "eightysix (SG+RAM, POKEY, 160A banner, 86 sprites) frame 600" eb3ed746 $(crc eightysix.a78 600)
check "rmtromdemo (48K, POKEY) frame 600" d2481a3c $(crc rmtromdemo.a78 600)
check "spacephysics (320C sprite) frame 600" effd75c9 $(crc spacephysics.a78 600)
check "pokey (POKEY detected at \$450) frame 300" 19778847 $(crc pokey.a78 300)
p=$(peak trackertest.a78 900 100-110:10); [ "$p" -gt 5000 ] && echo "PASS trackertest TIA music plays (peak $p)" || { echo "FAIL trackertest peak $p"; fail=1; }
p=$(peak pokey.a78 300); [ "$p" -gt 5000 ] && echo "PASS pokey music plays (peak $p)" || { echo "FAIL pokey peak $p"; fail=1; }

[ $fail == 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
