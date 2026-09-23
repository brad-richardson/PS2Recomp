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

#ifdef PS2X_HAS_PARALLEL_SHADOW
#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"
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
        if (!Vulkan::Context::init_loader(nullptr))
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
