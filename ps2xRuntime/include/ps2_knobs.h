#pragma once
// CF2 S0/S1: knob dump + Android compiled defaults.
//
// S0: cf2DumpKnobs() prints one always-on startup line,
//   [knobs] platform=<p> K=V ...
// listing every curated behavioural knob (CF1 §1.1) with its effective
// value (`unset` when absent from the process environment), plus any other
// set PS2X_*/PGS_*/GE1_*/GE7_*/GP3_*/GRANITE_*/PS2RECOMP_* variable (diag
// knobs that are actually set). Later stages prove "empty env == old env"
// by diffing this line. It shows env inputs, not semantic resolution:
// `unset` means the code default applies (see CF1 §1.1 for what each is),
// and GE1_AUTO-style inputs (e.g. GE1_ADRENO_DSTREAD unset = AUTO) resolve
// inside the adapter, which exports no knob query over its C ABI.
//
// S1+S3: cf2ApplyAndroidDefaults() compiles the CF1 §Q2 Android column into
// the runtime: on Android the env-file loader calls it after loading ps2x.env,
// and every defaulted knob absent from the environment gets its play value via
// setenv (explicit env, including an explicit empty value, still wins).
// Paths derive from the boot-ELF dir; no absolute paths in code.
// PS2X_PROFILE=reference disables every compiled default, restoring the
// exact knob-off path. S3 (CF3) added the guest-affecting lagV default, so an
// empty env reproduced the PB7 play env (modulo the output-only VERTEX_KICK,
// the dev PAD_RECORD_DIR, and the inert PGS lines the play env still carries).
// CF4 turns the readback default off again (Brad 09-28 "let's turn readback
// off"): the compiled default is the literal "0", matching the Odin play env,
// so an empty env is the play state.
// CU3 adds the Odin default set (Brad 09-30, "2 sounds good"): our Turnip,
// the VU1 GIF/VIF stages, draw buffering and 2x internal resolution (export
// sized to the display). The full-120 layer (FULL120, FIX, FASTHOOKS,
// DISPLAY_HZ, ...) stays in full120.env. GE1_VK_TURNIP=1 needs the Turnip HAL
// in the APK's jniLibs (play-current carries it); set GE1_VK_TURNIP=0
// without it.
// CFG1 Part 2 folds the remaining signed-off play settings into kDefaults
// (D1 microVU flag hack, D4 the MQ2+MQ3 probe-drain pair, D8 aniso 16x — all
// det IDENTICAL) and deletes the dead GE1_VERTEX_KICK row (knob gone on the
// ARMSX2 base; the Odin env line is inert). New per-platform literal tables
// below do the same for iOS (D1/D4, AU13 sinc, CF4-explicit readback off, D2
// split120 at 60, D8 aniso) and Mac (D1/D4, D5 movie skip, D8 aniso);
// PS2X_PROFILE=reference disables every table.
//
// Platform-neutral on purpose so the host unit test compiles this header
// (same shape as ps2_android_env.h / ps2_env_file.h). The Android loader,
// the iOS prepareEnvironment and Mac main() call their appliers after the
// env layers; every platform calls cf2DumpKnobs from main().

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// The environ scan + sysconf need POSIX; Vita and Windows skip them (diag
// extras omitted from the dump there, nproc reads unknown → pins allowed).
#if !defined(_WIN32) && !defined(PLATFORM_VITA) && !defined(__vita__)
#define CF2_HAS_POSIX_ENV 1
#include <unistd.h>
extern char **environ;
#else
#define CF2_HAS_POSIX_ENV 0
#endif

namespace ps2x
{

// PS2X_PROFILE=reference restores the exact knob-off path (no compiled
// defaults). Unknown values are ignored (defaults still apply).
inline bool cf2ProfileIsReference()
{
    const char *profile = std::getenv("PS2X_PROFILE");
    return profile != nullptr && std::strcmp(profile, "reference") == 0;
}

struct Cf2AndroidDefault
{
    const char *key;
    // Literal value, or a "/..." suffix joined onto the boot-ELF dir when
    // bootdir_relative is true.
    const char *value;
    bool bootdir_relative;
    // Set only when the resolved path names an existing file (TFX prewarm).
    bool only_if_exists;
    // Set only when the device has >= 8 online CPUs (Odin topology pins).
    bool needs_big_cpu;
};

// CF1 §Q2 Android column minus GE1_ADRENO_DSTREAD (already AUTO in the
// adapter). Every value is the signed-off play value (S1 = the P3 set,
// S3 = lagV, CF4 = readback off: the VIF1_REVERSE_DMA default is "0";
// CU3 = the Odin default set; CFG1 Part 2 = D1 flag hack + D4 probe-drain
// pair + D8 anisotropic 16x, minus the dead GE1_VERTEX_KICK row).
// The value must be the literal "0" (unset is also off in the code default,
// but the applier always sets the key, so only "0" keeps it off).
inline const Cf2AndroidDefault *cf2AndroidDefaults(size_t *countOut)
{
    static const Cf2AndroidDefault kDefaults[] = {
        {"PS2X_GS_BACKEND", "external", false, false, false},
        {"PS2X_GS_EXTERNAL_LIBRARY", "libge1_gs.so", false, false, false},
        {"GE1_GS_RESOURCES_DIR", "/ge1-resources", true, false, false},
        {"GE1_GS_DATA_DIR", "/ge1-data", true, false, false},
        {"GE1_GS_AHB_EXPORT", "1", false, false, false},
        {"GE1_ADRENO_BLEND_MIX", "1", false, false, false},
        {"GE1_TFX_PREWARM", "/ge1-data/tfx-selectors.bin", true, true, false},
        {"GE1_PIPE_FLUSH_VSYNCS", "600", false, false, false},
        {"PS2X_VU1_ENGINE", "microvu", false, false, false},
        {"PS2X_MTVU", "1", false, false, false},
        {"PS2X_MTVU_LAG", "1", false, false, false},
        {"PS2X_VU0_RECOMP", "1", false, false, false},
        {"PS2X_VU0_DIRECT", "1", false, false, false},
        {"PS2X_GS_HANDOFF_DIET", "1", false, false, false},
        {"PS2X_GS_FINISH_TIMING", "pcsx2", false, false, false},
        {"PS2X_SSX3_SIM_MODE", "split120_render60_v1", false, false, false},
        {"PS2X_VIF1_REVERSE_DMA", "0", false, false, false},
        {"PS2X_SKIP_MOVIE", "1", false, false, false},
        {"PS2X_SOUND", "1", false, false, false},
        {"PS2X_PERF_LOG", "1", false, false, false},
        {"PS2X_PERF_LOG_DIR", "/perf", true, false, false},
        {"PS2X_GAME_THREAD_CPUS", "6", false, false, true},
        {"PS2X_MTVU_CPUS", "7", false, false, true},
        {"GE1_VK_TURNIP", "1", false, false, false},
        {"GE1_DRAW_BUFFERING", "1", false, false, false},
        {"GE1_UPSCALE", "2", false, false, false},
        {"PS2X_GE1_EXPORT_SIZE", "display", false, false, false},
        {"PS2X_MTVU_GIF_STAGE", "1", false, false, false},
        {"PS2X_MTVU_VIF_STAGE", "1", false, false, false},
        // CFG1 Part 2 (D1/D4): the microVU flag hack (Brad 09-30 default) and
        // the MQ2+MQ3 probe-drain pair (Brad 10-02, always together). Both
        // det IDENTICAL (no new key); presence-gated readers keep the exact
        // path under PS2X_PROFILE=reference.
        {"PS2X_MICROVU_FLAG_HACK", "1", false, false, false},
        {"PS2X_MTVU_FINISH_EE", "1", false, false, false},
        {"PS2X_MTVU_VIF1_STAT_FREE", "1", false, false, false},
        // CFG1 Part 2 (D8): anisotropic 16x on every platform (Brad 10-02).
        // Output-only (GS adapter plain getenv); det IDENTICAL.
        {"GE1_ANISO", "16", false, false, false},
        // AU19 (Brad 10-02): DC-blocking high-pass on everywhere. Output-only
        // (AU18 1-pole 5 Hz filter on the final host output); det IDENTICAL
        // with the knob on. Same literal flag fields as the AU13 sinc entry.
        {"PS2X_AUDIO_DC_BLOCK", "1", false, false, false},
    };
    if (countOut)
        *countOut = sizeof(kDefaults) / sizeof(kDefaults[0]);
    return kDefaults;
}

inline std::string cf2BootDirForBootElf(const char *bootElf)
{
    if (bootElf == nullptr || bootElf[0] == '\0')
        return std::string();
    const std::string path(bootElf);
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return std::string();
    return path.substr(0, slash);
}

// The nproc guard only engages on a confident < 8 reading; unknown (-1)
// keeps P3 parity (sysconf cannot usefully fail on Android).
inline bool cf2CpuPinAllowed(long nproc)
{
    return nproc < 0 || nproc >= 8;
}

inline long cf2HostNproc()
{
#if CF2_HAS_POSIX_ENV
    return ::sysconf(_SC_NPROCESSORS_ONLN);
#else
    return -1;
#endif
}

inline bool cf2FileExists(const std::string &path)
{
    std::ifstream file(path.c_str(), std::ios::binary);
    return file.is_open();
}

// Set every S1+S3 default absent from the environment. Returns the number of
// keys set. Under PS2X_PROFILE=reference sets nothing (exact knob-off path).
inline size_t cf2ApplyAndroidDefaults(const char *bootElf)
{
    if (cf2ProfileIsReference())
        return 0;
    const std::string bootDir = cf2BootDirForBootElf(bootElf);
    const long nproc = cf2HostNproc();
    size_t count = 0;
    size_t n = 0;
    const Cf2AndroidDefault *defs = cf2AndroidDefaults(&n);
    for (size_t i = 0; i < n; ++i)
    {
        const Cf2AndroidDefault &d = defs[i];
        if (std::getenv(d.key) != nullptr)
            continue; // explicit env (even empty) wins
        if (d.needs_big_cpu && !cf2CpuPinAllowed(nproc))
            continue;
        std::string value;
        if (d.bootdir_relative)
        {
            if (bootDir.empty())
                continue;
            value = bootDir + d.value;
            if (d.only_if_exists && !cf2FileExists(value))
                continue;
        }
        else
        {
            value = d.value;
        }
#if defined(_WIN32)
        _putenv_s(d.key, value.c_str());
#else
        ::setenv(d.key, value.c_str(), 0);
#endif
        ++count;
    }
    return count;
}

// CFG1 Part 2: per-platform compiled play defaults for iOS and Mac. Same
// Cf2AndroidDefault shape with literal values only (no bootdir-relative,
// file-gated or topology-gated entries on these platforms). The iOS loader
// (prepareEnvironment) and Mac main() call their applier after the env
// layers; explicit env (bundle, Documents, launcher, boot script) wins and
// PS2X_PROFILE=reference disables the table, exactly like Android.

// iOS bundled-play equivalent (D1 flag hack, D4 probe-drain pair, AU13 sinc
// resample, CF4-explicit readback off, D2 split120 at 60, D8 aniso 16x).
// Device-specific values (upscale, SSAA, scanout, zero-copy, paths) and the
// 120-layer user toggle stay in the bundled env + Settings layers; D8 (AF16)
// is decided (this table), not waiting. The Target 120 switch defaults ON, so
// the D2 split120 line rules only the user-picked 60 path (the 120 preset
// overrides to stock + full-120 events).
inline const Cf2AndroidDefault *cf2IosDefaults(size_t *countOut)
{
    static const Cf2AndroidDefault kIosDefaults[] = {
        {"PS2X_MICROVU_FLAG_HACK", "1", false, false, false},
        {"PS2X_MTVU_FINISH_EE", "1", false, false, false},
        {"PS2X_MTVU_VIF1_STAT_FREE", "1", false, false, false},
        {"PS2X_AUDIO_RESAMPLE", "sinc", false, false, false},
        {"PS2X_VIF1_REVERSE_DMA", "0", false, false, false},
        {"PS2X_SSX3_SIM_MODE", "split120_render60_v1", false, false, false},
        {"GE1_ANISO", "16", false, false, false},
        // AU19 (Brad 10-02): DC block on everywhere (same literal fields).
        {"PS2X_AUDIO_DC_BLOCK", "1", false, false, false},
    };
    if (countOut)
        *countOut = sizeof(kIosDefaults) / sizeof(kIosDefaults[0]);
    return kIosDefaults;
}

// Mac play-profile equivalent (D1 flag hack, D4 probe-drain pair, D5 movie
// skip, D8 aniso 16x). SOUND stays harness-set (D6); the rest of PLAY_ENV
// (engines, stages, sim mode, readback, timing) stays explicit in ssx3_boot.py.
inline const Cf2AndroidDefault *cf2MacDefaults(size_t *countOut)
{
    static const Cf2AndroidDefault kMacDefaults[] = {
        {"PS2X_MICROVU_FLAG_HACK", "1", false, false, false},
        {"PS2X_MTVU_FINISH_EE", "1", false, false, false},
        {"PS2X_MTVU_VIF1_STAT_FREE", "1", false, false, false},
        {"PS2X_SKIP_MOVIE", "1", false, false, false},
        {"GE1_ANISO", "16", false, false, false},
        // AU19 (Brad 10-02): DC block on everywhere (same literal fields).
        {"PS2X_AUDIO_DC_BLOCK", "1", false, false, false},
    };
    if (countOut)
        *countOut = sizeof(kMacDefaults) / sizeof(kMacDefaults[0]);
    return kMacDefaults;
}

// Set every absent key from a literal per-platform table. Returns the number
// of keys set. Under PS2X_PROFILE=reference sets nothing.
inline size_t cf2ApplyLiteralDefaults(const Cf2AndroidDefault *defs, size_t n)
{
    if (cf2ProfileIsReference())
        return 0;
    size_t count = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (std::getenv(defs[i].key) != nullptr)
            continue; // explicit env (even empty) wins
#if defined(_WIN32)
        _putenv_s(defs[i].key, defs[i].value);
#else
        ::setenv(defs[i].key, defs[i].value, 0);
#endif
        ++count;
    }
    return count;
}

inline size_t cf2ApplyIosDefaults()
{
    size_t n = 0;
    const Cf2AndroidDefault *defs = cf2IosDefaults(&n);
    return cf2ApplyLiteralDefaults(defs, n);
}

inline size_t cf2ApplyMacDefaults()
{
    size_t n = 0;
    const Cf2AndroidDefault *defs = cf2MacDefaults(&n);
    return cf2ApplyLiteralDefaults(defs, n);
}

// Curated S0 list: every CF1 §1.1 behavioural knob + PS2X_PROFILE. Sorted;
// cf2DumpKnobs prints `unset` for absent entries. Diag knobs are not listed:
// set ones are picked up by the environ scan below.
inline const char *const *cf2DumpKnobs(size_t *countOut)
{
    static const char *const kKnobs[] = {
        "GE1_ADRENO_BLEND_MIX",
        "GE1_ADRENO_DSTREAD",
        "GE1_BACKTHREAD",
        "GE1_DRAW_BUFFERING",
        "GE1_GS_AHB_EXPORT",
        "GE1_GS_DATA_DIR",
        "GE1_GS_RESOURCES_DIR",
        "GE1_PIPE_FLUSH_VSYNCS",
        "GE1_RENDERER",
        "GE1_TFX_PREWARM",
        "GE1_UPSCALE",
        "GE1_VERTEX_KICK",
        "GE1_VK_TURNIP",
        "PS2RECOMP_SCE_SYMBOL_DB",
        "PS2X_ASPECT",
        "PS2X_AUDIO_STRETCH",
        "PS2X_BOOT_ELF",
        "PS2X_CD_IMAGE",
        "PS2X_CD_OVERLAY",
        "PS2X_DEINTERLACE",
        "PS2X_DETERMINISTIC",
        "PS2X_EE_FPMODE",
        "PS2X_GAME_THREAD_CPUS",
        "PS2X_GAME_THREAD_STACK_KB",
        "PS2X_GE1_EXPORT_SIZE",
        "PS2X_GS_BACKEND",
        "PS2X_GS_EXTERNAL_LIBRARY",
        "PS2X_GS_EXTERNAL_LOG",
        "PS2X_GS_FINISH_TIMING",
        "PS2X_GS_GIF_BATCH",
        "PS2X_GS_GIF_BATCH_BYTES",
        "PS2X_GS_HANDOFF_DIET",
        "PS2X_GS_REPLAY_BACKEND",
        "PS2X_GS_REPLAY_MODE",
        "PS2X_GS_WORKER_CPUS",
        "PS2X_MC_ROOT",
        "PS2X_MICROVU_LIB",
        "PS2X_MISSING_FUNCTION_POLICY",
        "PS2X_MTVU",
        "PS2X_MTVU_CPUS",
        "PS2X_MTVU_FINISH_EE",
        "PS2X_MTVU_GIF_STAGE",
        "PS2X_MTVU_GS_BATCH",
        "PS2X_MTVU_LAG",
        "PS2X_MTVU_VIF1_STAT_FREE",
        "PS2X_MTVU_VIF_STAGE",
        "PS2X_PAD_RECORD",
        "PS2X_PAD_RECORD_DIR",
        "PS2X_PAD_RECORD_KEEP",
        "PS2X_PAD_SCRIPT",
        "PS2X_PAD_SCRIPT_CLOCK",
        "PS2X_PERF_LOG",
        "PS2X_PERF_LOG_DIR",
        "PS2X_PKB",
        "PS2X_PRESENT_FILTER",
        "PS2X_PRESENT_VULKAN",
        "PS2X_PRESENT_ZERO_COPY",
        "PS2X_PROFILE",
        "PS2X_SAVESTATE_EXIT_AFTER_SAVE",
        "PS2X_SAVESTATE_LOAD",
        "PS2X_SAVESTATE_PATH",
        "PS2X_SAVESTATE_SAVE_AT",
        "PS2X_SAVESTATE_STRICT",
        "PS2X_SKIP_MOVIE",
        "PS2X_SOUND",
        "PS2X_SSX3_ANIM_PICK_GUARD",
        "PS2X_SSX3_COURSE_MANIFEST",
        "PS2X_SSX3_COURSE_PICKER",
        "PS2X_SSX3_DRAW_HZ",
        "PS2X_SSX3_FULL120",
        "PS2X_SSX3_FULL120_FASTHOOKS",
        "PS2X_SSX3_FULL120_FIX",
        "PS2X_SSX3_JCAM2_SCRATCH",
        "PS2X_SSX3_LOD_SCALE",
        "PS2X_SSX3_SIM_MODE",
        "PS2X_SSX3_TRICKY_GEMS",
        "PS2X_SSX3_TRICKY_GEMS_TABLE",
        "PS2X_SSX3_TRICKY_GEM_SPIN",
        "PS2X_SSX3_TRICKY_HUD",
        "PS2X_SSX3_TRICKY_HUD_ART",
        "PS2X_SSX3_TRICKY_LOD_SCALE",
        "PS2X_SSX3_TRICKY_MENU",
        "PS2X_SSX3_TRICKY_SONG",
        "PS2X_STRETCH_LEAVE",
        "PS2X_STRETCH_REJOIN_MS",
        "PS2X_STRETCH_SUSTAIN_MS",
        "PS2X_TEST_DEFERRED",
        "PS2X_TK12_AP_DPAD_DEG",
        "PS2X_TK12_AP_FROM",
        "PS2X_TK12_AP_FULL_DEG",
        "PS2X_TK12_AP_LOG_EVERY",
        "PS2X_TK12_AP_LOOK",
        "PS2X_TK12_AP_PTR",
        "PS2X_TK12_AP_ROUTE",
        "PS2X_TK12_AP_TO",
        "PS2X_TK12_CLIP",
        "PS2X_TRICKY_HUD_ASYNC",
        "PS2X_TRICKY_HUD_CACHE",
        "PS2X_UNPACED",
        "PS2X_VIF1_REVERSE_DMA",
        "PS2X_VIF_UNPACK_FAST",
        "PS2X_VIRTUAL_PAD",
        "PS2X_VU0_BLOCKS",
        "PS2X_VU0_DIRECT",
        "PS2X_VU0_RECOMP",
        "PS2X_VU1_ENGINE",
        "PS2X_VU_FLOAT",
        "PS2X_WIDESCREEN",
    };
    if (countOut)
        *countOut = sizeof(kKnobs) / sizeof(kKnobs[0]);
    return kKnobs;
}

inline bool cf2KnobExtraPrefix(const char *key)
{
    static const char *const kPrefixes[] = {
        "PS2X_", "PGS_", "GE1_", "GE7_", "GP3_", "GRANITE_", "PS2RECOMP_",
    };
    for (const char *prefix : kPrefixes)
    {
        const size_t len = std::strlen(prefix);
        if (std::strncmp(key, prefix, len) == 0)
            return true;
    }
    return false;
}

inline bool cf2KnobIsCurated(const char *key)
{
    size_t n = 0;
    const char *const *knobs = cf2DumpKnobs(&n);
    for (size_t i = 0; i < n; ++i)
    {
        if (std::strcmp(key, knobs[i]) == 0)
            return true;
    }
    return false;
}

// Keep the dump one line: no newlines or returns (padrec convention).
inline std::string cf2SanitizeKnobValue(const char *value)
{
    std::string out(value ? value : "");
    for (char &c : out)
    {
        if (c == '\n' || c == '\r')
            c = '_';
    }
    return out;
}

inline const char *cf2PlatformName()
{
#if defined(__ANDROID__)
    return "android";
#elif defined(PS2X_IOS)
    return "ios";
#elif defined(__APPLE__)
    return "mac";
#elif defined(__linux__)
    return "linux";
#else
    return "other";
#endif
}

inline std::string cf2BuildKnobsLine()
{
    std::string line("[knobs] platform=");
    line += cf2PlatformName();
    size_t n = 0;
    const char *const *knobs = cf2DumpKnobs(&n);
    for (size_t i = 0; i < n; ++i)
    {
        const char *value = std::getenv(knobs[i]);
        line += ' ';
        line += knobs[i];
        line += '=';
        line += (value != nullptr) ? cf2SanitizeKnobValue(value) : "unset";
    }
#if CF2_HAS_POSIX_ENV
    // Set diag (or future) knobs not in the curated list, sorted.
    std::vector<std::string> extras;
    for (char **e = environ; e != nullptr && *e != nullptr; ++e)
    {
        const char *entry = *e;
        const char *eq = std::strchr(entry, '=');
        if (eq == nullptr || eq == entry)
            continue;
        const std::string key(entry, static_cast<size_t>(eq - entry));
        if (!cf2KnobExtraPrefix(key.c_str()) || cf2KnobIsCurated(key.c_str()))
            continue;
        extras.push_back(key + "=" + cf2SanitizeKnobValue(eq + 1));
    }
    std::sort(extras.begin(), extras.end());
    for (const std::string &extra : extras)
    {
        line += ' ';
        line += extra;
    }
#endif
    return line;
}

inline void cf2DumpKnobs()
{
    const std::string line = cf2BuildKnobsLine();
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

} // namespace ps2x
