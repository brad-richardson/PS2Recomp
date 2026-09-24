// GB3 Part 2: paraLLEl-GS live backend. See ps2_gs_parallel_backend.h.
//
// Device/interface init, the priv-register copy and the scanout readback
// follow G44's shadow (ps2_gs_shadow.cpp: ensureInitLocked, syncPrivLocked,
// onPresentFrame's CachedHost copy), minus the shadow's SMODE1 override:
// GB3 Part 1 programs SMODE1 through SetGsCrt/sceGsResetGraph.
#include "runtime/gs/ps2_gs_parallel_backend.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif
#if defined(__ANDROID__) && defined(PS2X_HAS_PARALLEL_SHADOW)
#include <dlfcn.h>
#endif

#ifdef PS2X_HAS_PARALLEL_SHADOW
#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"
#include "gs_util.hpp"
#include "n8d5_tile_spirv.hpp"
#endif
#include "runtime/gs/ps2_gs_psmct32.h"

namespace ps2x_gs_parallel
{
namespace
{
#ifdef PS2X_HAS_PARALLEL_SHADOW
constexpr uint32_t kSelectedWidth = 512u, kSelectedHeight = 224u;
constexpr size_t kSelectedBytes = size_t(kSelectedWidth) * kSelectedHeight * 4u;
using SelectedTiles = std::array<uint16_t, 448>;

std::string selectedSha256(const void *data, size_t bytes)
{
#if defined(__APPLE__)
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data, static_cast<CC_LONG>(bytes), digest);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto byte : digest)
        out << std::setw(2) << unsigned(byte);
    return out.str();
#else
    (void)data;
    (void)bytes;
    return "unavailable";
#endif
}

SelectedTiles selectedTileCounts(const uint8_t *rgba, uint64_t &occupied, uint32_t &active)
{
    SelectedTiles counts{};
    occupied = 0;
    active = 0;
    for (uint32_t y = 0; y < kSelectedHeight; ++y)
        for (uint32_t x = 0; x < kSelectedWidth; ++x)
        {
            const uint8_t *p = rgba + 4u * (size_t(y) * kSelectedWidth + x);
            counts[(y / 16u) * 32u + x / 16u] += std::max({p[0], p[1], p[2]}) >= 32u;
        }
    for (uint16_t n : counts)
    {
        occupied += n;
        active += n >= 32u;
    }
    return counts;
}

void selectedLogTiles(const char *name, const SelectedTiles &counts, uint64_t occupied, uint32_t active)
{
    std::array<uint8_t, 896> packed{};
    for (size_t i = 0; i < counts.size(); ++i)
    {
        packed[2 * i] = uint8_t(counts[i]);
        packed[2 * i + 1] = uint8_t(counts[i] >> 8);
    }
    std::cerr << "[n8d7f] " << name << " tiles=448 occupied=" << occupied
              << " active=" << active << " packed_sha256="
              << selectedSha256(packed.data(), packed.size()) << std::endl;
    std::cerr << "[n8d7f] " << name << "_tile_counts=";
    for (size_t i = 0; i < counts.size(); ++i)
        std::cerr << (i ? "," : "") << counts[i];
    std::cerr << std::endl;
}

bool selectedDecode(const uint8_t *vram, const ParallelGS::ScanoutResult &shot,
                    std::vector<uint8_t> &rgba)
{
    using namespace ParallelGS;
    rgba.resize(kSelectedBytes);
    const uint32_t base = shot.selected_fbp * PGS_BLOCKS_PER_PAGE;
    for (uint32_t y = 0; y < kSelectedHeight; ++y)
    {
        const uint32_t sy = shot.selected_dby + shot.selected_phase + y * shot.selected_stride;
        uint8_t *dst = rgba.data() + size_t(y) * kSelectedWidth * 4u;
        if (shot.selected_psm == PSMCT16 || shot.selected_psm == PSMCT16S ||
            shot.selected_psm == PSMZ16 || shot.selected_psm == PSMZ16S)
        {
            std::array<uint16_t, kSelectedWidth> raw{};
            switch (shot.selected_psm)
            {
            case PSMCT16: vram_readback<PSMCT16>(raw.data(), vram, base, shot.selected_fbw, shot.selected_dbx, sy, kSelectedWidth, 1, shot.selected_mask); break;
            case PSMCT16S: vram_readback<PSMCT16S>(raw.data(), vram, base, shot.selected_fbw, shot.selected_dbx, sy, kSelectedWidth, 1, shot.selected_mask); break;
            case PSMZ16: vram_readback<PSMZ16>(raw.data(), vram, base, shot.selected_fbw, shot.selected_dbx, sy, kSelectedWidth, 1, shot.selected_mask); break;
            default: vram_readback<PSMZ16S>(raw.data(), vram, base, shot.selected_fbw, shot.selected_dbx, sy, kSelectedWidth, 1, shot.selected_mask); break;
            }
            for (uint32_t x = 0; x < kSelectedWidth; ++x)
            {
                const uint16_t p = raw[x];
                dst[4u*x] = uint8_t((p & 31u) << 3);
                dst[4u*x+1] = uint8_t(((p >> 5) & 31u) << 3);
                dst[4u*x+2] = uint8_t(((p >> 10) & 31u) << 3);
                dst[4u*x+3] = (p & 0x8000u) ? 255u : 0u;
            }
        }
        else if (shot.selected_psm == PSMCT24 || shot.selected_psm == PSMZ24)
        {
            std::array<uint8_t, kSelectedWidth * 3u> raw{};
            if (shot.selected_psm == PSMCT24)
                vram_readback<PSMCT24>(raw.data(), vram, base, shot.selected_fbw, shot.selected_dbx, sy, kSelectedWidth, 1, shot.selected_mask);
            else
                vram_readback<PSMZ24>(raw.data(), vram, base, shot.selected_fbw, shot.selected_dbx, sy, kSelectedWidth, 1, shot.selected_mask);
            for (uint32_t x = 0; x < kSelectedWidth; ++x)
            {
                std::memcpy(dst + 4u*x, raw.data() + 3u*x, 3u);
                dst[4u*x+3] = 128u;
            }
        }
        else if (shot.selected_psm == PSMCT32 || shot.selected_psm == PSMZ32)
        {
            if (shot.selected_psm == PSMCT32)
                vram_readback<PSMCT32>(dst, vram, base, shot.selected_fbw, shot.selected_dbx, sy, kSelectedWidth, 1, shot.selected_mask);
            else
            {
                vram_readback<PSMZ32>(dst, vram, base, shot.selected_fbw, shot.selected_dbx, sy, kSelectedWidth, 1, shot.selected_mask);
                for (uint32_t x = 0; x < kSelectedWidth; ++x) dst[4u*x+3] = 128u;
            }
        }
        else return false;
    }
    return true;
}

// N8D7L: independent full-frame selected-input oracle. Decodes the same
// 512x224 selected field as selectedDecode, but through the fork's
// independently implemented GSPSMCT32::addrPSMCT32 literal tables
// (ps2_gs_psmct32.h), never the G43 bit-arithmetic swizzle. RGB-only threshold: the source
// alpha byte is ignored. Zero new probe bytes: reads the already-mapped
// selected_vram_staging. Worst-case added log is ~3 KiB (448 counts +
// controls + summaries), under the 16 KiB cap. No VRAM/PNG dump.
constexpr uint32_t kOracleVramBytes = 4u * 1024u * 1024u;
constexpr uint32_t kOracleVramMask = 0x3FFFFFu;
constexpr size_t kOracleTiles = 448u;
static_assert(kOracleTiles == 448u, "oracle vector must hold all 16x16 tiles");

struct OracleControl
{
    uint32_t gx, gy;
    uint32_t byte;
};
// Literal (x,y)->byte pairs from N8D7K REPORT §3, derived from the fork
// tables then cross-checked 8/8 against G43 after derivation. Controls,
// not a frame classifier: odd-y entries (e.g. 63,31) are never census
// pixels under stride 2, they only prove the read path.
constexpr OracleControl kOracleControls[8] = {
    {0u, 0u, 0xE0000u},
    {31u, 0u, 0xE0534u},
    {0u, 2u, 0xE0040u},
    {511u, 446u, 0x1BFFF4u},
    {64u, 0u, 0xE2000u},
    {0u, 32u, 0xF0000u},
    {63u, 31u, 0xE1FFCu},
    {64u, 32u, 0xF2000u},
};

bool oracleDecodeCensus(const uint8_t *vram, uint32_t fbp, uint32_t fbw,
                        uint32_t dbx, uint32_t dby, uint32_t phase, uint32_t stride,
                        SelectedTiles &out, uint64_t &occupied, uint32_t &active)
{
    if (!vram || fbw == 0u)
        return false;
    out.fill(0);
    occupied = 0;
    active = 0;
    const uint32_t block = fbp << 5u;
    for (uint32_t y = 0; y < kSelectedHeight; ++y)
        for (uint32_t x = 0; x < kSelectedWidth; ++x)
        {
            const uint32_t gx = x + dbx;
            const uint32_t gy = dby + phase + y * stride;
            if (gx >= 2048u || gy >= 2048u)
                return false;
            const uint32_t byte = GSPSMCT32::addrPSMCT32(block, fbw, gx, gy);
            if (byte + 4u > kOracleVramBytes)
                return false; // outside the 4 MiB map: OTHER, never wrap
            const uint32_t masked = byte & kOracleVramMask;
            uint32_t word = 0;
            std::memcpy(&word, vram + masked, 4u);
            const uint8_t r = uint8_t(word), g = uint8_t(word >> 8), b = uint8_t(word >> 16);
            out[(y / 16u) * 32u + x / 16u] += std::max({r, g, b}) >= 32u;
        }
    for (uint16_t n : out)
    {
        occupied += n;
        active += n >= 32u;
    }
    return out.size() == kOracleTiles;
}
#endif
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
        const char *tileEnv = std::getenv("PS2X_N8D5_TILE_CAPTURE");
        const char *selectedEnv = std::getenv("PS2X_N8D7F_SELECTED_CAPTURE");
        const bool selectedRequested = request.vsyncTick == 2050u && selectedEnv &&
                                       std::strcmp(selectedEnv, "1") == 0;
        const bool tileRequested = selectedRequested ||
            (request.vsyncTick == 2050u && tileEnv && std::strcmp(tileEnv, "1") == 0);
        vsync.capture_scanout_stages = tileRequested;
        vsync.capture_selected_input = selectedRequested;
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
        Vulkan::BufferHandle tileBuffer;
        Vulkan::ImageHandle controlImage;
        bool tileDispatched = false;
        struct StageCapture
        {
            const char *name;
            Vulkan::ImageHandle image;
            Vulkan::BufferHandle counts;
            uint32_t width = 0, height = 0, tiles = 0;
        };
        std::array<StageCapture, 2> stages{{
            {"circuit1", shot.circuit1, {}},
            {"pre_deinterlace_merged", shot.pre_deinterlace_merged, {}}
        }};
        if (tileRequested)
        {
            const uint32_t fbp = static_cast<uint32_t>(m_priv ? (m_priv->dispfb1 & 0x1ffu) : 0u);
            std::cerr << "[n8d5b] alignment tick=" << request.vsyncTick << " fbp=" << fbp
                      << " pmode=" << std::hex << request.pmode << std::dec
                      << " width=" << w << " height=" << h << std::endl;
            if (w == 512u && h == 448u && fbp == 112u && request.pmode == 0xff21u)
            {
                // A sampled 16x16 checkerboard verifies the same shader's texture
                // access and the small storage-buffer map before tiles are used.
                std::array<uint32_t, 256> controlPixels = {};
                for (uint32_t y = 0; y < 16u; ++y)
                    for (uint32_t x = 0; x < 16u; ++x)
                        controlPixels[y * 16u + x] = ((x + y) & 1u) ? 0xffffffffu : 0xff000000u;
                const Vulkan::ImageInitialData controlData = {controlPixels.data()};
                const auto controlInfo = Vulkan::ImageCreateInfo::immutable_2d_image(
                    16u, 16u, VK_FORMAT_R8G8B8A8_UNORM);
                controlImage = m_device->create_image(controlInfo, &controlData);
                Vulkan::BufferCreateInfo tileInfo = {};
                tileInfo.size = (1u + 32u * 28u) * sizeof(uint32_t);
                tileInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                tileInfo.domain = Vulkan::BufferDomain::CachedHost;
                tileBuffer = m_device->create_buffer(tileInfo);
                Vulkan::ResourceLayout layout = {};
                layout.sets[0].sampled_image_mask = 0x3u;
                layout.sets[0].fp_mask = 0x3u;
                layout.sets[0].storage_buffer_mask = 0x4u;
                layout.push_constant_size = 8u;
                auto *program = m_device->request_program(n8d5_tile_spirv, sizeof(n8d5_tile_spirv), &layout);
                if (controlImage && tileBuffer && program)
                {
                    struct Dimensions { uint32_t width, height; } dims{w, h};
                    cmd->image_barrier(*shot.image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                                       VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                    cmd->set_program(program);
                    cmd->set_texture(0, 0, shot.image->get_view(), Vulkan::StockSampler::NearestClamp);
                    cmd->set_texture(0, 1, controlImage->get_view(), Vulkan::StockSampler::NearestClamp);
                    cmd->set_storage_buffer(0, 2, *tileBuffer);
                    cmd->push_constants(&dims, 0, sizeof(dims));
                    cmd->dispatch((1u + 32u * 28u + 31u) / 32u, 1, 1);
                    cmd->barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
                    tileDispatched = true;
                    for (auto &stage : stages)
                    {
                        if (!stage.image)
                        {
                            std::cerr << "[n8d6a] stage=" << stage.name << " ERROR missing-image" << std::endl;
                            continue;
                        }
                        stage.width = stage.image->get_width();
                        stage.height = stage.image->get_height();
                        const uint32_t tilesX = stage.width / 16u;
                        const uint32_t tilesY = stage.height / 16u;
                        stage.tiles = tilesX * tilesY;
                        if (!stage.tiles || stage.width > 4096u || stage.height > 4096u)
                        {
                            std::cerr << "[n8d6a] stage=" << stage.name << " ERROR dimensions"
                                      << " width=" << stage.width << " height=" << stage.height << std::endl;
                            continue;
                        }
                        Vulkan::BufferCreateInfo stageInfo = {};
                        stageInfo.size = (1u + stage.tiles) * sizeof(uint32_t);
                        stageInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                        stageInfo.domain = Vulkan::BufferDomain::CachedHost;
                        stage.counts = m_device->create_buffer(stageInfo);
                        if (!stage.counts)
                        {
                            std::cerr << "[n8d6a] stage=" << stage.name << " ERROR buffer" << std::endl;
                            continue;
                        }
                        const Dimensions stageDims{stage.width, stage.height};
                        cmd->image_barrier(*stage.image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                                           VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                        cmd->set_program(program);
                        cmd->set_texture(0, 0, stage.image->get_view(), Vulkan::StockSampler::NearestClamp);
                        cmd->set_texture(0, 1, controlImage->get_view(), Vulkan::StockSampler::NearestClamp);
                        cmd->set_storage_buffer(0, 2, *stage.counts);
                        cmd->push_constants(&stageDims, 0, sizeof(stageDims));
                        cmd->dispatch((1u + stage.tiles + 31u) / 32u, 1, 1);
                        cmd->barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                     VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
                    }
                }
                else
                    std::cerr << "[n8d5b] control=ERROR resource-or-program" << std::endl;
            }
            else
                std::cerr << "[n8d5b] alignment=OTHER" << std::endl;
        }
        cmd->image_barrier(*shot.image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           tileDispatched ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT :
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
        if (selectedRequested)
        {
            std::cerr << "[n8d7f] tick=" << request.vsyncTick
                      << " fbp=" << shot.selected_fbp << " fbw=" << shot.selected_fbw
                      << " psm=" << shot.selected_psm << " dbx=" << shot.selected_dbx
                      << " dby=" << shot.selected_dby << " phase=" << shot.selected_phase
                      << " stride=" << shot.selected_stride << " mask=" << shot.selected_mask
                      << " samples=" << shot.selected_samples << " promoted=" << shot.selected_promoted
                      << " extent=" << shot.selected_width << "x" << shot.selected_height
                      << " valid=" << shot.selected_valid_width << "x" << shot.selected_valid_height
                      << " status=" << shot.selected_capture_status << std::endl;
            if (shot.selected_capture_status != 2 || !shot.selected_vram_staging ||
                !shot.circuit1_staging || !stages[0].counts ||
                w != 512u || h != 448u || request.pmode != 0xff21u ||
                shot.selected_fbp != 112u)
                std::cerr << "[n8d7f] OTHER capture-or-alignment" << std::endl;
            else
            {
                const auto *vram = static_cast<const uint8_t *>(m_device->map_host_buffer(
                    *shot.selected_vram_staging, Vulkan::MEMORY_ACCESS_READ_BIT));
                const auto *circuit = static_cast<const uint8_t *>(m_device->map_host_buffer(
                    *shot.circuit1_staging, Vulkan::MEMORY_ACCESS_READ_BIT));
                const auto *stageCounts = static_cast<const uint32_t *>(m_device->map_host_buffer(
                    *stages[0].counts, Vulkan::MEMORY_ACCESS_READ_BIT));
                if (!vram || !circuit || !stageCounts)
                    std::cerr << "[n8d7f] OTHER map" << std::endl;
                else
                {
                    std::vector<uint8_t> decoded;
                    if (!selectedDecode(vram, shot, decoded))
                        std::cerr << "[n8d7f] OTHER decode" << std::endl;
                    else
                    {
                        uint64_t inputOccupied = 0, circuitOccupied = 0, stageOccupied = 0;
                        uint32_t inputActive = 0, circuitActive = 0, stageActive = 0;
                        const auto inputTiles = selectedTileCounts(decoded.data(), inputOccupied, inputActive);
                        const auto circuitTiles = selectedTileCounts(circuit, circuitOccupied, circuitActive);
                        SelectedTiles stageTiles{};
                        for (size_t i = 0; i < stageTiles.size(); ++i)
                        {
                            stageTiles[i] = uint16_t(stageCounts[i + 1]);
                            stageOccupied += stageCounts[i + 1];
                            stageActive += stageCounts[i + 1] >= 32u;
                        }
                        std::cerr << "[n8d7f] bytes=5177344 vram_sha256="
                                  << selectedSha256(vram, 4u * 1024u * 1024u)
                                  << " input_sha256=" << selectedSha256(decoded.data(), decoded.size())
                                  << " circuit_sha256=" << selectedSha256(circuit, kSelectedBytes)
                                  << std::endl;
                        selectedLogTiles("input", inputTiles, inputOccupied, inputActive);
                        selectedLogTiles("circuit", circuitTiles, circuitOccupied, circuitActive);
                        selectedLogTiles("stage", stageTiles, stageOccupied, stageActive);
                        size_t inputCircuitEqual = 0, circuitStageEqual = 0;
                        for (size_t i = 0; i < inputTiles.size(); ++i)
                        {
                            inputCircuitEqual += inputTiles[i] == circuitTiles[i];
                            circuitStageEqual += circuitTiles[i] == stageTiles[i];
                        }
                        std::cerr << "[n8d7f] input_circuit_equal=" << inputCircuitEqual
                                  << "/448 circuit_stage_equal=" << circuitStageEqual << "/448"
                                  << std::endl;
                        // N8D7L: independent fork-table oracle on the same mapped
                        // snapshot. Flag unset (default) emits nothing and leaves
                        // output unchanged.
                        const char *oracleEnv = std::getenv("PS2X_N8D7L_ORACLE");
                        const bool oracleRequested = oracleEnv && std::strcmp(oracleEnv, "1") == 0;
                        if (oracleRequested)
                        {
                            std::cerr << "[n8d7l] tick=" << request.vsyncTick
                                      << " fbp=" << shot.selected_fbp << " fbw=" << shot.selected_fbw
                                      << " psm=" << shot.selected_psm << " dbx=" << shot.selected_dbx
                                      << " dby=" << shot.selected_dby << " phase=" << shot.selected_phase
                                      << " stride=" << shot.selected_stride << " mask=" << shot.selected_mask
                                      << " samples=" << shot.selected_samples
                                      << " promoted=" << shot.selected_promoted << std::endl;
                            if (shot.selected_psm != ParallelGS::PSMCT24 ||
                                shot.selected_fbp != 112u || shot.selected_fbw != 8u ||
                                shot.selected_samples != 1u || shot.selected_promoted != 0u ||
                                shot.selected_mask != kOracleVramMask)
                                std::cerr << "[n8d7l] OTHER metadata" << std::endl;
                            else
                            {
                                bool controlsOk = true;
                                for (const auto &c : kOracleControls)
                                {
                                    const uint32_t want =
                                        GSPSMCT32::addrPSMCT32(112u << 5u, 8u, c.gx, c.gy);
                                    if (want != c.byte || want + 4u > kOracleVramBytes)
                                    {
                                        controlsOk = false;
                                        break;
                                    }
                                }
                                if (!controlsOk)
                                    std::cerr << "[n8d7l] OTHER control-selfcheck" << std::endl;
                                else
                                {
                                    SelectedTiles oracleTiles{};
                                    uint64_t oracleOccupied = 0;
                                    uint32_t oracleActive = 0;
                                    if (!oracleDecodeCensus(vram, shot.selected_fbp, shot.selected_fbw,
                                                            shot.selected_dbx, shot.selected_dby,
                                                            shot.selected_phase, shot.selected_stride,
                                                            oracleTiles, oracleOccupied, oracleActive))
                                        std::cerr << "[n8d7l] OTHER address" << std::endl;
                                    else
                                    {
                                        selectedLogTiles("oracle", oracleTiles, oracleOccupied,
                                                         oracleActive);
                                        std::cerr << "[n8d7l] oracle_controls=";
                                        for (size_t i = 0; i < 8u; ++i)
                                        {
                                            uint32_t word = 0;
                                            std::memcpy(&word,
                                                        vram + (kOracleControls[i].byte & kOracleVramMask),
                                                        4u);
                                            char buf[32];
                                            std::snprintf(buf, sizeof(buf), "%s0x%06X=0x%08X",
                                                          (i ? "," : ""), kOracleControls[i].byte, word);
                                            std::cerr << buf;
                                        }
                                        std::cerr << std::endl;
                                        size_t oracleInputEqual = 0;
                                        for (size_t i = 0; i < oracleTiles.size(); ++i)
                                            oracleInputEqual += oracleTiles[i] == inputTiles[i];
                                        std::cerr << "[n8d7l] oracle_input_equal=" << oracleInputEqual
                                                  << "/448" << std::endl;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (tileDispatched)
            for (const auto &stage : stages)
            {
                if (!stage.counts)
                    continue;
                const auto *counts = static_cast<const uint32_t *>(
                    m_device->map_host_buffer(*stage.counts, Vulkan::MEMORY_ACCESS_READ_BIT));
                if (!counts)
                {
                    std::cerr << "[n8d6a] stage=" << stage.name << " ERROR map" << std::endl;
                    continue;
                }
                uint64_t occupied = 0;
                uint32_t active = 0;
                for (uint32_t i = 1; i <= stage.tiles; ++i)
                {
                    occupied += counts[i];
                    active += counts[i] >= 32u;
                }
                std::cerr << "[n8d6a] stage=" << stage.name
                          << " width=" << stage.width << " height=" << stage.height
                          << " tiles=" << stage.tiles << " occupied=" << occupied
                          << " active=" << active << " control=" << counts[0] << std::endl;
            }
        if (tileDispatched)
        {
            const auto *counts = static_cast<const uint32_t *>(
                m_device->map_host_buffer(*tileBuffer, Vulkan::MEMORY_ACCESS_READ_BIT));
            if (!counts)
                std::cerr << "[n8d5b] control=ERROR map" << std::endl;
            else
            {
                std::cerr << "[n8d5b] control=" << counts[0] << " expected=128"
                          << (counts[0] == 128u ? " PASS" : " FAIL") << std::endl;
                std::cerr << "[n8d5b] sampled_tile_counts=";
                uint64_t sampledOccupied = 0;
                uint32_t sampledActive = 0;
                for (uint32_t i = 1; i <= 32u * 28u; ++i)
                {
                    std::cerr << (i == 1 ? "" : ",") << counts[i];
                    sampledOccupied += counts[i];
                    sampledActive += counts[i] >= 32u;
                }
                std::cerr << std::endl;
                if (counts[0] == 128u)
                    std::cerr << "[n8d5b] sampled_summary tiles=896 occupied=" << sampledOccupied
                              << " active=" << sampledActive << std::endl;
                std::cerr << "[n8d6a] stage=final width=" << w << " height=" << h
                          << " tiles=896 occupied=" << sampledOccupied
                          << " active=" << sampledActive << " control=" << counts[0] << std::endl;
            }
        }
        const uint8_t *px = static_cast<const uint8_t *>(
            m_device->map_host_buffer(*rb, Vulkan::MEMORY_ACCESS_READ_BIT));
        const uint64_t r1 = nowNanos();
        if (!px)
            return out;
        if (tileRequested && w == 512u && h == 448u)
        {
            std::cerr << "[n8d5b] raw_tile_counts=";
            uint64_t rawOccupied = 0;
            uint32_t rawActive = 0;
            for (uint32_t ty = 0; ty < 28u; ++ty)
                for (uint32_t tx = 0; tx < 32u; ++tx)
                {
                    uint32_t hits = 0;
                    for (uint32_t y = 0; y < 16u; ++y)
                        for (uint32_t x = 0; x < 16u; ++x)
                        {
                            const uint8_t *p = px + 4u * ((ty * 16u + y) * w + tx * 16u + x);
                            hits += std::max({p[0], p[1], p[2]}) >= 32u;
                        }
                    std::cerr << (tx == 0u && ty == 0u ? "" : ",") << hits;
                    rawOccupied += hits;
                    rawActive += hits >= 32u;
                }
            std::cerr << std::endl;
            std::cerr << "[n8d5b] raw_summary tiles=896 occupied=" << rawOccupied
                      << " active=" << rawActive << std::endl;
        }

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
