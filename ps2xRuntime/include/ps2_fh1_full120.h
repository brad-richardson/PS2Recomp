// FH1 full-120 mode (local branch full120; PS2X_SSX3_FULL120=1, default off).
//
// The whole SSX 3 update runs 120 times per guest second with dt 1/120
// (RV13 option A) and every update renders, so the output is native 120 fps:
//   - guest VBlank runs at 2 x 59.94 Hz (EeScheduler period / 2, pacer / 2):
//     each VBlank is one wake -> one producer record -> one app update -> one
//     render -> one present, i.e. two updates and two presents per stock
//     VBlank period. The render gate, flip and present stay the game's own.
//   - manager A=[0x4a5b64]: rate A+0x10 60 -> 120 and dt A+0x14 1/60 -> 1/120,
//     patched once at the manager-init hook (TM3's site). Multiplier and
//     accumulator stay stock (one record per wake).
//   - EE clock x PS2X_SSX3_FULL120_EE_X (1|2|4, default 2): checkpoints charge
//     1/EE_X cycles, so a 120 Hz frame has a stock frame's EE budget. EE
//     timers, COP0 Count, alarms and the sound tick all run off the charged
//     m_eeCycle, so they keep their stock rate in guest seconds (PCSX2's EE
//     cycle-rate setting does the same).
//   - host-side vsync-time consumers keep stock time: the pad-script vsync
//     clock and the CD field clock count ticks / 2; the [vsync-rate] log
//     reports stock-equivalent vs/s.
// Fix groups (PS2X_SSX3_FULL120_FIX=comma list | all; default none) patch
// single-reader per-step constants at the same hook (TS1's list, RV13 §2).
// Knob off: every entry point is one cached-bool test; no guest effect.
#pragma once

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ps2_fh1
{
inline bool enabled() noexcept
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_SSX3_FULL120");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}

// Guest VBlanks per stock VBlank period.
inline uint32_t vblankDivisor() noexcept
{
    return enabled() ? 2u : 1u;
}

// log2 of the EE clock multiplier (charged cycles = cycles >> shift).
inline uint32_t eeClockShift() noexcept
{
    static const uint32_t shift = [] {
        if (!enabled())
            return 0u;
        const char *v = std::getenv("PS2X_SSX3_FULL120_EE_X");
        if (!v || !*v)
            return 1u;
        if (std::strcmp(v, "1") == 0)
            return 0u;
        if (std::strcmp(v, "2") == 0)
            return 1u;
        if (std::strcmp(v, "4") == 0)
            return 2u;
        std::fprintf(stderr, "fh1-full120-refused PS2X_SSX3_FULL120_EE_X=%s (want 1|2|4)\n", v);
        std::abort();
    }();
    return shift;
}

enum Fix : uint32_t
{
    kFixRider = 1u << 0,     // K1-K4: rider integrators' private dt (class h)
    kFixCountdown = 1u << 1, // C1-C4 + solver quantum/epsilon/residual/gap (class c)
    kFixDrag = 1u << 2,      // 1139a0 per-call drag (a) and z increments (b)
    kFixEvent = 1u << 3,     // 10496c event-node step (e/h)
    kFixSlew = 1u << 4,      // 114124 mode-0 return slew (a/j)
};

inline uint32_t fixMask() noexcept
{
    static const uint32_t mask = [] {
        const char *v = std::getenv("PS2X_SSX3_FULL120_FIX");
        if (!enabled() || !v || !*v)
            return 0u;
        const std::string s(v);
        if (s == "all")
            return kFixRider | kFixCountdown | kFixDrag | kFixEvent | kFixSlew;
        uint32_t m = 0u;
        size_t at = 0u;
        while (at <= s.size())
        {
            const size_t comma = s.find(',', at);
            const std::string item = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            if (item == "rider") m |= kFixRider;
            else if (item == "countdown") m |= kFixCountdown;
            else if (item == "drag") m |= kFixDrag;
            else if (item == "event") m |= kFixEvent;
            else if (item == "slew") m |= kFixSlew;
            else if (!item.empty())
            {
                std::fprintf(stderr, "fh1-full120-refused PS2X_SSX3_FULL120_FIX item=%s\n", item.c_str());
                std::abort();
            }
            if (comma == std::string::npos)
                break;
            at = comma + 1u;
        }
        return m;
    }();
    return mask;
}

inline constexpr uint32_t kMgrPtr = 0x4a5b64u;   // gp+0x2a74 -> A
inline constexpr uint32_t kHookSite = 0x316da8u; // jalr after 0x316d88/0x316d98 store dt/rate
inline constexpr uint32_t kUpdateTarget = 0x2306b8u;
inline constexpr uint32_t kRenderTarget = 0x22b008u;
inline constexpr uint32_t kSixtieth = 0x3c888889u;   // float(1/60)
inline constexpr uint32_t kHundredTwentieth = 0x3c088889u; // float(1/120)

inline bool rd32(const uint8_t *ram, uint32_t addr, uint32_t &out) noexcept
{
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (!ram || phys + 4u > PS2_RAM_SIZE)
        return false;
    std::memcpy(&out, ram + phys, 4);
    return true;
}

inline bool wr32(uint8_t *ram, uint32_t addr, uint32_t val) noexcept
{
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (!ram || phys + 4u > PS2_RAM_SIZE)
        return false;
    std::memcpy(ram + phys, &val, 4);
    return true;
}

struct Word
{
    uint32_t fix; // 0 = always (manager)
    uint32_t address;
    uint32_t expected;
    uint32_t replacement;
    const char *label;
};

// Manager rate/dt plus TS1's single-coherent word table (TS1 REPORT G0,
// ps2_ts1_mode.h @ ts1 420c34a), minus TS1's multiplier: here the doubled
// VBlank supplies the second update. Every word is verified before any write.
inline void patchAtManagerInit(uint8_t *ram)
{
    const char *sim = std::getenv("PS2X_SSX3_SIM_MODE");
    if (sim && std::strncmp(sim, "split", 5) == 0)
    {
        std::fprintf(stderr, "fh1-full120-refused PS2X_SSX3_SIM_MODE=%s (full120 replaces split120)\n", sim);
        std::abort();
    }
    uint32_t a = 0u;
    if (!rd32(ram, kMgrPtr, a) || !a || (a & 3u) || (a & 0x1FFFFFFFu) + 0x60u > PS2_RAM_SIZE)
    {
        std::fprintf(stderr, "fh1-full120-refused manager ptr=%08x\n", a);
        std::abort();
    }
    const std::array<Word, 21> words = {{
        {0u, a + 0x10u, 60u, 120u, "rate"},
        {0u, a + 0x14u, kSixtieth, kHundredTwentieth, "dt"},
        {0u, a + 0x24u, 0x3f800000u, 0x3f800000u, "mult(stock)"},
        {kFixRider, 0x49be9cu, kSixtieth, kHundredTwentieth, "K1"},
        {kFixRider, 0x49c08cu, kSixtieth, kHundredTwentieth, "K2"},
        {kFixRider, 0x49c12cu, kSixtieth, kHundredTwentieth, "K3"},
        {kFixRider, 0x49b828u, kSixtieth, kHundredTwentieth, "K4"},
        {kFixCountdown, 0x49b480u, kSixtieth, kHundredTwentieth, "C1"},
        {kFixCountdown, 0x49b48cu, kSixtieth, kHundredTwentieth, "C2"},
        {kFixCountdown, 0x49b4a0u, kSixtieth, kHundredTwentieth, "C3"},
        {kFixCountdown, 0x49bf1cu, kSixtieth, kHundredTwentieth, "C4"},
        {kFixCountdown, 0x49b494u, kSixtieth, kHundredTwentieth, "quantum"},
        {kFixCountdown, 0x49b498u, 0x3c23d70au, 0x3ba3d70au, "epsilon"},
        {kFixCountdown, 0x49b49cu, 0x426fffffu, 0x42efffffu, "residual_recip"},
        {kFixCountdown, 0x49bf20u, kSixtieth, kHundredTwentieth, "gap_floor"},
        {kFixCountdown, 0x49bf24u, 0x426fffffu, 0x42efffffu, "gap_recip"},
        {kFixDrag, 0x49b4a4u, 0xbb5a740fu, 0xbadaa2bdu, "damping"},
        {kFixDrag, 0x49b4a8u, 0xc1fd5556u, 0xc17d5556u, "z_up"},
        {kFixDrag, 0x49b4acu, 0xc162aaabu, 0xc0e2aaabu, "z_down"},
        {kFixEvent, 0x49b1b0u, kSixtieth, kHundredTwentieth, "event_step_10496c"},
        // 0x114124 applies this fraction of the remaining R+0x214 gap once per
        // update: alpha_half = 1-sqrt(1-alpha_stock).
        {kFixSlew, 0x49b4f0u, 0x3d2aa635u, 0x3cac76f5u, "mode0_slew_114124"},
    }};
    const uint32_t mask = fixMask();
    size_t written = 0u;
    for (const Word &w : words)
    {
        if (w.fix != 0u && (mask & w.fix) == 0u)
            continue;
        uint32_t got = 0u;
        if (!rd32(ram, w.address, got) || got != w.expected)
        {
            std::fprintf(stderr, "fh1-full120-refused word=%s addr=%08x got=%08x expected=%08x\n",
                         w.label, w.address, got, w.expected);
            std::abort();
        }
    }
    for (const Word &w : words)
    {
        if (w.fix != 0u && (mask & w.fix) == 0u)
            continue;
        wr32(ram, w.address, w.replacement);
        ++written;
    }
    std::fprintf(stderr, "fh1-full120-armed manager=%08x rate=120 dt=%08x vblank_div=%u ee_x=%u fix=0x%x words=%zu\n",
                 a, kHundredTwentieth, vblankDivisor(), 1u << eeClockShift(), mask, written);
}

// ---- Observation tap (PS2X_FH1_TAP=1; env-only, any build) ----------------
// One line per PS2X_FH1_TAP_EVERY guest VBlanks (default 60): update and
// render dispatch counts, manager ordinals, the rider snapshot position
// 0x5409c0 (TM2) and optional words (PS2X_FH1_PEEK=addr[:f|:u|:x][N], with
// '@ptr+off' for one dereference). PS2X_FH1_COUNT=pc,... adds dispatch
// counts for those targets.
struct Tap
{
    bool on = false;
    uint32_t every = 60u;
    uint32_t updates = 0u;
    uint32_t renders = 0u;
    std::array<uint32_t, 16> countPcs{};
    std::array<uint32_t, 16> counts{};
    size_t nCount = 0u;
    struct Peek
    {
        bool deref = false;
        uint32_t base = 0u;
        uint32_t off = 0u;
        char fmt = 'x';
        uint32_t n = 1u;
    };
    std::array<Peek, 16> peeks{};
    size_t nPeek = 0u;
};

inline Tap &tap()
{
    static Tap t = [] {
        Tap r;
        const char *on = std::getenv("PS2X_FH1_TAP");
        r.on = on && on[0] == '1';
        if (!r.on)
            return r;
        if (const char *e = std::getenv("PS2X_FH1_TAP_EVERY"))
            r.every = std::max<uint32_t>(1u, static_cast<uint32_t>(std::strtoul(e, nullptr, 0)));
        if (const char *c = std::getenv("PS2X_FH1_COUNT"))
        {
            const char *p = c;
            while (*p && r.nCount < r.countPcs.size())
            {
                char *end = nullptr;
                r.countPcs[r.nCount++] = static_cast<uint32_t>(std::strtoul(p, &end, 0));
                if (end == p || *end != ',')
                    break;
                p = end + 1;
            }
        }
        if (const char *s = std::getenv("PS2X_FH1_PEEK"))
        {
            const char *p = s;
            while (*p && r.nPeek < r.peeks.size())
            {
                Tap::Peek k;
                if (*p == '@')
                {
                    k.deref = true;
                    ++p;
                }
                char *end = nullptr;
                k.base = static_cast<uint32_t>(std::strtoul(p, &end, 0));
                p = end;
                if (*p == '+')
                {
                    k.off = static_cast<uint32_t>(std::strtoul(p + 1, &end, 0));
                    p = end;
                }
                if (*p == ':')
                {
                    k.fmt = p[1];
                    p += 2;
                    if (*p >= '1' && *p <= '9')
                    {
                        k.n = static_cast<uint32_t>(std::strtoul(p, &end, 10));
                        p = end;
                    }
                }
                r.peeks[r.nPeek++] = k;
                if (*p != ',')
                    break;
                ++p;
            }
        }
        return r;
    }();
    return t;
}

inline bool tapOn() noexcept
{
    return tap().on;
}

// Set once the manager words are converted (EE thread only).
inline bool g_patched = false;

// EE thread, every guest dispatch while enabled() or the tap is on.
inline void onBranch(uint8_t *ram, uint32_t sourcePc, uint32_t targetPc)
{
    if (enabled() && sourcePc == kHookSite && !g_patched)
    {
        g_patched = true;
        patchAtManagerInit(ram);
    }
    Tap &t = tap();
    if (!t.on)
        return;
    if (targetPc == kUpdateTarget)
        ++t.updates;
    else if (targetPc == kRenderTarget)
        ++t.renders;
    for (size_t i = 0; i < t.nCount; ++i)
        if (t.countPcs[i] == targetPc)
            ++t.counts[i];
}

// EE thread, at VBlankStart after the tick advanced. A state loaded from a
// stock run (SS1) skips the init hook: convert at the first VBlank that finds
// the manager at stock rate/dt instead.
inline void onVBlank(uint8_t *ram, uint64_t tick)
{
    if (enabled() && !g_patched)
    {
        uint32_t a = 0u, rate = 0u, dt = 0u;
        if (rd32(ram, kMgrPtr, a) && a && !(a & 3u) && rd32(ram, a + 0x10u, rate) &&
            rd32(ram, a + 0x14u, dt) && rate == 60u && dt == kSixtieth)
        {
            g_patched = true;
            std::fprintf(stderr, "fh1-full120-late-arm tick=%llu\n", static_cast<unsigned long long>(tick));
            patchAtManagerInit(ram);
        }
    }
    Tap &t = tap();
    if (!t.on || (tick % t.every) != 0u)
        return;
    char line[1536];
    int n = 0;
    uint32_t a = 0u, wake = 0u, upd = 0u, rate = 0u, dt = 0u;
    rd32(ram, kMgrPtr, a);
    if (a)
    {
        rd32(ram, a + 0x18u, wake);
        rd32(ram, a + 0x1cu, upd);
        rd32(ram, a + 0x10u, rate);
        rd32(ram, a + 0x14u, dt);
    }
    float pos[3] = {};
    for (uint32_t i = 0; i < 3u; ++i)
    {
        uint32_t w = 0u;
        rd32(ram, 0x5409c0u + 4u * i, w);
        std::memcpy(&pos[i], &w, 4);
    }
    n += std::snprintf(line + n, sizeof(line) - n,
                       "fh1-tap tick=%llu upd=%u rend=%u A18=%u A1c=%u rate=%u dt=%08x pos=%.3f,%.3f,%.3f",
                       static_cast<unsigned long long>(tick), t.updates, t.renders, wake, upd, rate, dt,
                       pos[0], pos[1], pos[2]);
    t.updates = t.renders = 0u;
    for (size_t i = 0; i < t.nCount && n < static_cast<int>(sizeof(line)) - 32; ++i)
    {
        n += std::snprintf(line + n, sizeof(line) - n, " c%x=%u", t.countPcs[i], t.counts[i]);
        t.counts[i] = 0u;
    }
    for (size_t i = 0; i < t.nPeek && n < static_cast<int>(sizeof(line)) - 64; ++i)
    {
        const Tap::Peek &k = t.peeks[i];
        uint32_t addr = k.base;
        if (k.deref && !rd32(ram, k.base, addr))
            addr = 0u;
        addr += k.off;
        n += std::snprintf(line + n, sizeof(line) - n, " %s%x=", k.deref ? "@" : "", addr);
        for (uint32_t j = 0; j < k.n && n < static_cast<int>(sizeof(line)) - 24; ++j)
        {
            uint32_t w = 0u;
            rd32(ram, addr + 4u * j, w);
            float f = 0.0f;
            std::memcpy(&f, &w, 4);
            if (k.fmt == 'f')
                n += std::snprintf(line + n, sizeof(line) - n, "%s%.4f", j ? "," : "", f);
            else if (k.fmt == 'u')
                n += std::snprintf(line + n, sizeof(line) - n, "%s%u", j ? "," : "", w);
            else
                n += std::snprintf(line + n, sizeof(line) - n, "%s%08x", j ? "," : "", w);
        }
    }
    std::fprintf(stderr, "%s\n", line);
}
} // namespace ps2_fh1
