#include "MiniTest.h"
#include "ps2_knobs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
// Save/restore one env key across a case (the suite shares one process).
struct EnvGuard
{
    const char *key;
    bool had;
    std::string saved;
    explicit EnvGuard(const char *k) : key(k)
    {
        const char *v = std::getenv(k);
        had = (v != nullptr);
        if (had)
            saved = v;
    }
    ~EnvGuard()
    {
        if (had)
            ::setenv(key, saved.c_str(), 1);
        else
            ::unsetenv(key);
    }
};

struct EnvSet
{
    std::vector<EnvGuard> guards;
    void track(const char *key) { guards.emplace_back(key); }
    void trackDefaults()
    {
        size_t n = 0;
        const ps2x::Cf2AndroidDefault *defs = ps2x::cf2AndroidDefaults(&n);
        for (size_t i = 0; i < n; ++i)
            track(defs[i].key);
        defs = ps2x::cf2IosDefaults(&n);
        for (size_t i = 0; i < n; ++i)
            track(defs[i].key);
        defs = ps2x::cf2MacDefaults(&n);
        for (size_t i = 0; i < n; ++i)
            track(defs[i].key);
    }
};

void clearDefaults()
{
    size_t n = 0;
    const ps2x::Cf2AndroidDefault *defs = ps2x::cf2AndroidDefaults(&n);
    for (size_t i = 0; i < n; ++i)
        ::unsetenv(defs[i].key);
    defs = ps2x::cf2IosDefaults(&n);
    for (size_t i = 0; i < n; ++i)
        ::unsetenv(defs[i].key);
    defs = ps2x::cf2MacDefaults(&n);
    for (size_t i = 0; i < n; ++i)
        ::unsetenv(defs[i].key);
    ::unsetenv("PS2X_PROFILE");
}
} // namespace

void register_ps2_knobs_tests()
{
    MiniTest::Case("Ps2Knobs", [](TestCase &tc)
                   {
        tc.Run("android defaults table is exactly the 33 S1+S3+CF4+CU3+CFG1 play keys (VX2: minus VU1_BLOCKS/FLAG_ELIDE; CFG1: minus VERTEX_KICK)", [](TestCase &t)
               {
            size_t n = 0;
            const ps2x::Cf2AndroidDefault *defs = ps2x::cf2AndroidDefaults(&n);
            t.Equals(n, static_cast<size_t>(33), "33 defaults");
            const char *want[] = {
                "PS2X_GS_BACKEND", "PS2X_GS_EXTERNAL_LIBRARY", "GE1_GS_RESOURCES_DIR",
                "GE1_GS_DATA_DIR", "GE1_GS_AHB_EXPORT", "GE1_ADRENO_BLEND_MIX",
                "GE1_TFX_PREWARM", "GE1_PIPE_FLUSH_VSYNCS", "PS2X_VU1_ENGINE",
                "PS2X_MTVU", "PS2X_MTVU_LAG", "PS2X_VU0_RECOMP",
                "PS2X_VU0_DIRECT", "PS2X_GS_HANDOFF_DIET",
                "PS2X_GS_FINISH_TIMING", "PS2X_SSX3_SIM_MODE", "PS2X_VIF1_REVERSE_DMA",
                "PS2X_SKIP_MOVIE", "PS2X_SOUND", "PS2X_PERF_LOG", "PS2X_PERF_LOG_DIR",
                "PS2X_GAME_THREAD_CPUS", "PS2X_MTVU_CPUS", "GE1_VK_TURNIP",
                "GE1_DRAW_BUFFERING", "GE1_UPSCALE", "PS2X_GE1_EXPORT_SIZE",
                "PS2X_MTVU_GIF_STAGE", "PS2X_MTVU_VIF_STAGE",
                // CFG1 Part 2 (D1/D4/D8).
                "PS2X_MICROVU_FLAG_HACK", "PS2X_MTVU_FINISH_EE", "PS2X_MTVU_VIF1_STAT_FREE",
                "GE1_ANISO",
            };
            for (const char *key : want)
            {
                bool found = false;
                for (size_t i = 0; i < n; ++i)
                    found = found || std::strcmp(defs[i].key, key) == 0;
                t.IsTrue(found, std::string("table holds ") + key);
            }
            // CF4: readback is off by default (literal "0", matching the play env).
            bool readbackOff = false;
            for (size_t i = 0; i < n; ++i)
                if (std::strcmp(defs[i].key, "PS2X_VIF1_REVERSE_DMA") == 0)
                    readbackOff = std::strcmp(defs[i].value, "0") == 0 && !defs[i].bootdir_relative;
            t.IsTrue(readbackOff, "readback defaulted to 0");
        });

        tc.Run("apply sets absent keys and keeps explicit env", [](TestCase &t)
               {
            EnvSet g;
            g.trackDefaults();
            g.track("PS2X_PROFILE");
            clearDefaults();
            ::setenv("PS2X_SOUND", "0", 1); // explicit off wins over default on
            ::setenv("PS2X_MTVU", "", 1);   // explicit empty counts as set
            const size_t applied = ps2x::cf2ApplyAndroidDefaults("/fake/files/SLUS_207.72");
            t.IsTrue(applied > 0, "something applied");
            t.Equals(std::string(::getenv("PS2X_SOUND")), std::string("0"), "explicit PS2X_SOUND kept");
            t.Equals(std::string(::getenv("PS2X_MTVU")), std::string(""), "explicit empty PS2X_MTVU kept");
            t.Equals(std::string(::getenv("PS2X_MTVU_LAG")), std::string("1"), "absent PS2X_MTVU_LAG defaulted");
            t.Equals(std::string(::getenv("PS2X_GS_BACKEND")), std::string("external"), "backend defaulted");
            t.Equals(std::string(::getenv("PS2X_VU1_ENGINE")), std::string("microvu"), "vu1 engine defaulted");
            t.Equals(std::string(::getenv("PS2X_VIF1_REVERSE_DMA")), std::string("0"), "readback defaulted off");
            t.Equals(std::string(::getenv("GE1_VK_TURNIP")), std::string("1"), "CU3: our Turnip defaulted");
            t.Equals(std::string(::getenv("GE1_UPSCALE")), std::string("2"), "CU3: 2x defaulted");
            t.Equals(std::string(::getenv("PS2X_GE1_EXPORT_SIZE")), std::string("display"), "CU3: display export");
            t.Equals(std::string(::getenv("PS2X_MTVU_VIF_STAGE")), std::string("1"), "CU3: VIF stage defaulted");
            t.Equals(std::string(::getenv("PS2X_MICROVU_FLAG_HACK")), std::string("1"), "CFG1 D1: flag hack defaulted");
            t.Equals(std::string(::getenv("PS2X_MTVU_FINISH_EE")), std::string("1"), "CFG1 D4: probe-drain EE defaulted");
            t.Equals(std::string(::getenv("PS2X_MTVU_VIF1_STAT_FREE")), std::string("1"), "CFG1 D4: probe-drain STAT defaulted");
            t.IsTrue(::getenv("GE1_VERTEX_KICK") == nullptr, "CFG1: dead VERTEX_KICK no longer defaulted");
        });

        tc.Run("reference profile disables every default", [](TestCase &t)
               {
            EnvSet g;
            g.trackDefaults();
            g.track("PS2X_PROFILE");
            clearDefaults();
            ::setenv("PS2X_PROFILE", "reference", 1);
            t.IsTrue(ps2x::cf2ProfileIsReference(), "reference detected");
            const size_t applied = ps2x::cf2ApplyAndroidDefaults("/fake/files/SLUS_207.72");
            t.Equals(applied, static_cast<size_t>(0), "nothing applied under reference");
            t.IsTrue(::getenv("PS2X_SOUND") == nullptr, "PS2X_SOUND stays unset");
            t.IsTrue(::getenv("PS2X_GS_BACKEND") == nullptr, "backend stays unset");
            ::setenv("PS2X_PROFILE", "play", 1);
            t.IsFalse(ps2x::cf2ProfileIsReference(), "unknown profile ignored");
            t.IsTrue(ps2x::cf2ApplyAndroidDefaults("/fake/files/SLUS_207.72") > 0, "defaults apply again");
        });

        tc.Run("CFG1 per-platform tables hold the folded play keys", [](TestCase &t)
               {
            size_t ni = 0, nm = 0;
            const ps2x::Cf2AndroidDefault *ios = ps2x::cf2IosDefaults(&ni);
            const ps2x::Cf2AndroidDefault *mac = ps2x::cf2MacDefaults(&nm);
            t.Equals(ni, static_cast<size_t>(7), "7 iOS defaults");
            t.Equals(nm, static_cast<size_t>(5), "5 Mac defaults");
            const char *wantIos[] = {
                "PS2X_MICROVU_FLAG_HACK", "PS2X_MTVU_FINISH_EE", "PS2X_MTVU_VIF1_STAT_FREE",
                "PS2X_AUDIO_RESAMPLE", "PS2X_VIF1_REVERSE_DMA",
                "PS2X_SSX3_SIM_MODE", "GE1_ANISO",
            };
            const char *wantMac[] = {
                "PS2X_MICROVU_FLAG_HACK", "PS2X_MTVU_FINISH_EE", "PS2X_MTVU_VIF1_STAT_FREE",
                "PS2X_SKIP_MOVIE", "GE1_ANISO",
            };
            for (const char *key : wantIos)
            {
                bool found = false;
                for (size_t i = 0; i < ni; ++i)
                    found = found || std::strcmp(ios[i].key, key) == 0;
                t.IsTrue(found, std::string("iOS table holds ") + key);
            }
            for (const char *key : wantMac)
            {
                bool found = false;
                for (size_t i = 0; i < nm; ++i)
                    found = found || std::strcmp(mac[i].key, key) == 0;
                t.IsTrue(found, std::string("Mac table holds ") + key);
            }
            for (size_t i = 0; i < ni; ++i)
                t.IsTrue(!ios[i].bootdir_relative && !ios[i].only_if_exists && !ios[i].needs_big_cpu,
                         "iOS literals only");
            for (size_t i = 0; i < nm; ++i)
                t.IsTrue(!mac[i].bootdir_relative && !mac[i].only_if_exists && !mac[i].needs_big_cpu,
                         "Mac literals only");
        });

        tc.Run("CFG1 platform appliers set absent keys, keep explicit env, honor reference", [](TestCase &t)
               {
            EnvSet g;
            g.trackDefaults();
            g.track("PS2X_PROFILE");
            clearDefaults();
            ::setenv("PS2X_AUDIO_RESAMPLE", "off", 1); // explicit wins over sinc
            t.Equals(ps2x::cf2ApplyIosDefaults(), static_cast<size_t>(6), "6 iOS keys applied");
            t.Equals(std::string(::getenv("PS2X_AUDIO_RESAMPLE")), std::string("off"), "explicit resample kept");
            t.Equals(std::string(::getenv("PS2X_MICROVU_FLAG_HACK")), std::string("1"), "iOS flag hack defaulted");
            t.Equals(std::string(::getenv("PS2X_VIF1_REVERSE_DMA")), std::string("0"), "iOS readback explicit 0");
            t.Equals(std::string(::getenv("PS2X_SSX3_SIM_MODE")), std::string("split120_render60_v1"), "iOS 60-path split120");
            t.Equals(std::string(::getenv("GE1_ANISO")), std::string("16"), "iOS aniso defaulted");
            t.Equals(ps2x::cf2ApplyMacDefaults(), static_cast<size_t>(1), "1 Mac key applied (only SKIP_MOVIE still absent)");
            t.Equals(std::string(::getenv("PS2X_SKIP_MOVIE")), std::string("1"), "Mac movie skip defaulted");
            clearDefaults();
            ::setenv("PS2X_PROFILE", "reference", 1);
            t.Equals(ps2x::cf2ApplyIosDefaults(), static_cast<size_t>(0), "iOS nothing under reference");
            t.Equals(ps2x::cf2ApplyMacDefaults(), static_cast<size_t>(0), "Mac nothing under reference");
        });

        tc.Run("bootdir-relative paths derive from the boot ELF dir", [](TestCase &t)
               {
            EnvSet g;
            g.trackDefaults();
            g.track("PS2X_PROFILE");
            clearDefaults();
            ps2x::cf2ApplyAndroidDefaults("/fake/files/SLUS_207.72");
            t.Equals(std::string(::getenv("GE1_GS_DATA_DIR")), std::string("/fake/files/ge1-data"),
                     "data dir derived");
            t.Equals(std::string(::getenv("GE1_GS_RESOURCES_DIR")), std::string("/fake/files/ge1-resources"),
                     "resources dir derived");
            t.Equals(std::string(::getenv("PS2X_PERF_LOG_DIR")), std::string("/fake/files/perf"),
                     "perf dir derived");
            t.Equals(ps2x::cf2BootDirForBootElf(nullptr), std::string(""), "null boot ELF, no dir");
            t.Equals(ps2x::cf2BootDirForBootElf("bare.elf"), std::string(""), "dir-less boot ELF, no dir");
            clearDefaults();
            ps2x::cf2ApplyAndroidDefaults(nullptr);
            t.IsTrue(::getenv("GE1_GS_DATA_DIR") == nullptr, "no bootdir, no derived paths");
            t.Equals(std::string(::getenv("PS2X_SOUND")), std::string("1"), "literals still apply");
        });

        tc.Run("tfx prewarm defaults only when the file exists", [](TestCase &t)
               {
            EnvSet g;
            g.trackDefaults();
            g.track("PS2X_PROFILE");
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path tmp = fs::temp_directory_path(ec) / "cf2-knobs-test";
            fs::remove_all(tmp, ec);
            fs::create_directories(tmp / "files" / "ge1-data", ec);
            const std::string bootElf = (tmp / "files" / "SLUS_207.72").string();
            clearDefaults();
            ps2x::cf2ApplyAndroidDefaults(bootElf.c_str());
            t.IsTrue(::getenv("GE1_TFX_PREWARM") == nullptr, "missing selectors file, no prewarm");
            t.Equals(std::string(::getenv("GE1_PIPE_FLUSH_VSYNCS")), std::string("600"),
                     "flush vsyncs still default");
            { // now with the selectors file present
                FILE *f = std::fopen((tmp / "files" / "ge1-data" / "tfx-selectors.bin").string().c_str(), "wb");
                t.IsTrue(f != nullptr, "fixture file created");
                if (f)
                {
                    std::fputc(0, f);
                    std::fclose(f);
                }
            }
            clearDefaults();
            ps2x::cf2ApplyAndroidDefaults(bootElf.c_str());
            t.Equals(std::string(::getenv("GE1_TFX_PREWARM")),
                     (tmp / "files" / "ge1-data" / "tfx-selectors.bin").string(), "prewarm defaulted");
            fs::remove_all(tmp, ec);
        });

        tc.Run("cpu pin guard allows unknown and 8+, denies below 8", [](TestCase &t)
               {
            t.IsFalse(ps2x::cf2CpuPinAllowed(7), "7 cpus denies pins");
            t.IsFalse(ps2x::cf2CpuPinAllowed(4), "4 cpus denies pins");
            t.IsTrue(ps2x::cf2CpuPinAllowed(8), "8 cpus allows pins");
            t.IsTrue(ps2x::cf2CpuPinAllowed(20), "20 cpus allows pins");
            t.IsTrue(ps2x::cf2CpuPinAllowed(-1), "unknown nproc keeps P3 parity");
            t.IsTrue(ps2x::cf2HostNproc() >= 1, "host nproc reads");
        });

        tc.Run("curated dump list is sorted and unique", [](TestCase &t)
               {
            size_t n = 0;
            const char *const *knobs = ps2x::cf2DumpKnobs(&n);
            t.IsTrue(n >= 100, "at least a hundred curated knobs (VX2 dropped four dead VU1 knobs)");
            for (size_t i = 1; i < n; ++i)
                t.IsTrue(std::strcmp(knobs[i - 1], knobs[i]) < 0, "sorted and unique");
            t.IsTrue(ps2x::cf2KnobIsCurated("PS2X_SOUND"), "curated hit");
            t.IsFalse(ps2x::cf2KnobIsCurated("PS2X_VSYNC_RATE_LOG"), "diag left to the scan");
            t.IsTrue(ps2x::cf2KnobIsCurated("PS2X_TK12_AP_ROUTE"), "APH1 dev AP route curated");
            t.IsTrue(ps2x::cf2KnobIsCurated("PS2X_TK12_CLIP"), "APH1 dev clip curated");
        });

        tc.Run("dump line shows set values, unset markers and set diag extras", [](TestCase &t)
               {
            EnvSet g;
            g.track("PS2X_SOUND");
            g.track("PS2X_CD_IMAGE");
            g.track("PS2X_VSYNC_RATE_LOG");
            ::unsetenv("PS2X_SOUND");
            ::setenv("PS2X_CD_IMAGE", "/tmp/x.iso", 1);
            ::setenv("PS2X_VSYNC_RATE_LOG", "1", 1);
            const std::string line = ps2x::cf2BuildKnobsLine();
            t.IsTrue(line.compare(0, 17, "[knobs] platform=") == 0, "knobs prefix");
            t.IsTrue(line.find(" PS2X_SOUND=unset") != std::string::npos, "unset marker");
            t.IsTrue(line.find(" PS2X_CD_IMAGE=/tmp/x.iso") != std::string::npos, "set value shown");
            t.IsTrue(line.find(" PS2X_VSYNC_RATE_LOG=1") != std::string::npos, "diag extra shown");
            t.IsTrue(line.find('\n') == std::string::npos, "single line");
            ::setenv("PS2X_CD_IMAGE", "a\nb\rc", 1);
            const std::string clean = ps2x::cf2BuildKnobsLine();
            t.IsTrue(clean.find(" PS2X_CD_IMAGE=a_b_c") != std::string::npos, "newlines sanitized");
        });

        tc.Run("PB7-play-env dump stays under the logcat cap", [](TestCase &t)
               {
            EnvSet g;
            g.trackDefaults();
            g.track("PS2X_PROFILE");
            g.track("PS2X_CD_IMAGE");
            g.track("PS2X_PAD_SCRIPT");
            g.track("PS2X_VSYNC_RATE_LOG");
            g.track("PS2X_UNPACED");
            g.track("PS2X_DETERMINISTIC");
            g.track("PS2X_DET_HASH_EVERY");
            g.track("GE1_VERTEX_KICK");
            g.track("PS2X_PAD_RECORD_DIR");
            clearDefaults();
            ps2x::cf2ApplyAndroidDefaults("/storage/emulated/0/Android/data/com.ps2x.runner/files/SLUS_207.72");
            ::setenv("PS2X_CD_IMAGE",
                     "/storage/emulated/0/Android/data/com.ps2x.runner/files/SSX3.iso", 1);
            ::setenv("PS2X_PAD_SCRIPT", "/storage/emulated/0/Android/data/com.ps2x.runner/files/I26-FAST.txt", 1);
            ::setenv("PS2X_VSYNC_RATE_LOG", "1", 1);
            ::setenv("PS2X_UNPACED", "1", 1);
            ::setenv("PS2X_DETERMINISTIC", "1", 1);
            ::setenv("PS2X_DET_HASH_EVERY", "5", 1);
            // PB5/6 play-env lines past P3 (explicit: not compiled defaults;
            // CFG1: VERTEX_KICK is compiled nowhere, kept here as a stale-env line).
            ::setenv("GE1_VERTEX_KICK", "2", 1);
            ::setenv("PS2X_PAD_RECORD_DIR",
                     "/storage/emulated/0/Android/data/com.ps2x.runner/files/padrec", 1);
            const std::string line = ps2x::cf2BuildKnobsLine();
            // logd truncates past ~4 KB (LOGGER_ENTRY_MAX_PAYLOAD 4076); the
            // real PB7-shape leg prints ~3987 bytes. Trip here before growth
            // truncates. APH1 adds nine TK12 dev knobs (~239 B when unset),
            // so the PB7-shape leg now prints ~4226 bytes.
            t.IsTrue(line.size() < 4300, "dump under 4300 bytes");
            t.IsTrue(line.find(" PS2X_GS_BACKEND=external") != std::string::npos, "default visible");
            t.IsTrue(line.find(" PS2X_VIF1_REVERSE_DMA=0") != std::string::npos, "readback-off default visible");
        });
    });
}
