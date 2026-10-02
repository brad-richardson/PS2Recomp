// UV1 DEV-ONLY per-vsync UNPACK-format + DMA-stall counters.
//
// PS2X_VIF_FMT_LOG=1   -> one [uv1-vif] line per guest vsync to stderr:
//   UNPACK count, counts per (vn,vl) format, and usn/mask/tops/num0/mode/
//   fill/skip/(cl,wl)-pair/raw-zero flag counts. One note() call per decoded
//   VIF1 UNPACK command (counts commands, not vectors).
// PS2X_DMA_STALL_LOG=1 -> one [uv1-dma] line per guest vsync to stderr:
//   per-channel chain kicks, REFS (tag id 4) tags, kicks with D_CTRL STS/STD
//   (bits 4:1) set, and the distinct raw D_CTRL values seen at kicks
//   ("unwritten" when the guest never wrote D_CTRL).
//
// Lag-one emission: a vsync's line prints when activity for a later vsync
// arrives, so a SIGTERM-stopped boot loses only its last active vsync; idle
// vsyncs between active ones print as "idle". Default off: one static-bool
// check per UNPACK/tag, zero guest-visible behaviour change, no I/O.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace ps2_uv1_vif_fmt
{

inline bool enabled()
{
    static const bool on = [] {
        if (const char *env = std::getenv("PS2X_VIF_FMT_LOG"))
            return env[0] == '1' && env[1] == '\0';
        return false;
    }();
    return on;
}

namespace detail
{

struct ClWlPair
{
    uint32_t cl = 0u;
    uint32_t wl = 0u;
    uint64_t n = 0u;
};

struct State
{
    std::mutex mutex;
    bool have = false;
    uint64_t cur = 0u;
    uint64_t n = 0u;
    uint64_t fmt[16] = {};
    uint64_t usn = 0u;
    uint64_t mask = 0u;
    uint64_t tops = 0u;
    uint64_t num0 = 0u;
    uint64_t mode[4] = {};
    uint64_t fill = 0u;
    uint64_t skip = 0u;
    uint64_t rawCl0 = 0u;
    uint64_t rawWl0 = 0u;
    ClWlPair pairs[8];
    uint32_t nPairs = 0u;
    uint64_t morePairs = 0u;
};

inline State &state()
{
    static State s;
    return s;
}

inline const char *fmtName(unsigned vn, unsigned vl)
{
    static const char *names[16] = {
        "S_32", "S_16", "S_8", "S_5",
        "V2_32", "V2_16", "V2_8", "V2_5",
        "V3_32", "V3_16", "V3_8", "V3_5",
        "V4_32", "V4_16", "V4_8", "V4_5",
    };
    return names[(vn & 3u) * 4u + (vl & 3u)];
}

inline void resetLocked(State &s)
{
    s.n = 0u;
    for (unsigned i = 0u; i < 16u; ++i)
        s.fmt[i] = 0u;
    s.usn = s.mask = s.tops = s.num0 = 0u;
    for (unsigned i = 0u; i < 4u; ++i)
        s.mode[i] = 0u;
    s.fill = s.skip = 0u;
    s.rawCl0 = s.rawWl0 = 0u;
    s.nPairs = 0u;
    s.morePairs = 0u;
}

inline void emitLocked(State &s, uint64_t vsync)
{
    if (s.n == 0u)
    {
        std::fprintf(stderr, "[uv1-vif] vsync=%llu idle\n",
                     static_cast<unsigned long long>(vsync));
        return;
    }
    std::fprintf(stderr, "[uv1-vif] vsync=%llu n=%llu",
                 static_cast<unsigned long long>(vsync),
                 static_cast<unsigned long long>(s.n));
    for (unsigned i = 0u; i < 16u; ++i)
    {
        if (s.fmt[i] != 0u)
            std::fprintf(stderr, " %s=%llu", fmtName(i / 4u, i % 4u),
                         static_cast<unsigned long long>(s.fmt[i]));
    }
    std::fprintf(stderr, " usn=%llu mask=%llu tops=%llu num0=%llu mode=%llu/%llu/%llu/%llu",
                 static_cast<unsigned long long>(s.usn),
                 static_cast<unsigned long long>(s.mask),
                 static_cast<unsigned long long>(s.tops),
                 static_cast<unsigned long long>(s.num0),
                 static_cast<unsigned long long>(s.mode[0]),
                 static_cast<unsigned long long>(s.mode[1]),
                 static_cast<unsigned long long>(s.mode[2]),
                 static_cast<unsigned long long>(s.mode[3]));
    std::fprintf(stderr, " fill=%llu skip=%llu",
                 static_cast<unsigned long long>(s.fill),
                 static_cast<unsigned long long>(s.skip));
    std::fputs(" clwl=", stderr);
    for (uint32_t i = 0u; i < s.nPairs; ++i)
        std::fprintf(stderr, "%s%ux%u:%llu", i == 0u ? "" : ",",
                     s.pairs[i].cl, s.pairs[i].wl,
                     static_cast<unsigned long long>(s.pairs[i].n));
    if (s.morePairs != 0u)
        std::fprintf(stderr, " +moreclwl=%llu",
                     static_cast<unsigned long long>(s.morePairs));
    std::fprintf(stderr, " raw0=%llu/%llu\n",
                 static_cast<unsigned long long>(s.rawCl0),
                 static_cast<unsigned long long>(s.rawWl0));
}

} // namespace detail

// One call per decoded VIF1 UNPACK. rawCl/rawWl are the STCYCL register
// bytes (0 = unwritten-cleared, clamped to 1 by the interpreter).
inline void note(uint64_t vsync, uint8_t opcode, bool usn, uint32_t mode,
                 uint32_t rawCl, uint32_t rawWl, bool tops, uint8_t num)
{
    if (!enabled())
        return;
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.have)
    {
        s.have = true;
        s.cur = vsync;
    }
    while (s.cur < vsync)
    {
        detail::emitLocked(s, s.cur);
        detail::resetLocked(s);
        ++s.cur;
    }
    const unsigned vn = (opcode >> 2) & 3u;
    const unsigned vl = opcode & 3u;
    ++s.n;
    ++s.fmt[vn * 4u + vl];
    if (usn)
        ++s.usn;
    if ((opcode & 0x10u) != 0u)
        ++s.mask;
    if (tops)
        ++s.tops;
    if (num == 0u)
        ++s.num0;
    ++s.mode[mode & 3u];
    const uint32_t cl = (rawCl == 0u) ? 1u : rawCl;
    const uint32_t wl = (rawWl == 0u) ? 1u : rawWl;
    if (cl < wl)
        ++s.fill;
    else if (cl > wl)
        ++s.skip;
    if (rawCl == 0u)
        ++s.rawCl0;
    if (rawWl == 0u)
        ++s.rawWl0;
    uint32_t i = 0u;
    while (i < s.nPairs && (s.pairs[i].cl != cl || s.pairs[i].wl != wl))
        ++i;
    if (i < s.nPairs)
        ++s.pairs[i].n;
    else if (s.nPairs < 8u)
        s.pairs[s.nPairs++] = detail::ClWlPair{cl, wl, 1u};
    else
        ++s.morePairs;
}

} // namespace ps2_uv1_vif_fmt

namespace ps2_uv1_dma_stall
{

inline bool enabled()
{
    static const bool on = [] {
        if (const char *env = std::getenv("PS2X_DMA_STALL_LOG"))
            return env[0] == '1' && env[1] == '\0';
        return false;
    }();
    return on;
}

namespace detail
{

inline constexpr uint32_t kVif0 = 0x10008000u;
inline constexpr uint32_t kVif1 = 0x10009000u;
inline constexpr uint32_t kGif = 0x1000A000u;
inline constexpr uint32_t kStsStdMask = 0x1Eu; // D_CTRL bits 4:1

struct State
{
    std::mutex mutex;
    bool have = false;
    uint64_t cur = 0u;
    uint64_t chains[4] = {};
    uint64_t refs[4] = {};
    uint64_t stall[4] = {};
    uint32_t dctrl[8] = {};
    uint32_t nDctrl = 0u;
    bool dctrlUnwritten = false;
    uint64_t moreDctrl = 0u;
};

inline State &state()
{
    static State s;
    return s;
}

inline unsigned chanIndex(uint32_t channelBase)
{
    if (channelBase == kVif0)
        return 0u;
    if (channelBase == kVif1)
        return 1u;
    if (channelBase == kGif)
        return 2u;
    return 3u;
}

inline const char *chanName(unsigned i)
{
    static const char *names[4] = {"vif0", "vif1", "gif", "oth"};
    return names[i & 3u];
}

inline void resetLocked(State &s)
{
    for (unsigned i = 0u; i < 4u; ++i)
        s.chains[i] = s.refs[i] = s.stall[i] = 0u;
    s.nDctrl = 0u;
    s.dctrlUnwritten = false;
    s.moreDctrl = 0u;
}

inline bool idleLocked(const State &s)
{
    for (unsigned i = 0u; i < 4u; ++i)
    {
        if (s.chains[i] != 0u || s.refs[i] != 0u)
            return false;
    }
    return true;
}

inline void emitLocked(State &s, uint64_t vsync)
{
    if (idleLocked(s))
    {
        std::fprintf(stderr, "[uv1-dma] vsync=%llu idle\n",
                     static_cast<unsigned long long>(vsync));
        return;
    }
    std::fprintf(stderr, "[uv1-dma] vsync=%llu",
                 static_cast<unsigned long long>(vsync));
    for (unsigned i = 0u; i < 4u; ++i)
    {
        if (s.chains[i] != 0u || s.refs[i] != 0u)
            std::fprintf(stderr, " %s=%llu/%llu/%llu", chanName(i),
                         static_cast<unsigned long long>(s.chains[i]),
                         static_cast<unsigned long long>(s.refs[i]),
                         static_cast<unsigned long long>(s.stall[i]));
    }
    std::fputs(" dctrl=", stderr);
    bool any = false;
    for (uint32_t i = 0u; i < s.nDctrl; ++i)
    {
        std::fprintf(stderr, "%s0x%x", any ? "," : "", s.dctrl[i]);
        any = true;
    }
    if (s.dctrlUnwritten)
    {
        std::fprintf(stderr, "%sunwritten", any ? "," : "");
        any = true;
    }
    if (!any)
        std::fputs("-", stderr);
    if (s.moreDctrl != 0u)
        std::fprintf(stderr, " +moredctrl=%llu",
                     static_cast<unsigned long long>(s.moreDctrl));
    std::fputs("\n", stderr);
}

inline void advanceLocked(State &s, uint64_t vsync)
{
    if (!s.have)
    {
        s.have = true;
        s.cur = vsync;
    }
    while (s.cur < vsync)
    {
        emitLocked(s, s.cur);
        resetLocked(s);
        ++s.cur;
    }
}

} // namespace detail

// One call per chain-mode (MOD=1) DMA kick. dctrl is the D_CTRL register
// value (0x1000E000); dctrlPresent=false when the guest never wrote it.
inline void noteKick(uint64_t vsync, uint32_t channelBase, uint32_t dctrl,
                     bool dctrlPresent)
{
    if (!enabled())
        return;
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    detail::advanceLocked(s, vsync);
    const unsigned i = detail::chanIndex(channelBase);
    ++s.chains[i];
    if (dctrlPresent && (dctrl & detail::kStsStdMask) != 0u)
        ++s.stall[i];
    if (!dctrlPresent)
    {
        s.dctrlUnwritten = true;
        return;
    }
    for (uint32_t k = 0u; k < s.nDctrl; ++k)
    {
        if (s.dctrl[k] == dctrl)
            return;
    }
    if (s.nDctrl < 8u)
        s.dctrl[s.nDctrl++] = dctrl;
    else
        ++s.moreDctrl;
}

// One call per chain tag walked (any id; id==4 REFS counted).
inline void noteTag(uint64_t vsync, uint32_t channelBase, uint32_t id)
{
    if (!enabled())
        return;
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    detail::advanceLocked(s, vsync);
    if (id == 4u)
        ++s.refs[detail::chanIndex(channelBase)];
}

} // namespace ps2_uv1_dma_stall
