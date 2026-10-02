#!/bin/bash
# AX4b (SP02): one script builds libge1_gs + libmv2_microvu from one ARMSX2
# SHA on every platform. Unifies microvu_libs.sh, bradflix_ge1_lib.sh, the
# GE1S Mac recipe and the GI1/IG1 iOS recipes; only this file's location
# changed (PS2Recomp ps2xRuntime/third_party/armsx2, AX4 §2.2).
#
# Usage: build.sh <mac|android|ios|ios-sim> --armsx2 <dir> [--build-root <dir>] [--jobs N]
#   mac:      libge1_gs.dylib + ge1_replay (Metal + MoltenVK) + libmv2_microvu.dylib
#             (om1rt recipe, pinned OM1RT_* inputs) via platform/mac.
#   android:  assembles a fork-shaped srcdir (vendor = --armsx2 HEAD, adapter +
#             platform from this repo's HEAD) and runs bradflix_ge1_lib.sh
#             <srcdir> ax4b-<sha12> on bradflix (Docker ssx3-android, memory =
#             memory-swap, oom-score-adj 500, shared ccache). Produces the
#             canonical ge1lib-* + pull; MV2/OM1 subdirs are NOT staged by that
#             recipe, so platform/android skips them behind EXISTS/DEFINED guards.
#   ios:      static ge1 (device, iphoneos) via platform/ios (IG1 recipe).
#   ios-sim:  static ge1 (simulator, iphonesimulator) via platform/ios (IG1 recipe).
#             mv2 is not wired on iOS: mv2-adapter links shaderc_combined, which
#             does not exist on iOS (the app runs VU1 from the OM1 offline stage).
# Inputs (mini): ~/dev/ssx3-work/pinned (PI1; pinned.py verify) for OM1RT_*,
#   mac ge1-deps and the GE1 android-platform bundle seed (iOS deps).
# Scratch: fresh dirs under --build-root (default ~/dev/ssx3-work/AX4b/build).
set -euo pipefail

PLAT=${1:-}; shift || true
ARMSX2=""; BUILD_ROOT="$HOME/dev/ssx3-work/AX4b/build"; JOBS=8
while [ $# -gt 0 ]; do
  case "$1" in
    --armsx2) ARMSX2=$2; shift 2;;
    --build-root) BUILD_ROOT=$2; shift 2;;
    --jobs) JOBS=$2; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
case "$PLAT" in mac|android|ios|ios-sim) ;; *) echo "usage: build.sh <mac|android|ios|ios-sim> --armsx2 <dir> [--build-root <dir>] [--jobs N]" >&2; exit 2;; esac
[ -n "$ARMSX2" ] || { echo "missing --armsx2 <ARMSX2 worktree>" >&2; exit 2; }
[ -d "$ARMSX2/pcsx2" ] || { echo "not an ARMSX2 tree: $ARMSX2" >&2; exit 2; }
ARMSX2_SHA=$(git -C "$ARMSX2" rev-parse HEAD) || { echo "not a git checkout: $ARMSX2" >&2; exit 2; }
[ -n "$(git -C "$ARMSX2" status --short)" ] && { echo "refusing dirty ARMSX2 tree: $ARMSX2" >&2; exit 2; }

HERE=$(cd "$(dirname "$0")" && pwd)
# The adapter/platform content must be the committed repo content (byte-proof).
[ -n "$(git -C "$HERE" status --short -- .)" ] && { echo "refusing dirty $HERE (commit SP moves first)" >&2; exit 2; }
REPO_SHA=$(git -C "$HERE" rev-parse HEAD)

PIN=$HOME/dev/ssx3-work/pinned
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
sha2() { shasum -a 256 "$1"; shasum -a 256 "$1"; }

case "$PLAT" in
  mac)
    for d in "$PIN/microvu/om1rt-inputs" "$PIN/mac/ge1-deps/lib/cmake"; do
      [ -d "$d" ] || { echo "missing pinned input $d" >&2; exit 2; }
    done
    ASM=$BUILD_ROOT/mac-asm-$STAMP; BLD=$BUILD_ROOT/mac-build-$STAMP
    [ -e "$ASM" ] || [ -e "$BLD" ] && { echo "refusing to reuse $ASM or $BLD" >&2; exit 2; }
    mkdir -p "$ASM" "$BLD"
    ln -s "$ARMSX2" "$ASM/vendor-pcsx2"
    ln -s "$HERE/ge1" "$ASM/adapter"
    ln -s "$HERE/om1/rt" "$ASM/om1rt-adapter"
    ln -s "$HERE/om1/record" "$ASM/om1-adapter"
    ln -s "$ARMSX2/pcsx2" "$ASM/pcsx2"
    ln -s "$ARMSX2/tests" "$ASM/tests"
    ln -s "$ARMSX2/3rdparty" "$ASM/3rdparty"
    ln -s "$ARMSX2/common" "$ASM/common"
    ln -s "$ARMSX2/cmake" "$ASM/cmake"
    cp -a "$HERE/platform/mac" "$ASM/mac-platform"
    cmake -S "$ASM/mac-platform" -B "$BLD" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -Dplutovg_DIR=$PIN/mac/ge1-deps/lib/cmake/plutovg \
      -Dplutosvg_DIR=$PIN/mac/ge1-deps/lib/cmake/plutosvg \
      -DOM1RT_WORKTREE=$PIN/microvu/om1rt-inputs/ps2recomp-4f1a953 -DOM1RT_SRC=$PIN/microvu/om1rt-inputs/rt-src \
      -DOM1_REC_DIR=$PIN/microvu/om1rt-inputs -DOM1_WORKTREE=$PIN/microvu/om1rt-inputs/ps2recomp-4f1a953 \
      -DOM1_CAPTURE_INCLUDE=$PIN/microvu/om1rt-inputs/capture-include
    cmake --build "$BLD" --target ge1_gs mv2_microvu ge1_replay -j "$JOBS"
    echo "== libs (armsx2=$ARMSX2_SHA repo=$REPO_SHA)"
    sha2 "$BLD/ge1-adapter/libge1_gs.dylib"
    sha2 "$BLD/om1rt-adapter/libmv2_microvu.dylib"
    echo "BUILD_DIR=$BLD"
    ;;
  android)
    command -v ssh >/dev/null && ssh -o ConnectTimeout=10 bradflix 'echo ok' >/dev/null || { echo "bradflix unreachable" >&2; exit 2; }
    SRC=$BUILD_ROOT/android-src-$STAMP
    [ -e "$SRC" ] && { echo "refusing to reuse $SRC" >&2; exit 2; }
    mkdir -p "$SRC/ssx3-ge1"
    git -C "$ARMSX2" archive HEAD | tar -x -C "$SRC"
    mkdir -p "$SRC/ssx3-ge1/android-platform"
    cp -a "$HERE/ge1" "$SRC/ssx3-ge1/adapter"
    cp "$HERE/platform/android/CMakeLists.txt" "$SRC/ssx3-ge1/android-platform/CMakeLists.txt"
    [ -f "$SRC/pcsx2/GS/GSState.cpp" ] && [ -f "$SRC/ssx3-ge1/adapter/ge1_gs.cpp" ] && [ -f "$SRC/ssx3-ge1/android-platform/CMakeLists.txt" ] \
      || { echo "srcdir assembly failed" >&2; exit 2; }
    S12=$(git -C "$ARMSX2" rev-parse --short=12 HEAD)
    bash ~/dev/ssx3/local/tooling/build/bradflix_ge1_lib.sh "$SRC" "ax4b-$S12"
    ;;
  ios|ios-sim)
    if [ "$PLAT" = "ios" ]; then SYSROOT=iphoneos; else SYSROOT=iphonesimulator; fi
    BUNDLE=$HOME/dev/ssx3-work/GE1/android-platform/3rdparty
    [ -d "$BUNDLE" ] || { echo "missing GE1 bundle: $BUNDLE" >&2; exit 2; }
    ASM=$BUILD_ROOT/ios-asm-$STAMP; BLD=$BUILD_ROOT/ios-build-$STAMP; DEPS=$BUILD_ROOT/ios-deps-$STAMP
    [ -e "$ASM" ] || [ -e "$BLD" ] || [ -e "$DEPS" ] && { echo "refusing to reuse $ASM/$BLD/$DEPS" >&2; exit 2; }
    mkdir -p "$ASM" "$BLD" "$DEPS"
    for d in zstd freetype sdl3 libjpeg-turbo libpng libwebp lz4 rapidyaml plutovg plutosvg; do
      [ -e "$BUNDLE/$d" ] || { echo "missing bundle dep $d" >&2; exit 2; }
      ln -s "$BUNDLE/$d" "$DEPS/$d"
    done
    ln -s "$ARMSX2" "$ASM/vendor-pcsx2"
    ln -s "$HERE/ge1" "$ASM/adapter"
    cp -a "$HERE/platform/ios" "$ASM/ios-platform"
    env -u CC -u CXX -u OBJC -u OBJCXX cmake -S "$ASM/ios-platform" -B "$BLD" -G Ninja \
      -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$SYSROOT \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$(xcrun -f clang)" -DCMAKE_CXX_COMPILER="$(xcrun -f clang++)" \
      -DGE1_IOS_DEPS="$DEPS"
    cmake --build "$BLD" --target ge1_gs -j "$JOBS"
    echo "== libs (armsx2=$ARMSX2_SHA repo=$REPO_SHA)"
    sha2 "$BLD/ge1-adapter/libge1_gs.a"
    echo "BUILD_DIR=$BLD"
    ;;
esac
