#!/bin/bash
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
if ./host_coleco selftest | grep -q "SELFTEST PASS"; then pass "VDP selftest"; else bad "VDP selftest"; fi

# cart frames crc [script]; CRCs pinned after checking the screenshots by eye.
check() {
    local got
    got=$(./host_coleco run "testroms/$1.col" "$2" "" "" "${4:-}" | sed -n 's/^BENCH .*crc=\([0-9a-f]*\).*/\1/p')
    if [ "$got" = "$3" ]; then pass "$1 frame $2 crc $3"; else bad "$1 frame $2 crc $got, want $3"; fi
}
check viboritas 600 c887bdbf
check space_attack 600 85a9e2a4
check space_attack 120 b539387c "30-400:08"
check brinquitos 600 359c6c10
check oscar 600 d0978e73
check bank 1200 d0978e73
check test1 600 5f64df27
check controller 120 f4e66479 "0-200:936"
check modes 60 306152f0
check modes 180 385fb700
exit $fail
