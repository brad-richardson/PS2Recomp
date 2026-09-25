// GB3 Part 2: paraLLEl-GS live backend. See ps2_gs_parallel_backend.h.
//
// Device/interface init, the priv-register copy and the scanout readback
// follow G44's shadow (ps2_gs_shadow.cpp: ensureInitLocked, syncPrivLocked,
// onPresentFrame's CachedHost copy), minus the shadow's SMODE1 override:
// GB3 Part 1 programs SMODE1 through SetGsCrt/sceGsResetGraph.
#include "runtime/gs/ps2_gs_parallel_backend.h"
#include "runtime/gs/ps2_present_share.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__ANDROID__) && defined(PS2X_HAS_PARALLEL_SHADOW)
#include <dlfcn.h>
#endif

#ifdef PS2X_HAS_PARALLEL_SHADOW
#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"
#include "pgs_env_knobs.hpp" // GB9: PGS_HIER_BINNING mode parser (parallel-gs fork)
#if defined(__APPLE__)
#include <vulkan/vulkan_metal.h> // HR1: VK_EXT_metal_objects (IOSurface export)
#endif
#endif

// HR1 prototype: shared-frame mailbox for PS2X_PRESENT_ZERO_COPY.
namespace ps2x_present_share
{
namespace
{
std::mutex g_shareMutex;
SharedFrame g_shareFrame;
} // namespace

bool enabled()
{
#if defined(__APPLE__) && TARGET_OS_OSX
    static const bool on = [] {
        const char *v = std::getenv("PS2X_PRESENT_ZERO_COPY");
        return v && std::strcmp(v, "1") == 0;
    }();
    return on;
#else
    return false;
#endif
}

void publish(const SharedFrame &frame)
{
    std::lock_guard<std::mutex> lock(g_shareMutex);
    g_shareFrame = frame;
}

bool latest(SharedFrame &out)
{
    std::lock_guard<std::mutex> lock(g_shareMutex);
    out = g_shareFrame;
    return out.surface != nullptr;
}
} // namespace ps2x_present_share

namespace ps2x_gs_parallel
{
namespace
{
struct Counters
{
    std::atomic<uint64_t> gifPackets{0}, gifBytes{0}, regWrites{0}, presents{0}, nullScanouts{0};
    std::atomic<uint64_t> presentNanos{0}, readbackNanos{0}, snapshots{0};
    std::atomic<uint64_t> copyNanos{0}; // HR1: CPU stride/alpha copy after the map
    std::atomic<uint64_t> unsupportedClears{0}, unsupportedVramIo{0}, localToHostBytes{0};
    std::atomic<bool> initOk{false}, initFailed{false};
};

Counters &counters()
{
    static Counters c;
    return c;
}

uint64_t nowNanos()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

void logStats(const char *why)
{
    const Stats s = stats();
    const double presentMs = s.presents ? (static_cast<double>(s.presentNanos) / 1e6) / s.presents : 0.0;
    const double readbackMs = s.presents ? (static_cast<double>(s.readbackNanos) / 1e6) / s.presents : 0.0;
    const double copyMs = s.presents ? (static_cast<double>(counters().copyNanos.load()) / 1e6) / s.presents : 0.0;
    std::cerr << "[gs:parallel] " << why << " gif=" << s.gifPackets << " gif_bytes=" << s.gifBytes
              << " regs=" << s.regWrites << " presents=" << s.presents << " null_scanouts=" << s.nullScanouts
              << " present_ms_avg=" << presentMs << " readback_ms_avg=" << readbackMs
              << " copy_ms_avg=" << copyMs
              << " snapshots=" << s.snapshots << " unsupported_clears=" << s.unsupportedClears
              << " unsupported_vram_io=" << s.unsupportedVramIo << " l2h_bytes=" << s.localToHostBytes
              << std::endl;
}

uint32_t transferBitsPerPixel(uint8_t psm)
{
    switch (psm)
    {
    case 0x00: // CT32
    case 0x30: // Z32
        return 32u;
    case 0x1B: // T8H: stored in 32-bit words, transfers 8 bits/px
        return 8u;
    case 0x01: // CT24
    case 0x31: // Z24
        return 24u;
    case 0x02: // CT16
    case 0x0A: // CT16S
    case 0x32: // Z16
    case 0x3A: // Z16S
        return 16u;
    case 0x13: // T8
        return 8u;
    case 0x14: // T4
    case 0x24: // T4HL
    case 0x2C: // T4HH
        return 4u;
    default:
        return 32u;
    }
}

#ifdef PS2X_HAS_PARALLEL_SHADOW
#if defined(__ANDROID__)
// G43's Turnip HAL ABI, ported from tools/gs_dump_replayer.cpp. The
// exported HMI is an object, not a vkGetInstanceProcAddr function.
struct HalModuleMethods
{
    int (*open)(const void *module, const char *id, void **device);
};
struct HalModule
{
    uint32_t tag;
    uint16_t moduleApiVersion;
    uint16_t halApiVersion;
    const char *id;
    const char *name;
    const char *author;
    HalModuleMethods *methods;
};
struct HalDevice
{
    uint32_t tag;
    uint32_t version;
    void *module;
    uint64_t reserved[12];
    int (*close)(void *device);
    void *enumerateInstanceExtensions;
    void *createInstance;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr;
};
static_assert(sizeof(void *) == 8, "Turnip HAL loader requires arm64");
static_assert(offsetof(HalModule, methods) == 32, "HMI methods offset");
static_assert(offsetof(HalDevice, close) == 112, "HAL close offset");
static_assert(offsetof(HalDevice, enumerateInstanceExtensions) == 120, "HAL enumerate offset");
static_assert(offsetof(HalDevice, createInstance) == 128, "HAL create offset");
static_assert(offsetof(HalDevice, getInstanceProcAddr) == 136, "HAL get-proc offset");

bool initTurnipLoader()
{
    void *library = dlopen("libvulkan_freedreno.so", RTLD_NOW | RTLD_LOCAL);
    if (!library)
    {
        const char *error = dlerror();
        std::cerr << "[gs:parallel] Turnip dlopen failed: " << (error ? error : "unknown") << std::endl;
        return false;
    }
    dlerror(); // Clear a previous error before dlsym.
    auto *hmi = reinterpret_cast<HalModule *>(dlsym(library, "HMI"));
    const char *symbolError = dlerror();
    if (symbolError || !hmi)
    {
        std::cerr << "[gs:parallel] Turnip HMI dlsym failed: "
                  << (symbolError ? symbolError : "null HMI") << std::endl;
        dlclose(library);
        return false;
    }
    Dl_info mapped = {};
    if (dladdr(hmi, &mapped) && mapped.dli_fname)
        std::cerr << "[gs:parallel] Turnip HMI mapped base=" << mapped.dli_fbase
                  << " file=" << mapped.dli_fname << std::endl;
    else
        std::cerr << "[gs:parallel] Turnip dladdr(HMI) failed" << std::endl;

    if (hmi->tag != 0x48574d54 || !hmi->methods || !hmi->methods->open)
    {
        std::cerr << "[gs:parallel] Turnip HMI layout/open invalid" << std::endl;
        dlclose(library);
        return false;
    }
    void *device = nullptr;
    const int rc = hmi->methods->open(hmi, "vulkan0", &device);
    std::cerr << "[gs:parallel] Turnip HAL open rc=" << rc << " device=" << device << std::endl;
    if (rc != 0 || !device)
    {
        std::cerr << "[gs:parallel] Turnip HAL open failed" << std::endl;
        dlclose(library);
        return false;
    }
    auto *hal = reinterpret_cast<HalDevice *>(device);
    std::cerr << "[gs:parallel] Turnip HAL tag=" << std::hex << hal->tag << std::dec
              << " close=" << reinterpret_cast<const void *>(hal->close)
              << " enum_ext=" << hal->enumerateInstanceExtensions
              << " create_inst=" << hal->createInstance
              << " get_proc=" << reinterpret_cast<const void *>(hal->getInstanceProcAddr) << std::endl;
    if (!hal->getInstanceProcAddr)
    {
        std::cerr << "[gs:parallel] Turnip HAL get-proc is null" << std::endl;
        if (hal->close)
            hal->close(device);
        dlclose(library);
        return false;
    }
    // Keep the HAL device and library resident for the lifetime of Granite.
    // Releasing them here would leave Granite with a dangling proc address.
    return Vulkan::Context::init_loader(hal->getInstanceProcAddr);
}
#endif

class GSParallelBackend final : public GSRasterBackend
{
public:
    explicit GSParallelBackend(const GSRegisters *priv) : m_priv(priv) {}

    ~GSParallelBackend() override
    {
        delete m_iface;
        delete m_device;
        delete m_ctx;
    }

    void Initialize(uint8_t *vram, uint32_t vramSize) override
    {
        // The frontend hands VRAM over through this buffer on a backend
        // swap (GS::setRasterBackend). Keep it and upload on first init.
        m_handoff = vram;
        m_handoffSize = vramSize;
        m_needHandoff = (vram != nullptr && vramSize != 0u);
        if (m_initOk && m_needHandoff)
            uploadHandoff();
    }

    void Reset() override
    {
        if (m_initOk)
            m_iface->reset_context_state();
    }

    void Submit(const GSPrimitiveBatch &) override {}
    void UploadImage(const uint8_t *, uint32_t) override {}

    void BeginTransfer(const GSTransferCommand &command) override
    {
        // paraLLEl runs the transfer from the raw stream; only track how many
        // local->host bytes the EE may consume.
        if (command.direction == 1u)
        {
            const uint64_t bits = static_cast<uint64_t>(command.trxreg.rrw) * command.trxreg.rrh *
                                  transferBitsPerPixel(command.bitbltbuf.spsm);
            m_l2hPending = static_cast<uint32_t>((bits + 7u) / 8u);
        }
    }

    void Flush() override {}
    void TextureFlush() override {}
    void Sync(GSSyncReason) override {}

    PresentationFrame Present(const GSPresentationRequest &request) override
    {
        PresentationFrame out{};
        if (!ensureInit())
            return out;
        const uint64_t t0 = nowNanos();
        syncPriv();
        ParallelGS::VSyncInfo vsync = {};
        vsync.phase = static_cast<uint32_t>(request.vsyncTick & 1u);
        vsync.dst_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
        vsync.dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        vsync.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        vsync.adapt_to_internal_horizontal_resolution = true;
        // ST1 (local-only): SSX 3 renders a full frame every vsync into one
        // buffer (SMODE2 INT=1/FFMD=0, single circuit, DY even). The field
        // weave shows the previous tick's lines on alternating rows
        // (stripes on motion, phase flips each tick), so scan the full
        // buffer as-is like the CPU weave instead of deinterlacing.
        vsync.force_progressive = true;
        // HR1: PS2X_PGS_HIRES_SCANOUT=1 scans out 2x width/height from the
        // super-samples (paraLLEl needs SSAA >= 4 with both axes sampled;
        // it silently falls back to 1x scanout otherwise).
        vsync.high_resolution_scanout = m_hiresScanout;
        m_iface->flush();
        ParallelGS::ScanoutResult shot = m_iface->vsync(vsync);
        Counters &c = counters();
        const uint64_t n = c.presents.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (!shot.image)
        {
            const uint64_t nulls = c.nullScanouts.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (nulls <= 4u)
            {
                const auto &pr = m_iface->get_priv_register_state();
                std::cerr << "[gs:parallel] null scanout tick=" << request.vsyncTick
                          << " CMOD=" << pr.smode1.CMOD << " LC=" << pr.smode1.LC
                          << " INT=" << pr.smode2.INT << " FFMD=" << pr.smode2.FFMD << std::endl;
            }
            return out;
        }
        const uint32_t w = shot.image->get_width();
        const uint32_t h = shot.image->get_height();
        if (w == 0u || h == 0u || w > 4096u || h > 4096u)
            return out;
        if (w != m_lastScanW || h != m_lastScanH)
        {
            std::cerr << "[gs:parallel] scanout size " << w << "x" << h << " tick=" << request.vsyncTick
                      << " hires=" << (shot.high_resolution_scanout ? 1 : 0) << std::endl;
            m_lastScanW = w;
            m_lastScanH = h;
        }

#if defined(__APPLE__)
        if (m_zeroCopy && presentShared(*shot.image, w, h))
        {
            const uint64_t t1 = nowNanos();
            c.presentNanos.fetch_add(t1 - t0, std::memory_order_relaxed);
            if (n == 1u || n % 300u == 0u)
                logStats(n == 1u ? "first-present" : "periodic");
            return out; // empty: the presenter reads ps2x_present_share instead
        }
#endif
        const uint64_t r0 = nowNanos();
        auto cmd = m_device->request_command_buffer();
        cmd->image_barrier(*shot.image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        Vulkan::BufferCreateInfo info = {};
        info.size = static_cast<size_t>(w) * h * sizeof(uint32_t);
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.domain = Vulkan::BufferDomain::CachedHost;
        Vulkan::BufferHandle rb = m_device->create_buffer(info);
        cmd->copy_image_to_buffer(*rb, *shot.image, 0, {}, {w, h, 1}, 0, 0,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        m_device->submit(cmd);
        m_device->wait_idle();
        const uint8_t *px = static_cast<const uint8_t *>(
            m_device->map_host_buffer(*rb, Vulkan::MEMORY_ACCESS_READ_BIT));
        const uint64_t r1 = nowNanos();
        if (!px)
            return out;

        // Frontend contract: 640-pixel row stride (kHostFrameWidth), rows
        // of `width` valid pixels, height <= 512. HR1: frames that don't
        // fit (high-resolution scanout) are packed at stride = width; the
        // frontend infers that from the size (GS::copyLatchedHostPresentationFrame).
        const bool large = w > 640u || h > 512u;
        const uint32_t kStride = large ? w : 640u;
        out.width = std::min<uint32_t>(w, kStride);
        out.height = large ? h : std::min<uint32_t>(h, 512u);
        out.pixels.assign(static_cast<size_t>(kStride) * out.height * 4u, 0u);
        // DK1: presentation pixels must be opaque. The shared presenter
        // uploads them to a raylib RGBA texture and alpha-blends the game
        // quad over black, so paraLLEl scanout alpha (PS2 alpha, 0x80 on
        // content rows) would show the frame at half brightness. The CPU
        // backend already normalizes (normalizePresentationAlpha in
        // gs_cpu_backend.cpp); fold the same into the copy here.
        for (uint32_t y = 0; y < out.height; ++y)
        {
            uint8_t *dst = out.pixels.data() + static_cast<size_t>(y) * kStride * 4u;
            std::memcpy(dst, px + static_cast<size_t>(y) * w * 4u,
                        static_cast<size_t>(out.width) * 4u);
            for (uint32_t x = 0; x < out.width; ++x)
                dst[x * 4u + 3u] = 255u;
        }
        out.displayFbp = static_cast<uint32_t>(m_priv ? (m_priv->dispfb1 & 0x1FFu) : 0u);
        out.sourceFbp = out.displayFbp;

        const uint64_t t1 = nowNanos();
        c.readbackNanos.fetch_add(r1 - r0, std::memory_order_relaxed);
        c.copyNanos.fetch_add(t1 - r1, std::memory_order_relaxed);
        c.presentNanos.fetch_add(t1 - t0, std::memory_order_relaxed);
        if (n == 1u || n % 300u == 0u)
            logStats(n == 1u ? "first-present" : "periodic");
        return out;
    }

    bool ClearFramebuffer(const GSContext &, uint32_t) override
    {
        counters().unsupportedClears.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }

    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
    {
        if (!dst || maxBytes < 16u || m_l2hPending == 0u || !ensureInit())
            return 0u;
        const uint32_t bytes = std::min<uint32_t>(maxBytes, (m_l2hPending + 15u) & ~15u) & ~15u;
        m_iface->read_transfer_fifo(dst, bytes / 16u);
        m_l2hPending = (bytes >= m_l2hPending) ? 0u : (m_l2hPending - bytes);
        counters().localToHostBytes.fetch_add(bytes, std::memory_order_relaxed);
        return bytes;
    }

    uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override
    {
        counters().unsupportedVramIo.fetch_add(1u, std::memory_order_relaxed);
        return 0u;
    }

    void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override
    {
        counters().unsupportedVramIo.fetch_add(1u, std::memory_order_relaxed);
    }

    void SnapshotVram(std::vector<uint8_t> &out) const override
    {
        auto *self = const_cast<GSParallelBackend *>(this);
        if (!self->ensureInit())
        {
            out.clear();
            return;
        }
        constexpr size_t kVram = 4u * 1024u * 1024u;
        self->m_iface->flush();
        const void *p = self->m_iface->map_vram_read(0, kVram);
        if (!p)
        {
            out.clear();
            return;
        }
        out.resize(kVram);
        std::memcpy(out.data(), p, kVram);
        counters().snapshots.fetch_add(1u, std::memory_order_relaxed);
    }

    GSTransferSnapshot GetTransferSnapshot() const override
    {
        GSTransferSnapshot s{};
        s.localToHostPendingBytes = m_l2hPending;
        return s;
    }

    bool WantsRawGif() const override { return true; }

    void RawGifPacket(uint32_t path, const uint8_t *data, uint32_t sizeBytes) override
    {
        if (!data || sizeBytes == 0u || !ensureInit())
            return;
        m_iface->gif_transfer(path, data, static_cast<size_t>(sizeBytes));
        Counters &c = counters();
        c.gifPackets.fetch_add(1u, std::memory_order_relaxed);
        c.gifBytes.fetch_add(sizeBytes, std::memory_order_relaxed);
    }

    void RawWriteRegister(uint8_t regAddr, uint64_t value) override
    {
        if (!ensureInit())
            return;
        m_iface->write_register(static_cast<ParallelGS::RegisterAddr>(regAddr), value);
        counters().regWrites.fetch_add(1u, std::memory_order_relaxed);
    }

private:
    bool ensureInit()
    {
        if (m_initOk)
            return true;
        if (m_initFailed)
            return false;
        m_initFailed = true; // cleared on success below
        std::cerr << "[gs:parallel] init: GRANITE_VULKAN_LIBRARY="
                  << (std::getenv("GRANITE_VULKAN_LIBRARY") ? std::getenv("GRANITE_VULKAN_LIBRARY") : "(unset)")
                  << std::endl;
        bool loaderOk = false;
#if defined(__ANDROID__)
        const char *turnip = std::getenv("PS2X_GS_TURNIP");
        if (turnip && std::strcmp(turnip, "1") == 0)
        {
            std::cerr << "[gs:parallel] Turnip requested via PS2X_GS_TURNIP=1" << std::endl;
            loaderOk = initTurnipLoader();
        }
        else
#endif
            loaderOk = Vulkan::Context::init_loader(nullptr);
        if (!loaderOk)
            return fail("Context::init_loader failed");
        m_ctx = new Vulkan::Context();
        m_ctx->set_num_thread_indices(1);
        constexpr uint32_t kFlags = Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
                                    Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
                                    Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT;
        // HR1 prototype: IOSurface export needs VK_EXT_metal_objects.
        m_zeroCopy = ps2x_present_share::enabled();
        static const char *const kZeroCopyExt[] = {"VK_EXT_metal_objects"};
        if (!m_ctx->init_instance_and_device(nullptr, 0, m_zeroCopy ? kZeroCopyExt : nullptr,
                                             m_zeroCopy ? 1u : 0u, kFlags))
            return fail("init_instance_and_device failed");
        if (m_zeroCopy)
            std::cerr << "[gs:parallel] zero-copy present (IOSurface) requested" << std::endl;
        m_device = new Vulkan::Device();
        m_device->set_context(*m_ctx);
        m_device->init_frame_contexts(4);
        m_iface = new ParallelGS::GSInterface();
        ParallelGS::GSOptions opts = {};
        readQualityKnobs(opts);
        if (!m_iface->init(m_device, opts))
            return fail("GSInterface::init failed");
        ParallelGS::DebugMode dm;
        dm.feedback_render_target = false; // G26-corrected default (as G44)
        m_iface->set_debug_mode(dm);
        logGpuPath(dm); // TL1: one-time GPU path line (default-on, no per-frame cost)
        m_initFailed = false;
        m_initOk = true;
        counters().initOk.store(true);
        if (m_needHandoff)
            uploadHandoff();
        std::cerr << "[gs:parallel] init ok (live backend on the GS worker thread)" << std::endl;
        return true;
    }

    // HR1: output-quality knobs, default off (= 1x, native scanout).
    // PS2X_PGS_SSAA=1|2|4|8|16 -> GSOptions::super_sampling (paraLLEl clamps
    // to the device max: X4 without subgroup-size control, X8/X16 with it);
    // PS2X_PGS_SSAA_TEXTURES=1 -> super_sampled_textures;
    // PS2X_PGS_HIRES_SCANOUT=1 -> VSyncInfo::high_resolution_scanout.
    void readQualityKnobs(ParallelGS::GSOptions &opts)
    {
        const char *ssaa = std::getenv("PS2X_PGS_SSAA");
        const int rate = ssaa ? std::atoi(ssaa) : 1;
        switch (rate)
        {
        case 2: opts.super_sampling = ParallelGS::SuperSampling::X2; break;
        case 4: opts.super_sampling = ParallelGS::SuperSampling::X4; break;
        case 8: opts.super_sampling = ParallelGS::SuperSampling::X8; break;
        case 16: opts.super_sampling = ParallelGS::SuperSampling::X16; break;
        default: opts.super_sampling = ParallelGS::SuperSampling::X1; break;
        }
        const char *tex = std::getenv("PS2X_PGS_SSAA_TEXTURES");
        opts.super_sampled_textures = tex && std::strcmp(tex, "1") == 0;
        const char *hires = std::getenv("PS2X_PGS_HIRES_SCANOUT");
        m_hiresScanout = hires && std::strcmp(hires, "1") == 0;
        std::cerr << "[gs:parallel] quality ssaa=" << static_cast<int>(opts.super_sampling)
                  << " (asked " << (ssaa ? ssaa : "unset") << ")"
                  << " ssaa_textures=" << (opts.super_sampled_textures ? 1 : 0)
                  << " hires_scanout=" << (m_hiresScanout ? 1 : 0) << std::endl;
    }

#if defined(__APPLE__)
    // HR1 prototype: blit the scanout into a rotating IOSurface-backed image
    // and publish the surface. GPU-side copy only; waits on its own fence
    // (the readback path waits for the whole device).
    struct ShareSlot
    {
        Vulkan::ImageHandle image;
        void *surface = nullptr;
    };
    bool createShareSlots(uint32_t w, uint32_t h)
    {
        auto exportFn = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
            vkGetDeviceProcAddr(m_device->get_device(), "vkExportMetalObjectsEXT"));
        if (!exportFn)
            return false;
        for (auto &slot : m_share)
        {
            if (slot.image)
                m_shareGraveyard.push_back(slot.image); // GL may still hold the old surfaces
            Vulkan::ImageCreateInfo info = Vulkan::ImageCreateInfo::render_target(w, h, VK_FORMAT_B8G8R8A8_UNORM);
            info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT;
            info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkExportMetalObjectCreateInfoEXT exportInfo = {VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT};
            exportInfo.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_IOSURFACE_BIT_EXT;
            info.pnext = &exportInfo;
            slot.image = m_device->create_image(info);
            if (!slot.image)
                return false;
            VkExportMetalIOSurfaceInfoEXT surf = {VK_STRUCTURE_TYPE_EXPORT_METAL_IO_SURFACE_INFO_EXT};
            surf.image = slot.image->get_image();
            VkExportMetalObjectsInfoEXT objs = {VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
            objs.pNext = &surf;
            exportFn(m_device->get_device(), &objs);
            slot.surface = surf.ioSurface;
            std::cerr << "[gs:parallel] zero-copy slot " << w << "x" << h << " surface=" << slot.surface
                      << std::endl;
            if (!slot.surface)
                return false;
        }
        m_shareW = w;
        m_shareH = h;
        return true;
    }

    bool presentShared(Vulkan::Image &src, uint32_t w, uint32_t h)
    {
        if (m_zeroCopyBroken)
            return false;
        if ((w != m_shareW || h != m_shareH) && !createShareSlots(w, h))
        {
            std::cerr << "[gs:parallel] zero-copy setup failed; falling back to readback" << std::endl;
            m_zeroCopyBroken = true;
            return false;
        }
        ShareSlot &slot = m_share[m_shareIndex];
        m_shareIndex = (m_shareIndex + 1u) % 3u;
        auto cmd = m_device->request_command_buffer();
        cmd->image_barrier(src, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                           VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        cmd->image_barrier(*slot.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_2_NONE, 0,
                           VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkOffset3D zero = {0, 0, 0};
        const VkOffset3D extent = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
        cmd->blit_image(*slot.image, src, zero, extent, zero, extent, 0, 0, 0, 0, 1, VK_FILTER_NEAREST);
        cmd->image_barrier(*slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
        Vulkan::Fence fence;
        m_device->submit(cmd, &fence);
        fence->wait();
        ps2x_present_share::publish({slot.surface, w, h, ++m_shareSeq});
        return true;
    }
#endif

    bool fail(const char *what)
    {
        std::cerr << "[gs:parallel] FATAL: " << what << " (frames will be empty)" << std::endl;
        counters().initFailed.store(true);
        return false;
    }

    // TL1 Part 1: one-time GPU path line at paraLLEl backend init, so a Mac
    // vs Odin baseline can be checked for path equivalence. Default-on,
    // single line, runs once inside ensureInit (no per-frame cost). The
    // binning fields mirror GSRenderer::get_target_hierarchical_binning and
    // GSRenderer::set_hierarchical_binning_subgroup_config (gs/gs_renderer.cpp
    // on the parallel-gs fork, including its wave64-fixed preference): the
    // same Granite queries the renderer uses, so the line states the choice
    // the renderer will make for flat (hier=1) and hierarchical passes.
    void logGpuPath(const ParallelGS::DebugMode &dm)
    {
        const Vulkan::DeviceFeatures &feats = m_device->get_device_features();
        const VkPhysicalDeviceProperties &props = m_device->get_gpu_properties();
        const uint32_t subgroupSize = feats.vk11_props.subgroupSize;
        const uint32_t maxWgInv = props.limits.maxComputeWorkGroupInvocations;
        // Renderer clamp for a medium (<4096 prims, target 2) and a large
        // (>=4096 prims, target 4) pass. GB9: the effective rule follows
        // PGS_HIER_BINNING (force|auto|off, unset = today's platform
        // default); hier needs >=256 prims and >4x4 coarse tiles before
        // these targets apply.
        auto clampTarget = [subgroupSize, maxWgInv](uint32_t target) {
            uint32_t maxInv = subgroupSize * target * target;
            while (target > 0u && maxInv > maxWgInv)
            {
                maxInv /= 4u;
                target /= 2u;
            }
            return target;
        };
        // Renderer subgroup choice: exact wave from startLog2 down to
        // 4-wide, else the fork's fixed wave64, else the free 4..128 range.
        // Flat passes start at 64-wide (log2 6), hierarchical at 32 (log2 5).
        auto subgroupChoice = [this](uint32_t startLog2) -> const char * {
            static const char *names[] = {"wave4", "wave8", "wave16", "wave32", "wave64", "wave128"};
            for (uint32_t log2 = startLog2;; --log2)
            {
                if (m_device->supports_subgroup_size_log2(true, static_cast<uint8_t>(log2),
                                                           static_cast<uint8_t>(log2)))
                    return names[log2 - 2u];
                if (log2 == 2u)
                    break;
            }
            if (m_device->supports_subgroup_size_log2(true, 6, 6))
                return "wave64-fixed";
            return "free-4..128";
        };
        const char *desc = "plain";
        if (feats.supports_descriptor_buffer)
            desc = "buffer";
        else if (feats.descriptor_heap_features.descriptorHeap)
            desc = "heap";
        // GB9: print the effective rule (same parser the renderer uses).
        const bool flatAlways = ParallelGS::pgs_hier_binning_flat_always(ParallelGS::pgs_hier_binning_mode());
        std::cerr << "[gs-path] hier_rule=";
        if (flatAlways)
            std::cerr << "flat-always hier_t2=1 hier_t4=1";
        else
            std::cerr << "hier-if-large hier_t2=" << clampTarget(2u) << " hier_t4=" << clampTarget(4u);
        std::cerr << " subgroup_flat=" << subgroupChoice(6u) << " subgroup_hier=" << subgroupChoice(5u)
                  << " vk11_subgroup=" << subgroupSize << " max_wg_inv=" << maxWgInv << " desc=" << desc
                  << " desc_req=push+heap+buffer"
                  << " sampler_feedback=" << (dm.disable_sampler_feedback ? "off" : "on")
                  << " feedback_rt=" << (dm.feedback_render_target ? "on" : "off") << " gpu=" << props.deviceName
                  << std::endl;
    }

    void uploadHandoff()
    {
        m_needHandoff = false;
        const size_t n = std::min<size_t>(m_handoffSize, 4u * 1024u * 1024u);
        void *dst = m_iface->map_vram_write(0, n);
        if (!dst)
            return;
        std::memcpy(dst, m_handoff, n);
        m_iface->end_vram_write(0, n);
        m_iface->write_register(ParallelGS::RegisterAddr::TEXFLUSH, uint64_t(0));
    }

    void syncPriv()
    {
        if (!m_priv)
            return;
        ParallelGS::PrivRegisterState &dst = m_iface->get_priv_register_state();
#define GB3_SYNC_PRIV(field)                                                     \
    do                                                                           \
    {                                                                            \
        static_assert(sizeof(dst.field) >= sizeof(uint64_t), "priv image");      \
        const uint64_t v_ = m_priv->field;                                       \
        std::memcpy(&dst.field, &v_, sizeof(v_));                                \
    } while (0)
        GB3_SYNC_PRIV(pmode);
        GB3_SYNC_PRIV(smode1);
        GB3_SYNC_PRIV(smode2);
        GB3_SYNC_PRIV(srfsh);
        GB3_SYNC_PRIV(synch1);
        GB3_SYNC_PRIV(synch2);
        GB3_SYNC_PRIV(syncv);
        GB3_SYNC_PRIV(dispfb1);
        GB3_SYNC_PRIV(display1);
        GB3_SYNC_PRIV(dispfb2);
        GB3_SYNC_PRIV(display2);
        GB3_SYNC_PRIV(extbuf);
        GB3_SYNC_PRIV(extdata);
        GB3_SYNC_PRIV(extwrite);
        GB3_SYNC_PRIV(bgcolor);
#undef GB3_SYNC_PRIV
    }

    const GSRegisters *m_priv = nullptr;
    uint8_t *m_handoff = nullptr;
    uint32_t m_handoffSize = 0u;
    bool m_needHandoff = false;
    bool m_initOk = false;
    bool m_initFailed = false;
    uint32_t m_l2hPending = 0u;
    bool m_hiresScanout = false;              // HR1
    bool m_zeroCopy = false;                  // HR1 prototype (PS2X_PRESENT_ZERO_COPY)
#if defined(__APPLE__) && defined(PS2X_HAS_PARALLEL_SHADOW)
    ShareSlot m_share[3];
    std::vector<Vulkan::ImageHandle> m_shareGraveyard;
    uint32_t m_shareIndex = 0u, m_shareW = 0u, m_shareH = 0u;
    uint64_t m_shareSeq = 0u;
    bool m_zeroCopyBroken = false;
#endif
    uint32_t m_lastScanW = 0u, m_lastScanH = 0u; // HR1: log scanout size changes
    Vulkan::Context *m_ctx = nullptr;
    Vulkan::Device *m_device = nullptr;
    ParallelGS::GSInterface *m_iface = nullptr;
};
#endif
} // namespace

bool available()
{
#ifdef PS2X_HAS_PARALLEL_SHADOW
    return true;
#else
    return false;
#endif
}

bool requested()
{
    static const bool on = [] {
        const char *env = std::getenv("PS2X_GS_BACKEND");
        return env && std::strcmp(env, "parallel") == 0;
    }();
    return on;
}

std::unique_ptr<GSRasterBackend> create(const GSRegisters *priv)
{
#ifdef PS2X_HAS_PARALLEL_SHADOW
    return std::make_unique<GSParallelBackend>(priv);
#else
    (void)priv;
    return nullptr;
#endif
}

Stats stats()
{
    const Counters &c = counters();
    Stats s;
    s.gifPackets = c.gifPackets.load();
    s.gifBytes = c.gifBytes.load();
    s.regWrites = c.regWrites.load();
    s.presents = c.presents.load();
    s.nullScanouts = c.nullScanouts.load();
    s.presentNanos = c.presentNanos.load();
    s.readbackNanos = c.readbackNanos.load();
    s.snapshots = c.snapshots.load();
    s.unsupportedClears = c.unsupportedClears.load();
    s.unsupportedVramIo = c.unsupportedVramIo.load();
    s.localToHostBytes = c.localToHostBytes.load();
    s.initOk = c.initOk.load();
    s.initFailed = c.initFailed.load();
    return s;
}
} // namespace ps2x_gs_parallel
