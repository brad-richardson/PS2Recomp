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
    }
};

void clearDefaults()
{
    size_t n = 0;
    const ps2x::Cf2AndroidDefault *defs = ps2x::cf2AndroidDefaults(&n);
    for (size_t i = 0; i < n; ++i)
        ::unsetenv(defs[i].key);
    ::unsetenv("PS2X_PROFILE");
}
} // namespace

void register_ps2_knobs_tests()
{
    MiniTest::Case("Ps2Knobs", [](TestCase &tc)
                   {
        tc.Run("android defaults table is exactly the 24 S1 P3 keys", [](TestCase &t)
               {
            size_t n = 0;
            const ps2x::Cf2AndroidDefault *defs = ps2x::cf2AndroidDefaults(&n);
            t.Equals(n, static_cast<size_t>(24), "24 defaults");
            const char *want[] = {
                "PS2X_GS_BACKEND", "PS2X_GS_EXTERNAL_LIBRARY", "GE1_GS_RESOURCES_DIR",
                "GE1_GS_DATA_DIR", "GE1_GS_AHB_EXPORT", "GE1_ADRENO_BLEND_MIX",
                "GE1_TFX_PREWARM", "GE1_PIPE_FLUSH_VSYNCS", "PS2X_VU1_ENGINE",
                "PS2X_MTVU", "PS2X_MTVU_LAG", "PS2X_VU1_BLOCKS", "PS2X_VU0_RECOMP",
                "PS2X_VU0_DIRECT", "PS2X_VU1_FLAG_ELIDE", "PS2X_GS_HANDOFF_DIET",
                "PS2X_GS_FINISH_TIMING", "PS2X_SSX3_SIM_MODE", "PS2X_SKIP_MOVIE",
                "PS2X_SOUND", "PS2X_PERF_LOG", "PS2X_PERF_LOG_DIR",
                "PS2X_GAME_THREAD_CPUS", "PS2X_MTVU_CPUS",
            };
            for (const char *key : want)
            {
                bool found = false;
                for (size_t i = 0; i < n; ++i)
                    found = found || std::strcmp(defs[i].key, key) == 0;
                t.IsTrue(found, std::string("table holds ") + key);
            }
            // lagV is S3, not S1.
            for (size_t i = 0; i < n; ++i)
                t.IsFalse(std::strcmp(defs[i].key, "PS2X_VIF1_REVERSE_DMA") == 0, "no lagV in S1");
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
            t.IsTrue(n > 100, "over a hundred curated knobs");
            for (size_t i = 1; i < n; ++i)
                t.IsTrue(std::strcmp(knobs[i - 1], knobs[i]) < 0, "sorted and unique");
            t.IsTrue(ps2x::cf2KnobIsCurated("PS2X_SOUND"), "curated hit");
            t.IsFalse(ps2x::cf2KnobIsCurated("PS2X_VSYNC_RATE_LOG"), "diag left to the scan");
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

        tc.Run("P3-like dump stays under the logcat cap", [](TestCase &t)
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
            g.track("PS2X_GS_TURNIP");
            g.track("PGS_SKIP_COMPILATION_TASKS");
            g.track("PGS_PRECOMPILE_LIST");
            g.track("PGS_PRECOMPILE_THREADS");
            g.track("PGS_PIPELINE_CACHE");
            clearDefaults();
            ps2x::cf2ApplyAndroidDefaults("/storage/emulated/0/Android/data/com.ps2x.runner/files/SLUS_207.72");
            ::setenv("PS2X_CD_IMAGE",
                     "/storage/emulated/0/Android/data/com.ps2x.runner/files/SSX3.iso", 1);
            ::setenv("PS2X_PAD_SCRIPT", "/storage/emulated/0/Android/data/com.ps2x.runner/files/I26-FAST.txt", 1);
            ::setenv("PS2X_VSYNC_RATE_LOG", "1", 1);
            ::setenv("PS2X_UNPACED", "1", 1);
            ::setenv("PS2X_DETERMINISTIC", "1", 1);
            ::setenv("PS2X_DET_HASH_EVERY", "5", 1);
            // The 5 inert P3 lines (present in the real P3 leg, absent in empty).
            ::setenv("PS2X_GS_TURNIP", "0", 1);
            ::setenv("PGS_SKIP_COMPILATION_TASKS", "1", 1);
            ::setenv("PGS_PRECOMPILE_LIST",
                     "/storage/emulated/0/Android/data/com.ps2x.runner/files/variants-odin.txt", 1);
            ::setenv("PGS_PRECOMPILE_THREADS", "2", 1);
            ::setenv("PGS_PIPELINE_CACHE",
                     "/storage/emulated/0/Android/data/com.ps2x.runner/files/pcache-sc1.bin", 1);
            const std::string line = ps2x::cf2BuildKnobsLine();
            // logd truncates past ~4 KB (LOGGER_ENTRY_MAX_PAYLOAD 4076); the
            // real P3 leg prints 3939 bytes. Trip here before growth truncates.
            t.IsTrue(line.size() < 4000, "dump under 4000 bytes");
            t.IsTrue(line.find(" PS2X_GS_BACKEND=external") != std::string::npos, "default visible");
        });
    });
}
