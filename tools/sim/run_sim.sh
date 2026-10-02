#!/bin/bash
# usage: run_sim.sh <root> <script> [max_runs]
# <root> must hold sd/cart (ROMs) and emus (x64 .mpy); launcher sources are copied in with
# /sd, /emus and /saves rewritten to live under <root>.
set -u
here=$(cd "$(dirname "$0")" && pwd); L=$(cd "$here/../.." && pwd)
root=$(cd "$1" && pwd); script=$(cd "$(dirname "$2")" && pwd)/$(basename "$2"); max=${3:-4}
MP=${MP:-$HOME/wt-fjarcade/ports/unix/build-coverage/micropython}
mkdir -p "$root/saves"
sub() { sed -e "s/^\( *\)import displayio$/\1import sim_displayio as displayio/" -e "s/^\( *\)import audiocore$/\1import sim_audiocore as audiocore/" -e "s/time\.monotonic_ns/time.time_ns/g" -e "s#\"/sd#\"$root/sd#g" -e "s#\"/saves#\"$root/saves#g" -e "s#\"/emus#\"$root/emus#g" "$1" > "$2"; }
sub "$L/CIRCUITPY/code.py" "$root/code.py"
for f in "$L"/CIRCUITPY/emus/*.py; do sub "$f" "$root/emus/$(basename "$f")"; done
rm -f "$root"/fb_run*.bin "$root"/labels_run*.txt
for ((r=0; r<max; r++)); do
  echo "=== run $r"
  FJ_ROOT=$root FJ_RUN=$r FJ_SCRIPT=$script MICROPYPATH="$here/stubs:$root/emus" \
    "$MP" -X heapsize=16M "$here/main_sim.py"; rc=$?
  [ $rc -eq 3 ] || break
done
