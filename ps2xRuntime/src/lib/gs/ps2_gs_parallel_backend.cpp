// GB3 Part 2: paraLLEl-GS live backend. See ps2_gs_parallel_backend.h.
//
// Device/interface init, the priv-register copy and the scanout readback
// follow G44's shadow (ps2_gs_shadow.cpp: ensureInitLocked, syncPrivLocked,
// onPresentFrame's CachedHost copy), minus the shadow's SMODE1 override:
// GB3 Part 1 programs SMODE1 through SetGsCrt/sceGsResetGraph.
#include <type_traits>
#include "runtime/gs/ps2_gs_parallel_backend.h"
#include "runtime/gs/ps2_present_share.h"
#include "runtime/gs/ps2_present_vk.h"
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
#include <android/hardware_buffer.h>
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
#if defined(__ANDROID__)
#include <vulkan/vulkan_android.h> // VK1: AHardwareBuffer import
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
#if defined(__APPLE__) // macOS: GL blit (HR1 prototype); iOS: GLES texture cache (HR1 spike)
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
              << " unsupported_vram_io=" << s.unsupportedVramIo << " l2h_bytes=" << s.localToHostBytes;
#if defined(__ANDROID__)
    if (ps2x_present_vk::enabled())
    {
        char vk[1024];
        ps2x_present_vk::appendStats(vk, sizeof(vk));
        std::cerr << vk;
    }
#endif
    std::cerr << std::endl;
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
#if defined(__ANDROID__)
        destroyVkSlots();
#endif
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
        const uint64_t tf0 = nowNanos();
        m_iface->flush();
        const uint64_t tf1 = nowNanos();
        ParallelGS::ScanoutResult shot = m_iface->vsync(vsync);
        m_flushNs += tf1 - tf0; // VK1 Part 2A: where the present's wall goes
        m_vsyncNs += nowNanos() - tf1;
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
                logPeriodic(n);
            return out; // empty: the presenter reads ps2x_present_share instead
        }
#endif
#if defined(__ANDROID__)
        if (m_vkPresent && ps2x_present_vk::broken())
        {
            // The sink gave up (no usable layer): back to the readback path for this run.
            std::cerr << "[present-vk] sink broken; backend back to readback" << std::endl;
            destroyVkSlots();
            m_vkPresent = false;
        }
        if (m_vkPresent && presentVk(*shot.image, w, h, request.vsyncTick))
        {
            const uint64_t t1 = nowNanos();
            c.presentNanos.fetch_add(t1 - t0, std::memory_order_relaxed);
            if (n == 1u || n % 300u == 0u)
                logPeriodic(n);
            return out; // empty: the frame went to the SurfaceControl layer
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
        Vulkan::BufferHandle rb;
        uint32_t fw = w, fh = h; // dims of the frame delivered below
        if (m_pipeline)
        {
            // HR1: PS2X_PGS_PRESENT_PIPELINE=1. Two persistent readback
            // slots: submit this frame's copy with its own fence and deliver
            // the previous frame (one frame of latency), so the GS worker
            // no longer drains the GPU (wait_idle) on every present.
            ReadbackSlot &cur = m_rb[m_rbIndex];
            ReadbackSlot &prev = m_rb[m_rbIndex ^ 1u];
            m_rbIndex ^= 1u;
            if (cur.fence)
            {
                cur.fence->wait();
                cur.fence.reset();
            }
            if (!cur.buf || cur.buf->get_create_info().size < info.size)
                cur.buf = m_device->create_buffer(info);
            cmd->copy_image_to_buffer(*cur.buf, *shot.image, 0, {}, {w, h, 1}, 0, 0,
                                      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            m_device->submit(cmd, &cur.fence);
            cur.w = w;
            cur.h = h;
            if (!prev.fence)
                return out; // first present: nothing to deliver yet
            prev.fence->wait();
            prev.fence.reset();
            rb = prev.buf;
            fw = prev.w;
            fh = prev.h;
        }
        else
        {
            rb = m_device->create_buffer(info);
            cmd->copy_image_to_buffer(*rb, *shot.image, 0, {}, {w, h, 1}, 0, 0,
                                      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            m_device->submit(cmd);
            m_device->wait_idle();
        }
        const uint8_t *px = static_cast<const uint8_t *>(
            m_device->map_host_buffer(*rb, Vulkan::MEMORY_ACCESS_READ_BIT));
        const uint64_t r1 = nowNanos();
        if (!px)
            return out;

        // Frontend contract: 640-pixel row stride (kHostFrameWidth), rows
        // of `width` valid pixels, height <= 512. HR1: frames that don't
        // fit (high-resolution scanout) are packed at stride = width; the
        // frontend infers that from the size (GS::copyLatchedHostPresentationFrame).
        const bool large = fw > 640u || fh > 512u;
        const uint32_t kStride = large ? fw : 640u;
        out.width = std::min<uint32_t>(fw, kStride);
        out.height = large ? fh : std::min<uint32_t>(fh, 512u);
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
            std::memcpy(dst, px + static_cast<size_t>(y) * fw * 4u,
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
            logPeriodic(n);
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

    // SS1 save states: VRAM (host mirror after a flush), the raw register,
    // priv and GIF-path state paraLLEl decodes itself. SS3 (v3 tail): the
    // CLUT ring + renderer cursors + the interface palette indices (S2), and
    // the retained strip/fan vertices (S3). Not captured: SSAA planes
    // (cleared by the VRAM upload) and in-flight host->local transfers (the
    // save defers while one is live). The footer lets tests and future tools
    // find the tail without paraLLEl's struct sizes.
    static constexpr uint32_t kTailMagic = 0x33534750u; // "PGS3" LE
    static constexpr size_t kTailFooterSize = sizeof(uint64_t) + sizeof(uint32_t);
    void SavestateSave(std::vector<uint8_t> &out) override
    {
        static_assert(std::is_trivially_copyable_v<ParallelGS::RegisterState>, "RegisterState");
        static_assert(std::is_trivially_copyable_v<ParallelGS::PrivRegisterState>, "PrivRegisterState");
        static_assert(std::is_trivially_copyable_v<ParallelGS::GIFPath>, "GIFPath");
        out.clear();
        if (!ensureInit())
            return;
        constexpr size_t kVram = 4u * 1024u * 1024u;
        m_iface->flush();
        const void *p = m_iface->map_vram_read(0, kVram);
        if (!p)
            return;
        const auto put = [&out](const void *src, size_t n) {
            const auto *b = static_cast<const uint8_t *>(src);
            out.insert(out.end(), b, b + n);
        };
        put(p, kVram);
        put(&m_iface->get_register_state(), sizeof(ParallelGS::RegisterState));
        put(&m_iface->get_priv_register_state(), sizeof(ParallelGS::PrivRegisterState));
        for (uint32_t i = 0; i < 4u; ++i)
            put(&m_iface->get_gif_path(i), sizeof(ParallelGS::GIFPath));
        put(&m_l2hPending, sizeof(m_l2hPending));
#if defined(PARALLEL_GS_HAS_SAVESTATE_V3)
        // v3 tail: [u8 hasClut][clut?][u32 vcount][vcount x 36B verts]
        // then [u64 tailLen][u32 magic]. tailLen covers hasClut..verts.
        std::vector<uint8_t> tail;
        const auto tput = [&tail](const void *src, size_t n) {
            const auto *b = static_cast<const uint8_t *>(src);
            tail.insert(tail.end(), b, b + n);
        };
        std::vector<uint8_t> clut;
        uint32_t clutBase = 0u, clutNext = 0u, clutIface = 0u, clutLatest = 0u;
        uint8_t hasClut = 0u;
        if (m_iface->read_clut_state(clut, clutBase, clutNext, clutIface, clutLatest))
            hasClut = 1u;
        tput(&hasClut, 1u);
        if (hasClut)
        {
            const uint64_t n = clut.size();
            tput(&n, sizeof(n));
            tput(clut.data(), clut.size());
            tput(&clutBase, sizeof(clutBase));
            tput(&clutNext, sizeof(clutNext));
            tput(&clutIface, sizeof(clutIface));
            tput(&clutLatest, sizeof(clutLatest));
        }
        // S3: a strip/fan may span the save point, so its retained vertices
        // travel with the state (count 0 = empty queue).
        std::vector<uint8_t> vtx;
        if (!m_iface->read_vertex_queue_state(vtx))
        {
            out.clear(); // never a blob with a missing queue; the load refuses it
            return;
        }
        tput(vtx.data(), vtx.size());
        const uint64_t tailLen = tail.size();
        tput(&tailLen, sizeof(tailLen));
        tput(&kTailMagic, sizeof(kTailMagic));
        out.insert(out.end(), tail.begin(), tail.end());
#else
        // Optional tail: the CLUT ring + cursors (paraLLEl ss1-clut accessor).
        uint8_t hasClut = 0u;
#if defined(PARALLEL_GS_HAS_CLUT_STATE)
        std::vector<uint8_t> clut;
        uint32_t clutBase = 0u, clutNext = 0u;
        if (m_iface->read_clut_state(clut, clutBase, clutNext))
            hasClut = 1u;
        put(&hasClut, 1u);
        if (hasClut)
        {
            const uint64_t n = clut.size();
            put(&n, sizeof(n));
            put(clut.data(), clut.size());
            put(&clutBase, sizeof(clutBase));
            put(&clutNext, sizeof(clutNext));
        }
#else
        put(&hasClut, 1u);
#endif
#endif
    }

    bool SavestateIdle() const override
    {
        if (m_l2hPending != 0u)
            return false;
#if defined(PARALLEL_GS_HAS_CLUT_STATE)
        // Palette uploads wait for the next render pass; save after it.
        if (m_initOk && !m_iface->clut_state_idle())
            return false;
#endif
#if defined(PARALLEL_GS_HAS_SAVESTATE_V3)
        // S3: an in-flight host->local transfer keeps its payload/cursors
        // out of the state; the guest completes it, so the save waits.
        if (m_initOk && !m_iface->gs_transfer_idle())
            return false;
#endif
        return true;
    }

    std::string SavestateBusyReason() const override
    {
#if defined(PARALLEL_GS_HAS_SAVESTATE_V3)
        if (m_initOk && !m_iface->gs_transfer_idle())
            return "gs-transfer";
#endif
        return {};
    }

    bool SavestateLoad(const uint8_t *data, size_t size) override
    {
        constexpr size_t kVram = 4u * 1024u * 1024u;
        const size_t fixed = kVram + sizeof(ParallelGS::RegisterState) + sizeof(ParallelGS::PrivRegisterState) +
                             4u * sizeof(ParallelGS::GIFPath) + sizeof(m_l2hPending);
        if (size < fixed + 1u || !ensureInit())
            return false;
        m_needHandoff = false;
        void *dst = m_iface->map_vram_write(0, kVram);
        if (!dst)
            return false;
        std::memcpy(dst, data, kVram);
        m_iface->end_vram_write(0, kVram);
        m_iface->write_register(ParallelGS::RegisterAddr::TEXFLUSH, uint64_t(0));
        size_t off = kVram;
        std::memcpy(&m_iface->get_register_state(), data + off, sizeof(ParallelGS::RegisterState));
        off += sizeof(ParallelGS::RegisterState);
        std::memcpy(&m_iface->get_priv_register_state(), data + off, sizeof(ParallelGS::PrivRegisterState));
        off += sizeof(ParallelGS::PrivRegisterState);
        for (uint32_t i = 0; i < 4u; ++i)
        {
            std::memcpy(&m_iface->get_gif_path(i), data + off, sizeof(ParallelGS::GIFPath));
            off += sizeof(ParallelGS::GIFPath);
        }
        std::memcpy(&m_l2hPending, data + off, sizeof(m_l2hPending));
        off += sizeof(m_l2hPending);
        bool clutRestored = false;
#if defined(PARALLEL_GS_HAS_SAVESTATE_V3)
        if (size < off + 1u + kTailFooterSize)
            return false;
        uint32_t tailMagic = 0u;
        std::memcpy(&tailMagic, data + size - sizeof(tailMagic), sizeof(tailMagic));
        if (tailMagic != kTailMagic)
            return false;
        uint64_t tailLen = 0u;
        std::memcpy(&tailLen, data + size - kTailFooterSize, sizeof(tailLen));
        if (tailLen < 1u || tailLen > size || off + tailLen + kTailFooterSize != size)
            return false;
        const size_t tailEnd = off + static_cast<size_t>(tailLen);
        size_t t = off;
        const uint8_t hasClut = data[t++];
        if (hasClut)
        {
            uint64_t n = 0u;
            if (tailEnd < t + sizeof(n))
                return false;
            std::memcpy(&n, data + t, sizeof(n));
            t += sizeof(n);
            const size_t rest = tailEnd - t;
            if (n > rest || rest - static_cast<size_t>(n) < 4u * sizeof(uint32_t))
                return false;
            std::vector<uint8_t> clut(data + t, data + t + static_cast<size_t>(n));
            uint32_t clutBase = 0u, clutNext = 0u, clutIface = 0u, clutLatest = 0u;
            std::memcpy(&clutBase, data + t + n, sizeof(clutBase));
            std::memcpy(&clutNext, data + t + n + sizeof(clutBase), sizeof(clutNext));
            std::memcpy(&clutIface, data + t + n + 2u * sizeof(uint32_t), sizeof(clutIface));
            std::memcpy(&clutLatest, data + t + n + 3u * sizeof(uint32_t), sizeof(clutLatest));
            clutRestored = m_iface->write_clut_state(clut, clutBase, clutNext, clutIface, clutLatest);
            if (!clutRestored)
                return false;
            t += static_cast<size_t>(n) + 4u * sizeof(uint32_t);
        }
        // S3: the vertex queue runs to the tail end; the accessor validates
        // its count and exact size.
        if (tailEnd - t < sizeof(uint32_t))
            return false;
        if (!m_iface->write_vertex_queue_state(std::vector<uint8_t>(data + t, data + tailEnd)))
            return false;
#else
        const uint8_t hasClut = data[off++];
        if (hasClut)
        {
            uint64_t n = 0u;
            if (size < off + sizeof(n))
                return false;
            std::memcpy(&n, data + off, sizeof(n));
            off += sizeof(n);
            if (size != off + n + 2u * sizeof(uint32_t))
                return false;
#if defined(PARALLEL_GS_HAS_CLUT_STATE)
            std::vector<uint8_t> clut(data + off, data + off + n);
            uint32_t clutBase = 0u, clutNext = 0u;
            std::memcpy(&clutBase, data + off + n, sizeof(clutBase));
            std::memcpy(&clutNext, data + off + n + sizeof(clutBase), sizeof(clutNext));
            clutRestored = m_iface->write_clut_state(clut, clutBase, clutNext);
            if (!clutRestored)
                return false;
#else
            std::fprintf(stderr, "[savestate] warning: state has a paraLLEl CLUT but this build can't restore it\n");
#endif
        }
        else if (size != off)
            return false;
#endif
        auto &regs = m_iface->get_register_state();
        if (!clutRestored)
            regs.cached_cbp[0] = regs.cached_cbp[1] = ~0u; // stale CLUT: force CLD 4/5 reloads
        m_iface->clobber_register_state();
        return true;
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
#if defined(__ANDROID__)
        // VK1 prototype: AHardwareBuffer import for the SurfaceControl present.
        m_vkPresent = ps2x_present_vk::enabled();
        static const char *const kVkPresentExt[] = {"VK_ANDROID_external_memory_android_hardware_buffer",
                                                    "VK_EXT_queue_family_foreign"};
        if (m_vkPresent && !m_ctx->init_instance_and_device(nullptr, 0, kVkPresentExt, 2u, kFlags))
        {
            std::cerr << "[present-vk] device lacks AHardwareBuffer import; falling back to readback" << std::endl;
            ps2x_present_vk::fallBack("no AHardwareBuffer import extensions");
            m_vkPresent = false;
            delete m_ctx;
            m_ctx = new Vulkan::Context();
            m_ctx->set_num_thread_indices(1);
        }
        else if (m_vkPresent)
            std::cerr << "[present-vk] AHardwareBuffer import extensions enabled" << std::endl;
        if (!m_vkPresent)
#endif
        if (!m_ctx->init_instance_and_device(nullptr, 0, m_zeroCopy ? kZeroCopyExt : nullptr,
                                             m_zeroCopy ? 1u : 0u, kFlags))
            return fail("init_instance_and_device failed");
        if (m_zeroCopy)
            std::cerr << "[gs:parallel] zero-copy present (IOSurface) requested" << std::endl;
        m_device = new Vulkan::Device();
        m_device->set_context(*m_ctx);
        // VK1 Part 2A: PS2X_PGS_FRAME_CONTEXTS=N (2..16, default 4 = before). paraLLEl
        // advances Granite's frame context on every flush_submit, and advancing
        // waits for the fences of the context being recycled.
        m_frameContexts = 4u;
        if (const char *fc = std::getenv("PS2X_PGS_FRAME_CONTEXTS"))
        {
            const long v = std::strtol(fc, nullptr, 10);
            if (v >= 2 && v <= 16)
                m_frameContexts = static_cast<uint32_t>(v);
        }
        m_device->init_frame_contexts(m_frameContexts);
        std::cerr << "[gs:parallel] frame_contexts=" << m_frameContexts << std::endl;
#if defined(__ANDROID__)
        if (m_vkPresent)
        {
            m_getAhbProps = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
                vkGetDeviceProcAddr(m_device->get_device(), "vkGetAndroidHardwareBufferPropertiesANDROID"));
            if (!m_getAhbProps)
            {
                std::cerr << "[present-vk] vkGetAndroidHardwareBufferPropertiesANDROID missing; readback" << std::endl;
                ps2x_present_vk::fallBack("vkGetAndroidHardwareBufferPropertiesANDROID missing");
                m_vkPresent = false;
            }
        }
#endif
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
        const char *pipe = std::getenv("PS2X_PGS_PRESENT_PIPELINE");
        m_pipeline = pipe && std::strcmp(pipe, "1") == 0;
        // Mirror GSRenderer::get_max_supported_super_sampling (parallel-gs
        // gs_renderer.cpp): paraLLEl silently clamps to X4 unless the device
        // can pin compute subgroup sizes (MoltenVK can't: X4 on Apple).
        int maxRate = 4;
        if (m_device->supports_subgroup_size_log2(true, 3, 6))
            maxRate = 8;
        if (m_device->supports_subgroup_size_log2(true, 4, 6))
            maxRate = 16;
        std::cerr << "[gs:parallel] quality ssaa=" << std::min(static_cast<int>(opts.super_sampling), maxRate)
                  << " (asked " << (ssaa ? ssaa : "unset") << ", device max " << maxRate << ")"
                  << " ssaa_textures=" << (opts.super_sampled_textures ? 1 : 0)
                  << " hires_scanout=" << (m_hiresScanout ? 1 : 0)
                  << " present_pipeline=" << (m_pipeline ? 1 : 0) << std::endl;
    }

#if defined(__APPLE__)
    // HR1 prototype: blit the scanout into a rotating IOSurface-backed image
    // and publish the surface. GPU-side copy only; waits on its own fence
    // (the readback path waits for the whole device).
    struct ShareSlot
    {
        Vulkan::ImageHandle image;
        void *surface = nullptr;
        Vulkan::Fence fence; // pipelined mode: pending blit
        uint32_t w = 0u, h = 0u;
    };
    bool createShareSlots(uint32_t w, uint32_t h)
    {
        m_device->wait_idle(); // size change (rare): retire pending blits first
        for (auto &slot : m_share)
        {
            slot.fence.reset();
            if (slot.image)
                m_shareGraveyard.push_back(slot.image); // GL may still hold the old surfaces
            Vulkan::ImageCreateInfo info = Vulkan::ImageCreateInfo::render_target(w, h, VK_FORMAT_B8G8R8A8_UNORM);
            info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT;
            info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            // Our own CoreVideo-compatible surface, imported into the VkImage.
            slot.surface = ps2x_present_share::createSurface(w, h);
            if (!slot.surface)
                return false;
            VkImportMetalIOSurfaceInfoEXT importInfo = {VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT};
            importInfo.ioSurface = static_cast<IOSurfaceRef>(slot.surface);
            info.pnext = &importInfo;
            slot.image = m_device->create_image(info);
            if (!slot.image)
                return false;
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
        if (m_pipeline)
        {
            // HR1: publish the previous slot (one frame of latency) instead
            // of waiting for this blit, i.e. for all rendering before it.
            m_device->submit(cmd, &slot.fence);
            slot.w = w;
            slot.h = h;
            ShareSlot &prev = m_share[(m_shareIndex + 1u) % 3u]; // index before `slot`
            if (prev.fence)
            {
                prev.fence->wait();
                prev.fence.reset();
                ps2x_present_share::publish({prev.surface, prev.w, prev.h, ++m_shareSeq});
            }
            return true;
        }
        Vulkan::Fence fence;
        m_device->submit(cmd, &fence);
        fence->wait();
        ps2x_present_share::publish({slot.surface, w, h, ++m_shareSeq});
        return true;
    }
#endif

#if defined(__ANDROID__)
    // VK1 prototype (PS2X_PRESENT_VULKAN=1): four AHardwareBuffer-backed
    // images imported into Turnip; each scanout is blitted into one and the
    // buffer is queued on the SurfaceControl layer (ps2_present_vk_android.cpp).
    struct VkSlot
    {
        AHardwareBuffer *ahb = nullptr; // valid while id is live in the sink
        uint64_t id = 0u;               // the sink's allocation id
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        Vulkan::ImageHandle wrapped;
        Vulkan::Fence fence;
        uint32_t w = 0u, h = 0u;
    };
    static constexpr uint32_t kVkSlots = 4u;

    void destroyVkSlots()
    {
        if (!m_device)
            return;
        bool any = false;
        for (auto &slot : m_vk)
            any = any || slot.id != 0u;
        if (!any)
            return;
        m_device->wait_idle(); // our blits into these buffers are done
        const auto &t = m_device->get_device_table();
        for (auto &slot : m_vk)
        {
            slot.fence.reset();
            slot.wrapped.reset();
            if (slot.image)
                t.vkDestroyImage(m_device->get_device(), slot.image, nullptr);
            if (slot.memory)
                t.vkFreeMemory(m_device->get_device(), slot.memory, nullptr);
            // VK2: the sink keeps its reference until the compositor released
            // every queued use; the backend never writes this buffer again.
            ps2x_present_vk::retireBuffer(slot.id);
            slot = VkSlot{};
        }
        m_vkPrev = -1;
        m_vkW = m_vkH = 0u;
    }

    bool createVkSlots(uint32_t w, uint32_t h)
    {
        destroyVkSlots(); // size change (rare); waits for the GPU
        const auto &t = m_device->get_device_table();
        const VkDevice dev = m_device->get_device();
        constexpr VkImageUsageFlags kUsage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        for (auto &slot : m_vk)
        {
            slot.id = ps2x_present_vk::allocateBuffer(w, h, &slot.ahb);
            if (slot.id == 0u)
                return false;
            VkAndroidHardwareBufferFormatPropertiesANDROID fmt = {
                VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
            VkAndroidHardwareBufferPropertiesANDROID props = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
                                                              &fmt};
            const VkResult pr = m_getAhbProps(dev, slot.ahb, &props);
            if (&slot == &m_vk[0])
                std::cerr << "[present-vk] AHB props rc=" << pr << " size=" << props.allocationSize
                          << " memTypeBits=0x" << std::hex << props.memoryTypeBits << " format=" << std::dec
                          << fmt.format << " externalFormat=" << fmt.externalFormat << " features=0x" << std::hex
                          << fmt.formatFeatures << std::dec << std::endl;
            if (pr != VK_SUCCESS || props.memoryTypeBits == 0u)
                return false;
            VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
            ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
            VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = VK_FORMAT_R8G8B8A8_UNORM;
            ici.extent = {w, h, 1u};
            ici.mipLevels = 1u;
            ici.arrayLayers = 1u;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL; // required for AHB images; the driver maps the real layout
            ici.usage = kUsage;
            ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (t.vkCreateImage(dev, &ici, nullptr, &slot.image) != VK_SUCCESS)
                return false;
            VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
            imp.buffer = slot.ahb;
            VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
            ded.image = slot.image;
            VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
            mai.allocationSize = props.allocationSize;
            mai.memoryTypeIndex = static_cast<uint32_t>(__builtin_ctz(props.memoryTypeBits));
            if (t.vkAllocateMemory(dev, &mai, nullptr, &slot.memory) != VK_SUCCESS)
                return false;
            if (t.vkBindImageMemory(dev, slot.image, slot.memory, 0) != VK_SUCCESS)
                return false;
            Vulkan::ImageCreateInfo info = Vulkan::ImageCreateInfo::render_target(w, h, VK_FORMAT_R8G8B8A8_UNORM);
            info.usage = kUsage;
            info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            slot.wrapped = m_device->wrap_image(info, slot.image);
            if (!slot.wrapped)
                return false;
        }
        m_vkW = w;
        m_vkH = h;
        m_vkIndex = 0u;
        m_vkPrev = -1;
        m_vkEpoch = ps2x_present_vk::bufferEpoch(m_vk[0].id);
        std::cerr << "[present-vk] " << kVkSlots << " slots " << w << "x" << h << " imported, epoch " << m_vkEpoch
                  << std::endl;
        return true;
    }

    // PS2X_PRESENT_VK_COMPARE_TICKS="a,b": at those ticks the present is
    // synchronous and the scanout is also read back; the buffer SurfaceFlinger
    // gets is locked through gralloc and compared byte for byte (RGB).
    bool vkCompareDue(uint64_t tick)
    {
        if (!m_vkDumpParsed)
        {
            m_vkDumpParsed = true;
            if (const char *v = std::getenv("PS2X_PRESENT_VK_COMPARE_TICKS"))
            {
                const char *p = v;
                while (*p)
                {
                    char *end = nullptr;
                    const unsigned long long t = std::strtoull(p, &end, 10);
                    if (end == p)
                        break;
                    m_vkDumpTicks.push_back(t);
                    p = (*end == ',') ? end + 1 : end;
                }
            }
        }
        for (uint64_t &t : m_vkDumpTicks)
        {
            if (t != 0u && tick >= t)
            {
                t = 0u;
                return true;
            }
        }
        return false;
    }

    bool presentVk(Vulkan::Image &src, uint32_t w, uint32_t h, uint64_t tick)
    {
        if (m_vkW != 0u && ps2x_present_vk::poolEpoch() != m_vkEpoch)
        {
            // VK2 (RV5 B2): the window changed. Its layer's buffers are never
            // written again; they are released as their completions arrive.
            std::cerr << "[present-vk] window changed: slot pool (epoch " << m_vkEpoch << ") retired" << std::endl;
            destroyVkSlots();
        }
        if ((w != m_vkW || h != m_vkH) && !createVkSlots(w, h))
        {
            std::cerr << "[present-vk] slot setup failed; falling back to readback" << std::endl;
            destroyVkSlots();
            ps2x_present_vk::fallBack("AHardwareBuffer slot setup failed");
            m_vkPresent = false;
            return false;
        }
        // Queue the previous (pipelined) frame first: its blit is normally long
        // done, and once queued its slot counts as held.
        if (m_vkPrev >= 0)
        {
            VkSlot &prev = m_vk[m_vkPrev];
            if (prev.fence)
            {
                prev.fence->wait();
                prev.fence.reset();
            }
            ps2x_present_vk::queue(prev.id, prev.w, prev.h);
            m_vkPrev = -1;
        }
        // VK2 (RV5 B1): write only a slot the compositor has released. A
        // timeout or poll error grants nothing: skip this frame (the screen
        // keeps the last one); persistent failure falls back to readback.
        uint64_t ids[kVkSlots];
        for (uint32_t i = 0; i < kVkSlots; ++i)
            ids[i] = m_vk[i].id;
        const ps2x_present_vk::Pick pick =
            ps2x_present_vk::pickReusable(ids, static_cast<int>(kVkSlots), static_cast<int>(m_vkIndex), 100);
        if (pick.index < 0)
        {
            if (!pick.giveUp)
                return true; // frame skipped (counted by the sink)
            std::cerr << "[present-vk] no released slot for many frames; falling back to readback" << std::endl;
            destroyVkSlots();
            ps2x_present_vk::fallBack("compositor release waits failing");
            m_vkPresent = false;
            return false;
        }
        const uint32_t cur = static_cast<uint32_t>(pick.index);
        VkSlot &slot = m_vk[cur];
        m_vkIndex = (cur + 1u) % kVkSlots;
        if (slot.fence)
        {
            slot.fence->wait();
            slot.fence.reset();
        }
        const bool compare = vkCompareDue(tick);
        const uint32_t gfx = m_device->get_queue_info().family_indices[Vulkan::QUEUE_INDEX_GRAPHICS];
        auto cmd = m_device->request_command_buffer();
        cmd->image_barrier(src, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                           VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkImageMemoryBarrier2 acquire = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        acquire.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        acquire.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        acquire.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        acquire.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // whole image rewritten: no ownership acquire needed
        acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire.image = slot.image;
        acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        cmd->image_barriers(1, &acquire);
        const VkOffset3D zero = {0, 0, 0};
        const VkOffset3D extent = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
        cmd->blit_image(*slot.wrapped, src, zero, extent, zero, extent, 0, 0, 0, 0, 1, VK_FILTER_NEAREST);
        VkImageMemoryBarrier2 release = acquire; // hand the buffer to SurfaceFlinger/HWC
        release.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        release.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        release.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        release.dstAccessMask = 0;
        release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        release.srcQueueFamilyIndex = gfx;
        release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        cmd->image_barriers(1, &release);
        Vulkan::BufferHandle rb;
        if (compare)
        {
            Vulkan::BufferCreateInfo info = {};
            info.size = static_cast<size_t>(w) * h * sizeof(uint32_t);
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            info.domain = Vulkan::BufferDomain::CachedHost;
            rb = m_device->create_buffer(info);
            cmd->copy_image_to_buffer(*rb, src, 0, {}, {w, h, 1}, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                         VK_ACCESS_2_HOST_READ_BIT);
        }
        m_device->submit(cmd, &slot.fence);
        slot.w = w;
        slot.h = h;
        if (m_pipeline && !compare)
        {
            // Queued on the next present (one frame of latency, as the readback pipeline).
            m_vkPrev = static_cast<int>(cur);
            return true;
        }
        slot.fence->wait();
        slot.fence.reset();
        if (compare && rb)
        {
            const auto *px = static_cast<const uint8_t *>(m_device->map_host_buffer(*rb, Vulkan::MEMORY_ACCESS_READ_BIT));
            if (px)
                ps2x_present_vk::compareBuffer(slot.ahb, px, w, h, tick, std::getenv("PS2X_PRESENT_VK_DUMP_DIR"));
        }
        ps2x_present_vk::queue(slot.id, w, h);
        return true;
    }
#endif

    // VK1 Part 2A: stats line + paraLLEl sync counters, per present since the last line.
    void logPeriodic(uint64_t n)
    {
        logStats(n == 1u ? "first-present" : "periodic");
        if (!m_iface)
            return;
        const auto &sc = m_iface->get_sync_counters();
        const uint64_t v[8] = {sc.flush_submits.load(), sc.frame_context_advances.load(), sc.frame_context_ns.load(),
                               sc.timeline_waits.load(), sc.timeline_wait_ns.load(), sc.flush_submit_ns.load(),
                               m_flushNs, m_vsyncNs};
        const double dn = static_cast<double>(n - m_syncLastPresents);
        if (dn > 0.0)
        {
            std::cerr << "[gs:parallel] sync presents=" << n << " frame_contexts=" << m_frameContexts
                      << " flush_submits_per_present=" << (v[0] - m_syncLast[0]) / dn
                      << " frame_ctx_advances_per_present=" << (v[1] - m_syncLast[1]) / dn
                      << " frame_ctx_wait_ms_per_present=" << (v[2] - m_syncLast[2]) / 1e6 / dn
                      << " timeline_waits_per_present=" << (v[3] - m_syncLast[3]) / dn
                      << " timeline_wait_ms_per_present=" << (v[4] - m_syncLast[4]) / 1e6 / dn
                      << " flush_submit_ms_per_present=" << (v[5] - m_syncLast[5]) / 1e6 / dn
                      << " iface_flush_ms_per_present=" << (v[6] - m_syncLast[6]) / 1e6 / dn
                      << " iface_vsync_ms_per_present=" << (v[7] - m_syncLast[7]) / 1e6 / dn << std::endl;
        }
        for (int i = 0; i < 8; ++i)
            m_syncLast[i] = v[i];
        m_syncLastPresents = n;
    }

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
    bool m_pipeline = false;                  // HR1: PS2X_PGS_PRESENT_PIPELINE=1
    struct ReadbackSlot
    {
        Vulkan::BufferHandle buf;
        Vulkan::Fence fence;
        uint32_t w = 0u, h = 0u;
    };
    ReadbackSlot m_rb[2];
    uint32_t m_rbIndex = 0u;
#if defined(__APPLE__) && defined(PS2X_HAS_PARALLEL_SHADOW)
    ShareSlot m_share[3];
    std::vector<Vulkan::ImageHandle> m_shareGraveyard;
    uint32_t m_shareIndex = 0u, m_shareW = 0u, m_shareH = 0u;
    uint64_t m_shareSeq = 0u;
    bool m_zeroCopyBroken = false;
#endif
#if defined(__ANDROID__) && defined(PS2X_HAS_PARALLEL_SHADOW)
    VkSlot m_vk[kVkSlots];
    uint32_t m_vkIndex = 0u, m_vkW = 0u, m_vkH = 0u;
    int m_vkPrev = -1; // slot submitted by the previous present, queued on the next one
    uint32_t m_vkEpoch = 0u; // sink pool epoch the slots were allocated under
    bool m_vkPresent = false;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID m_getAhbProps = nullptr;
    std::vector<uint64_t> m_vkDumpTicks;
    bool m_vkDumpParsed = false;
#endif
    uint32_t m_frameContexts = 4u;    // VK1 Part 2A: PS2X_PGS_FRAME_CONTEXTS
    uint64_t m_syncLast[8] = {};       // sync counters at the last stats line
    uint64_t m_flushNs = 0u, m_vsyncNs = 0u; // wall in GSInterface::flush / vsync (Present)
    uint64_t m_syncLastPresents = 0u;
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
