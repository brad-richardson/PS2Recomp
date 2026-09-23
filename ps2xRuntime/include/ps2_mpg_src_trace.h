// E40 DEV-ONLY VIF1 MPG source-address trace behind PS2X_MPG_SRC_TRACE=<file>.
//
// Master gate: PS2X_MPG_SRC_TRACE names the text file receiving `mpgsrc`
// lines (one per qualifying VIF1 REF/REFS/REFE tag seen by the DMA chain
// walker), `tagwrite` lines (guest stores to an armed tag's addr word)
// and `srcread` lines (guest loads overlapping the two fixed microcode
// source regions, first 64 hits each). Unset/empty (default) = one
// relaxed atomic check per tap; zero guest-visible behavior change, no I/O.
// Optional binds: PS2X_MPG_SRC_TRACE_FROM / PS2X_MPG_SRC_TRACE_TO
// (inclusive guest-vsync window; default 0..2^64-1, vsync from GS vsyncTick).
// Hard cap: 2,000 lines total (both kinds), then the file goes quiet.
//
// A tag qualifies when it is a VIF1 (0x10009000) chain tag with id 0/3/4
// (REF/REFS/REFE) AND either its REF addr falls in [0x430000,0x440000) or a
// bounded command-boundary scan of its payload finds a VIF MPG opcode. The
// scan is a bonus path: it walks at most 64 QWs, understands fixed-size
// commands plus MPG (hit) and DIRECT (skip), and stops conservatively at
// UNPACK/overrun, so out-of-range MPGs deeper in a payload may be missed
// (stated gap). In-range tags always log.
//
// `mpgsrc` line:
//   mpgsrc vsync=<n> tag_at=0x<EE addr of the tag> id=<0/3/4> qwc=<n>
//     addr=0x<ref addr> tte_vif=<16 hex digits: tag bytes 8..16 in stream order>
// The first time a new tag_at is logged (and the vsync is inside the
// window), a write watch is armed on tag_at+4 (the tag's addr word, holding
// the REF addr in bits 32..63 of the 128-bit tag). At most 64 watches.
//
// `tagwrite` line (one per armed watch overlapped by a guest store):
//   tagwrite vsync=<n> addr=0x<watched word> value=0x<overlapped word>
//     pc=0x<guest pc> ra=0x<> fn=<host func, i.e. sub_* for guest code>
//     a0=.. a1=.. a2=.. a3=.. v0=.. v1=.. t0=.. .. t9=..
// with the GPRs ($a0-$a3,$v0,$v1,$t0-$t9) read at the store.
//
// Plumbing (cited reuse, not a new mechanism): guest RAM stores funnel
// through the WRITE8/16/32/64/128 macros in ps2_runtime_macros.h, which
// already host the P1f watchpoint (ps2DiagWatchReport, gated by
// PS2X_DIAG_WATCH) and the no-op ps2TraceGuestWrite tap. The E40 store
// hook sits alongside them behind writeArmed() (one relaxed atomic check
// when off) and forwards runtime/ctx/__func__ to noteStoreCtx.
// Constant-address stores emitted as inlined `ps2TraceGuestWrite(...);
// FAST_WRITE*(...)` sequences in generated code bypass the macros and are
// NOT watched (stated gap); chain-builder tag writes are dynamic
// (register+offset) and use the macros. Part-2 adds the same tap shape to
// the READ8/16/32/64/128 macros (behind readArmed() plus a lock-free
// address pre-filter, since loads are hotter), forwarding to
// noteReadCtx. Inlined constant-address FAST_READ* sequences bypass it
// the same way (stated gap). Part-3 adds (a) a payload source map: the
// VIF1 chain walker records EE spans per appended byte range
// (Ps2VifSrcSpan cargo on PendingTransfer) and each delivery installs it
// around processVIF1Data, so the MPG handler logs `mpgpay` with the EE
// source of dest-0 payloads (normal mode installs MADR-based spans,
// FIFO a sourceless marker); (b) a `dmareg` watch on the VIF1 DMA
// registers behind dmaregArmed(), tapped from WRITE8/16/32/64 (generated
// code has no inlined stores to 0x1000xxxx, verified codegen-wide, so the
// macros see every DMA-reg write). Part-4 adds (a) an `arenastore` watch
// on the two chain arenas behind arenastoreArmed() (WRITE32/64/128, one
// line per matching 32-bit lane); (b) a `ctag` dump of every tag walked
// in the first two in-window VIF1 chain walks.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "ps2_runtime.h"
#include "ps2_vif_src_span.h"

namespace ps2_mpg_src_trace
{

// E40 Part-5: raised from 2000 for the 150-vsync scene-build window
// (first-2-kick ctag alone is ~1700 lines; per-frame mpgpay/dmareg add
// ~8/vsync): the file must survive to vsync 1300 so the arenastore and
// uploadload quarries are not cut off by an early global cap.
// E40 Part-7: raised to 40000 for the whole-boot window 0-1300 (uncapped
// st/sema/irq/mpgpay lines run ~19/vsync over 1300 vsyncs ≈ 24K).
inline constexpr uint64_t kMaxLines = 40000ull;
inline constexpr uint64_t kFlushEvery = 128ull;
inline constexpr uint32_t kMaxWatches = 64u;
inline constexpr uint32_t kAddrLo = 0x00430000u;
inline constexpr uint32_t kAddrHi = 0x00440000u;

// GPR indices logged on a tagwrite: a0-a3, v0-v1, t0-t9.
inline constexpr int kWatchRegCount = 16;
inline constexpr int kWatchRegs[16] = {4, 5, 6, 7, 2, 3, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25};
inline constexpr const char *kWatchRegNames[16] = {
    "a0", "a1", "a2", "a3", "v0", "v1",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9"};

// E40 Part-2: guest READ watch. Two fixed EE regions (the VU1 microcode
// source image heads seen by the orchestrator): 16 bytes each. The first
// kMaxReadHits reads overlapping each region inside the vsync window log
// one `srcread` line:
//   srcread vsync=<n> addr=0x<read addr> size=<bytes> pc=0x<guest pc>
//     ra=0x<> fn=<host func, i.e. sub_* for guest code>
//     a0..a3 v0 v1 t0..t9 s0..s7 (low 32 bits hex)
// with the GPRs read at the load.
inline constexpr uint32_t kReadBase[2] = {0x00435BF8u, 0x004349B8u};
inline constexpr uint32_t kReadSize = 16u;
inline constexpr uint64_t kMaxReadHits = 64ull;

// GPR indices logged on a srcread: a0-a3, v0-v1, t0-t9, s0-s7.
inline constexpr int kReadRegCount = 24;
inline constexpr int kReadRegs[24] = {4, 5, 6, 7, 2, 3,
                                      8, 9, 10, 11, 12, 13, 14, 15, 24, 25,
                                      16, 17, 18, 19, 20, 21, 22, 23};
inline constexpr const char *kReadRegNames[24] = {
    "a0", "a1", "a2", "a3", "v0", "v1",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7"};

// E40 Part-4: chain-arena store watch. The per-frame VIF1 chains live in
// heap arenas (defaults below, from the Part-3 TADR values). Any guest
// WRITE32/64/128 overlapping them whose stored 32-bit
// word (any lane) falls in the microcode library range (or its
// 0x20/0x30/0x80 mirrors, folded by & 0x0FFFFFFF) logs one `arenastore`
// line per matching lane:
//   arenastore vsync=<n> addr=0x<lane addr> value=0x<lane word>
//     pc=0x<guest pc> ra=0x<> fn=<host func> + a0..s7 (low 32 hex)
// First kMaxArenaLines in-window lines.
// E40 Part-5: the watched ranges are env-configurable via
// PS2X_MPG_SRC_ARENAS="lo-hi,lo-hi" (hex, 0x optional, up to kMaxArenas
// ranges; truncated past the cap). Absent or malformed env keeps the
// defaults below.
inline constexpr int kMaxArenas = 8;
inline constexpr uint32_t kArenaDefaultBase[2] = {0x0063B800u, 0x00708400u};
inline constexpr uint32_t kArenaDefaultEnd[2] = {0x0063C400u, 0x00708D00u};
inline constexpr uint64_t kMaxArenaLines = 256ull;
// E40 Part-7 sizes (struct State uses them; values documented at the watch).
inline constexpr int kTagMaxWords = 16;
inline constexpr uint32_t kTagDefaultAddr[4] = {0x0063B994u, 0x0063BBE4u, 0x0063BEA4u, 0x0063C134u};
inline constexpr uint64_t kMaxTagHitsPerWord = 64ull;
inline constexpr int kSrcRing = 16;

// Parses "lo-hi,lo-hi" into base/end (at most maxN ranges). Returns the
// range count, or -1 when any pair is malformed (empty, non-hex,
// missing '-', lo >= hi). Over-long lists truncate to maxN.
inline bool parseHexU32(const char *&p, uint32_t &out)
{
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p += 2;
    }
    uint32_t value = 0u;
    int digits = 0;
    for (;; ++p)
    {
        const char c = *p;
        uint32_t d;
        if (c >= '0' && c <= '9')
            d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = static_cast<uint32_t>(c - 'A' + 10);
        else
            break;
        value = value * 16u + d;
        ++digits;
    }
    if (digits == 0)
    {
        return false;
    }
    out = value;
    return true;
}

inline int parseArenas(const char *text, uint32_t *base, uint32_t *end, int maxN)
{
    if (text == nullptr || maxN <= 0)
    {
        return -1;
    }
    const char *p = text;
    int n = 0;
    for (;;)
    {
        uint32_t lo = 0u, hi = 0u;
        if (!parseHexU32(p, lo))
        {
            return -1;
        }
        if (*p != '-')
        {
            return -1;
        }
        ++p;
        if (!parseHexU32(p, hi))
        {
            return -1;
        }
        if (lo >= hi)
        {
            return -1;
        }
        if (n < maxN)
        {
            base[n] = lo;
            end[n] = hi;
            ++n;
        }
        if (*p == '\0')
        {
            return n == 0 ? -1 : n;
        }
        if (*p != ',')
        {
            return -1;
        }
        ++p;
        if (*p == '\0')
        {
            return -1;
        }
    }
}

// E40 Part-7: parses "a,b,c" hex words (at most maxN). Returns the word
// count, or -1 when any entry is malformed.
inline int parseTagAddrs(const char *text, uint32_t *words, int maxN)
{
    if (text == nullptr || words == nullptr || maxN <= 0)
    {
        return -1;
    }
    const char *p = text;
    int n = 0;
    for (;;)
    {
        uint32_t w = 0u;
        if (!parseHexU32(p, w))
        {
            return -1;
        }
        if (n < maxN)
        {
            words[n++] = w;
        }
        if (*p == '\0')
        {
            return n == 0 ? -1 : n;
        }
        if (*p != ',')
        {
            return -1;
        }
        ++p;
        if (*p == '\0')
        {
            return -1;
        }
    }
}

inline constexpr uint32_t kLibLo = 0x00430000u;
inline constexpr uint32_t kLibHi = 0x00440000u;

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
        std::vector<uint32_t> watches; // armed tag_at+4 words
        uint64_t readHits[2] = {0u, 0u}; // per-region srcread hits
        // E40 Part-3 active payload source map (one delivery at a time;
        // the EE thread delivers synchronously).
        bool payInstalled = false;
        const uint8_t *payBase = nullptr;
        uint32_t payLen = 0u;
        uint32_t payEeBase = 0u;
        uint32_t payMode = 0u;
        std::vector<Ps2VifSrcSpan> paySpans;
        uint64_t dmHits = 0u; // dmareg lines emitted
        uint64_t arHits = 0u; // arenastore lines emitted
        uint64_t ctagKicks = 0u; // VIF1 chain walks with tag dump
        uint64_t ulHits = 0u; // uploadload lines emitted
        uint32_t arenaBase[kMaxArenas]; // watched store ranges
        uint32_t arenaEnd[kMaxArenas];
        int arenaCount = 0;
        uint32_t tagAddr[kTagMaxWords]; // watched ADDR words
        int tagCount = 0;
        uint64_t tagHits[kTagMaxWords]; // per-word tagaddrwrite hits
        uint32_t srcAddr[kSrcRing]; // recent uploader-valued load addrs
        uint32_t srcVal[kSrcRing]; // recent uploader-valued load values
        int srcHead = 0;
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

    inline std::atomic<uint32_t> &armedCount()
    {
        static std::atomic<uint32_t> n{0u};
        return n;
    }

    // 1 while any read region still has hits left (set under lock).
    inline std::atomic<uint32_t> &readOpen()
    {
        static std::atomic<uint32_t> n{1u};
        return n;
    }

    // 1 while a payload source map is installed (set under lock).
    inline std::atomic<uint32_t> &payInstalledFlag()
    {
        static std::atomic<uint32_t> n{0u};
        return n;
    }

    // 1 while the dmareg watch still has lines left (set under lock).
    inline std::atomic<uint32_t> &dmOpen()
    {
        static std::atomic<uint32_t> n{1u};
        return n;
    }

    // 1 while the arenastore watch still has lines left (set under lock).
    inline std::atomic<uint32_t> &arOpen()
    {
        static std::atomic<uint32_t> n{1u};
        return n;
    }

    // 1 while the uploadload watch still has lines left (set under lock).
    inline std::atomic<uint32_t> &ulOpen()
    {
        static std::atomic<uint32_t> n{1u};
        return n;
    }

    // 1 while at least one tagaddr word still has hits left (set under lock).
    inline std::atomic<uint32_t> &tagOpen()
    {
        static std::atomic<uint32_t> n{1u};
        return n;
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

    // Installs defaults, then the PS2X_MPG_SRC_ARENAS env override when
    // it parses. Shared by initLocked and the test hook below.
    inline void installArenasLocked(State &s, const char *env)
    {
        s.arenaBase[0] = kArenaDefaultBase[0];
        s.arenaEnd[0] = kArenaDefaultEnd[0];
        s.arenaBase[1] = kArenaDefaultBase[1];
        s.arenaEnd[1] = kArenaDefaultEnd[1];
        s.arenaCount = 2;
        if (env != nullptr && env[0] != '\0')
        {
            uint32_t base[kMaxArenas];
            uint32_t end[kMaxArenas];
            const int n = parseArenas(env, base, end, kMaxArenas);
            if (n > 0)
            {
                for (int i = 0; i < n; ++i)
                {
                    s.arenaBase[i] = base[i];
                    s.arenaEnd[i] = end[i];
                }
                s.arenaCount = n;
            }
        }
    }

    // Installs default tagaddr words, then the PS2X_MPG_SRC_TAGADDRS
    // env override when it parses. Shared by initLocked and tests.
    inline void installTagAddrsLocked(State &s, const char *env)
    {
        s.tagAddr[0] = kTagDefaultAddr[0];
        s.tagAddr[1] = kTagDefaultAddr[1];
        s.tagAddr[2] = kTagDefaultAddr[2];
        s.tagAddr[3] = kTagDefaultAddr[3];
        s.tagCount = 4;
        if (env != nullptr && env[0] != '\0')
        {
            uint32_t words[kTagMaxWords];
            const int n = parseTagAddrs(env, words, kTagMaxWords);
            if (n > 0)
            {
                for (int i = 0; i < n; ++i)
                {
                    s.tagAddr[i] = words[i];
                }
                s.tagCount = n;
            }
        }
        for (int i = 0; i < kTagMaxWords; ++i)
        {
            s.tagHits[i] = 0u;
        }
        for (int i = 0; i < kSrcRing; ++i)
        {
            s.srcAddr[i] = 0u;
            s.srcVal[i] = 0u;
        }
        s.srcHead = 0;
    }

    inline void initLocked(State &s)
    {
        s.initDone = true;
        installArenasLocked(s, std::getenv("PS2X_MPG_SRC_ARENAS"));
        installTagAddrsLocked(s, std::getenv("PS2X_MPG_SRC_TAGADDRS"));
        const char *file = std::getenv("PS2X_MPG_SRC_TRACE");
        if (!file || file[0] == '\0')
        {
            return;
        }
        s.path = file;
        uint64_t from = 0u;
        uint64_t to = ~0ull;
        if (const char *env = std::getenv("PS2X_MPG_SRC_TRACE_FROM"))
        {
            if (!parseU64(env, from))
            {
                return;
            }
        }
        if (const char *env = std::getenv("PS2X_MPG_SRC_TRACE_TO"))
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

    inline void emitLineLocked(State &s, const char *line)
    {
        openLocked(s);
        if (!s.outOpen)
        {
            return;
        }
        s.out << line << '\n';
        ++s.linesWritten;
        // Boot harnesses SIGTERM the runner at the wall cap, which skips
        // static destructors and drops the final stdio buffer. Flush
        // periodically so a killed run loses at most kFlushEvery lines.
        if ((s.linesWritten % kFlushEvery) == 0u)
        {
            s.out.flush();
        }
        if (s.linesWritten >= kMaxLines)
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

// Fast gate for the per-store macro hook: enabled AND at least one watch.
inline bool writeArmed()
{
    if (!enabled())
    {
        return false;
    }
    return detail::armedCount().load(std::memory_order_relaxed) != 0u;
}

// Walker side: log one mpgsrc line and arm a watch on tagAt+4. No-ops
// outside the vsync window (no line, no arm).
inline void noteMpgsrc(uint64_t vsync, uint32_t tagAt, uint32_t id,
                       uint32_t qwc, uint32_t addr,
                       uint32_t tteWord0, uint32_t tteWord1)
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
    char line[192];
    std::snprintf(line, sizeof(line),
                  "mpgsrc vsync=%llu tag_at=0x%08x id=%u qwc=%u addr=0x%08x tte_vif=%08x%08x",
                  static_cast<unsigned long long>(vsync), tagAt, id, qwc,
                  addr, tteWord0, tteWord1);
    detail::emitLineLocked(s, line);
    if (s.capped)
    {
        return;
    }
    const uint32_t watch = tagAt + 4u;
    bool known = false;
    for (const uint32_t w : s.watches)
    {
        if (w == watch)
        {
            known = true;
            break;
        }
    }
    if (!known && s.watches.size() < kMaxWatches)
    {
        s.watches.push_back(watch);
        detail::armedCount().store(static_cast<uint32_t>(s.watches.size()),
                                   std::memory_order_relaxed);
    }
}

// Store side (called from the WRITE* macros via noteStoreCtx): for every
// armed watch overlapped by [addr, addr+width), emit one tagwrite line with
// the overlapped 32-bit word, pc/ra/fn and the computing GPRs.
inline void noteStore(uint64_t vsync, uint32_t addr, uint32_t width,
                      uint64_t valueLo, uint64_t valueHi,
                      uint32_t pc, uint32_t ra, const char *fn,
                      const R5900Context *ctx)
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
    if (s.watches.empty())
    {
        return;
    }
    uint8_t bytes[16];
    std::memcpy(bytes + 0u, &valueLo, sizeof(valueLo));
    std::memcpy(bytes + 8u, &valueHi, sizeof(valueHi));
    for (const uint32_t watch : s.watches)
    {
        if (addr + width <= watch || watch + 4u <= addr)
        {
            continue;
        }
        // Overlapped word, little-endian byte pick.
        uint32_t value = 0u;
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            const uint32_t src = watch + i;
            if (src < addr || src >= addr + width || src - addr >= 16u)
            {
                continue;
            }
            value |= static_cast<uint32_t>(bytes[src - addr]) << (i * 8u);
        }
        char line[640];
        int w = std::snprintf(line, sizeof(line),
                              "tagwrite vsync=%llu addr=0x%08x value=0x%08x pc=0x%08x ra=0x%08x fn=%s",
                              static_cast<unsigned long long>(vsync), watch, value,
                              pc, ra, fn ? fn : "?");
        for (int r = 0; r < kWatchRegCount && w > 0; ++r)
        {
            const uint32_t rv = (ctx != nullptr) ? getRegU32(ctx, kWatchRegs[r]) : 0u;
            w += std::snprintf(line + w, sizeof(line) - static_cast<size_t>(w),
                               " %s=0x%08x", kWatchRegNames[r], rv);
        }
        if (w > 0)
        {
            detail::emitLineLocked(s, line);
            if (s.capped)
            {
                return;
            }
        }
    }
}

// Lock-free pre-filter for the per-load macro hook: true when
// [addr, addr+size) overlaps a read region. No state, no atomics.
inline bool isReadWatched(uint32_t addr, uint32_t size)
{
    for (int i = 0; i < 2; ++i)
    {
        if (addr < kReadBase[i] + kReadSize && kReadBase[i] < addr + size)
        {
            return true;
        }
    }
    return false;
}

// Fast gate for the per-load macro hook: enabled AND a region still open.
inline bool readArmed()
{
    if (!enabled())
    {
        return false;
    }
    return detail::readOpen().load(std::memory_order_relaxed) != 0u;
}

// Load side (called from the READ* macros via noteReadCtx): the first
// kMaxReadHits reads overlapping each region inside the window emit one
// srcread line with pc/ra/fn and the pointer-forming GPRs (incl. s0-s7).
// Out-of-window reads do not consume hits. One line per load at most
// (the regions are disjoint).
inline void noteRead(uint64_t vsync, uint32_t addr, uint32_t size,
                     uint32_t pc, uint32_t ra, const char *fn,
                     const R5900Context *ctx)
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
    for (int i = 0; i < 2; ++i)
    {
        const uint32_t base = kReadBase[i];
        if (addr + size <= base || base + kReadSize <= addr)
        {
            continue;
        }
        if (s.readHits[i] >= kMaxReadHits)
        {
            continue;
        }
        char line[768];
        int w = std::snprintf(line, sizeof(line),
                              "srcread vsync=%llu addr=0x%08x size=%u pc=0x%08x ra=0x%08x fn=%s",
                              static_cast<unsigned long long>(vsync), addr, size,
                              pc, ra, fn ? fn : "?");
        for (int r = 0; r < kReadRegCount && w > 0; ++r)
        {
            const uint32_t rv = (ctx != nullptr) ? getRegU32(ctx, kReadRegs[r]) : 0u;
            w += std::snprintf(line + w, sizeof(line) - static_cast<size_t>(w),
                               " %s=0x%08x", kReadRegNames[r], rv);
        }
        if (w > 0)
        {
            detail::emitLineLocked(s, line);
            ++s.readHits[i];
            if (s.readHits[0] >= kMaxReadHits && s.readHits[1] >= kMaxReadHits)
            {
                detail::readOpen().store(0u, std::memory_order_relaxed);
            }
        }
        return;
    }
}

// Macro glue: extracts vsync/pc/ra/GPRs from the load site context.
inline void noteReadCtx(const PS2Runtime *runtime, const R5900Context *ctx,
                        uint32_t addr, uint32_t size, const char *fn)
{
    uint64_t vsync = 0u;
    if (runtime != nullptr)
    {
        vsync = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
    }
    const uint32_t pc = (ctx != nullptr) ? ctx->pc : 0u;
    const uint32_t ra = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
    noteRead(vsync, addr, size, pc, ra, fn, ctx);
}

// Macro glue: extracts vsync/pc/ra/GPRs from the store site context.
inline void noteStoreCtx(const PS2Runtime *runtime, const R5900Context *ctx,
                         uint32_t addr, uint32_t width,
                         uint64_t valueLo, uint64_t valueHi, const char *fn)
{
    uint64_t vsync = 0u;
    if (runtime != nullptr)
    {
        vsync = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
    }
    const uint32_t pc = (ctx != nullptr) ? ctx->pc : 0u;
    const uint32_t ra = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
    noteStore(vsync, addr, width, valueLo, valueHi, pc, ra, fn, ctx);
}

// E40 Part-3: payload source map. Installed around each VIF1 delivery
// so the MPG handler can attribute dest-0 payload bytes to the EE
// address they came from.
inline constexpr uint32_t PayNone = 0u;
inline constexpr uint32_t PayChain = 1u;
inline constexpr uint32_t PayNormal = 2u;
inline constexpr uint32_t PayFifo = 3u;

// Install a map for one delivery: buffer [base, base+len) reads EE from
// eeBase when spans is empty (normal mode), or per-span when given
// (chain mode). Fifo carries no spans and no base.
inline void setPayMap(const uint8_t *base, uint32_t len,
                      const Ps2VifSrcSpan *spans, uint32_t n,
                      uint32_t mode, uint32_t eeBase)
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.payInstalled = true;
    s.payBase = base;
    s.payLen = len;
    s.payEeBase = eeBase;
    s.payMode = mode;
    s.paySpans.clear();
    if (spans != nullptr && n > 0u)
    {
        s.paySpans.assign(spans, spans + n);
    }
    detail::payInstalledFlag().store(1u, std::memory_order_relaxed);
}

inline void clearPayMap()
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.payInstalled = false;
    s.payBase = nullptr;
    s.payLen = 0u;
    s.payEeBase = 0u;
    s.payMode = PayNone;
    s.paySpans.clear();
    detail::payInstalledFlag().store(0u, std::memory_order_relaxed);
}

// Fast gate for the MPG-handler hook: enabled AND a map installed.
inline bool payArmed()
{
    if (!enabled())
    {
        return false;
    }
    return detail::payInstalledFlag().load(std::memory_order_relaxed) != 0u;
}

// Attribute a payload byte to its EE source. Returns false when no map
// is installed or the pointer falls outside it.
inline bool lookupPay(const uint8_t *p, uint32_t &ee, int32_t &tagId,
                      uint32_t &tagAt, uint32_t &mode)
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.payInstalled)
    {
        return false;
    }
    mode = s.payMode;
    if (mode == PayFifo)
    {
        ee = 0u;
        tagId = -1;
        tagAt = 0u;
        return true;
    }
    for (const Ps2VifSrcSpan &span : s.paySpans)
    {
        if (p >= s.payBase + span.bufOff && p < s.payBase + span.bufOff + span.len)
        {
            ee = span.eeAddr + static_cast<uint32_t>(p - (s.payBase + span.bufOff));
            tagId = span.tagId;
            tagAt = span.tagAt;
            return true;
        }
    }
    if (mode == PayNormal && s.payBase != nullptr &&
        p >= s.payBase && p < s.payBase + s.payLen)
    {
        ee = s.payEeBase + static_cast<uint32_t>(p - s.payBase);
        tagId = -1;
        tagAt = 0u;
        return true;
    }
    return false;
}

// `mpgpay` line for a dest-0 upload with a mapped source:
//   mpgpay vsync=<n> imm=0 num=<n> src=0x<raw EE> srcmask=0x<raw & 0x1FFFFFFF>
//     mode=<chain:<id>|normal|fifo> tag_at=0x<…> (chain) or -
// The uncached/KSEG-mirror note: a REF addr like 0x20435BF8 aliases the
// same RAM as 0x00435BF8; srcmask folds all mirrors to one value.
inline void noteMpgpay(uint64_t vsync, uint32_t num, uint32_t ee,
                       uint32_t mode, int32_t tagId, uint32_t tagAt)
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
    char modestr[16];
    if (mode == PayChain)
        std::snprintf(modestr, sizeof(modestr), "chain:%d", tagId);
    else if (mode == PayNormal)
        std::snprintf(modestr, sizeof(modestr), "normal");
    else
        std::snprintf(modestr, sizeof(modestr), "fifo");
    char tagstr[16];
    if (mode == PayChain)
        std::snprintf(tagstr, sizeof(tagstr), "0x%08x", tagAt);
    else
        std::snprintf(tagstr, sizeof(tagstr), "-");
    char line[192];
    std::snprintf(line, sizeof(line),
                  "mpgpay vsync=%llu imm=0 num=%u src=0x%08x srcmask=0x%08x mode=%s tag_at=%s",
                  static_cast<unsigned long long>(vsync), num,
                  ee, ee & 0x1FFFFFFFu, modestr, tagstr);
    detail::emitLineLocked(s, line);
}

// E40 Part-3: VIF1 DMA register store watch (D1_CHCR/MADR/QWC/TADR).
// First kMaxDmLines stores in-window log one `dmareg` line:
//   dmareg vsync=<n> reg=<CHCR|MADR|QWC|TADR> value=0x<low 32 bits>
//     pc=0x<guest pc> ra=0x<> fn=<host func> + a0..s7 (low 32 hex)
inline constexpr uint32_t kDmaChcr = 0x10009000u;
inline constexpr uint32_t kDmaMadr = 0x10009010u;
inline constexpr uint32_t kDmaQwc = 0x10009020u;
inline constexpr uint32_t kDmaTadr = 0x10009030u;
inline constexpr uint64_t kMaxDmLines = 256ull;

inline bool isDmareg(uint32_t addr)
{
    return addr == kDmaChcr || addr == kDmaMadr || addr == kDmaQwc || addr == kDmaTadr;
}

inline const char *dmaregName(uint32_t addr)
{
    if (addr == kDmaChcr)
        return "CHCR";
    if (addr == kDmaMadr)
        return "MADR";
    if (addr == kDmaQwc)
        return "QWC";
    return "TADR";
}

// Fast gate for the per-store macro hook: enabled AND lines left.
inline bool dmaregArmed()
{
    if (!enabled())
    {
        return false;
    }
    return detail::dmOpen().load(std::memory_order_relaxed) != 0u;
}

inline void noteDmareg(uint64_t vsync, uint32_t addr, uint32_t value,
                       uint32_t pc, uint32_t ra, const char *fn,
                       const R5900Context *ctx)
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
    if (!isDmareg(addr) || s.dmHits >= kMaxDmLines)
    {
        return;
    }
    char line[768];
    int w = std::snprintf(line, sizeof(line),
                          "dmareg vsync=%llu reg=%s value=0x%08x pc=0x%08x ra=0x%08x fn=%s",
                          static_cast<unsigned long long>(vsync), dmaregName(addr), value,
                          pc, ra, fn ? fn : "?");
    for (int r = 0; r < kReadRegCount && w > 0; ++r)
    {
        const uint32_t rv = (ctx != nullptr) ? getRegU32(ctx, kReadRegs[r]) : 0u;
        w += std::snprintf(line + w, sizeof(line) - static_cast<size_t>(w),
                           " %s=0x%08x", kReadRegNames[r], rv);
    }
    if (w > 0)
    {
        detail::emitLineLocked(s, line);
        ++s.dmHits;
        if (s.dmHits >= kMaxDmLines)
        {
            detail::dmOpen().store(0u, std::memory_order_relaxed);
        }
    }
}

// Macro glue for the WRITE* taps (do…while scope: __func__ is already
// the host function, no capture needed).
inline void noteDmaregCtx(const PS2Runtime *runtime, const R5900Context *ctx,
                          uint32_t addr, uint64_t value, const char *fn)
{
    uint64_t vsync = 0u;
    if (runtime != nullptr)
    {
        vsync = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
    }
    const uint32_t pc = (ctx != nullptr) ? ctx->pc : 0u;
    const uint32_t ra = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
    noteDmareg(vsync, addr, static_cast<uint32_t>(value), pc, ra, fn, ctx);
}

// Lock-free pre-filter for the arena store hook: true when
// [addr, addr+size) overlaps a watched arena (env-configurable; the
// ranges are installed once at init before the tracer enables, and the
// test hooks reinstall them under lock). Reads plain state, no atomics.
inline bool isArenaWatched(uint32_t addr, uint32_t size)
{
    detail::State &s = detail::state();
    const int n = s.arenaCount;
    for (int i = 0; i < n; ++i)
    {
        if (addr < s.arenaEnd[i] && s.arenaBase[i] < addr + size)
        {
            return true;
        }
    }
    return false;
}

// True when a stored word points into the microcode library range,
// folding the 0x20/0x30/0x80 (and 0xA0/0xB0) EE RAM mirrors. The 0x30
// form keeps bit 28, so the fold must clear it too (the runtime itself
// maps 0x20000000-0x3FFFFFFF onto RDRAM).
inline bool isArenaValue(uint32_t word)
{
    const uint32_t masked = word & 0x0FFFFFFFu;
    return masked >= kLibLo && masked < kLibHi;
}

// Fast gate for the per-store macro hook: enabled AND lines left.
inline bool arenastoreArmed()
{
    if (!enabled())
    {
        return false;
    }
    return detail::arOpen().load(std::memory_order_relaxed) != 0u;
}

// Store side (called from the WRITE32/64/128 macros via
// noteArenastoreCtx): one `arenastore` line per 32-bit lane that both
// overlaps an arena and carries a library-range word, until the cap.
inline void noteArenastore(uint64_t vsync, uint32_t addr, uint32_t width,
                           uint64_t valueLo, uint64_t valueHi,
                           uint32_t pc, uint32_t ra, const char *fn,
                           const R5900Context *ctx)
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
    uint32_t lanes = 1u;
    if (width >= 16u)
        lanes = 4u;
    else if (width >= 8u)
        lanes = 2u;
    for (uint32_t i = 0u; i < lanes; ++i)
    {
        if (s.arHits >= kMaxArenaLines)
        {
            break;
        }
        const uint32_t laneAddr = addr + i * 4u;
        uint32_t word;
        if (i == 0u)
            word = static_cast<uint32_t>(valueLo);
        else if (i == 1u)
            word = static_cast<uint32_t>(valueLo >> 32u);
        else if (i == 2u)
            word = static_cast<uint32_t>(valueHi);
        else
            word = static_cast<uint32_t>(valueHi >> 32u);
        if (!isArenaWatched(laneAddr, 4u) || !isArenaValue(word))
        {
            continue;
        }
        char line[768];
        int w = std::snprintf(line, sizeof(line),
                              "arenastore vsync=%llu addr=0x%08x value=0x%08x pc=0x%08x ra=0x%08x fn=%s",
                              static_cast<unsigned long long>(vsync), laneAddr, word,
                              pc, ra, fn ? fn : "?");
        for (int r = 0; r < kReadRegCount && w > 0; ++r)
        {
            const uint32_t rv = (ctx != nullptr) ? getRegU32(ctx, kReadRegs[r]) : 0u;
            w += std::snprintf(line + w, sizeof(line) - static_cast<size_t>(w),
                               " %s=0x%08x", kReadRegNames[r], rv);
        }
        if (w > 0)
        {
            detail::emitLineLocked(s, line);
            ++s.arHits;
            if (s.capped)
            {
                return;
            }
        }
    }
    if (s.arHits >= kMaxArenaLines)
    {
        detail::arOpen().store(0u, std::memory_order_relaxed);
    }
}

// Macro glue for the WRITE32/64/128 taps (do…while scope: __func__ is
// already the host function, no capture needed).
inline void noteArenastoreCtx(const PS2Runtime *runtime, const R5900Context *ctx,
                              uint32_t addr, uint32_t width,
                              uint64_t valueLo, uint64_t valueHi, const char *fn)
{
    uint64_t vsync = 0u;
    if (runtime != nullptr)
    {
        vsync = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
    }
    const uint32_t pc = (ctx != nullptr) ? ctx->pc : 0u;
    const uint32_t ra = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
    noteArenastore(vsync, addr, width, valueLo, valueHi, pc, ra, fn, ctx);
}

// E40 Part-5: uploader-address load watch. Logs the first
// kMaxUploadloadLines in-window guest loads (READ32/64/128, any lane)
// whose returned 32-bit word folds (& 0x0FFFFFFF, same as arenastore)
// to the static uploader 0x435bd0 or the alt-uploader slot range
// 0x434990-0x4349b8 — i.e. where the chain builder reads the uploader
// address from (the table entry):
//   uploadload vsync=<n> pc=0x<> ra=0x<> fn=<host func> addr=0x<lane>
//     value=0x<lane word> + a0..s7 (low 32 hex)
inline constexpr uint32_t kUploaderExact = 0x00435BD0u;
inline constexpr uint32_t kUploaderAltLo = 0x00434990u;
inline constexpr uint32_t kUploaderAltHi = 0x004349B8u;
inline constexpr uint64_t kMaxUploadloadLines = 64ull;

inline bool isUploaderValue(uint32_t word)
{
    const uint32_t folded = word & 0x0FFFFFFFu;
    if (folded == kUploaderExact)
    {
        return true;
    }
    return folded >= kUploaderAltLo && folded <= kUploaderAltHi;
}

// Fast gate for the per-load macro hook: enabled AND lines left.
inline bool uploadloadArmed()
{
    if (!enabled())
    {
        return false;
    }
    return detail::ulOpen().load(std::memory_order_relaxed) != 0u;
}

inline void noteUploadload(uint64_t vsync, uint32_t addr, uint32_t width,
                           uint64_t valueLo, uint64_t valueHi,
                           uint32_t pc, uint32_t ra, const char *fn,
                           const R5900Context *ctx)
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
    uint32_t lanes = 1u;
    if (width >= 16u)
        lanes = 4u;
    else if (width >= 8u)
        lanes = 2u;
    for (uint32_t i = 0u; i < lanes; ++i)
    {
        if (s.ulHits >= kMaxUploadloadLines)
        {
            break;
        }
        const uint32_t laneAddr = addr + i * 4u;
        uint32_t word;
        if (i == 0u)
            word = static_cast<uint32_t>(valueLo);
        else if (i == 1u)
            word = static_cast<uint32_t>(valueLo >> 32u);
        else if (i == 2u)
            word = static_cast<uint32_t>(valueHi);
        else
            word = static_cast<uint32_t>(valueHi >> 32u);
        if (!isUploaderValue(word))
        {
            continue;
        }
        char line[768];
        int w = std::snprintf(line, sizeof(line),
                              "uploadload vsync=%llu pc=0x%08x ra=0x%08x fn=%s addr=0x%08x value=0x%08x",
                              static_cast<unsigned long long>(vsync), pc, ra, fn ? fn : "?",
                              laneAddr, word);
        for (int r = 0; r < kReadRegCount && w > 0; ++r)
        {
            const uint32_t rv = (ctx != nullptr) ? getRegU32(ctx, kReadRegs[r]) : 0u;
            w += std::snprintf(line + w, sizeof(line) - static_cast<size_t>(w),
                               " %s=0x%08x", kReadRegNames[r], rv);
        }
        if (w > 0)
        {
            detail::emitLineLocked(s, line);
            ++s.ulHits;
            if (s.capped)
            {
                return;
            }
        }
    }
    if (s.ulHits >= kMaxUploadloadLines)
    {
        detail::ulOpen().store(0u, std::memory_order_relaxed);
    }
}

// Macro glue for the READ32/64/128 taps: value already loaded.
inline void noteUploadloadCtx(const PS2Runtime *runtime, const R5900Context *ctx,
                              uint32_t addr, uint32_t width,
                              uint64_t valueLo, uint64_t valueHi, const char *fn)
{
    uint64_t vsync = 0u;
    if (runtime != nullptr)
    {
        vsync = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
    }
    const uint32_t pc = (ctx != nullptr) ? ctx->pc : 0u;
    const uint32_t ra = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
    noteUploadload(vsync, addr, width, valueLo, valueHi, pc, ra, fn, ctx);
}

// E40 Part-6: render-DMA-thread state sequence (mirrors T51's PCSX2
// format, one file, execution order). Over the vsync window:
//  1. every guest store (WRITE32/64/128 lanes; 8/16-bit stores are not
//     tapped) to the thread-struct fields 0x6214E0-0x62151F:
//       st vsync=<n> addr=0x<> value=0x<> pc=0x<> ra=0x<> fn=<> intc=<0|1>
//     (intc=1 while the executor runs an Interrupt-kind invocation).
//  2. SignalSema/iSignalSema/WaitSema/PollSema calls whose sema id
//     equals the live RAM word at 0x62150C (+0x5ACC) or 0x621508:
//       sema vsync=<n> pc=0x<> ra=0x<> call=<name> id=<n>
//  3. every INTC/DMAC handler dispatch the runtime performs:
//       irq vsync=<n> cause=0x<> ch=<n> handler=0x<>
//  4. D1 kicks (existing dmareg lines).
// No sub-caps (the window is 5 vsyncs); the global file cap bounds.
inline constexpr uint32_t kStBase = 0x006214E0u;
inline constexpr uint32_t kStEnd = 0x00621520u; // exclusive
inline constexpr uint32_t kSemaIdAddrA = 0x0062150Cu; // +0x5ACC
inline constexpr uint32_t kSemaIdAddrB = 0x00621508u;

inline bool isStWatched(uint32_t addr, uint32_t size)
{
    return addr < kStEnd && kStBase < addr + size;
}

// Gates: enabled only (window-checked inside; no sub-caps).
inline bool stArmed()
{
    return enabled();
}
inline bool semaArmed()
{
    return enabled();
}
inline bool irqArmed()
{
    return enabled();
}

inline void noteSt(uint64_t vsync, uint32_t addr, uint32_t width,
                   uint64_t valueLo, uint64_t valueHi,
                   uint32_t pc, uint32_t ra, const char *fn, uint32_t intc)
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
    uint32_t lanes = 1u;
    if (width >= 16u)
        lanes = 4u;
    else if (width >= 8u)
        lanes = 2u;
    for (uint32_t i = 0u; i < lanes; ++i)
    {
        const uint32_t laneAddr = addr + i * 4u;
        uint32_t word;
        if (i == 0u)
            word = static_cast<uint32_t>(valueLo);
        else if (i == 1u)
            word = static_cast<uint32_t>(valueLo >> 32u);
        else if (i == 2u)
            word = static_cast<uint32_t>(valueHi);
        else
            word = static_cast<uint32_t>(valueHi >> 32u);
        if (!isStWatched(laneAddr, 4u))
        {
            continue;
        }
        char line[256];
        const int w = std::snprintf(line, sizeof(line),
                                    "st vsync=%llu addr=0x%08x value=0x%08x pc=0x%08x ra=0x%08x fn=%s intc=%u",
                                    static_cast<unsigned long long>(vsync), laneAddr, word,
                                    pc, ra, fn ? fn : "?", intc);
        if (w > 0)
        {
            detail::emitLineLocked(s, line);
            if (s.capped)
            {
                return;
            }
        }
    }
}

// Mirrored executor interrupt state (the trace header cannot see the
// full scheduler type; EeScheduler.cpp mirrors m_insideInterrupt here
// at its assignment sites, same pattern as the ps2_e3 slice hook).
inline std::atomic<uint32_t> &sliceIrq()
{
    static std::atomic<uint32_t> n{0u};
    return n;
}

inline void noteSliceIrq(bool inside)
{
    sliceIrq().store(inside ? 1u : 0u, std::memory_order_relaxed);
}

// Macro glue for the WRITE32/64/128 taps.
inline void noteStCtx(const PS2Runtime *runtime, const R5900Context *ctx,
                      uint32_t addr, uint32_t width,
                      uint64_t valueLo, uint64_t valueHi, const char *fn)
{
    uint64_t vsync = 0u;
    if (runtime != nullptr)
    {
        vsync = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
    }
    const uint32_t intc = sliceIrq().load(std::memory_order_relaxed) != 0u ? 1u : 0u;
    const uint32_t pc = (ctx != nullptr) ? ctx->pc : 0u;
    const uint32_t ra = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
    noteSt(vsync, addr, width, valueLo, valueHi, pc, ra, fn, intc);
}

// Masked RAM word read for the sema-id filter (plain RAM only).
inline uint32_t readSemaIdWord(const uint8_t *rdram, uint32_t addr, bool &ok)
{
    ok = false;
    if (rdram == nullptr)
    {
        return 0u;
    }
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (offset > PS2_RAM_SIZE - sizeof(uint32_t))
    {
        return 0u;
    }
    uint32_t word = 0u;
    std::memcpy(&word, rdram + offset, sizeof(word));
    ok = true;
    return word;
}

inline void noteSema(uint64_t vsync, uint32_t pc, uint32_t ra,
                     const char *call, uint32_t id)
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
    char line[128];
    const int w = std::snprintf(line, sizeof(line),
                                "sema vsync=%llu pc=0x%08x ra=0x%08x call=%s id=%u",
                                static_cast<unsigned long long>(vsync), pc, ra,
                                call ? call : "?", id);
    if (w > 0)
    {
        detail::emitLineLocked(s, line);
    }
}

// Syscall-dispatch glue: logs when the sema id (a0) equals either
// watched id word. Called from the Dispatcher sema cases.
inline void noteSemaDispatch(const uint8_t *rdram, const R5900Context *ctx,
                             PS2Runtime *runtime, const char *call)
{
    if (!semaArmed())
    {
        return;
    }
    const uint32_t id = (ctx != nullptr) ? getRegU32(ctx, 4) : 0u;
    bool okA = false, okB = false;
    const uint32_t watchA = readSemaIdWord(rdram, kSemaIdAddrA, okA);
    const uint32_t watchB = readSemaIdWord(rdram, kSemaIdAddrB, okB);
    if (!((okA && id == watchA) || (okB && id == watchB)))
    {
        return;
    }
    uint64_t vsync = 0u;
    if (runtime != nullptr)
    {
        vsync = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
    }
    const uint32_t pc = (ctx != nullptr) ? ctx->pc : 0u;
    const uint32_t ra = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
    noteSema(vsync, pc, ra, call, id);
}

inline void noteIrq(uint64_t vsync, bool dmac, uint32_t cause, uint32_t handler)
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
    // ch is the DMA channel for DMAC dispatch (cause == channel there);
    // INTC lines carry no channel (no INTC dispatch exists today).
    const uint32_t ch = dmac ? cause : 0xFFFFFFFFu;
    char line[128];
    const int w = std::snprintf(line, sizeof(line),
                                "irq vsync=%llu cause=0x%x ch=%u handler=0x%08x",
                                static_cast<unsigned long long>(vsync), cause, ch, handler);
    if (w > 0)
    {
        detail::emitLineLocked(s, line);
    }
}

// E40 Part-7: chain-builder catch. A whole-boot store watch on the ADDR
// words (tag+4) of the CALL tags seen pointing at 0x435bd0, armed from
// vsync 0 over the SRC window, first kMaxTagHitsPerWord hits per
// address (any value logged — the build is caught whichever uploader
// is chosen):
//   tagaddrwrite vsync=<n> addr=0x<> value=0x<> pc=0x<> ra=0x<> fn=<>
//     srcload=0x<loader addr>|none + a0..s7 (low 32 hex)
// `srcload` attributes the value to the most recent in-window guest
// load (any width, any lane) that returned an uploader-valued word
// (exact 0x435bd0 or the 0x434990-0x4349b8 alt range, folded with
// mirrors) — the table-entry copy. The word list is env-configurable
// via PS2X_MPG_SRC_TAGADDRS="a,b,c" (hex, up to kTagMaxWords);
// absent/malformed env keeps the defaults (the four 0x63b8-set ADDR
// words; no 0x7085 CALL offsets exist in any ctag data yet).
// (Sizes live with the arena constants above State.)
inline bool isTagWatched(uint32_t addr, uint32_t size)
{
    detail::State &s = detail::state();
    const int n = s.tagCount;
    for (int i = 0; i < n; ++i)
    {
        if (s.tagAddr[i] >= addr && s.tagAddr[i] < addr + size)
        {
            return true;
        }
    }
    return false;
}

// Fast gate for the per-store/per-load macro hooks: enabled AND at
// least one watched word still has hits left (set under lock).
inline bool tagaddrArmed()
{
    if (!enabled())
    {
        return false;
    }
    return detail::tagOpen().load(std::memory_order_relaxed) != 0u;
}

// Records uploader-valued load lanes for srcload attribution.
inline void noteLoadForSrcCtx(const PS2Runtime *runtime, const R5900Context *ctx,
                              uint32_t addr, uint32_t width,
                              uint64_t valueLo, uint64_t valueHi)
{
    if (!tagaddrArmed())
    {
        return;
    }
    uint32_t lanes = 1u;
    if (width >= 16u)
        lanes = 4u;
    else if (width >= 8u)
        lanes = 2u;
    uint32_t hitAddr[kSrcRing];
    uint32_t hitVal[kSrcRing];
    uint32_t hits = 0u;
    for (uint32_t i = 0u; i < lanes && hits < kSrcRing; ++i)
    {
        const uint32_t laneAddr = addr + i * 4u;
        uint32_t word;
        if (i == 0u)
            word = static_cast<uint32_t>(valueLo);
        else if (i == 1u)
            word = static_cast<uint32_t>(valueLo >> 32u);
        else if (i == 2u)
            word = static_cast<uint32_t>(valueHi);
        else
            word = static_cast<uint32_t>(valueHi >> 32u);
        if (!isUploaderValue(word))
        {
            continue;
        }
        hitAddr[hits] = laneAddr;
        hitVal[hits] = word;
        ++hits;
    }
    if (hits == 0u)
    {
        return;
    }
    uint64_t vsync = 0u;
    if (runtime != nullptr)
    {
        vsync = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
    }
    (void)ctx;
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
    for (uint32_t i = 0u; i < hits; ++i)
    {
        s.srcAddr[s.srcHead] = hitAddr[i];
        s.srcVal[s.srcHead] = hitVal[i];
        s.srcHead = (s.srcHead + 1) % kSrcRing;
    }
}

inline void noteTagaddrwrite(uint64_t vsync, uint32_t addr, uint32_t width,
                             uint64_t valueLo, uint64_t valueHi,
                             uint32_t pc, uint32_t ra, const char *fn,
                             const R5900Context *ctx)
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
    uint32_t lanes = 1u;
    if (width >= 16u)
        lanes = 4u;
    else if (width >= 8u)
        lanes = 2u;
    for (uint32_t i = 0u; i < lanes; ++i)
    {
        const uint32_t laneAddr = addr + i * 4u;
        uint32_t word;
        if (i == 0u)
            word = static_cast<uint32_t>(valueLo);
        else if (i == 1u)
            word = static_cast<uint32_t>(valueLo >> 32u);
        else if (i == 2u)
            word = static_cast<uint32_t>(valueHi);
        else
            word = static_cast<uint32_t>(valueHi >> 32u);
        int slot = -1;
        for (int t = 0; t < s.tagCount; ++t)
        {
            if (s.tagAddr[t] == laneAddr)
            {
                slot = t;
                break;
            }
        }
        if (slot < 0 || s.tagHits[slot] >= kMaxTagHitsPerWord)
        {
            continue;
        }
        // Most-recent in-window load that returned this exact value.
        uint32_t src = 0u;
        for (int r = 0; r < kSrcRing; ++r)
        {
            const int k = (s.srcHead + kSrcRing - 1 - r) % kSrcRing;
            if (s.srcVal[k] == word && s.srcAddr[k] != 0u)
            {
                src = s.srcAddr[k];
                break;
            }
        }
        char line[768];
        int w;
        if (src != 0u)
        {
            w = std::snprintf(line, sizeof(line),
                              "tagaddrwrite vsync=%llu addr=0x%08x value=0x%08x pc=0x%08x ra=0x%08x fn=%s srcload=0x%08x",
                              static_cast<unsigned long long>(vsync), laneAddr, word,
                              pc, ra, fn ? fn : "?", src);
        }
        else
        {
            w = std::snprintf(line, sizeof(line),
                              "tagaddrwrite vsync=%llu addr=0x%08x value=0x%08x pc=0x%08x ra=0x%08x fn=%s srcload=none",
                              static_cast<unsigned long long>(vsync), laneAddr, word,
                              pc, ra, fn ? fn : "?");
        }
        for (int r = 0; r < kReadRegCount && w > 0; ++r)
        {
            const uint32_t rv = (ctx != nullptr) ? getRegU32(ctx, kReadRegs[r]) : 0u;
            w += std::snprintf(line + w, sizeof(line) - static_cast<size_t>(w),
                               " %s=0x%08x", kReadRegNames[r], rv);
        }
        if (w > 0)
        {
            detail::emitLineLocked(s, line);
            ++s.tagHits[slot];
            if (s.capped)
            {
                return;
            }
        }
    }
    bool anyLeft = false;
    for (int t = 0; t < s.tagCount; ++t)
    {
        if (s.tagHits[t] < kMaxTagHitsPerWord)
        {
            anyLeft = true;
            break;
        }
    }
    if (!anyLeft)
    {
        detail::tagOpen().store(0u, std::memory_order_relaxed);
    }
}

// Macro glue for the WRITE32/64/128 taps.
inline void noteTagaddrwriteCtx(const PS2Runtime *runtime, const R5900Context *ctx,
                                uint32_t addr, uint32_t width,
                                uint64_t valueLo, uint64_t valueHi, const char *fn)
{
    uint64_t vsync = 0u;
    if (runtime != nullptr)
    {
        vsync = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
    }
    const uint32_t pc = (ctx != nullptr) ? ctx->pc : 0u;
    const uint32_t ra = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
    noteTagaddrwrite(vsync, addr, width, valueLo, valueHi, pc, ra, fn, ctx);
}

// E40 Part-4: chain-tag dump. The first kMaxCtagKicks in-window VIF1
// chain walks log every tag they walk:
//   ctag tag_at=0x<EE addr of tag> id=<0-7> qwc=<n> addr=0x<ref addr>
//     tte=<16 hex digits: tag bytes 8..16 in stream order>
inline constexpr uint64_t kMaxCtagKicks = 2ull;

// Called once per VIF1 chain walk: true when this walk's tags should log
// (first kicks inside the window; out-of-window walks consume nothing).
inline bool noteCtagKick(uint64_t vsync)
{
    if (!enabled())
    {
        return false;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || s.capped)
    {
        return false;
    }
    if (vsync < s.from || vsync > s.to)
    {
        return false;
    }
    if (s.ctagKicks >= kMaxCtagKicks)
    {
        return false;
    }
    ++s.ctagKicks;
    return true;
}

inline void noteCtag(uint64_t vsync, uint32_t tagAt, uint32_t id,
                     uint32_t qwc, uint32_t addr,
                     uint32_t tteWord0, uint32_t tteWord1)
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
    char line[192];
    std::snprintf(line, sizeof(line),
                  "ctag tag_at=0x%08x id=%u qwc=%u addr=0x%08x tte=%08x%08x",
                  tagAt, id, qwc, addr, tteWord0, tteWord1);
    detail::emitLineLocked(s, line);
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
    s.watches.clear();
    s.readHits[0] = 0u;
    s.readHits[1] = 0u;
    s.payInstalled = false;
    s.payBase = nullptr;
    s.payLen = 0u;
    s.payEeBase = 0u;
    s.payMode = PayNone;
    s.paySpans.clear();
    s.dmHits = 0u;
    s.arHits = 0u;
    s.ctagKicks = 0u;
    s.ulHits = 0u;
    detail::installArenasLocked(s, nullptr);
    detail::installTagAddrsLocked(s, nullptr);
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.enabled = true;
    detail::enabledFlag().store(true, std::memory_order_relaxed);
    detail::armedCount().store(0u, std::memory_order_relaxed);
    detail::readOpen().store(1u, std::memory_order_relaxed);
    detail::payInstalledFlag().store(0u, std::memory_order_relaxed);
    detail::dmOpen().store(1u, std::memory_order_relaxed);
    detail::arOpen().store(1u, std::memory_order_relaxed);
    detail::ulOpen().store(1u, std::memory_order_relaxed);
    detail::tagOpen().store(1u, std::memory_order_relaxed);
    return true;
}

// Test hook for the Part-5 env path: installs arenas from text (or
// defaults when text is null/empty/malformed), same code as initLocked.
inline void applyArenasForTest(const char *text)
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    detail::installArenasLocked(s, text);
}

// Test hook for the Part-7 env path: installs tagaddr words the same
// way initLocked does.
inline void applyTagAddrsForTest(const char *text)
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    detail::installTagAddrsLocked(s, text);
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
    s.watches.clear();
    s.readHits[0] = 0u;
    s.readHits[1] = 0u;
    s.payInstalled = false;
    s.payBase = nullptr;
    s.payLen = 0u;
    s.payEeBase = 0u;
    s.payMode = PayNone;
    s.paySpans.clear();
    s.dmHits = 0u;
    s.arHits = 0u;
    s.ctagKicks = 0u;
    s.ulHits = 0u;
    detail::installArenasLocked(s, nullptr);
    detail::installTagAddrsLocked(s, nullptr);
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.enabled = false;
    detail::enabledFlag().store(false, std::memory_order_relaxed);
    detail::armedCount().store(0u, std::memory_order_relaxed);
    detail::readOpen().store(1u, std::memory_order_relaxed);
    detail::payInstalledFlag().store(0u, std::memory_order_relaxed);
    detail::dmOpen().store(1u, std::memory_order_relaxed);
    detail::arOpen().store(1u, std::memory_order_relaxed);
    detail::ulOpen().store(1u, std::memory_order_relaxed);
    detail::tagOpen().store(1u, std::memory_order_relaxed);
}

} // namespace ps2_mpg_src_trace
