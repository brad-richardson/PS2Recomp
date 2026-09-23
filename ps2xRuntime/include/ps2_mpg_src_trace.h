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
// macros see every DMA-reg write).

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

inline constexpr uint64_t kMaxLines = 2000ull;
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
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.enabled = true;
    detail::enabledFlag().store(true, std::memory_order_relaxed);
    detail::armedCount().store(0u, std::memory_order_relaxed);
    detail::readOpen().store(1u, std::memory_order_relaxed);
    detail::payInstalledFlag().store(0u, std::memory_order_relaxed);
    detail::dmOpen().store(1u, std::memory_order_relaxed);
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
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.enabled = false;
    detail::enabledFlag().store(false, std::memory_order_relaxed);
    detail::armedCount().store(0u, std::memory_order_relaxed);
    detail::readOpen().store(1u, std::memory_order_relaxed);
    detail::payInstalledFlag().store(0u, std::memory_order_relaxed);
    detail::dmOpen().store(1u, std::memory_order_relaxed);
}

} // namespace ps2_mpg_src_trace
