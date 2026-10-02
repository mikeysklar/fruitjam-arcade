#!/bin/bash
# Host correctness gate. Needs zex/ and testroms/ (./get_testroms.sh) and a C
# compiler. Frame CRCs are CRC-32 of the visible RGB565 pixels of the last
# frame; each was checked by eye (screenshot) once, then pinned here.
cd "$(dirname "$0")"
make -s -f Makefile.host || exit 1
fail=0
check() {  # name, expected, actual
    if [ "$2" == "$3" ]; then echo "PASS $1"; else echo "FAIL $1: want $2 got $3"; fail=1; fi
}

for z in zexdoc zexall; do
    out=$(./host_sms zex zex/$z.com | tail -1)
    check "$z (all 67 groups)" "0 errors" "$(echo "$out" | grep -o '[0-9]* errors')"
done
./host_sms psg > /dev/null
check "PSG tone pitch (440 Hz)" 0 $?
./host_sms mapper > /dev/null
check "Sega and Codemasters mapper paging, cart RAM" 0 $?

frame_crc() {  # rom frames [script]
    ./host_sms run "testroms/$1" $2 "" "$3" 2>/dev/null | sed -n 's/^BENCH.*crc=\([0-9a-f]*\).*/\1/p'
}
P="300-306:10,420-900:08,900-1200:02"  # start, walk right, then down
check "SwordWork SMS title, frame 600" 27070fd9 $(frame_crc SwordWork-SMS/sword.sms 600)
check "SwordWork SMS in game, frame 1200" b70c59f8 $(frame_crc SwordWork-SMS/sword.sms 1200 $P)
check "SwordWork Micro GG title, frame 600" f9b8a7e2 $(frame_crc SwordWorkMicro-GG/sword.gg 600)
check "SwordWork Micro GG in game, frame 1200" 67c7f707 $(frame_crc SwordWorkMicro-GG/sword.gg 1200 $P)
check "SwordWork SG-1000 title (Graphics II), frame 600" 334c7c2d $(frame_crc SwordWork-SG/sword.sg 600)
check "SwordWork SG-1000 in game, frame 1200" 4b20f3f9 $(frame_crc SwordWork-SG/sword.sg 1200 $P)
check "Astro Climber SMS (64 KB Sega mapper, scrolling) in game, frame 2400" 5d3add09 \
    $(frame_crc "AstroClimber-SMS/Astro Climber (v1.2.1).sms" 2400 \
      "600-606:10,900-906:10,1200-1206:10,1500-1506:10,1800-1806:10,2000-2400:08")
check "Tetris4GG (128 KB, GG Start button) in game, frame 1800" 89ae16bb \
    $(frame_crc "Tetris4GG-GG/Tetris4GG - (1.0 GG).gg" 1800 \
      "600-606:80,800-806:80,1000-1006:80,1200-1206:80,1400-1406:10")
check "Nyan Cat SG-1000, frame 600" 0872cce2 $(frame_crc NyanCat-SG/example01.sg 600)
check "Nyan Cat SMS build (TMS mode on SMS VDP), frame 600" 0872cce2 $(frame_crc NyanCat-SG/example01.sms 600)
check "Structris SG-1000 in game, frame 1500" cce0f12f $(frame_crc Structris-SG/structrs.sg 1500 "900-906:10,1000-1500:08")

[ $fail == 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
