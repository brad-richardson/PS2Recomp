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

bool readWholeFile(const std::string &path, std::string *out)
{
    std::ifstream file(path.c_str());
    if (!file.is_open())
    {
        return false;
    }
    std::ostringstream content;
    content << file.rdbuf();
    *out = content.str();
    return true;
}

void setEnvEntries(const std::string &content, const char *layer)
{
    for (const auto &entry : ps2x::parseEnvFileContent(content))
    {
        setenv(entry.first.c_str(), entry.second.c_str(), 1);
        __android_log_write(ANDROID_LOG_INFO, kPs2xLogTag, (std::string(layer) + ": set " + entry.first).c_str());
    }
}

void loadPs2xEnvFile()
{
#if defined(PS2X_DEFAULT_BOOT_ELF)
    const std::string path = ps2x::envFilePathForBootElf(PS2X_DEFAULT_BOOT_ELF);
    std::string base;
    const bool haveBase = readWholeFile(path, &base);
    if (!haveBase)
    {
        __android_log_write(ANDROID_LOG_INFO, kPs2xLogTag, ("ps2x.env: not found at " + path).c_str());
    }
    else
    {
        setEnvEntries(base, "ps2x.env");
    }
    // TG1: the "SSX 3 · 120" launcher entry (LaunchActivity) sets
    // PS2X_LAUNCH_HZ=120 before this library loads; full120.env then layers
    // over ps2x.env (base < full120 < nothing else). The 60 entry sets
    // nothing, so the 60 launch reads exactly today's env.
    // TKO1: the "SSX 3 · Tricky" entry sets PS2X_LAUNCH_LAYER=tricky instead;
    // tricky.env layers over ps2x.env the same way (base < tricky), at 60.
    const bool full120 = ps2x::launchHzSelectsFull120(std::getenv("PS2X_LAUNCH_HZ"));
    const bool tricky = !full120 && ps2x::launchLayerSelectsTricky(std::getenv("PS2X_LAUNCH_LAYER"));
    if (!full120 && !tricky)
    {
        __android_log_write(ANDROID_LOG_INFO, kPs2xLogTag, "ps2x.env: launch 60 (base env only)");
        // PL1: stash the raw bytes' hash for the padrec header's env_sha.
        if (haveBase)
        {
            ps2x::setRecordedEnvFileHash(ps2x::fnv1a64Hex(base));
        }
        return;
    }
    const char *layerName = full120 ? "full120.env" : "tricky.env";
    const char *launchName = full120 ? "launch 120" : "launch tricky";
    const std::string presetPath = full120 ? ps2x::full120EnvFilePathForBootElf(PS2X_DEFAULT_BOOT_ELF)
                                           : ps2x::trickyEnvFilePathForBootElf(PS2X_DEFAULT_BOOT_ELF);
    std::string preset;
    if (!readWholeFile(presetPath, &preset))
    {
        __android_log_write(ANDROID_LOG_WARN, kPs2xLogTag,
                            (std::string("ps2x.env: ") + launchName + " but " + layerName + " not found at " +
                             presetPath).c_str());
        if (haveBase)
        {
            ps2x::setRecordedEnvFileHash(ps2x::fnv1a64Hex(base));
        }
        return;
    }
    __android_log_write(ANDROID_LOG_INFO, kPs2xLogTag,
                        (std::string("ps2x.env: ") + launchName + " -> layering " + presetPath).c_str());
    setEnvEntries(preset, layerName);
    // PL1 + TG1: both layers, NUL-separated (as iOS IQ1), so a padrec
    // header tells a layered launch from a 60 one.
    ps2x::setRecordedEnvFileHash(ps2x::fnv1a64Hex(base + '\0' + preset));
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
