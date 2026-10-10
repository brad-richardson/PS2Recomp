// GE2: external-GS backend shell + recording stub. See the header for the
// boundary contract. Single-threaded: every backend call runs on the GS
// worker (the runtime forces the queue on, like PS2X_GS_BACKEND=parallel);
// only stats()/lastPresent() take the mutex (cross-thread readers).

#include "runtime/gs/ps2_gs_external_backend.h"
#include "ps2_telemetry.h"
#include "ps2_adpf.h"
#include "ps2_perf_log.h"
#include "runtime/gs/ge1_gs_api.h"
#include "ps2_native_world.h"
#if defined(__ANDROID__)
#include "runtime/gs/ps2_present_vk.h"
#include "ps2_present_geometry.h"
// HUD4: the Android GPU composite runs inside GE1 (ge1_gs_hud_scene, via the
// Ge1Api table); no GLES backend (HUD3's EGL glue is dropped).
#endif
#if defined(__ANDROID__) || defined(PS2X_GE1_STATIC_IOSURFACE)
// THD1: the Tricky HUD packet + stamp math are platform-neutral; iOS uses
// the same packet and the BGRA-lane stamp as the Android AHB path.
#include "ps2_ssx3_tricky_hud.h"
#include "ps2_ssx3_tricky_layer.h"
#include "runtime/gs/gs_serial_job_thread.h"
#endif
#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#include <android/rect.h>
#endif
#if defined(PS2X_GE1_STATIC_IOSURFACE)
#include "runtime/gs/ps2_present_share.h"
#include "runtime/gs/ps2_present_owner.h"
#include "ps2_ios_runtime.h"
#include "ps2_present_geometry.h"
#include "runtime/gs/ps2_tricky_hud_gl.h" // HUD4 (from HUD3): iOS EAGL composite
#include <IOSurface/IOSurfaceRef.h>
#include <atomic>
#include <mach/kern_return.h>
#endif

#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <array>
#include <chrono>
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
// v1 files refuse cleanly on the version check). CN2b: v3 adds the GE1
// local->host arm (byte count + served flag) after the adapter FIFO; v2
// files still load as "nothing armed".
constexpr uint32_t kSaveVersion = 3u;
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
    // PT2 Part 2: optional (pre-query libraries lack it; without it the
    // gsback.busy stage reads n=0).
    decltype(&ge1_gs_back_ms) backMs = nullptr;
#if defined(PS2X_GE1_STATIC_IOSURFACE)
    decltype(&ge1_gs_export_iosurface) exportIOSurface = nullptr;
#endif
    // BG1: optional (pre-PW1 libraries lack it; a missing symbol only skips the pause flush).
    decltype(&ge1_gs_flush_caches) flushCaches = nullptr;
    decltype(&ge1_gs_set_cache_flush_deferred) deferCacheFlush = nullptr;
    // LT1b: optional probe quartet (pre-LT1b libraries lack it; lagF then reads
    // synchronously at the set).
    decltype(&ge1_gs_probe_request) probeRequest = nullptr;
    decltype(&ge1_gs_probe_resolve_frame) probeResolve = nullptr;
    decltype(&ge1_gs_probe_take) probeTake = nullptr;
    decltype(&ge1_gs_probe_stats) probeStats = nullptr;
    // IOSL1: same policy as the suite-pinned Ge1ProbeBindTable (stats optional).
    bool probeBound() const
    {
        return ps2x_gs_external::Ge1ProbeBindTable{probeRequest, probeResolve, probeTake,
                                                   probeStats}
            .bound();
    }
    // DS1: optional (pre-DS1 libraries lack them; without them a GE1-live
    // save keeps refusing, as before).
    decltype(&ge1_gs_freeze_size) freezeSize = nullptr;
    decltype(&ge1_gs_freeze_save) freezeSave = nullptr;
    decltype(&ge1_gs_freeze_load) freezeLoad = nullptr;
    bool freezeBound() const { return freezeSize && freezeSave && freezeLoad; }
    // NRT1: optional on the dlopen path (an older library lacks it; records
    // are then delivered packet by packet); linked on the static (iOS) path.
    decltype(&ge1_gs_native_record) nativeRecord = nullptr;
    // NRS1: same for compact records (without it the frontend expands them).
    decltype(&ge1_gs_native_record_compact) nativeRecordCompact = nullptr;
    // RZV1 S4a: same for keyed records (without it the embedded compact record goes to nativeRecordCompact).
    decltype(&ge1_gs_native_record_keyed) nativeRecordKeyed = nullptr;
    // RZV1 S4c: optional; the MTVU's prepare call (PS2X_SSX3_NATIVE_KEYED=3).
    decltype(&ge1_gs_static_prepare) staticPrepare = nullptr;
    // HUD4: optional, Android only (a missing symbol keeps the CPU stamp; the
    // static iOS path never binds it, so old iOS GE1 libs still link).
    decltype(&ge1_gs_hud_scene) hudScene = nullptr;

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
            backMs = ::ge1_gs_back_ms; // PT2 Part 2: static bind (same ABI)
            flushCaches = ::ge1_gs_flush_caches; // BG1 fold: static bind (same ABI)
            deferCacheFlush = ::ge1_gs_set_cache_flush_deferred;
            // IOSL1: static probe bind (LT1b quartet). iOS links GE1
            // statically, so lagF uses the linked symbols directly. Unlike
            // the dlsym path this needs the symbols at link time (the
            // a64c36c1 GE1 build provides them); a pre-LT1b dylib on the
            // dlopen path still leaves them null and falls back to a sync
            // read at the set.
            probeRequest = ::ge1_gs_probe_request;
            probeResolve = ::ge1_gs_probe_resolve_frame;
            probeTake = ::ge1_gs_probe_take;
            probeStats = ::ge1_gs_probe_stats;
            freezeSize = ::ge1_gs_freeze_size; // DS1: static bind (same ABI)
            freezeSave = ::ge1_gs_freeze_save;
            freezeLoad = ::ge1_gs_freeze_load;
            nativeRecord = ::ge1_gs_native_record; // NRT1: needs a GE1 build with the folded adapter
            nativeRecordCompact = ::ge1_gs_native_record_compact; // NRS1: same (compact records)
            nativeRecordKeyed = ::ge1_gs_native_record_keyed;     // RZV1 S4a: same (keyed records)
            staticPrepare = ::ge1_gs_static_prepare;              // RZV1 S4c: same
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
        // PCF1: optional for older GE1 libraries (their periodic policy is unchanged).
        deferCacheFlush = reinterpret_cast<decltype(deferCacheFlush)>(
            dlsym(library, "ge1_gs_set_cache_flush_deferred"));
        // DS1: optional freeze trio (see above); never fails the load.
        freezeSize = reinterpret_cast<decltype(freezeSize)>(dlsym(library, "ge1_gs_freeze_size"));
        freezeSave = reinterpret_cast<decltype(freezeSave)>(dlsym(library, "ge1_gs_freeze_save"));
        freezeLoad = reinterpret_cast<decltype(freezeLoad)>(dlsym(library, "ge1_gs_freeze_load"));
        if (!freezeBound())
            std::fprintf(stderr, "[gs:external] GE1 library predates ge1_gs_freeze_*; live saves refuse\n");
        // LT1b: optional probe quartet (see above); never fails the load.
        probeRequest = reinterpret_cast<decltype(probeRequest)>(dlsym(library, "ge1_gs_probe_request"));
        probeResolve = reinterpret_cast<decltype(probeResolve)>(dlsym(library, "ge1_gs_probe_resolve_frame"));
        probeTake = reinterpret_cast<decltype(probeTake)>(dlsym(library, "ge1_gs_probe_take"));
        probeStats = reinterpret_cast<decltype(probeStats)>(dlsym(library, "ge1_gs_probe_stats"));
        // PT2 Part 2: optional back-thread query (see above); never fails the load.
        backMs = reinterpret_cast<decltype(backMs)>(dlsym(library, "ge1_gs_back_ms"));
        nativeRecord = reinterpret_cast<decltype(nativeRecord)>(dlsym(library, "ge1_gs_native_record"));
        nativeRecordCompact = reinterpret_cast<decltype(nativeRecordCompact)>(
            dlsym(library, "ge1_gs_native_record_compact"));
        nativeRecordKeyed = reinterpret_cast<decltype(nativeRecordKeyed)>(
            dlsym(library, "ge1_gs_native_record_keyed"));
        staticPrepare = reinterpret_cast<decltype(staticPrepare)>(dlsym(library, "ge1_gs_static_prepare"));
#if defined(__ANDROID__)
        // HUD4: optional (see above); never fails the load.
        hudScene = reinterpret_cast<decltype(hudScene)>(dlsym(library, "ge1_gs_hud_scene"));
#endif
        if (!backMs)
            std::fprintf(stderr, "[gs:external] GE1 library predates ge1_gs_back_ms; gsback.busy reads n=0\n");
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

#if defined(__ANDROID__) || defined(PS2X_GE1_STATIC_IOSURFACE)
// TKL1: consume the EE's immutable packet (tick + epoch); never
// touch RDRAM. A stale (pre-load/pre-exit) packet is skipped. Shared by the
// Android AHB call site and the iOS IOSurface call site (THD1).
inline bool trickyHudPacket(ps2_ssx3_tricky_layer::PresentationPacket &p)
{
    if (!ps2_ssx3_tricky_layer::config().hud || !ps2_ssx3_tricky_layer::latestPacket(p))
        return false;
    if (p.epoch != ps2_ssx3_tricky_layer::epoch())
        return false;
    return p.draw && p.atlas;
}
#endif

#if defined(PS2X_GE1_STATIC_IOSURFACE)
// GI1: IOSurface slot pool for the async Metal export. Process-global: the
// surfaces are retained for the process (like createSurface's pool) and the
// completion handler may fire after the backend is destroyed, so the epoch
// stales in-flight exports instead of freeing anything. IX1: the slots take
// the backend's export size (640x480 unless PS2X_GE1_EXPORT_SIZE is set).
// PSO1 Part 3: the ownership pool holds 5 slots (ps2x_present_own::kSharedSlots):
// with 3, CURRENT + RETIRING (~33 ms hold at a 60 Hz presenter) dropped ~40 %
// of 120/s exports. The legacy first-free path (knob 0) still uses only the
// first kLegacySlotCount, unchanged.
constexpr int kIOSurfaceSlotCount = 5;
constexpr int kLegacySlotCount = 3;
static_assert(ps2x_present_own::kSharedSlots <= kIOSurfaceSlotCount, "pool slots need surfaces");

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
    uint64_t submitWallNs; // PL3: steady-clock export-submit time (frame age birth)
    uint64_t ownGen = 0u;  // PSO1: ownership generation (PS2X_PRESENT_OWNERSHIP=1)
    uint64_t ownSeq = 0u;  // PSO1: content sequence, assigned at reserve
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
    const uint32_t width = static_cast<uint32_t>(IOSurfaceGetWidth(ref));
    const uint32_t height = static_cast<uint32_t>(IOSurfaceGetHeight(ref));
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
        std::fprintf(f, "P6\n%u %u\n255\n", width, height);
        std::vector<uint8_t> row(static_cast<size_t>(width) * 3u);
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *src = base + static_cast<size_t>(y) * rowBytes;
            for (uint32_t x = 0; x < width; ++x)
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

// THD1: Tricky HUD composite for the iOS zero-copy path. The export is done
// and the slot is ours (reserved/busy) until publish, so the HUD stamps here,
// after the GPU export completes and before the consumer reads it — the same
// timing point as the Android AHB path's composite-after-waitExport-before-
// queue. Same packet, same stamp math; BGRA lanes (the pool creates 32BGRA
// surfaces). With PS2X_TRICKY_HUD_ASYNC=1 (the same knob as Android) the stamp
// + publish run on the GsHud helper; submit() joins the previous job first,
// so publishes stay ordered with at most one job waiting (backpressure).
// Knob off, the stamp runs inline under the pool mutex. Both run on the Metal
// command-buffer completion thread, always off the GS worker. Overlay failure
// never fails the present: the frame publishes unstamped, as usual.
struct IOHudState
{
    ps2_ssx3_tricky_hud::HudSprites sprites; // pre-scaled art (built once per run)
    uint64_t composites = 0u;
    uint64_t compositeNs = 0u;
    uint64_t lockFails = 0u;
    uint64_t buildFails = 0u;
    bool submitted = false; // a HUD job exists; later publishes must join it
    bool asyncLogged = false;
    ps2_ssx3_tricky_hud::HudCache cache; // HUD2: fused layer (PS2X_TRICKY_HUD_CACHE=1)
    uint64_t cacheFallbacks = 0u;
    HudGlBackend *gl = nullptr; // HUD4 (from HUD3): lazy, owned; process-global like the rest
    uint64_t gpuFallbacks = 0u;
    // HUD4 lane diag (diag knob): real GPU output vs the CPU stamp (BGRA
    // lane) every 30th composite.
    uint64_t diagSeen = 0u;
    uint64_t diagPx = 0u;
    unsigned diagMax = 0u;
};

IOHudState &ioHudState()
{
    static IOHudState st;
    return st;
}

ps2x_gs::SerialJobThread &ioHudThread()
{
    static ps2x_gs::SerialJobThread t{"GsHud"};
    return t;
}

std::mutex &ioHudMutex()
{
    static std::mutex m;
    return m;
}

bool ioHudAsync()
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_TRICKY_HUD_ASYNC");
        return v && std::strcmp(v, "1") == 0;
    }();
    return on;
}

// HUD2: PS2X_TRICKY_HUD_CACHE=1 serves the composite from the fused layer.
bool ioHudCache()
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_TRICKY_HUD_CACHE");
        return v && std::strcmp(v, "1") == 0;
    }();
    return on;
}

// HUD4 (from HUD3): PS2X_TRICKY_HUD_GPU=1 composites on the GPU (EAGL over
// the IOSurface); the CPU stamp stays as the reference and the fallback.
bool ioHudGpu()
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_TRICKY_HUD_GPU");
        return v && std::strcmp(v, "1") == 0;
    }();
    return on;
}

// HUD4 lane diag (diag knob).
bool ioHudDiag()
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_TRICKY_HUD_GPU_DIAG");
        return v && std::strcmp(v, "1") == 0;
    }();
    return on;
}

// The composite proper. Runs on the GsHud helper (async) or on the completion
// thread under the pool mutex (inline); each mode serializes it, so the state
// above needs no lock. Not under the pool mutex in the async case: the slot is
// ours until publish, and the IOSurface lock is the CPU/GPU barrier.
bool stampIOSurfaceHud(void *surface, uint64_t tick,
                       const ps2_ssx3_tricky_layer::PresentationPacket &p)
{
    using namespace ps2_ssx3_tricky_hud;
    IOHudState &st = ioHudState();
    if (!surface)
        return false;
    IOSurfaceRef ref = static_cast<IOSurfaceRef>(surface);
    const uint32_t imgW = static_cast<uint32_t>(IOSurfaceGetWidth(ref));
    const uint32_t imgH = static_cast<uint32_t>(IOSurfaceGetHeight(ref));
    if (imgW == 0u || imgH == 0u)
        return false;
    const ps2_ssx3_tricky_hud::Rect r = hudRegionRect(static_cast<int>(imgW), static_cast<int>(imgH));
    if (r.w <= 0 || r.h <= 0)
        return false;
    // HUD2: cached fused layer (PS2X_TRICKY_HUD_CACHE=1). ensureHudLayer owns
    // its own sprites; on refusal the direct stamp below serves.
    // HUD4 (from HUD3): the GPU path takes precedence over the cache when both are on.
    const bool useCache =
        ioHudCache() && !ioHudGpu() &&
        ensureHudLayer(st.cache, *p.atlas, static_cast<int>(imgW), static_cast<int>(imgH), p.fill,
                       p.full, tick, p.splashUntil, p.litLetters, p.flashUntil);
    if (ioHudCache() && !ioHudGpu() && !useCache && ++st.cacheFallbacks == 1u)
        std::fprintf(stderr, "[ssx3-tricky-hud] io: layer build failed, direct stamp fallback\n");
    // Pre-scaled art, once per run (the export size and atlas are fixed).
    // Skipped when the cached layer serves this composite.
    if (!useCache && (!st.sprites.ok || st.sprites.atlas != p.atlas ||
                      st.sprites.fw != static_cast<int>(imgW) || st.sprites.fh != static_cast<int>(imgH)))
    {
        if (!buildHudSprites(st.sprites, *p.atlas, static_cast<int>(imgW), static_cast<int>(imgH)))
        {
            if (++st.buildFails == 1u)
                std::fprintf(stderr, "[ssx3-tricky-hud] io: sprite build failed, overlay off\n");
            return false;
        }
        std::fprintf(stderr, "[ssx3-tricky-hud] io: sprites built %ux%u\n", imgW, imgH);
    }
    // HUD4 (from HUD3): GPU composite (PS2X_TRICKY_HUD_GPU=1). The sprites
    // above supply the scene rects; any failure falls through to the CPU stamp.
    if (ioHudGpu())
    {
        const ps2_ssx3_tricky_hud::HudVisualKey key =
            visualKeyFor(p.atlas, static_cast<int>(imgW), static_cast<int>(imgH), p.fill, p.full,
                         tick, p.splashUntil, p.litLetters, p.flashUntil);
        const bool wantDiag = ioHudDiag() && (st.diagSeen % 30u) == 0u;
        std::vector<uint8_t> pristine; // LANE DIAG: full pristine frame
        size_t pristineStride = 0u;
        if (wantDiag && IOSurfaceLock(ref, kIOSurfaceLockReadOnly, nullptr) == KERN_SUCCESS)
        {
            const uint8_t *plock = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(ref));
            pristineStride = IOSurfaceGetBytesPerRow(ref);
            if (plock && pristineStride >= static_cast<size_t>(imgW) * 4u)
            {
                pristine.resize(pristineStride * imgH);
                std::memcpy(pristine.data(), plock, pristine.size());
            }
            IOSurfaceUnlock(ref, kIOSurfaceLockReadOnly, nullptr);
        }
        if (!st.gl)
            st.gl = hudGlCreate();
        const bool gpuOk =
            st.gl && hudGlComposite(st.gl, surface, static_cast<int>(imgW),
                                    static_cast<int>(imgH), *p.atlas, st.sprites, key);
        ++st.diagSeen;
        if (gpuOk && wantDiag && !pristine.empty())
        {
            // LANE DIAG: GPU region vs the CPU BGRA stamp of the pristine.
            std::vector<uint8_t> gpuReg(static_cast<size_t>(r.w) * r.h * 4u);
            const size_t rowBytes = static_cast<size_t>(r.w) * 4u;
            if (IOSurfaceLock(ref, kIOSurfaceLockReadOnly, nullptr) == KERN_SUCCESS)
            {
                const uint8_t *glock =
                    static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(ref));
                const size_t rb = IOSurfaceGetBytesPerRow(ref);
                if (glock && rb == pristineStride)
                {
                    for (int y = 0; y < r.h; ++y)
                        std::memcpy(&gpuReg[static_cast<size_t>(y) * rowBytes],
                                    glock + static_cast<size_t>(r.y + y) * rb +
                                        static_cast<size_t>(r.x) * 4u,
                                    rowBytes);
                }
                IOSurfaceUnlock(ref, kIOSurfaceLockReadOnly, nullptr);
            }
            std::vector<uint8_t> cpuFull(static_cast<size_t>(imgW) * imgH * 4u);
            for (uint32_t y = 0; y < imgH; ++y)
                std::memcpy(&cpuFull[static_cast<size_t>(y) * imgW * 4u],
                            pristine.data() + static_cast<size_t>(y) * pristineStride,
                            static_cast<size_t>(imgW) * 4u);
            stampHudDirect<true>(cpuFull.data(), static_cast<size_t>(imgW) * 4u, r, st.sprites,
                                 p.fill, p.full, tick, p.splashUntil, p.litLetters, p.flashUntil);
            std::vector<uint8_t> cpuReg(static_cast<size_t>(r.w) * r.h * 4u);
            for (int y = 0; y < r.h; ++y)
                std::memcpy(&cpuReg[static_cast<size_t>(y) * rowBytes],
                            cpuFull.data() +
                                (static_cast<size_t>(r.y + y) * imgW + static_cast<size_t>(r.x)) *
                                    4u,
                            rowBytes);
            const HudGlCompare cmp = hudGlCompareRegion(cpuReg.data(), gpuReg.data(), r.w, r.h);
            st.diagPx += cmp.diffPx;
            if (cmp.maxErr > st.diagMax)
                st.diagMax = cmp.maxErr;
            std::fprintf(stderr, "[ssx3-tricky-hud] io gpu-diag t=%llu px=%llumax%u (tot %llu max %u)\n",
                         (unsigned long long)tick, (unsigned long long)cmp.diffPx, cmp.maxErr,
                         (unsigned long long)st.diagPx, st.diagMax);
        }
        if (gpuOk)
            return true; // the GPU backend logs its own composites/avg line
        if (++st.gpuFallbacks == 1u)
            std::fprintf(stderr, "[ssx3-tricky-hud] io: GPU composite failed, CPU fallback\n");
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (IOSurfaceLock(ref, 0, nullptr) != KERN_SUCCESS)
    {
        if (++st.lockFails == 1u)
            std::fprintf(stderr, "[ssx3-tricky-hud] io: IOSurfaceLock failed, overlay off\n");
        return false;
    }
    uint8_t *base = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(ref));
    const size_t rowBytes = IOSurfaceGetBytesPerRow(ref);
    if (base && rowBytes >= static_cast<size_t>(imgW) * 4u)
    {
        // HUD2: the cached layer serves from one fused pass when enabled.
        if (useCache)
            stampHudCached<true>(base, rowBytes, r, st.cache.layer);
        else
            stampHudDirect<true>(base, rowBytes, r, st.sprites, p.fill, p.full, tick,
                                 p.splashUntil, p.litLetters, p.flashUntil);
    }
    IOSurfaceUnlock(ref, 0, nullptr);
    const auto t1 = std::chrono::steady_clock::now();
    st.compositeNs +=
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    if (++st.composites == 1u)
        std::fprintf(stderr, "[ssx3-tricky-hud] io: first composite tick=%llu region=%dx%d\n",
                     static_cast<unsigned long long>(tick), r.w, r.h);
    else if (st.composites % 600u == 0u)
        std::fprintf(stderr, "[ssx3-tricky-hud] io: composites=%llu avg=%.1f us\n",
                     static_cast<unsigned long long>(st.composites),
                     static_cast<double>(st.compositeNs) / 1000.0 /
                         static_cast<double>(st.composites));
    return true;
}

struct IOPublish
{
    int slot = -1;
    uint64_t ownGen = 0u;
    uint64_t ownSeq = 0u;
    uint64_t tick = 0u;
    uint64_t submitWallNs = 0u;
    bool ok = false;
    uint64_t epoch = 0u;
};

// The ioExportDone publish tails, extracted verbatim: call with pool.mutex
// held (the dump contract), from the completion thread or the HUD job.
void publishOwnedFrame(const IOPublish &a)
{
    IOSurfacePool &pool = ioPool();
    // PSO1: READY only on success in the current epoch; the slot stays
    // reserved (READY/CURRENT) until the presenter's read retires it.
    const bool live = a.ok && a.epoch == pool.epoch;
    void *surface = pool.surfaces[a.slot];
    IOSurfaceRef ref = static_cast<IOSurfaceRef>(surface);
    ps2x_present_own::FrameInfo info;
    if (live && surface)
    {
        dumpIOSurface(surface, a.tick);
        info = {surface, static_cast<uint32_t>(IOSurfaceGetWidth(ref)),
                static_cast<uint32_t>(IOSurfaceGetHeight(ref)), a.ownSeq, a.tick, a.submitWallNs};
    }
    const bool published = ps2x_present_own::sharedPool().complete(
        a.slot, a.ownGen, live && surface, info, ps2x::perflog::steadyNs());
    if (published)
    {
        static std::once_flag once;
        std::call_once(once, [tick = a.tick] {
            std::fprintf(stderr, "[gs:external] GE1 IOSurface first publish tick=%llu (ownership)\n",
                         static_cast<unsigned long long>(tick));
        });
    }
}

void publishLegacyFrame(const IOPublish &a)
{
    IOSurfacePool &pool = ioPool();
    if (a.ok && a.epoch == pool.epoch)
    {
        void *surface = pool.surfaces[a.slot];
        dumpIOSurface(surface, a.tick);
        IOSurfaceRef ref = static_cast<IOSurfaceRef>(surface);
        ps2x_present_share::publish({surface, static_cast<uint32_t>(IOSurfaceGetWidth(ref)),
                                     static_cast<uint32_t>(IOSurfaceGetHeight(ref)), ++pool.seq,
                                     a.tick, a.submitWallNs, a.slot});
        static std::once_flag once;
        std::call_once(once, [tick = a.tick] {
            std::fprintf(stderr, "[gs:external] GE1 IOSurface first publish tick=%llu\n",
                         static_cast<unsigned long long>(tick));
        });
    }
}

// A HUD publish job: stamp outside the pool mutex (the helper must never
// block the GS worker's reserve), then complete exactly as ioExportDone would.
struct IOHudJob
{
    IOPublish pub;
    bool owned = false;
    ps2_ssx3_tricky_layer::PresentationPacket packet;
};

void runIOHudJob(const IOHudJob &job)
{
    IOSurfacePool &pool = ioPool();
    void *surface = nullptr;
    bool live = false;
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        live = job.pub.ok && job.pub.epoch == pool.epoch;
        surface = pool.surfaces[job.pub.slot];
    }
    // The slot stays reserved/busy until the tail below completes it, so no
    // other export can replace pool.surfaces[slot] under this stamp.
    if (live && surface)
        stampIOSurfaceHud(surface, job.pub.tick, job.packet);
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        if (job.owned)
            publishOwnedFrame(job.pub);
        else
            publishLegacyFrame(job.pub);
    }
    if (!job.owned)
        pool.busy[job.pub.slot].store(false);
}

// Command-buffer completion thread: publish only here (GE2 contract), then
// free the slot. Never touches backend state; the epoch drops stale exports.
void ioExportDone(void *rawCtx, int ok)
{
    std::unique_ptr<IOSurfaceExportCtx> ctx(static_cast<IOSurfaceExportCtx *>(rawCtx));
    // THD1: in async mode every completion takes the HUD mutex, so submits
    // and inline publishes serialize in completion order (the helper's FIFO
    // then publishes in that order). Knob off, this costs nothing.
    std::unique_lock<std::mutex> hudLock(ioHudMutex(), std::defer_lock);
    const bool async = ioHudAsync();
    if (async)
        hudLock.lock();
    const bool owned = ps2x_present_share::ownershipEnabled();
    const IOPublish pub{ctx->slot, ctx->ownGen, ctx->ownSeq,       ctx->tick,
                        ctx->submitWallNs,   ok != 0,       ctx->epoch};
    if (!async)
    {
        // Knob off: today's locking (one pool-mutex critical section), with
        // the stamp inside it so overlapping completions publish in order.
        std::lock_guard<std::mutex> lock(ioPool().mutex);
        const bool live = pub.ok && pub.epoch == ioPool().epoch;
        void *surface = ioPool().surfaces[pub.slot];
        ps2_ssx3_tricky_layer::PresentationPacket p;
        if (live && surface && trickyHudPacket(p))
            stampIOSurfaceHud(surface, pub.tick, p);
        if (owned)
            publishOwnedFrame(pub);
        else
            publishLegacyFrame(pub);
        if (!owned)
            ioPool().busy[pub.slot].store(false);
        return;
    }
    // Async: drain an in-flight HUD job first, so this frame can never
    // publish ahead of an earlier HUD frame's job. Instant unless a job runs.
    if (ioHudState().submitted)
        ioHudThread().join();
    void *surface = nullptr;
    bool live = false;
    {
        std::lock_guard<std::mutex> lock(ioPool().mutex);
        live = pub.ok && pub.epoch == ioPool().epoch;
        surface = ioPool().surfaces[pub.slot];
    }
    ps2_ssx3_tricky_layer::PresentationPacket p;
    if (live && surface && trickyHudPacket(p))
    {
        IOHudState &st = ioHudState();
        if (!st.asyncLogged)
        {
            st.asyncLogged = true;
            std::fprintf(stderr, "[ssx3-tricky-hud] io: THD1 async composite on (GsHud helper)\n");
        }
        st.submitted = true;
        // submit() joins the previous job first: publishes stay ordered and
        // the helper never queues more than one.
        ioHudThread().submit([job = IOHudJob{pub, owned, p}] { runIOHudJob(job); });
        return;
    }
    // No HUD: today's publish.
    {
        std::lock_guard<std::mutex> lock(ioPool().mutex);
        if (owned)
            publishOwnedFrame(pub);
        else
            publishLegacyFrame(pub);
    }
    if (!owned)
        ioPool().busy[pub.slot].store(false);
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
        m_hudThread.stop();
#endif
#if defined(PS2X_GE1_STATIC_IOSURFACE)
        // GI1: stale in-flight completion handlers (they still free their
        // slots, but must not publish after the backend is gone).
        {
            std::lock_guard<std::mutex> lock(ioPool().mutex);
            ++ioPool().epoch;
        }
        if (ps2x_present_share::ownershipEnabled())
            ps2x_present_own::sharedPool().bumpGeneration();
#endif
        ps2_native_world::setStaticPrepare(nullptr); // RZV1 S4c: before GE1 goes
        if (m_ge1Active)
            m_ge1.close();
        m_ge1.unload();
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
#if defined(__ANDROID__) || defined(PS2X_GE1_STATIC_IOSURFACE)
            configureOutputSize();
#endif
            // TEL1: PS2X_GS_TELEMETRY / PS2X_GS_TFX_RECORD set GE1's PW1 env before open.
            ps2x::telemetry::gsBeforeOpen();
            const bool ge1Ok = m_ge1.load(path) && m_ge1.open(4);
            ps2x::telemetry::gsAfterOpen(ge1Ok);
            if (!ge1Ok)
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
            // RZV1 S4c: the MTVU's prepare call goes straight to GE1 (it only
            // reads GE1's published cull state). Not with the per-packet log,
            // which routes records packet by packet.
            if (!m_log)
                ps2_native_world::setStaticPrepare(m_ge1.staticPrepare);
            // AD1: ADPF reuses this accounting, so it turns it on too.
            m_perfTail = ps2x::perflog::enabled() || ps2x::adpf::enabled();
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

    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override
    {
        // UPR1: upstream's CLUT-load hook; the inner CPU backend keeps its
        // shadow VRAM in step, as for Submit.
        if (m_inner)
            m_inner->LoadClut(tex0, texclut);
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
                // FH6: in per-VSync mode GuestVsync already queued this frame
                // while the layer is live. A new window's layer only goes live
                // on its first queue, so until then the latch still exports.
                if ((m_perVsyncLive && ps2x_present_vk::layerLive()) || presentAhb(request.vsyncTick))
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
            const IOExport exported = presentIOSurface(request.vsyncTick);
            if (exported == IOExport::Queued)
            {
                frame.width = 640u;
                frame.height = 480u;
                frame.displayFbp = static_cast<uint32_t>(request.dispfb1 & 0x1ffu);
                frame.sourceFbp = frame.displayFbp;
            }
            else if (exported == IOExport::NoSlot && ps2x_present_share::ownershipEnabled() &&
                     ps2x_present_own::sharedPool().hasFrame())
            {
                // PSO1 (PRV1 §2): pool exhaustion is ordinary backpressure; the
                // presenter keeps its last safe frame (UploadFrame prefers the
                // shared frame anyway), so no synchronous CPU snapshot.
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

    // LT1b: lagF probes go to GE1's ticketed async path when the library has it.
    // The setup stays armed exactly as BeginTransfer left it (a verify-mode sync
    // consume after this call still reads this transfer).
    bool RequestLocalToHostAsync(const GSTransferCommand &command, uint64_t ticket) override
    {
        if (!m_ge1Active || !m_ge1.probeBound())
            return false;
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
        return m_ge1.probeRequest(bitbltbuf, trxpos, trxreg, ticket) == 1;
    }

    void ResolveLocalToHostAsync(uint64_t ticketHi) override
    {
        if (m_ge1Active && m_ge1.probeBound())
            m_ge1.probeResolve(0u, ticketHi);
    }

    uint32_t TakeLocalToHostAsync(uint64_t ticket, uint8_t *dst, uint32_t maxBytes) override
    {
        if (!m_ge1Active || !m_ge1.probeBound())
            return 0u;
        const int n = m_ge1.probeTake(ticket, dst, maxBytes);
        return n > 0 ? static_cast<uint32_t>(n) : 0u;
    }

    bool LocalToHostAsyncStats(uint64_t out[8]) const override
    {
        return m_ge1Active && m_ge1.probeStats && m_ge1.probeStats(out) == 1;
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

    bool RawNativeRecord(uint32_t path, const uint8_t *data, uint32_t sizeBytes) override
    {
        // RZV1 S4a: a keyed record goes whole to the keyed export; without it,
        // its embedded compact record continues below as a compact record.
        if (ge1_is_keyed_record(data, sizeBytes))
        {
            Ge1KeyedParts parts;
            if (!ge1_keyed_record_parts(data, sizeBytes, parts))
                return false; // the frontend's fallback refuses it loudly
            if (m_ge1Active && !m_log && path == 1u && m_ge1.nativeRecordKeyed &&
                m_ge1.nativeRecordKeyed(data, sizeBytes))
            {
                m_ge1LastPath = 1u;
                ge1_compact_record_for_each(parts.compact, parts.compactSize, [this](const uint8_t *, uint32_t n) {
                    ++m_stats.gifPackets;
                    m_stats.gifBytes += n;
                    ++m_stats.gifPacketsByPath[1];
                    m_stats.gifQwords[1] += n / 16u;
                });
                return true;
            }
            data = parts.compact;
            sizeBytes = parts.compactSize;
        }
        // NRS1: compact records need the compact export; without it the
        // frontend expands them to GIF packets.
        const bool compact = ge1_is_compact_record(data, sizeBytes);
        // The per-packet log keeps one line per packet: deliver one by one then.
        if (!m_ge1Active || m_log || path != 1u)
            return false;
        if (compact && !m_ge1.nativeRecordCompact)
            return false;
        if (!compact && !m_ge1.nativeRecord)
            return false;
        const int ok = compact ? m_ge1.nativeRecordCompact(data, sizeBytes)
                               : m_ge1.nativeRecord(data, sizeBytes);
        if (!ok && compact)
        {
            // The vendor refuses malformed compact records fail-closed; the
            // frontend expands them instead of dying here.
            return false;
        }
        if (!ok)
        {
            std::fprintf(stderr, "[gs:external] GE1 native record rejected bytes=%u\n", sizeBytes);
            std::exit(78);
        }
        m_ge1LastPath = 1u;
        auto countPacket = [this](const uint8_t *, uint32_t n) {
            ++m_stats.gifPackets;
            m_stats.gifBytes += n;
            ++m_stats.gifPacketsByPath[1];
            m_stats.gifQwords[1] += n / 16u;
        };
        if (compact)
            ge1_compact_record_for_each(data, sizeBytes, countPacket);
        else
            ge1_native_record_for_each(data, sizeBytes, countPacket);
        return true;
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

    void SetCacheFlushDeferred(bool deferred) override
    {
        if (m_ge1Active && m_ge1.deferCacheFlush)
            m_ge1.deferCacheFlush(deferred ? 1 : 0);
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
            // PT2: gpuMs() is reset-on-read; it feeds the gpu.busy ring. With
            // the ring off the call is skipped; <0 (closed/unsupported)
            // pushes nothing (the stage line reads n=0).
            if (m_perfTail)
            {
                const float gpuMs = m_ge1.gpuMs();
                if (gpuMs >= 0.0f)
                    ps2x::perflog::stageRing(ps2x::perflog::Stage::GpuBusy)
                        .push(static_cast<uint32_t>(tick), gpuMs);
            }
            // PT2 Part 2: back-thread busy per tick (reset-on-read like
            // gpuMs; null on pre-query libraries, <0 with the back thread
            // off — both push nothing and the stage reads n=0).
            if (m_perfTail && m_ge1.backMs)
            {
                const float backMs = m_ge1.backMs();
                if (backMs >= 0.0f)
                {
                    ps2x::perflog::stageRing(ps2x::perflog::Stage::GsBackBusy)
                        .push(static_cast<uint32_t>(tick), backMs);
                    // AD1: the back thread's busy feeds its ADPF session (its
                    // TID is discovered via /proc; the report may come from
                    // this thread).
                    ps2x::adpf::report(ps2x::adpf::Thread::GsBack,
                                       static_cast<uint64_t>(static_cast<double>(backMs) * 1e6));
                }
            }
        }
        ++m_stats.vsyncs;
        ps2x::perflog::noteGsVsync();
#if defined(__ANDROID__)
        // FH6 (PS2X_PRESENT_PER_VSYNC=1, output-only): export and queue every
        // guest frame here, on the worker, right after GE1's VSync, instead of
        // once per host-loop latch RPC (which, behind a saturated GPU, samples
        // ~48 of ~100 guest frames/s). Only once the layer is live; before
        // that the latch path runs as before.
        if (m_ge1Active && presentPerVsync() && ps2x_present_vk::active() && ps2x_present_vk::layerLive() &&
            !ps2x_present_vk::broken())
        {
            m_perVsyncLive = true;
            presentAhb(tick);
        }
#elif defined(PS2X_GE1_STATIC_IOSURFACE)
        // IQ1 (FH6 on iOS, PS2X_PRESENT_PER_VSYNC=1, output-only): export every
        // guest frame into the IOSurface mailbox right after GE1's VSync; the
        // host loop stops latching once it shows a shared frame. A busy pool
        // drops this frame (the next VSync publishes a newer one).
        if (m_ge1Active && iosPresentPerVsync())
            presentIOSurface(tick);
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
        // CN2b: an armed, not yet downloaded GE1 FIFO (SQ3: readback off
        // leaves 768 bytes armed every ~9 ticks mid-race). The freeze blob
        // restores the TRX regs, so a post-load consume reads the same bytes.
        putU32(out, m_ge1FifoBytes);
        out.push_back(m_ge1FifoServed ? 1u : 0u);
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
            ok = takeU32(p, end, version) && (version == 2u || version == kSaveVersion) &&
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
            uint32_t ge1FifoBytes = 0u;
            uint8_t ge1FifoServed = 0u;
            if (ok)
            {
                fifo.assign(p, p + static_cast<size_t>(fifoSize));
                p += static_cast<size_t>(fifoSize);
                if (version >= 3u)
                    ok = takeU32(p, end, ge1FifoBytes) && end - p >= 1 &&
                         (ge1FifoServed = *p++, ge1FifoServed <= 1u);
            }
            if (ok)
            {
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
                m_ge1FifoBytes = ge1FifoBytes;
                m_ge1FifoServed = ge1FifoServed != 0u;
                // GL1: GSfreeze carries VRAM and GIF state, not the privileged
                // registers (PCSX2 saves PS2MEM_GS on the EE side). The guest
                // writes PMODE/DISPFB/DISPLAY once at boot, so the mirror never
                // re-sends them after a load: push the restored image into the
                // lib, or it keeps its pre-load values (PMODE=0 at startup:
                // every frame black).
                if (m_ge1Active)
                    for (size_t i = 0; i < m_mirror.size(); ++i)
                        if (!m_ge1.privWrite(kMirrorOffsets[i], m_mirror[i]))
                            ok = false;
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

    static bool presentPerVsync()
    {
        static const bool on = [] {
            const char *v = std::getenv("PS2X_PRESENT_PER_VSYNC");
            return v && std::strcmp(v, "1") == 0;
        }();
        return on;
    }
#endif

#if defined(__ANDROID__) || defined(PS2X_GE1_STATIC_IOSURFACE)
    // UR1: the GE1 output size. The GS opens before raylib's window, so the
    // panel size comes from the display itself (Android:
    // ps2x_present_vk::panelSize, Display.getRealSize; IX1 iOS:
    // ps2x::ios::panelSize, UIScreen.nativeBounds; both landscape). It is
    // exported as GE1_DISPLAY_SIZE (unless set) so GE1_UPSCALE=native|halfnative
    // resolves to its height / 448: the panel on Android (the Odin's 16:9 panel
    // is the game rect), the game rect on iOS (the iPad's 1640 px panel height
    // is not the 16:9 rect's 1328). PS2X_GE1_EXPORT_SIZE=WxH|display sizes the
    // buffers GE1 draws into (Android AHBs, iOS IOSurfaces): display = the game
    // rect on the panel (panel height x the present aspect), so GE1 does the
    // one final scale (with PS2X_PRESENT_FILTER=sharp if set) and the
    // compositor (SurfaceFlinger; iOS: the host draw) shows it 1:1. Unset =
    // 640x480, the compositor scales.
    void configureOutputSize()
    {
        int pw = 0, ph = 0;
#if defined(__ANDROID__)
        const bool panel = ps2x_present_vk::panelSize(pw, ph);
#else
        const bool panel = ps2x::ios::panelSize(pw, ph);
#endif
        ps2x::present::Rect game{0.0f, 0.0f, 0.0f, 0.0f};
        if (panel)
        {
            const ps2x::present::Aspect aspect = ps2x::present::aspectFromEnv(
                std::getenv("PS2X_ASPECT"), ps2x::present::ssx3WidescreenModeFromEnv(std::getenv("PS2X_WIDESCREEN")) != 0u);
            game = ps2x::present::presentRect(static_cast<float>(pw), static_cast<float>(ph), 640.0f, 448.0f, aspect);
        }
        const unsigned gameW = static_cast<unsigned>(game.w + 0.5f), gameH = static_cast<unsigned>(game.h + 0.5f);
        if (panel && !std::getenv("GE1_DISPLAY_SIZE"))
        {
#if defined(__ANDROID__)
            const std::string v = std::to_string(pw) + "x" + std::to_string(ph);
#else
            const std::string v = std::to_string(gameW) + "x" + std::to_string(gameH);
#endif
            setenv("GE1_DISPLAY_SIZE", v.c_str(), 0);
        }
        const char *size = std::getenv("PS2X_GE1_EXPORT_SIZE");
        if (!size || !*size)
            return;
        unsigned w = 0, h = 0;
        if (std::strcmp(size, "display") == 0)
        {
            w = gameW;
            h = gameH;
        }
        else if (std::sscanf(size, "%ux%u", &w, &h) != 2)
        {
            w = h = 0;
        }
        if (w >= 64u && h >= 64u && w <= 4096u && h <= 4096u)
        {
            m_exportW = w;
            m_exportH = h;
        }
#if defined(__ANDROID__)
        // OUT1 (c): the AHB pool is panel-oriented (portrait); GE1 writes the
        // stretch pre-rotated into it. HUD scene builders still run against
        // the unrotated (landscape) dims (see compositeTrickyHudGpu).
        if (m_exportW && m_exportH && ps2x_present_vk::prerotate() != 0)
            std::swap(m_exportW, m_exportH);
#endif
        // IX1: GE1 keeps PCSX2's auto 4:3 unless a UR1 knob is set, and would
        // pillarbox a 4:3 picture inside a 16:9 export. Any UR1 knob switches it
        // to Stretch (the runtime owns the aspect); GE1_SNAPSHOT_SIZE=640x480
        // is the one that changes nothing else (the snapshot default size).
        if ((m_exportW != 640u || m_exportH != 480u) && !std::getenv("GE1_SNAPSHOT_SIZE"))
            setenv("GE1_SNAPSHOT_SIZE", "640x480", 0);
        std::fprintf(stderr, "[gs:external] UR1 panel=%dx%d GE1_DISPLAY_SIZE=%s export=%ux%u (PS2X_GE1_EXPORT_SIZE=%s)\n",
                     pw, ph, std::getenv("GE1_DISPLAY_SIZE") ? std::getenv("GE1_DISPLAY_SIZE") : "unset", m_exportW,
                     m_exportH, size);
    }
#endif

#if defined(__ANDROID__)
    // GSW1 (PS2X_TRICKY_HUD_ASYNC=1): wait for the helper's previous
    // composite + queue. False when that queue was refused (it already fell
    // back), as the inline path reports its own refused queue.
    bool joinHudJob()
    {
        if (m_hudInFlight < 0)
            return true;
        m_hudThread.join();
        m_hudInFlight = -1;
        return m_hudJobQueued;
    }

    bool queuePendingAhb()
    {
        const bool prevQueued = joinHudJob();
        if (m_pendingAhb < 0)
            return prevQueued;
        m_ge1.waitExport(m_pendingFence);
        AhbSlot &slot = m_ahbSlots[static_cast<size_t>(m_pendingAhb)];
        // HUD4: a GPU-served slot skips every stamp path (its fence already
        // covers the composite); the diag compare (if stashed) runs here.
        if (m_pendingGpu)
        {
            if (m_hudDiagPending.armed)
                hudGpuDiagCompare(slot.buffer, m_hudDiagPending, m_hudDiagPx, m_hudDiagMax, "");
            const bool gpuQueued = ps2x_present_vk::queue(slot.id, m_exportW, m_exportH);
            m_pendingAhb = -1;
            m_pendingFence = 0u;
            m_pendingGpu = false;
            if (!gpuQueued)
                ps2x_present_vk::fallBack("GE1 AHB queue failed");
            return gpuQueued;
        }
        if (m_hudAsync)
        {
            // GSW1: the packet is read here, where the inline path reads it;
            // the helper stamps the same buffer with the same packet and tick
            // and queues it. presentAhb skips this slot until the next join,
            // and the next present joins before it queues, so buffers still
            // queue in order and are never picked while the helper owns them.
            ps2_ssx3_tricky_layer::PresentationPacket p;
            if (trickyHudPacket(p))
            {
                const AhbSlot job = slot;
                const uint32_t w = m_exportW, h = m_exportH;
                const uint64_t tick = m_pendingTick;
                if (!m_hudAsyncLogged)
                {
                    m_hudAsyncLogged = true;
                    std::fprintf(stderr, "[ssx3-tricky-hud] vk: GSW1 async composite on (GsHud helper)\n");
                }
                m_hudInFlight = m_pendingAhb;
                m_pendingAhb = -1;
                m_pendingFence = 0u;
                m_hudThread.submit([this, job, w, h, tick, p] {
                    compositeTrickyHudAhbAsync(job.buffer, w, h, tick, p);
                    m_hudJobQueued = ps2x_present_vk::queue(job.id, w, h);
                    if (!m_hudJobQueued)
                        ps2x_present_vk::fallBack("GE1 AHB queue failed");
                });
                return prevQueued;
            }
        }
        else
        {
            // TK43e: the export is done and the buffer is not yet queued, so
            // the slot is ours: composite the TRICKY meter before queueing.
            compositeTrickyHudAhb(slot.buffer, m_exportW, m_exportH, m_pendingTick);
        }
        const bool queued = ps2x_present_vk::queue(slot.id, m_exportW, m_exportH);
        m_pendingAhb = -1;
        m_pendingFence = 0u;
        if (!queued)
            ps2x_present_vk::fallBack("GE1 AHB queue failed");
        return queued;
    }

    // TK43e: the VK/AHB overlay call site (GS worker). The HUD region is
    // copied out of the locked AHB, composed on the CPU (the same draw list
    // as the GL path) and copied back. Overlay failure NEVER fails the
    // present: it skips the composite and the frame queues as usual.
    void compositeTrickyHudAhb(AHardwareBuffer *buffer, uint32_t imgW, uint32_t imgH, uint64_t tick)
    {
        ps2_ssx3_tricky_layer::PresentationPacket p;
        if (!trickyHudPacket(p))
            return;
        stampTrickyHudAhb(buffer, imgW, imgH, tick, p);
    }

    // The composite proper. With PS2X_TRICKY_HUD_ASYNC=1 it runs on the GsHud
    // helper, which then owns m_hudSprites and the m_hud* counters.
    void stampTrickyHudAhb(AHardwareBuffer *buffer, uint32_t imgW, uint32_t imgH, uint64_t tick,
                           const ps2_ssx3_tricky_layer::PresentationPacket &p)
    {
        using namespace ps2_ssx3_tricky_hud;
        if (!buffer || imgW == 0u || imgH == 0u)
            return;
        // OUT1 (c): the scene builds against the unrotated (landscape) dims;
        // the AHB itself stays portrait.
        const int pro = ps2x_present_vk::prerotate();
        const int sceneW = pro ? static_cast<int>(imgH) : static_cast<int>(imgW);
        const int sceneH = pro ? static_cast<int>(imgW) : static_cast<int>(imgH);
        const ps2_ssx3_tricky_hud::Rect r = hudRegionRect(sceneW, sceneH);
        if (r.w <= 0 || r.h <= 0)
            return;
        // HUD2: cached fused layer (PS2X_TRICKY_HUD_CACHE=1). ensureHudLayer
        // owns its own sprites; on refusal the direct stamp below serves.
        // HUD4: the GPU composite (if any) already ran at export time; a
        // GPU-served slot never reaches this stamp (queuePendingAhb skips
        // it), so the cache arbitration below is CPU-only, as before.
        // OUT1 (c): the fused layer stays in frame coords; the rotated CPU
        // path (fallback-only: GPU serves play) stamps direct.
        const bool useCache =
            !pro && m_hudCache &&
            ensureHudLayer(m_hudCacheState, *p.atlas, sceneW, sceneH, p.fill, p.full, tick,
                           p.splashUntil, p.litLetters, p.flashUntil);
        if (m_hudCache && !useCache && !pro && ++m_hudCacheFallbacks == 1u)
            std::fprintf(stderr, "[ssx3-tricky-hud] vk: layer build failed, direct stamp fallback\n");
        // Pre-scaled art, once per run (the export size and atlas are fixed).
        // Skipped when the cached layer serves this composite.
        if (!useCache && (!m_hudSprites.ok || m_hudSprites.atlas != p.atlas ||
                          m_hudSprites.fw != sceneW || m_hudSprites.fh != sceneH))
        {
            if (!buildHudSprites(m_hudSprites, *p.atlas, sceneW, sceneH))
            {
                if (++m_hudBuildFails == 1u)
                    std::fprintf(stderr, "[ssx3-tricky-hud] vk: sprite build failed, overlay off\n");
                return;
            }
            std::fprintf(stderr, "[ssx3-tricky-hud] vk: sprites built %dx%d\n", sceneW, sceneH);
        }
        // OUT1 (c): rotated stamp via a region temp (portrait pristine in,
        // unrotated stamp, portrait composited out).
        if (pro)
        {
            stampTrickyHudAhbRotated(buffer, imgW, imgH, tick, p, r, m_hudSprites, sceneW, sceneH,
                                     pro);
            return;
        }
        AHardwareBuffer_Desc desc = {};
        AHardwareBuffer_describe(buffer, &desc);
        if (desc.stride < imgW)
            return;
        const auto t0 = std::chrono::steady_clock::now();
        void *ptr = nullptr;
        // Region-scoped lock (the NDK returns the buffer base either way;
        // the rect scopes the write-back, and the copy is stride-aware).
        const ARect lockRect{r.x, r.y, r.x + r.w, r.y + r.h};
        if (AHardwareBuffer_lock(buffer,
                                 AHARDWAREBUFFER_USAGE_CPU_READ_RARELY |
                                     AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY,
                                 -1, &lockRect, &ptr) != 0 ||
            !ptr)
        {
            if (++m_hudLockFails == 1u)
                std::fprintf(stderr, "[ssx3-tricky-hud] vk: AHardwareBuffer_lock failed, overlay off\n");
            return;
        }
        const size_t strideBytes = static_cast<size_t>(desc.stride) * 4u;
        // Part 2: stamp straight into the locked AHB (no region temp
        // round-trip; same values as the temp path, locked by test).
        // HUD2: the cached layer serves from one fused pass when enabled.
        if (useCache)
            stampHudCached(static_cast<uint8_t *>(ptr), strideBytes, r, m_hudCacheState.layer);
        else
            stampHudDirect(static_cast<uint8_t *>(ptr), strideBytes, r, m_hudSprites, p.fill, p.full,
                           tick, p.splashUntil, p.litLetters, p.flashUntil);
        AHardwareBuffer_unlock(buffer, nullptr);
        const auto t1 = std::chrono::steady_clock::now();
        m_hudCompositeNs +=
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        if (++m_hudComposites == 1u)
            std::fprintf(stderr, "[ssx3-tricky-hud] vk: first composite tick=%llu region=%dx%d\n",
                         static_cast<unsigned long long>(tick), r.w, r.h);
        else if (m_hudComposites % 600u == 0u)
            std::fprintf(stderr, "[ssx3-tricky-hud] vk: composites=%llu avg=%.1f us\n",
                         static_cast<unsigned long long>(m_hudComposites),
                         static_cast<double>(m_hudCompositeNs) / 1000.0 /
                             static_cast<double>(m_hudComposites));
    }

    // OUT1 (c): shift a built sprite set from frame coords to region-local
    // coords (subtract the region origin from every dest). The stamp then runs
    // against a region-sized temp with r0 = {0, 0, w, h} and computes
    // identical values (same ints: pre-shifted dst minus 0).
    static ps2_ssx3_tricky_hud::HudSprites shiftHudSpritesToRegion(
        const ps2_ssx3_tricky_hud::HudSprites &ss, int ox, int oy)
    {
        using namespace ps2_ssx3_tricky_hud;
        HudSprites out = ss;
        auto sh = [ox, oy](Rect &rc) {
            rc.x -= ox;
            rc.y -= oy;
        };
        sh(out.region);
        sh(out.coilCover);
        sh(out.poleDst);
        for (int i = 0; i < kCoils; ++i)
            sh(out.ringDst[i]);
        sh(out.jewelDst);
        out.smearX0 -= ox;
        out.smearY0 -= oy;
        out.smearX1 -= ox;
        out.smearY1 -= oy;
        for (int i = 0; i < 6; ++i)
            sh(out.archDst[i]);
        sh(out.pillDst);
        sh(out.splashDst);
        return out;
    }

    // OUT1 (c): the rotated CPU stamp (fallback-only: GPU serves play). The
    // portrait pristine region un-transposes into a temp, the unrotated stamp
    // runs against it with region-local sprites, and the composited region
    // transposes back. r is the unrotated region; the AHB is portrait.
    void stampTrickyHudAhbRotated(AHardwareBuffer *buffer, uint32_t imgW, uint32_t imgH,
                                  uint64_t tick,
                                  const ps2_ssx3_tricky_layer::PresentationPacket &p,
                                  const ps2_ssx3_tricky_hud::Rect &r,
                                  const ps2_ssx3_tricky_hud::HudSprites &sprites, int sceneW,
                                  int sceneH, int sense)
    {
        using namespace ps2_ssx3_tricky_hud;
        const ps2x_present_vk::PrerotateRect pr =
            ps2x_present_vk::prerotateRegion(sense, r.x, r.y, r.w, r.h, sceneW, sceneH);
        if (pr.x < 0 || pr.y < 0 || pr.w != r.h || pr.h != r.w ||
            pr.x + pr.w > static_cast<int>(imgW) || pr.y + pr.h > static_cast<int>(imgH))
            return;
        AHardwareBuffer_Desc desc = {};
        AHardwareBuffer_describe(buffer, &desc);
        if (desc.stride < imgW)
            return;
        const auto t0 = std::chrono::steady_clock::now();
        void *ptr = nullptr;
        const ARect lockRect{pr.x, pr.y, pr.x + pr.w, pr.y + pr.h};
        if (AHardwareBuffer_lock(buffer,
                                 AHARDWAREBUFFER_USAGE_CPU_READ_RARELY |
                                     AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY,
                                 -1, &lockRect, &ptr) != 0 ||
            !ptr)
        {
            if (++m_hudLockFails == 1u)
                std::fprintf(stderr, "[ssx3-tricky-hud] vk: AHardwareBuffer_lock failed, overlay off\n");
            return;
        }
        const size_t strideBytes = static_cast<size_t>(desc.stride) * 4u;
        const uint8_t *ahb = static_cast<const uint8_t *>(ptr);
        std::vector<uint8_t> temp(static_cast<size_t>(r.w) * r.h * 4u);
        for (int y = 0; y < r.h; ++y)
        {
            for (int x = 0; x < r.w; ++x)
            {
                const ps2x_present_vk::PrerotatePos pp =
                    ps2x_present_vk::prerotateMap(sense, x, y, r.w, r.h, pr.x, pr.y);
                std::memcpy(&temp[(static_cast<size_t>(y) * r.w + x) * 4u],
                            ahb + static_cast<size_t>(pp.y) * strideBytes +
                                static_cast<size_t>(pp.x) * 4u,
                            4u);
            }
        }
        const HudSprites shifted = shiftHudSpritesToRegion(sprites, r.x, r.y);
        const Rect r0{0, 0, r.w, r.h};
        stampHudDirect(temp.data(), static_cast<size_t>(r.w) * 4u, r0, shifted, p.fill, p.full,
                       tick, p.splashUntil, p.litLetters, p.flashUntil);
        uint8_t *ahbW = static_cast<uint8_t *>(ptr);
        for (int y = 0; y < r.h; ++y)
        {
            for (int x = 0; x < r.w; ++x)
            {
                const ps2x_present_vk::PrerotatePos pp =
                    ps2x_present_vk::prerotateMap(sense, x, y, r.w, r.h, pr.x, pr.y);
                std::memcpy(ahbW + static_cast<size_t>(pp.y) * strideBytes +
                                static_cast<size_t>(pp.x) * 4u,
                            &temp[(static_cast<size_t>(y) * r.w + x) * 4u], 4u);
            }
        }
        AHardwareBuffer_unlock(buffer, nullptr);
        const auto t1 = std::chrono::steady_clock::now();
        m_hudCompositeNs +=
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        if (++m_hudComposites == 1u)
            std::fprintf(stderr,
                         "[ssx3-tricky-hud] vk: first composite tick=%llu region=%dx%d (rotated)\n",
                         static_cast<unsigned long long>(tick), r.w, r.h);
        else if (m_hudComposites % 600u == 0u)
            std::fprintf(stderr, "[ssx3-tricky-hud] vk: composites=%llu avg=%.1f us (rotated)\n",
                         static_cast<unsigned long long>(m_hudComposites),
                         static_cast<double>(m_hudCompositeNs) / 1000.0 /
                             static_cast<double>(m_hudComposites));
    }

    // HUD4: fill the GE1 scene blob from a built scene. Field order mirrors
    // Ge1HudScene (ge1_gs_api.h); the adapter + vendor check magic + size.
    static bool fillGe1HudScene(Ge1HudScene &out, const ps2_ssx3_tricky_hud::HudScene &sc)
    {
        using namespace ps2_ssx3_tricky_hud;
        std::memset(&out, 0, sizeof(out));
        if (!sc.ok || sc.nsmears < 0 || sc.nsmears > 2 || sc.nquads <= 0 ||
            sc.nquads > GE1_HUD_SCENE_MAX_QUADS)
            return false;
        out.magic = GE1_HUD_SCENE_MAGIC;
        out.version = GE1_HUD_SCENE_VERSION;
        out.regionX = sc.region.x;
        out.regionY = sc.region.y;
        out.regionW = sc.region.w;
        out.regionH = sc.region.h;
        out.nsmears = sc.nsmears;
        for (int i = 0; i < sc.nsmears; ++i)
        {
            out.smearX0[i] = sc.smears[i].x0;
            out.smearY0[i] = sc.smears[i].y0;
            out.smearX1[i] = sc.smears[i].x1;
            out.smearY1[i] = sc.smears[i].y1;
            out.smearLX[i] = sc.smears[i].lx;
            out.smearRX[i] = sc.smears[i].rx;
        }
        out.nquads = sc.nquads;
        for (int i = 0; i < sc.nquads; ++i)
        {
            out.quadSrcX[i] = sc.quads[i].src.x;
            out.quadSrcY[i] = sc.quads[i].src.y;
            out.quadSrcW[i] = sc.quads[i].src.w;
            out.quadSrcH[i] = sc.quads[i].src.h;
            out.quadDX[i] = sc.quads[i].dx;
            out.quadDY[i] = sc.quads[i].dy;
            out.quadDW[i] = sc.quads[i].dw;
            out.quadDH[i] = sc.quads[i].dh;
            out.quadDim[i] = sc.quads[i].dim;
        }
        return true;
    }

    // HUD4: composite at export time (GS worker, back-to-back with exportAhb
    // in presentAhb). The scene submits right behind the export copy on
    // GE1's queue; *fence takes the composite fence (which covers the copy).
    // True = GPU-served (the stamp site skips this slot); false = the CPU
    // stamp serves it as before. Any failure falls back silently after the
    // first log line; the overlay never drops.
    bool compositeTrickyHudGpu(AHardwareBuffer *buffer, uint32_t imgW, uint32_t imgH,
                               uint64_t tick, uint64_t exportFence, uint64_t *fence)
    {
        using namespace ps2_ssx3_tricky_hud;
        if (!m_hudGpu || !m_ge1.hudScene || !buffer || imgW == 0u || imgH == 0u || !fence)
            return false;
        ps2_ssx3_tricky_layer::PresentationPacket p;
        if (!trickyHudPacket(p))
            return false;
        // OUT1 (c): the scene builds against the unrotated (landscape) dims;
        // GE1 transposes the composite into the portrait AHB.
        const int pro = ps2x_present_vk::prerotate();
        const int sceneW = pro ? static_cast<int>(imgH) : static_cast<int>(imgW);
        const int sceneH = pro ? static_cast<int>(imgW) : static_cast<int>(imgH);
        const Rect r = hudRegionRect(sceneW, sceneH);
        if (r.w <= 0 || r.h <= 0 || !p.atlas || !p.atlas->ok)
            return false;
        // GS-worker-owned sprites (the GsHud helper owns m_hudSprites for the
        // CPU path; the two never share: see the member comment).
        if (!m_hudGpuSprites.ok || m_hudGpuSprites.atlas != p.atlas ||
            m_hudGpuSprites.fw != sceneW || m_hudGpuSprites.fh != sceneH)
        {
            if (!buildHudSprites(m_hudGpuSprites, *p.atlas, sceneW, sceneH))
            {
                if (++m_hudGpuFallbacks == 1u)
                    std::fprintf(stderr,
                                 "[ssx3-tricky-hud] vk: GPU sprite build failed, CPU fallback\n");
                return false;
            }
            std::fprintf(stderr, "[ssx3-tricky-hud] vk: GPU sprites built %dx%d\n", sceneW, sceneH);
        }
        const HudVisualKey key = visualKeyFor(p.atlas, sceneW, sceneH, p.fill, p.full, tick,
                                              p.splashUntil, p.litLetters, p.flashUntil);
        HudScene sc;
        if (!buildHudScene(sc, m_hudGpuSprites, key))
        {
            if (++m_hudGpuFallbacks == 1u)
                std::fprintf(stderr,
                             "[ssx3-tricky-hud] vk: GPU scene build failed, CPU fallback\n");
            return false;
        }
        Ge1HudScene abi;
        if (!fillGe1HudScene(abi, sc))
        {
            if (++m_hudGpuFallbacks == 1u)
                std::fprintf(stderr, "[ssx3-tricky-hud] vk: GPU scene fill failed, CPU fallback\n");
            return false;
        }
        ++m_hudGpuSeen;
        // LANE DIAG (HUD4, PS2X_TRICKY_HUD_GPU_DIAG=1): stash the pristine
        // region for the queue-time compare. The extra wait + lock exist only
        // with the diag knob on; the play path submits straight through.
        const bool wantDiag = m_hudDiag && (m_hudGpuSeen % 30u) == 0u;
        m_hudDiagPending = HudDiagPending{};
        if (wantDiag)
        {
            AHardwareBuffer_Desc dd = {};
            AHardwareBuffer_describe(buffer, &dd);
            void *plock = nullptr;
            if (exportFence != 0u)
                m_ge1.waitExport(exportFence);
            if (dd.stride >= imgW &&
                AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr,
                                     &plock) == 0 &&
                plock)
            {
                m_hudDiagPending.stride = static_cast<size_t>(dd.stride) * 4u;
                m_hudDiagPending.frame.resize(m_hudDiagPending.stride * imgH);
                std::memcpy(m_hudDiagPending.frame.data(), plock,
                            m_hudDiagPending.frame.size());
                AHardwareBuffer_unlock(buffer, nullptr);
                m_hudDiagPending.armed = true;
                m_hudDiagPending.packet = p;
                m_hudDiagPending.sprites = m_hudGpuSprites;
                m_hudDiagPending.tick = tick;
                // OUT1 (c): the stash frame stays portrait; fw/fh are the
                // unrotated scene dims the compare builds against.
                m_hudDiagPending.fw = sceneW;
                m_hudDiagPending.fh = sceneH;
            }
        }
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t compositeFence = 0u;
        const int rc = m_ge1.hudScene(
            buffer, &abi, p.atlas->rgba.data(), static_cast<uint32_t>(p.atlas->w),
            static_cast<uint32_t>(p.atlas->h), static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p.atlas)),
            &compositeFence);
        const auto t1 = std::chrono::steady_clock::now();
        if (rc != 1 || compositeFence == 0u)
        {
            m_hudDiagPending = HudDiagPending{};
            if (++m_hudGpuFallbacks == 1u)
                std::fprintf(stderr, "[ssx3-tricky-hud] vk: GPU composite failed, CPU fallback\n");
            return false;
        }
        *fence = compositeFence;
        m_hudGpuCompositeNs +=
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        if (++m_hudGpuServed == 1u)
            std::fprintf(stderr,
                         "[ssx3-tricky-hud] vk: first GPU composite tick=%llu region=%dx%d\n",
                         static_cast<unsigned long long>(tick), r.w, r.h);
        else if (m_hudGpuServed % 600u == 0u)
            std::fprintf(stderr, "[ssx3-tricky-hud] vk: GPU composites=%llu avg=%.1f us\n",
                         static_cast<unsigned long long>(m_hudGpuServed),
                         static_cast<double>(m_hudGpuCompositeNs) / 1000.0 /
                             static_cast<double>(m_hudGpuServed));
        return true;
    }

    // HUD4 Part 2: helper-side GPU composite (m_hudAsync=1). Runs on the GsHud
    // helper AFTER the worker's export wait, so the AHB is stable and no wait
    // is needed before the pristine stash; submits through ge1_gs_hud_scene,
    // waits its own fence, and returns with the slot composited. True =
    // GPU-served (skip the stamp); false = the CPU stamp fallback serves.
    // The GS worker never waits on the helper's fence. State here is
    // helper-owned (the m_hudGpuAsync* members); the inline path above keeps
    // its worker-owned twins.
    bool submitTrickyHudGpuAsync(AHardwareBuffer *buffer, uint32_t imgW, uint32_t imgH,
                                 uint64_t tick,
                                 const ps2_ssx3_tricky_layer::PresentationPacket &p)
    {
        using namespace ps2_ssx3_tricky_hud;
        if (!m_hudGpu || !m_ge1.hudScene || !buffer || imgW == 0u || imgH == 0u)
            return false;
        // OUT1 (c): unrotated scene dims; the AHB stays portrait.
        const int pro = ps2x_present_vk::prerotate();
        const int sceneW = pro ? static_cast<int>(imgH) : static_cast<int>(imgW);
        const int sceneH = pro ? static_cast<int>(imgW) : static_cast<int>(imgH);
        const Rect r = hudRegionRect(sceneW, sceneH);
        if (r.w <= 0 || r.h <= 0 || !p.atlas || !p.atlas->ok)
            return false;
        if (!m_hudGpuAsyncSprites.ok || m_hudGpuAsyncSprites.atlas != p.atlas ||
            m_hudGpuAsyncSprites.fw != sceneW || m_hudGpuAsyncSprites.fh != sceneH)
        {
            if (!buildHudSprites(m_hudGpuAsyncSprites, *p.atlas, sceneW, sceneH))
            {
                if (++m_hudGpuAsyncFallbacks == 1u)
                    std::fprintf(stderr, "[ssx3-tricky-hud] vk: GPU sprite build failed, "
                                         "CPU fallback (async)\n");
                return false;
            }
            std::fprintf(stderr, "[ssx3-tricky-hud] vk: GPU sprites built %dx%d (async)\n",
                         sceneW, sceneH);
        }
        const HudVisualKey key = visualKeyFor(p.atlas, sceneW, sceneH, p.fill, p.full, tick,
                                              p.splashUntil, p.litLetters, p.flashUntil);
        HudScene sc;
        if (!buildHudScene(sc, m_hudGpuAsyncSprites, key))
        {
            if (++m_hudGpuAsyncFallbacks == 1u)
                std::fprintf(stderr,
                             "[ssx3-tricky-hud] vk: GPU scene build failed, CPU fallback (async)\n");
            return false;
        }
        Ge1HudScene abi;
        if (!fillGe1HudScene(abi, sc))
        {
            if (++m_hudGpuAsyncFallbacks == 1u)
                std::fprintf(stderr,
                             "[ssx3-tricky-hud] vk: GPU scene fill failed, CPU fallback (async)\n");
            return false;
        }
        ++m_hudGpuAsyncSeen;
        // LANE DIAG (PS2X_TRICKY_HUD_GPU_DIAG=1): stash the pristine frame for
        // the compare below. The worker already waited the export fence, so no
        // wait is needed here; the play path (diag off) skips this entirely.
        const bool wantDiag = m_hudDiag && (m_hudGpuAsyncSeen % 30u) == 0u;
        m_hudGpuAsyncDiag = HudDiagPending{};
        if (wantDiag)
        {
            AHardwareBuffer_Desc dd = {};
            AHardwareBuffer_describe(buffer, &dd);
            void *plock = nullptr;
            if (dd.stride >= imgW &&
                AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr,
                                     &plock) == 0 &&
                plock)
            {
                m_hudGpuAsyncDiag.stride = static_cast<size_t>(dd.stride) * 4u;
                m_hudGpuAsyncDiag.frame.resize(m_hudGpuAsyncDiag.stride * imgH);
                std::memcpy(m_hudGpuAsyncDiag.frame.data(), plock, m_hudGpuAsyncDiag.frame.size());
                AHardwareBuffer_unlock(buffer, nullptr);
                m_hudGpuAsyncDiag.armed = true;
                m_hudGpuAsyncDiag.packet = p;
                m_hudGpuAsyncDiag.sprites = m_hudGpuAsyncSprites;
                m_hudGpuAsyncDiag.tick = tick;
                // OUT1 (c): portrait stash, unrotated scene dims (see above).
                m_hudGpuAsyncDiag.fw = sceneW;
                m_hudGpuAsyncDiag.fh = sceneH;
            }
        }
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t compositeFence = 0u;
        const int rc = m_ge1.hudScene(
            buffer, &abi, p.atlas->rgba.data(), static_cast<uint32_t>(p.atlas->w),
            static_cast<uint32_t>(p.atlas->h), static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p.atlas)),
            &compositeFence);
        // The helper waits its own fence here; the GS worker never waits on it.
        if (rc == 1 && compositeFence != 0u)
            m_ge1.waitExport(compositeFence);
        const auto t1 = std::chrono::steady_clock::now();
        if (rc != 1 || compositeFence == 0u)
        {
            m_hudGpuAsyncDiag = HudDiagPending{};
            if (++m_hudGpuAsyncFallbacks == 1u)
                std::fprintf(stderr,
                             "[ssx3-tricky-hud] vk: GPU composite failed, CPU fallback (async)\n");
            return false;
        }
        // Timed submit+wait: the helper's per-frame cost, comparable to the CPU stamp's
        // lock-to-unlock average on the same thread.
        m_hudGpuAsyncCompositeNs +=
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        if (++m_hudGpuAsyncServed == 1u)
            std::fprintf(stderr,
                         "[ssx3-tricky-hud] vk: first GPU composite tick=%llu region=%dx%d (async)\n",
                         static_cast<unsigned long long>(tick), r.w, r.h);
        else if (m_hudGpuAsyncServed % 600u == 0u)
            std::fprintf(stderr, "[ssx3-tricky-hud] vk: GPU composites=%llu avg=%.1f us (async)\n",
                         static_cast<unsigned long long>(m_hudGpuAsyncServed),
                         static_cast<double>(m_hudGpuAsyncCompositeNs) / 1000.0 /
                             static_cast<double>(m_hudGpuAsyncServed));
        if (m_hudGpuAsyncDiag.armed)
            hudGpuDiagCompare(buffer, m_hudGpuAsyncDiag, m_hudGpuAsyncDiagPx, m_hudGpuAsyncDiagMax,
                              " (async)");
        return true;
    }

    // HUD4 Part 2: the helper job's composite (m_hudAsync=1). GPU submit when
    // available, else the same CPU stamp the helper runs today. Ordering is
    // exactly the CPU async path's: the worker waited the export fence, the
    // helper composites, then the job queues (see queuePendingAhb).
    void compositeTrickyHudAhbAsync(AHardwareBuffer *buffer, uint32_t imgW, uint32_t imgH,
                                    uint64_t tick,
                                    const ps2_ssx3_tricky_layer::PresentationPacket &p)
    {
        if (!submitTrickyHudGpuAsync(buffer, imgW, imgH, tick, p))
            stampTrickyHudAhb(buffer, imgW, imgH, tick, p);
    }

    // HUD4 lane diag, queue-time half: the stashed pristine frame (same
    // packet) through the CPU stamp vs the GPU-composited AHB region.
    struct HudRegCompare
    {
        uint64_t diffPx = 0u;
        unsigned maxErr = 0u;
    };
    static HudRegCompare hudCompareRegion(const uint8_t *exp, const uint8_t *got, int w, int h)
    {
        HudRegCompare c;
        if (!exp || !got || w <= 0 || h <= 0)
            return c;
        const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
        for (size_t i = 0; i < n; ++i)
        {
            bool diff = false;
            for (int ch = 0; ch < 4; ++ch)
            {
                const unsigned a = exp[i * 4u + static_cast<size_t>(ch)];
                const unsigned b = got[i * 4u + static_cast<size_t>(ch)];
                const unsigned e = a > b ? a - b : b - a;
                if (e != 0u)
                    diff = true;
                if (e > c.maxErr)
                    c.maxErr = e;
            }
            if (diff)
                ++c.diffPx;
        }
        return c;
    }
    // HUD4 Part 2: shared by the inline (worker) and async (helper) GPU paths, with
    // per-mode stash + totals (the two modes never mix in one run, and each caller's
    // state is touched only on its own thread).
    struct HudDiagPending; // defined with the members below (parameter lists need the name now)
    void hudGpuDiagCompare(AHardwareBuffer *buffer, HudDiagPending &pend, uint64_t &totPx,
                           unsigned &totMax, const char *mode)
    {
        using namespace ps2_ssx3_tricky_hud;
        HudDiagPending st;
        st.frame.swap(pend.frame);
        st.stride = pend.stride;
        st.packet = pend.packet;
        st.sprites = pend.sprites;
        st.tick = pend.tick;
        st.fw = pend.fw;
        st.fh = pend.fh;
        st.armed = pend.armed;
        pend = HudDiagPending{};
        if (!st.armed || !buffer || st.frame.empty() || st.stride == 0u)
            return;
        // OUT1 (c): fw/fh are the unrotated scene dims; the stash frame and
        // the AHB are portrait. The reference stamps unrotated and compares
        // in portrait space.
        const int pro = ps2x_present_vk::prerotate();
        if (pro)
        {
            hudGpuDiagCompareRotated(buffer, st, totPx, totMax, mode, pro);
            return;
        }
        const int fw = st.fw, fh = st.fh;
        const Rect r = hudRegionRect(fw, fh);
        if (r.w <= 0 || r.h <= 0 || !st.sprites.ok)
            return;
        AHardwareBuffer_Desc desc = {};
        AHardwareBuffer_describe(buffer, &desc);
        if (static_cast<size_t>(desc.stride) * 4u != st.stride)
            return;
        const ARect lockRect{r.x, r.y, r.x + r.w, r.y + r.h};
        void *ptr = nullptr;
        if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, &lockRect,
                                 &ptr) != 0 ||
            !ptr)
            return;
        std::vector<uint8_t> gpuReg(static_cast<size_t>(r.w) * r.h * 4u);
        const size_t rowBytes = static_cast<size_t>(r.w) * 4u;
        const uint8_t *base =
            static_cast<const uint8_t *>(ptr) + static_cast<size_t>(r.y) * st.stride +
            static_cast<size_t>(r.x) * 4u;
        for (int y = 0; y < r.h; ++y)
            std::memcpy(&gpuReg[static_cast<size_t>(y) * rowBytes], base + y * st.stride,
                        rowBytes);
        AHardwareBuffer_unlock(buffer, nullptr);
        std::vector<uint8_t> tight(static_cast<size_t>(fw) * fh * 4u);
        for (int y = 0; y < fh; ++y)
            std::memcpy(&tight[static_cast<size_t>(y) * fw * 4u],
                        st.frame.data() + static_cast<size_t>(y) * st.stride,
                        static_cast<size_t>(fw) * 4u);
        stampHudDirect<false>(tight.data(), static_cast<size_t>(fw) * 4u, r, st.sprites,
                              st.packet.fill, st.packet.full, st.tick, st.packet.splashUntil,
                              st.packet.litLetters, st.packet.flashUntil);
        std::vector<uint8_t> cpuReg(static_cast<size_t>(r.w) * r.h * 4u);
        for (int y = 0; y < r.h; ++y)
            std::memcpy(&cpuReg[static_cast<size_t>(y) * rowBytes],
                        tight.data() +
                            (static_cast<size_t>(r.y + y) * fw + static_cast<size_t>(r.x)) * 4u,
                        rowBytes);
        const HudRegCompare cmp = hudCompareRegion(cpuReg.data(), gpuReg.data(), r.w, r.h);
        totPx += cmp.diffPx;
        if (cmp.maxErr > totMax)
            totMax = cmp.maxErr;
        std::fprintf(stderr, "[ssx3-tricky-hud] vk gpu-diag t=%llu px=%llu max=%u (tot %llu max %u)%s\n",
                     static_cast<unsigned long long>(st.tick),
                     static_cast<unsigned long long>(cmp.diffPx), cmp.maxErr,
                     static_cast<unsigned long long>(totPx), totMax, mode);
    }

    // OUT1 (c): the rotated diag compare. st.fw/fh are the unrotated scene
    // dims; st.frame and the AHB are portrait. The reference un-transposes
    // the portrait pristine region, stamps it unrotated, and compares in
    // portrait space against the GPU region.
    void hudGpuDiagCompareRotated(AHardwareBuffer *buffer, HudDiagPending &st, uint64_t &totPx,
                                  unsigned &totMax, const char *mode, int sense)
    {
        using namespace ps2_ssx3_tricky_hud;
        const int fw = st.fw, fh = st.fh;
        const Rect r = hudRegionRect(fw, fh);
        if (r.w <= 0 || r.h <= 0 || !st.sprites.ok)
            return;
        const ps2x_present_vk::PrerotateRect pr =
            ps2x_present_vk::prerotateRegion(sense, r.x, r.y, r.w, r.h, fw, fh);
        // Portrait stash dims: the stash frame is the portrait AHB (stride rows).
        const int pw = fh, ph = fw;
        if (pr.x < 0 || pr.y < 0 || pr.x + pr.w > pw || pr.y + pr.h > ph)
            return;
        AHardwareBuffer_Desc desc = {};
        AHardwareBuffer_describe(buffer, &desc);
        if (static_cast<size_t>(desc.stride) * 4u != st.stride)
            return;
        const ARect lockRect{pr.x, pr.y, pr.x + pr.w, pr.y + pr.h};
        void *ptr = nullptr;
        if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, &lockRect,
                                 &ptr) != 0 ||
            !ptr)
            return;
        std::vector<uint8_t> gpuReg(static_cast<size_t>(pr.w) * pr.h * 4u);
        const uint8_t *base = static_cast<const uint8_t *>(ptr);
        for (int y = 0; y < pr.h; ++y)
            std::memcpy(&gpuReg[static_cast<size_t>(y) * pr.w * 4u],
                        base + static_cast<size_t>(pr.y + y) * st.stride +
                            static_cast<size_t>(pr.x) * 4u,
                        static_cast<size_t>(pr.w) * 4u);
        AHardwareBuffer_unlock(buffer, nullptr);
        // Unrotated pristine region from the portrait stash.
        std::vector<uint8_t> temp(static_cast<size_t>(r.w) * r.h * 4u);
        for (int y = 0; y < r.h; ++y)
        {
            for (int x = 0; x < r.w; ++x)
            {
                const ps2x_present_vk::PrerotatePos pp =
                    ps2x_present_vk::prerotateMap(sense, x, y, r.w, r.h, pr.x, pr.y);
                std::memcpy(&temp[(static_cast<size_t>(y) * r.w + x) * 4u],
                            st.frame.data() + static_cast<size_t>(pp.y) * st.stride +
                                static_cast<size_t>(pp.x) * 4u,
                            4u);
            }
        }
        const HudSprites shifted = shiftHudSpritesToRegion(st.sprites, r.x, r.y);
        const Rect r0{0, 0, r.w, r.h};
        stampHudDirect<false>(temp.data(), static_cast<size_t>(r.w) * 4u, r0, shifted,
                              st.packet.fill, st.packet.full, st.tick, st.packet.splashUntil,
                              st.packet.litLetters, st.packet.flashUntil);
        std::vector<uint8_t> cpuReg(static_cast<size_t>(pr.w) * pr.h * 4u);
        for (int y = 0; y < r.h; ++y)
        {
            for (int x = 0; x < r.w; ++x)
            {
                const ps2x_present_vk::PrerotatePos pp =
                    ps2x_present_vk::prerotateMap(sense, x, y, r.w, r.h, 0, 0);
                std::memcpy(&cpuReg[(static_cast<size_t>(pp.y) * pr.w + pp.x) * 4u],
                            &temp[(static_cast<size_t>(y) * r.w + x) * 4u], 4u);
            }
        }
        const HudRegCompare cmp = hudCompareRegion(cpuReg.data(), gpuReg.data(), pr.w, pr.h);
        totPx += cmp.diffPx;
        if (cmp.maxErr > totMax)
            totMax = cmp.maxErr;
        std::fprintf(stderr,
                     "[ssx3-tricky-hud] vk gpu-diag t=%llu px=%llu max=%u (tot %llu max %u)%s "
                     "(rotated)\n",
                     static_cast<unsigned long long>(st.tick),
                     static_cast<unsigned long long>(cmp.diffPx), cmp.maxErr,
                     static_cast<unsigned long long>(totPx), totMax, mode);
    }

    void retireAhbSlots()
    {
        joinHudJob();
        if (m_pendingAhb >= 0 && m_ge1Active)
            m_ge1.waitExport(m_pendingFence);
        m_pendingAhb = -1;
        m_pendingGpu = false; // HUD4: drop the served flag + any diag stash with the slots
        m_hudDiagPending = HudDiagPending{};
        m_hudGpuAsyncDiag = HudDiagPending{}; // HUD4 Part 2: the async twin (helper joined above)
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
        m_ahbSlots.clear();
    }

    bool presentAhb(uint64_t tick)
    {
        if (!m_ahbSlots.empty() && m_ahbEpoch != ps2x_present_vk::poolEpoch())
            retireAhbSlots();
        if (!queuePendingAhb())
            return false;
        if (m_ahbSlots.empty())
        {
            // OUT2: under DEVICE composition the compositor holds each AHB
            // ~4 frames, so the 4-deep pool leaves pick() polling the release
            // fence ~2 ms/frame. A deeper pool restores the slack (N=6 covers
            // the measured hold with margin); unset = today's 4.
            const int poolSize = ps2x_present_vk::ahbPoolSize();
            m_ahbSlots.resize(static_cast<size_t>(poolSize));
            for (AhbSlot &slot : m_ahbSlots)
            {
                slot.id = ps2x_present_vk::allocateBuffer(m_exportW, m_exportH, &slot.buffer);
                if (!slot.id || !slot.buffer)
                {
                    retireAhbSlots();
                    ps2x_present_vk::fallBack("GE1 AHB pool allocation failed");
                    return false;
                }
            }
            m_ahbEpoch = ps2x_present_vk::bufferEpoch(m_ahbSlots[0].id);
            std::fprintf(stderr, "[gs:external] GE1 AHB pool ready epoch=%u size=%d\n", m_ahbEpoch,
                         poolSize);
        }
        const int poolN = static_cast<int>(m_ahbSlots.size());
        uint64_t ids[8]; // ahbPoolSize() is clamped to 4..8
        for (int i = 0; i < poolN; ++i) ids[i] = m_ahbSlots[static_cast<size_t>(i)].id;
        const ps2x_present_vk::Pick pick =
            ps2x_present_vk::pickReusable(ids, poolN, m_ahbStart, 1000, m_hudInFlight);
        if (pick.index < 0)
        {
            if (pick.giveUp)
                ps2x_present_vk::fallBack("GE1 AHB compositor release timeout");
            return false;
        }
        AhbSlot &slot = m_ahbSlots[static_cast<size_t>(pick.index)];
        m_ahbStart = (pick.index + 1) % poolN;
        uint64_t fence = 0u;
        const int exported = m_ge1.exportAhb(slot.buffer, m_exportW, m_exportH, &fence);
        if (exported == 0)
            return false; // the first host present may precede the first GS VSync
        if (exported < 0)
        {
            std::fprintf(stderr, "[gs:external] AHB export failed at tick=%llu\n", (unsigned long long)tick);
            ps2x_present_vk::fallBack("GE1 GPU to AHB export failed");
            return false;
        }
        m_pendingAhb = pick.index;
        m_pendingFence = fence;
        m_pendingTick = tick;
        // HUD4: submit the HUD composite right behind the export copy on
        // GE1's queue (same thread, back-to-back: no added drain or wait).
        // On success m_pendingFence takes the composite fence (it covers the
        // copy) and the queue path skips the CPU stamp for this slot.
        // HUD4 Part 2: the inline (worker) submit runs only when the helper
        // is off; in async mode the helper submits after the worker's export
        // wait (compositeTrickyHudAhbAsync), so the worker never waits on it.
        m_pendingGpu = !m_hudAsync &&
            compositeTrickyHudGpu(slot.buffer, m_exportW, m_exportH, tick, fence, &m_pendingFence);
        return true;
    }
#endif

#if defined(PS2X_GE1_STATIC_IOSURFACE)
    static bool iosPresentPerVsync()
    {
        static const bool on = [] {
            const char *v = std::getenv("PS2X_PRESENT_PER_VSYNC");
            return v && std::strcmp(v, "1") == 0;
        }();
        return on;
    }

    // GI1: queue an async GPU export into a free IOSurface slot. The publish
    // happens later, from the command buffer's completion handler (ioExportDone).
    // Not Queued = fall back to the CPU snapshot for this frame (all slots
    // busy, no composed frame yet, or the export failed). PSO1: NoSlot is
    // reported separately (ordinary backpressure under ownership).
    enum class IOExport
    {
        Queued,
        NoSlot,
        Failed,
    };
    IOExport presentIOSurface(uint64_t tick)
    {
        if (!ps2x_present_share::enabled() || !m_ge1.exportIOSurface)
            return IOExport::Failed;
        // PSO1 Part 2 diagnostic: tick-locked capture gate (inert by default).
        if (!ps2x_present_share::captureGateAllowsExport(tick))
            return IOExport::NoSlot;
        if (ps2x_present_share::ownershipEnabled())
            return presentIOSurfaceOwned(tick);
        IOSurfacePool &pool = ioPool();
        int slot = -1;
        for (int i = 0; i < kLegacySlotCount; ++i)
        {
            bool expected = false;
            if (pool.busy[i].compare_exchange_strong(expected, true))
            {
                slot = i;
                break;
            }
        }
        if (slot < 0)
            return IOExport::NoSlot;
        ps2x_present_own::witnessWrite(slot);
        void *surface = nullptr;
        uint64_t epoch = 0u;
        {
            std::lock_guard<std::mutex> lock(pool.mutex);
            // IX1: a slot made at another size (an earlier backend) is replaced;
            // createSurface keeps the old one alive for the process.
            if (!pool.surfaces[slot] ||
                IOSurfaceGetWidth(static_cast<IOSurfaceRef>(pool.surfaces[slot])) != m_exportW ||
                IOSurfaceGetHeight(static_cast<IOSurfaceRef>(pool.surfaces[slot])) != m_exportH)
                pool.surfaces[slot] = ps2x_present_share::createSurface(m_exportW, m_exportH);
            surface = pool.surfaces[slot];
            epoch = pool.epoch;
        }
        if (!surface)
        {
            pool.busy[slot].store(false);
            return IOExport::Failed;
        }
        std::unique_ptr<IOSurfaceExportCtx> ctx(
            new IOSurfaceExportCtx{epoch, slot, tick, ps2x::perflog::steadyNs()});
        const int rc =
            m_ge1.exportIOSurface(surface, m_exportW, m_exportH, &ioExportDone, ctx.get());
        if (rc != 1)
        {
            pool.busy[slot].store(false);
            return IOExport::Failed;
        }
        ctx.release(); // ioExportDone owns it from here (fires exactly once)
        return IOExport::Queued;
    }

    // PSO1 (PRV1 §3): reserve a FREE slot (never READY, CURRENT or a slot
    // whose last GL read is outstanding), export into it, and let the
    // completion handler publish it READY. Same export call and sizes as the
    // legacy path; only the slot choice and its release differ.
    IOExport presentIOSurfaceOwned(uint64_t tick)
    {
        IOSurfacePool &pool = ioPool();
        ps2x_present_own::Pool &own = ps2x_present_own::sharedPool();
        const uint64_t now = ps2x::perflog::steadyNs();
        uint64_t gen = 0u;
        const int slot = own.reserve(&gen, now);
        if (slot < 0)
            return IOExport::NoSlot;
        ps2x_present_own::witnessWrite(slot);
        void *surface = nullptr;
        uint64_t epoch = 0u;
        uint64_t seq = 0u;
        {
            std::lock_guard<std::mutex> lock(pool.mutex);
            // IX1: resize replaces only this FREE slot's surface; the old one
            // stays alive for the process (createSurface).
            if (!pool.surfaces[slot] ||
                IOSurfaceGetWidth(static_cast<IOSurfaceRef>(pool.surfaces[slot])) != m_exportW ||
                IOSurfaceGetHeight(static_cast<IOSurfaceRef>(pool.surfaces[slot])) != m_exportH)
                pool.surfaces[slot] = ps2x_present_share::createSurface(m_exportW, m_exportH);
            surface = pool.surfaces[slot];
            epoch = pool.epoch;
            seq = ++pool.seq;
        }
        if (!surface)
        {
            own.complete(slot, gen, false, {}, ps2x::perflog::steadyNs());
            return IOExport::Failed;
        }
        std::unique_ptr<IOSurfaceExportCtx> ctx(new IOSurfaceExportCtx{epoch, slot, tick, now, gen, seq});
        const int rc = m_ge1.exportIOSurface(surface, m_exportW, m_exportH, &ioExportDone, ctx.get());
        if (rc != 1)
        {
            own.complete(slot, gen, false, {}, ps2x::perflog::steadyNs());
            return IOExport::Failed;
        }
        ctx.release(); // ioExportDone owns it from here (fires exactly once)
        return IOExport::Queued;
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
    // UR1/IX1: GE1 export size (Android AHBs, iOS IOSurfaces), PS2X_GE1_EXPORT_SIZE
    // (default 640x480 = today).
    uint32_t m_exportW = 640u, m_exportH = 480u;
#if defined(__ANDROID__)
    // OUT2: sized from PS2X_PRESENT_AHB_POOL at pool creation (default 4 =
    // today). Empty = no pool (retired or never built).
    std::vector<AhbSlot> m_ahbSlots;
    uint32_t m_ahbEpoch = 0u;
    int m_ahbStart = 0;
    int m_pendingAhb = -1;
    bool m_perVsyncLive = false; // FH6: GuestVsync presents (latch no longer exports)
    uint64_t m_pendingFence = 0u;
    uint64_t m_pendingTick = 0u;
    ps2_ssx3_tricky_hud::HudSprites m_hudSprites; // pre-scaled art (built once per run)
    uint64_t m_hudComposites = 0u;
    uint64_t m_hudCompositeNs = 0u;
    uint64_t m_hudLockFails = 0u;
    uint64_t m_hudBuildFails = 0u;
    // HUD2: PS2X_TRICKY_HUD_CACHE=1 serves the composite from the fused
    // layer (rebuilt on visual-key change); falls back to direct on refusal.
    const bool m_hudCache = [] {
        const char *v = std::getenv("PS2X_TRICKY_HUD_CACHE");
        return v && std::strcmp(v, "1") == 0;
    }();
    ps2_ssx3_tricky_hud::HudCache m_hudCacheState;
    uint64_t m_hudCacheFallbacks = 0u;
    // HUD4: PS2X_TRICKY_HUD_GPU=1 composites on GE1's queue
    // (ge1_gs_hud_scene, submitted at export time); the CPU stamp stays as
    // the reference and the fallback. m_hudGpuSprites is GS-worker-owned:
    // the GsHud helper owns m_hudSprites for the CPU path, and a GPU-served
    // slot N can overlap a CPU-stamped slot N-1 on the helper, so the two
    // paths must not share sprite state.
    const bool m_hudGpu = [] {
        const char *v = std::getenv("PS2X_TRICKY_HUD_GPU");
        return v && std::strcmp(v, "1") == 0;
    }();
    ps2_ssx3_tricky_hud::HudSprites m_hudGpuSprites;
    uint64_t m_hudGpuSeen = 0u;
    uint64_t m_hudGpuServed = 0u;
    uint64_t m_hudGpuCompositeNs = 0u;
    uint64_t m_hudGpuFallbacks = 0u;
    bool m_pendingGpu = false; // the pending slot is GPU-served (skip its stamp)
    // HUD4 lane diag (PS2X_TRICKY_HUD_GPU_DIAG=1): every 30th served
    // composite stashes its pristine frame at export time; the queue path
    // compares the GPU region against the same-packet CPU stamp.
    const bool m_hudDiag = [] {
        const char *v = std::getenv("PS2X_TRICKY_HUD_GPU_DIAG");
        return v && std::strcmp(v, "1") == 0;
    }();
    struct HudDiagPending
    {
        bool armed = false;
        std::vector<uint8_t> frame; // pristine full frame, st.stride rows
        size_t stride = 0u;
        ps2_ssx3_tricky_layer::PresentationPacket packet;
        ps2_ssx3_tricky_hud::HudSprites sprites; // deep copy (geometry is fixed)
        uint64_t tick = 0u;
        int fw = 0, fh = 0;
    };
    HudDiagPending m_hudDiagPending;
    uint64_t m_hudDiagPx = 0u;
    unsigned m_hudDiagMax = 0u;
    // GSW1: PS2X_TRICKY_HUD_ASYNC=1 runs the composite + queue on m_hudThread.
    // m_hudInFlight: the slot it owns until joinHudJob (-1 = none);
    // m_hudJobQueued: that job's queue result, read only after the join.
    const bool m_hudAsync = [] {
        const char *v = std::getenv("PS2X_TRICKY_HUD_ASYNC");
        return v && std::strcmp(v, "1") == 0;
    }();
    int m_hudInFlight = -1;
    bool m_hudJobQueued = true;
    bool m_hudAsyncLogged = false;
    ps2x_gs::SerialJobThread m_hudThread{"GsHud"};
    // HUD4 Part 2: async GPU submit (m_hudAsync=1): the helper builds the scene,
    // submits through ge1_gs_hud_scene, waits its own fence and serves the slot;
    // the worker never waits on it. Helper-owned (the worker touches none of
    // these in async mode); the inline path keeps its worker-owned twins above.
    ps2_ssx3_tricky_hud::HudSprites m_hudGpuAsyncSprites;
    uint64_t m_hudGpuAsyncSeen = 0u;
    uint64_t m_hudGpuAsyncServed = 0u;
    uint64_t m_hudGpuAsyncCompositeNs = 0u;
    uint64_t m_hudGpuAsyncFallbacks = 0u;
    HudDiagPending m_hudGpuAsyncDiag;
    uint64_t m_hudGpuAsyncDiagPx = 0u;
    unsigned m_hudGpuAsyncDiagMax = 0u;
#endif
    uint8_t m_ge1LastPath = 3u;
    uint32_t m_ge1FifoBytes = 0u;
    bool m_ge1FifoServed = false;
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
