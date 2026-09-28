// TODO: on-screen touch joystick / button overlay.
#if defined(__ANDROID__)

#include "ps2_android_env.h"
#include "ps2_knobs.h"
#include "ps2_record_env.h"

#include <android/log.h>
#include <cstdio>
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
    // PL1: stash the raw bytes' hash for the padrec header's env_sha.
    ps2x::setRecordedEnvFileHash(ps2x::fnv1a64Hex(content.str()));
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
    Ps2xEnvLoader()
    {
        loadPs2xEnvFile();
        // CF2 S1: compiled P3 play defaults for keys the file did not set
        // (runs even when the file is missing: a missing/empty env must
        // reproduce P3). PS2X_PROFILE=reference disables them all, restoring
        // the exact knob-off path.
        if (ps2x::cf2ProfileIsReference())
        {
            __android_log_write(ANDROID_LOG_INFO, kPs2xLogTag, "ps2x.env: profile=reference, no compiled defaults");
            return;
        }
#if defined(PS2X_DEFAULT_BOOT_ELF)
        const size_t applied = ps2x::cf2ApplyAndroidDefaults(PS2X_DEFAULT_BOOT_ELF);
#else
        const size_t applied = ps2x::cf2ApplyAndroidDefaults(nullptr);
#endif
        char summary[64];
        std::snprintf(summary, sizeof(summary), "ps2x.env: %u compiled defaults applied",
                      static_cast<unsigned>(applied));
        __android_log_write(ANDROID_LOG_INFO, kPs2xLogTag, summary);
    }
};

static Ps2xEnvLoader g_ps2xEnvLoader;
} // namespace

#endif // __ANDROID__
