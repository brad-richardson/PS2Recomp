// E44 DEV-ONLY scratchpad write watch behind PS2X_E44_TRACE=<file>.
//
// Watches the 8 words at 0x70000000..0x7000000c and 0x70000500..0x7000050c
// (the w0..w3 inputs of the two mode-6 items on PCSX2), plus up to 8
// env-configured extra EE words (PS2X_E44_EXTRA="0x<addr>[,...]", for the
// Boot-B follow-up on a DMA source word).
//
// Part 4 (PS2X_E44_APPEND=1): render-list append watch for
// sub_00376938. (a) At the count-increment store (pc 0x3797E8, same
// basic block as the 0x3797EC append copy, post-store) logs the T60
// app line (count, t0, tw0..tw4, s4, t1, raw ra). (b) Write-watches
// the template words, logging T60 tpl lines for tw0/tw1 changes.
// Amendment: tplm value filter (amendment 2: exact mode 6), cap 200;
// per-vsync appsum aggregation (ungated counters, cap 3000 lines);
// the apc appender-pc census over the EE item region (ungated,
// uncapped). app/tpl/tplrearm grammars match T60 exactly for
// diffing; tplm/appsum/apc are E44-only.
// Amendment 2 (T61/T62 parity): appx filtered log at the three copy
// sites (tick >= 1270, count<=1 or mode 6, cap 300) and the v1b0
// value watch ((lane & 0x3FF) == 0x1B0, cap 200, ungated).
// Amendment 3 (T62/T63 parity): tw change-only watch over the four
// 0x14 templates (whole boot, no per-word cap, 2000 lines).
// EE canonicalization folds every RAM segment (0x00/0x20/0x30/0x80/
// 0xA0) via PS2_RAM_MASK, so UCAB (0x30) stores match extras.
//
// Master gate: PS2X_E44_TRACE names the text file receiving every line.
// Unset/empty (default) = one relaxed atomic check per tap; zero
// guest-visible behavior change, no I/O.
// Optional binds: PS2X_E44_FROM / PS2X_E44_TO (inclusive guest-vsync
// window; default 1270..1280 — the settled window from E43), vsync = the
// shared E41 VBlank mirror via lastVsyncTick().
// Hard caps: first 64 lines per watched word, then quiet for that word
// (16 words x 64 = 1024 lines max).
//
// Line kind:
//   spw vsync=<t> addr=0x<> value=0x<> via=<store8|store16|store32|store64|
//     store128|fast|mem-write|spr-dma> src=0x<EE|->
//     pc=0x<> ra=0x<> fn=<...> a0=.. a3=.. v0=.. v1=.. t0=..t9 s0..s7
//     One per watched word overlapped by the store (value = the word read
//     back after the store, so sub-word stores report the containing
//     word). via=store* are guest CPU stores via PS2Runtime::Store*
//     (the single choke point: statically-resolved special-address
//     stores, WRITE-macro-routed special stores, and all SDL/SDR/SWL/SWR
//     merges land here); via=fast is a Ps2FastWrite* hit on a watched
//     word (unreachable by construction — static special addresses emit
//     Store*, dynamic ones route through the WRITE macros — so any line
//     here is a codegen bug); via=mem-write is a host-side
//     PS2Memory::write* that bypassed Store* (suppressed while a Store*
//     tap holds the guard, so no double lines); via=spr-dma is an SPR_TO
//     (RAM -> scratchpad) DMA copy, with src= the EE RAM address the
//     word was copied from.
//
// Tap call sites (all default-off, all post-store so read-back is live):
//   PS2Runtime::Store8/16/32/64/128 (ps2_runtime.cpp) — full regs.
//   Ps2FastWrite8/16/32/64/128 (ps2_runtime_macros.h) — value from args.
//   PS2Memory::write8/16/32/64/128 scratchpad branches (ps2_memory.cpp)
//     — ctx-less fallback, suppressed under the Store* guard.
//   SPR_TO memcpy (ps2_memory.cpp) — per-word EE src addresses.

#pragma once

#include "runtime/ps2_memory.h" // PS2_RAM_SIZE / PS2_RAM_MASK / scratchpad helpers
#include "ps2_runtime.h" // R5900Context / getRegU32
#include "ps2_e41_trace.h" // lastVsyncTick (shared VBlank mirror)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

namespace ps2_e44_trace
{

inline constexpr uint64_t kMaxLinesPerWord = 64ull;
inline constexpr int kFixedWords = 8;
inline constexpr int kMaxExtra = 8;
inline constexpr int kMaxWords = kFixedWords + kMaxExtra;
// Part-2 caps (total lines per kind).
inline constexpr uint64_t kMaxVif0UnkLines = 1024ull;
inline constexpr uint64_t kMaxVu0CallLines = 2000ull;
inline constexpr uint64_t kMaxSprModLines = 256ull;
// Boot C: last-writer readout lines.
inline constexpr uint64_t kMaxLastLines = 40ull;
inline constexpr uint32_t kItem0Base = 0x70000000u;
// Part 3: per-word-per-changed-vsync EE readout lines.
inline constexpr uint64_t kMaxEbLines = 400ull;
// Part 4: append-copy watch (sub_00376938 render-list append).
// kAppendCountPc is the count-increment sw, in the same basic block as
// the 0x3797EC ldl append copy (no branch between; the count>=2600
// beqz skips both). The WRITE-macro hook fires post-store, so the
// observed *a3 equals T60's 0x3797EC-hook count (post-increment), and
// $ra is logged raw (T60: constant 0x379780, verified not clobbered).
inline constexpr uint32_t kAppendCountPc = 0x3797E8u;
inline constexpr uint64_t kMaxAppLines = 300ull;
inline constexpr uint64_t kMaxTplLines = 300ull;
inline constexpr uint32_t kTplWords = 5u;
// Part 4 amendment (T60): tplm value filter (amendment 2: exact mode
// 6, ((new tw0 >> 6) & 0xF) == 6), per-vsync appsum aggregation, and
// the appender-pc census over the EE item region.
inline constexpr uint64_t kMaxTplmLines = 200ull;
// Part 4 amendment 2 (T61/T62): appx filtered log at the three copy
// sites (0 = 0x3797EC via trigger 0x3797E8, 1 = 0x37AD44 via trigger
// 0x37AD40, 2 = 0x37B474 via trigger 0x37B470; each trigger is the
// site's count-increment sw, same basic block as its copy), rows with
// count(v1)<=1 or mode 6, tick >= kAppxFromTick, cap 300; plus the
// v1b0 value watch (any store lane with (v & 0x3FF) == 0x1B0, cap 200,
// ungated).
inline constexpr uint32_t kAppxTrigger[3] = {0x3797E8u, 0x37AD40u, 0x37B470u};
inline constexpr uint64_t kMaxAppxLines = 300ull;
inline constexpr uint64_t kAppxFromTick = 1270ull;
inline constexpr uint64_t kMaxV1b0Lines = 200ull;
// Part 4 amendment 3 (T62/T63): change-only watch over the four
// 0x14 templates at kTwBase (tw0 of template 1 at +0x14 carries the
// SC mode word). Whole boot, no per-word cap, 2000 total lines.
inline constexpr uint32_t kTwBase = 0x61c8fcu;
inline constexpr uint32_t kTwSize = 0x50u;
inline constexpr uint32_t kTwWords = 20u;
inline constexpr uint64_t kMaxTwLines = 2000ull;
inline constexpr uint64_t kMaxAppSumLines = 3000ull;
inline constexpr uint32_t kApcBase = 0x809670u;
inline constexpr uint32_t kApcStride = 0x80u;
inline constexpr uint32_t kApcItems = 64u;
inline constexpr int kMaxApcPc = 64;

// Canonical watched scratchpad offsets (word-aligned).
inline constexpr uint32_t kFixedOffsets[kFixedWords] = {
    0x000u, 0x004u, 0x008u, 0x00Cu, 0x500u, 0x504u, 0x508u, 0x50Cu};

namespace detail
{
    // Last-writer record for item-0 words (Boot C): updated on every
    // watched store while enabled (no window, no cap); read out at the
    // item-0 walk. valid=false until the first tracked store.
    struct LastWriter
    {
        bool valid = false;
        uint64_t wtick = 0u;
        uint32_t value = 0u;
        uint32_t pc = 0u;
        uint32_t ra = 0u;
        char via[16] = {0};
        char fn[64] = {0};
        char src[24] = {0};
    };

    struct State
    {
        std::mutex mutex;
        bool initDone = false;
        bool enabled = false;
        std::string path;
        uint64_t from = 1270u;
        uint64_t to = 1280u;
        uint32_t extra[kMaxExtra] = {0u};
        int numExtra = 0;
        std::ofstream out;
        bool outOpen = false;
        uint64_t perWord[kMaxWords] = {0u};
        LastWriter lastW[kFixedWords];
        uint64_t lastLines = 0u;
        // Part 3: per-extra-word change record for ebwlast (one line per
        // word per vsync the word was written in). Flushed when the tick
        // advances (trailing tick may never flush); ebFlushTick is the
        // last tick a flush ran on.
        struct EbRec
        {
            bool dirty = false;
            uint64_t tick = 0u;
            uint32_t value = 0u;
            uint32_t pc = 0u;
            uint32_t ra = 0u;
            char via[16] = {0};
            char fn[64] = {0};
        };
        EbRec eb[kMaxExtra];
        uint64_t ebFlushTick = 0u;
        bool ebFlushValid = false;
        uint64_t ebLines = 0u;
        // Part-2: VIF0 kick aggregation (flush-on-tick-advance; the
        // trailing tick is flushed by the next kick or not at all).
        bool kickValid = false;
        uint64_t kickTick = 0u;
        uint64_t kickCount = 0u;
        uint64_t vif0UnkLines = 0u;
        uint64_t vu0CallLines = 0u;
        uint64_t sprModLines = 0u;
        // Part 4: append watch. appLines caps the (a) log; tplBase
        // arms the template write watch (0 = unarmed), with the last
        // seen tw0/tw1 for change detection, tplLines cap (shared with
        // tplrearm), and the tplm value-filtered cap.
        bool appendOn = false;
        uint64_t appLines = 0u;
        uint32_t tplBase = 0u;
        uint32_t tplLast0 = 0u;
        uint32_t tplLast1 = 0u;
        uint64_t tplLines = 0u;
        uint64_t tplmLines = 0u;
        // Part 4 amendment: per-vsync aggregation (ungated counters;
        // lines are capped). sumApp = append executions, sumTpl = tw0
        // /tw1 words changed, sumHist[m] over append tw0 with
        // m = (tw0>>6)&0xF. apcPc/apcCount = per-pc stores into the
        // EE item region for the in-progress tick.
        bool sumTickValid = false;
        uint64_t sumTick = 0u;
        uint64_t sumApp = 0u;
        uint64_t sumTpl = 0u;
        uint64_t sumHist[16] = {0u};
        uint64_t sumLines = 0u;
        uint32_t apcPc[kMaxApcPc] = {0u};
        uint32_t apcCount[kMaxApcPc] = {0u};
        int numApc = 0;
        // Part 4 amendment 2: appx/v1b0 line caps.
        uint64_t appxLines = 0u;
        uint64_t v1b0Lines = 0u;
        // Part 4 amendment 3: tw block watch (lazy baseline, then
        // change-only).
        bool twValid = false;
        uint32_t twLast[kTwWords] = {0u};
        uint64_t twLines = 0u;
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

    // Part 4 fast gates (atomics so the WRITE-macro checks stay cheap).
    inline std::atomic<bool> &appendOnFlag()
    {
        static std::atomic<bool> on{false};
        return on;
    }

    inline std::atomic<bool> &tplArmedFlag()
    {
        static std::atomic<bool> armed{false};
        return armed;
    }

    // Canonical RAM fold (Part 4 fix): every guest spelling of the same
    // EE word folds to the RAM offset. UCAB 0x30809670 used to fold with
    // & 0x1FFFFFFF to 0x10809670 and miss the 0x00809670 extra; with the
    // RAM mask all of 0x00/0x20/0x30/0x80/0xA0 spellings agree.
    // Non-RAM segments keep the legacy fold (no behavior change there).
    inline uint32_t ramCanon(uint32_t addr)
    {
        const uint32_t seg = addr >> 24u;
        if (seg == 0x00u || seg == 0x20u || seg == 0x30u || seg == 0x80u ||
            seg == 0xA0u)
        {
            return (addr & PS2_RAM_MASK) & ~3u;
        }
        return (addr & 0x1FFFFFFFu) & ~3u;
    }

    // Guard: Store* holds this across m_memory.write so the PS2Memory-level
    // fallback tap does not double-log Store-routed writes.
    inline bool &memSuppressed()
    {
        thread_local bool suppressed = false;
        return suppressed;
    }

    struct ScopedMemSuppress
    {
        bool active = false;
        explicit ScopedMemSuppress(bool on) : active(on)
        {
            if (on)
            {
                memSuppressed() = true;
            }
        }
        ~ScopedMemSuppress()
        {
            if (active)
            {
                memSuppressed() = false;
            }
        }
    };

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

    // Parses PS2X_E44_EXTRA: comma-separated numbers, strtoul base 0
    // (0x.. hex or decimal). Word-aligns each entry; caps at kMaxExtra.
    inline void parseExtraLocked(State &s, const char *text)
    {
        s.numExtra = 0;
        if (text == nullptr || text[0] == '\0')
        {
            return;
        }
        const char *p = text;
        while (*p != '\0' && s.numExtra < kMaxExtra)
        {
            while (*p == ' ' || *p == '\t' || *p == ',')
            {
                ++p;
            }
            if (*p == '\0')
            {
                break;
            }
            char *end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 0);
            if (end == p)
            {
                return;
            }
            s.extra[s.numExtra++] = static_cast<uint32_t>(v) & ~3u;
            p = end;
        }
    }

    inline void initLocked(State &s)
    {
        s.initDone = true;
        const char *file = std::getenv("PS2X_E44_TRACE");
        if (!file || file[0] == '\0')
        {
            return;
        }
        s.path = file;
        uint64_t from = 1270u, to = 1280u;
        if (const char *env = std::getenv("PS2X_E44_FROM"))
        {
            if (!parseU64(env, from))
            {
                return;
            }
        }
        if (const char *env = std::getenv("PS2X_E44_TO"))
        {
            if (!parseU64(env, to))
            {
                return;
            }
        }
        s.from = from;
        s.to = to;
        parseExtraLocked(s, std::getenv("PS2X_E44_EXTRA"));
        // Part 4: PS2X_E44_APPEND=1 arms the render-list append watch
        // (same trace file and FROM/TO window as the spw watch).
        if (const char *append = std::getenv("PS2X_E44_APPEND"))
        {
            if (append[0] == '1' && append[1] == '\0')
            {
                s.appendOn = true;
                appendOnFlag().store(true, std::memory_order_relaxed);
            }
        }
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

    // Watch index for a guest address, or -1. Fixed scratchpad words win
    // over extras (an extra aliasing a fixed word still logs once).
    inline int watchIndexLocked(const State &s, uint32_t addr)
    {
        if (ps2IsScratchpadAddress(addr))
        {
            const uint32_t off = ps2ScratchpadOffset(addr);
            for (int i = 0; i < kFixedWords; ++i)
            {
                if (off == kFixedOffsets[i])
                {
                    return i;
                }
            }
        }
        const uint32_t canon = ramCanon(addr);
        for (int i = 0; i < s.numExtra; ++i)
        {
            if (canon == ramCanon(s.extra[i]))
            {
                return kFixedWords + i;
            }
        }
        return -1;
    }

    // Reads one little-endian word back (scratchpad backing for
    // scratchpad addrs, rdram for RAM). False when unreadable.
    inline bool readWordBack(const uint8_t *rdram, uint32_t addr, uint32_t &out)
    {
        if (ps2IsScratchpadAddress(addr))
        {
            const uint8_t *sp = ps2GetScratchpadHostPtr();
            if (sp == nullptr)
            {
                return false;
            }
            const uint32_t off = ps2ScratchpadOffset(addr);
            if (off + 4u > PS2_SCRATCHPAD_SIZE)
            {
                return false;
            }
            std::memcpy(&out, sp + off, sizeof(out));
            return true;
        }
        if (rdram == nullptr)
        {
            return false;
        }
        const uint32_t off = (addr & 0x0FFFFFFFu) & PS2_RAM_MASK;
        if (off + 4u > PS2_RAM_SIZE)
        {
            return false;
        }
        std::memcpy(&out, rdram + off, sizeof(out));
        return true;
    }

    inline void emitLineLocked(State &s, const char *line)
    {
        if (!s.outOpen)
        {
            s.out.open(s.path, std::ios::out | std::ios::trunc);
            if (!s.out.is_open())
            {
                s.enabled = false;
                enabledFlag().store(false, std::memory_order_relaxed);
                return;
            }
            s.outOpen = true;
        }
        s.out << line << '\n';
    }

    // Part 3: flushes one ebwlast line per dirty EXTRA word for the
    // word's own tick (window-gated per tick; out-of-window ticks drop
    // silently). Call with s.mutex held, from every locked tap: the
    // flush rides the tap traffic, so no dedicated vsync hook is needed.
    // The trailing tick may never flush (wall-kill / quiet tail).
    inline void flushEbLocked(State &s, uint64_t tick)
    {
        if (s.ebFlushValid && s.ebFlushTick == tick)
        {
            return;
        }
        for (int i = 0; i < s.numExtra; ++i)
        {
            State::EbRec &eb = s.eb[i];
            if (!eb.dirty)
            {
                continue;
            }
            eb.dirty = false;
            if (eb.tick < s.from || eb.tick > s.to)
            {
                continue;
            }
            if (s.ebLines >= kMaxEbLines)
            {
                continue;
            }
            ++s.ebLines;
            char line[192];
            std::snprintf(line, sizeof(line),
                          "ebwlast vsync=%llu addr=0x%08x value=0x%08x via=%s pc=0x%08x ra=0x%08x fn=%s",
                          static_cast<unsigned long long>(eb.tick),
                          s.extra[i] & ~3u, eb.value, eb.via, eb.pc, eb.ra, eb.fn);
            emitLineLocked(s, line);
            if (!s.enabled)
            {
                return;
            }
        }
        s.ebFlushValid = true;
        s.ebFlushTick = tick;
    }

    // Part 3: records an EXTRA word change (call with s.mutex held).
    inline void ebNoteLocked(State &s, uint64_t tick, int extra,
                             uint32_t value, uint32_t pc, uint32_t ra,
                             const char *via, const char *fn)
    {
        if (extra < 0 || extra >= kMaxExtra)
        {
            return;
        }
        State::EbRec &eb = s.eb[extra];
        eb.dirty = true;
        eb.tick = tick;
        eb.value = value;
        eb.pc = pc;
        eb.ra = ra;
        std::snprintf(eb.via, sizeof(eb.via), "%s", via != nullptr ? via : "-");
        std::snprintf(eb.fn, sizeof(eb.fn), "%s", fn != nullptr ? fn : "-");
    }

} // namespace detail

// Hot-path gate: one relaxed atomic load when disabled.
inline bool enabled()
{
    detail::ensureInit();
    return detail::enabledFlag().load(std::memory_order_relaxed);
}

// Overlap pre-check for Store*/fast taps: enabled and [addr, addr+size)
// touches any watched word. Cheap compares first, atomics last.
inline bool storeArmed(uint32_t addr, uint32_t size)
{
    if (!enabled())
    {
        return false;
    }
    if (size == 0u)
    {
        return false;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
    {
        return false;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    // Part 3: flush pending EXTRA change lines on every armed check,
    // even out-of-window (emission is per-tick window-gated inside).
    detail::flushEbLocked(s, tick);
    if (tick < s.from || tick > s.to)
    {
        return false;
    }
    const uint32_t first = addr & ~3u;
    // 128-bit stores are the widest (16 bytes); cap the scan. A word is
    // overlapped when [w, w+4) meets [addr, addr+span).
    const uint32_t span = size > 16u ? 16u : size;
    const uint64_t end = static_cast<uint64_t>(addr) + span;
    for (uint32_t w = first; static_cast<uint64_t>(w) < end; w += 4u)
    {
        const int idx = detail::watchIndexLocked(s, w);
        if (idx >= 0 && s.perWord[idx] < kMaxLinesPerWord)
        {
            return true;
        }
    }
    return false;
}

// Shared overlap emitter. Call AFTER the store/DMA is visible. One line
// per watched word in [addr, addr+size); value = word read-back.
inline void emitOverlap(const uint8_t *rdram, const R5900Context *ctx,
                        uint32_t addr, uint32_t size, const char *via,
                        uint32_t srcBase, bool hasSrc, const char *fn)
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    if (tick < s.from || tick > s.to)
    {
        return;
    }
    detail::flushEbLocked(s, tick);
    const uint32_t first = addr & ~3u;
    const uint32_t span = size > 16u ? 16u : size;
    const uint64_t end = static_cast<uint64_t>(addr) + span;
    for (uint32_t w = first; static_cast<uint64_t>(w) < end; w += 4u)
    {
        const int idx = detail::watchIndexLocked(s, w);
        if (idx < 0)
        {
            continue;
        }
        uint32_t value = 0u;
        const bool ok = detail::readWordBack(rdram, w, value);
        const uint32_t pc = (ctx != nullptr) ? ctx->pc : 0u;
        const uint32_t ra = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
        // Part 3: EXTRA change record updates even past the spw cap.
        if (idx >= kFixedWords && ok)
        {
            detail::ebNoteLocked(s, tick, idx - kFixedWords, value, pc, ra, via, fn);
        }
        if (s.perWord[idx] >= kMaxLinesPerWord)
        {
            continue;
        }
        ++s.perWord[idx];
        const uint32_t a0 = (ctx != nullptr) ? getRegU32(ctx, 4) : 0u;
        const uint32_t a1 = (ctx != nullptr) ? getRegU32(ctx, 5) : 0u;
        const uint32_t a2 = (ctx != nullptr) ? getRegU32(ctx, 6) : 0u;
        const uint32_t a3 = (ctx != nullptr) ? getRegU32(ctx, 7) : 0u;
        const uint32_t v0 = (ctx != nullptr) ? getRegU32(ctx, 2) : 0u;
        const uint32_t v1 = (ctx != nullptr) ? getRegU32(ctx, 3) : 0u;
        const uint32_t t0 = (ctx != nullptr) ? getRegU32(ctx, 8) : 0u;
        const uint32_t t1 = (ctx != nullptr) ? getRegU32(ctx, 9) : 0u;
        const uint32_t t2 = (ctx != nullptr) ? getRegU32(ctx, 10) : 0u;
        const uint32_t t3 = (ctx != nullptr) ? getRegU32(ctx, 11) : 0u;
        const uint32_t t4 = (ctx != nullptr) ? getRegU32(ctx, 12) : 0u;
        const uint32_t t5 = (ctx != nullptr) ? getRegU32(ctx, 13) : 0u;
        const uint32_t t6 = (ctx != nullptr) ? getRegU32(ctx, 14) : 0u;
        const uint32_t t7 = (ctx != nullptr) ? getRegU32(ctx, 15) : 0u;
        const uint32_t t8 = (ctx != nullptr) ? getRegU32(ctx, 24) : 0u;
        const uint32_t t9 = (ctx != nullptr) ? getRegU32(ctx, 25) : 0u;
        const uint32_t s0 = (ctx != nullptr) ? getRegU32(ctx, 16) : 0u;
        const uint32_t s1 = (ctx != nullptr) ? getRegU32(ctx, 17) : 0u;
        const uint32_t s2 = (ctx != nullptr) ? getRegU32(ctx, 18) : 0u;
        const uint32_t s3 = (ctx != nullptr) ? getRegU32(ctx, 19) : 0u;
        const uint32_t s4 = (ctx != nullptr) ? getRegU32(ctx, 20) : 0u;
        const uint32_t s5 = (ctx != nullptr) ? getRegU32(ctx, 21) : 0u;
        const uint32_t s6 = (ctx != nullptr) ? getRegU32(ctx, 22) : 0u;
        const uint32_t s7 = (ctx != nullptr) ? getRegU32(ctx, 23) : 0u;
        char srcBuf[24];
        if (hasSrc)
        {
            // srcBase + (w - first): EE source of this word. Wraps inside
            // the source space the same way the copy loop does.
            std::snprintf(srcBuf, sizeof(srcBuf), "0x%08x",
                          static_cast<uint32_t>(srcBase + (w - first)));
        }
        else
        {
            std::snprintf(srcBuf, sizeof(srcBuf), "-");
        }
        char line[1024];
        if (ok)
        {
            std::snprintf(line, sizeof(line),
                          "spw vsync=%llu addr=0x%08x value=0x%08x via=%s src=%s "
                          "pc=0x%08x ra=0x%08x fn=%s "
                          "a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x "
                          "v0=0x%08x v1=0x%08x "
                          "t0=0x%08x t1=0x%08x t2=0x%08x t3=0x%08x "
                          "t4=0x%08x t5=0x%08x t6=0x%08x t7=0x%08x "
                          "t8=0x%08x t9=0x%08x "
                          "s0=0x%08x s1=0x%08x s2=0x%08x s3=0x%08x "
                          "s4=0x%08x s5=0x%08x s6=0x%08x s7=0x%08x",
                          static_cast<unsigned long long>(tick),
                          w, value, via, srcBuf, pc, ra, fn != nullptr ? fn : "-",
                          a0, a1, a2, a3, v0, v1,
                          t0, t1, t2, t3, t4, t5, t6, t7, t8, t9,
                          s0, s1, s2, s3, s4, s5, s6, s7);
        }
        else
        {
            std::snprintf(line, sizeof(line),
                          "spw vsync=%llu addr=0x%08x value=- via=%s src=%s "
                          "pc=0x%08x ra=0x%08x fn=%s "
                          "a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x "
                          "v0=0x%08x v1=0x%08x "
                          "t0=0x%08x t1=0x%08x t2=0x%08x t3=0x%08x "
                          "t4=0x%08x t5=0x%08x t6=0x%08x t7=0x%08x "
                          "t8=0x%08x t9=0x%08x "
                          "s0=0x%08x s1=0x%08x s2=0x%08x s3=0x%08x "
                          "s4=0x%08x s5=0x%08x s6=0x%08x s7=0x%08x",
                          static_cast<unsigned long long>(tick),
                          w, via, srcBuf, pc, ra, fn != nullptr ? fn : "-",
                          a0, a1, a2, a3, v0, v1,
                          t0, t1, t2, t3, t4, t5, t6, t7, t8, t9,
                          s0, s1, s2, s3, s4, s5, s6, s7);
        }
        detail::emitLineLocked(s, line);
        if (!s.enabled)
        {
            return;
        }
    }
}

// Shared range emitter for bulk host copies (libc memcpy, SIF, CD
// reads, ELF loads). Iterates the WATCH WORDS (never the range: ranges
// can be megabytes) testing containment in [base, base+size). One spw
// line per contained word with cap room, plus the EXTRA change record.
// Linear guest space assumed (no 4 GB wrap). src words map linearly
// (srcBase + offset) when hasSrc.
inline void emitRangeOverlap(const uint8_t *rdram, const R5900Context *ctx,
                             uint32_t base, uint32_t size, const char *via,
                             uint32_t srcBase, bool hasSrc, const char *fn)
{
    if (size == 0u)
    {
        return;
    }
    if (!enabled())
    {
        return;
    }
    const uint64_t end = static_cast<uint64_t>(base) + size;
    auto contained = [&](uint32_t w) {
        return static_cast<uint64_t>(w) >= base &&
               static_cast<uint64_t>(w) + 4u <= end;
    };
    for (int i = 0; i < kFixedWords; ++i)
    {
        const uint32_t w = PS2_SCRATCHPAD_BASE + kFixedOffsets[i];
        if (!contained(w))
        {
            continue;
        }
        const uint32_t src = hasSrc ? static_cast<uint32_t>(srcBase + (w - base)) : 0u;
        emitOverlap(rdram, ctx, w, 4u, via, src, hasSrc, fn);
    }
    uint32_t extras[kMaxExtra] = {0u};
    int n = 0;
    {
        detail::State &s = detail::state();
        std::lock_guard<std::mutex> lock(s.mutex);
        n = s.numExtra;
        for (int i = 0; i < n; ++i)
        {
            extras[i] = s.extra[i];
        }
    }
    for (int i = 0; i < n; ++i)
    {
        const uint32_t ew = extras[i] & ~3u;
        // Every RAM-segment spelling of the same word (phys, cached
        // mirror, UCAB, KSEG0, KSEG1), plus the configured spelling.
        const uint32_t off = ew & PS2_RAM_MASK;
        const uint32_t spell[6] = {ew, off, 0x20000000u | off,
                                   0x30000000u | off, 0x80000000u | off,
                                   0xA0000000u | off};
        for (int k = 0; k < 6; ++k)
        {
            if (!contained(spell[k]))
            {
                continue;
            }
            const uint32_t src = hasSrc ? static_cast<uint32_t>(srcBase + (spell[k] - base)) : 0u;
            emitOverlap(rdram, ctx, spell[k], 4u, via, src, hasSrc, fn);
            break;
        }
    }
}

// via label from the store width (callers pass the host fn separately).
inline const char *viaForSize(uint32_t size)
{
    switch (size)
    {
    case 1u:
        return "store8";
    case 2u:
        return "store16";
    case 4u:
        return "store32";
    case 8u:
        return "store64";
    default:
        return "store128";
    }
}

// ---- Boot C: per-word last-writer record for item 0 ----

// Ungated tracker (enabled-only: no window, no cap) so a late-window
// readout sees the true last writer even when it wrote before the
// window opened. Call AFTER the store/DMA is visible. Only the four
// item-0 words (0x70000000..0x7000000c) are recorded.
inline void trackLastWriter(const uint8_t *rdram, const R5900Context *ctx,
                            uint32_t addr, uint32_t size, const char *via,
                            const char *fn)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    const uint32_t first = addr & ~3u;
    detail::flushEbLocked(s, tick);
    const uint32_t span = size > 16u ? 16u : size;
    const uint64_t end = static_cast<uint64_t>(addr) + span;
    const uint32_t pc = (ctx != nullptr) ? ctx->pc : 0u;
    const uint32_t ra = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
    for (uint32_t w = first; static_cast<uint64_t>(w) < end; w += 4u)
    {
        const int idx = detail::watchIndexLocked(s, w);
        if (idx < 0)
        {
            continue;
        }
        uint32_t value = 0u;
        if (!detail::readWordBack(rdram, w, value))
        {
            continue;
        }
        // Part 3: EXTRA change records update here (ungated: no window,
        // no cap), so post-cap and out-of-window stores still move ebwlast.
        if (idx >= kFixedWords)
        {
            detail::ebNoteLocked(s, tick, idx - kFixedWords, value, pc, ra, via, fn);
            continue;
        }
        if (idx >= 4)
        {
            continue;
        }
        detail::LastWriter &rec = s.lastW[idx];
        rec.valid = true;
        rec.wtick = tick;
        rec.value = value;
        rec.pc = pc;
        rec.ra = ra;
        std::snprintf(rec.via, sizeof(rec.via), "%s", via != nullptr ? via : "-");
        std::snprintf(rec.fn, sizeof(rec.fn), "%s", fn != nullptr ? fn : "-");
        std::snprintf(rec.src, sizeof(rec.src), "-");
    }
}

// SPR_TO variant with a per-word EE source (call after the memcpy loop).
inline void trackLastWriterDma(const uint8_t *rdram, uint32_t ramStart,
                               uint32_t sprStart, uint32_t totalBytes)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    if (totalBytes == 0u)
    {
        return;
    }
    detail::flushEbLocked(s, tick);
    for (int i = 0; i < 4; ++i)
    {
        const uint32_t off = kFixedOffsets[i];
        const uint32_t d = (off - (sprStart & (PS2_SCRATCHPAD_SIZE - 1u))) &
                           (PS2_SCRATCHPAD_SIZE - 1u);
        if (d >= totalBytes)
        {
            continue;
        }
        uint32_t value = 0u;
        if (!detail::readWordBack(rdram, PS2_SCRATCHPAD_BASE + off, value))
        {
            continue;
        }
        uint32_t dLast = d;
        if (totalBytes > PS2_SCRATCHPAD_SIZE)
        {
            const uint32_t wraps = (totalBytes - 1u - d) / PS2_SCRATCHPAD_SIZE;
            dLast = d + wraps * PS2_SCRATCHPAD_SIZE;
        }
        detail::LastWriter &rec = s.lastW[i];
        rec.valid = true;
        rec.wtick = tick;
        rec.value = value;
        rec.pc = 0u;
        rec.ra = 0u;
        std::snprintf(rec.via, sizeof(rec.via), "spr-dma");
        std::snprintf(rec.fn, sizeof(rec.fn), "spr-to");
        std::snprintf(rec.src, sizeof(rec.src), "0x%08x",
                      static_cast<uint32_t>((ramStart + dLast) & PS2_RAM_MASK));
    }
}

// Guest-CPU store tap: call AFTER m_memory.write is visible. Holds the
// mem-suppress guard when armed so the PS2Memory-level fallback stays
// silent for Store-routed writes.
inline void noteStore(const uint8_t *rdram, const R5900Context *ctx,
                      uint32_t addr, uint32_t size, const char *fn)
{
    // Ungated last-writer record first (call sites run post-store).
    trackLastWriter(rdram, ctx, addr, size, viaForSize(size), fn);
    if (!storeArmed(addr, size))
    {
        return;
    }
    detail::ScopedMemSuppress suppress(true);
    emitOverlap(rdram, ctx, addr, size, viaForSize(size), 0u, false, fn);
}

// Fast-path tap (value from the store args; the fast path writes rdram,
// so a watched scratchpad addr here would itself be the story).
inline void noteFast(const uint8_t *rdram, const R5900Context *ctx,
                     uint32_t addr, uint32_t size, uint32_t valueLo, const char *fn)
{
    (void)ctx;
    (void)valueLo;
    trackLastWriter(rdram, nullptr, addr, size, "fast", fn);
    if (!storeArmed(addr, size))
    {
        return;
    }
    // emitOverlap picks the backing by address (scratchpad backing for
    // scratchpad words, rdram for RAM extras); the fast path wrote rdram,
    // so a RAM read-back is exact and a scratchpad read-back is context.
    emitOverlap(rdram, nullptr, addr, size, "fast", 0u, false, fn);
}

// Host-side PS2Memory::write* fallback: silent unless enabled, and silent
// while a Store* tap holds the guard (Store-routed, already logged).
inline void noteMemWrite(const uint8_t *rdram, uint32_t addr, uint32_t size, const char *fn)
{
    if (detail::memSuppressed())
    {
        return;
    }
    trackLastWriter(rdram, nullptr, addr, size, "mem-write", fn);
    if (!storeArmed(addr, size))
    {
        return;
    }
    emitOverlap(rdram, nullptr, addr, size, "mem-write", 0u, false, fn);
}

// SPR_TO (RAM -> scratchpad) DMA tap: call after the memcpy loop with the
// RAM MADR start, the scratchpad SADR start, and the total byte count.
// Per watched word overlapped, src = the EE RAM address copied from.
inline void noteSprDma(const uint8_t *rdram, uint32_t ramStart,
                       uint32_t sprStart, uint32_t totalBytes)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    if (tick < s.from || tick > s.to)
    {
        return;
    }
    detail::flushEbLocked(s, tick);
    if (totalBytes == 0u)
    {
        return;
    }
    for (int i = 0; i < kFixedWords; ++i)
    {
        const uint32_t off = kFixedOffsets[i];
        // Distance of this word from the transfer start, mod scratchpad.
        const uint32_t d = (off - (sprStart & (PS2_SCRATCHPAD_SIZE - 1u))) &
                           (PS2_SCRATCHPAD_SIZE - 1u);
        if (d >= totalBytes)
        {
            continue;
        }
        if (s.perWord[i] >= kMaxLinesPerWord)
        {
            continue;
        }
        uint32_t value = 0u;
        const bool ok = detail::readWordBack(rdram, PS2_SCRATCHPAD_BASE + off, value);
        ++s.perWord[i];
        // Multi-wrap transfers hit the word more than once; the read-back
        // above is the final content, so report the last source offset.
        uint32_t dLast = d;
        if (totalBytes > PS2_SCRATCHPAD_SIZE)
        {
            const uint32_t wraps = (totalBytes - 1u - d) / PS2_SCRATCHPAD_SIZE;
            dLast = d + wraps * PS2_SCRATCHPAD_SIZE;
        }
        const uint32_t src = (ramStart + dLast) & PS2_RAM_MASK;
        char line[1024];
        if (ok)
        {
            std::snprintf(line, sizeof(line),
                          "spw vsync=%llu addr=0x%08x value=0x%08x via=spr-dma src=0x%08x "
                          "pc=0x00000000 ra=0x00000000 fn=spr-to "
                          "a0=0x00000000 a1=0x00000000 a2=0x00000000 a3=0x00000000 "
                          "v0=0x00000000 v1=0x00000000 "
                          "t0=0x00000000 t1=0x00000000 t2=0x00000000 t3=0x00000000 "
                          "t4=0x00000000 t5=0x00000000 t6=0x00000000 t7=0x00000000 "
                          "t8=0x00000000 t9=0x00000000 "
                          "s0=0x00000000 s1=0x00000000 s2=0x00000000 s3=0x00000000 "
                          "s4=0x00000000 s5=0x00000000 s6=0x00000000 s7=0x00000000",
                          static_cast<unsigned long long>(tick),
                          PS2_SCRATCHPAD_BASE + off, value, src);
        }
        else
        {
            std::snprintf(line, sizeof(line),
                          "spw vsync=%llu addr=0x%08x value=- via=spr-dma src=0x%08x "
                          "pc=0x00000000 ra=0x00000000 fn=spr-to "
                          "a0=0x00000000 a1=0x00000000 a2=0x00000000 a3=0x00000000 "
                          "v0=0x00000000 v1=0x00000000 "
                          "t0=0x00000000 t1=0x00000000 t2=0x00000000 t3=0x00000000 "
                          "t4=0x00000000 t5=0x00000000 t6=0x00000000 t7=0x00000000 "
                          "t8=0x00000000 t9=0x00000000 "
                          "s0=0x00000000 s1=0x00000000 s2=0x00000000 s3=0x00000000 "
                          "s4=0x00000000 s5=0x00000000 s6=0x00000000 s7=0x00000000",
                          static_cast<unsigned long long>(tick),
                          PS2_SCRATCHPAD_BASE + off, src);
        }
        detail::emitLineLocked(s, line);
        if (!s.enabled)
        {
            return;
        }
    }
}

// ---- Part-2 taps ----

// VIF0 kick census: one aggregated line per vsync with the kick count.
// Flush-on-tick-advance: the previous tick's line emits when a kick
// arrives in a new tick (the trailing tick may never flush).
inline void noteVif0Kick()
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    if (tick < s.from || tick > s.to)
    {
        return;
    }
    detail::flushEbLocked(s, tick);
    if (!s.kickValid || tick != s.kickTick)
    {
        if (s.kickValid && s.kickCount > 0u)
        {
            char line[96];
            std::snprintf(line, sizeof(line), "vif0kick vsync=%llu n=%llu",
                          static_cast<unsigned long long>(s.kickTick),
                          static_cast<unsigned long long>(s.kickCount));
            detail::emitLineLocked(s, line);
            if (!s.enabled)
            {
                return;
            }
        }
        s.kickValid = true;
        s.kickTick = tick;
        s.kickCount = 0u;
    }
    ++s.kickCount;
}

// VIF0 unknown opcode: every command word that reaches the parser's
// terminal else-break (no MSCAL/MSCALF/MSCNT/BASE/OFFSET branch exists,
// so VU0 programs kicked via VIF0 are silently dropped with the packet
// tail). rem = bytes left in the packet after the unknown word.
inline void noteVif0Unk(uint32_t opcode, uint32_t imm, uint32_t num, uint32_t rem)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    if (tick < s.from || tick > s.to)
    {
        return;
    }
    detail::flushEbLocked(s, tick);
    if (s.vif0UnkLines >= kMaxVif0UnkLines)
    {
        return;
    }
    ++s.vif0UnkLines;
    char line[128];
    std::snprintf(line, sizeof(line),
                  "vif0unk vsync=%llu op=0x%02x imm=0x%04x num=%u rem=%u",
                  static_cast<unsigned long long>(tick),
                  opcode & 0xFFu, imm & 0xFFFFu, num, rem);
    detail::emitLineLocked(s, line);
}

// VU0 microprogram call: per executed call (the invalid-address early
// return executes nothing and logs nothing). cycles = interpreter
// cycles used (reset per call, budget 4096); budgethit = cycles at or
// past budget; vi1/vi2 = state after the run. First 2000 lines.
inline void noteVu0Call(const R5900Context *ctx, uint32_t startPC,
                        uint64_t cyclesUsed, bool budgetHit,
                        int32_t vi1, int32_t vi2)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    if (tick < s.from || tick > s.to)
    {
        return;
    }
    detail::flushEbLocked(s, tick);
    if (s.vu0CallLines >= kMaxVu0CallLines)
    {
        return;
    }
    ++s.vu0CallLines;
    const uint32_t caller = (ctx != nullptr) ? ctx->pc : 0u;
    char line[192];
    std::snprintf(line, sizeof(line),
                  "vu0call vsync=%llu caller=0x%08x start=0x%08x cycles=%llu budgethit=%d vi1=%d vi2=%d",
                  static_cast<unsigned long long>(tick), caller, startPC,
                  static_cast<unsigned long long>(cyclesUsed),
                  budgetHit ? 1 : 0, vi1, vi2);
    detail::emitLineLocked(s, line);
}

// SPR_FROM (scratchpad -> RAM) DMA tap: the fixed words are scratchpad
// (never a FROM dest), so only EXTRA EE words are matched. src = the
// full scratchpad address copied from. Call after the memcpy loop with
// the RAM MADR dest start, the scratchpad SADR source start, and the
// total byte count.
inline void noteSprFromDma(const uint8_t *rdram, uint32_t ramStart,
                           uint32_t sprStart, uint32_t totalBytes)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    if (tick < s.from || tick > s.to)
    {
        return;
    }
    detail::flushEbLocked(s, tick);
    if (totalBytes == 0u || s.numExtra == 0)
    {
        return;
    }
    const uint32_t sprBase = sprStart & (PS2_SCRATCHPAD_SIZE - 1u);
    const uint32_t ramBase = ramStart & PS2_RAM_MASK;
    for (int i = 0; i < s.numExtra; ++i)
    {
        const int idx = kFixedWords + i;
        // EXTRA addrs are EE words; match them in RAM space.
        const uint32_t want = detail::ramCanon(s.extra[i]);
        // Distance of this word from the transfer dest start, mod RAM.
        // The copy loop wraps RAM and SPR independently, so the word is
        // hit iff its linear offset lands inside totalBytes.
        bool hit = false;
        uint32_t dHit = 0u;
        // Scan is bounded: extras are few; totalBytes decides by compare.
        // d = (want - ramBase) mod RAM-size; hit iff d < totalBytes.
        const uint32_t d = (want - ramBase) & PS2_RAM_MASK;
        if (d < totalBytes && want + 4u <= PS2_RAM_SIZE)
        {
            hit = true;
            dHit = d;
        }
        if (!hit)
        {
            continue;
        }
        uint32_t value = 0u;
        const bool ok = (rdram != nullptr) &&
                        (want + 4u <= PS2_RAM_SIZE) &&
                        (std::memcpy(&value, rdram + want, sizeof(value)), true);
        // Part 3: EXTRA change record updates even past the spw cap.
        if (ok)
        {
            detail::ebNoteLocked(s, tick, i, value, 0u, 0u, "spr-from", "spr-from");
        }
        if (s.perWord[idx] >= kMaxLinesPerWord)
        {
            continue;
        }
        ++s.perWord[idx];
        // Last-hit source on multi-wrap transfers (same reasoning as TO).
        uint32_t dLast = dHit;
        if (totalBytes > PS2_SCRATCHPAD_SIZE)
        {
            const uint32_t wraps = (totalBytes - 1u - dHit) / PS2_SCRATCHPAD_SIZE;
            dLast = dHit + wraps * PS2_SCRATCHPAD_SIZE;
        }
        const uint32_t src = PS2_SCRATCHPAD_BASE +
                             ((sprBase + dLast) & (PS2_SCRATCHPAD_SIZE - 1u));
        char line[1024];
        if (ok)
        {
            std::snprintf(line, sizeof(line),
                          "spw vsync=%llu addr=0x%08x value=0x%08x via=spr-from src=0x%08x "
                          "pc=0x00000000 ra=0x00000000 fn=spr-from "
                          "a0=0x00000000 a1=0x00000000 a2=0x00000000 a3=0x00000000 "
                          "v0=0x00000000 v1=0x00000000 "
                          "t0=0x00000000 t1=0x00000000 t2=0x00000000 t3=0x00000000 "
                          "t4=0x00000000 t5=0x00000000 t6=0x00000000 t7=0x00000000 "
                          "t8=0x00000000 t9=0x00000000 "
                          "s0=0x00000000 s1=0x00000000 s2=0x00000000 s3=0x00000000 "
                          "s4=0x00000000 s5=0x00000000 s6=0x00000000 s7=0x00000000",
                          static_cast<unsigned long long>(tick),
                          s.extra[i] & ~3u, value, src);
        }
        else
        {
            std::snprintf(line, sizeof(line),
                          "spw vsync=%llu addr=0x%08x value=- via=spr-from src=0x%08x "
                          "pc=0x00000000 ra=0x00000000 fn=spr-from "
                          "a0=0x00000000 a1=0x00000000 a2=0x00000000 a3=0x00000000 "
                          "v0=0x00000000 v1=0x00000000 "
                          "t0=0x00000000 t1=0x00000000 t2=0x00000000 t3=0x00000000 "
                          "t4=0x00000000 t5=0x00000000 t6=0x00000000 t7=0x00000000 "
                          "t8=0x00000000 t9=0x00000000 "
                          "s0=0x00000000 s1=0x00000000 s2=0x00000000 s3=0x00000000 "
                          "s4=0x00000000 s5=0x00000000 s6=0x00000000 s7=0x00000000",
                          static_cast<unsigned long long>(tick),
                          s.extra[i] & ~3u, src);
        }
        detail::emitLineLocked(s, line);
        if (!s.enabled)
        {
            return;
        }
    }
}

// SPR MOD!=0 kick counter: chain/interleave SPR modes are not
// implemented (silent drop), so every such kick is evidence.
inline void noteSprMod(bool sprFrom, uint32_t mode, uint32_t madr,
                       uint32_t sadr, uint32_t qwc)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    if (tick < s.from || tick > s.to)
    {
        return;
    }
    detail::flushEbLocked(s, tick);
    if (s.sprModLines >= kMaxSprModLines)
    {
        return;
    }
    ++s.sprModLines;
    char line[128];
    std::snprintf(line, sizeof(line),
                  "sprmod vsync=%llu dir=%s mode=%u madr=0x%08x sadr=0x%08x qwc=%u",
                  static_cast<unsigned long long>(tick),
                  sprFrom ? "from" : "to", mode, madr, sadr, qwc);
    detail::emitLineLocked(s, line);
}

// Walk readout: call at the sub_00362DE8@0x362F68 -> func_394ED0
// dispatch when the item pointer is 0x70000000. Emits one spwlast line
// with the last-writer record per item-0 word. Window-gated (Boot C:
// 1355..1400), first 40 lines, no per-word cap.
inline void noteItem0Walk(const R5900Context *ctx, uint32_t s1)
{
    if (s1 != kItem0Base)
    {
        return;
    }
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    if (tick < s.from || tick > s.to)
    {
        return;
    }
    detail::flushEbLocked(s, tick);
    if (s.lastLines >= kMaxLastLines)
    {
        return;
    }
    ++s.lastLines;
    char words[4][160];
    for (int i = 0; i < 4; ++i)
    {
        const detail::LastWriter &rec = s.lastW[i];
        if (!rec.valid)
        {
            std::snprintf(words[i], sizeof(words[i]), "-");
        }
        else
        {
            std::snprintf(words[i], sizeof(words[i]),
                          "%s,0x%08x,0x%08x,%s,0x%08x,%s,%llu",
                          rec.via, rec.pc, rec.ra, rec.fn, rec.value, rec.src,
                          static_cast<unsigned long long>(rec.wtick));
        }
    }
    (void)ctx;
    char line[768];
    std::snprintf(line, sizeof(line),
                  "spwlast vsync=%llu w0=%s w1=%s w2=%s w3=%s",
                  static_cast<unsigned long long>(tick),
                  words[0], words[1], words[2], words[3]);
    detail::emitLineLocked(s, line);
}

// ---- Part 4: render-list append watch (sub_00376938) ----

// Hot-path gates for the WRITE-macro trigger checks: one relaxed
// atomic load when off.
inline bool appendArmed()
{
    return detail::appendOnFlag().load(std::memory_order_relaxed);
}

inline bool tplArmed()
{
    return detail::tplArmedFlag().load(std::memory_order_relaxed);
}

// Per-vsync aggregation flush (Part 4 amendment): emits the appsum
// line for a completed tick (only when it saw appends or tpl changes)
// plus one apc line per appender pc, then resets. Call with s.mutex
// held at the top of every Part-4 tap. The trailing tick may never
// flush (no shutdown hook), same caveat as ebwlast.
inline void flushPerVsyncLocked(detail::State &s, uint64_t tick)
{
    if (!s.sumTickValid)
    {
        s.sumTickValid = true;
        s.sumTick = tick;
        return;
    }
    if (s.sumTick == tick)
    {
        return;
    }
    if ((s.sumApp > 0u || s.sumTpl > 0u) && s.sumLines < kMaxAppSumLines)
    {
        ++s.sumLines;
        char hist[160];
        hist[0] = '\0';
        for (int m = 0; m < 16; ++m)
        {
            if (s.sumHist[m] == 0u)
            {
                continue;
            }
            char b[24];
            std::snprintf(b, sizeof(b), "%s%x:%llu",
                          hist[0] != '\0' ? "," : "",
                          static_cast<unsigned>(m),
                          static_cast<unsigned long long>(s.sumHist[m]));
            const size_t room = sizeof(hist) - std::strlen(hist) - 1u;
            std::strncat(hist, b, room);
        }
        char line[320];
        std::snprintf(line, sizeof(line),
                      "appsum vsync=%llu n_app=%llu n_tpl=%llu mode_hist=%s",
                      static_cast<unsigned long long>(s.sumTick),
                      static_cast<unsigned long long>(s.sumApp),
                      static_cast<unsigned long long>(s.sumTpl),
                      hist[0] != '\0' ? hist : "-");
        detail::emitLineLocked(s, line);
    }
    for (int i = 0; i < s.numApc; ++i)
    {
        char line[128];
        std::snprintf(line, sizeof(line), "apc vsync=%llu pc=0x%x count=%llu",
                      static_cast<unsigned long long>(s.sumTick),
                      s.apcPc[i],
                      static_cast<unsigned long long>(s.apcCount[i]));
        detail::emitLineLocked(s, line);
    }
    s.sumTick = tick;
    s.sumApp = 0u;
    s.sumTpl = 0u;
    for (int m = 0; m < 16; ++m)
    {
        s.sumHist[m] = 0u;
    }
    s.numApc = 0;
}

// Appender-pc census tap (Part 4 amendment): call on every
// WRITE-macro store. Counts per-vsync per-pc stores into the EE
// list-item region [kApcBase, kApcBase + stride*items), matched in
// RAM space (any segment spelling). No window gate and no line cap
// (bounded by distinct pcs per vsync).
inline void noteApcMaybe(const R5900Context *ctx, uint32_t addr)
{
    if (ctx == nullptr)
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.appendOn)
    {
        return;
    }
    const uint32_t canon = detail::ramCanon(addr);
    const uint32_t lo = detail::ramCanon(kApcBase);
    if (canon < lo || canon >= lo + kApcStride * kApcItems)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    detail::flushEbLocked(s, tick);
    flushPerVsyncLocked(s, tick);
    if (!s.enabled)
    {
        return;
    }
    const uint32_t pc = ctx->pc;
    for (int i = 0; i < s.numApc; ++i)
    {
        if (s.apcPc[i] == pc)
        {
            ++s.apcCount[i];
            return;
        }
    }
    if (s.numApc < kMaxApcPc)
    {
        s.apcPc[s.numApc] = pc;
        s.apcCount[s.numApc] = 1u;
        ++s.numApc;
    }
}

// Multi-site filtered log (Part 4 amendment 2, T62 parity): call at
// the END of any count-increment sw whose pc is one of kAppxTrigger
// (post-store; v1 holds the post-increment count, t0 the template).
// Logs only rows with count(v1) <= 1 or mode 6 (((tw0>>6)&0xF)==6),
// tick >= kAppxFromTick, cap 300. site = index into kAppxTrigger
// (0 = 0x3797EC, 1 = 0x37AD44, 2 = 0x37B474).
inline void noteAppxMaybe(const uint8_t *rdram, const R5900Context *ctx)
{
    if (ctx == nullptr)
    {
        return;
    }
    int site = -1;
    for (int i = 0; i < 3; ++i)
    {
        if (ctx->pc == kAppxTrigger[i])
        {
            site = i;
            break;
        }
    }
    if (site < 0)
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.appendOn)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    if (tick < kAppxFromTick)
    {
        return;
    }
    const uint32_t t0 = getRegU32(ctx, 8);
    uint32_t tw[kTplWords] = {0u};
    for (uint32_t i = 0u; i < kTplWords; ++i)
    {
        detail::readWordBack(rdram, t0 + 4u * i, tw[i]);
    }
    const uint32_t v1 = getRegU32(ctx, 3);
    if (v1 > 1u && ((tw[0] >> 6u) & 0xFu) != 6u)
    {
        return;
    }
    if (s.appxLines >= kMaxAppxLines)
    {
        return;
    }
    ++s.appxLines;
    const uint32_t ra = getRegU32(ctx, 31);
    char line[320];
    std::snprintf(line, sizeof(line),
                  "appx vsync=%llu site=%d count=%u t0=0x%x tw0=0x%x "
                  "tw1=0x%x tw2=0x%x tw3=0x%x tw4=0x%x ra=0x%x",
                  static_cast<unsigned long long>(tick),
                  site, v1, t0, tw[0], tw[1], tw[2], tw[3], tw[4], ra);
    detail::emitLineLocked(s, line);
}

// 0x1B0 value watch (Part 4 amendment 2, T61 parity): call at the END
// of every WRITE-macro store with the stored lanes. Logs one line per
// 32-bit lane with (v & 0x3FF) == 0x1B0, with pc/ra/regs; no window
// gate (the set instant is unknown), cap 200. Early-boot unrelated
// 0x1B0-pattern stores can burn the cap; each line carries pc for
// triage.
inline void noteV1b0Maybe(const R5900Context *ctx, uint32_t addr,
                          uint32_t size, uint64_t valueLo,
                          uint64_t valueHi, const char *fn)
{
    if (ctx == nullptr)
    {
        return;
    }
    uint32_t lanes = size / 4u;
    if (lanes < 1u)
    {
        lanes = 1u;
    }
    if (lanes > 4u)
    {
        lanes = 4u;
    }
    const uint64_t words[2] = {valueLo, valueHi};
    bool hit = false;
    for (uint32_t l = 0u; l < lanes; ++l)
    {
        const uint32_t v =
            static_cast<uint32_t>((words[l / 2u] >> ((l % 2u) * 32u)) & 0xFFFFFFFFull);
        if ((v & 0x3FFu) == 0x1B0u)
        {
            hit = true;
            break;
        }
    }
    if (!hit)
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.appendOn)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    detail::flushEbLocked(s, tick);
    flushPerVsyncLocked(s, tick);
    if (!s.enabled)
    {
        return;
    }
    const uint32_t pc = ctx->pc;
    const uint32_t ra = getRegU32(ctx, 31);
    for (uint32_t l = 0u; l < lanes; ++l)
    {
        const uint32_t v =
            static_cast<uint32_t>((words[l / 2u] >> ((l % 2u) * 32u)) & 0xFFFFFFFFull);
        if ((v & 0x3FFu) != 0x1B0u)
        {
            continue;
        }
        if (s.v1b0Lines >= kMaxV1b0Lines)
        {
            return;
        }
        ++s.v1b0Lines;
        const uint32_t a0 = getRegU32(ctx, 4);
        const uint32_t a1 = getRegU32(ctx, 5);
        const uint32_t a2 = getRegU32(ctx, 6);
        const uint32_t a3 = getRegU32(ctx, 7);
        const uint32_t v0 = getRegU32(ctx, 2);
        const uint32_t v1 = getRegU32(ctx, 3);
        const uint32_t s0 = getRegU32(ctx, 16);
        const uint32_t s1 = getRegU32(ctx, 17);
        const uint32_t s2 = getRegU32(ctx, 18);
        const uint32_t s3 = getRegU32(ctx, 19);
        const uint32_t s4 = getRegU32(ctx, 20);
        const uint32_t s5 = getRegU32(ctx, 21);
        const uint32_t s6 = getRegU32(ctx, 22);
        const uint32_t s7 = getRegU32(ctx, 23);
        char line[640];
        std::snprintf(line, sizeof(line),
                      "v1b0 vsync=%llu addr=0x%x lane=%u value=0x%x "
                      "pc=0x%x ra=0x%x fn=%s "
                      "a0=%08x a1=%08x a2=%08x a3=%08x "
                      "v0=%08x v1=%08x "
                      "s0=%08x s1=%08x s2=%08x s3=%08x "
                      "s4=%08x s5=%08x s6=%08x s7=%08x",
                      static_cast<unsigned long long>(tick),
                      addr, l, v, pc, ra, fn != nullptr ? fn : "-",
                      a0, a1, a2, a3, v0, v1,
                      s0, s1, s2, s3, s4, s5, s6, s7);
        detail::emitLineLocked(s, line);
        if (!s.enabled)
        {
            return;
        }
    }
}

// Template-block watch (Part 4 amendment 3, T63 parity): call at the
// END of every WRITE-macro store (post-write, read-back live).
// Change-only over [kTwBase, kTwBase + kTwSize): the first overlapping
// store snapshots the whole block silently (lazy baseline, valid
// thereafter); later stores log one tw line per changed word and
// refresh the lasts. Whole boot (no window gate), no per-word cap,
// 2000 total lines. Guest-CPU stores only (pc/ra required); host
// bulk paths cannot name a setter.
inline void noteTwMaybe(const uint8_t *rdram, const R5900Context *ctx,
                        uint32_t addr, uint32_t size, const char *fn)
{
    if (ctx == nullptr)
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.appendOn)
    {
        return;
    }
    const uint32_t lo = detail::ramCanon(kTwBase);
    const uint32_t span = size > 16u ? 16u : size;
    // Canonicalize the store range: UCAB/kseg spellings of block words
    // must intersect the canonical block.
    const uint32_t cFirst = detail::ramCanon(addr & ~3u);
    const uint64_t cEnd = static_cast<uint64_t>(cFirst) + span;
    const uint64_t blockEnd = static_cast<uint64_t>(lo) + kTwSize;
    uint32_t w0 = cFirst > lo ? cFirst : lo;
    uint64_t wEnd = cEnd < blockEnd ? cEnd : blockEnd;
    if (static_cast<uint64_t>(w0) >= wEnd)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    detail::flushEbLocked(s, tick);
    flushPerVsyncLocked(s, tick);
    if (!s.enabled)
    {
        return;
    }
    if (!s.twValid)
    {
        for (uint32_t i = 0u; i < kTwWords; ++i)
        {
            if (!detail::readWordBack(rdram, lo + 4u * i, s.twLast[i]))
            {
                return;
            }
        }
        s.twValid = true;
    }
    const uint32_t pc = ctx->pc;
    const uint32_t ra = getRegU32(ctx, 31);
    const char *via = viaForSize(size);
    for (uint32_t w = w0; static_cast<uint64_t>(w) < wEnd; w += 4u)
    {
        uint32_t now = 0u;
        if (!detail::readWordBack(rdram, w, now))
        {
            continue;
        }
        const uint32_t idx = (w - lo) / 4u;
        if (idx >= kTwWords || now == s.twLast[idx])
        {
            continue;
        }
        const uint32_t old = s.twLast[idx];
        s.twLast[idx] = now;
        if (s.twLines >= kMaxTwLines)
        {
            return;
        }
        ++s.twLines;
        const uint32_t a0 = getRegU32(ctx, 4);
        const uint32_t a1 = getRegU32(ctx, 5);
        const uint32_t a2 = getRegU32(ctx, 6);
        const uint32_t a3 = getRegU32(ctx, 7);
        const uint32_t v0 = getRegU32(ctx, 2);
        const uint32_t v1 = getRegU32(ctx, 3);
        const uint32_t s0 = getRegU32(ctx, 16);
        const uint32_t s1 = getRegU32(ctx, 17);
        const uint32_t s2 = getRegU32(ctx, 18);
        const uint32_t s3 = getRegU32(ctx, 19);
        const uint32_t s4 = getRegU32(ctx, 20);
        const uint32_t s5 = getRegU32(ctx, 21);
        const uint32_t s6 = getRegU32(ctx, 22);
        const uint32_t s7 = getRegU32(ctx, 23);
        char line[640];
        std::snprintf(line, sizeof(line),
                      "tw vsync=%llu addr=0x%x old=0x%x new=0x%x via=%s "
                      "pc=0x%x ra=0x%x fn=%s "
                      "a0=%08x a1=%08x a2=%08x a3=%08x "
                      "v0=%08x v1=%08x "
                      "s0=%08x s1=%08x s2=%08x s3=%08x "
                      "s4=%08x s5=%08x s6=%08x s7=%08x",
                      static_cast<unsigned long long>(tick),
                      w, old, now, via, pc, ra,
                      fn != nullptr ? fn : "-",
                      a0, a1, a2, a3, v0, v1,
                      s0, s1, s2, s3, s4, s5, s6, s7);
        detail::emitLineLocked(s, line);
        if (!s.enabled)
        {
            return;
        }
    }
}

// Append tap: call at the END of the count-increment sw
// (pc == kAppendCountPc, same basic block as the 0x3797EC append
// copy; post-store, so *a3 already holds the T60 count). Arms (or
// re-arms on template move) the template watch ungated; aggregates
// the appsum census ungated; emits the (a) line (T60 app format)
// only in-window and under the cap.
inline void noteAppend(const uint8_t *rdram, const R5900Context *ctx)
{
    if (ctx == nullptr)
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.appendOn)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    detail::flushEbLocked(s, tick);
    flushPerVsyncLocked(s, tick);
    if (!s.enabled)
    {
        return;
    }
    const uint32_t a3 = getRegU32(ctx, 7);
    const uint32_t t0 = getRegU32(ctx, 8);
    uint32_t tw0 = 0u;
    detail::readWordBack(rdram, t0, tw0);
    if (s.tplBase != t0)
    {
        const uint32_t oldBase = s.tplBase;
        s.tplBase = t0;
        uint32_t w1 = 0u;
        detail::readWordBack(rdram, t0 + 4u, w1);
        s.tplLast0 = tw0;
        s.tplLast1 = w1;
        detail::tplArmedFlag().store(true, std::memory_order_relaxed);
        // Rearm line on a genuine move (T60 tplrearm format, sharing
        // the tpl budget); the first arm stays silent.
        if (oldBase != 0u && tick >= s.from && tick <= s.to &&
            s.tplLines < kMaxTplLines)
        {
            ++s.tplLines;
            char rline[128];
            std::snprintf(rline, sizeof(rline),
                          "tplrearm vsync=%llu old=0x%x new=0x%x",
                          static_cast<unsigned long long>(tick),
                          oldBase, t0);
            detail::emitLineLocked(s, rline);
            if (!s.enabled)
            {
                return;
            }
        }
    }
    // Unfiltered aggregation (amendment): every append counts, any tick.
    ++s.sumApp;
    s.sumHist[(tw0 >> 6u) & 0xFu]++;
    if (tick < s.from || tick > s.to)
    {
        return;
    }
    if (s.appLines >= kMaxAppLines)
    {
        return;
    }
    ++s.appLines;
    uint32_t count = 0u;
    detail::readWordBack(rdram, a3, count);
    uint32_t tw[kTplWords] = {0u};
    tw[0] = tw0;
    for (uint32_t i = 1u; i < kTplWords; ++i)
    {
        detail::readWordBack(rdram, t0 + 4u * i, tw[i]);
    }
    const uint32_t s4 = getRegU32(ctx, 20);
    const uint32_t t1 = getRegU32(ctx, 9);
    const uint32_t ra = getRegU32(ctx, 31);
    char line[256];
    std::snprintf(line, sizeof(line),
                  "app vsync=%llu count=%u t0=0x%x tw0=0x%x tw1=0x%x "
                  "tw2=0x%x tw3=0x%x tw4=0x%x s4=0x%x t1=0x%x ra=0x%x",
                  static_cast<unsigned long long>(tick),
                  count, t0, tw[0], tw[1], tw[2], tw[3], tw[4],
                  s4, t1, ra);
    detail::emitLineLocked(s, line);
}

// Template write tap: call at the END of every WRITE-macro store once
// armed (post-write, so read-back is live). Change detection always
// updates the lasts (so in-window state stays true); tpl/tplm lines
// are window-gated. Logs one T60-format tpl line per changed word
// (tw0/tw1 only), plus a tplm line when a tw0 change sets mode bits
// ((new & 0x3C0) != 0). Sub-word and 64/128-bit stores are handled
// by read-back comparison.
inline void noteTplMaybe(const uint8_t *rdram, const R5900Context *ctx,
                         uint32_t addr, uint32_t size, const char *fn)
{
    if (ctx == nullptr)
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.appendOn || s.tplBase == 0u)
    {
        return;
    }
    const uint32_t span = size > 16u ? 16u : size;
    // Canonicalize both sides: the store and the armed base may use
    // different segment spellings (kuseg vs UCAB) of the same words.
    const uint32_t cBase = detail::ramCanon(s.tplBase);
    const uint32_t cFirst = detail::ramCanon(addr & ~3u);
    const uint64_t cEnd = static_cast<uint64_t>(cFirst) + span;
    bool touches01 = false;
    for (uint32_t w = cFirst; static_cast<uint64_t>(w) < cEnd; w += 4u)
    {
        if (w == cBase || w == cBase + 4u)
        {
            touches01 = true;
            break;
        }
    }
    if (!touches01)
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    detail::flushEbLocked(s, tick);
    flushPerVsyncLocked(s, tick);
    if (!s.enabled)
    {
        return;
    }
    uint32_t now0 = 0u, now1 = 0u;
    if (!detail::readWordBack(rdram, s.tplBase, now0) ||
        !detail::readWordBack(rdram, s.tplBase + 4u, now1))
    {
        return;
    }
    const uint32_t pc = ctx->pc;
    const uint32_t ra = getRegU32(ctx, 31);
    const bool ch0 = (now0 != s.tplLast0);
    const bool ch1 = (now1 != s.tplLast1);
    if (!ch0 && !ch1)
    {
        return;
    }
    const uint32_t old0 = s.tplLast0, old1 = s.tplLast1;
    s.tplLast0 = now0;
    s.tplLast1 = now1;
    // Ungated churn counter feeds appsum (amendment).
    s.sumTpl += (ch0 ? 1u : 0u) + (ch1 ? 1u : 0u);
    const bool inWin = (tick >= s.from && tick <= s.to);
    const uint32_t a0 = getRegU32(ctx, 4);
    const uint32_t a1 = getRegU32(ctx, 5);
    const uint32_t a2 = getRegU32(ctx, 6);
    const uint32_t a3 = getRegU32(ctx, 7);
    const uint32_t v0 = getRegU32(ctx, 2);
    const uint32_t v1 = getRegU32(ctx, 3);
    const uint32_t s0 = getRegU32(ctx, 16);
    const uint32_t s1 = getRegU32(ctx, 17);
    const uint32_t s2 = getRegU32(ctx, 18);
    const uint32_t s3 = getRegU32(ctx, 19);
    const uint32_t s4 = getRegU32(ctx, 20);
    const uint32_t s5 = getRegU32(ctx, 21);
    const uint32_t s6 = getRegU32(ctx, 22);
    const uint32_t s7 = getRegU32(ctx, 23);
    // tplm (amendment 2: exact mode 6; the 0x3C0 mask admitted mode
    // 3 and flooded): tw0 change with ((new tw0 >> 6) & 0xF) == 6.
    // Own 200-line cap; emitted alongside (not instead of) the tpl line.
    if (ch0 && ((now0 >> 6u) & 0xFu) == 6u && inWin && s.tplmLines < kMaxTplmLines)
    {
        ++s.tplmLines;
        char mline[768];
        std::snprintf(mline, sizeof(mline),
                      "tplm vsync=%llu addr=0x%x tw0old=0x%x tw0new=0x%x "
                      "tw1old=0x%x tw1new=0x%x pc=0x%x ra=0x%x fn=%s "
                      "a0=%08x a1=%08x a2=%08x a3=%08x "
                      "v0=%08x v1=%08x "
                      "s0=%08x s1=%08x s2=%08x s3=%08x "
                      "s4=%08x s5=%08x s6=%08x s7=%08x",
                      static_cast<unsigned long long>(tick),
                      s.tplBase, old0, now0, old1, now1,
                      pc, ra, fn != nullptr ? fn : "-",
                      a0, a1, a2, a3, v0, v1,
                      s0, s1, s2, s3, s4, s5, s6, s7);
        detail::emitLineLocked(s, mline);
        if (!s.enabled)
        {
            return;
        }
    }
    if (!inWin)
    {
        return;
    }
    const uint32_t changed[2] = {ch0 ? 1u : 0u, ch1 ? 1u : 0u};
    const uint32_t oldV[2] = {old0, old1};
    const uint32_t newV[2] = {now0, now1};
    for (int k = 0; k < 2; ++k)
    {
        if (!changed[k])
        {
            continue;
        }
        if (s.tplLines >= kMaxTplLines)
        {
            return;
        }
        ++s.tplLines;
        char line[640];
        std::snprintf(line, sizeof(line),
                      "tpl vsync=%llu addr=0x%x old=0x%x new=0x%x "
                      "pc=0x%x ra=0x%x "
                      "a0=%08x a1=%08x a2=%08x a3=%08x "
                      "v0=%08x v1=%08x "
                      "s0=%08x s1=%08x s2=%08x s3=%08x "
                      "s4=%08x s5=%08x s6=%08x s7=%08x",
                      static_cast<unsigned long long>(tick),
                      s.tplBase + 4u * static_cast<uint32_t>(k),
                      oldV[k], newV[k], pc, ra,
                      a0, a1, a2, a3, v0, v1,
                      s0, s1, s2, s3, s4, s5, s6, s7);
        detail::emitLineLocked(s, line);
        if (!s.enabled)
        {
            return;
        }
    }
}

// Test hooks (mirror ps2_e43_trace.h conventions).
inline void configureForTest(const char *path, uint64_t from, uint64_t to,
                             const uint32_t *extras = nullptr, int numExtras = 0)
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.path = path != nullptr ? path : "";
    s.from = from;
    s.to = to;
    s.numExtra = 0;
    if (extras != nullptr && numExtras > 0)
    {
        const int n = numExtras < kMaxExtra ? numExtras : kMaxExtra;
        for (int i = 0; i < n; ++i)
        {
            s.extra[i] = extras[i] & ~3u;
        }
        s.numExtra = n;
    }
    s.enabled = !s.path.empty();
    detail::enabledFlag().store(s.enabled, std::memory_order_relaxed);
    s.outOpen = false;
    for (int i = 0; i < kMaxWords; ++i)
    {
        s.perWord[i] = 0u;
    }
    s.kickValid = false;
    s.kickTick = 0u;
    s.kickCount = 0u;
    s.vif0UnkLines = 0u;
    s.vu0CallLines = 0u;
    s.sprModLines = 0u;
    s.lastLines = 0u;
    for (int i = 0; i < kFixedWords; ++i)
    {
        s.lastW[i].valid = false;
    }
    for (int i = 0; i < kMaxExtra; ++i)
    {
        s.eb[i].dirty = false;
    }
    s.ebFlushValid = false;
    s.ebFlushTick = 0u;
    s.ebLines = 0u;
    detail::memSuppressed() = false;
    s.appendOn = false;
    detail::appendOnFlag().store(false, std::memory_order_relaxed);
    detail::tplArmedFlag().store(false, std::memory_order_relaxed);
    s.appLines = 0u;
    s.tplBase = 0u;
    s.tplLast0 = 0u;
    s.tplLast1 = 0u;
    s.tplLines = 0u;
    s.tplmLines = 0u;
    s.sumTickValid = false;
    s.sumTick = 0u;
    s.sumApp = 0u;
    s.sumTpl = 0u;
    for (int m = 0; m < 16; ++m)
    {
        s.sumHist[m] = 0u;
    }
    s.sumLines = 0u;
    s.numApc = 0;
    s.appxLines = 0u;
    s.v1b0Lines = 0u;
    s.twValid = false;
    s.twLines = 0u;
}

inline void clearForTest()
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.outOpen)
    {
        s.out.flush();
        s.out.close();
        s.outOpen = false;
    }
    s.initDone = false;
    detail::initDone().store(false, std::memory_order_relaxed);
    s.enabled = false;
    detail::enabledFlag().store(false, std::memory_order_relaxed);
    s.path.clear();
    s.from = 1270u;
    s.to = 1280u;
    s.numExtra = 0;
    for (int i = 0; i < kMaxExtra; ++i)
    {
        s.extra[i] = 0u;
    }
    for (int i = 0; i < kMaxWords; ++i)
    {
        s.perWord[i] = 0u;
    }
    s.kickValid = false;
    s.kickTick = 0u;
    s.kickCount = 0u;
    s.vif0UnkLines = 0u;
    s.vu0CallLines = 0u;
    s.sprModLines = 0u;
    s.lastLines = 0u;
    for (int i = 0; i < kFixedWords; ++i)
    {
        s.lastW[i].valid = false;
    }
    for (int i = 0; i < kMaxExtra; ++i)
    {
        s.eb[i].dirty = false;
    }
    s.ebFlushValid = false;
    s.ebFlushTick = 0u;
    s.ebLines = 0u;
    detail::memSuppressed() = false;
    s.appendOn = false;
    detail::appendOnFlag().store(false, std::memory_order_relaxed);
    detail::tplArmedFlag().store(false, std::memory_order_relaxed);
    s.appLines = 0u;
    s.tplBase = 0u;
    s.tplLast0 = 0u;
    s.tplLast1 = 0u;
    s.tplLines = 0u;
    s.tplmLines = 0u;
    s.sumTickValid = false;
    s.sumTick = 0u;
    s.sumApp = 0u;
    s.sumTpl = 0u;
    for (int m = 0; m < 16; ++m)
    {
        s.sumHist[m] = 0u;
    }
    s.sumLines = 0u;
    s.numApc = 0;
    s.appxLines = 0u;
    s.v1b0Lines = 0u;
    s.twValid = false;
    s.twLines = 0u;
}

// Part 4 test config: arms the append/template watch on an already
// configured (path + window) state. Call after configureForTest.
inline void configureAppendForTest()
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.appendOn = true;
    detail::appendOnFlag().store(true, std::memory_order_relaxed);
}

} // namespace ps2_e44_trace
