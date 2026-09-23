// E41 DEV-ONLY CD-read + render-chain CALL plant trace behind
// PS2X_CD_READ_TRACE=<file>.
//
// Master gate: PS2X_CD_READ_TRACE names the text file receiving every line
// kind below. Unset/empty (default) = one relaxed atomic check per tap;
// zero guest-visible behavior change, no I/O.
// Optional binds: PS2X_CD_READ_TRACE_FROM / PS2X_CD_READ_TRACE_TO
// (inclusive guest-vsync window; default 0..2^64-1, vsync supplied by each
// call site from EeScheduler::currentVSyncTick, except EE-store plants
// which use the last VBlank tick noted via noteVsync()).
// Hard cap: 40000 lines total, then the file goes quiet.
//
// Line kinds (T52-compatible `cdread`/`cdsearch` plus plant attribution):
//   cdread seq=<n> vsync=<n> lbn=0x<x> sectors=<n> mode=<path>
//     dest=0x<EE addr> file=<host leaf|->
//     One per sector payload the runtime copies into EE RAM. mode names the
//     service path: sceCdRead, sceCdRead-unresolved (LBN not resolvable;
//     destination zero-filled), sceCdReadChain, sceCdStRead (streaming).
//     seq is a per-boot counter; plant lines cite it when attributable.
//     NOTE: this runtime maps registered files to pseudo-LBNs
//     (registerCdFile in Stubs/Helpers/Support.h) and reads unregistered
//     LBNs raw from the ISO image, so lbn is a pseudo-LBN for file-backed
//     reads and a raw ISO LBN otherwise; file= names the host leaf when
//     the LBN falls in a registered range.
//   cdsearch vsync=<n> name=<ps2 path> lbn=0x<x> size=<n>
//     One per successful sceCdSearchFile.
//   cdopen vsync=<n> name=<ps2 path> host=<host path> fd=<n>
//     One per successful ioman fioOpen.
//   fioread vsync=<n> fd=<n> buf=0x<EE addr> bytes=<n> name=<ps2 path|->
//     One per fioRead with bytes>0 (host-file data, no LBN by construction).
//   plant vsync=<n> addr=0x<canonical watched word> value=0x<word after write>
//     via=<path> src=<path-specific source> seq=<cdread seq|->
//     One per watched-word overlap by a non-macro EE-RAM writer, first 256
//     hits per word. Watched words (canonical = folded): 0x63B994,
//     0x63BBE4, 0x63BEA4, 0x63C134 (the four render-chain CALL ADDR words
//     from E40 Part-7). Both the watched words and the write address are
//     folded by & 0x0FFFFFFF, covering the 0x00/0x20/0x30/0x80 mirrors
//     (E40 Part-7's raw-compare gap closed here).
//
// Non-macro EE-RAM write paths tapped (each cites its via= tag):
//   ee-store ......... Ps2FastWrite8/16/32/64/128 in ps2_runtime_macros.h
//                      (single chokepoint for ALL EE CPU stores: the WRITE*
//                      macros route through FAST_WRITE*, and inlined
//                      constant-address FAST_WRITE* sequences in generated
//                      code land here too).
//   sceCdRead ........ CD.cpp tryRead (readCdSectors into rdram+offset).
//   sceCdRead-zero ... CD.cpp unresolved-LBN zero fill.
//   sceCdReadChain ... CD.cpp chain loop. sceCdStRead: continueCdStRead.
//   sceCdSearchFile .. CD.cpp writeCdSearchResult (sceCdlFILE struct).
//   cd-toc/clock/tray  CD.cpp GetToc/ReadClock/TrayReq fills.
//   sif-dma .......... SIF.cpp copyGuestByteRange (sceSifSetDma +
//                      sceSifGetOtherData; guest<->IOP-heap aware).
//   sif-recvdata ..... SIF.cpp GetOtherData SifRpcReceiveData_t update.
//   iop-write/zero ... ps2_iop_host.cpp IopHost writeGuest/zeroGuest.
//   libc-<op> ........ LibC.cpp memcpy/memset/memclr/memmove/strcpy/
//                      strncpy/sprintf/snprintf.
//   fio-read ......... Syscalls/FileIO.cpp fioRead (fread into EE buf).
//   sif-rpc .......... Syscalls/Helpers/Runtime.h rpcCopyToRdram/Zero.
//   syscall-copy ..... Syscalls/System.cpp Copy.
//   sys-kernel-word .. Syscalls/System.cpp writeGuestKernelWord.
//   gs-store-image ... Stubs/GS.cpp sceGsExecStoreImage.
//   heap-realloc ..... ps2_runtime.cpp guestRealloc memmove.
//                      (guestMalloc's memset plants only zeros: skipped.)
//   irq-handler-* .... ps2_runtime.cpp exception-handler installs.
//   dma-getenv ....... Stubs/DMA.cpp sceDmaGetEnv.
//   fio-fstat/stat/ioctl  Stubs/FileIO.cpp struct fills.
//   pad-<op> ......... Stubs/Pad.cpp PortOpen/Read DMA fills.
//   font-<op> ........ Stubs/Font.cpp glyph/kern copies + field stores.
//   vu-copy .......... Stubs/VU.cpp sceVu0CopyMatrix/Vector(V)/XYZ.
//   compat ........... Stubs/Compatibility.cpp char-out write.
//   mc-<op> .......... Stubs/MemoryCard.cpp string/dir-table fills.
// Ssx3Movie.cpp and MPEG.cpp perform no EE-RAM writes (guest reads only;
// packet memcpy targets host packets), so they carry no tap.

#pragma once

#include "runtime/ps2_memory.h" // PS2_RAM_SIZE / PS2_RAM_MASK

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2_e41_trace
{

inline constexpr uint64_t kMaxLines = 40000ull;
inline constexpr uint64_t kFlushEvery = 128ull;
inline constexpr uint64_t kMaxPlantHitsPerWord = 256ull;
inline constexpr size_t kMaxNameLen = 192u;

// Canonical (folded) watched words: the four render-chain CALL ADDR words.
inline constexpr uint32_t kPlantWord[4] = {0x0063B994u, 0x0063BBE4u, 0x0063BEA4u, 0x0063C134u};
inline constexpr int kPlantWords = 4;

// Folds the 0x00/0x20/0x30/0x80 EE-RAM mirrors onto one canonical address.
inline uint32_t foldAddr(uint32_t addr)
{
    return addr & 0x0FFFFFFFu;
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
        uint64_t seq = 0u; // cdread sequence counter
        uint64_t plantHits[kPlantWords] = {0u, 0u, 0u, 0u};
        std::unordered_map<int, std::string> fdNames; // fio fd -> ps2 path
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

    // Last VBlank tick noted via noteVsync(); the vsync source for
    // EE-store plants, which have no runtime/ctx at the tap.
    inline std::atomic<uint64_t> &lastVsync()
    {
        static std::atomic<uint64_t> v{0u};
        return v;
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
        const char *file = std::getenv("PS2X_CD_READ_TRACE");
        if (!file || file[0] == '\0')
        {
            return;
        }
        s.path = file;
        uint64_t from = 0u;
        uint64_t to = ~0ull;
        if (const char *env = std::getenv("PS2X_CD_READ_TRACE_FROM"))
        {
            if (!parseU64(env, from))
            {
                return;
            }
        }
        if (const char *env = std::getenv("PS2X_CD_READ_TRACE_TO"))
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

    // Copies at most kMaxNameLen bytes, mapping whitespace onto '_' so
    // every line stays one line and space/tab separated. '=' is kept:
    // src= values (lbn=0x.., src=0x.., fd=..) carry it by construction.
    // Always NUL-terminates.
    inline void sanitizeInto(const char *src, char *dst, size_t dstSize)
    {
        if (dstSize == 0u)
        {
            return;
        }
        size_t n = 0u;
        if (src != nullptr)
        {
            for (const char *p = src; *p != '\0' && n + 1u < dstSize && n < kMaxNameLen; ++p, ++n)
            {
                const char c = *p;
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                {
                    dst[n] = '_';
                }
                else
                {
                    dst[n] = c;
                }
            }
        }
        dst[n] = '\0';
    }

    inline void emitLocked(State &s, const char *line)
    {
        if (!s.enabled || s.capped)
        {
            return;
        }
        if (s.linesWritten >= kMaxLines)
        {
            s.capped = true;
            return;
        }
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
        ++s.linesWritten;
        if ((s.linesWritten % kFlushEvery) == 0u)
        {
            s.out.flush();
        }
    }

} // namespace detail

// Hot-path gate: one relaxed atomic load when disabled.
inline bool armed()
{
    return detail::enabledFlag().load(std::memory_order_relaxed);
}

inline bool plantArmed()
{
    return armed();
}

// VBlank tick mirror (call from EeScheduler::processEvent VBlankStart).
// Also lazily initializes from env on first tick.
inline void noteVsync(uint64_t tick)
{
    detail::ensureInit();
    detail::lastVsync().store(tick, std::memory_order_relaxed);
}

inline uint64_t lastVsyncTick()
{
    return detail::lastVsync().load(std::memory_order_relaxed);
}

// Logs one cdread line; returns the assigned sequence number (assigned
// even out-of-window so plant attribution stays gapless).
inline uint64_t noteCdRead(uint64_t vsync, uint32_t lbn, uint32_t sectors,
                           uint32_t destEe, const char *mode, const char *file)
{
    detail::ensureInit();
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
    {
        return 0u;
    }
    const uint64_t seq = ++s.seq;
    if (vsync < s.from || vsync > s.to)
    {
        return seq;
    }
    char fileBuf[kMaxNameLen + 1u];
    detail::sanitizeInto(file != nullptr ? file : "-", fileBuf, sizeof(fileBuf));
    char line[512];
    std::snprintf(line, sizeof(line),
                  "cdread seq=%llu vsync=%llu lbn=0x%x sectors=%u mode=%s dest=0x%08x file=%s",
                  static_cast<unsigned long long>(seq),
                  static_cast<unsigned long long>(vsync),
                  lbn, sectors, mode != nullptr ? mode : "-",
                  destEe, fileBuf[0] != '\0' ? fileBuf : "-");
    detail::emitLocked(s, line);
    return seq;
}

inline void noteCdSearch(uint64_t vsync, const char *name, uint32_t lbn, uint32_t sizeBytes)
{
    detail::ensureInit();
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || vsync < s.from || vsync > s.to)
    {
        return;
    }
    char nameBuf[kMaxNameLen + 1u];
    detail::sanitizeInto(name, nameBuf, sizeof(nameBuf));
    char line[512];
    std::snprintf(line, sizeof(line), "cdsearch vsync=%llu name=%s lbn=0x%x size=%u",
                  static_cast<unsigned long long>(vsync),
                  nameBuf[0] != '\0' ? nameBuf : "-",
                  lbn, sizeBytes);
    detail::emitLocked(s, line);
}

inline void noteFioOpen(uint64_t vsync, const char *ps2Path, const char *hostPath, int fd)
{
    detail::ensureInit();
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
    {
        return;
    }
    if (fd >= 0 && ps2Path != nullptr)
    {
        s.fdNames[fd] = ps2Path;
    }
    if (vsync < s.from || vsync > s.to)
    {
        return;
    }
    char nameBuf[kMaxNameLen + 1u];
    char hostBuf[kMaxNameLen + 1u];
    detail::sanitizeInto(ps2Path, nameBuf, sizeof(nameBuf));
    detail::sanitizeInto(hostPath, hostBuf, sizeof(hostBuf));
    char line[768];
    std::snprintf(line, sizeof(line), "cdopen vsync=%llu name=%s host=%s fd=%d",
                  static_cast<unsigned long long>(vsync),
                  nameBuf[0] != '\0' ? nameBuf : "-",
                  hostBuf[0] != '\0' ? hostBuf : "-",
                  fd);
    detail::emitLocked(s, line);
}

inline void noteFioClose(int fd)
{
    if (!armed() || fd < 0)
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.fdNames.erase(fd);
}

inline void noteFioRead(uint64_t vsync, int fd, uint32_t bufEe, uint64_t bytes)
{
    detail::ensureInit();
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || vsync < s.from || vsync > s.to)
    {
        return;
    }
    std::string name("-");
    if (fd >= 0)
    {
        const auto it = s.fdNames.find(fd);
        if (it != s.fdNames.end())
        {
            name = it->second;
        }
    }
    char nameBuf[kMaxNameLen + 1u];
    detail::sanitizeInto(name.c_str(), nameBuf, sizeof(nameBuf));
    char line[512];
    std::snprintf(line, sizeof(line), "fioread vsync=%llu fd=%d buf=0x%08x bytes=%llu name=%s",
                  static_cast<unsigned long long>(vsync),
                  fd, bufEe,
                  static_cast<unsigned long long>(bytes),
                  nameBuf[0] != '\0' ? nameBuf : "-");
    detail::emitLocked(s, line);
}

// Host-side write watch: logs one plant line per watched word overlapped
// by [dstAddr, dstAddr+size). Call AFTER the bytes are visible in rdram.
// src cites the path-specific source (e.g. "lbn=0x<x>+0x<off>",
// "src=0x<EE/IOP addr>", "fd=<n>"); seq is the attributing cdread seq, or
// 0 for unattributable (printed as '-').
inline void notePlantRange(uint64_t vsync, uint32_t dstAddr, uint64_t size,
                           const uint8_t *rdram, const char *via,
                           const char *src, uint64_t seq)
{
    if (size == 0u || !plantArmed())
    {
        return;
    }
    const uint32_t f = foldAddr(dstAddr);
    const uint64_t fEnd = static_cast<uint64_t>(f) + size;
    int hit = -1;
    for (int i = 0; i < kPlantWords; ++i)
    {
        const uint64_t w = kPlantWord[i];
        if (w < fEnd && static_cast<uint64_t>(f) < w + 4u)
        {
            hit = i;
            break;
        }
    }
    if (hit < 0)
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || vsync < s.from || vsync > s.to)
    {
        return;
    }
    // Re-scan under lock so multi-word overlaps log every word (still one
    // line per word, each with its own cap).
    for (int i = 0; i < kPlantWords; ++i)
    {
        const uint64_t w = kPlantWord[i];
        if (!(w < fEnd && static_cast<uint64_t>(f) < w + 4u))
        {
            continue;
        }
        if (s.plantHits[i] >= kMaxPlantHitsPerWord)
        {
            continue;
        }
        const uint32_t off = kPlantWord[i] & PS2_RAM_MASK;
        if (rdram == nullptr || off + 4u > PS2_RAM_SIZE)
        {
            continue;
        }
        uint32_t value = 0u;
        std::memcpy(&value, rdram + off, sizeof(value));
        ++s.plantHits[i];
        char srcBuf[128];
        detail::sanitizeInto(src, srcBuf, sizeof(srcBuf));
        char seqBuf[32];
        if (seq == 0u)
        {
            std::snprintf(seqBuf, sizeof(seqBuf), "-");
        }
        else
        {
            std::snprintf(seqBuf, sizeof(seqBuf), "%llu", static_cast<unsigned long long>(seq));
        }
        char line[512];
        std::snprintf(line, sizeof(line), "plant vsync=%llu addr=0x%08x value=0x%08x via=%s src=%s seq=%s",
                      static_cast<unsigned long long>(vsync),
                      kPlantWord[i], value,
                      via != nullptr ? via : "-",
                      srcBuf[0] != '\0' ? srcBuf : "-",
                      seqBuf);
        detail::emitLocked(s, line);
    }
}

// EE CPU store tap (call from Ps2FastWrite* AFTER the write). vsync comes
// from the VBlank mirror; unattributable by construction.
inline void noteFastWrite(const uint8_t *rdram, uint32_t addr, uint32_t size)
{
    if (!plantArmed())
    {
        return;
    }
    notePlantRange(lastVsyncTick(), addr, size, rdram, "ee-store", "-", 0u);
}

// Test hooks (mirror ps2_mpg_src_trace.h conventions).
inline void configureForTest(const char *path, uint64_t from, uint64_t to)
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.path = path != nullptr ? path : "";
    s.from = from;
    s.to = to;
    s.enabled = !s.path.empty();
    detail::enabledFlag().store(s.enabled, std::memory_order_relaxed);
    s.outOpen = false;
    s.capped = false;
    s.linesWritten = 0u;
    s.seq = 0u;
    for (int i = 0; i < kPlantWords; ++i)
    {
        s.plantHits[i] = 0u;
    }
    s.fdNames.clear();
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
    s.from = 0u;
    s.to = ~0ull;
    s.capped = false;
    s.linesWritten = 0u;
    s.seq = 0u;
    for (int i = 0; i < kPlantWords; ++i)
    {
        s.plantHits[i] = 0u;
    }
    s.fdNames.clear();
    detail::lastVsync().store(0u, std::memory_order_relaxed);
}

} // namespace ps2_e41_trace
