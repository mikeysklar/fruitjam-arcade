#!/bin/bash
# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
# Host test suite. Needs testroms/ (see get_testroms.sh). Exit status 0 = all pass.
# ZEXALL=1 also runs zexall (another ~15 s).
set -u
cd "$(dirname "$0")"
make -s -f Makefile.host || exit 1
fail=0
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=1; }

if ./host_zex testroms/zex/zexdoc.com | grep -q "ZEX .* errors=0"; then pass zexdoc; else bad zexdoc; fi
if [ "${ZEXALL:-0}" = 1 ]; then
    if ./host_zex testroms/zex/zexall.com | grep -q "ZEX .* errors=0"; then pass zexall; else bad zexall; fi
fi
if ./host_msx selftest | grep -q "SELFTEST PASS"; then pass "selftest (VDP, slots, PPI, mappers, sound)"; else bad selftest; fi

# rom frames crc [script] [mapper]; CRCs pinned after checking the screenshots
# by eye. All boot through the built-in C-BIOS (logo for the first ~2 s).
check() {
    local got
    got=$(MSX_MAPPER=${5:--1} ./host_msx run "testroms/$1.rom" "$2" "" "" "${4:-}" |
        sed -n 's/^BENCH .*crc=\([0-9a-f]*\).*/\1/p')
    if [ "$got" = "$3" ]; then pass "$1 frame $2 crc $3"; else bad "$1 frame $2 crc $got, want $3"; fi
}
check viboritas 600 bb4460ba
check space_attack 400 ec069670 "30-400:04"      # joystick left
check space_attack 400 ec069670 "30-400:k84"     # cursor left, same result
check brinquitos 600 927f752d
check oscar 600 d0978e73
check test1 600 55bf2475
check controller 300 6813b91c "200-400:35,200-400:k01"  # up left A B, key 1
check bank128 1200 d0978e73                      # ASCII16, guessed
check bank_konami128 1200 d0978e73               # Konami, guessed
check wrally_rally-demo 600 02b739f1             # 48 KB plain at $0000
check portraits 600 74cc8d07 "" 3                # ASCII8 (the guess says ASCII16)
exit $fail
