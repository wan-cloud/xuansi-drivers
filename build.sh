#!/bin/sh
# build.sh — build one or all driver modules (wasi-sdk -> wasm32; the
# toolchain-v3 surgery wamrc -> xtensa AOT, a first-class deliverable
# since P3-T12: .wasm + .aot are dual artifacts, both pinned in
# index.json files[] and each module's meta.json aot namespace).
#
# Toolchain pins (frozen; see README "Toolchain"):
#   wasi-sdk 24.0 (LLVM 18.1.2)
#     https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-24/wasi-sdk-24.0-x86_64-linux.tar.gz
#     sha256 c6c38aab56e5de88adf6c1ebc9c3ae8da72f88ec2b656fb024eda8d4167a0bc5
#   wamrc = the toolchain-v3 SURGERY wamrc: WAMR fork xuansi/base-2.4.0
#     built against wan-cloud/llvm-project xuansi/a3-surgery (kind
#     xuansi-surgery-wamrc). The default path is the xuansi_toolchain_fetch
#     cache layout (~/.cache/xuansi-wasm/wamrc/wamrc); set WAMRC to
#     override. A stock wamrc (toolchain-v2) does NOT reproduce the
#     committed .aot hashes — its containers are the internal-SRAM lane.
#
# Usage:
#   build.sh [name ...]     build named modules (default: all)
#   build.sh --check        rebuild into a scratch dir and compare
#                           hashes against index.json / *.meta.json
#
# Layout: src/<name>/<name>.c + src/<name>/VERSION ->
#         modules/<name>/<ver>/<name>.{wasm,aot}
#
# The ONE lane (since the B15 W3-T2 wave-3 flip): every artifact is
# recarved-ABI-1 shaped — initial == max linear memory (the ABI 1
# freeze; the fw 0.5.0 loader refuses initial != max with the bytes
# in the refusal). The pre-recarve era pinned --max-memory=1048576
# (max > initial); those artifacts were purged from the tree and the
# index in the same flip — git history keeps them, the loader
# honestly refuses them, nothing silently falls back.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

CACHE="${XUANSI_WASM_CACHE:-$HOME/.cache/xuansi-wasm}"
WASI_SDK="${WASI_SDK:-$CACHE/wasi-sdk-24.0-x86_64-linux}"
WAMRC="${WAMRC:-$CACHE/wamrc/wamrc}"   # toolchain-v3 surgery wamrc
                                              # (xuansi_toolchain_fetch layout)

CC="$WASI_SDK/bin/clang"
SYSROOT="$WASI_SDK/share/wasi-sysroot"

# Module build flags (frozen with the ABI):
#  - ref-types encodings -> the board enables WAMR ref-types
#  - -fno-jump-tables     -> avoids br_table jump-table encodings
#  - memory: module-provided (initial per module / max == initial —
#    the recarved ABI 1 freeze / global-base 1024)
#  The freeze's 256KB initial covers hello-scale modules; ledwatch's
#  CCL working set (QVGA frame + u16 label map + per-label arrays
#  ~627KB + the wasm-ld 64KB stack) pins 11 pages; jpge's VGA-capable
#  static layout pins 14. max == initial everywhere (the admission
#  contract: the static footprint is the whole account).
init_mem_for() {  # per-module --initial-memory (64KB wasm pages)
    case "$1" in
        ledwatch) echo 720896 ;;   # 11 pages: 626.7KB arrays + 64KB stack + base
        jpge)     echo 917504 ;;   # 14 pages: VGA frame 614.4K + jpg out 128K
                                  # + mcu pool 30.7K + line 1.9K + heap/stack
                                  # (jpge codec module; VGA-capable static
                                  # layout, see src/jpge/jpge.c)
        *)        echo 262144 ;;
    esac
}

CFLAGS="--target=wasm32-wasi --sysroot=$SYSROOT -O2 -fno-jump-tables \
 -nostdlib -Wl,--no-entry -Wl,--allow-undefined \
 -Wl,--global-base=1024 -Wl,--strip-debug -Wl,--export-dynamic \
 -I src -Wall -Wextra -Wno-unused-parameter"

# C++ module flags: the ONE sanctioned deviation from the frozen C flag
# set (B12-P0 jpge — vendored C++ encoder core; the module uses no C++
# runtime facility: no RTTI, no exceptions, no new/delete at runtime, no
# global ctors — only classes/vtables, which are pure codegen, plus the
# minimal operator new/delete shims the deleting-dtor symbol references
# want at LINK time). -fvisibility=hidden keeps those shims and every
# other C++-TU symbol out of the wasm export face (the module face comes
# from the C TU's export_name attributes). Everything else mirrors
# CFLAGS verbatim.
CXXFLAGS_EXTRA="-fno-rtti -fno-exceptions -fvisibility=hidden"

build_one() {  # build_one <name> <out-dir>
    name="$1"; out="$2"
    ver="$(tr -d ' \t\r\n' < "src/$name/VERSION")"
    case "$ver" in *[!a-z0-9.]*|'') echo "bad VERSION for $name: '$ver'" >&2; exit 1;; esac
    mkdir -p "$out/$name/$ver"
    mem="$(init_mem_for "$name")"
    echo "== $name@$ver: wasm (recarved ABI 1 lane: initial=$mem max=$mem) =="
    if [ -f "src/$name/$name.cpp" ]; then
        # multi-TU module: the wasi-sdk clang driver compiles .cpp inputs
        # as C++ (dispatch by extension) but still links C-style (no C++
        # stdlib pulled in) — exactly what a no-runtime C++ module needs.
        # Currently jpge only.
        "$CC" $CFLAGS $CXXFLAGS_EXTRA \
            -Wl,--initial-memory="$mem" -Wl,--max-memory="$mem" \
            -o "$out/$name/$ver/$name.wasm" \
            "src/$name/$name.c" "src/$name/$name.cpp"
    else
        "$CC" $CFLAGS -Wl,--initial-memory="$mem" -Wl,--max-memory="$mem" \
            -o "$out/$name/$ver/$name.wasm" "src/$name/$name.c"
    fi
    echo "   $(wc -c < "$out/$name/$ver/$name.wasm") bytes $name.wasm  sha256 $(sha256sum "$out/$name/$ver/$name.wasm" | cut -d' ' -f1)"
    if [ -x "$WAMRC" ]; then
        echo "== $name@$ver: aot (deliverable, toolchain-v3 surgery) =="
        "$WAMRC" --target=xtensa --cpu=esp32s3 --size-level=3 \
                 -o "$out/$name/$ver/$name.aot" "$out/$name/$ver/$name.wasm" >/dev/null
        echo "   $(wc -c < "$out/$name/$ver/$name.aot") bytes $name.aot  sha256 $(sha256sum "$out/$name/$ver/$name.aot" | cut -d' ' -f1)"
    else
        echo "== $name@$ver: wamrc not found at $WAMRC — skipping .aot (deliverable; index files[] pins it — CI is the reproduction gate) =="
    fi
}

all_names() {
    for d in src/*/; do
        n="$(basename "$d")"
        [ -f "src/$n/$n.c" ] || continue
        echo "$n"
    done
}

if [ "$1" = "--check" ]; then
    # Rebuild into a scratch dir and compare hashes against the committed
    # index.json (the files[] deliverables: .wasm AND the toolchain-v3
    # .aot) and *.meta.json (the aot namespace cross-check). The check
    # rebuilds THE lane — the recarved ABI 1 (initial == max) shape the
    # wave-3 republish committed; it verifies the published artifacts.
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
