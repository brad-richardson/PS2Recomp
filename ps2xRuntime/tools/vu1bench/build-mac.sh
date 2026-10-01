#!/bin/bash
# VRB1: build the Mac vu1bench (standalone, no cmake).
# Usage: build-mac.sh <fork-worktree> <vu1gen-dir> <out-binary>
# Same Release recipe as the runner for the VU1 TUs: -O3 -DNDEBUG + SIMD FMAC.
set -euo pipefail
[ $# -eq 3 ] || { echo "usage: $0 <fork-worktree> <vu1gen-dir> <out>" >&2; exit 2; }
WT=$1; VU1=$2; OUT=$3
CXX=${CXX:-/opt/homebrew/opt/llvm/bin/clang++}
[ -x "$CXX" ] || CXX=clang++
NEON=${SSE2NEON:-$HOME/dev/ssx3-work/VRB1/sse2neon}
# Same ARM recipe as the root CMakeLists: sse2neon shim, E53 no-FP-contraction.
"$CXX" -std=c++20 -O3 -DNDEBUG -DPS2X_VU1_FMAC_SIMD=1 -DUSE_SSE2NEON -ffp-contract=off \
  -I "$NEON" \
  -I "$WT/ps2xRuntime/include" -I "$WT/ps2xRuntime/src/lib/vu" -I "$WT/ps2xRuntime/src/lib/Kernel" \
  "$WT/ps2xRuntime/src/lib/vu/ps2_vu_core.cpp" \
  "$WT/ps2xRuntime/src/lib/vu/ps2_vu0.cpp" \
  "$WT/ps2xRuntime/src/lib/vu/ps2_vu1.cpp" \
  "$WT/ps2xRuntime/src/lib/vu/ps2_vu_upper.cpp" \
  "$WT/ps2xRuntime/src/lib/vu/ps2_vu_lower.cpp" \
  "$WT/ps2xRuntime/src/lib/vu/ps2_vu_recomp.cpp" \
  "$VU1"/vu1_*.cpp \
  "$WT/ps2xRuntime/tools/vu1bench/vu1bench.cpp" \
  -o "$OUT"
ls -l "$OUT"
