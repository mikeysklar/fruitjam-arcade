#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Convert Loom PC-Engine CD audio tracks (TrackNN.flac/.wav/.bin, any format
# ffmpeg reads) to the IMA ADPCM WAV files the engine streams: 22050 Hz,
# 4 bits a sample, 1 KB blocks. Mono by default (11 KB/s); CHANNELS=2 for
# stereo (22 KB/s).
#
# usage: tools/pce_tracks.sh SRCDIR DESTDIR
set -eu
src=$1
dest=$2
ch=${CHANNELS:-1}
mkdir -p "$dest"
# Track 1 is the "not a CD player" warning and track 2 is data; the game
# plays tracks 3-21.
for n in 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20 21; do
	in=$(ls "$src"/Track$n.* 2>/dev/null | grep -iv '\.wav$' | head -1 || true)
	[ -n "$in" ] || { echo "Track$n missing in $src"; continue; }
	ffmpeg -nostdin -loglevel error -y -i "$in" -ac "$ch" -ar 22050 \
		-c:a adpcm_ima_wav -block_size 1024 "$dest/Track$n.wav"
done
ls -l "$dest"/Track*.wav
