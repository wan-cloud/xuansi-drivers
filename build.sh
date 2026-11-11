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
#   tools/build.sh [name ...]     build named modules (default: all)
#   tools/build.sh --check        rebuild into a scratch dir and compare
#                                 hashes against index.json / *.meta.json
#   tools/build.sh --next [n ..]  the B15 recarved-ABI-1 lane -> staging/
#                                 (default: the next-lane set; initial==max
#                                 memory — the ABI 1 freeze). staging/ is
#                                 git-ignored: NOT published until W3-T2.
#
# Layout: src/<name>/<name>.c + src/<name>/VERSION ->
#         modules/<name>/<ver>/<name>.{wasm,aot}        (default)
#         staging/<name>/<ver>/<name>.{wasm,aot}        (--next)
#
# The two lanes (B15 W2-T3): the committed artifacts are the LEGACY
# flag set (--max-memory=1048576) — CI must keep reproducing them
# byte-for-byte until the wave-3 republish flips the whole repo to the
# recarved ABI 1 (initial==max). --next builds ONLY the recarved lane,
# into the git-ignored staging/, for board verification ahead of the
# flip. The next-lane module set (st7789 migration + hclock) is
# excluded from the default build on purpose.
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
#  - memory: module-provided (initial per module / max 1MB / global-base 1024)
#  The ABI freeze's 256KB initial covers hello-scale modules; ledwatch's
#  CCL working set (QVGA frame + u16 label map + per-label arrays
#  ~627KB + the wasm-ld 64KB stack) pins 11 pages. Max stays the frozen
#  1MB ceiling.
init_mem_for() {  # per-module --initial-memory (64KB wasm pages)
    case "$1" in
        ledwatch) echo 720896 ;;   # 11 pages: 626.7KB arrays + 64KB stack + base
        jpge)     echo 917504 ;;   # 14 pages: VGA frame 614.4K + jpg out 128K
                                  # + mcu pool 30.7K + line 1.9K + heap/stack
                                  # (jpge@1.0.0 codec module; VGA-capable
                                  # static layout, see src/jpge/jpge.c)
        *)        echo 262144 ;;
    esac
}

# The B15 recarved-ABI-1 lane (staged, NOT published until W3-T2):
#   st7789 — the migration pilot (source declares the recarved 1; the
#            artifact difference is the memory section max == initial)
#   hclock — new entrant (the millis one-native module; src lands with
#            W2-T3, the published version lands W3-T2)
next_names() {
    echo st7789
    echo hclock
}

# --max-memory per lane: legacy committed artifacts pin the frozen 1MB
# ceiling (max > initial — the pre-recarve shape); the recarved ABI 1
# freezes max == initial (the static-footprint admission contract —
# the fw loader refuses initial != max with the bytes in the refusal).
max_mem_for() {  # max_mem_for <name> <lane:legacy|next>
    if [ "$2" = "next" ]; then
        init_mem_for "$1"
    else
        echo 1048576
    fi
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

build_one() {  # build_one <name> <out-dir> <lane:legacy|next>
    name="$1"; out="$2"; lane="$3"
    ver="$(tr -d ' \t\r\n' < "src/$name/VERSION")"
    case "$ver" in *[!a-z0-9.]*|'') echo "bad VERSION for $name: '$ver'" >&2; exit 1;; esac
    mkdir -p "$out/$name/$ver"
    echo "== $name@$ver: wasm ($lane lane: initial=$(init_mem_for "$name") max=$(max_mem_for "$name" "$lane")) =="
    if [ -f "src/$name/$name.cpp" ]; then
        # multi-TU module: the wasi-sdk clang driver compiles .cpp inputs
        # as C++ (dispatch by extension) but still links C-style (no C++
        # stdlib pulled in) — exactly what a no-runtime C++ module needs.
        # Currently jpge only.
        "$CC" $CFLAGS $CXXFLAGS_EXTRA \
            -Wl,--initial-memory="$(init_mem_for "$name")" \
            -Wl,--max-memory="$(max_mem_for "$name" "$lane")" \
            -o "$out/$name/$ver/$name.wasm" \
            "src/$name/$name.c" "src/$name/$name.cpp"
    else
        "$CC" $CFLAGS -Wl,--initial-memory="$(init_mem_for "$name")" \
            -Wl,--max-memory="$(max_mem_for "$name" "$lane")" \
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
        case " $(next_names | tr '\n' ' ')" in
            *" $n "*) ;;           # next-lane module: --next only
            *) echo "$n" ;;
        esac
    done
}

if [ "$1" = "--check" ]; then
    # Rebuild into a scratch dir and compare hashes against the committed
    # index.json (the files[] deliverables: .wasm AND, since P3-T12, the
    # toolchain-v3 .aot) and *.meta.json (the aot namespace cross-check +
    # historical auxiliary entries). The check always rebuilds the LEGACY
    # lane — it verifies the committed, published artifacts.
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT
    for n in $(all_names); do build_one "$n" "$scratch" legacy; done
    python3 tools/verify_index.py "$scratch" modules index.json
    echo "check: hashes reproduce"
    exit 0
fi

if [ "$1" = "--next" ]; then
    # The recarved-ABI-1 lane (B15 W2-T3): initial==max memory, into the
    # git-ignored staging/ — NOT published (index.json untouched). Named
    # args narrow the set; default = the full next-lane list.
    shift
    names="$*"
    [ -z "$names" ] && names="$(next_names)"
    for n in $names; do build_one "$n" staging next; done
    echo "done: staging/ (recarved ABI 1 lane — not published; W3-T2 promotes)"
    exit 0
fi

if [ "$#" -gt 0 ]; then
    for n in "$@"; do build_one "$n" modules legacy; done
else
    for n in $(all_names); do build_one "$n" modules legacy; done
fi
echo "done: modules/"
