// TODO: on-screen touch joystick / button overlay.
#if defined(__ANDROID__)

#include "ps2_android_env.h"

#include <android/log.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

// N-lane env-file shim (Android only): runs as a static initializer, before
// main() reads anything. Reads <files dir>/ps2x.env (KEY=VALUE lines, '#'
// comments) and calls setenv for each entry, logging every key it sets to
// logcat under tag "ps2x". The files dir is the directory
// PS2X_DEFAULT_BOOT_ELF points into. Absent file = nothing to set.
namespace
{
constexpr const char *kPs2xLogTag = "ps2x";

void loadPs2xEnvFile()
{
#if defined(PS2X_DEFAULT_BOOT_ELF)
    const std::string path = ps2x::envFilePathForBootElf(PS2X_DEFAULT_BOOT_ELF);
    std::ifstream file(path.c_str());
    if (!file.is_open())
    {
        __android_log_write(ANDROID_LOG_INFO, kPs2xLogTag, ("ps2x.env: not found at " + path).c_str());
        return;
    }
    std::ostringstream content;
    content << file.rdbuf();
    for (const auto &entry : ps2x::parseEnvFileContent(content.str()))
    {
        setenv(entry.first.c_str(), entry.second.c_str(), 1);
        __android_log_write(ANDROID_LOG_INFO, kPs2xLogTag, ("ps2x.env: set " + entry.first).c_str());
    }
#else
    __android_log_write(ANDROID_LOG_INFO, kPs2xLogTag, "ps2x.env: no PS2X_DEFAULT_BOOT_ELF; skipped");
#endif
}

struct Ps2xEnvLoader
{
    Ps2xEnvLoader() { loadPs2xEnvFile(); }
};

static Ps2xEnvLoader g_ps2xEnvLoader;
} // namespace

#endif // __ANDROID__
