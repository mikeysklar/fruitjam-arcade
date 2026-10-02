#!/bin/bash
# usage: test_all.sh <rom dir> <x64 mpy dir> <out dir> [seconds]
# Boots each ROM alone (single-cart path) for a few seconds and dumps the frame.
set -u
here=$(cd "$(dirname "$0")" && pwd)
roms=$(cd "$1" && pwd); mpys=$(cd "$2" && pwd); out=$3; secs=${4:-5}
mkdir -p "$out"; out=$(cd "$out" && pwd)
printf 'run 0\nend %d\n' $((secs * 1000)) > "$out/plain.script"
for rom in "$roms"/*; do
  n=$(basename "$rom"); t="$out/root_$n"
  rm -rf "$t"; mkdir -p "$t/sd/cart" "$t/emus"
  cp "$rom" "$t/sd/cart/"; cp "$mpys"/*.mpy "$t/emus/"
  echo "##### $n"
  "$here/run_sim.sh" "$t" "$out/plain.script" 1 2>&1 | grep -vE "^=== run|gamepad off" | tail -8
  [ -f "$t/fb_run0.bin" ] && python3 "$here/fb2png.py" "$t/fb_run0.bin" "$out/$n.png"
done
