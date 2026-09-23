// G44 synchronous paraLLEl shadow. See ps2_gs_shadow.h for the contract.
#include "runtime/gs/ps2_gs_shadow.h"
#include "runtime/ps2_memory.h"

#ifdef PS2X_SHADOW_HAVE_RAYLIB
#include "raylib.h"
#endif

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <vector>
#ifdef __APPLE__
#include <execinfo.h>
#endif

#ifdef PS2X_HAS_PARALLEL_SHADOW
#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"
#endif

namespace ps2x_gs_shadow
{
namespace
{

struct State
{
    std::mutex latchMutex;
    bool latched = false;
    Config cfg;
    std::string dir = "g44-shadow";
    bool loggedNoBackend = false;

    std::mutex mutex; // serializes ALL backend calls (game + main threads)
    std::atomic<uint64_t> gifFed{0u};
    std::atomic<uint64_t> regFed{0u};
    std::atomic<uint64_t> seen{0u};
    std::atomic<uint64_t> pairs{0u};
    std::atomic<bool> failed{false};
    std::atomic<bool> csvHeaderWrote{false};
    std::atomic<uint32_t> skipLogged{0u};

#ifdef PS2X_HAS_PARALLEL_SHADOW
    Vulkan::Context *ctx = nullptr;
    Vulkan::Device *device = nullptr;
    ParallelGS::GSInterface *iface = nullptr;
    bool initOk = false;
    bool needReinit = false;
    uint64_t initGifCount = 0u;
#endif
};

State &state()
{
    static State s;
    return s;
}

void latchConfig()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.latchMutex);
    if (s.latched)
        return;
    s.latched = true;
    const char *mode = std::getenv("PS2X_GS_SHADOW");
    s.cfg.wantParallel = parseModeParallel(mode);
    s.cfg.from = parseU64(std::getenv("PS2X_GS_SHADOW_FROM"), 0u);
    s.cfg.to = parseU64(std::getenv("PS2X_GS_SHADOW_TO"), ~0ull);
    s.cfg.cap = kPairCap;
    const char *dir = std::getenv("PS2X_GS_SHADOW_DIR");
    if (dir && dir[0] != '\0')
        s.dir = dir;
}

void logBacktrace()
{
#ifdef __APPLE__
    void *frames[16];
    const int n = ::backtrace(frames, 16);
    char **syms = ::backtrace_symbols(frames, n);
    std::cerr << "[shadow] backtrace (" << n << " frames):" << std::endl;
    for (int i = 0; i < n; ++i)
        std::cerr << "[shadow]   #" << i << " " << (syms ? syms[i] : "?") << std::endl;
    std::free(syms);
#else
    std::cerr << "[shadow] (no backtrace on this platform)" << std::endl;
#endif
}

uint32_t fnv1a32(const uint8_t *data, size_t size)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

#ifdef PS2X_HAS_PARALLEL_SHADOW
void failLocked(State &s, const char *what)
{
    std::cerr << "[shadow] FATAL: " << what << " (disabling shadow, CPU path continues)" << std::endl;
    logBacktrace();
    s.failed.store(true, std::memory_order_relaxed);
}

bool ensureInitLocked(State &s)
{
    if (s.initOk && !s.needReinit)
        return true;
    if (s.needReinit)
    {
        delete s.iface;
        s.iface = nullptr;
        // Context/Device teardown ordering follows the replayer (device outlives
        // iface use); full re-creation below keeps this path simple and rare.
        delete s.device;
        s.device = nullptr;
        delete s.ctx;
        s.ctx = nullptr;
        s.initOk = false;
        s.needReinit = false;
        std::cerr << "[shadow] re-init after GS reset" << std::endl;
    }
    if (s.initOk)
        return true;

    const uint64_t t0 = s.gifFed.load(std::memory_order_relaxed) + s.regFed.load(std::memory_order_relaxed);
    std::cerr << "[shadow] GRANITE_VULKAN_LIBRARY="
              << (std::getenv("GRANITE_VULKAN_LIBRARY") ? std::getenv("GRANITE_VULKAN_LIBRARY") : "(unset)")
              << std::endl;
    if (!Vulkan::Context::init_loader(nullptr))
    {
        failLocked(s, "Context::init_loader(system MoltenVK) failed");
        return false;
    }
    Vulkan::Context *ctx = new (std::nothrow) Vulkan::Context();
    if (!ctx)
    {
        failLocked(s, "Context alloc failed");
        return false;
    }
    ctx->set_num_thread_indices(1);
    constexpr uint32_t kFlags = Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
                                Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
                                Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT;
    if (!ctx->init_instance_and_device(nullptr, 0, nullptr, 0, kFlags))
    {
        delete ctx;
        failLocked(s, "init_instance_and_device failed");
        return false;
    }
    Vulkan::Device *device = new (std::nothrow) Vulkan::Device();
    if (!device)
    {
        delete ctx;
        failLocked(s, "Device alloc failed");
        return false;
    }
    device->set_context(*ctx);
    device->init_frame_contexts(4);

    ParallelGS::GSInterface *iface = new (std::nothrow) ParallelGS::GSInterface();
    if (!iface)
    {
        delete device;
        delete ctx;
        failLocked(s, "GSInterface alloc failed");
        return false;
    }
    const ParallelGS::GSOptions opts = {};
    if (!iface->init(device, opts))
    {
        delete iface;
        delete device;
        delete ctx;
        failLocked(s, "GSInterface::init failed");
        return false;
    }
    ParallelGS::DebugMode dm;
    dm.feedback_render_target = false; // G26-corrected default
    iface->set_debug_mode(dm);

    s.ctx = ctx;
    s.device = device;
    s.iface = iface;
    s.initOk = true;
    s.initGifCount = t0;
    std::cerr << "[shadow] backend init ok (MoltenVK system loader, standalone slangmosh shaders)" << std::endl;
    return true;
}

void syncPrivLocked(State &s, const GSRegisters *priv)
{
    // Priv fields are raw hardware bit-word structs (FIELD32 pairs, 8 bytes),
    // not Reg64 unions: copy the register image verbatim, like the dump
    // parser's bulk PrivRegisters read.
    if (!priv || !s.iface)
        return;
    ParallelGS::PrivRegisterState &dst = s.iface->get_priv_register_state();
// All fields are 8-byte hardware images except smode2 (12 bytes: the third
// word is pure pad; the first 8 bytes carry INT/FFMD/DPMS, which is all the
// scanout path reads). Probe receipt: /tmp/g44-priv-probe.cpp sizes.
#define G44_SYNC_PRIV(field)                                                            \
    do                                                                                  \
    {                                                                                   \
        static_assert(sizeof(dst.field) >= sizeof(uint64_t), "priv image");             \
        static_assert(offsetof(ParallelGS::PrivRegisterState, field) % 16u == 0u,        \
                      "priv slot");                                                     \
        const uint64_t v_ = priv->field;                                                \
        std::memcpy(&dst.field, &v_, sizeof(v_));                                       \
    } while (0)
    G44_SYNC_PRIV(pmode);
    G44_SYNC_PRIV(smode1);
    G44_SYNC_PRIV(smode2);
    G44_SYNC_PRIV(srfsh);
    G44_SYNC_PRIV(synch1);
    G44_SYNC_PRIV(synch2);
    G44_SYNC_PRIV(syncv);
    G44_SYNC_PRIV(dispfb1);
    G44_SYNC_PRIV(display1);
    G44_SYNC_PRIV(dispfb2);
    G44_SYNC_PRIV(display2);
    G44_SYNC_PRIV(extbuf);
    G44_SYNC_PRIV(extdata);
    G44_SYNC_PRIV(extwrite);
    G44_SYNC_PRIV(bgcolor);
#undef G44_SYNC_PRIV
}
#endif

void writeStatsFile(State &s)
{
    std::ofstream out(s.dir + "/shadow-stats.txt", std::ios::trunc);
    if (!out)
        return;
    out << "gif_packets_fed=" << s.gifFed.load() << "\n"
        << "reg_writes_fed=" << s.regFed.load() << "\n"
        << "presents_seen=" << s.seen.load() << "\n"
        << "pairs_wrote=" << s.pairs.load() << "\n"
        << "init_failed=" << (s.failed.load() ? 1 : 0) << "\n"
#ifdef PS2X_HAS_PARALLEL_SHADOW
        << "backend=parallel-gs-standalone\n"
#else
        << "backend=none\n"
#endif
        ;
}

struct CompareResult
{
    uint32_t cmpW = 0u, cmpH = 0u;
    double psnrDb = 0.0;
    uint64_t diffPx = 0u;
    int64_t bbX0 = -1, bbY0 = -1, bbX1 = -1, bbY1 = -1;
    uint32_t cpuFnv = 0u, parFnv = 0u;
};

CompareResult compareRgb(const uint8_t *cpu, uint32_t cpuW, uint32_t cpuH,
                         const uint8_t *par, uint32_t parW, uint32_t parH)
{
    CompareResult r;
    r.cmpW = (cpuW < parW) ? cpuW : parW;
    r.cmpH = (cpuH < parH) ? cpuH : parH;
    if (r.cmpW == 0u || r.cmpH == 0u)
        return r;
    r.cpuFnv = fnv1a32(cpu, static_cast<size_t>(cpuW) * cpuH * 4u);
    r.parFnv = fnv1a32(par, static_cast<size_t>(parW) * parH * 4u);
    uint64_t sse = 0u;
    for (uint32_t y = 0; y < r.cmpH; ++y)
    {
        for (uint32_t x = 0; x < r.cmpW; ++x)
        {
            const size_t co = (static_cast<size_t>(y) * cpuW + x) * 4u;
            const size_t po = (static_cast<size_t>(y) * parW + x) * 4u;
            bool diff = false;
            for (int c = 0; c < 3; ++c)
            {
                const int d = static_cast<int>(cpu[co + c]) - static_cast<int>(par[po + c]);
                sse += static_cast<uint64_t>(d * d);
                diff = diff || (d != 0);
            }
            if (diff)
            {
                ++r.diffPx;
                if (r.bbX0 < 0 || static_cast<int64_t>(x) < r.bbX0)
                    r.bbX0 = x;
                if (r.bbY0 < 0 || static_cast<int64_t>(y) < r.bbY0)
                    r.bbY0 = y;
                if (static_cast<int64_t>(x) > r.bbX1)
                    r.bbX1 = x;
                if (static_cast<int64_t>(y) > r.bbY1)
                    r.bbY1 = y;
            }
        }
    }
    const double n = static_cast<double>(r.cmpW) * r.cmpH * 3.0;
    const double mse = static_cast<double>(sse) / n;
    r.psnrDb = (mse <= 0.0) ? std::numeric_limits<double>::infinity()
                            : 10.0 * std::log10(65025.0 / mse);
    return r;
}

void exportRgbaPng(const uint8_t *rgba, uint32_t w, uint32_t h, const std::string &path)
{
#ifdef PS2X_SHADOW_HAVE_RAYLIB
    if (!rgba || w == 0u || h == 0u)
        return;
    Image img{};
    img.data = const_cast<uint8_t *>(rgba);
    img.width = static_cast<int>(w);
    img.height = static_cast<int>(h);
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    ExportImage(img, path.c_str());
#else
    (void)rgba;
    (void)w;
    (void)h;
    (void)path;
#endif
}

void exportSideBySide(const uint8_t *cpu, uint32_t cpuW, uint32_t cpuH,
                      const uint8_t *par, uint32_t parW, uint32_t parH,
                      const std::string &path)
{
    const uint32_t w = cpuW + parW;
    const uint32_t h = (cpuH > parH) ? cpuH : parH;
    if (!cpu || !par || w == 0u || h == 0u)
        return;
    std::vector<uint8_t> canvas(static_cast<size_t>(w) * h * 4u, 0u);
    for (uint32_t y = 0; y < cpuH; ++y)
        std::memcpy(canvas.data() + static_cast<size_t>(y) * w * 4u,
                    cpu + static_cast<size_t>(y) * cpuW * 4u,
                    static_cast<size_t>(cpuW) * 4u);
    for (uint32_t y = 0; y < parH; ++y)
        std::memcpy(canvas.data() + (static_cast<size_t>(y) * w + cpuW) * 4u,
                    par + static_cast<size_t>(y) * parW * 4u,
                    static_cast<size_t>(parW) * 4u);
    exportRgbaPng(canvas.data(), w, h, path);
}

} // namespace

bool parseModeParallel(const char *value)
{
    return value && std::strcmp(value, "parallel") == 0;
}

uint64_t parseU64(const char *value, uint64_t dflt)
{
    if (!value || !value[0])
        return dflt;
    char *end = nullptr;
    const unsigned long long v = std::strtoull(value, &end, 10);
    if (end == value)
        return dflt;
    return static_cast<uint64_t>(v);
}

bool tickEligible(uint64_t tick, uint64_t from, uint64_t to, uint64_t pairsDone, uint64_t cap)
{
    return tick >= from && tick < to && pairsDone < cap;
}

const Config &config()
{
    latchConfig();
    return state().cfg;
}

bool hasBackend()
{
#ifdef PS2X_HAS_PARALLEL_SHADOW
    return true;
#else
    return false;
#endif
}

bool enabled()
{
    latchConfig();
    State &s = state();
    if (!s.cfg.wantParallel || !hasBackend())
    {
        if (s.cfg.wantParallel && !hasBackend() && !s.loggedNoBackend)
        {
            s.loggedNoBackend = true;
            std::cerr << "[shadow] PS2X_GS_SHADOW=parallel requested but this build has no parallel backend"
                      << std::endl;
        }
        return false;
    }
    return !s.failed.load(std::memory_order_relaxed);
}

bool routeGifViaArbiter()
{
    return enabled();
}

void onGifPacket(uint32_t path, const uint8_t *data, uint32_t sizeBytes)
{
    if (!enabled())
        return;
#ifdef PS2X_HAS_PARALLEL_SHADOW
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.failed.load(std::memory_order_relaxed))
        return;
    try
    {
        if (!ensureInitLocked(s))
            return;
        s.iface->gif_transfer(path, data, static_cast<size_t>(sizeBytes));
        s.gifFed.fetch_add(1u, std::memory_order_relaxed);
    }
    catch (...)
    {
        failLocked(s, "gif_transfer threw");
    }
#else
    (void)path;
    (void)data;
    (void)sizeBytes;
#endif
}

void onWriteRegister(uint8_t regAddr, uint64_t value)
{
    if (!enabled())
        return;
#ifdef PS2X_HAS_PARALLEL_SHADOW
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.failed.load(std::memory_order_relaxed))
        return;
    try
    {
        if (!ensureInitLocked(s))
            return;
        s.iface->write_register(static_cast<ParallelGS::RegisterAddr>(regAddr), value);
        s.regFed.fetch_add(1u, std::memory_order_relaxed);
    }
    catch (...)
    {
        failLocked(s, "write_register threw");
    }
#else
    (void)regAddr;
    (void)value;
#endif
}

void onReset()
{
#ifdef PS2X_HAS_PARALLEL_SHADOW
    if (!enabled())
        return;
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.initOk)
    {
        s.needReinit = true;
        std::cerr << "[shadow] GS reset observed; backend will re-init on next feed" << std::endl;
    }
#endif
}

void onPresentFrame(uint64_t tick,
                    const uint8_t *cpuRgba,
                    uint32_t cpuWidth,
                    uint32_t cpuHeight,
                    const GSRegisters *priv)
{
    if (!enabled())
        return;
    State &s = state();
    const uint64_t seen = s.seen.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (!cpuRgba || cpuWidth == 0u || cpuHeight == 0u)
        return;
    // G44 Part-2: persist feed observability every 60 presents even when no
    // pair is captured (0-pair runs otherwise leave no counter receipts).
    if (seen % 60u == 0u)
    {
        std::error_code ec;
        std::filesystem::create_directories(s.dir, ec);
        writeStatsFile(s);
    }
    {
        // Fast path: eligibility without the backend lock.
        latchConfig();
        if (!tickEligible(tick, s.cfg.from, s.cfg.to, s.pairs.load(std::memory_order_relaxed), s.cfg.cap))
            return;
    }
#ifdef PS2X_HAS_PARALLEL_SHADOW
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.failed.load(std::memory_order_relaxed))
        return;
    if (!tickEligible(tick, s.cfg.from, s.cfg.to, s.pairs.load(std::memory_order_relaxed), s.cfg.cap))
        return;
    try
    {
        if (!ensureInitLocked(s))
            return;
        syncPrivLocked(s, priv);

        ParallelGS::VSyncInfo vsync = {};
        vsync.phase = static_cast<uint32_t>(tick & 1u);
        vsync.dst_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
        vsync.dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        vsync.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        vsync.adapt_to_internal_horizontal_resolution = true;

        s.iface->flush();
        ParallelGS::ScanoutResult shot = s.iface->vsync(vsync);
        if (!shot.image)
        {
            // G44 single-mechanism workaround (S1/S2 finding): SSX3 scans out
            // with SMODE1 CMOD=PROGRESSIVE(0) + SMODE2 INT=1 — effectively
            // 480i — but paraLLEl's scanout only maps (NTSC || (PROGRESSIVE
            // && !INT)) && ANALOG (+PAL/HDTV/VGA/29). S2 showed LC=0, not 32
            // (SMODE1 likely never programmed on the SKIP_MOVIE path), so
            // the first condition (LC==32) never fired. Retry the vsync with
            // CMOD=NTSC for scanout-geometry purposes, then restore.
            // Rendering is unaffected (VRAM content is final); only the
            // scanout mode mapping is missing. GS sources stay pristine
            // (G43 equality proof intact). S3 must validate this fires and
            // recovers scanouts (committed UNVALIDATED: no boot budget left).
            ParallelGS::PrivRegisterState &pr = s.iface->get_priv_register_state();
            const unsigned cm = pr.smode1.CMOD;
            const unsigned lc = pr.smode1.LC;
            const unsigned in = pr.smode2.INT;
            const unsigned ff = pr.smode2.FFMD;
            if (cm == 0u && in != 0u && (lc == 0u || lc == 32u))
            {
                pr.smode1.CMOD = 2u;
                shot = s.iface->vsync(vsync);
                pr.smode1.CMOD = 0u;
                const uint32_t n = s.skipLogged.fetch_add(1u, std::memory_order_relaxed);
                if (n < 4u)
                    std::cerr << "[shadow] tick=" << tick
                              << " CMOD PROGRESSIVE+INT+ANALOG workaround fired"
                              << (shot.image ? " (scanout recovered)" : " (still null)") << std::endl;
            }
            else
            {
                const uint32_t n = s.skipLogged.fetch_add(1u, std::memory_order_relaxed);
                if (n < 4u)
                    std::cerr << "[shadow] tick=" << tick << " no scanout image CMOD=" << cm
                              << " LC=" << lc << " INT=" << in << " FFMD=" << ff << std::endl;
            }
            if (!shot.image)
                return;
        }
        const uint32_t parW = shot.image->get_width();
        const uint32_t parH = shot.image->get_height();
        if (parW == 0u || parH == 0u || parW > 4096u || parH > 4096u)
        {
            std::cerr << "[shadow] tick=" << tick << " implausible scanout " << parW << "x" << parH << std::endl;
            return;
        }

        // Readback mirrors the G-series replayer P1 path (CachedHost).
        std::vector<uint8_t> par;
        {
            Vulkan::Device *device = s.device;
            auto cmd = device->request_command_buffer();
            cmd->image_barrier(*shot.image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            Vulkan::BufferCreateInfo info = {};
            info.size = static_cast<size_t>(parW) * parH * sizeof(uint32_t);
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            info.domain = Vulkan::BufferDomain::CachedHost;
            Vulkan::BufferHandle rb = device->create_buffer(info);
            cmd->copy_image_to_buffer(*rb, *shot.image, 0, {}, {parW, parH, 1}, 0, 0,
                                      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            device->submit(cmd);
            device->wait_idle();
            const uint8_t *px = static_cast<const uint8_t *>(
                device->map_host_buffer(*rb, Vulkan::MEMORY_ACCESS_READ_BIT));
            if (!px)
            {
                failLocked(s, "map_host_buffer returned null");
                return;
            }
            par.assign(px, px + info.size);
        }

        const CompareResult r = compareRgb(cpuRgba, cpuWidth, cpuHeight, par.data(), parW, parH);

        std::error_code ec;
        std::filesystem::create_directories(s.dir, ec);
        char name[64];
        std::snprintf(name, sizeof(name), "%llu", static_cast<unsigned long long>(tick));
        exportRgbaPng(cpuRgba, cpuWidth, cpuHeight, s.dir + "/cpu-" + name + ".png");
        exportRgbaPng(par.data(), parW, parH, s.dir + "/par-" + name + ".png");
        exportSideBySide(cpuRgba, cpuWidth, cpuHeight, par.data(), parW, parH,
                         s.dir + "/side-" + name + ".png");

        const std::string csvPath = s.dir + "/pairs.csv";
        const bool needHeader = !s.csvHeaderWrote.exchange(true, std::memory_order_relaxed);
        std::ofstream csv(csvPath, std::ios::app);
        if (needHeader && csv.tellp() == std::streampos(0))
        {
            csv << "tick,cpu_w,cpu_h,par_w,par_h,cmp_w,cmp_h,psnr_db,diff_px,"
                   "bb_x0,bb_y0,bb_x1,bb_y1,cpu_fnv,par_fnv,phase\n";
        }
        char psnr[32];
        std::snprintf(psnr, sizeof(psnr),
                      std::isinf(r.psnrDb) ? "inf" : "%.2f", r.psnrDb);
        csv << tick << "," << cpuWidth << "," << cpuHeight << "," << parW << "," << parH << ","
            << r.cmpW << "," << r.cmpH << "," << psnr << "," << r.diffPx << "," << r.bbX0 << ","
            << r.bbY0 << "," << r.bbX1 << "," << r.bbY1 << "," << std::hex << r.cpuFnv << ","
            << r.parFnv << std::dec << "," << (tick & 1u) << "\n";
        csv.close();

        const uint64_t n = s.pairs.fetch_add(1u, std::memory_order_relaxed) + 1u;
        writeStatsFile(s);
        if (n == 1u || n % 50u == 0u || n >= s.cfg.cap)
        {
            std::cerr << "[shadow] pair " << n << " tick=" << tick << " cpu=" << cpuWidth << "x"
                      << cpuHeight << " par=" << parW << "x" << parH << " psnr=" << psnr
                      << " diff=" << r.diffPx << " bbox=(" << r.bbX0 << "," << r.bbY0 << ")-("
                      << r.bbX1 << "," << r.bbY1 << ")" << std::endl;
        }
    }
    catch (...)
    {
        failLocked(s, "present/compare threw");
    }
#else
    (void)priv;
#endif
}

uint64_t gifPacketsFed()
{
    return state().gifFed.load(std::memory_order_relaxed);
}
uint64_t regWritesFed()
{
    return state().regFed.load(std::memory_order_relaxed);
}
uint64_t presentsSeen()
{
    return state().seen.load(std::memory_order_relaxed);
}
uint64_t pairsWrote()
{
    return state().pairs.load(std::memory_order_relaxed);
}
const char *shadowDir()
{
    latchConfig();
    return state().dir.c_str();
}
bool initFailed()
{
    return state().failed.load(std::memory_order_relaxed);
}

void resetForTest()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.latchMutex);
    s.latched = false;
    s.cfg = Config();
    s.dir = "g44-shadow";
    s.loggedNoBackend = false;
    s.gifFed.store(0u);
    s.regFed.store(0u);
    s.seen.store(0u);
    s.pairs.store(0u);
    s.failed.store(false);
    s.csvHeaderWrote.store(false);
    s.skipLogged.store(0u);
}

} // namespace ps2x_gs_shadow
