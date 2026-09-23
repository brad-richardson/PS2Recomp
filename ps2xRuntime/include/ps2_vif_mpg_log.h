// E39 DEV-ONLY VIF1 MPG upload log behind PS2X_VIF_MPG_LOG=<file>.
//
// Master gate: PS2X_VIF_MPG_LOG names the text file receiving one line per
// decoded VIF1 MPG command. Unset/empty (default) = one relaxed atomic
// check per MPG; zero guest-visible behavior change, no I/O.
// Optional binds: PS2X_VIF_MPG_LOG_FROM / PS2X_VIF_MPG_LOG_TO (inclusive
// guest-vsync window; default 0..2^64-1, vsync from GS vsyncTick).
// Hard cap: 20,000 lines, then the file goes quiet.
//
// One line per MPG carries: guest vsync, full 16-bit imm, num, linear dest
// byte range (imm*8 .. imm*8+bytes), outcome under the current interpreter
// semantics (copied / drop_addr / drop_partial / clipped), payload bytes
// available in this processVIF1Data buffer, FNV-1a/32 of the available
// payload bytes, and the lower/upper VU-code words covering microcode slots
// 2 and 8 when covered ("-" when the slot is outside this upload).
//
// Slot words use the PCSX2 masked destination ((imm*8) & 0x3FFF) with
// per-instruction wrap, so a drop_addr line still shows the words it WOULD
// have written (for in-range uploads the masked mapping is identical to the
// linear one). When the slot is covered but its bytes are not in this
// buffer, the slot prints "short" instead of "-".

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

namespace ps2_vif_mpg_log
{

inline constexpr uint64_t kMaxLines = 20000ull;
inline constexpr uint64_t kFlushEvery = 128ull;
inline constexpr uint32_t kVu1CodeSize = 16u * 1024u;
inline constexpr uint32_t kNoOffset = 0xFFFFFFFFu;

inline uint32_t fnv1a32(const uint8_t *data, uint32_t size)
{
    uint32_t hash = 0x811C9DC5u;
    for (uint32_t i = 0u; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

// Payload offset of VU-code slot `slot` (8 bytes each) under the masked,
// per-instruction-wrapped destination, or kNoOffset when the upload's
// [maskedDest, maskedDest+mpgBytes) range (mod 16K) does not cover it.
inline uint32_t slotPayloadOffset(uint32_t maskedDest, uint32_t mpgBytes, uint32_t slot)
{
    const uint32_t slotByte = slot * 8u;
    const uint32_t rel = (slotByte + kVu1CodeSize - (maskedDest & (kVu1CodeSize - 1u))) & (kVu1CodeSize - 1u);
    if (rel + 8u > mpgBytes)
        return kNoOffset;
    return rel;
}

namespace detail
{

    struct State
    {
        std::mutex mutex;
        bool initDone = false;
        bool enabled = false;
        std::string path;
        uint64_t from = 0u;
        uint64_t to = ~0ull;
        std::ofstream out;
        bool outOpen = false;
        bool capped = false;
        uint64_t linesWritten = 0u;
    };

    inline State &state()
    {
        static State s;
        return s;
    }

    inline std::atomic<bool> &initDone()
    {
        static std::atomic<bool> done{false};
        return done;
    }

    inline std::atomic<bool> &enabledFlag()
    {
        static std::atomic<bool> on{false};
        return on;
    }

    inline bool parseU64(const char *text, uint64_t &out)
    {
        if (!text || text[0] == '\0')
        {
            return false;
        }
        uint64_t value = 0u;
        for (const char *p = text; *p != '\0'; ++p)
        {
            if (*p < '0' || *p > '9')
            {
                return false;
            }
            value = value * 10u + static_cast<uint64_t>(*p - '0');
        }
        out = value;
        return true;
    }

    inline void initLocked(State &s)
    {
        s.initDone = true;
        const char *file = std::getenv("PS2X_VIF_MPG_LOG");
        if (!file || file[0] == '\0')
        {
            return;
        }
        s.path = file;
        uint64_t from = 0u;
        uint64_t to = ~0ull;
        if (const char *env = std::getenv("PS2X_VIF_MPG_LOG_FROM"))
        {
            if (!parseU64(env, from))
            {
                return;
            }
        }
        if (const char *env = std::getenv("PS2X_VIF_MPG_LOG_TO"))
        {
            if (!parseU64(env, to))
            {
                return;
            }
        }
        s.from = from;
        s.to = to;
        s.enabled = true;
        enabledFlag().store(true, std::memory_order_relaxed);
    }

    inline void ensureInit()
    {
        if (initDone().load(std::memory_order_relaxed))
        {
            return;
        }
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.initDone)
        {
            initDone().store(true, std::memory_order_relaxed);
            return;
        }
        initLocked(s);
        initDone().store(true, std::memory_order_relaxed);
    }

    inline void openLocked(State &s)
    {
        if (s.outOpen || s.capped || s.path.empty())
        {
            return;
        }
        s.out.open(s.path, std::ios::out | std::ios::trunc);
        s.outOpen = s.out.is_open();
        if (!s.outOpen)
        {
            s.capped = true;
        }
    }

} // namespace detail

inline bool enabled()
{
    detail::ensureInit();
    return detail::enabledFlag().load(std::memory_order_relaxed);
}

// One line per MPG. slot words are printed only when the corresponding
// hasSlot flag holds; otherwise the slot prints "-" (not covered) or
// "short" (covered, bytes missing from this buffer).
inline void note(uint64_t vsync, uint32_t imm, uint32_t num,
                 uint32_t destStart, uint32_t destEnd, const char *outcome,
                 uint32_t availBytes, uint32_t fnv,
                 bool hasSlot2, uint32_t slot2Lo, uint32_t slot2Hi, bool slot2Short,
                 bool hasSlot8, uint32_t slot8Lo, uint32_t slot8Hi, bool slot8Short)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || s.capped)
    {
        return;
    }
    if (vsync < s.from || vsync > s.to)
    {
        return;
    }
    detail::openLocked(s);
    if (!s.outOpen)
    {
        return;
    }
    char s2[32];
    if (hasSlot2)
        std::snprintf(s2, sizeof(s2), "%08x %08x", slot2Lo, slot2Hi);
    else if (slot2Short)
        std::snprintf(s2, sizeof(s2), "short");
    else
        std::snprintf(s2, sizeof(s2), "-");
    char s8[32];
    if (hasSlot8)
        std::snprintf(s8, sizeof(s8), "%08x %08x", slot8Lo, slot8Hi);
    else if (slot8Short)
        std::snprintf(s8, sizeof(s8), "short");
    else
        std::snprintf(s8, sizeof(s8), "-");
    char line[256];
    std::snprintf(line, sizeof(line),
                  "mpg vsync=%llu imm=%u num=%u dest=%u-%u outcome=%s avail=%u fnv=%08x slot2=%s slot8=%s",
                  static_cast<unsigned long long>(vsync), imm, num,
                  destStart, destEnd, outcome, availBytes, fnv, s2, s8);
    s.out << line << '\n';
    ++s.linesWritten;
    // Boot harnesses SIGTERM the runner at the wall cap, which skips static
    // destructors and drops the final stdio buffer. Flush periodically so a
    // killed run loses at most kFlushEvery lines instead of the whole buffer.
    if ((s.linesWritten % kFlushEvery) == 0u)
    {
        s.out.flush();
    }
    if (s.linesWritten >= kMaxLines)
    {
        s.capped = true;
    }
}

inline bool configureForTest(const char *path, uint64_t from = 0u, uint64_t to = ~0ull)
{
    if (!path || path[0] == '\0')
    {
        return false;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.outOpen)
    {
        s.out.close();
        s.outOpen = false;
    }
    s.path = path;
    s.from = from;
    s.to = to;
    s.linesWritten = 0u;
    s.capped = false;
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.enabled = true;
    detail::enabledFlag().store(true, std::memory_order_relaxed);
    return true;
}

inline void clearForTest()
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.outOpen)
    {
        s.out.close();
        s.outOpen = false;
    }
    s.path.clear();
    s.from = 0u;
    s.to = ~0ull;
    s.linesWritten = 0u;
    s.capped = false;
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.enabled = false;
    detail::enabledFlag().store(false, std::memory_order_relaxed);
}

} // namespace ps2_vif_mpg_log
