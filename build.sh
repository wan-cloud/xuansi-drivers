#!/bin/sh
# build.sh — build one or all driver modules (wasi-sdk -> wasm32; wamrc ->
# xtensa AOT as an auxiliary artifact).
#
# Toolchain pins (frozen; see README "Toolchain"):
#   wasi-sdk 24.0 (LLVM 18.1.2)
#     https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-24/wasi-sdk-24.0-x86_64-linux.tar.gz
#     sha256 c6c38aab56e5de88adf6c1ebc9c3ae8da72f88ec2b656fb024eda8d4167a0bc5
#   wamrc WAMR-2.4.0 (source-built with the espressif/llvm-project
#     xtensa_release_18.1.2 backend; prebuilt releases lack xtensa)
#
# Usage:
#   tools/build.sh [name ...]     build named modules (default: all)
#   tools/build.sh --check        rebuild into a scratch dir and compare
#                                 hashes against index.json / *.meta.json
#
# Layout: src/<name>/<name>.c + src/<name>/VERSION ->
#         modules/<name>/<ver>/<name>.{wasm,aot}
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

CACHE="${XUANSI_WASM_CACHE:-$HOME/.cache/xuansi-wasm}"
WASI_SDK="${WASI_SDK:-$CACHE/wasi-sdk-24.0-x86_64-linux}"
WAMRC="${WAMRC:-$CACHE/wamr/wamr-compiler/build/wamrc}"

CC="$WASI_SDK/bin/clang"
SYSROOT="$WASI_SDK/share/wasi-sysroot"

# Module build flags (frozen with the ABI):
#  - ref-types encodings -> the board enables WAMR ref-types
#  - -fno-jump-tables     -> avoids br_table jump-table encodings
#  - memory: module-provided (initial 256KB / max 1MB / global-base 1024)
CFLAGS="--target=wasm32-wasi --sysroot=$SYSROOT -O2 -fno-jump-tables \
 -nostdlib -Wl,--no-entry -Wl,--allow-undefined \
 -Wl,--initial-memory=262144 -Wl,--max-memory=1048576 \
 -Wl,--global-base=1024 -Wl,--strip-debug -Wl,--export-dynamic \
 -I src -Wall -Wextra -Wno-unused-parameter"

build_one() {  # build_one <name> <out-dir>
    name="$1"; out="$2"
    ver="$(tr -d ' \t\r\n' < "src/$name/VERSION")"
    case "$ver" in *[!a-z0-9.]*|'') echo "bad VERSION for $name: '$ver'" >&2; exit 1;; esac
    mkdir -p "$out/$name/$ver"
    echo "== $name@$ver: wasm =="
    "$CC" $CFLAGS -o "$out/$name/$ver/$name.wasm" "src/$name/$name.c"
    echo "   $(wc -c < "$out/$name/$ver/$name.wasm") bytes $name.wasm  sha256 $(sha256sum "$out/$name/$ver/$name.wasm" | cut -d' ' -f1)"
    if [ -x "$WAMRC" ]; then
        echo "== $name@$ver: aot (auxiliary) =="
        "$WAMRC" --target=xtensa --cpu=esp32s3 --size-level=3 \
                 -o "$out/$name/$ver/$name.aot" "$out/$name/$ver/$name.wasm" >/dev/null
        echo "   $(wc -c < "$out/$name/$ver/$name.aot") bytes $name.aot  sha256 $(sha256sum "$out/$name/$ver/$name.aot" | cut -d' ' -f1)"
    else
        echo "== $name@$ver: wamrc not found at $WAMRC — skipping .aot (auxiliary) =="
    fi
}

all_names() {
    for d in src/*/; do
        n="$(basename "$d")"
        [ -f "src/$n/$n.c" ] && echo "$n"
    done
}

if [ "$1" = "--check" ]; then
    # Rebuild into a scratch dir and compare hashes against the committed
    # index.json (deliverable files[]) and *.meta.json (auxiliary .aot).
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT
    for n in $(all_names); do build_one "$n" "$scratch"; done
    python3 tools/verify_index.py "$scratch" modules index.json
    echo "check: hashes reproduce"
    exit 0
fi

if [ "$#" -gt 0 ]; then
    for n in "$@"; do build_one "$n" modules; done
else
    for n in $(all_names); do build_one "$n" modules; done
fi
echo "done: modules/"
