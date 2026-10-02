#pragma once
// PL1: build id for the pad-recording header (build=<id>) and APK-side
// verification (padrec_replay_env.py scans libps2EntryRunner.so for the
// "PS2X-BUILD-ID:<id>" marker emitted by ps2_build_id.cpp).
//
// The id is "<12-hex fork SHA>" plus "-dirty" when the worktree had tracked
// modifications at configure time. It arrives as -DPS2X_BUILD_ID (the
// bradflix drivers pass the exported commit; gradle maps -Pps2xBuildId), or
// CMake detects it from git when the define is empty; archive exports and
// non-git trees read "unavailable".

namespace ps2x
{
// Out-of-line in ps2_build_id.cpp on purpose: the call pulls that TU out of
// the static archive, which carries the APK-scan marker with it.
const char *buildId();
} // namespace ps2x
