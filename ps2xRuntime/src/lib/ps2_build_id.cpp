// PL1: build-id accessor + the "PS2X-BUILD-ID:<id>" marker in rodata, so
// padrec_replay_env.py can read the build id out of an APK's
// libps2EntryRunner.so without running it. buildId() returns a pointer
// INTO the marker, so the string is genuinely referenced and survives
// --gc-sections/ThinLTO (a `used` static alone does not: PL1-apk2's .so
// lost it while the archive kept it).
#include "ps2_build_id.h"

#include <cstddef>

namespace
{
#if defined(PS2X_BUILD_ID)
const char kPs2xBuildIdMarker[] = "PS2X-BUILD-ID:" PS2X_BUILD_ID;
#else
const char kPs2xBuildIdMarker[] = "PS2X-BUILD-ID:";
#endif
constexpr std::size_t kMarkerPrefixLen = 14; // strlen("PS2X-BUILD-ID:")
} // namespace

namespace ps2x
{
const char *buildId()
{
    // The id is the marker's tail; a bare "PS2X-BUILD-ID:" (empty -D, no
    // git) reads as unavailable.
    return kPs2xBuildIdMarker[kMarkerPrefixLen] != '\0' ? kPs2xBuildIdMarker + kMarkerPrefixLen
                                                       : "unavailable";
}
} // namespace ps2x
