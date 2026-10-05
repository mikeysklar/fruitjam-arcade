#!/bin/bash
# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
#
# Builds the release files in dist/:
#   fruitjam-arcade-code.py         self-extracting code.py for a board already on CircuitPython 11
#   fruitjam-arcade.uf2             CircuitPython + the whole CIRCUITPY drive in one UF2
#   fruitjam-arcade-CIRCUITPY.zip   the same files, to copy by hand
#
# usage: tools/build.sh             use the prebuilt ports/*/CIRCUITPY/lib/*.mpy
#        MPY_DIR=../circuitpython tools/build.sh   rebuild every .mpy first
#
# Needs: folder2uf2 0.1.2 or later and circup (pip install 'folder2uf2>=0.1.2' circup), curl, zip.
# Rebuilding also needs arm-none-eabi-gcc and pyelftools.
set -euo pipefail

CP_VERSION=${CP_VERSION:-11.0.0-alpha.1}
BOARD=adafruit_fruit_jam
CP_UF2=adafruit-circuitpython-$BOARD-en_US-$CP_VERSION.uf2
CP_URL=https://downloads.circuitpython.org/bin/$BOARD/en_US/$CP_UF2

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIST=$ROOT/dist
STAGE=$DIST/CIRCUITPY
PORTS="chip8 gbc atari2600 atari7800 nes sms coleco lynx msx supervision scumm"

# Older folder2uf2 put the Fruit Jam's CIRCUITPY at 0x100000, over CPSAVES.
folder2uf2 --list-boards | grep -q "^$BOARD .*offset=0x300000" ||
  { echo "folder2uf2 is too old: pip install -U 'folder2uf2>=0.1.2'"; exit 1; }

rm -rf "$DIST"
mkdir -p "$STAGE/emus"

if [ -n "${MPY_DIR:-}" ]; then
  for p in $PORTS; do
    echo "== building $p"
    src=$ROOT/ports/$p/src
    [ -f "$src/Makefile" ] || src=$ROOT/ports/$p  # scumm builds from its port folder
    make -C "$src" MPY_DIR="$(cd "$MPY_DIR" && pwd)" ARCH=armv7emsp
    cp "$src/"*.mpy "$ROOT/ports/$p/CIRCUITPY/lib/"
  done
fi

cp -R "$ROOT/CIRCUITPY/." "$STAGE/"
for p in $PORTS; do
  cp "$ROOT/ports/$p/CIRCUITPY/lib/"*.mpy "$STAGE/emus/"
done
find "$STAGE" -name __pycache__ -prune -exec rm -rf {} +

echo "== libraries (.py source: no library bundle targets CircuitPython 11 yet)"
circup --path "$STAGE" --board-id "$BOARD" --cpy-version "$CP_VERSION" --allow-unsupported \
  install --py adafruit_fruitjam relic_usb_host_gamepad adafruit_display_text

echo "== self-extracting code.py"
folder2uf2 --self-extract -o "$DIST/fruitjam-arcade-code.py" "$STAGE"

echo "== CircuitPython $CP_VERSION + CIRCUITPY UF2"
curl -sfL -o "$DIST/$CP_UF2" "$CP_URL"
# folder2uf2 0.1.2+ knows CIRCUITPY sits at 0x300000, after the Fruit Jam's 2 MB CPSAVES.
folder2uf2 --board "$BOARD" --combine "$DIST/$CP_UF2" -o "$DIST/fruitjam-arcade.uf2" "$STAGE"
rm "$DIST/$CP_UF2"

(cd "$STAGE" && zip -qr "$DIST/fruitjam-arcade-CIRCUITPY.zip" .)
ls -l "$DIST"
