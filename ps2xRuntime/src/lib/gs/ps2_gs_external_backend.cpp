// GE2: external-GS backend shell + recording stub. See the header for the
// boundary contract. Single-threaded: every backend call runs on the GS
// worker (the runtime forces the queue on, like PS2X_GS_BACKEND=parallel);
// only stats()/lastPresent() take the mutex (cross-thread readers).

#include "runtime/gs/ps2_gs_external_backend.h"

#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace
{
constexpr uint64_t kLogCapBytes = 1ull * 1024ull * 1024ull * 1024ull;
constexpr char kSaveMagic[8] = {'P', 'S', '2', 'X', 'E', 'G', 'S', '1'};
constexpr uint32_t kSaveVersion = 1u;
// GE2 mirror set: same 19 offsets the frontend diffs (gs_frontend.cpp).
constexpr uint32_t kMirrorOffsets[19] = {
    0x0000u, 0x0010u, 0x0020u, 0x0030u, 0x0040u, 0x0050u, 0x0060u,
    0x0070u, 0x0080u, 0x0090u, 0x00A0u, 0x00B0u, 0x00C0u, 0x00D0u,
    0x00E0u, 0x1000u, 0x1010u, 0x1040u, 0x1080u};

uint32_t fnv1a32(const uint8_t *data, size_t size, uint32_t hash = 2166136261u)
{
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

void putU32(std::vector<uint8_t> &out, uint32_t v)
{
    const size_t at = out.size();
    out.resize(at + 4u);
    std::memcpy(out.data() + at, &v, 4u);
}

void putU64(std::vector<uint8_t> &out, uint64_t v)
{
    const size_t at = out.size();
    out.resize(at + 8u);
    std::memcpy(out.data() + at, &v, 8u);
}

bool takeU32(const uint8_t *&p, const uint8_t *end, uint32_t &v)
{
    if (static_cast<size_t>(end - p) < 4u)
        return false;
    std::memcpy(&v, p, 4u);
    p += 4u;
    return true;
}

bool takeU64(const uint8_t *&p, const uint8_t *end, uint64_t &v)
{
    if (static_cast<size_t>(end - p) < 8u)
        return false;
    std::memcpy(&v, p, 8u);
    p += 8u;
    return true;
}

class ExternalGsBackend final : public GSRasterBackend
{
public:
    explicit ExternalGsBackend(const GSRegisters *priv) : m_priv(priv) {}

    ~ExternalGsBackend() override
    {
        {
            std::lock_guard<std::mutex> lock(s_apiMutex);
            if (s_live == this)
                s_live = nullptr;
        }
        if (m_log)
        {
            std::fprintf(m_log, "# end gif=%llu npack=%llu reg=%llu priv=%llu vsync=%llu gaps=%llu "
                                "xfer=%llu up=%llu con=%llu dl=%llu pres=%llu save=%llu load=%llu\n",
                         (unsigned long long)m_stats.gifPackets, (unsigned long long)m_stats.nativePacked,
                         (unsigned long long)m_stats.regWrites, (unsigned long long)m_stats.privMirrored,
                         (unsigned long long)m_stats.vsyncs, (unsigned long long)m_stats.vsyncGaps,
                         (unsigned long long)m_stats.transfers, (unsigned long long)m_stats.uploads,
                         (unsigned long long)m_stats.consumes, (unsigned long long)m_stats.fifoDownloads,
                         (unsigned long long)m_stats.presents, (unsigned long long)m_stats.saves,
                         (unsigned long long)m_stats.loads);
            std::fclose(m_log);
            m_log = nullptr;
        }
        std::fprintf(stderr,
                     "[gs:external] stub done gif=%llu (p1=%llu p2=%llu p3=%llu) npack=%llu reg=%llu priv=%llu "
                     "vsync=%llu gaps=%llu xfer=%llu up=%llu con=%llu dl=%llu pres=%llu log=%s\n",
                     (unsigned long long)m_stats.gifPackets,
                     (unsigned long long)m_stats.gifPacketsByPath[1],
                     (unsigned long long)m_stats.gifPacketsByPath[2],
                     (unsigned long long)m_stats.gifPacketsByPath[3],
                     (unsigned long long)m_stats.nativePacked, (unsigned long long)m_stats.regWrites,
                     (unsigned long long)m_stats.privMirrored, (unsigned long long)m_stats.vsyncs,
                     (unsigned long long)m_stats.vsyncGaps, (unsigned long long)m_stats.transfers,
                     (unsigned long long)m_stats.uploads, (unsigned long long)m_stats.consumes,
                     (unsigned long long)m_stats.fifoDownloads, (unsigned long long)m_stats.presents,
                     m_stats.logOpen ? "open" : (m_stats.logTruncated ? "truncated" : "off"));
    }

    void Initialize(uint8_t *vram, uint32_t vramSize) override
    {
        m_inner = std::make_unique<GSCpuBackend>();
        m_inner->Initialize(vram, vramSize);
        {
            std::lock_guard<std::mutex> lock(s_apiMutex);
            s_live = this;
        }
        if (const char *path = std::getenv("PS2X_GS_EXTERNAL_LOG"))
        {
            if (path[0] != '\0')
            {
                m_log = std::fopen(path, "w");
                if (!m_log)
                    std::fprintf(stderr, "[gs:external] log open failed path=%s; counting only\n", path);
                else
                {
                    m_stats.logOpen = true;
                    std::fprintf(m_log, "# ps2x-external-gs-log v1\n");
                }
            }
        }
        log("# init vram=%u\n", vramSize);
    }

    void Reset() override
    {
        m_fifo.clear();
        m_fifoCursor = 0u;
        m_fifoValid = false;
        if (m_inner)
            m_inner->Reset();
        logLine("# reset\n");
    }

    void Submit(const GSPrimitiveBatch &batch) override
    {
        // Raw path owns draws (RV14: never submit decoded geometry twice);
        // the stub still delegates so the guest stays CPU-identical.
        ++m_stats.submitsIgnored;
        if (m_inner)
            m_inner->Submit(batch);
    }

    void BeginTransfer(const GSTransferCommand &command) override
    {
        ++m_stats.transfers;
        const uint64_t bitbltbuf = static_cast<uint64_t>(command.bitbltbuf.sbp) |
                                   (static_cast<uint64_t>(command.bitbltbuf.sbw) << 16) |
                                   (static_cast<uint64_t>(command.bitbltbuf.spsm) << 24) |
                                   (static_cast<uint64_t>(command.bitbltbuf.dbp) << 32) |
                                   (static_cast<uint64_t>(command.bitbltbuf.dbw) << 48) |
                                   (static_cast<uint64_t>(command.bitbltbuf.dpsm) << 56);
        const uint64_t trxpos = static_cast<uint64_t>(command.trxpos.ssax) |
                                (static_cast<uint64_t>(command.trxpos.ssay) << 16) |
                                (static_cast<uint64_t>(command.trxpos.dsax) << 32) |
                                (static_cast<uint64_t>(command.trxpos.dsay) << 48) |
                                (static_cast<uint64_t>(command.trxpos.dir) << 59);
        const uint64_t trxreg = static_cast<uint64_t>(command.trxreg.rrw) |
                                (static_cast<uint64_t>(command.trxreg.rrh) << 32);
        log("T dir=%u bb=%016llx tp=%016llx tr=%016llx\n", command.direction,
            (unsigned long long)bitbltbuf, (unsigned long long)trxpos, (unsigned long long)trxreg);
        if (command.direction == 1u)
        {
            // New local->host setup replaces the adapter FIFO (RV14: never
            // return fabricated bytes; unread leftovers are counted, lost).
            if (m_fifoValid && m_fifoCursor < m_fifo.size())
                m_stats.fifoReplacedUnread += m_fifo.size() - m_fifoCursor;
            m_fifo.clear();
            m_fifoCursor = 0u;
            m_fifoValid = false;
        }
        if (m_inner)
            m_inner->BeginTransfer(command);
    }

    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
    {
        ++m_stats.uploads;
        m_stats.uploadBytes += sizeBytes;
        log("U size=%u crc=%08x tick=%llu\n", sizeBytes,
            data && sizeBytes ? fnv1a32(data, sizeBytes) : 0u, tickNow());
        if (m_inner)
            m_inner->UploadImage(data, sizeBytes);
    }

    void Flush() override
    {
        if (m_inner)
            m_inner->Flush();
    }

    void TextureFlush() override
    {
        if (m_inner)
            m_inner->TextureFlush();
    }

    void Sync(GSSyncReason reason) override
    {
        if (m_inner)
            m_inner->Sync(reason);
    }

    PresentationFrame Present(const GSPresentationRequest &request) override
    {
        PresentationFrame frame;
        if (m_inner)
            frame = m_inner->Present(request);
        ++m_stats.presents;
        const uint32_t hash =
            !frame.pixels.empty() ? fnv1a32(frame.pixels.data(), frame.pixels.size()) : 0u;
        {
            std::lock_guard<std::mutex> lock(m_apiMutex);
            m_exported.tick = request.vsyncTick;
            m_exported.width = frame.width;
            m_exported.height = frame.height;
            m_exported.displayFbp = frame.displayFbp;
            m_exported.sourceFbp = frame.sourceFbp;
            m_exported.usedPreferred = frame.usedPreferred;
            m_exported.hash = hash;
            m_exported.pixels = frame.pixels; // stub-only copy; the real sink reads the device texture
            m_hasExported = true;
        }
        log("F tick=%llu %ux%u fbp=%u src=%u pref=%u hash=%08x\n",
            (unsigned long long)request.vsyncTick, frame.width, frame.height,
            frame.displayFbp, frame.sourceFbp, frame.usedPreferred ? 1u : 0u, hash);
        return frame;
    }

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
    {
        log("K rgba=%08x tick=%llu\n", rgba, tickNow());
        return m_inner ? m_inner->ClearFramebuffer(context, rgba) : false;
    }

    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
    {
        // Lazy download at first consume (PCSX2 GSInitAndReadFIFO shape):
        // the inner CPU buffer holds the setup-time VRAM snapshot, so the
        // bytes stay CPU-identical while the adapter exercises download
        // trigger, cursor and partial-read serving.
        if (!m_fifoValid)
        {
            m_fifo.clear();
            m_fifoCursor = 0u;
            if (m_inner)
            {
                uint8_t chunk[65536];
                for (;;)
                {
                    const uint32_t n = m_inner->ConsumeLocalToHostBytes(chunk, sizeof(chunk));
                    if (n == 0u)
                        break;
                    m_fifo.insert(m_fifo.end(), chunk, chunk + n);
                }
            }
            m_fifoValid = true;
            ++m_stats.fifoDownloads;
            m_stats.fifoDownloadBytes += m_fifo.size();
            log("D bytes=%zu tick=%llu\n", m_fifo.size(), tickNow());
        }
        uint32_t n = 0u;
        if (dst && maxBytes != 0u && m_fifoCursor < m_fifo.size())
        {
            n = static_cast<uint32_t>(
                std::min<size_t>(maxBytes, m_fifo.size() - m_fifoCursor));
            std::memcpy(dst, m_fifo.data() + m_fifoCursor, n);
            m_fifoCursor += n;
        }
        ++m_stats.consumes;
        m_stats.consumeBytes += n;
        log("C max=%u got=%u cursor=%zu/%zu tick=%llu\n", maxBytes, n,
            m_fifoCursor, m_fifo.size(), tickNow());
        return n;
    }

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
    {
        return m_inner ? m_inner->ReadVram(psm, base, bw, x, y) : 0u;
    }

    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y,
                   uint32_t value) override
    {
        if (m_inner)
            m_inner->WriteVram(psm, base, bw, x, y, value);
    }

    void SnapshotVram(std::vector<uint8_t> &out) const override
    {
        if (m_inner)
            m_inner->SnapshotVram(out);
        else
            out.clear();
    }

    GSTransferSnapshot GetTransferSnapshot() const override
    {
        GSTransferSnapshot snap{};
        if (m_inner)
            snap = m_inner->GetTransferSnapshot();
        if (m_fifoValid)
            snap.localToHostPendingBytes =
                m_fifoCursor < m_fifo.size() ? m_fifo.size() - m_fifoCursor : 0u;
        return snap;
    }

    bool WantsRawGif() const override { return true; }

    void RawGifPacket(uint32_t path, const uint8_t *data, uint32_t sizeBytes) override
    {
        ++m_stats.gifPackets;
        m_stats.gifBytes += sizeBytes;
        const size_t slot = path < 4u ? path : 0u;
        ++m_stats.gifPacketsByPath[slot];
        m_stats.gifQwords[slot] += sizeBytes / 16u;
        m_pathCrc[slot] = data && sizeBytes ? fnv1a32(data, sizeBytes, m_pathCrc[slot]) : m_pathCrc[slot];
        log("G path=%u size=%u crc=%08x tick=%llu%s\n", path, sizeBytes,
            data && sizeBytes ? fnv1a32(data, sizeBytes) : 0u, tickNow(),
            (sizeBytes % 16u) ? " ODD" : "");
    }

    void RawWriteRegister(uint8_t regAddr, uint64_t value) override
    {
        ++m_stats.regWrites;
        log("R addr=%02x val=%016llx tick=%llu\n", regAddr,
            (unsigned long long)value, tickNow());
    }

    void RawNativePackedPacket(const uint8_t *data, uint32_t sizeBytes) override
    {
        ++m_stats.nativePacked;
        log("N size=%u crc=%08x tick=%llu\n", sizeBytes,
            data && sizeBytes ? fnv1a32(data, sizeBytes) : 0u, tickNow());
    }

    bool WantsGuestVsync() const override { return true; }

    void GuestVsync(uint64_t tick, uint32_t field) override
    {
        ++m_stats.vsyncs;
        if (m_lastVsyncTick != 0u && tick != m_lastVsyncTick + 1u)
            ++m_stats.vsyncGaps;
        m_lastVsyncTick = tick;
        log("V tick=%llu field=%u\n", (unsigned long long)tick, field);
    }

    bool WantsPrivMirror() const override { return true; }

    void PrivMirrored(uint32_t registerOffset, uint64_t value) override
    {
        ++m_stats.privMirrored;
        for (size_t i = 0; i < 19u; ++i)
            if (kMirrorOffsets[i] == registerOffset)
                m_mirror[i] = value;
        log("P off=%04x val=%016llx tick=%llu\n", registerOffset,
            (unsigned long long)value, tickNow());
    }

    bool SavestateIdle() const override
    {
        const bool innerIdle = m_inner ? m_inner->SavestateIdle() : true;
        const bool fifoDrained = !m_fifoValid || m_fifoCursor >= m_fifo.size();
        return innerIdle && fifoDrained;
    }

    std::string SavestateBusyReason() const override
    {
        // Armed-but-undownloaded (inner holds the setup snapshot) and
        // downloaded-but-undrained (the adapter cursor) are both "bytes the
        // guest may still read": the snapshot already unifies the two.
        if (GetTransferSnapshot().localToHostPendingBytes != 0u)
            return "gs-transfer";
        return m_inner ? m_inner->SavestateBusyReason() : std::string{};
    }

    void SavestateSave(std::vector<uint8_t> &out) override
    {
        out.clear();
        out.insert(out.end(), kSaveMagic, kSaveMagic + 8u);
        putU32(out, kSaveVersion);
        putU64(out, m_stats.gifPackets);
        putU64(out, m_stats.vsyncs);
        putU64(out, m_stats.privMirrored);
        putU64(out, m_stats.consumes);
        putU64(out, m_stats.presents);
        putU64(out, m_lastVsyncTick);
        for (const uint64_t v : m_mirror)
            putU64(out, v);
        out.push_back(m_fifoValid ? 1u : 0u);
        putU64(out, m_fifoCursor);
        putU64(out, m_fifo.size());
        out.insert(out.end(), m_fifo.begin(), m_fifo.end());
        std::vector<uint8_t> innerBlob;
        if (m_inner)
            m_inner->SavestateSave(innerBlob);
        putU64(out, innerBlob.size());
        out.insert(out.end(), innerBlob.begin(), innerBlob.end());
        ++m_stats.saves;
        log("# save bytes=%zu\n", out.size());
    }

    bool SavestateLoad(const uint8_t *data, size_t size) override
    {
        ++m_stats.loads;
        bool ok = false;
        if (data && size >= 8u + 4u && std::memcmp(data, kSaveMagic, 8u) == 0)
        {
            const uint8_t *p = data + 8u;
            const uint8_t *end = data + size;
            uint32_t version = 0u;
            uint64_t gif = 0u, vsync = 0u, priv = 0u, con = 0u, pres = 0u, lastV = 0u;
            std::array<uint64_t, 19> mirror{};
            ok = takeU32(p, end, version) && version == kSaveVersion &&
                 takeU64(p, end, gif) && takeU64(p, end, vsync) && takeU64(p, end, priv) &&
                 takeU64(p, end, con) && takeU64(p, end, pres) && takeU64(p, end, lastV);
            for (size_t i = 0; ok && i < mirror.size(); ++i)
                ok = takeU64(p, end, mirror[i]);
            uint8_t valid = 0u;
            uint64_t cursor = 0u, fifoSize = 0u;
            if (ok && (end - p < 1 || (valid = *p++, valid > 1u) || !takeU64(p, end, cursor) ||
                       !takeU64(p, end, fifoSize) ||
                       static_cast<uint64_t>(end - p) < fifoSize))
                ok = false;
            std::vector<uint8_t> fifo;
            uint64_t innerSize = 0u;
            if (ok)
            {
                fifo.assign(p, p + static_cast<size_t>(fifoSize));
                p += static_cast<size_t>(fifoSize);
                ok = takeU64(p, end, innerSize) &&
                     static_cast<uint64_t>(end - p) == innerSize;
            }
            bool innerOk = false;
            if (ok)
            {
                if (!m_inner)
                    m_inner = std::make_unique<GSCpuBackend>();
                innerOk = m_inner->SavestateLoad(p, static_cast<size_t>(innerSize));
                ok = innerOk;
            }
            if (ok)
            {
                m_stats.gifPackets = gif;
                m_stats.vsyncs = vsync;
                m_stats.privMirrored = priv;
                m_stats.consumes = con;
                m_stats.presents = pres;
                m_lastVsyncTick = lastV;
                m_mirror = mirror;
                m_fifoValid = valid != 0u;
                m_fifo = std::move(fifo);
                m_fifoCursor = static_cast<size_t>(cursor);
                if (m_fifoCursor > m_fifo.size())
                    m_fifoCursor = m_fifo.size();
            }
        }
        log("# load bytes=%zu ok=%u\n", size, ok ? 1u : 0u);
        return ok;
    }

    // Snapshot readers hold s_apiMutex across the copy, so a concurrent
    // destructor (which clears s_live under the same mutex first) cannot
    // strand them on freed members.
    static ps2x_gs_external::Stats copyStats()
    {
        std::lock_guard<std::mutex> lock(s_apiMutex);
        if (!s_live)
            return ps2x_gs_external::Stats{};
        std::lock_guard<std::mutex> inst(s_live->m_apiMutex);
        return s_live->m_stats;
    }

    static bool copyLastPresent(ps2x_gs_external::ExportedFrame &out)
    {
        std::lock_guard<std::mutex> lock(s_apiMutex);
        if (!s_live)
            return false;
        std::lock_guard<std::mutex> inst(s_live->m_apiMutex);
        if (!s_live->m_hasExported)
            return false;
        out = s_live->m_exported;
        return true;
    }

private:
    void logLine(const char *line)
    {
        if (!m_log || m_stats.logTruncated)
            return;
        const int n = std::fputs(line, m_log);
        if (n >= 0)
            noteLogBytes(static_cast<uint64_t>(std::strlen(line)));
    }

    template <typename... Args>
    void log(const char *fmt, Args... args)
    {
        if (!m_log || m_stats.logTruncated)
            return;
        char buf[256];
        const int n = std::snprintf(buf, sizeof(buf), fmt, args...);
        if (n > 0)
        {
            const size_t w = static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n)
                                                                  : sizeof(buf) - 1u;
            std::fwrite(buf, 1, w, m_log);
            noteLogBytes(static_cast<uint64_t>(w));
        }
    }

    void noteLogBytes(uint64_t n)
    {
        m_stats.logBytes += n;
        if (m_stats.logBytes >= kLogCapBytes)
        {
            std::fputs("# truncated at 1 GiB\n", m_log);
            std::fprintf(stderr, "[gs:external] log capped at 1 GiB; counting only\n");
            m_stats.logTruncated = true;
        }
    }

    uint64_t tickNow() const
    {
        return m_priv ? m_priv->vsyncTick.load(std::memory_order_acquire) : 0u;
    }

    const GSRegisters *m_priv = nullptr;
    std::unique_ptr<GSCpuBackend> m_inner;
    FILE *m_log = nullptr;
    ps2x_gs_external::Stats m_stats;
    uint32_t m_pathCrc[4] = {2166136261u, 2166136261u, 2166136261u, 2166136261u};
    uint64_t m_lastVsyncTick = 0u;
    std::array<uint64_t, 19> m_mirror{};
    // Lazy local->host FIFO: downloaded from the inner backend at the first
    // consume after a local->host setup, served with a cursor.
    std::vector<uint8_t> m_fifo;
    size_t m_fifoCursor = 0u;
    bool m_fifoValid = false;
    mutable std::mutex m_apiMutex;
    ps2x_gs_external::ExportedFrame m_exported;
    bool m_hasExported = false;

    static std::mutex s_apiMutex;
    static const ExternalGsBackend *s_live;
};

std::mutex ExternalGsBackend::s_apiMutex;
const ExternalGsBackend *ExternalGsBackend::s_live = nullptr;
} // namespace

namespace ps2x_gs_external
{
bool available()
{
    return true;
}

bool requested()
{
    const char *env = std::getenv("PS2X_GS_BACKEND");
    return env && std::strcmp(env, "external") == 0;
}

std::unique_ptr<GSRasterBackend> create(const GSRegisters *priv)
{
    if (!available())
        return nullptr;
    return std::unique_ptr<GSRasterBackend>(new ExternalGsBackend(priv));
}

Stats stats()
{
    return ExternalGsBackend::copyStats();
}

bool lastPresent(ExportedFrame &out)
{
    return ExternalGsBackend::copyLastPresent(out);
}
} // namespace ps2x_gs_external
