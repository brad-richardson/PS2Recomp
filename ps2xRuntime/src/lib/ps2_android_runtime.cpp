// TODO: on-screen touch joystick / button overlay.
#if defined(__ANDROID__)

#include "ps2_android_env.h"
#include "ps2_diag_gate.h"
#include "ps2_knobs.h"
#include "ps2_login_paths.h"
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
    const auto parsed = ps2x::parseEnvFileContent(content);
    // F4 (appsec-fix-1): public builds ignore diag-class file knobs unless
    // PS2X_DEV=1 (process env or this same file). One log line names them.
    const bool devOpen = ps2x::diaggate::devModeOpenIn(parsed);
    std::vector<std::pair<std::string, std::string>> kept;
    std::vector<std::string> ignored;
    ps2x::diaggate::splitDiagEntries(parsed, devOpen, &kept, &ignored);
    if (!ignored.empty())
    {
        __android_log_write(ANDROID_LOG_INFO, kPs2xLogTag,
                            (std::string(layer) + ": ignored diag knobs (PS2X_DEV=1 to allow): " +
                             ps2x::diaggate::joinIgnored(ignored))
                                .c_str());
    }
    for (const auto &entry : kept)
    {
        // The Application prepares the bundled pack before NativeActivity
        // loads this library. Explicit filesystem paths still pass through.
        const char *appPack = std::getenv("PS2X_APP_PACK");
        std::string value = entry.second;
        const std::string token = "${APP_PACK}";
        const std::string replacement = appPack ? appPack : "";
        size_t pos = 0;
        while ((pos = value.find(token, pos)) != std::string::npos)
        {
            value.replace(pos, token.size(), replacement);
            pos += replacement.size();
        }
        setenv(entry.first.c_str(), value.c_str(), 1);
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

void loadSettingsEnvFile(bool explicitInputMarker)
{
    // SET2: the user settings layer (<files dir>/settings.env, written by
    // the in-game panel) reads last, over every layer above. Allowlisted to
    // the keys the panel owns, so a hand-edited file can't brick the boot.
    const std::string path = ps2x::settingsEnvFilePath();
    if (!path.empty())
    {
        std::string content;
        if (readWholeFile(path, &content))
        {
            for (const auto &entry : ps2x::parseEnvFileContent(content))
            {
                // android-touch-1: Touch row persists under its env name.
                // settings-res-1: Menu button row persists under its env name;
                // Upscale row too (same iPhone-bug class as the iOS mirror:
                // without this the panel's Upscale choice never survives a
                // restart on Android either).
                // shortcuts-1: Input marker row persists under its env name;
                // an explicit PS2X_INPUT_MARKER (lab use) wins over the file,
                // mirroring the iOS launcherKeys carve-out.
                if (entry.first != "GE1_UPSCALE" && entry.first != "PS2X_SETTINGS_PERF" &&
                    entry.first != "PS2X_VIRTUAL_PAD" && entry.first != "PS2X_MENU_BUTTON" &&
                    entry.first != "PS2X_INPUT_MARKER" && entry.first != "GE1_ONDEVICE_UPSCALE")
                    continue;
                if (entry.first == "PS2X_INPUT_MARKER" && explicitInputMarker)
                {
                    __android_log_write(ANDROID_LOG_INFO, kPs2xLogTag,
                                        "settings.env: PS2X_INPUT_MARKER kept from explicit env");
                    continue;
                }
                setenv(entry.first.c_str(), entry.second.c_str(), 1);
                __android_log_write(ANDROID_LOG_INFO, kPs2xLogTag,
                                    (std::string("settings.env: set ") + entry.first).c_str());
            }
            return;
        }
    }
#if defined(PS2X_DEFAULT_BOOT_ELF)
    __android_log_write(ANDROID_LOG_INFO, kPs2xLogTag,
                        ("settings.env: not found at " + ps2x::settingsEnvFilePath()).c_str());
#else
    __android_log_write(ANDROID_LOG_INFO, kPs2xLogTag, "settings.env: no PS2X_DEFAULT_BOOT_ELF; skipped");
#endif
}

struct Ps2xEnvLoader
{
    Ps2xEnvLoader()
    {
        // shortcuts-1: capture an explicit PS2X_INPUT_MARKER before any
        // layer applies (no preset or compiled default sets it, so present
        // here means the launcher/lab set it); the settings file must not
        // overwrite it below.
        const bool explicitInputMarker = (std::getenv("PS2X_INPUT_MARKER") != nullptr);
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
        loadSettingsEnvFile(explicitInputMarker); // SET2: the user layer reads last (skipped under reference above)
    }
};

static Ps2xEnvLoader g_ps2xEnvLoader;
} // namespace

namespace ps2x
{
// SET2: files-dir settings path for the panel. Defined at file scope (not
// in the anonymous namespace above — a nested ps2x would shadow ::ps2x).
std::string settingsEnvFilePath()
{
#if defined(PS2X_DEFAULT_BOOT_ELF)
    return siblingFilePathForBootElf(PS2X_DEFAULT_BOOT_ELF, "settings.env");
#else
    return std::string();
#endif
}

// ONL11: Quick Play login path (read, created on first Online entry when
// missing). F2 (appsec-fix-1): the internal files dir
// (PS2X_ANDROID_INTERNAL_DIR, set by RunnerApplication before native load),
// not the shared external files tree. Migrates the three login files once
// from the boot-ELF sibling dir; cards are never touched.
std::string quickplayLoginFilePath()
{
#if defined(PS2X_DEFAULT_BOOT_ELF)
    const std::string legacy = siblingFilePathForBootElf(PS2X_DEFAULT_BOOT_ELF, "quickplay.login");
    const char *internal = std::getenv("PS2X_ANDROID_INTERNAL_DIR");
    if (internal && *internal)
    {
        const std::string internalDir(internal);
        const std::string legacyDir = legacy.substr(0, legacy.find_last_of('/'));
        if (!legacyDir.empty() && legacyDir != internalDir)
        {
            ps2x::loginpaths::migrateLoginFiles(legacyDir, internalDir);
        }
        return ps2x::loginpaths::loginPathForDir(internalDir);
    }
    return legacy;
#else
    const char *internal = std::getenv("PS2X_ANDROID_INTERNAL_DIR");
    if (internal && *internal)
    {
        return ps2x::loginpaths::loginPathForDir(internal);
    }
    return std::string();
#endif
}
} // namespace ps2x

#endif // __ANDROID__
