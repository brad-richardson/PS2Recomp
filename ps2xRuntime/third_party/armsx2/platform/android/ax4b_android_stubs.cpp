// AX4b: upstream (e23b6a55) ChdFileReader calls
// FileSystem::FindContentChdSiblings; on Android the only in-tree definition
// lives behind ENABLE_LIBRETRO (pcsx2/Android/AndroidStubs.cpp) while the
// APK's JNI layer (platforms/android native-lib.cpp) provides it for the app
// build. The GE1 lib has no JNI layer, and ENABLE_LIBRETRO would reconfigure
// PCSX2 as an OBJECT library, so stub it here exactly like the libretro core
// does (no siblings) — which is the 2.7.2 behavior this carry was validated
// against. Signature mirrors common/FileSystem.h.
#include <string>
#include <vector>

namespace FileSystem
{
std::vector<std::string> FindContentChdSiblings(const char* filename);
}

std::vector<std::string> FileSystem::FindContentChdSiblings(const char* /*filename*/)
{
	return {};
}
