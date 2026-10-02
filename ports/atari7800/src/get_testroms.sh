#!/bin/bash
# Build the test cartridges used by run_tests.sh into testroms/ (gitignored):
# the sample programs that ship with 7800basic v0.41, compiled with its
# WebAssembly release under wasmtime, plus Klaus Dormann's 6502 test.
# macOS arm64 or Linux x86_64. Needs curl, tar, xz.
set -e
cd "$(dirname "$0")"
mkdir -p testroms
TMP=$(mktemp -d)
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) WT=wasmtime-v49.0.1-aarch64-macos ;;
    Darwin-x86_64) WT=wasmtime-v49.0.1-x86_64-macos ;;
    *) WT=wasmtime-v49.0.1-x86_64-linux ;;
esac
curl -sSL https://github.com/bytecodealliance/wasmtime/releases/download/v49.0.1/$WT.tar.xz | tar xJ -C "$TMP"
curl -sSL https://github.com/7800-devtools/7800basic/releases/download/v0.41/7800basic-0.41-wasm.tar.gz | tar xz -C "$TMP"
export PATH="$TMP/$WT:$PATH" bas7800dir="$TMP/7800basic"
for d in "$bas7800dir"/samples/*/; do
    b=$(ls "$d"*.bas 2>/dev/null | head -1) || true
    [ -n "$b" ] || continue
    (cd "$d" && sh "$bas7800dir/7800basic.sh" "$(basename "$b")" > build.log 2>&1) || true
    for a in "$d"*.a78; do
        [ -f "$a" ] && cp "$a" "testroms/$(basename "$a" .bas.a78).a78"
    done
done
# These two build broken from the release (missing files); not used.
rm -f testroms/backuptest.a78 testroms/include_01.a78
curl -sSL -o testroms/6502_functional_test.bin \
    https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files/6502_functional_test.bin
rm -rf "$TMP"
ls testroms
