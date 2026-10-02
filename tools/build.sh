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
# Needs: folder2uf2 and circup (pip install folder2uf2 circup), curl, zip.
# Rebuilding also needs arm-none-eabi-gcc and pyelftools.
set -euo pipefail

CP_VERSION=${CP_VERSION:-11.0.0-alpha.1}
BOARD=adafruit_fruit_jam
CP_UF2=adafruit-circuitpython-$BOARD-en_US-$CP_VERSION.uf2
CP_URL=https://downloads.circuitpython.org/bin/$BOARD/en_US/$CP_UF2

# The Fruit Jam keeps a 2 MB CPSAVES partition at the start of the filesystem area, so
# CIRCUITPY starts at 0x300000 and runs to the end of the 16 MB flash.
FS_OFFSET=0x300000
FLASH_SIZE=0x1000000
FAMILY=0xe48bff59  # RP2350 ARM

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIST=$ROOT/dist
STAGE=$DIST/CIRCUITPY
PORTS="chip8 gbc atari2600 atari7800 nes sms coleco lynx msx supervision"

rm -rf "$DIST"
mkdir -p "$STAGE/emus"

if [ -n "${MPY_DIR:-}" ]; then
  for p in $PORTS; do
    echo "== building $p"
    make -C "$ROOT/ports/$p/src" MPY_DIR="$(cd "$MPY_DIR" && pwd)" ARCH=armv7emsp
    cp "$ROOT/ports/$p/src/"*.mpy "$ROOT/ports/$p/CIRCUITPY/lib/"
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
folder2uf2 --flash-offset $FS_OFFSET --flash-size $FLASH_SIZE --family-id $FAMILY \
  --combine "$DIST/$CP_UF2" -o "$DIST/fruitjam-arcade.uf2" "$STAGE"
rm "$DIST/$CP_UF2"

(cd "$STAGE" && zip -qr "$DIST/fruitjam-arcade-CIRCUITPY.zip" .)
ls -l "$DIST"
