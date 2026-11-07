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
# Usage: tools/build-wamrc.sh <install-dir>   # wamrc lands at <install-dir>/wamrc
# Work happens in a scratch checkout under <install-dir>/../wamrc-src.
set -euo pipefail

WAMR_COMMIT="${WAMR_COMMIT:-47c7c85ac21f240a878026af603e032065d183fb}"     # tag WAMR-2.4.0
XTENSA_LLVM_COMMIT="${XTENSA_LLVM_COMMIT:-344b928e67bfc1d07cb150609c55548e86a9bf19}"  # espressif xtensa_release_18.1.2

install_dir="$1"
scratch="$(dirname "$install_dir")/wamrc-src"
mkdir -p "$install_dir" "$scratch"

echo "== deps =="
sudo apt-get update -qq && sudo apt-get install -y -qq ninja-build ccache

echo "== fetch pinned sources (by SHA; shallow clones cannot check out non-tip commits) =="
git init "$scratch/wamr"
git -C "$scratch/wamr" remote add origin https://github.com/bytecodealliance/wasm-micro-runtime.git
git -C "$scratch/wamr" fetch --depth 1 origin "$WAMR_COMMIT"
git -C "$scratch/wamr" checkout FETCH_HEAD
git init "$scratch/wamr/core/deps/llvm"
git -C "$scratch/wamr/core/deps/llvm" remote add origin https://github.com/espressif/llvm-project.git
git -C "$scratch/wamr/core/deps/llvm" fetch --depth 1 origin "$XTENSA_LLVM_COMMIT"
git -C "$scratch/wamr/core/deps/llvm" checkout FETCH_HEAD

echo "== LLVM (Release; Xtensa is an EXPERIMENTAL target in the espressif fork) =="
cmake -G Ninja -S "$scratch/wamr/core/deps/llvm/llvm" -B "$scratch/wamr/core/deps/llvm/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD="AArch64;ARM;Mips;RISCV;X86" \
  -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=Xtensa \
  -DCMAKE_CXX_FLAGS="-include cstdint" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
ninja -C "$scratch/wamr/core/deps/llvm/build"

echo "== wamr-compiler =="
cmake -G Ninja -S "$scratch/wamr/wamr-compiler" -B "$scratch/wamr/wamr-compiler/build" \
  -DCMAKE_BUILD_TYPE= \
  -DCMAKE_CXX_FLAGS="-include cstdint" \
  -DLLVM_DIR="$scratch/wamr/core/deps/llvm/build/lib/cmake/llvm"
ninja -C "$scratch/wamr/wamr-compiler/build"

echo "== install + assert the xtensa backend =="
cp "$scratch/wamr/wamr-compiler/build/wamrc" "$install_dir/wamrc"
targets="$("$install_dir/wamrc" --target=help 2>&1)"
echo "$targets"
echo "$targets" | grep -q "xtensa" || { echo "wamrc lacks the xtensa backend" >&2; exit 1; }
"$install_dir/wamrc" --version
echo "build-wamrc: done -> $install_dir/wamrc"
