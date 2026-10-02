#!/bin/bash
# Host correctness gate. Needs testroms/ (./get_testroms.sh) and a C compiler.
# Frame CRCs are CRC-32 of the 256x240 NES color indices of the last frame;
# each was checked by eye (screenshot) once, then pinned here as a regression.
cd "$(dirname "$0")"
make -s -f Makefile.host || exit 1
fail=0
check() {  # name, expected, actual
    if [ "$2" == "$3" ]; then echo "PASS $1"; else echo "FAIL $1: want $2 got $3"; fail=1; fi
}

out=$(./host_nes nestest testroms/nestest.nes testroms/nestest.log)
check "nestest trace (8991 lines, PC/regs/P/SP/cycles) and \$02/\$03" "NESTEST 8991 lines match; \$02=00 \$03=00" "$out"

for r in official_only all_instrs instr_timing oam_read apu_test/1-len_ctr apu_test/2-len_table apu_test/3-irq_flag; do
    ./host_nes blargg testroms/$r.nes 5000 > /dev/null
    check "blargg $r" 0 $?
done

frame_crc() {  # rom frames [script]
    ./host_nes run testroms/$1 $2 "" "" $3 2>/dev/null | sed -n 's/^BENCH.*crc=\([0-9a-f]*\).*/\1/p'
}
# Screen text says "PASSED" for all of these.
for t in 01.basics:7e1a18f0 02.alignment:dee3a156 03.corners:201f0749 04.flip:2d2fe297 \
         05.left_clip:875fbd94 06.right_edge:d1467d07 07.screen_bottom:63fc7ec6 \
         08.double_height:0dd4ddac 09.timing_basics:c8744ae1 10.timing_order:ff13e6a5 \
         11.edge_timing:46bcd1ff; do
    check "sprite_hit ${t%%:*} (PASSED screen)" ${t##*:} $(frame_crc sprite_hit_tests_2005.10.05/${t%%:*}.nes 900)
done
check "cpu_timing_test (PASSED screen)" 381de675 $(frame_crc cpu_timing_test.nes 900)
check "nestest menu frame" b23afa6e $(frame_crc nestest.nes 120)
check "spritecans (NROM, 64 sprites) frame 900" d840d75d $(frame_crc spritecans.nes 900)
check "240pee (UxROM, CHR RAM) frame 900" 1211c1ca $(frame_crc 240pee.nes 900)
check "quantum_disco_brothers (CNROM) frame 1200" d544ad4f $(frame_crc quantum_disco_brothers_by_wAMMA.nes 1200)
check "oam3 (AxROM) frame 1200" ed031b4c $(frame_crc oam3.nes 1200)
check "BladeBuster (MMC3 + IRQ split) in game, frame 1800" a8334379 $(frame_crc BladeBuster.nes 1800 start)

[ $fail == 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
