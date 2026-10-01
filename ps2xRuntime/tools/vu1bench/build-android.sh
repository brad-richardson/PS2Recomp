#!/bin/bash
# VRB1: build the Android arm64 vu1bench (adb-shell binary, no APK).
# Usage: build-android.sh <fork-worktree> <vu1gen-dir> <out-binary>
# NDK: $NDK (default /opt/android-sdk/ndk/28.2.13676358 in ssx3-android) or $CXX.
set -euo pipefail
[ $# -eq 3 ] || { echo "usage: $0 <fork-worktree> <vu1gen-dir> <out>" >&2; exit 2; }
WT=$1; VU1=$2; OUT=$3
NDK=${NDK:-/opt/android-sdk/ndk/28.2.13676358}
CXX=${CXX:-$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang++}
NEON=${SSE2NEON:-$HOME/dev/ssx3-work/VRB1/sse2neon}
# Same recipe as the root CMakeLists (ARM64 non-Apple: crypto+crc when supported).
"$CXX" -std=c++20 -O3 -DNDEBUG -DPS2X_VU1_FMAC_SIMD=1 -DUSE_SSE2NEON -ffp-contract=off \
  -march=armv8-a+fp+simd+crypto+crc -static-libstdc++ \
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
