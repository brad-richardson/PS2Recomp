// GB3 Part 2: paraLLEl-GS live backend. See ps2_gs_parallel_backend.h.
//
// Device/interface init, the priv-register copy and the scanout readback
// follow G44's shadow (ps2_gs_shadow.cpp: ensureInitLocked, syncPrivLocked,
// onPresentFrame's CachedHost copy), minus the shadow's SMODE1 override:
// GB3 Part 1 programs SMODE1 through SetGsCrt/sceGsResetGraph.
#include "runtime/gs/ps2_gs_parallel_backend.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#if defined(__ANDROID__) && defined(PS2X_HAS_PARALLEL_SHADOW)
#include <dlfcn.h>
#endif

#ifdef PS2X_HAS_PARALLEL_SHADOW
#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"
#include "pgs_env_knobs.hpp" // GB9: PGS_HIER_BINNING mode parser (parallel-gs fork)
#endif

namespace ps2x_gs_parallel
{
namespace
{
struct Counters
{
    std::atomic<uint64_t> gifPackets{0}, gifBytes{0}, regWrites{0}, presents{0}, nullScanouts{0};
    std::atomic<uint64_t> presentNanos{0}, readbackNanos{0}, snapshots{0};
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
    std::cerr << "[gs:parallel] " << why << " gif=" << s.gifPackets << " gif_bytes=" << s.gifBytes
              << " regs=" << s.regWrites << " presents=" << s.presents << " null_scanouts=" << s.nullScanouts
              << " present_ms_avg=" << presentMs << " readback_ms_avg=" << readbackMs
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
        // of `width` valid pixels, height <= 512.
        constexpr uint32_t kStride = 640u;
        out.width = std::min<uint32_t>(w, kStride);
        out.height = std::min<uint32_t>(h, 512u);
        out.pixels.assign(static_cast<size_t>(kStride) * out.height * 4u, 0u);
        for (uint32_t y = 0; y < out.height; ++y)
            std::memcpy(out.pixels.data() + static_cast<size_t>(y) * kStride * 4u,
                        px + static_cast<size_t>(y) * w * 4u, static_cast<size_t>(out.width) * 4u);
        out.displayFbp = static_cast<uint32_t>(m_priv ? (m_priv->dispfb1 & 0x1FFu) : 0u);
        out.sourceFbp = out.displayFbp;

        const uint64_t t1 = nowNanos();
        c.readbackNanos.fetch_add(r1 - r0, std::memory_order_relaxed);
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
        if (!m_ctx->init_instance_and_device(nullptr, 0, nullptr, 0, kFlags))
            return fail("init_instance_and_device failed");
        m_device = new Vulkan::Device();
        m_device->set_context(*m_ctx);
        m_device->init_frame_contexts(4);
        m_iface = new ParallelGS::GSInterface();
        const ParallelGS::GSOptions opts = {};
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
