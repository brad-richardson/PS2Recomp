// PL1: build-id accessor + the "PS2X-BUILD-ID:<id>" marker in rodata, so
// padrec_replay_env.py can read the build id out of an APK's
// libps2EntryRunner.so without running it. `used` keeps the unreferenced
// string under --gc-sections; stripping keeps rodata literals.
#include "ps2_build_id.h"

#if defined(__clang__) || defined(__GNUC__)
#define PS2X_BUILD_ID_USED __attribute__((used))
#else
#define PS2X_BUILD_ID_USED
#endif

namespace
{
// "" + macro: defined-but-empty (-DPS2X_BUILD_ID=) still concatenates.
#if defined(PS2X_BUILD_ID)
PS2X_BUILD_ID_USED const char kPs2xBuildIdMarker[] = "PS2X-BUILD-ID:" PS2X_BUILD_ID;
const char kPs2xBuildId[] = "" PS2X_BUILD_ID;
#else
PS2X_BUILD_ID_USED const char kPs2xBuildIdMarker[] = "PS2X-BUILD-ID:unavailable";
const char kPs2xBuildId[] = "";
#endif
} // namespace

namespace ps2x
{
const char *buildId()
{
    return kPs2xBuildId[0] != '\0' ? kPs2xBuildId : "unavailable";
}
} // namespace ps2x
