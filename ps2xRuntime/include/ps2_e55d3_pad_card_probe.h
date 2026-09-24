// E55D3 DEV-ONLY pad/card guest-write tap behind PS2X_PAD_CARD_PROBE=<file>.
//
// Master gate: PS2X_PAD_CARD_PROBE names the text file receiving every line.
// Unset/empty (default) = one relaxed-atomic check per tap call site; zero
// guest-visible behavior change, no file I/O, no payload scan.
//
// Coverage (called AFTER the bytes are visible in guest RDRAM; the tap never
// writes guest memory and never changes stub results):
//   notePad    one line per scePadRead call: ok=1 carries the full 32-byte
//              fill as hex; ok=0 carries a reason and no bytes.
//   noteGetDir one line per sceMcGetDir call: copied=1 carries the full
//              entryCount*64 table bytes as hex; otherwise a reason.
//   noteGetDirPath one sibling line per sceMcGetDir call: the raw guest
//              query and its normalized query/pattern plus host directory.
//              Bad-port/unformatted exits carry "-" for the unresolved
//              query/parent/pattern/host fields.
//   noteMcRead one line per sceMcRead call: ok=1 with actual>0 carries the
//              full payload as hex; otherwise a reason.
// A shared monotonic seq orders all three families; each family also carries
// its own per-family ordinal (ord), incremented for every call of that
// family including skip/fail lines, so counts distinguish calls that wrote
// no bytes without claiming bytes.
//
// Line format (one line per record; hex carries no whitespace, so framing is
// unambiguous; every line ends with '\n'):
//   pad seq=<s> vsync=<t> ord=<padOrd> port=<p> slot=<sl> addr=0x<8x>
//     len=32 ok=1 bytes=<64 hex>
//   pad seq=<s> vsync=<t> ord=<padOrd> port=<p> slot=<sl> addr=0x<8x>
//     len=0 ok=0 reason=<bad-addr|closed>
//   getdir seq=<s> vsync=<t> ord=<dirOrd> port=<p> slot=<sl> addr=0x<8x>
//     entries=<n> len=<n64> ok=1 bytes=<hex>
//   getdir seq=<s> ... entries=<n> len=0 ok=0 reason=<bad-port|unformatted|
//     no-dir|empty|bad-addr>
//   getdirpath seq=<s> vsync=<t> pord=<pathOrd> port=<p> slot=<sl> max=<m>
//     rawLen=<r> raw="<esc>" query="<esc>" parent="<esc>" pattern="<esc>"
//     host="<esc>" (each escaped field capped at 1024 chars; unknown
//     query/parent/pattern/host on bad-port/unformatted is "-")
//   mcread seq=<s> vsync=<t> ord=<readOrd> fd=<f> addr=0x<8x>
//     req=<q> len=<a> ok=1 err=<-|io-error> bytes=<hex>
//     (err records a stream error observed alongside a positive payload;
//     the payload is still complete guest-written bytes)
//   mcread seq=<s> ... req=<q> len=<a> ok=0 reason=<zero-size|bad-fd|
//     bad-addr|eof|io-error>
//   cap seq=<s> vsync=<t> bytes=<written> msg=byte-cap-reached
//
// Hard cap: 16 MiB of file bytes total. Before writing a record, its full
// size is computed; if it would exceed the cap (minus a reserve for the cap
// line itself), exactly one cap line is written instead and the tap disables
// itself permanently for the process. No truncated payload record is ever
// written. Total file size never exceeds 16 MiB.
//
// Concurrency: one process-local log; seq/ordinals/file writes are protected
// by a single mutex (leaf lock: never acquires any stub mutex).
//
// Test hooks mirror ps2_e41_trace.h conventions. configureForTest takes an
// explicit byte cap so tests can exercise the cap path with a tiny budget.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

namespace ps2_e55d3_probe
{

inline constexpr uint64_t kByteCap = 16ull * 1024ull * 1024ull; // 16 MiB
inline constexpr uint64_t kFlushEvery = 16ull;
inline constexpr size_t kCapReserve = 256u; // worst-case cap-line bytes
inline constexpr size_t kPathFieldMax = 1024u; // per-field escaped cap

namespace detail
{

    struct State
    {
        std::mutex mutex;
        bool initDone = false;
        bool enabled = false;
        std::string path;
        uint64_t byteCap = kByteCap;
        std::ofstream out;
        bool outOpen = false;
        bool capped = false;
        uint64_t bytesWritten = 0u;
        uint64_t linesWritten = 0u;
        uint64_t seq = 0u;     // shared across pad/getdir/mcread/cap
        uint64_t padOrd = 0u;  // per-family ordinals (every call counts)
        uint64_t dirOrd = 0u;
        uint64_t readOrd = 0u;
        uint64_t pathOrd = 0u; // getdirpath sibling ordinal (every GetDir counts)
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

    inline void initLocked(State &s)
    {
        s.initDone = true;
        const char *file = std::getenv("PS2X_PAD_CARD_PROBE");
        if (!file || file[0] == '\0')
        {
            return;
        }
        s.path = file;
        s.byteCap = kByteCap;
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

    inline void emitRecordLocked(State &s, const char *header, const uint8_t *bytes, size_t len,
                                 uint64_t vsync)
    {
        if (!s.enabled || s.capped)
        {
            return;
        }
        const size_t headerLen = std::strlen(header);
        const uint64_t recordBytes = static_cast<uint64_t>(headerLen) +
                                     static_cast<uint64_t>(len) * 2u + 1u; // + '\n'
        if (s.bytesWritten + recordBytes > s.byteCap - kCapReserve)
        {
            const uint64_t seq = ++s.seq;
            char cap[192];
            const int n = std::snprintf(cap, sizeof(cap),
                                        "cap seq=%llu vsync=%llu bytes=%llu msg=byte-cap-reached\n",
                                        static_cast<unsigned long long>(seq),
                                        static_cast<unsigned long long>(vsync),
                                        static_cast<unsigned long long>(s.bytesWritten));
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
            s.out << cap;
            s.out.flush();
            if (n > 0)
            {
                s.bytesWritten += static_cast<uint64_t>(n);
            }
            ++s.linesWritten;
            s.capped = true;
            s.enabled = false;
            enabledFlag().store(false, std::memory_order_relaxed);
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
        s.out << header;
        static constexpr char kHex[] = "0123456789abcdef";
        std::string hex;
        hex.resize(len * 2u);
        for (size_t i = 0u; i < len; ++i)
        {
            hex[2u * i] = kHex[(bytes[i] >> 4) & 0xFu];
            hex[2u * i + 1u] = kHex[bytes[i] & 0xFu];
        }
        s.out << hex << '\n';
        s.bytesWritten += recordBytes;
        ++s.linesWritten;
        if ((s.linesWritten % kFlushEvery) == 0u)
        {
            s.out.flush();
        }
    }

} // namespace detail

// Hot-path gate: lazy env init once (one relaxed atomic after the first
// call), then one relaxed atomic load per call when disabled.
inline bool armed()
{
    detail::ensureInit();
    return detail::enabledFlag().load(std::memory_order_relaxed);
}

// Post-fill scePadRead tap. Call after the 32-byte fill is visible; when ok
// is false (or bytes is null) only a status line is written. bytes must
// point at 32 valid RDRAM bytes iff ok.
inline void notePad(uint64_t vsync, int port, int slot, uint32_t addr,
                    bool ok, const char *reason, const uint8_t *bytes)
{
    if (!armed())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || s.capped)
    {
        return;
    }
    const uint64_t seq = ++s.seq;
    const uint64_t ord = ++s.padOrd;
    char header[256];
    if (ok && bytes != nullptr)
    {
        std::snprintf(header, sizeof(header),
                      "pad seq=%llu vsync=%llu ord=%llu port=%d slot=%d addr=0x%08x len=32 ok=1 bytes=",
                      static_cast<unsigned long long>(seq),
                      static_cast<unsigned long long>(vsync),
                      static_cast<unsigned long long>(ord),
                      port, slot, addr);
        detail::emitRecordLocked(s, header, bytes, 32u, vsync);
    }
    else
    {
        std::snprintf(header, sizeof(header),
                      "pad seq=%llu vsync=%llu ord=%llu port=%d slot=%d addr=0x%08x len=0 ok=0 reason=%s bytes=\n",
                      static_cast<unsigned long long>(seq),
                      static_cast<unsigned long long>(vsync),
                      static_cast<unsigned long long>(ord),
                      port, slot, addr, reason != nullptr ? reason : "-");
        const size_t headerLen = std::strlen(header);
        const uint64_t recordBytes = static_cast<uint64_t>(headerLen);
        if (s.bytesWritten + recordBytes > s.byteCap - kCapReserve)
        {
            detail::emitRecordLocked(s, "", nullptr, 0u, vsync);
            return;
        }
        if (!s.outOpen)
        {
            s.out.open(s.path, std::ios::out | std::ios::trunc);
            if (!s.out.is_open())
            {
                s.enabled = false;
                detail::enabledFlag().store(false, std::memory_order_relaxed);
                return;
            }
            s.outOpen = true;
        }
        s.out << header;
        s.bytesWritten += recordBytes;
        ++s.linesWritten;
        if ((s.linesWritten % kFlushEvery) == 0u)
        {
            s.out.flush();
        }
    }
}

// sceMcGetDir tap. Call after the table copy (or its skip/fail decision).
// bytes must point at entryCount*64 valid RDRAM bytes iff copied.
inline void noteGetDir(uint64_t vsync, int port, int slot, uint32_t tableAddr,
                       uint64_t entryCount, int32_t maxEntries, bool copied,
                       const char *reason, const uint8_t *bytes)
{
    if (!armed())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || s.capped)
    {
        return;
    }
    const uint64_t seq = ++s.seq;
    const uint64_t ord = ++s.dirOrd;
    const uint64_t len = copied ? entryCount * 64u : 0u;
    char header[256];
    if (copied && bytes != nullptr)
    {
        std::snprintf(header, sizeof(header),
                      "getdir seq=%llu vsync=%llu ord=%llu port=%d slot=%d addr=0x%08x entries=%llu max=%d len=%llu ok=1 bytes=",
                      static_cast<unsigned long long>(seq),
                      static_cast<unsigned long long>(vsync),
                      static_cast<unsigned long long>(ord),
                      port, slot, tableAddr,
                      static_cast<unsigned long long>(entryCount), maxEntries,
                      static_cast<unsigned long long>(len));
        detail::emitRecordLocked(s, header, bytes, static_cast<size_t>(len), vsync);
    }
    else
    {
        std::snprintf(header, sizeof(header),
                      "getdir seq=%llu vsync=%llu ord=%llu port=%d slot=%d addr=0x%08x entries=%llu max=%d len=0 ok=0 reason=%s bytes=\n",
                      static_cast<unsigned long long>(seq),
                      static_cast<unsigned long long>(vsync),
                      static_cast<unsigned long long>(ord),
                      port, slot, tableAddr,
                      static_cast<unsigned long long>(entryCount), maxEntries,
                      reason != nullptr ? reason : "-");
        const uint64_t recordBytes = static_cast<uint64_t>(std::strlen(header));
        if (s.bytesWritten + recordBytes > s.byteCap - kCapReserve)
        {
            detail::emitRecordLocked(s, "", nullptr, 0u, vsync);
            return;
        }
        if (!s.outOpen)
        {
            s.out.open(s.path, std::ios::out | std::ios::trunc);
            if (!s.out.is_open())
            {
                s.enabled = false;
                detail::enabledFlag().store(false, std::memory_order_relaxed);
                return;
            }
            s.outOpen = true;
        }
        s.out << header;
        s.bytesWritten += recordBytes;
        ++s.linesWritten;
        if ((s.linesWritten % kFlushEvery) == 0u)
        {
            s.out.flush();
        }
    }
}

// sceMcGetDir path tap. Sibling line carrying the raw guest query and its
// normalized query/parent/pattern plus the resolved host directory. Read-only:
// captures std::strings, never touches RDRAM, return values or results.
// Unknown query/parent/pattern/host on bad-port/unformatted exits must be
// passed as "-" by the caller (no normalization is moved into those branches).
inline void noteGetDirPath(uint64_t vsync, int port, int slot, int32_t maxEntries,
                           const std::string &rawPath, const std::string &query,
                           const std::string &parentRel, const std::string &pattern,
                           const std::string &hostDir)
{
    if (!armed())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || s.capped)
    {
        return;
    }
    const uint64_t seq = ++s.seq;
    const uint64_t pord = ++s.pathOrd;
    const uint64_t rawLen = static_cast<uint64_t>(rawPath.size());
    auto escapeField = [](const std::string &in, std::string &out)
    {
        out.clear();
        out.reserve(in.size());
        static constexpr char kHex[] = "0123456789abcdef";
        for (size_t i = 0u; i < in.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(in[i]);
            if (c == static_cast<unsigned char>('\\'))
            {
                if (out.size() + 2u > kPathFieldMax)
                {
                    break;
                }
                out += "\\\\";
            }
            else if (c == static_cast<unsigned char>('"'))
            {
                if (out.size() + 2u > kPathFieldMax)
                {
                    break;
                }
                out += "\\\"";
            }
            else if (c >= 0x20u && c <= 0x7Eu)
            {
                if (out.size() + 1u > kPathFieldMax)
                {
                    break;
                }
                out.push_back(static_cast<char>(c));
            }
            else
            {
                if (out.size() + 4u > kPathFieldMax)
                {
                    break;
                }
                out.push_back('\\');
                out.push_back('x');
                out.push_back(kHex[(c >> 4) & 0xFu]);
                out.push_back(kHex[c & 0xFu]);
            }
        }
    };
    std::string eRaw, eQuery, eParent, ePattern, eHost;
    escapeField(rawPath, eRaw);
    escapeField(query, eQuery);
    escapeField(parentRel, eParent);
    escapeField(pattern, ePattern);
    escapeField(hostDir, eHost);
    std::string line;
    line.reserve(256u + eRaw.size() + eQuery.size() + eParent.size() + ePattern.size() + eHost.size());
    char head[256];
    std::snprintf(head, sizeof(head),
                  "getdirpath seq=%llu vsync=%llu pord=%llu port=%d slot=%d max=%d rawLen=%llu raw=\"",
                  static_cast<unsigned long long>(seq),
                  static_cast<unsigned long long>(vsync),
                  static_cast<unsigned long long>(pord),
                  port, slot, maxEntries,
                  static_cast<unsigned long long>(rawLen));
    line += head;
    line += eRaw;
    line += "\" query=\"";
    line += eQuery;
    line += "\" parent=\"";
    line += eParent;
    line += "\" pattern=\"";
    line += ePattern;
    line += "\" host=\"";
    line += eHost;
    line += "\"\n";
    const uint64_t recordBytes = static_cast<uint64_t>(line.size());
    if (s.bytesWritten + recordBytes > s.byteCap - kCapReserve)
    {
        // Whole-or-nothing: emit the single cap line directly (delegating to
        // emitRecordLocked with an empty header would write a blank line here
        // instead of the cap, since its 1-byte record still fits).
        const uint64_t capSeq = ++s.seq;
        char cap[192];
        const int n = std::snprintf(cap, sizeof(cap),
                                    "cap seq=%llu vsync=%llu bytes=%llu msg=byte-cap-reached\n",
                                    static_cast<unsigned long long>(capSeq),
                                    static_cast<unsigned long long>(vsync),
                                    static_cast<unsigned long long>(s.bytesWritten));
        if (!s.outOpen)
        {
            s.out.open(s.path, std::ios::out | std::ios::trunc);
            if (!s.out.is_open())
            {
                s.enabled = false;
                detail::enabledFlag().store(false, std::memory_order_relaxed);
                return;
            }
            s.outOpen = true;
        }
        s.out << cap;
        s.out.flush();
        if (n > 0)
        {
            s.bytesWritten += static_cast<uint64_t>(n);
        }
        ++s.linesWritten;
        s.capped = true;
        s.enabled = false;
        detail::enabledFlag().store(false, std::memory_order_relaxed);
        return;
    }
    if (!s.outOpen)
    {
        s.out.open(s.path, std::ios::out | std::ios::trunc);
        if (!s.out.is_open())
        {
            s.enabled = false;
            detail::enabledFlag().store(false, std::memory_order_relaxed);
            return;
        }
        s.outOpen = true;
    }
    s.out << line;
    s.bytesWritten += recordBytes;
    ++s.linesWritten;
    if ((s.linesWritten % kFlushEvery) == 0u)
    {
        s.out.flush();
    }
}

// sceMcRead tap. Call after the fread (or its skip/fail decision). bytes
// must point at actual valid RDRAM bytes iff ok && actual > 0. reason on an
// ok line is an accompanying stream note (io-error) printed as err; on a
// non-ok line it is the skip/fail reason.
inline void noteMcRead(uint64_t vsync, int fd, uint32_t dstAddr, int32_t requested,
                       uint64_t actual, bool ok, const char *reason, const uint8_t *bytes)
{
    if (!armed())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || s.capped)
    {
        return;
    }
    const uint64_t seq = ++s.seq;
    const uint64_t ord = ++s.readOrd;
    char header[256];
    if (ok && actual > 0u && bytes != nullptr)
    {
        std::snprintf(header, sizeof(header),
                      "mcread seq=%llu vsync=%llu ord=%llu fd=%d addr=0x%08x req=%d len=%llu ok=1 err=%s bytes=",
                      static_cast<unsigned long long>(seq),
                      static_cast<unsigned long long>(vsync),
                      static_cast<unsigned long long>(ord),
                      fd, dstAddr, requested,
                      static_cast<unsigned long long>(actual),
                      reason != nullptr ? reason : "-");
        detail::emitRecordLocked(s, header, bytes, static_cast<size_t>(actual), vsync);
    }
    else
    {
        std::snprintf(header, sizeof(header),
                      "mcread seq=%llu vsync=%llu ord=%llu fd=%d addr=0x%08x req=%d len=%llu ok=0 reason=%s bytes=\n",
                      static_cast<unsigned long long>(seq),
                      static_cast<unsigned long long>(vsync),
                      static_cast<unsigned long long>(ord),
                      fd, dstAddr, requested,
                      static_cast<unsigned long long>(actual),
                      reason != nullptr ? reason : "-");
        const uint64_t recordBytes = static_cast<uint64_t>(std::strlen(header));
        if (s.bytesWritten + recordBytes > s.byteCap - kCapReserve)
        {
            detail::emitRecordLocked(s, "", nullptr, 0u, vsync);
            return;
        }
        if (!s.outOpen)
        {
            s.out.open(s.path, std::ios::out | std::ios::trunc);
            if (!s.out.is_open())
            {
                s.enabled = false;
                detail::enabledFlag().store(false, std::memory_order_relaxed);
                return;
            }
            s.outOpen = true;
        }
        s.out << header;
        s.bytesWritten += recordBytes;
        ++s.linesWritten;
        if ((s.linesWritten % kFlushEvery) == 0u)
        {
            s.out.flush();
        }
    }
}

// Test hooks (mirror ps2_e41_trace.h conventions).
inline void configureForTest(const char *path, uint64_t byteCap)
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.path = path != nullptr ? path : "";
    s.byteCap = (byteCap == 0u) ? kByteCap : byteCap;
    s.enabled = !s.path.empty();
    detail::enabledFlag().store(s.enabled, std::memory_order_relaxed);
    s.outOpen = false;
    s.capped = false;
    s.bytesWritten = 0u;
    s.linesWritten = 0u;
    s.seq = 0u;
    s.padOrd = 0u;
    s.dirOrd = 0u;
    s.readOrd = 0u;
    s.pathOrd = 0u;
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
    s.byteCap = kByteCap;
    s.capped = false;
    s.bytesWritten = 0u;
    s.linesWritten = 0u;
    s.seq = 0u;
    s.padOrd = 0u;
    s.dirOrd = 0u;
    s.readOrd = 0u;
    s.pathOrd = 0u;
}

} // namespace ps2_e55d3_probe
