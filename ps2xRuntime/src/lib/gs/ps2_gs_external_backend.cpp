// GE2: external-GS backend shell + recording stub. See the header for the
// boundary contract. Single-threaded: every backend call runs on the GS
// worker (the runtime forces the queue on, like PS2X_GS_BACKEND=parallel);
// only stats()/lastPresent() take the mutex (cross-thread readers).

#include "runtime/gs/ps2_gs_external_backend.h"
#include "ps2_perf_log.h"
#include "runtime/gs/ge1_gs_api.h"
#if defined(__ANDROID__)
#include "runtime/gs/ps2_present_vk.h"
#include <android/hardware_buffer.h>
#endif
#if defined(PS2X_GE1_STATIC_IOSURFACE)
#include "runtime/gs/ps2_present_share.h"
#include <IOSurface/IOSurfaceRef.h>
#include <atomic>
#include <mach/kern_return.h>
#endif

#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace
{
constexpr uint64_t kLogCapBytes = 1ull * 1024ull * 1024ull * 1024ull;
constexpr char kSaveMagic[8] = {'P', 'S', '2', 'X', 'E', 'G', 'S', '1'};
// DS1: v2 appends the GE1 freeze blob (v1 never completed a GE1-live save;
// v1 files refuse cleanly on the version check).
constexpr uint32_t kSaveVersion = 2u;
// GE2 mirror set: same 19 offsets the frontend diffs (gs_frontend.cpp).
constexpr uint32_t kMirrorOffsets[19] = {
    0x0000u, 0x0010u, 0x0020u, 0x0030u, 0x0040u, 0x0050u, 0x0060u,
    0x0070u, 0x0080u, 0x0090u, 0x00A0u, 0x00B0u, 0x00C0u, 0x00D0u,
    0x00E0u, 0x1000u, 0x1010u, 0x1040u, 0x1080u};

struct Ge1Api
{
    void *library = nullptr;
    decltype(&ge1_gs_open) open = nullptr;
    decltype(&ge1_gs_close) close = nullptr;
    decltype(&ge1_gs_reset) reset = nullptr;
    decltype(&ge1_gs_priv_write) privWrite = nullptr;
    decltype(&ge1_gs_packet) packet = nullptr;
    decltype(&ge1_gs_vsync) vsync = nullptr;
    decltype(&ge1_gs_read_fifo) readFifo = nullptr;
    decltype(&ge1_gs_snapshot) snapshot = nullptr;
    decltype(&ge1_gs_export_ahb) exportAhb = nullptr;
    decltype(&ge1_gs_wait_export) waitExport = nullptr;
    decltype(&ge1_gs_release_ahb) releaseAhb = nullptr;
    decltype(&ge1_gs_gpu_ms) gpuMs = nullptr;
#if defined(PS2X_GE1_STATIC_IOSURFACE)
    decltype(&ge1_gs_export_iosurface) exportIOSurface = nullptr;
#endif
    // BG1: optional (pre-PW1 libraries lack it; a missing symbol only skips the pause flush).
    decltype(&ge1_gs_flush_caches) flushCaches = nullptr;
    // DS1: optional (pre-DS1 libraries lack them; without them a GE1-live
    // save keeps refusing, as before).
    decltype(&ge1_gs_freeze_size) freezeSize = nullptr;
    decltype(&ge1_gs_freeze_save) freezeSave = nullptr;
    decltype(&ge1_gs_freeze_load) freezeLoad = nullptr;
    bool freezeBound() const { return freezeSize && freezeSave && freezeLoad; }

    bool load(const char *path)
    {
#if defined(PS2X_GE1_STATIC)
        // GI1: the GE1 adapter is statically linked (iOS, no dlopen). The
        // sentinel "builtin" binds the linked symbols behind the same C ABI.
        if (std::strcmp(path, "builtin") == 0)
        {
            open = ::ge1_gs_open;
            close = ::ge1_gs_close;
            reset = ::ge1_gs_reset;
            privWrite = ::ge1_gs_priv_write;
            packet = ::ge1_gs_packet;
            vsync = ::ge1_gs_vsync;
            readFifo = ::ge1_gs_read_fifo;
            snapshot = ::ge1_gs_snapshot;
            gpuMs = ::ge1_gs_gpu_ms;
            flushCaches = ::ge1_gs_flush_caches; // BG1 fold: static bind (same ABI)
            freezeSize = ::ge1_gs_freeze_size; // DS1: static bind (same ABI)
            freezeSave = ::ge1_gs_freeze_save;
            freezeLoad = ::ge1_gs_freeze_load;
#if defined(PS2X_GE1_STATIC_IOSURFACE)
            exportIOSurface = ::ge1_gs_export_iosurface;
#endif
            std::fprintf(stderr, "[gs:external] GE1 builtin static GS bound\n");
            return true;
        }
#endif
        library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!library)
        {
            std::fprintf(stderr, "[gs:external] GE1 dlopen(%s): %s\n", path, dlerror());
            return false;
        }
#define GE1_SYMBOL(member, symbol) \
        member = reinterpret_cast<decltype(member)>(dlsym(library, #symbol)); \
        if (!member) return false
        GE1_SYMBOL(open, ge1_gs_open);
        GE1_SYMBOL(close, ge1_gs_close);
        GE1_SYMBOL(reset, ge1_gs_reset);
        GE1_SYMBOL(privWrite, ge1_gs_priv_write);
        GE1_SYMBOL(packet, ge1_gs_packet);
        GE1_SYMBOL(vsync, ge1_gs_vsync);
        GE1_SYMBOL(readFifo, ge1_gs_read_fifo);
        GE1_SYMBOL(snapshot, ge1_gs_snapshot);
#if defined(__ANDROID__)
        GE1_SYMBOL(exportAhb, ge1_gs_export_ahb);
        GE1_SYMBOL(waitExport, ge1_gs_wait_export);
        GE1_SYMBOL(releaseAhb, ge1_gs_release_ahb);
#endif
        GE1_SYMBOL(gpuMs, ge1_gs_gpu_ms);
#undef GE1_SYMBOL
        // BG1: optional symbol (see above); never fails the load.
        flushCaches =
            reinterpret_cast<decltype(flushCaches)>(dlsym(library, "ge1_gs_flush_caches"));
        if (!flushCaches)
            std::fprintf(stderr, "[gs:external] GE1 library predates ge1_gs_flush_caches; pause flush off\n");
        // DS1: optional freeze trio (see above); never fails the load.
        freezeSize = reinterpret_cast<decltype(freezeSize)>(dlsym(library, "ge1_gs_freeze_size"));
        freezeSave = reinterpret_cast<decltype(freezeSave)>(dlsym(library, "ge1_gs_freeze_save"));
        freezeLoad = reinterpret_cast<decltype(freezeLoad)>(dlsym(library, "ge1_gs_freeze_load"));
        if (!freezeBound())
            std::fprintf(stderr, "[gs:external] GE1 library predates ge1_gs_freeze_*; live saves refuse\n");
        return true;
    }

    void unload()
    {
        if (library)
            dlclose(library);
        library = nullptr;
    }
};

uint32_t fnv1a32(const uint8_t *data, size_t size, uint32_t hash = 2166136261u)
{
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

#if defined(PS2X_GE1_STATIC_IOSURFACE)
// GI1: IOSurface slot pool for the async Metal export. Process-global: the
// surfaces are retained for the process (like createSurface's pool) and the
// completion handler may fire after the backend is destroyed, so the epoch
// stales in-flight exports instead of freeing anything.
constexpr int kIOSurfaceSlotCount = 3;
constexpr uint32_t kIOSurfaceWidth = 640u;
constexpr uint32_t kIOSurfaceHeight = 480u;

struct IOSurfacePool
{
    std::mutex mutex;
    uint64_t epoch = 1u;
    void *surfaces[kIOSurfaceSlotCount] = {};
    std::atomic<bool> busy[kIOSurfaceSlotCount];
    std::atomic<uint64_t> seq{0u};
};

IOSurfacePool &ioPool()
{
    static IOSurfacePool pool;
    return pool;
}

struct IOSurfaceExportCtx
{
    uint64_t epoch;
    int slot;
    uint64_t tick;
};

void dumpIOSurface(void *surface, uint64_t tick)
{
    // Dump the first export at/after each target tick (exact-tick equality is
    // unreliable: ticks whose export finds all pool slots busy are skipped).
    // Called only from ioExportDone under the pool mutex; the statics below
    // are safe. The filename records the actual tick dumped.
    static bool dumped2100 = false, dumped3000 = false;
    // Earliest pending target only; one file per call, filename = actual tick.
    const bool want = (!dumped2100 && tick >= 2100u) || (!dumped3000 && tick >= 3000u);
    if (!want)
        return;
    const char *dir = std::getenv("PS2X_GS_IOSURFACE_DUMP_DIR");
    if (!dir || !*dir)
        return;
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    IOSurfaceRef ref = static_cast<IOSurfaceRef>(surface);
    if (IOSurfaceGetWidth(ref) != kIOSurfaceWidth || IOSurfaceGetHeight(ref) != kIOSurfaceHeight)
        return;
    if (IOSurfaceLock(ref, kIOSurfaceLockReadOnly, nullptr) != KERN_SUCCESS)
        return;
    if (!dumped2100 && tick >= 2100u)
        dumped2100 = true;
    else
        dumped3000 = true;
    const uint8_t *base = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(ref));
    const size_t rowBytes = IOSurfaceGetBytesPerRow(ref);
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/ge1-iosurface-t%llu.ppm", dir, (unsigned long long)tick);
    if (FILE *f = std::fopen(path, "wb"))
    {
        std::fprintf(f, "P6\n%u %u\n255\n", kIOSurfaceWidth, kIOSurfaceHeight);
        std::vector<uint8_t> row(static_cast<size_t>(kIOSurfaceWidth) * 3u);
        for (uint32_t y = 0; y < kIOSurfaceHeight; ++y)
        {
            const uint8_t *src = base + static_cast<size_t>(y) * rowBytes;
            for (uint32_t x = 0; x < kIOSurfaceWidth; ++x)
            {
                row[static_cast<size_t>(x) * 3u + 0u] = src[static_cast<size_t>(x) * 4u + 2u];
                row[static_cast<size_t>(x) * 3u + 1u] = src[static_cast<size_t>(x) * 4u + 1u];
                row[static_cast<size_t>(x) * 3u + 2u] = src[static_cast<size_t>(x) * 4u + 0u];
            }
            std::fwrite(row.data(), 1u, row.size(), f);
        }
        std::fclose(f);
        std::fprintf(stderr, "[gs:external] GE1 IOSurface dump tick=%llu -> %s\n",
                     (unsigned long long)tick, path);
    }
    IOSurfaceUnlock(ref, kIOSurfaceLockReadOnly, nullptr);
}

// Command-buffer completion thread: publish only here (GE2 contract), then
// free the slot. Never touches backend state; the epoch drops stale exports.
void ioExportDone(void *rawCtx, int ok)
{
    std::unique_ptr<IOSurfaceExportCtx> ctx(static_cast<IOSurfaceExportCtx *>(rawCtx));
    IOSurfacePool &pool = ioPool();
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        if (ok && ctx->epoch == pool.epoch)
        {
            void *surface = pool.surfaces[ctx->slot];
            dumpIOSurface(surface, ctx->tick);
            ps2x_present_share::publish({surface, kIOSurfaceWidth, kIOSurfaceHeight, ++pool.seq});
            static std::once_flag once;
            std::call_once(once, [tick = ctx->tick] {
                std::fprintf(stderr, "[gs:external] GE1 IOSurface first publish tick=%llu\n",
                             (unsigned long long)tick);
            });
        }
    }
    pool.busy[ctx->slot].store(false);
}
#endif

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
#if defined(__ANDROID__)
        retireAhbSlots();
#endif
#if defined(PS2X_GE1_STATIC_IOSURFACE)
        // GI1: stale in-flight completion handlers (they still free their
        // slots, but must not publish after the backend is gone).
        {
            std::lock_guard<std::mutex> lock(ioPool().mutex);
            ++ioPool().epoch;
        }
#endif
        if (m_ge1Active)
            m_ge1.close();
        m_ge1.unload();
        if (m_gpuCsv)
            std::fclose(m_gpuCsv);
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
                     "[gs:external] %s done gif=%llu (p1=%llu p2=%llu p3=%llu) npack=%llu reg=%llu priv=%llu "
                     "vsync=%llu gaps=%llu xfer=%llu up=%llu con=%llu dl=%llu pres=%llu log=%s\n",
                     m_ge1Active ? "GE1 live" : "stub", (unsigned long long)m_stats.gifPackets,
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
        m_vram = vram;
        m_vramSize = vramSize;
        if (const char *path = std::getenv("PS2X_GS_EXTERNAL_LIBRARY"); path && *path)
        {
            if (!m_ge1.load(path) || !m_ge1.open(4))
            {
                std::fprintf(stderr, "[gs:external] GE1 Full GS init failed\n");
                std::exit(78);
            }
            m_ge1Active = true;
            const std::array<uint64_t, 20> words = resetWords();
            if (!m_ge1.reset(words.data(), vram, vramSize))
            {
                std::fprintf(stderr, "[gs:external] GE1 reset import failed (vram=%u)\n", vramSize);
                std::exit(78);
            }
            std::fprintf(stderr, "[gs:external] GE1 Full live GS loaded: %s\n", path);
            m_perfTail = ps2x::perflog::enabled();
            if (const char *csv = std::getenv("PS2X_GS_EXTERNAL_GPU_CSV"); csv && *csv)
            {
                m_gpuCsv = std::fopen(csv, "w");
                if (m_gpuCsv)
                    std::fputs("tick,gpu_ms\n", m_gpuCsv);
            }
        }
        else
        {
            m_inner = std::make_unique<GSCpuBackend>();
            m_inner->Initialize(vram, vramSize);
        }
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
        m_ge1FifoBytes = 0u;
        m_ge1FifoServed = false;
        if (m_ge1Active)
        {
            const std::array<uint64_t, 20> words = resetWords();
            if (!m_ge1.reset(words.data(), m_vram, m_vramSize))
            {
                std::fprintf(stderr, "[gs:external] GE1 reset failed\n");
                std::exit(78);
            }
        }
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
            m_ge1FifoBytes = 3u * static_cast<uint32_t>(command.trxreg.rrw) *
                             static_cast<uint32_t>(command.trxreg.rrh);
            m_ge1FifoServed = false;
        }
        else
            m_ge1FifoBytes = 0u;
        if (m_inner)
            m_inner->BeginTransfer(command);
    }

    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
    {
        ++m_stats.uploads;
        m_stats.uploadBytes += sizeBytes;
        if (m_log)
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

    void snapshotToFrame(const GSPresentationRequest &request, PresentationFrame &frame)
    {
        uint32_t width = 0u, height = 0u;
        const uint32_t *rgba = nullptr;
        if (m_ge1.snapshot(&width, &height, &rgba) && rgba && width && height)
        {
            frame.width = width;
            frame.height = height;
            frame.pixels.resize(static_cast<size_t>(width) * height * 4u);
            std::memcpy(frame.pixels.data(), rgba, frame.pixels.size());
            frame.displayFbp = static_cast<uint32_t>(request.dispfb1 & 0x1ffu);
            frame.sourceFbp = frame.displayFbp;
        }
    }

    PresentationFrame Present(const GSPresentationRequest &request) override
    {
        PresentationFrame frame;
        if (m_ge1Active)
        {
#if defined(__ANDROID__)
            if (ps2x_present_vk::enabled() && !ps2x_present_vk::broken())
            {
                if (presentAhb(request.vsyncTick))
                {
                    frame.width = 640u;
                    frame.height = 480u;
                    frame.displayFbp = static_cast<uint32_t>(request.dispfb1 & 0x1ffu);
                    frame.sourceFbp = frame.displayFbp;
                }
            }
            else
            {
                snapshotToFrame(request, frame);
            }
#elif defined(PS2X_GE1_STATIC_IOSURFACE)
            // GI1: async GPU export publishes through the shared mailbox; the
            // pixels stay empty either way (AHB shape). Zero-copy off, or a
            // busy/failed export, falls back to the CPU snapshot.
            if (presentIOSurface(request.vsyncTick))
            {
                frame.width = 640u;
                frame.height = 480u;
                frame.displayFbp = static_cast<uint32_t>(request.dispfb1 & 0x1ffu);
                frame.sourceFbp = frame.displayFbp;
            }
            else
            {
                snapshotToFrame(request, frame);
            }
#else
            {
                snapshotToFrame(request, frame);
            }
#endif
        }
        else if (m_inner)
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
            if (m_ge1Active && !m_ge1FifoServed && m_ge1FifoBytes)
            {
                const uint32_t qwords = (m_ge1FifoBytes + 15u) / 16u;
                std::vector<uint8_t> readback(static_cast<size_t>(qwords) * 16u);
                if (!m_ge1.readFifo(readback.data(), qwords))
                {
                    std::fprintf(stderr, "[gs:external] GE1 FIFO read failed, qwc=%u\n", qwords);
                    std::exit(78);
                }
                m_fifo.assign(readback.begin(), readback.begin() + m_ge1FifoBytes);
                m_ge1FifoServed = true;
            }
            else if (m_inner)
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
        if (m_ge1Active && !m_ge1FifoServed)
            snap.localToHostPendingBytes = m_ge1FifoBytes;
        if (m_fifoValid)
            snap.localToHostPendingBytes =
                m_fifoCursor < m_fifo.size() ? m_fifo.size() - m_fifoCursor : 0u;
        return snap;
    }

    bool WantsRawGif() const override { return true; }
    bool WantsMinimalGifDecode() const override { return m_ge1Active; }

    void RawGifPacket(uint32_t path, const uint8_t *data, uint32_t sizeBytes) override
    {
        if (m_ge1Active)
        {
            if (path < 1u || path > 3u || !m_ge1.packet(static_cast<uint8_t>(path), data, sizeBytes))
            {
                std::fprintf(stderr, "[gs:external] GE1 GIF rejected path=%u bytes=%u\n", path, sizeBytes);
                std::exit(78);
            }
            m_ge1LastPath = static_cast<uint8_t>(path);
        }
        ++m_stats.gifPackets;
        m_stats.gifBytes += sizeBytes;
        const size_t slot = path < 4u ? path : 0u;
        ++m_stats.gifPacketsByPath[slot];
        m_stats.gifQwords[slot] += sizeBytes / 16u;
        if (m_log)
            log("G path=%u size=%u crc=%08x tick=%llu%s\n", path, sizeBytes,
                data && sizeBytes ? fnv1a32(data, sizeBytes) : 0u, tickNow(),
                (sizeBytes % 16u) ? " ODD" : "");
    }

    void RawWriteRegister(uint8_t regAddr, uint64_t value) override
    {
        if (m_ge1Active)
        {
            // HLE writes bypass GIF. Feed an ordinary one-loop A+D GIF tag
            // through the same public packet entry point, in stream order.
            alignas(16) const uint64_t ad[4] = {
                1ull | (1ull << 15) | (1ull << 60), 0xEull, value, regAddr};
            if (!m_ge1.packet(3u, reinterpret_cast<const uint8_t *>(ad), sizeof(ad)))
            {
                std::fprintf(stderr, "[gs:external] GE1 HLE register rejected addr=%u\n", regAddr);
                std::exit(78);
            }
        }
        ++m_stats.regWrites;
        log("R addr=%02x val=%016llx tick=%llu\n", regAddr,
            (unsigned long long)value, tickNow());
    }

    void RawNativePackedPacket(const uint8_t *data, uint32_t sizeBytes) override
    {
        if (m_ge1Active && !m_ge1.packet(m_ge1LastPath, data, sizeBytes))
        {
            std::fprintf(stderr, "[gs:external] GE1 native packet rejected bytes=%u\n", sizeBytes);
            std::exit(78);
        }
        ++m_stats.nativePacked;
        if (m_log)
            log("N size=%u crc=%08x tick=%llu\n", sizeBytes,
                data && sizeBytes ? fnv1a32(data, sizeBytes) : 0u, tickNow());
    }

    bool WantsGuestVsync() const override { return true; }

    // BG1: pause-hook persist (pipeline cache + TFX selectors). Runs on the GS
    // worker at stream position via GS::flushExternalCaches, under the same
    // single-threaded backend contract as GuestVsync.
    void FlushCaches() override
    {
        int rc = -1;
        if (m_ge1Active && m_ge1.flushCaches)
            rc = m_ge1.flushCaches();
        std::fprintf(stderr, "[gs:external] BG1 pause flush rc=%d tick=%llu\n", rc, tickNow());
        log("# bg1-flush rc=%d tick=%llu\n", rc, tickNow());
    }

    void GuestVsync(uint64_t tick, uint32_t field) override
    {
        if (m_ge1Active)
        {
            // GE2 passes CSR FIELD; PCSX2 GSvsync uses the opposite field convention.
            if (!m_ge1.vsync(field ? 0u : 1u, m_mirror[15], m_mirror[1], m_mirror[6]))
            {
                std::fprintf(stderr, "[gs:external] GE1 VSync failed tick=%llu\n",
                             (unsigned long long)tick);
                std::exit(78);
            }
            // PT2: gpuMs() is reset-on-read, so sample once and share between
            // the CSV and the gpu.busy ring. With both off the call is
            // skipped exactly as before; <0 (closed/unsupported) pushes
            // nothing (the stage line reads n=0).
            if (m_gpuCsv || m_perfTail)
            {
                const float gpuMs = m_ge1.gpuMs();
                if (m_gpuCsv)
                    std::fprintf(m_gpuCsv, "%llu,%.6f\n", (unsigned long long)tick, gpuMs);
                if (m_perfTail && gpuMs >= 0.0f)
                    ps2x::perflog::stageRing(ps2x::perflog::Stage::GpuBusy)
                        .push(static_cast<uint32_t>(tick), gpuMs);
            }
        }
        ++m_stats.vsyncs;
#if PS2X_ENABLE_DIAG_TAPS && defined(__ANDROID__)
        if (m_ge1Active && m_frameCensus && (tick % 300u) == 0u)
            std::fprintf(stderr, "[gs:frame-counts] tick=%llu processed=%llu "
                                 "present_attempt=%llu export_started=%llu export_complete=%llu "
                                 "queued=%llu no_slot=%llu\n",
                         (unsigned long long)tick, (unsigned long long)m_stats.vsyncs,
                         (unsigned long long)m_ahbAttempts, (unsigned long long)m_ahbExportStarted,
                         (unsigned long long)m_ahbExportCompleted, (unsigned long long)m_ahbQueued,
                         (unsigned long long)m_ahbNoSlot);
#endif
        if (m_lastVsyncTick != 0u && tick != m_lastVsyncTick + 1u)
            ++m_stats.vsyncGaps;
        m_lastVsyncTick = tick;
        log("V tick=%llu field=%u\n", (unsigned long long)tick, field);
    }

    bool WantsPrivMirror() const override { return true; }

    void PrivMirrored(uint32_t registerOffset, uint64_t value) override
    {
        if (m_ge1Active && !m_ge1.privWrite(registerOffset, value))
        {
            std::fprintf(stderr, "[gs:external] GE1 priv write rejected offset=%x\n", registerOffset);
            std::exit(78);
        }
        ++m_stats.privMirrored;
        for (size_t i = 0; i < 19u; ++i)
            if (kMirrorOffsets[i] == registerOffset)
                m_mirror[i] = value;
        log("P off=%04x val=%016llx tick=%llu\n", registerOffset,
            (unsigned long long)value, tickNow());
    }

    bool SavestateIdle() const override
    {
        // DS1: GE1-live saves ride the freeze trio; without it (or an old
        // library) keep refusing the incomplete save, as before.
        if (m_ge1Active && !m_ge1.freezeBound())
            return false;
        const bool innerIdle = m_inner ? m_inner->SavestateIdle() : true;
        const bool fifoDrained = !m_fifoValid || m_fifoCursor >= m_fifo.size();
        return innerIdle && fifoDrained;
    }

    std::string SavestateBusyReason() const override
    {
        if (m_ge1Active && !m_ge1.freezeBound())
            return "gs-external-freeze-unimplemented";
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
        // DS1: the GE1 lib's own state (VRAM, regs, paths) via GSfreeze.
        // Runs on the GS worker, the same thread as every other ge1 call. A
        // failed freeze writes an empty blob (loudly); the load refuses it,
        // so a failed save can never load silently.
        std::vector<uint8_t> freezeBlob;
        if (m_ge1Active && m_ge1.freezeBound())
        {
            const int need = m_ge1.freezeSize();
            if (need > 0 && static_cast<uint64_t>(need) <= (64u << 20))
            {
                freezeBlob.resize(static_cast<size_t>(need));
                if (!m_ge1.freezeSave(freezeBlob.data(), static_cast<uint32_t>(need)))
                {
                    std::fprintf(stderr, "[gs:external] GE1 freeze save failed\n");
                    freezeBlob.clear();
                }
            }
            else
            {
                std::fprintf(stderr, "[gs:external] GE1 freeze size failed (%d)\n", need);
            }
        }
        putU64(out, freezeBlob.size());
        out.insert(out.end(), freezeBlob.begin(), freezeBlob.end());
        ++m_stats.saves;
        log("# save bytes=%zu freeze=%zu\n", out.size(), freezeBlob.size());
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
                     static_cast<uint64_t>(end - p) >= innerSize + 8u;
            }
            const uint8_t *innerData = p;
            if (ok)
                p += static_cast<size_t>(innerSize);
            uint64_t freezeSize = 0u;
            if (ok)
            {
                ok = takeU64(p, end, freezeSize) &&
                     static_cast<uint64_t>(end - p) == freezeSize && freezeSize <= (64u << 20);
            }
            // DS1: an empty freeze blob under a live GE1 refuses (a failed
            // save must never load); a blob without the trio refuses too.
            if (ok && freezeSize == 0u && m_ge1Active)
                ok = false;
            if (ok && freezeSize != 0u && !m_ge1.freezeBound())
                ok = false;
            bool innerOk = false;
            if (ok)
            {
                if (!m_inner)
                    m_inner = std::make_unique<GSCpuBackend>();
                innerOk = m_inner->SavestateLoad(innerData, static_cast<size_t>(innerSize));
                ok = innerOk;
            }
            if (ok && freezeSize != 0u)
            {
                if (!m_ge1.freezeLoad(p, static_cast<uint32_t>(freezeSize)))
                {
                    std::fprintf(stderr, "[gs:external] GE1 freeze load failed\n");
                    ok = false;
                }
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
#if defined(__ANDROID__)
    struct AhbSlot
    {
        uint64_t id = 0u;
        AHardwareBuffer *buffer = nullptr;
    };

    bool queuePendingAhb()
    {
        if (m_pendingAhb < 0)
            return true;
        m_ge1.waitExport(m_pendingFence);
#if PS2X_ENABLE_DIAG_TAPS
        ++m_ahbExportCompleted;
#endif
        AhbSlot &slot = m_ahbSlots[static_cast<size_t>(m_pendingAhb)];
        dumpAhb(slot.buffer, m_pendingTick);
        const bool queued = ps2x_present_vk::queue(slot.id, 640u, 480u);
        m_pendingAhb = -1;
        m_pendingFence = 0u;
        if (!queued)
            ps2x_present_vk::fallBack("GE1 AHB queue failed");
#if PS2X_ENABLE_DIAG_TAPS
        if (queued)
            ++m_ahbQueued;
#endif
        return queued;
    }

    void retireAhbSlots()
    {
        if (m_pendingAhb >= 0 && m_ge1Active)
            m_ge1.waitExport(m_pendingFence);
        m_pendingAhb = -1;
        for (AhbSlot &slot : m_ahbSlots)
        {
            if (slot.id)
            {
                if (m_ge1Active && m_ge1.releaseAhb)
                    m_ge1.releaseAhb(slot.buffer);
                ps2x_present_vk::retireBuffer(slot.id);
                slot = {};
            }
        }
    }

    void dumpAhb(AHardwareBuffer *buffer, uint64_t tick)
    {
        if (tick != 2100u && tick != 3000u)
            return;
        const char *dir = std::getenv("PS2X_GS_AHB_DUMP_DIR");
        if (!dir || !*dir)
            return;
        AHardwareBuffer_Desc desc = {};
        AHardwareBuffer_describe(buffer, &desc);
        void *mapped = nullptr;
        if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr, &mapped) != 0 || !mapped)
        {
            std::fprintf(stderr, "[gs:external] AHB dump lock failed tick=%llu\n", (unsigned long long)tick);
            return;
        }
        const std::string path = std::string(dir) + "/ge1-ahb-t" + std::to_string(tick) + ".ppm";
        FILE *file = std::fopen(path.c_str(), "wb");
        if (file)
        {
            std::fprintf(file, "P6\n%u %u\n255\n", desc.width, desc.height);
            const auto *src = static_cast<const uint8_t *>(mapped);
            for (uint32_t y = 0; y < desc.height; ++y)
                for (uint32_t x = 0; x < desc.width; ++x)
                    std::fwrite(src + (static_cast<size_t>(y) * desc.stride + x) * 4u, 1, 3, file);
            std::fclose(file);
            std::fprintf(stderr, "[gs:external] AHB dump tick=%llu path=%s\n", (unsigned long long)tick, path.c_str());
        }
        AHardwareBuffer_unlock(buffer, nullptr);
    }

    bool presentAhb(uint64_t tick)
    {
#if PS2X_ENABLE_DIAG_TAPS
        ++m_ahbAttempts;
#endif
        if (m_ahbSlots[0].id && m_ahbEpoch != ps2x_present_vk::poolEpoch())
            retireAhbSlots();
        if (!queuePendingAhb())
            return false;
        if (!m_ahbSlots[0].id)
        {
            for (AhbSlot &slot : m_ahbSlots)
            {
                slot.id = ps2x_present_vk::allocateBuffer(640u, 480u, &slot.buffer);
                if (!slot.id || !slot.buffer)
                {
                    retireAhbSlots();
                    ps2x_present_vk::fallBack("GE1 AHB pool allocation failed");
                    return false;
                }
            }
            m_ahbEpoch = ps2x_present_vk::bufferEpoch(m_ahbSlots[0].id);
            std::fprintf(stderr, "[gs:external] GE1 AHB pool ready epoch=%u\n", m_ahbEpoch);
        }
        uint64_t ids[4];
        for (int i = 0; i < 4; ++i) ids[i] = m_ahbSlots[i].id;
        // Benchmark-only: let guest GS work continue when the display still
        // owns every buffer. A later VSync will present the newest frame.
        static const bool unpacedPresent = [] {
            const char *v = std::getenv("PS2X_PRESENT_UNPACED");
            return v && std::strcmp(v, "1") == 0;
        }();
        const ps2x_present_vk::Pick pick =
            ps2x_present_vk::pickReusable(ids, 4, m_ahbStart, unpacedPresent ? 0 : 1000);
        if (pick.index < 0)
        {
#if PS2X_ENABLE_DIAG_TAPS
            ++m_ahbNoSlot;
#endif
            if (!unpacedPresent && pick.giveUp)
                ps2x_present_vk::fallBack("GE1 AHB compositor release timeout");
            return false;
        }
        AhbSlot &slot = m_ahbSlots[pick.index];
        m_ahbStart = (pick.index + 1) % 4;
        uint64_t fence = 0u;
        const int exported = m_ge1.exportAhb(slot.buffer, 640u, 480u, &fence);
        if (exported == 0)
            return false; // the first host present may precede the first GS VSync
        if (exported < 0)
        {
            std::fprintf(stderr, "[gs:external] AHB export failed at tick=%llu\n", (unsigned long long)tick);
            ps2x_present_vk::fallBack("GE1 GPU to AHB export failed");
            return false;
        }
#if PS2X_ENABLE_DIAG_TAPS
        ++m_ahbExportStarted;
#endif
        m_pendingAhb = pick.index;
        m_pendingFence = fence;
        m_pendingTick = tick;
        // Finish fixed-tick diagnostic exports before their stop boundary.
        if (std::getenv("PS2X_GS_AHB_DUMP_DIR") && (tick == 2100u || tick == 3000u))
            return queuePendingAhb();
        return true;
    }
#endif

#if defined(PS2X_GE1_STATIC_IOSURFACE)
    // GI1: queue an async GPU export into a free IOSurface slot. The publish
    // happens later, from the command buffer's completion handler (ioExportDone).
    // False = fall back to the CPU snapshot for this frame (all slots busy,
    // no composed frame yet, or the export failed).
    bool presentIOSurface(uint64_t tick)
    {
        if (!ps2x_present_share::enabled() || !m_ge1.exportIOSurface)
            return false;
        IOSurfacePool &pool = ioPool();
        int slot = -1;
        for (int i = 0; i < kIOSurfaceSlotCount; ++i)
        {
            bool expected = false;
            if (pool.busy[i].compare_exchange_strong(expected, true))
            {
                slot = i;
                break;
            }
        }
        if (slot < 0)
            return false;
        void *surface = nullptr;
        uint64_t epoch = 0u;
        {
            std::lock_guard<std::mutex> lock(pool.mutex);
            if (!pool.surfaces[slot])
                pool.surfaces[slot] = ps2x_present_share::createSurface(kIOSurfaceWidth, kIOSurfaceHeight);
            surface = pool.surfaces[slot];
            epoch = pool.epoch;
        }
        if (!surface)
        {
            pool.busy[slot].store(false);
            return false;
        }
        std::unique_ptr<IOSurfaceExportCtx> ctx(new IOSurfaceExportCtx{epoch, slot, tick});
        const int rc =
            m_ge1.exportIOSurface(surface, kIOSurfaceWidth, kIOSurfaceHeight, &ioExportDone, ctx.get());
        if (rc != 1)
        {
            pool.busy[slot].store(false);
            return false;
        }
        ctx.release(); // ioExportDone owns it from here (fires exactly once)
        return true;
    }
#endif

    std::array<uint64_t, 20> resetWords() const
    {
        std::array<uint64_t, 20> words{};
        std::copy_n(m_mirror.begin(), 16u, words.begin());
        std::copy_n(m_mirror.begin() + 16u, 3u, words.begin() + 17u);
        return words;
    }

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
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0u;
    Ge1Api m_ge1;
    bool m_ge1Active = false;
#if defined(__ANDROID__)
    std::array<AhbSlot, 4> m_ahbSlots{};
    uint32_t m_ahbEpoch = 0u;
    int m_ahbStart = 0;
    int m_pendingAhb = -1;
    uint64_t m_pendingFence = 0u;
    uint64_t m_pendingTick = 0u;
#if PS2X_ENABLE_DIAG_TAPS
    const bool m_frameCensus = [] {
        const char *flag = std::getenv("PS2X_GS_FRAME_CENSUS");
        return flag != nullptr && std::strcmp(flag, "1") == 0;
    }();
    uint64_t m_ahbAttempts = 0, m_ahbExportStarted = 0;
    uint64_t m_ahbExportCompleted = 0, m_ahbQueued = 0, m_ahbNoSlot = 0;
#endif
#endif
    uint8_t m_ge1LastPath = 3u;
    uint32_t m_ge1FifoBytes = 0u;
    bool m_ge1FifoServed = false;
    FILE *m_gpuCsv = nullptr;
    // PT2: cached PS2X_PERF_LOG (set in Initialize); feeds the gpu.busy ring.
    bool m_perfTail = false;
    std::unique_ptr<GSCpuBackend> m_inner;
    FILE *m_log = nullptr;
    ps2x_gs_external::Stats m_stats;
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
