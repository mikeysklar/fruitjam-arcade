#!/bin/sh
# Fetch the free SCUMM v3/v4 demos ScummVM hosts into testdata/ (gitignored).
# Game files never go in the repo.
set -e
cd "$(dirname "$0")"
mkdir -p testdata
cd testdata
for d in loom-dos-long-demo-en loom-dos-short-demo-en monkey1-dos-ega-demo-en \
         indy3-ega-ni-demo-en pass-dos-en; do
	if [ ! -d "$d" ]; then
		[ -f "$d.zip" ] || curl -fL -o "$d.zip" "https://downloads.scummvm.org/frs/demos/scumm/$d.zip"
		mkdir -p "$d"
		unzip -q -o "$d.zip" -d "$d"
	fi
done
ls
