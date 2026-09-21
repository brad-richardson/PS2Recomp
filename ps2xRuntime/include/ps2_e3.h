// E3b one-or-two-frame order capture (frontier §E3 recipe, E3-3 rows R1-R4).
//
// Header-only diag module. Master gate: PS2X_E3_INV=<invocation ordinal>
// (0-based count of 0x362DE8 dispatches). Unset/invalid = every tap compiles
// to a single cached-bool check; zero guest-visible behavior change.
// Optional binds: PS2X_E3_S1BASE (expected steady s1 base, checked not
// assumed), PS2X_E3_BYTES (E3 emitted-byte cap, default 4 MiB).
//
// One shared u64 seq domain (e3SeqNext) stamps every R1-R4 row. Frame =
// last VBLANK tick noted from EeScheduler::processEvent. Thread = last id
// noted at the scheduler switch site (same site as ps2DiagWatchSetThread).
// Overlap math normalizes RAM/KSEG aliases + SPR offsets (mirroring
// ps2ResolveGuestPointer, wrap-safe) and splits wrapped DMA intervals.
// Constraint C1: observation only -- no tap writes guest state.
#pragma once

#include "runtime/ps2_memory.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace ps2_e3
{

inline constexpr uint32_t kRamSize = 32u * 1024u * 1024u;
inline constexpr uint32_t kSprSize = 16u * 1024u;
inline constexpr uint64_t kDefaultByteCap = 4ull * 1024ull * 1024ull;
inline constexpr uint32_t kNoSpace = 0u;
inline constexpr uint32_t kRamSpace = 1u;
inline constexpr uint32_t kSprSpace = 2u;

// ---- tiny pure parsers (unit-tested) ----

inline bool parseU64(const char *text, uint64_t &out)
{
    if (!text || !text[0])
    {
        return false;
    }
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (!end || end == text || *end != '\0')
    {
        return false;
    }
    out = static_cast<uint64_t>(parsed);
    return true;
}

// ---- cached env binds ----

inline uint64_t targetInvRaw()
{
    static const uint64_t target = [] {
        uint64_t parsed = 0u;
        if (const char *env = std::getenv("PS2X_E3_INV"))
        {
            if (parseU64(env, parsed))
            {
                return parsed;
            }
        }
        return static_cast<uint64_t>(~0ull); // sentinel: disabled
    }();
    return target;
}

inline bool enabled()
{
    return targetInvRaw() != static_cast<uint64_t>(~0ull);
}

inline uint64_t targetInv()
{
    return targetInvRaw();
}

inline uint32_t expectedS1Base()
{
    static const uint32_t base = [] {
        uint64_t parsed = 0u;
        if (const char *env = std::getenv("PS2X_E3_S1BASE"))
        {
            if (parseU64(env, parsed))
            {
                return static_cast<uint32_t>(parsed);
            }
        }
        return 0u;
    }();
    return base;
}

inline uint64_t byteCap()
{
    static const uint64_t cap = [] {
        uint64_t parsed = 0u;
        if (const char *env = std::getenv("PS2X_E3_BYTES"))
        {
            if (parseU64(env, parsed) && parsed > 0u)
            {
                return parsed;
            }
        }
        return kDefaultByteCap;
    }();
    return cap;
}

// ---- shared state (relaxed atomics; emits serialized by e3EmitMutex) ----

inline std::atomic<uint64_t> &seqStorage()
{
    static std::atomic<uint64_t> seq{0u};
    return seq;
}

inline uint64_t seqNext()
{
    return seqStorage().fetch_add(1u, std::memory_order_relaxed);
}

inline std::atomic<uint64_t> &frameStorage()
{
    static std::atomic<uint64_t> frame{0u};
    return frame;
}

inline void noteVBlank(uint64_t tick)
{
    if (!enabled())
    {
        return;
    }
    frameStorage().store(tick, std::memory_order_relaxed);
}

inline uint64_t frame()
{
    return frameStorage().load(std::memory_order_relaxed);
}

inline std::atomic<int> &threadStorage()
{
    static std::atomic<int> tid{-999};
    return tid;
}

inline void noteThread(int id)
{
    if (!enabled())
    {
        return;
    }
    threadStorage().store(id, std::memory_order_relaxed);
}

inline int threadId()
{
    return threadStorage().load(std::memory_order_relaxed);
}

inline std::atomic<uint64_t> &invStorage()
{
    static std::atomic<uint64_t> inv{0u};
    return inv;
}

inline std::atomic<bool> &spanDoneStorage()
{
    static std::atomic<bool> done{false};
    return done;
}

inline std::mutex &emitMutex()
{
    static std::mutex m;
    return m;
}

inline std::atomic<uint64_t> &byteCountStorage()
{
    static std::atomic<uint64_t> n{0u};
    return n;
}

inline std::atomic<uint64_t> &suppressedStorage()
{
    static std::atomic<uint64_t> n{0u};
    return n;
}

inline std::atomic<bool> &capLineStorage()
{
    static std::atomic<bool> done{false};
    return done;
}

// Single-insertion cerr write with byte-cap accounting. Returns false when
// the line was suppressed by the cap (counted, not emitted).
inline bool emitLine(const char *line, size_t len)
{
    std::lock_guard<std::mutex> lock(emitMutex());
    const uint64_t cap = byteCap();
    const uint64_t have = byteCountStorage().load(std::memory_order_relaxed);
    if (have + static_cast<uint64_t>(len) > cap)
    {
        suppressedStorage().fetch_add(1u, std::memory_order_relaxed);
        if (!capLineStorage().exchange(true, std::memory_order_relaxed))
        {
            char buf[256];
            const int w = std::snprintf(buf, sizeof(buf),
                                        "[e3:byte-cap] cap=%llu emitted=%llu suppressed=1\n",
                                        static_cast<unsigned long long>(cap),
                                        static_cast<unsigned long long>(have));
            std::cerr << buf;
            byteCountStorage().fetch_add(static_cast<uint64_t>(w) > 0 ? static_cast<uint64_t>(w) : 0u,
                                          std::memory_order_relaxed);
        }
        return false;
    }
    std::cerr << line;
    byteCountStorage().fetch_add(static_cast<uint64_t>(len), std::memory_order_relaxed);
    return true;
}

inline bool emitStr(const std::string &line)
{
    return emitLine(line.c_str(), line.size());
}

inline uint64_t suppressed()
{
    return suppressedStorage().load(std::memory_order_relaxed);
}

inline uint64_t bytesEmitted()
{
    return byteCountStorage().load(std::memory_order_relaxed);
}

// ---- address normalization (mirrors ps2ResolveGuestPointer exactly, so the
// overlap-test space always equals the writer space; wrap-safe, no false
// negatives). Pure; unit-tested. ----

struct NormAddr
{
    uint32_t space = kNoSpace; // kRamSpace / kSprSpace (kNoSpace unused: degenerate maps to RAM 0 like the writers)
    uint32_t off = 0u;
};

inline NormAddr normAddr(uint32_t addr)
{
    NormAddr out;
    // Scratchpad first (raw + 0x8xxxxxxx-bit alias form, same as ps2IsScratchpadAddress).
    if (addr >= PS2_SCRATCHPAD_BASE && addr < PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE)
    {
        out.space = kSprSpace;
        out.off = addr - PS2_SCRATCHPAD_BASE;
        return out;
    }
    if ((addr & 0x80000000u) != 0u)
    {
        const uint32_t lower = addr & 0x7FFFFFFFu;
        if (lower >= PS2_SCRATCHPAD_BASE && lower < PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE)
        {
            out.space = kSprSpace;
            out.off = lower - PS2_SCRATCHPAD_BASE;
            return out;
        }
    }
    uint32_t phys = 0u;
    if (addr < 0x20000000u)
    {
        phys = addr;
    }
    else if ((addr >= 0x20000000u && addr < 0x40000000u) || (addr >= 0x80000000u && addr < 0xC0000000u))
    {
        phys = addr & 0x1FFFFFFFu;
    }
    if (phys >= kRamSize)
    {
        phys &= (kRamSize - 1u);
    }
    out.space = kRamSpace;
    out.off = phys;
    return out;
}

struct Interval
{
    uint32_t space = kNoSpace;
    uint32_t off = 0u;
    uint64_t len = 0u; // >0, single space, no wrap
};

// Split [guestAddr, guestAddr+len) into single-space no-wrap intervals.
// Pure; unit-tested. Returns interval count (<= kMaxIntervals).
inline constexpr size_t kMaxIntervals = 4u;

inline size_t splitIntervals(uint32_t guestAddr, uint64_t len, Interval *out, size_t cap)
{
    if (len == 0u || !out || cap == 0u)
    {
        return 0u;
    }
    size_t n = 0u;
    uint64_t cur = guestAddr;
    const uint64_t end = cur + len;
    while (cur < end && n < cap)
    {
        const NormAddr na = normAddr(static_cast<uint32_t>(cur));
        // Next guest-addr boundary where the mapping rule may change: SPR
        // range edges + strip-range edges + 0x80000000-bit edge.
        uint64_t next = end;
        static const uint64_t kEdges[] = {
            0x20000000ull, 0x40000000ull, 0x70000000ull, 0x70004000ull, 0x80000000ull, 0xC0000000ull,
        };
        for (const uint64_t edge : kEdges)
        {
            if (edge > cur && edge < next)
            {
                next = edge;
            }
        }
        // Space wrap boundary.
        const uint32_t spaceSize = (na.space == kSprSpace) ? kSprSize : kRamSize;
        const uint64_t toWrap = static_cast<uint64_t>(spaceSize) - static_cast<uint64_t>(na.off);
        if (toWrap < next - cur)
        {
            next = cur + toWrap;
        }
        // Degenerate ranges (phys 0 for the whole rule region): collapse to
        // RAM[0, chunk) -- over-approximate, safe direction.
        if (toWrap == 0u)
        {
            break; // unreachable (off < spaceSize always), guards div-by-zero readers
        }
        out[n].space = na.space;
        out[n].off = na.off;
        out[n].len = next - cur;
        ++n;
        cur = next;
    }
    return n;
}

// ---- watch windows (PS2X_DIAG_WATCH verbatim parse, normalized once) ----

struct Win
{
    uint32_t addr = 0u; // as listed (reported form)
    uint32_t space = kNoSpace;
    uint32_t off = 0u; // normalized offset; window covers [off, off+8)
};

inline const std::vector<Win> &watchWins()
{
    static const std::vector<Win> wins = [] {
        std::vector<Win> out;
        if (const char *env = std::getenv("PS2X_DIAG_WATCH"))
        {
            std::string s(env);
            size_t pos = 0u;
            while (pos <= s.size())
            {
                const size_t comma = s.find(',', pos);
                std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                size_t a = 0u;
                while (a < tok.size() && std::isspace(static_cast<unsigned char>(tok[a])))
                {
                    ++a;
                }
                size_t b = tok.size();
                while (b > a && std::isspace(static_cast<unsigned char>(tok[b - 1u])))
                {
                    --b;
                }
                if (b > a)
                {
                    char *e = nullptr;
                    const unsigned long parsed = std::strtoul(tok.substr(a, b - a).c_str(), &e, 0);
                    if (e && *e == '\0')
                    {
                        const uint32_t addr = static_cast<uint32_t>(parsed);
                        const NormAddr na = normAddr(addr);
                        Win w;
                        w.addr = addr;
                        w.space = na.space;
                        w.off = na.off;
                        out.push_back(w);
                    }
                }
                if (comma == std::string::npos)
                {
                    break;
                }
                pos = comma + 1u;
            }
        }
        return out;
    }();
    return wins;
}

// Split an in-space (off,len) at the space wrap boundary. Used by the SPR
// engine tap: the engine wraps MADR/SADR inside RAM/SPR space explicitly
// (ps2_memory.cpp chunk loop), while the generic guest-addr mapping falls
// through to RAM 0 past 0x70004000 -- the writers genuinely differ, so the
// engine tap passes explicit space intervals instead of (guestAddr,len).
// Pure; unit-tested.
inline size_t splitSpace(uint32_t space, uint32_t off, uint64_t len, Interval *out, size_t cap)
{
    if (len == 0u || !out || cap == 0u)
    {
        return 0u;
    }
    const uint32_t spaceSize = (space == kSprSpace) ? kSprSize : kRamSize;
    const uint32_t o = (space == kSprSpace) ? (off & (kSprSize - 1u)) : (off & (kRamSize - 1u));
    const uint64_t first = static_cast<uint64_t>(spaceSize) - o;
    if (len <= first)
    {
        out[0].space = space;
        out[0].off = o;
        out[0].len = len;
        return 1u;
    }
    out[0].space = space;
    out[0].off = o;
    out[0].len = first;
    if (cap < 2u)
    {
        return 1u;
    }
    // Multi-wrap transfers (len > 2*space) revisit bytes; the covered SET is
    // the whole space, reported as [0, spaceSize) plus the first chunk.
    const uint64_t rest = len - first;
    out[1].space = space;
    out[1].off = 0u;
    out[1].len = rest > spaceSize ? spaceSize : rest;
    return 2u;
}

// Pure interval-vs-windows overlap; appends overlapped window indices.
// Unit-tested.
inline size_t overlapWins(uint32_t space,
                          uint32_t off,
                          uint64_t len,
                          const Win *wins,
                          size_t nwins,
                          size_t *out,
                          size_t cap)
{
    size_t n = 0u;
    if (len == 0u || !wins || !out)
    {
        return 0u;
    }
    const uint64_t end = static_cast<uint64_t>(off) + len;
    for (size_t i = 0u; i < nwins; ++i)
    {
        if (wins[i].space != space)
        {
            continue;
        }
        const uint64_t w0 = wins[i].off;
        const uint64_t w1 = w0 + 8u;
        if (static_cast<uint64_t>(off) < w1 && w0 < end)
        {
            if (n < cap)
            {
                out[n] = i;
            }
            ++n;
        }
    }
    return n;
}

// ---- arming: invocation ordinals [target, target+1] (0-based 362DE8 count) ----

inline bool spanComplete()
{
    return spanDoneStorage().load(std::memory_order_relaxed);
}

inline bool armed()
{
    if (!enabled() || spanComplete())
    {
        return false;
    }
    const uint64_t inv = invStorage().load(std::memory_order_relaxed);
    const uint64_t t = targetInv();
    return inv >= t && inv <= t + 1u;
}

// Called on every 0x362DE8 dispatch. Returns the entering ordinal (0-based).
inline uint64_t noteInvEntry()
{
    if (!enabled())
    {
        return invStorage().load(std::memory_order_relaxed);
    }
    const uint64_t ord = invStorage().fetch_add(1u, std::memory_order_relaxed);
    const uint64_t t = targetInv();
    if (ord == t)
    {
        char buf[128];
        const int w = std::snprintf(buf, sizeof(buf), "[e3:armed] inv=%llu target=%llu\n",
                                    static_cast<unsigned long long>(ord), static_cast<unsigned long long>(t));
        emitLine(buf, static_cast<size_t>(w > 0 ? w : 0));
    }
    else if (ord == t + 2u)
    {
        spanDoneStorage().store(true, std::memory_order_relaxed);
        char buf[256];
        const int w = std::snprintf(
            buf, sizeof(buf), "[e3:span-complete] inv=%llu seq=%llu frame=%llu bytes=%llu suppressed=%llu\n",
            static_cast<unsigned long long>(ord), static_cast<unsigned long long>(seqNext()),
            static_cast<unsigned long long>(frame()), static_cast<unsigned long long>(bytesEmitted()),
            static_cast<unsigned long long>(suppressed()));
        emitLine(buf, static_cast<size_t>(w > 0 ? w : 0));
    }
    return ord;
}

// ---- window slice reads (same mapping as the writers: getMemPtr) ----

inline bool readWinSlice(uint8_t *rdram, uint32_t winAddr, uint8_t out[8])
{
    if (!rdram || !out)
    {
        return false;
    }
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        const uint8_t *p = getConstMemPtr(rdram, winAddr + i);
        if (!p)
        {
            return false;
        }
        out[i] = *p;
    }
    return true;
}

inline void hex8(const uint8_t b[8], char out[17])
{
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 8; ++i)
    {
        out[2 * i] = kHex[(b[i] >> 4) & 0xFu];
        out[2 * i + 1] = kHex[b[i] & 0xFu];
    }
    out[16] = '\0';
}

// ---- R3 host-write tap: begin (before-slices) / end (after + rows) ----

struct TapWin
{
    uint32_t addr = 0u;
    uint8_t before[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool beforeOk = false;
};

struct Tap
{
    bool active = false;
    uint32_t guestAddr = 0u;
    uint64_t len = 0u;
    std::vector<TapWin> wins;
};

inline Tap tapBeginImpl(uint8_t *rdram, uint32_t guestAddr, uint64_t len, const std::vector<Win> &wins)
{
    Tap t;
    t.guestAddr = guestAddr;
    t.len = len;
    if (!rdram || len == 0u || wins.empty())
    {
        return t;
    }
    Interval ivs[kMaxIntervals];
    const size_t niv = splitIntervals(guestAddr, len, ivs, kMaxIntervals);
    for (size_t k = 0u; k < niv; ++k)
    {
        size_t idx[128];
        const size_t n = overlapWins(ivs[k].space, ivs[k].off, ivs[k].len, wins.data(), wins.size(), idx, 128u);
        const size_t m = n < 128u ? n : 128u;
        for (size_t j = 0u; j < m; ++j)
        {
            const Win &w = wins[idx[j]];
            bool dup = false;
            for (const TapWin &have : t.wins)
            {
                if (have.addr == w.addr)
                {
                    dup = true;
                    break;
                }
            }
            if (dup)
            {
                continue;
            }
            TapWin tw;
            tw.addr = w.addr;
            tw.beforeOk = readWinSlice(rdram, w.addr, tw.before);
            t.wins.push_back(tw);
        }
    }
    t.active = !t.wins.empty();
    return t;
}

inline Tap tapBegin(uint8_t *rdram, uint32_t guestAddr, uint64_t len)
{
    if (!armed())
    {
        Tap t;
        t.guestAddr = guestAddr;
        t.len = len;
        return t;
    }
    return tapBeginImpl(rdram, guestAddr, len, watchWins());
}

// Explicit-interval variant for writers whose space/wrap rule differs from
// the generic guest-addr mapping (the SPR engine). guestAddr/len are the
// reported (programmed) values; overlap uses ivs.
inline Tap tapBeginIntervals(uint8_t *rdram, uint32_t guestAddr, uint64_t len, const Interval *ivs, size_t niv)
{
    Tap t;
    t.guestAddr = guestAddr;
    t.len = len;
    if (!armed() || !rdram || !ivs)
    {
        return t;
    }
    const std::vector<Win> &wins = watchWins();
    if (wins.empty())
    {
        return t;
    }
    for (size_t k = 0u; k < niv; ++k)
    {
        size_t idx[128];
        const size_t n = overlapWins(ivs[k].space, ivs[k].off, ivs[k].len, wins.data(), wins.size(), idx, 128u);
        const size_t m = n < 128u ? n : 128u;
        for (size_t j = 0u; j < m; ++j)
        {
            const Win &w = wins[idx[j]];
            bool dup = false;
            for (const TapWin &have : t.wins)
            {
                if (have.addr == w.addr)
                {
                    dup = true;
                    break;
                }
            }
            if (dup)
            {
                continue;
            }
            TapWin tw;
            tw.addr = w.addr;
            tw.beforeOk = readWinSlice(rdram, w.addr, tw.before);
            t.wins.push_back(tw);
        }
    }
    t.active = !t.wins.empty();
    return t;
}

inline void emitR3Win(uint64_t seq,
                      uint64_t fr,
                      int tid,
                      const char *tag,
                      uint32_t dst,
                      uint64_t size,
                      uint32_t win,
                      const uint8_t before[8],
                      bool beforeOk,
                      const uint8_t after[8],
                      bool afterOk,
                      const char *extra)
{
    char bhex[17];
    char ahex[17];
    hex8(before, bhex);
    hex8(after, ahex);
    char buf[768];
    const int w = std::snprintf(
        buf, sizeof(buf),
        "[e3:r3] seq=%llu frame=%llu thread=%d tag=%s dst=0x%x size=%llu win=0x%x before=%s after=%s x=%s\n",
        static_cast<unsigned long long>(seq), static_cast<unsigned long long>(fr), tid, tag ? tag : "?",
        dst, static_cast<unsigned long long>(size), win, beforeOk ? bhex : "WILD", afterOk ? ahex : "WILD",
        extra ? extra : "-");
    if (w > 0)
    {
        emitLine(buf, static_cast<size_t>(w));
    }
}

inline void tapEnd(Tap &&t, const char *tag, uint8_t *rdram, const char *extra)
{
    if (!t.active)
    {
        return;
    }
    for (const TapWin &tw : t.wins)
    {
        uint8_t after[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        const bool afterOk = readWinSlice(rdram, tw.addr, after);
        emitR3Win(seqNext(), frame(), threadId(), tag, t.guestAddr, t.len, tw.addr, tw.before, tw.beforeOk,
                  after, afterOk, extra);
    }
}

// ---- R2 guest-write row (old/new + src for macro/store dedupe) ----

inline void emitR2(uint32_t src, // 0=macro 1=store 2=direct
                   uint32_t addr,
                   uint32_t width,
                   uint64_t oldLo,
                   uint64_t oldHi,
                   uint64_t newLo,
                   uint64_t newHi,
                   uint32_t pc,
                   int tid,
                   uint32_t ra,
                   uint32_t sp)
{
    static const char *kSrc[] = {"macro", "store", "direct"};
    const char *s = (src < 3u) ? kSrc[src] : "?";
    char buf[512];
    const int w = std::snprintf(buf, sizeof(buf),
                                "[e3:r2] seq=%llu frame=%llu thread=%d src=%s addr=0x%x width=%u "
                                "oldlo=0x%llx oldhi=0x%llx newlo=0x%llx newhi=0x%llx pc=0x%x ra=0x%x sp=0x%x\n",
                                static_cast<unsigned long long>(seqNext()),
                                static_cast<unsigned long long>(frame()), tid, s, addr, width,
                                static_cast<unsigned long long>(oldLo), static_cast<unsigned long long>(oldHi),
                                static_cast<unsigned long long>(newLo), static_cast<unsigned long long>(newHi),
                                pc, ra, sp);
    if (w > 0)
    {
        emitLine(buf, static_cast<size_t>(w));
    }
}

// Width-aware wrap-safe old-value gather (RAM + SPR via getMemPtr).
inline bool readOld(uint8_t *rdram, uint32_t addr, uint32_t width, uint64_t &lo, uint64_t &hi)
{
    lo = 0u;
    hi = 0u;
    if (!rdram || (width != 1u && width != 2u && width != 4u && width != 8u && width != 16u))
    {
        return false;
    }
    uint8_t bytes[16] = {0};
    for (uint32_t i = 0u; i < width; ++i)
    {
        const uint8_t *p = getConstMemPtr(rdram, addr + i);
        if (!p)
        {
            return false;
        }
        bytes[i] = *p;
    }
    for (uint32_t i = 0u; i < 8u && i < width; ++i)
    {
        lo |= static_cast<uint64_t>(bytes[i]) << (8u * i);
    }
    for (uint32_t i = 8u; i < width; ++i)
    {
        hi |= static_cast<uint64_t>(bytes[i]) << (8u * (i - 8u));
    }
    return true;
}

// Overlap check for single guest stores (R2): normalized, wrap-safe.
inline bool storeOverlaps(uint32_t addr, uint32_t width)
{
    const std::vector<Win> &wins = watchWins();
    if (wins.empty())
    {
        return false;
    }
    Interval ivs[kMaxIntervals];
    const size_t niv = splitIntervals(addr, width, ivs, kMaxIntervals);
    for (size_t k = 0u; k < niv; ++k)
    {
        size_t idx[8];
        if (overlapWins(ivs[k].space, ivs[k].off, ivs[k].len, wins.data(), wins.size(), idx, 8u) > 0u)
        {
            return true;
        }
    }
    return false;
}

// ---- R1 record row ----

inline void emitR1(uint64_t seqEntry,
                   uint64_t n,
                   uint64_t inv,
                   uint32_t a0,
                   uint32_t a1,
                   uint32_t a2,
                   uint32_t s1entry,
                   uint32_t ra,
                   uint32_t srcPc,
                   int32_t k,
                   uint32_t h10pre,
                   uint32_t h1cpre,
                   uint32_t h1epre,
                   bool preOk,
                   bool called,
                   uint32_t s1call,
                   uint32_t h10post,
                   uint32_t h1cpost,
                   uint32_t h1epost,
                   bool postOk,
                   const char *base)
{
    char buf[768];
    const int w = std::snprintf(
        buf, sizeof(buf),
        "[e3:r1] seq=%llu seqEntry=%llu frame=%llu thread=%d n=%llu inv=%llu a0=0x%x a1=0x%x a2=0x%x "
        "s1e=0x%x ra=0x%x src=0x%x k=%d h10p=0x%x h1cp=0x%x h1ep=0x%x preok=%d out=%s s1c=0x%x "
        "h10o=0x%x h1co=0x%x h1eo=0x%x postok=%d base=%s\n",
        static_cast<unsigned long long>(seqNext()), static_cast<unsigned long long>(seqEntry),
        static_cast<unsigned long long>(frame()), threadId(), static_cast<unsigned long long>(n),
        static_cast<unsigned long long>(inv), a0, a1, a2, s1entry, ra, srcPc, k, h10pre, h1cpre, h1epre,
        preOk ? 1 : 0, called ? "call" : "skip", s1call, h10post, h1cpost, h1epost, postOk ? 1 : 0,
        base ? base : "?");
    if (w > 0)
    {
        emitLine(buf, static_cast<size_t>(w));
    }
}

// ---- R4 boundary rows ----

inline void emitR4(uint64_t inv, const char *fn, uint32_t ra, uint32_t srcPc)
{
    char buf[256];
    const int w = std::snprintf(buf, sizeof(buf), "[e3:r4] seq=%llu frame=%llu thread=%d inv=%llu fn=%s ev=enter ra=0x%x src=0x%x\n",
                                static_cast<unsigned long long>(seqNext()),
                                static_cast<unsigned long long>(frame()), threadId(),
                                static_cast<unsigned long long>(inv), fn ? fn : "?", ra, srcPc);
    if (w > 0)
    {
        emitLine(buf, static_cast<size_t>(w));
    }
}

inline void emitR4Sum(uint64_t inv, uint64_t nrec, uint32_t s1min, uint32_t s1max, uint32_t nbases, bool drift)
{
    char buf[256];
    const int w = std::snprintf(buf, sizeof(buf),
                                "[e3:r4sum] seq=%llu frame=%llu thread=%d inv=%llu nrec=%llu s1min=0x%x s1max=0x%x nbases=%u drift=%d\n",
                                static_cast<unsigned long long>(seqNext()),
                                static_cast<unsigned long long>(frame()), threadId(),
                                static_cast<unsigned long long>(inv), static_cast<unsigned long long>(nrec),
                                s1min, s1max, nbases, drift ? 1 : 0);
    if (w > 0)
    {
        emitLine(buf, static_cast<size_t>(w));
    }
}

} // namespace ps2_e3
