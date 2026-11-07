#!/bin/bash
# build-wamrc.sh — build the xtensa-capable wamrc from pinned sources.
#
# The frozen toolchain recipe (see README "Toolchain"): WAMR-2.4.0's
# wamr-compiler linked against the espressif/llvm-project
# xtensa_release_18.1.2 LLVM. Prebuilt wamrc releases lack the xtensa
# backend, and official esp-clang prebuilts ship binaries only (no
# cmake config / headers / static libs), so the compiler is built from
# source (~55 min on a 4-core runner; cached / released afterward).
#
# Pins (override via environment):
#   WAMR_COMMIT          WAMR source           (tag WAMR-2.4.0)
#   XTENSA_LLVM_COMMIT   espressif/llvm-project (branch xtensa_release_18.1.2)
#
# Subcommands (the LLVM phase is the ~50min part; the toolchain
# workflow caches its tree between runs):
#   tools/build-wamrc.sh llvm   <install-dir>   # fetch sources + build LLVM
#   tools/build-wamrc.sh wamrc  <install-dir>   # build wamr-compiler -> <install-dir>/wamrc (needs the llvm phase)
#   tools/build-wamrc.sh all    <install-dir>   # llvm + wamrc
#
# Layout: scratch checkout at <install-dir>/../wamrc-src (LLVM tree at
# wamrc-src/wamr/core/deps/llvm/build).
set -euo pipefail

WAMR_COMMIT="${WAMR_COMMIT:-47c7c85ac21f240a878026af603e032065d183fb}"     # tag WAMR-2.4.0
XTENSA_LLVM_COMMIT="${XTENSA_LLVM_COMMIT:-344b928e67bfc1d07cb150609c55548e86a9bf19}"  # espressif xtensa_release_18.1.2

cmd="${1:-all}"; install_dir="${2:?usage: build-wamrc.sh [llvm|wamrc|all] <install-dir>}"
scratch="$(dirname "$install_dir")/wamrc-src"
mkdir -p "$install_dir" "$scratch"

fetch_sources() {
  echo "== fetch pinned sources (by SHA; shallow clones cannot check out non-tip commits) =="
  if [ ! -d "$scratch/wamr/wamr-compiler" ]; then
    git init "$scratch/wamr"
    git -C "$scratch/wamr" remote add origin https://github.com/bytecodealliance/wasm-micro-runtime.git
    git -C "$scratch/wamr" fetch --depth 1 origin "$WAMR_COMMIT"
    git -C "$scratch/wamr" checkout FETCH_HEAD
  fi
  if [ ! -d "$scratch/wamr/core/deps/llvm/llvm" ]; then
    git init "$scratch/wamr/core/deps/llvm"
    git -C "$scratch/wamr/core/deps/llvm" remote add origin https://github.com/espressif/llvm-project.git
    git -C "$scratch/wamr/core/deps/llvm" fetch --depth 1 origin "$XTENSA_LLVM_COMMIT"
    git -C "$scratch/wamr/core/deps/llvm" checkout FETCH_HEAD
  fi
}

phase_llvm() {
  fetch_sources
  if [ -x "$scratch/wamr/core/deps/llvm/build/bin/llvm-config" ]; then
    echo "== LLVM already built (cached tree) =="
    return 0
  fi
  echo "== deps =="
  sudo apt-get update -qq && sudo apt-get install -y -qq ninja-build ccache
  echo "== LLVM (Release; Xtensa is an EXPERIMENTAL target in the espressif fork) =="
  cmake -G Ninja -S "$scratch/wamr/core/deps/llvm/llvm" -B "$scratch/wamr/core/deps/llvm/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_TARGETS_TO_BUILD="AArch64;ARM;Mips;RISCV;X86" \
    -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=Xtensa \
    -DCMAKE_CXX_FLAGS="-include cstdint" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  ninja -C "$scratch/wamr/core/deps/llvm/build"
}

phase_wamrc() {
  fetch_sources
  llvm_build="$scratch/wamr/core/deps/llvm/build"
  [ -d "$llvm_build/lib/cmake/llvm" ] || { echo "LLVM tree missing — run the 'llvm' phase first" >&2; exit 1; }
  if [ -x "$install_dir/wamrc" ]; then
    echo "== wamrc already present =="
  else
    echo "== deps =="
    sudo apt-get update -qq && sudo apt-get install -y -qq ninja-build
    echo "== wamr-compiler =="
    cmake -G Ninja -S "$scratch/wamr/wamr-compiler" -B "$scratch/wamr/wamr-compiler/build" \
      -DCMAKE_BUILD_TYPE= \
      -DCMAKE_CXX_FLAGS="-include cstdint" \
      -DLLVM_DIR="$llvm_build/lib/cmake/llvm"
    ninja -C "$scratch/wamr/wamr-compiler/build"
    cp "$scratch/wamr/wamr-compiler/build/wamrc" "$install_dir/wamrc"
  fi
  assert_xtensa "$install_dir/wamrc"
  "$install_dir/wamrc" --version
  echo "build-wamrc: done -> $install_dir/wamrc"
}

# NOTE: `wamrc --target=help` prints the supported-target list and exits
# NONZERO (1) by design — the capture must swallow that exit code, the
# grep verdict alone decides pass/fail.
assert_xtensa() {
  local wamrc_bin="$1"
  local targets
  targets="$("$wamrc_bin" --target=help 2>&1 || true)"
  echo "$targets"
  echo "$targets" | grep -q "xtensa" || { echo "wamrc lacks the xtensa backend" >&2; exit 1; }
}

case "$cmd" in
  llvm) phase_llvm ;;
  wamrc) phase_wamrc ;;
  all) phase_llvm; phase_wamrc ;;
  *) echo "usage: build-wamrc.sh [llvm|wamrc|all] <install-dir>" >&2; exit 2 ;;
esac
