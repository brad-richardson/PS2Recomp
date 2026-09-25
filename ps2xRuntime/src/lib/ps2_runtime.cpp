#include "ps2_runtime.h"
#include "ps2_e3.h"
#include "ps2_e41_trace.h"
#include "ps2_e43_trace.h"
#include "ps2_e44_trace.h"
#include "ps2_gfx_stats.h"
#include "ps2_log.h"
#include "ps2_park_snapshot.h"
#include "ps2_present_fallback.h"
#include "ps2_present_geometry.h"
#include "ps2_pad_latch.h"
#include "ps2_virtual_pad.h"
#include "runtime/ps2_pad.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_stream_capture.h"
#include "runtime/gs/ps2_gs_shadow.h"
#include "runtime/gs/ps2_gs_parallel_backend.h"
#include "runtime/gs/ps2_present_share.h"
#include "ps2_e7.h"
#include "ps2_e15.h"
#include "ps2_pk.h"
#include "runtime/ee_scheduler.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_snd_audio_output.h"
#include "ps2_thread_affinity.h"
#include "ps2_host_backend.h"
#include "ps2_iop_host.h"
#include "ps2x/iop/iop_subsystem.h"
#if defined(PS2X_IOS)
#include "ps2_ios_runtime.h"
#endif

#include <iostream>
#include <fstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <chrono>
#include <cstdio>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <sstream>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace ps2_stubs
{
    void resetSifState();
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    // SLUS_207.72 stores its video choice in bits 20-21 of the first options
    // word. 0 is 4:3 and 2 is the menu's anamorphic choice. The game calls
    // 0x228C08 to apply that choice after defaults, menu edits, and profile
    // loading. Interpose at that call so all three paths obey the host option.
    std::atomic<bool> g_ssx3WidescreenActive{false};
    std::atomic<uint32_t> g_ssx3WidescreenMode{2u};
    constexpr uint32_t kSsx3OptionsWord = 0x00535610u;
    constexpr uint32_t kSsx3WidescreenMask = 0x00300000u;

    void applySsx3Widescreen(PS2Runtime &)
    {
        const char *env = std::getenv("PS2X_WIDESCREEN");
        const uint32_t mode = ps2x::present::ssx3WidescreenModeFromEnv(env);
        g_ssx3WidescreenMode.store(mode, std::memory_order_relaxed);
        g_ssx3WidescreenActive.store(true, std::memory_order_release);
        std::fprintf(stderr, "[widescreen] SSX3 mode=%u (PS2X_WIDESCREEN=%s)\n",
                     mode, env ? env : "default");
    }

    void enforceSsx3Widescreen(uint8_t *rdram, uint32_t sourcePc)
    {
        if (!rdram || !g_ssx3WidescreenActive.load(std::memory_order_acquire)) return;
        uint32_t word = 0u;
        std::memcpy(&word, rdram + kSsx3OptionsWord, sizeof(word));
        const uint32_t mode = g_ssx3WidescreenMode.load(std::memory_order_relaxed);
        const uint32_t wanted = (word & ~kSsx3WidescreenMask) | (mode << 20u);
        if (word == wanted) return;
        std::memcpy(rdram + kSsx3OptionsWord, &wanted, sizeof(wanted));
        std::fprintf(stderr, "[widescreen] apply source=0x%x guest_mode=%u host_mode=%u\n",
                     sourcePc, (word >> 20u) & 3u, mode);
    }

    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistoryImpl()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

PS2_REGISTER_GAME_OVERRIDE("ssx3-widescreen-default",
                           "SLUS_207.72",
                           0x00100008u,
                           0u,
                           applySsx3Widescreen);

// K1 P0: env-gated presentation-frame capture (PS2X_FRAME_DUMP_DIR).
// Unset/empty = disabled (zero behavior change). When set, saves the
// first two success uploads AND first two fallbacks (E3b per-path keeps)
// plus an always-overwritten latest pair:
//   upload-<seq>.png + .txt sidecar (frame number, tick, dimensions,
//   display/source FBP, preferred flag, fallback flag, FNV-1a hash,
//   SMODE2/PMODE, DISPLAY1/2 + DISPFB1/2 raw (ST1 diagnostic)) and
//   upload-latest.png/.txt rewritten every upload so
// the settled park frame survives SIGTERM.
namespace
{
const char *frameDumpDir()
{
    static const char *dir = [] {
        const char *env = std::getenv("PS2X_FRAME_DUMP_DIR");
        return (env && env[0] != '\0') ? env : nullptr;
    }();
    return dir;
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

void dumpPresentationFrame(const uint8_t *rgba,
                           uint32_t width,
                           uint32_t height,
                           uint64_t tick,
                           uint32_t displayFbp,
                           uint32_t sourceFbp,
                           bool preferred,
                           bool fallback,
                           uint64_t smode2,
                           uint64_t pmode,
                           // ST1 diagnostic (local-only): raw DISPLAY/DISPFB in the sidecar.
                           uint64_t display1,
                           uint64_t display2,
                           uint64_t dispfb1,
                           uint64_t dispfb2)
{
    const char *dir = frameDumpDir();
    if (!dir || !rgba || width == 0u || height == 0u || width > 4096u || height > 4096u)
    {
        return;
    }
    // Gate captures can request three bounded snapshots without encoding a
    // PNG on every present. Each target accepts the first frame within the
    // following two guest seconds, since present and vsync can be out of phase.
    static const std::array<uint64_t, 3> selectedTicks = [] {
        std::array<uint64_t, 3> ticks{};
        if (const char *env = std::getenv("PS2X_FRAME_DUMP_ONCE_TICKS"))
        {
            unsigned long long a = 0, b = 0, c = 0;
            if (std::sscanf(env, "%llu,%llu,%llu", &a, &b, &c) == 3)
                ticks = {static_cast<uint64_t>(a), static_cast<uint64_t>(b), static_cast<uint64_t>(c)};
        }
        return ticks;
    }();
    if (selectedTicks[0] != 0u)
    {
        static std::array<bool, 3> captured{};
        bool selected = false;
        for (size_t i = 0; i < selectedTicks.size(); ++i)
        {
            if (!captured[i] && tick >= selectedTicks[i] && tick < selectedTicks[i] + 120u)
            {
                captured[i] = true;
                selected = true;
                break;
            }
        }
        if (!selected)
            return;
    }
    static uint64_t s_dumpSeq = 0u;
    const uint64_t seq = s_dumpSeq++;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    Image img{};
    img.data = const_cast<uint8_t *>(rgba);
    img.width = static_cast<int>(width);
    img.height = static_cast<int>(height);
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    const size_t byteSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    const uint32_t hash = fnv1a32(rgba, byteSize);

    char pngPath[1024];
    char txtPath[1024];
    // E3b first-success addendum (K1 G4): per-path keep counters -- first two
    // success PNGs AND first two fallback PNGs (global seq still numbers).
    static uint64_t s_successKeep = 0u;
    static uint64_t s_fallbackKeep = 0u;
    bool keep = fallback ? (s_fallbackKeep++ < 2u) : (s_successKeep++ < 2u);
    // E15: preserve actual host uploads in the same dynamically armed interval.
    // Four additional image/metadata pairs maximum; observation only.
    static uint32_t s_alignedKeep = 0u;
    if (!fallback && ps2_e7::aligned() && ps2_e7::window(tick) && s_alignedKeep < 4u)
    {
        ++s_alignedKeep; keep = true;
        ps2_e7::event(tick,"e15-present","upload=%llu width=%u height=%u D=%u source=%u fnv32=0x%x smode2=0x%llx pmode=0x%llx",
            static_cast<unsigned long long>(seq),width,height,displayFbp,sourceFbp,hash,
            static_cast<unsigned long long>(smode2),static_cast<unsigned long long>(pmode));
    }
    if (keep)
    {
        std::snprintf(pngPath, sizeof(pngPath), "%s/%s-%llu.png", dir, fallback ? "fallback" : "upload",
                      static_cast<unsigned long long>(seq));
        ExportImage(img, pngPath);
    }
    std::snprintf(pngPath, sizeof(pngPath), "%s/%s-latest.png", dir, fallback ? "fallback" : "upload");
    ExportImage(img, pngPath);
    std::snprintf(txtPath, sizeof(txtPath), "%s/%s-latest.txt", dir, fallback ? "fallback" : "upload");
    if (keep)
    {
        char keepTxt[1024];
        std::snprintf(keepTxt, sizeof(keepTxt), "%s/%s-%llu.txt", dir, fallback ? "fallback" : "upload",
                      static_cast<unsigned long long>(seq));
        std::ofstream(keepTxt) << "seq=" << seq << " tick=" << tick << " size=" << width << "x" << height
                               << " displayFbp=" << displayFbp << " sourceFbp=" << sourceFbp
                               << " preferred=" << (preferred ? 1 : 0) << " fallback=" << (fallback ? 1 : 0)
                               << " fnv1a=" << std::hex << hash << std::dec << " smode2=0x" << std::hex
                               << smode2 << " pmode=0x" << pmode << " display1=0x" << display1 << " display2=0x" << display2 << " dispfb1=0x" << dispfb1 << " dispfb2=0x" << dispfb2 << std::dec << "\n";
    }
    std::ofstream(txtPath) << "seq=" << seq << " tick=" << tick << " size=" << width << "x" << height
                           << " displayFbp=" << displayFbp << " sourceFbp=" << sourceFbp
                           << " preferred=" << (preferred ? 1 : 0) << " fallback=" << (fallback ? 1 : 0)
                           << " fnv1a=" << std::hex << hash << std::dec << " smode2=0x" << std::hex << smode2
                           << " pmode=0x" << pmode << " display1=0x" << display1 << " display2=0x" << display2 << " dispfb1=0x" << dispfb1 << " dispfb2=0x" << dispfb2 << std::dec << "\n";
    std::cerr << "[frame:dump] seq=" << seq << " tick=" << tick << " size=" << width << "x" << height
              << " fbp=" << displayFbp << "/" << sourceFbp << " fallback=" << (fallback ? 1 : 0) << " fnv1a="
              << std::hex << hash << std::dec << std::endl;
}
} // namespace

// I26: on-screen virtual controls, drawn by the host over the presented frame
// (never into the guest frame). Layout and hit test in ps2_virtual_pad.h.
namespace
{
bool virtualPadWanted()
{
    const char *env = std::getenv("PS2X_VIRTUAL_PAD");
#if defined(PS2X_IOS)
    return ps2x::vpad::enabledFromEnv(env); // Settings > Virtual controls; default on
#else
    return env && env[0] == '1'; // desktop: dev-only, the mouse is the finger
#endif
}

// A connected controller hides the overlay once it has been used (any
// button, or a stick past half-way) since it connected. The iOS Simulator
// reports an always-connected device named "Gamepad" with nothing attached,
// so "connected" alone would hide the overlay for good.
bool gamepadInUse()
{
    static bool s_used = false;
    bool any = false;
    for (int i = 0; i < 4; ++i)
    {
        if (!IsGamepadAvailable(i))
            continue;
        any = true;
        for (int b = GAMEPAD_BUTTON_LEFT_FACE_UP; b <= GAMEPAD_BUTTON_RIGHT_THUMB && !s_used; ++b)
        {
            s_used = IsGamepadButtonDown(i, b);
        }
        for (int a = GAMEPAD_AXIS_LEFT_X; a <= GAMEPAD_AXIS_RIGHT_Y && !s_used; ++a)
        {
            s_used = std::fabs(GetGamepadAxisMovement(i, a)) > 0.5f;
        }
    }
    if (!any)
    {
        s_used = false;
    }
    return any && s_used;
}

int virtualPadTouches(float *xs, float *ys, int max, float screenWidth, float screenHeight)
{
#if defined(PS2X_IOS)
    const int n = ps2x::ios::touchPoints(xs, ys, max);
    for (int i = 0; i < n; ++i)
    {
        xs[i] *= screenWidth;
        ys[i] *= screenHeight;
    }
    return n;
#else
    (void)screenWidth;
    (void)screenHeight;
    if (max < 1 || !IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        return 0;
    }
    xs[0] = static_cast<float>(GetMouseX());
    ys[0] = static_cast<float>(GetMouseY());
    return 1;
#endif
}

void drawVirtualPad(const ps2x::vpad::Layout &layout, uint16_t pressed, const ps2x::vpad::StickVec &stick)
{
    using namespace ps2x::vpad;
    // I32: floating stick: a dim rest ring until touched, then base + knob.
    {
        const float bx = stick.active ? stick.ax : layout.stickRestX;
        const float by = stick.active ? stick.ay : layout.stickRestY;
        const Vector2 bc{bx, by};
        DrawCircleV(bc, layout.stickR, Color{255, 255, 255, static_cast<unsigned char>(stick.active ? 45 : 22)});
        DrawCircleLinesV(bc, layout.stickR, Color{255, 255, 255, static_cast<unsigned char>(stick.active ? 140 : 70)});
        const float knobR = layout.stickR * 0.42f;
        const Vector2 kc{bx + stick.x * layout.stickR * 0.58f, by + stick.y * layout.stickR * 0.58f};
        DrawCircleV(kc, knobR, Color{255, 255, 255, static_cast<unsigned char>(stick.active ? 120 : 50)});
    }
    for (const Button &b : layout.buttons)
    {
        const bool down = (pressed & b.mask) != 0u;
        const Vector2 c{b.x, b.y};
        DrawCircleV(c, b.r, Color{255, 255, 255, static_cast<unsigned char>(down ? 110 : 45)});
        DrawCircleLinesV(c, b.r, Color{255, 255, 255, 140});
        const float s = b.r * 0.45f;
        const Color ink{255, 255, 255, 200};
        switch (b.mask)
        {
        case kUp:
            DrawTriangle({b.x, b.y - s}, {b.x - s, b.y + s * 0.6f}, {b.x + s, b.y + s * 0.6f}, ink);
            break;
        case kDown:
            DrawTriangle({b.x, b.y + s}, {b.x + s, b.y - s * 0.6f}, {b.x - s, b.y - s * 0.6f}, ink);
            break;
        case kLeft:
            DrawTriangle({b.x - s, b.y}, {b.x + s * 0.6f, b.y + s}, {b.x + s * 0.6f, b.y - s}, ink);
            break;
        case kRight:
            DrawTriangle({b.x + s, b.y}, {b.x - s * 0.6f, b.y - s}, {b.x - s * 0.6f, b.y + s}, ink);
            break;
        case kCross:
            DrawLineEx({b.x - s, b.y - s}, {b.x + s, b.y + s}, 3.0f, Color{120, 170, 255, 230});
            DrawLineEx({b.x - s, b.y + s}, {b.x + s, b.y - s}, 3.0f, Color{120, 170, 255, 230});
            break;
        case kCircle:
            DrawRing(c, s * 0.8f, s * 1.05f, 0.0f, 360.0f, 32, Color{255, 110, 110, 230});
            break;
        case kSquare:
            DrawRectangleLinesEx({b.x - s * 0.85f, b.y - s * 0.85f, s * 1.7f, s * 1.7f}, 3.0f, Color{255, 140, 210, 230});
            break;
        case kTriangle:
            DrawTriangleLines({b.x, b.y - s}, {b.x - s, b.y + s * 0.75f}, {b.x + s, b.y + s * 0.75f}, Color{110, 230, 160, 230});
            break;
        default:
        {
            const int fontSize = std::max(8, static_cast<int>(b.r * (b.label[1] == '\0' || b.label[2] == '\0' ? 0.8f : 0.42f)));
            const int tw = MeasureText(b.label, fontSize);
            DrawText(b.label, static_cast<int>(b.x) - tw / 2, static_cast<int>(b.y) - fontSize / 2, fontSize, ink);
            break;
        }
        }
    }
}
} // namespace

// HR1: main-thread present cost split, reported by PS2X_THREAD_CPU_LOG=1.
static std::atomic<uint64_t> g_hr1LatchNs{0}, g_hr1UploadNs{0}, g_hr1Uploads{0};

#if defined(__APPLE__)
// HR1: PS2X_THREAD_CPU_LOG=1 prints cumulative user+system CPU ms per named
// thread of this process (Mach thread_info; works on iOS where no external
// per-thread tool can see the app), plus the main thread's present split.
static void logThreadCpu(uint64_t tick)
{
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return;
    std::string line = "[thread-cpu] tick=" + std::to_string(tick);
    for (mach_msg_type_number_t i = 0; i < count; ++i)
    {
        thread_basic_info_data_t info{};
        mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
        if (thread_info(threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) == KERN_SUCCESS)
        {
            char name[64] = {0};
            if (pthread_t pt = pthread_from_mach_thread_np(threads[i]))
                pthread_getname_np(pt, name, sizeof(name));
            const double ms = (info.user_time.seconds + info.system_time.seconds) * 1000.0 +
                              (info.user_time.microseconds + info.system_time.microseconds) / 1000.0;
            char buf[128];
            std::snprintf(buf, sizeof(buf), " %s#%u=%.0f", name[0] ? name : "t", i, ms);
            line += buf;
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
    char tail[160];
    std::snprintf(tail, sizeof(tail), " | main_latch_ms=%.0f main_upload_ms=%.0f uploads=%llu",
                  g_hr1LatchNs.load() / 1e6, g_hr1UploadNs.load() / 1e6,
                  static_cast<unsigned long long>(g_hr1Uploads.load()));
    std::fprintf(stderr, "%s%s\n", line.c_str(), tail);
}
#endif

static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);
    // HR1: the texture grows once to fit frames larger than FB_WIDTH x
    // FB_HEIGHT (paraLLEl high-resolution scanout); after that every frame
    // uploads its own w x h rectangle straight from s_scratch.
    const bool texGrown = tex.width != FB_WIDTH || tex.height != FB_HEIGHT;

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    const bool needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick;
    const auto hr1T0 = std::chrono::steady_clock::now();
    struct Hr1UploadTimer
    {
        std::chrono::steady_clock::time_point t0, t1;
        bool latched = false;
        ~Hr1UploadTimer()
        {
            if (!latched)
                return;
            const auto t2 = std::chrono::steady_clock::now();
            g_hr1LatchNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            g_hr1UploadNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
            ++g_hr1Uploads;
        }
    } hr1Timer{hr1T0, hr1T0};
    if (needsLatch)
    {
        rt->gs().latchHostPresentationFrame();
        hr1Timer.t1 = std::chrono::steady_clock::now();
        hr1Timer.latched = true;
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

#if defined(__APPLE__) && !defined(PS2X_IOS)
    // HR1 prototype (PS2X_PRESENT_ZERO_COPY=1): GPU blit from the backend's
    // IOSurface into the frame texture; no pixel bytes touch the CPU.
    if (ps2x_present_share::enabled())
    {
        ps2x_present_share::SharedFrame shared;
        if (ps2x_present_share::latest(shared))
        {
            if (static_cast<int>(shared.width) > tex.width || static_cast<int>(shared.height) > tex.height)
            {
                const int newW = std::max<int>(tex.width, static_cast<int>(shared.width));
                const int newH = std::max<int>(tex.height, static_cast<int>(shared.height));
                UnloadTexture(tex);
                Image grown = GenImageColor(newW, newH, BLANK);
                tex = LoadTextureFromImage(grown);
                UnloadImage(grown);
                std::fprintf(stderr, "[present] frame texture grown to %dx%d\n", newW, newH);
            }
            if (ps2x_present_share::blitToTexture(shared, tex.id))
            {
                // Diagnostic: PS2X_PRESENT_SHARE_DUMP_TICKS="a,b,c" + PS2X_FRAME_DUMP_DIR.
                static std::vector<uint64_t> s_dumpTicks = [] {
                    std::vector<uint64_t> ticks;
                    if (const char *v = std::getenv("PS2X_PRESENT_SHARE_DUMP_TICKS"))
                    {
                        std::stringstream ss(v);
                        std::string item;
                        while (std::getline(ss, item, ','))
                            ticks.push_back(std::strtoull(item.c_str(), nullptr, 10));
                    }
                    return ticks;
                }();
                const char *dumpDir = std::getenv("PS2X_FRAME_DUMP_DIR");
                for (uint64_t &t : s_dumpTicks)
                {
                    if (t != 0u && dumpDir && currentTick >= t)
                    {
                        const std::string path = std::string(dumpDir) + "/share-t" + std::to_string(t) + "-at" +
                                                 std::to_string(currentTick) + ".ppm";
                        std::error_code ec;
                        std::filesystem::create_directories(dumpDir, ec);
                        ps2x_present_share::dumpTexture(tex.id, tex.width, tex.height, shared.width,
                                                        shared.height, path.c_str());
                        t = 0u;
                    }
                }
                outWidth = s_lastWidth = shared.width;
                outHeight = s_lastHeight = shared.height;
                s_hasUploadedFrame = true;
                return;
            }
        }
    }
#endif
    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource))
    {
        // I26: black while no guest frame exists (was magenta); PS2X_FALLBACK_MAGENTA=1 restores it (dev only).
        static const ps2x::FallbackRgba s_fallback = ps2x::fallbackFrameColorFromEnv();
        Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, Color{s_fallback.r, s_fallback.g, s_fallback.b, s_fallback.a});
        dumpPresentationFrame(static_cast<const uint8_t *>(blank.data), FB_WIDTH, FB_HEIGHT, currentTick, 0u,
                              0u, false, true, rt->memory().gs().smode2, rt->memory().gs().pmode,
                              rt->memory().gs().display1, rt->memory().gs().display2,
                              rt->memory().gs().dispfb1, rt->memory().gs().dispfb2);
        if (texGrown)
            UpdateTextureRec(tex, Rectangle{0.0f, 0.0f, static_cast<float>(FB_WIDTH), static_cast<float>(FB_HEIGHT)},
                             blank.data);
        else
            UpdateTexture(tex, blank.data);
        UnloadImage(blank);
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;
    if (!s_scratch.empty() && width != 0u && height != 0u &&
        s_scratch.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4u)
    {
        dumpPresentationFrame(s_scratch.data(), width, height, currentTick, displayFbp, sourceFbp,
                              usedPreferredDisplaySource, false, rt->memory().gs().smode2,
                              rt->memory().gs().pmode, rt->memory().gs().display1, rt->memory().gs().display2,
                              rt->memory().gs().dispfb1, rt->memory().gs().dispfb2);
        // G44: per-vsync shadow compare against these CPU pixels.
        ps2x_gs_shadow::onPresentFrame(currentTick, s_scratch.data(), width, height, &rt->memory().gs());
    }

    const bool large = static_cast<int>(width) > FB_WIDTH || static_cast<int>(height) > FB_HEIGHT;
    if ((large || texGrown) && !s_scratch.empty() &&
        s_scratch.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4u)
    {
        if (static_cast<int>(width) > tex.width || static_cast<int>(height) > tex.height)
        {
            const int newW = std::max<int>(tex.width, static_cast<int>(width));
            const int newH = std::max<int>(tex.height, static_cast<int>(height));
            UnloadTexture(tex);
            Image grown = GenImageColor(newW, newH, BLANK);
            tex = LoadTextureFromImage(grown);
            UnloadImage(grown);
            std::fprintf(stderr, "[present] frame texture grown to %dx%d\n", newW, newH);
        }
        UpdateTextureRec(tex, Rectangle{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)},
                         s_scratch.data());
        outWidth = width;
        outHeight = height;
        s_hasUploadedFrame = true;
        return;
    }

    std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }

    UpdateTexture(tex, s_uploadBuffer.data());
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
}

PS2Runtime::PS2Runtime()
{
#ifndef NDEBUG
    MissingFunctionPolicy defaultPolicy = MissingFunctionPolicy::Stop;
#else
    MissingFunctionPolicy defaultPolicy = MissingFunctionPolicy::ContinueToTarget;
#endif
    if (const char *env = std::getenv("PS2X_MISSING_FUNCTION_POLICY"))
    {
        if (std::strcmp(env, "stop") == 0)
            defaultPolicy = MissingFunctionPolicy::Stop;
        else if (std::strcmp(env, "continue") == 0)
            defaultPolicy = MissingFunctionPolicy::ContinueToTarget;
        else
            std::cerr << "[missing-function] invalid policy '" << env
                      << "' (expected stop|continue); using build default" << std::endl;
    }
    m_missingFunctionPolicy.store(static_cast<uint32_t>(defaultPolicy), std::memory_order_relaxed);
    m_abortOnMissingFunction = defaultPolicy == MissingFunctionPolicy::Stop;
#if defined(PS2X_ENABLE_SBR_TRIPWIRE) && PS2X_ENABLE_SBR_TRIPWIRE
    if (const char *sbrMode = std::getenv("PS2X_SBR_MODE"))
    {
        if (std::strcmp(sbrMode, "s64") == 0)
            m_sbrUseS64 = true;
        else if (std::strcmp(sbrMode, "s32") == 0)
            m_sbrUseS64 = false;
        else
            std::cerr << "[sbr] invalid PS2X_SBR_MODE '" << sbrMode
                      << "' (expected s32|s64); using s32" << std::endl;
    }
#endif
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);
    m_eeScheduler = std::make_unique<EeScheduler>(*this);
#if defined(PS2X_IOP_ENABLE_PLUGINS) && PS2X_IOP_ENABLE_PLUGINS && \
    !defined(PLATFORM_VITA) && (defined(_WIN32) || defined(__linux__))
    if (const char *applicationDirectory = GetApplicationDirectory();
        applicationDirectory && applicationDirectory[0] != '\0')
    {
        m_iopSubsystem->setPluginSearchPaths({std::filesystem::path(applicationDirectory) / "iop_plugins"});
    }
#endif

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    // P1f: keep invocation stacks in kernel-reserved low RAM ([0x80000,
    // 0x100000)), below the ELF image and guest heap and far from every
    // guest thread stack (which grows down from the RAM top).
    m_asyncCallbackStackFloor = 0x00080000u;
    m_asyncCallbackStackTop = 0x00100000u;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    printMissingFunctionCounts();
    try
    {
        requestStop();
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        ps2_snd_audio_output::shutdown();
        if (IsAudioDeviceReady())
        {
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
            CloseWindow();
        }

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
    ps2_e15::closure(m_memory.gs().vsyncTick.load());
    ps2_e7::shutdown(m_memory.gs().vsyncTick.load());
    ps2x_gs_capture::close();
}

void PS2Runtime::setIopPluginSearchPaths(std::vector<std::filesystem::path> paths)
{
    m_iopSubsystem->setPluginSearchPaths(std::move(paths));
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    auto scope = m_iopHost->enterCall(ctx, rdram);
    request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    m_memory.setGsFrontend(&m_gs); // GB3: priv stores ride the GS stream when queued
    // GB2 step (a): PS2X_GS_QUEUE=1 runs the CPU GS backend on its own
    // thread behind the command queue. Default off: direct calls, today's
    // code path. Enabled here during init, before the game thread spawns.
    if (const char *queueEnv = std::getenv("PS2X_GS_QUEUE"))
    {
        if (std::strcmp(queueEnv, "1") == 0 && !m_gs.queueEnabled())
        {
            m_gs.setQueueEnabled(true);
            std::cerr << "[gs:queue] enabled (PS2X_GS_QUEUE=1): CPU backend on GS worker thread"
                      << std::endl;
        }
    }
    // GB3 Part 2: PS2X_GS_BACKEND=parallel = paraLLEl-GS as the live backend,
    // fed by the queue (forced on: every backend call must run on the one GS
    // worker thread). The swap is a SetBackend RPC, so paraLLEl initializes
    // lazily on the worker.
    if (ps2x_gs_parallel::requested() && !m_gs.rawGifBackendActive())
    {
        if (!ps2x_gs_parallel::available())
        {
            std::cerr << "[gs:parallel] PS2X_GS_BACKEND=parallel requested, but this build has no "
                         "paraLLEl backend (PS2X_GS_SHADOW_PARALLEL=OFF); staying on the CPU backend"
                      << std::endl;
        }
        else
        {
            if (!m_gs.queueEnabled())
            {
                m_gs.setQueueEnabled(true);
                std::cerr << "[gs:queue] enabled (forced by PS2X_GS_BACKEND=parallel)" << std::endl;
            }
            m_gs.setRasterBackend(ps2x_gs_parallel::create(&m_memory.gs()));
            std::cerr << "[gs:parallel] live backend selected (PS2X_GS_BACKEND=parallel)" << std::endl;
        }
    }
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    { m_gs.processGIFPacket(data, size); });
    // E33: per-path GIF census + GS draw attribution. The listener runs
    // before each packet's process call (same thread, synchronous drain),
    // so draws kicked while processing land on this packet's path. One
    // relaxed check per packet when stats are off.
    m_gifArbiter.setPacketListener([this](GifPathId path, uint32_t size)
                                   {
                                       const bool stats = ps2_gfx_stats::enabled();
                                       if (stats)
                                       {
                                           ps2_gfx_stats::noteGifPacket(path, size);
                                       }
                                       // GB3 Part 2: a raw-GIF backend needs every packet's path.
                                       if (stats || m_gs.rawGifBackendActive())
                                       {
                                           m_gs.noteGifPath(path);
                                       }
                                   });
    // G44: shadow observes the same drained packets with path preserved.
    m_gifArbiter.setShadowPacketFn([](GifPathId path, const uint8_t *data, uint32_t size)
                                   { ps2x_gs_shadow::onGifPacket(static_cast<uint32_t>(path), data, size); });
    m_memory.setGifArbiter(&m_gifArbiter);
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, top, itop, 65536);
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                  m_gs, &m_memory, top, itop, 65536);
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
#if defined(PS2X_IOP_ENABLE_PLUGINS) && PS2X_IOP_ENABLE_PLUGINS && \
    !defined(PLATFORM_VITA) && (defined(_WIN32) || defined(__linux__))
        std::string pluginError;
        if (!m_iopSubsystem->loadPlugins(&pluginError))
        {
            std::cerr << "Failed to load IOP plugins: " << pluginError << std::endl;
            return false;
        }
#endif
#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
#if defined(PS2X_IOS)
        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);
#else
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
#endif
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
#if defined(PS2X_IOS)
        ps2x::ios::syncWindowSize();
        SetTraceLogLevel(LOG_ERROR);
#endif
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
        if (const char *sound = std::getenv("PS2X_SOUND"); sound && std::strcmp(sound, "1") == 0 &&
            !ps2_snd_audio_output::initialize())
            std::cerr << "[snd-output] unable to initialize host AudioStream\n";
#endif
        SetTargetFPS(60);
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        // E44 Part-3 EE watch: ELF segment into RAM/scratchpad (dev-only,
        // default off). Boot-time; in-window for Boot D (FROM=0).
        ps2_e44_trace::emitRangeOverlap(m_memory.getRDRAM(), nullptr, ph.vaddr, ph.memsz,
                                        "elf-load", 0u, false, __func__);

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        // P1f: invocation stacks stay in kernel-reserved low RAM ([0x80000,
        // 0x100000)); the ELF image, guest heap, and guest thread stacks all
        // live at or above 0x100000, so this region cannot collide with them.
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        m_asyncCallbackStackFloor = 0x00080000u;
        m_asyncCallbackStackTop = 0x00100000u;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string iopError;
    if (!m_iopSubsystem->configure(identity, &iopError))
    {
        std::cerr << "[ps2xIOP] failed to configure profile: " << iopError << std::endl;
        return false;
    }

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    setIoPaths(paths);
}

namespace
{
    // P1c steady-state diagnostics, gated on PS2X_DIAG_PERIOD_MS (unset =
    // compiled in, nothing printed, callers pay only a counter increment).
    uint64_t diagPeriodMs()
    {
        static const uint64_t period = [] {
            if (const char *env = std::getenv("PS2X_DIAG_PERIOD_MS"))
            {
                if (env[0] != '\0')
                {
                    char *end = nullptr;
                    const unsigned long long parsed = std::strtoull(env, &end, 10);
                    if (end != env)
                    {
                        return static_cast<uint64_t>(parsed);
                    }
                }
            }
            return static_cast<uint64_t>(0);
        }();
        return period;
    }

    uint64_t diagNowMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }

    struct CallDiagEntry
    {
        uint64_t count = 0;
        uint32_t firstRa = 0;
        uint32_t lastRa = 0;
    };

    std::unordered_map<uint32_t, CallDiagEntry> g_diagCallCounts;
    uint64_t g_diagCallLastMs = 0;
    uint64_t g_diagCallBlock = 0;

    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

// Flushes the pending call-target histogram when a period boundary has
// passed. Called from the dispatchGuestBranch hook below and from the
// scheduler tick so a quiet steady state still emits (possibly empty)
// blocks.
void diagCallsPeriodicFlush()
{
    const uint64_t period = diagPeriodMs();
    if (period == 0u)
    {
        return;
    }
    const uint64_t now = diagNowMs();
    if (g_diagCallLastMs == 0u)
    {
        g_diagCallLastMs = now;
        return;
    }
    if (now - g_diagCallLastMs < period)
    {
        return;
    }
    g_diagCallLastMs = now;
    std::vector<std::pair<uint32_t, CallDiagEntry>> sorted(g_diagCallCounts.begin(), g_diagCallCounts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &a, const auto &b) { return a.second.count > b.second.count; });
    std::cerr << "[diag:stubs] block=" << g_diagCallBlock++
              << " distinct=" << sorted.size()
              << " period_ms=" << period << std::endl;
    for (size_t i = 0; i < sorted.size() && i < 30u; ++i)
    {
        std::cerr << "[diag:stub] target=0x" << std::hex << sorted[i].first << std::dec
                  << " count=" << sorted[i].second.count
                  << " firstRa=0x" << std::hex << sorted[i].second.firstRa
                  << " lastRa=0x" << std::hex << sorted[i].second.lastRa << std::dec << std::endl;
    }
    g_diagCallCounts.clear();
}

// P1f watchpoint (PS2X_DIAG_WATCH). Cached parse; unset/empty = disabled.
namespace
{
    const std::vector<uint32_t> &diagWatchAddrs()
    {
        static const std::vector<uint32_t> addrs = [] {
            std::vector<uint32_t> out;
            if (const char *env = std::getenv("PS2X_DIAG_WATCH"))
            {
                std::string s(env);
                size_t pos = 0;
                while (pos <= s.size())
                {
                    size_t comma = s.find(',', pos);
                    std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                    size_t a = 0;
                    while (a < tok.size() && std::isspace(static_cast<unsigned char>(tok[a])))
                    {
                        ++a;
                    }
                    size_t b = tok.size();
                    while (b > a && std::isspace(static_cast<unsigned char>(tok[b - 1])))
                    {
                        --b;
                    }
                    if (b > a)
                    {
                        std::string t = tok.substr(a, b - a);
                        char *end = nullptr;
                        const unsigned long parsed = std::strtoul(t.c_str(), &end, 0);
                        if (end != t.c_str())
                        {
                            out.push_back(static_cast<uint32_t>(parsed));
                        }
                    }
                    if (comma == std::string::npos)
                    {
                        break;
                    }
                    pos = comma + 1;
                }
            }
            return out;
        }();
        return addrs;
    }

    std::atomic<int> g_diagWatchThreadId{-999};

    void diagWatchEmit(uint32_t writeAddr,
                       uint32_t width,
                       uint64_t valueLo,
                       uint64_t valueHi,
                       uint32_t pc,
                       int threadId,
                       uint32_t ra,
                       uint32_t sp)
    {
        const std::vector<uint32_t> &watches = diagWatchAddrs();
        if (watches.empty())
        {
            return;
        }
        for (const uint32_t w : watches)
        {
            if (writeAddr < w + 8u && w < writeAddr + width)
            {
                std::cerr << "[diag:watch] addr=0x" << std::hex << writeAddr << std::dec
                          << " width=" << width << " value=0x" << std::hex;
                if (width <= 8u)
                {
                    std::cerr << valueLo;
                }
                else
                {
                    std::cerr.width(16);
                    std::cerr.fill('0');
                    std::cerr << valueHi;
                    std::cerr.width(16);
                    std::cerr.fill('0');
                    std::cerr << valueLo;
                    std::cerr.fill(' ');
                }
                std::cerr << std::dec << " pc=0x" << std::hex << pc << std::dec
                          << " thread=" << threadId
                          << " ra=0x" << std::hex << ra
                          << " sp=0x" << sp << std::dec << std::endl;
            }
        }
    }
}

bool ps2DiagWatchEnabled()
{
    return !diagWatchAddrs().empty();
}

void ps2DiagWatchSetThread(int id)
{
    g_diagWatchThreadId.store(id, std::memory_order_relaxed);
}

void ps2DiagWatchReportDirect(uint32_t writeAddr,
                              uint32_t width,
                              uint64_t valueLo,
                              uint64_t valueHi,
                              uint32_t pc,
                              int threadId,
                              uint32_t ra,
                              uint32_t sp)
{
    diagWatchEmit(writeAddr, width, valueLo, valueHi, pc, threadId, ra, sp);
}

// E3b R2: legacy [diag:watch] line stays verbatim; when the E3 span is armed
// and the store overlaps a watched window (normalized, wrap-safe), an
// [e3:r2] row with old/new values + src tag is emitted on the shared seq.
// src: 0 = WRITE* macro pre-report, 1 = Store* (special-address second
// report; miner dedupes adjacent macro+store identical pairs).
static void diagWatchReportImpl(uint8_t *rdram, uint32_t writeAddr, uint32_t width, uint64_t valueLo,
                                  uint64_t valueHi, const R5900Context *ctx, const PS2Runtime *runtime, uint32_t e3src)
{
    (void)runtime;
    const uint32_t pc = ctx != nullptr ? ctx->pc : 0u;
    const uint32_t ra = ctx != nullptr ? getRegU32(ctx, 31) : 0u;
    const uint32_t sp = ctx != nullptr ? getRegU32(ctx, 29) : 0u;
    const int tid = g_diagWatchThreadId.load(std::memory_order_relaxed);
    diagWatchEmit(writeAddr, width, valueLo, valueHi, pc, tid, ra, sp);
    if (ps2_e7::enabled() && runtime != nullptr)
        ps2_e7::fields(runtime->memory().gs().vsyncTick.load(), rdram, writeAddr, width, valueLo, valueHi, pc, tid);
    if (ps2_e3::armed() && ps2_e3::storeOverlaps(writeAddr, width))
    {
        uint64_t oldLo = 0u;
        uint64_t oldHi = 0u;
        // Called BEFORE the store it annotates on every path (WRITE* macros
        // report pre-store; Store* report before m_memory.write*), so this
        // read is the pre-store value.
        ps2_e3::readOld(rdram, writeAddr, width, oldLo, oldHi);
        ps2_e3::emitR2(e3src, writeAddr, width, oldLo, oldHi, valueLo, valueHi, pc, tid, ra, sp);
    }
}

void ps2DiagWatchReport(uint8_t *rdram,
                        uint32_t writeAddr,
                        uint32_t width,
                        uint64_t valueLo,
                        uint64_t valueHi,
                        const R5900Context *ctx,
                        const PS2Runtime *runtime)
{
    diagWatchReportImpl(rdram, writeAddr, width, valueLo, valueHi, ctx, runtime, 0u);
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistoryImpl()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
    // Programmatic policies are used by callers that handle a stopped dispatch.
    m_abortOnMissingFunction = false;
}

void PS2Runtime::noteUnknownSyscall(uint32_t id)
{
    std::lock_guard<std::mutex> lock(m_coverageMutex);
    ++m_unknownSyscallCounts[id];
}

void PS2Runtime::noteUnhandledRpc(uint32_t sid, uint32_t function)
{
    std::lock_guard<std::mutex> lock(m_coverageMutex);
    ++m_unhandledRpcCounts[(static_cast<uint64_t>(sid) << 32u) | function];
}

void PS2Runtime::printMissingFunctionCounts() const
{
    std::lock_guard<std::mutex> lock(m_coverageMutex);
    std::cerr << "[coverage:missing-functions] targets=" << m_missingFunctionCounts.size() << std::endl;
    for (const auto &[target, count] : m_missingFunctionCounts)
        std::cerr << "[coverage:missing-function] target=0x" << std::hex << target
                  << std::dec << " hits=" << count << std::endl;
    std::cerr << "[coverage:unknown-syscalls] ids=" << m_unknownSyscallCounts.size() << std::endl;
    for (const auto &[id, count] : m_unknownSyscallCounts)
        std::cerr << "[coverage:unknown-syscall] id=0x" << std::hex << id
                  << std::dec << " hits=" << count << std::endl;
    std::cerr << "[coverage:unhandled-rpcs] pairs=" << m_unhandledRpcCounts.size() << std::endl;
    for (const auto &[key, count] : m_unhandledRpcCounts)
        std::cerr << "[coverage:unhandled-rpc] sid=0x" << std::hex << (key >> 32u)
                  << " function=0x" << static_cast<uint32_t>(key)
                  << std::dec << " hits=" << count << std::endl;
#if defined(PS2X_ENABLE_SBR_TRIPWIRE) && PS2X_ENABLE_SBR_TRIPWIRE
    std::cerr << "[sbr:mismatches] pcs=" << m_sbrMismatchCounts.size() << std::endl;
    for (const auto &[pc, count] : m_sbrMismatchCounts)
        std::cerr << "[sbr:mismatch] pc=0x" << std::hex << pc
                  << std::dec << " hits=" << count << std::endl;
#endif
}

#if defined(PS2X_ENABLE_SBR_TRIPWIRE) && PS2X_ENABLE_SBR_TRIPWIRE
bool PS2Runtime::sbrTripwire(int kind, R5900Context *ctx, uint32_t rs, uint32_t pc)
{
    const int32_t s32 = GPR_S32(ctx, rs);
    const int64_t s64 = GPR_S64(ctx, rs);
    bool p32 = false;
    bool p64 = false;
    switch (kind)
    {
    case 0:
        p32 = s32 < 0;
        p64 = s64 < 0;
        break;
    case 1:
        p32 = s32 >= 0;
        p64 = s64 >= 0;
        break;
    case 2:
        p32 = s32 <= 0;
        p64 = s64 <= 0;
        break;
    default:
        p32 = s32 > 0;
        p64 = s64 > 0;
        break;
    }
    if (p32 != p64)
    {
        uint64_t tick = 0;
        uint64_t eeCycle = 0;
        if (m_eeScheduler)
        {
            tick = m_eeScheduler->currentVSyncTick();
            eeCycle = m_eeScheduler->currentEeCycle();
        }
        noteSignedBranchMismatch(pc, rs, GPR_U64(ctx, rs), tick, eeCycle);
    }
    return m_sbrUseS64 ? p64 : p32;
}

void PS2Runtime::noteSignedBranchMismatch(uint32_t pc, uint32_t rs, uint64_t value, uint64_t tick, uint64_t eeCycle)
{
    std::lock_guard<std::mutex> lock(m_coverageMutex);
    const bool firstSight = m_sbrMismatchCounts.find(pc) == m_sbrMismatchCounts.end();
    ++m_sbrMismatchCounts[pc];
    if (firstSight && m_sbrLoggedPcs < 64u)
    {
        ++m_sbrLoggedPcs;
        std::cerr << "[sbr] pc=0x" << std::hex << pc << std::dec
                  << " rs=" << rs
                  << " value=0x" << std::hex << value << std::dec
                  << " tick=" << tick << " cycle=" << eeCycle << std::endl;
    }
}
#endif

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

std::string PS2Runtime::formatDispatchHistory() const
{
    return formatDispatchHistoryImpl();
}

namespace
{
    bool diagReportAll()
    {
        static const bool reportAll = [] {
            if (const char *env = std::getenv("PS2X_DIAG_REPORT_ALL"))
            {
                return env[0] == '1' && env[1] == '\0';
            }
            return false;
        }();
        return reportAll;
    }

    // P1n driver-entry probe (P14-1d). PS2X_DIAG_DRIVER_PROBE=1 enables one
    // [diag:driver-entry] line per dispatch to 0x3dd1d8; unset/empty = off.
    bool diagDriverProbeEnabled()
    {
        static const bool enabled = [] {
            if (const char *env = std::getenv("PS2X_DIAG_DRIVER_PROBE"))
            {
                return env[0] == '1' && env[1] == '\0';
            }
            return false;
        }();
        return enabled;
    }

    void diagDriverEntryEmit(uint32_t sp, uint32_t ra, uint32_t sourcePc, bool checkpointed)
    {
        std::cerr << "[diag:driver-entry] sp=0x" << std::hex << sp
                  << " ra=0x" << ra
                  << " sourcePc=0x" << sourcePc
                  << std::dec << " checkpointed=" << (checkpointed ? 1 : 0) << std::endl;
    }

    // P1ad 394ED0 park probe. PS2X_DIAG_394ED0=1 enables one [diag:394ed0]
    // line per fresh guest dispatch to 0x394ED0 (the hash-intern routine
    // main parks in); unset/empty = off. Read-only: GPRs plus masked RAM
    // reads, no guest writes, no control-flow change. Unreadable words
    // print as WILD (never a masked alias). Capped; the cap line marks it.
    bool diag394Ed0Enabled()
    {
        static const bool enabled = [] {
            if (const char *env = std::getenv("PS2X_DIAG_394ED0"))
            {
                return env[0] == '1' && env[1] == '\0';
            }
            return false;
        }();
        return enabled;
    }

    uint32_t diag394Ed0Read(const uint8_t *rdram, uint64_t addr, bool &ok)
    {
        ok = (rdram != nullptr) && (addr + 3u < PS2_RAM_SIZE);
        return ok ? Ps2FastRead32(rdram, static_cast<uint32_t>(addr)) : 0u;
    }

    void diag394Ed0Word(std::ostream &out, const uint8_t *rdram, uint64_t addr)
    {
        bool ok = false;
        const uint32_t value = diag394Ed0Read(rdram, addr, ok);
        if (ok)
        {
            out << "0x" << std::hex << value << std::dec;
        }
        else
        {
            out << "WILD";
        }
    }

    void diag394Ed0Emit(const uint8_t *rdram, R5900Context *ctx, uint32_t sourcePc)
    {
        static uint64_t seq = 0;
        static uint64_t emitted = 0;
        static constexpr uint64_t kDiag394Ed0Cap = 20000u;
        const uint64_t n = seq++;
        if (emitted >= kDiag394Ed0Cap)
        {
            if (emitted == kDiag394Ed0Cap)
            {
                std::cerr << "[diag:394ed0] cap=" << kDiag394Ed0Cap << " reached" << std::endl;
                ++emitted;
            }
            return;
        }
        ++emitted;
        const uint32_t a0 = (ctx != nullptr) ? getRegU32(ctx, 4) : 0u;
        const uint32_t a1 = (ctx != nullptr) ? getRegU32(ctx, 5) : 0u;
        const uint32_t a2 = (ctx != nullptr) ? getRegU32(ctx, 6) : 0u;
        const uint32_t ra = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
        const uint64_t base = a0;
        const uint64_t bucketAddr = base + 0x674A0u + ((a2 & 0xFFu) << 2);
        bool headOk = false;
        const uint32_t head = diag394Ed0Read(rdram, bucketAddr, headOk);
        std::cerr << "[diag:394ed0] n=" << n
                  << " ra=0x" << std::hex << ra
                  << " src=0x" << sourcePc << std::dec
                  << " a0=0x" << std::hex << a0
                  << " a1=0x" << a1
                  << " a2=0x" << a2 << std::dec
                  << " total=";
        diag394Ed0Word(std::cerr, rdram, base);
        std::cerr << " pool=";
        diag394Ed0Word(std::cerr, rdram, base + 0x51480u);
        std::cerr << " head=";
        if (headOk)
        {
            std::cerr << "0x" << std::hex << head << std::dec;
        }
        else
        {
            std::cerr << "WILD";
        }
        std::cerr << " key=[";
        for (int i = 0; i < 4; ++i)
        {
            if (i != 0)
            {
                std::cerr << ' ';
            }
            diag394Ed0Word(std::cerr, rdram, static_cast<uint64_t>(a1) + i * 4u);
        }
        std::cerr << "] chain=[";
        uint32_t seen[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        uint32_t node = head;
        bool nodeOk = headOk;
        bool cycleFound = false;
        for (int i = 0; i < 8 && nodeOk && node != 0u; ++i)
        {
            bool repeat = false;
            for (int j = 0; j < i; ++j)
            {
                if (seen[j] == node)
                {
                    repeat = true;
                }
            }
            if (i != 0)
            {
                std::cerr << ' ';
            }
            if (repeat)
            {
                std::cerr << "CYCLE@0x" << std::hex << node << std::dec;
                cycleFound = true;
                break;
            }
            seen[i] = node;
            std::cerr << "0x" << std::hex << node << std::dec;
            node = diag394Ed0Read(rdram, static_cast<uint64_t>(node) + 0x14u, nodeOk);
        }
        if (cycleFound)
        {
        }
        else if (nodeOk && node == 0u)
        {
            std::cerr << " NULL";
        }
        else if (!nodeOk)
        {
            std::cerr << " WILD";
        }
        else
        {
            std::cerr << " ...";
        }
        std::cerr << "]" << std::endl;
    }
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    // E11: retain inputs to the already-reached sibling residual; no dispatch or result change.
    if (ps2_e7::enabled() && targetPc == 0x2c5358u)
        ps2_e7::cardCall(m_memory.gs().vsyncTick.load(), "mc-missing", rdram,
            targetPc, sourcePc, getRegU32(ctx,4), getRegU32(ctx,5), ctx->pc, getRegU32(ctx,2),
            getRegU32(ctx,5), getRegU32(ctx,6), getRegU32(ctx,7),
            getRegU32(ctx,29), getRegU32(ctx,31),
            g_diagWatchThreadId.load(std::memory_order_relaxed));
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    {
        std::lock_guard<std::mutex> lock(m_coverageMutex);
        ++m_missingFunctionCounts[targetPc];
    }
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);
    const bool shouldPrint = policy == MissingFunctionPolicy::Stop || firstReport || diagReportAll();

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (shouldPrint)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistoryImpl()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        if (m_abortOnMissingFunction)
        {
            printMissingFunctionCounts();
            std::abort();
        }
        requestStop();
    }
}

// E3b R1/R4: record state machine + invocation summaries, fed from
// dispatchGuestBranch only (362DE8/394ED0/395000/363490/376938/362CC8).
// Read-only: GPRs + getMemPtr halfword reads, no guest writes (C1).
// a1 == s1 at 394ED0 entry (362DE8:320/1043 set a1=s1; stride 0x80 at :896);
// the 0x362f84 check reads [s1+0x1E] after 394ED0 returns, so entry reads +
// shared-seq R2/R3 rows bracket exactly what the check saw.
namespace
{
struct E3Rec
{
    bool open = false;
    uint64_t n = 0u;
    uint64_t inv = 0u;
    uint64_t seqEntry = 0u;
    uint32_t a0 = 0u;
    uint32_t a1 = 0u;
    uint32_t a2 = 0u;
    uint32_t s1e = 0u;
    uint32_t ra = 0u;
    uint32_t src = 0u;
    int32_t k = -1;
    uint32_t h10pre = 0u;
    uint32_t h1cpre = 0u;
    uint32_t h1epre = 0u;
    bool preOk = false;
    bool sawCall = false;
    uint32_t s1c = 0u;
};

struct E3InvStat
{
    uint64_t nrec = 0u;
    uint32_t s1min = 0xFFFFFFFFu;
    uint32_t s1max = 0u;
    uint32_t bases = 0u; // bit0 ramp, bit1 steady(expected), bit2 other/unchecked
};

static E3Rec g_e3rec;
static E3InvStat g_e3inv;
static uint64_t g_e3n394 = 0u;

bool e3ReadH(uint8_t *rdram, uint32_t addr, uint32_t &out)
{
    const uint8_t *lo = getConstMemPtr(rdram, addr);
    const uint8_t *hi = getConstMemPtr(rdram, addr + 1u);
    if (!lo || !hi)
    {
        return false;
    }
    out = static_cast<uint32_t>(lo[0]) | (static_cast<uint32_t>(hi[0]) << 8);
    return true;
}

const char *e3BaseClass(uint32_t a1, int32_t &kOut)
{
    kOut = -1;
    const uint32_t base = ps2_e3::expectedS1Base();
    if (base == 0u)
    {
        return "unchecked";
    }
    if (a1 >= base && a1 < base + 20u * 0x80u && ((a1 - base) % 0x80u) == 0u)
    {
        kOut = static_cast<int32_t>((a1 - base) / 0x80u);
        return "ok";
    }
    if (a1 >= 0x70000000u && a1 < 0x70000000u + 20u * 0x80u && ((a1 - 0x70000000u) % 0x80u) == 0u)
    {
        return "ramp";
    }
    return "other";
}

void e3CloseRec(uint8_t *rdram)
{
    if (!g_e3rec.open)
    {
        return;
    }
    g_e3rec.open = false;
    uint32_t h10o = 0u;
    uint32_t h1co = 0u;
    uint32_t h1eo = 0u;
    const bool ok10 = e3ReadH(rdram, g_e3rec.a1 + 0x10u, h10o);
    const bool ok1c = e3ReadH(rdram, g_e3rec.a1 + 0x1Cu, h1co);
    const bool ok1e = e3ReadH(rdram, g_e3rec.a1 + 0x1Eu, h1eo);
    int32_t k = -1;
    const char *base = e3BaseClass(g_e3rec.a1, k);
    ps2_e3::emitR1(g_e3rec.seqEntry, g_e3rec.n, g_e3rec.inv, g_e3rec.a0, g_e3rec.a1, g_e3rec.a2, g_e3rec.s1e,
                   g_e3rec.ra, g_e3rec.src, k, g_e3rec.h10pre, g_e3rec.h1cpre, g_e3rec.h1epre, g_e3rec.preOk,
                   g_e3rec.sawCall, g_e3rec.s1c, h10o, h1co, h1eo, ok10 && ok1c && ok1e, base);
    g_e3inv.nrec++;
    if (g_e3rec.a1 < g_e3inv.s1min)
    {
        g_e3inv.s1min = g_e3rec.a1;
    }
    if (g_e3rec.a1 > g_e3inv.s1max)
    {
        g_e3inv.s1max = g_e3rec.a1;
    }
    if (std::strcmp(base, "ramp") == 0)
    {
        g_e3inv.bases |= 1u;
    }
    else if (std::strcmp(base, "ok") == 0)
    {
        g_e3inv.bases |= 2u;
    }
    else
    {
        g_e3inv.bases |= 4u;
    }
}

void e3Note394(uint8_t *rdram, R5900Context *ctx, uint32_t srcPc)
{
    const uint64_t n = g_e3n394++;
    if (!ps2_e3::armed())
    {
        return;
    }
    if (g_e3rec.open)
    {
        e3CloseRec(rdram); // previous record: outcome + post-reads at this entry
    }
    g_e3rec.open = true;
    g_e3rec.n = n;
    g_e3rec.inv = ps2_e3::invStorage().load(std::memory_order_relaxed);
    g_e3rec.seqEntry = ps2_e3::seqNext();
    g_e3rec.a0 = ctx ? getRegU32(ctx, 4) : 0u;
    g_e3rec.a1 = ctx ? getRegU32(ctx, 5) : 0u;
    g_e3rec.a2 = ctx ? getRegU32(ctx, 6) : 0u;
    g_e3rec.s1e = ctx ? getRegU32(ctx, 17) : 0u;
    g_e3rec.ra = ctx ? getRegU32(ctx, 31) : 0u;
    g_e3rec.src = srcPc;
    int32_t k = -1;
    e3BaseClass(g_e3rec.a1, k);
    g_e3rec.k = k;
    const bool ok10 = e3ReadH(rdram, g_e3rec.a1 + 0x10u, g_e3rec.h10pre);
    const bool ok1c = e3ReadH(rdram, g_e3rec.a1 + 0x1Cu, g_e3rec.h1cpre);
    const bool ok1e = e3ReadH(rdram, g_e3rec.a1 + 0x1Eu, g_e3rec.h1epre);
    g_e3rec.preOk = ok10 && ok1c && ok1e;
    g_e3rec.sawCall = false;
    g_e3rec.s1c = 0u;
}

void e3Note39500(R5900Context *ctx)
{
    if (!ps2_e3::armed() || !g_e3rec.open || g_e3rec.sawCall)
    {
        return;
    }
    g_e3rec.sawCall = true;
    g_e3rec.s1c = ctx ? getRegU32(ctx, 17) : 0u;
}

void e3Note362DE8(uint8_t *rdram, R5900Context *ctx, uint32_t srcPc)
{
    if (ps2_e3::armed())
    {
        // Flush the completing invocation while still armed (rows must
        // precede the span-complete marker); then advance the counter.
        const uint64_t inv = ps2_e3::invStorage().load(std::memory_order_relaxed);
        e3CloseRec(rdram);
        const uint32_t b = g_e3inv.bases;
        const uint32_t nb = (b & 1u) + ((b >> 1) & 1u) + ((b >> 2) & 1u);
        ps2_e3::emitR4Sum(inv, g_e3inv.nrec, g_e3inv.nrec ? g_e3inv.s1min : 0u,
                           g_e3inv.nrec ? g_e3inv.s1max : 0u, nb, nb > 1u);
    }
    const uint64_t ord = ps2_e3::noteInvEntry();
    const uint64_t t = ps2_e3::targetInv();
    if (ord == t || ord == t + 1u)
    {
        g_e3inv = E3InvStat{};
        ps2_e3::emitR4(ord, "362DE8", ctx ? getRegU32(ctx, 31) : 0u, srcPc);
    }
}

void e3NoteR4(uint64_t inv, const char *fn, R5900Context *ctx, uint32_t srcPc)
{
    ps2_e3::emitR4(inv, fn, ctx ? getRegU32(ctx, 31) : 0u, srcPc);
}
} // namespace

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    // E43 draw-record census (dev-only, default off; self-gated on the
    // target pc first so the common path pays one compare).
    if (targetPc == ps2_e43_trace::kWalkerTarget)
    {
        ps2_e43_trace::noteWalkerCall(rdram, ctx, sourcePc);
    }
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);
    if (isCall && targetPc == 0x00228C08u)
    {
        enforceSsx3Widescreen(rdram, sourcePc);
    }

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        // P1n driver-entry probe (P14-1d): checkpoint path.
        if (targetPc == 0x3DD1D8u && diagDriverProbeEnabled())
        {
            const uint32_t sp = (ctx != nullptr) ? getRegU32(ctx, 29) : 0u;
            const uint32_t ra = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
            diagDriverEntryEmit(sp, ra, sourcePc, true);
        }
        return false;
    }

    if (!isCall)
    {
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        return false;
    }

    if (!hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            return true;
        }

        return false;
    }

    // P1c HLE stub/call histogram at the register_functions.cpp binding
    // lookup (lookupFunction below resolves the guest call target to the
    // registered host function). Counts per call target with first/last $ra.
    static uint64_t s_diagCallTick = 0;
    ++s_diagCallTick;
    // T1: cumulative hot-pc tally, always on (the P1c map above is
    // per-period and cleared; the snapshot needs the whole boot).
    ps2_park::tallyDispatch(targetPc, (ctx != nullptr) ? getRegU32(ctx, 31) : 0u);
    if (diagPeriodMs() != 0u)
    {
        const uint32_t callerRa = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
        CallDiagEntry &entry = g_diagCallCounts[targetPc];
        if (entry.count == 0u)
        {
            entry.firstRa = callerRa;
        }
        entry.lastRa = callerRa;
        ++entry.count;
        diagCallsPeriodicFlush();
    }

    // P1n driver-entry probe (P14-1d): call path.
    if (targetPc == 0x3DD1D8u && diagDriverProbeEnabled())
    {
        const uint32_t sp = (ctx != nullptr) ? getRegU32(ctx, 29) : 0u;
        const uint32_t ra = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
        diagDriverEntryEmit(sp, ra, sourcePc, false);
    }

    // P1ad 394ED0 park probe: fresh dispatches only (checkpoint resumes do
    // not re-dispatch). Read-only; see diag394Ed0Emit above.
    if (targetPc == 0x394ED0u && diag394Ed0Enabled())
    {
        diag394Ed0Emit(rdram, ctx, sourcePc);
    }

    // E3b R1/R4 taps (read-only; self-gated on PS2X_E3_INV). No isCall gate:
    // the 394ED0 probe above counts every dispatch and matches the ps2_log
    // enter census exactly, so the E3 invocation counter uses the same rule.
    if (ps2_e3::enabled())
    {
        if (targetPc == 0x362DE8u)
        {
            e3Note362DE8(rdram, ctx, sourcePc);
        }
        else if (targetPc == 0x394ED0u)
        {
            e3Note394(rdram, ctx, sourcePc);
        }
        else if (targetPc == 0x395000u)
        {
            e3Note39500(ctx);
        }
        else if (ps2_e3::armed())
        {
            const uint64_t inv = ps2_e3::invStorage().load(std::memory_order_relaxed);
            if (targetPc == 0x363490u)
            {
                e3NoteR4(inv, "363490", ctx, sourcePc);
            }
            else if (targetPc == 0x376938u)
            {
                e3NoteR4(inv, "376938", ctx, sourcePc);
            }
            else if (targetPc == 0x362CC8u)
            {
                e3NoteR4(inv, "362CC8", ctx, sourcePc);
            }
        }
    }

    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    // E11: preserve entry a0 across the call for dynamic query-object joins.
    // Observation only, sharing the existing E7 window and byte budgets.
    const bool cardObservation = ps2_e7::enabled() && ps2_e7::cardTarget(targetPc, sourcePc);
    const uint32_t cardA0 = cardObservation ? getRegU32(ctx, 4) : 0u;
    // E12: retain original port across predicate execution (observation only).
    const uint32_t cardA1 = cardObservation ? getRegU32(ctx, 5) : 0u;
    auto noteCardCall = [&](const char *phase) {
        if (cardObservation)
            ps2_e7::cardCall(m_memory.gs().vsyncTick.load(), phase, rdram,
                targetPc, sourcePc, cardA0, cardA1, ctx->pc, getRegU32(ctx, 2),
                getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7),
                getRegU32(ctx, 29), getRegU32(ctx, 31),
                g_diagWatchThreadId.load(std::memory_order_relaxed));
    };
    noteCardCall("mc-call");
    ps2_e15::Trace mpegTrace("branch",m_memory.gs().vsyncTick.load(),rdram,ctx,targetPc,sourcePc,
                            g_diagWatchThreadId.load(std::memory_order_relaxed));
    // E43 h394 hash-call tap (dev-only, default off): pre-capture args,
    // run the synchronous callee, then capture v0 + the store flag.
    const bool e43h394 = ps2_e43_trace::h394CallArmed(sourcePc, targetPc);
    if (e43h394)
    {
        ps2_e43_trace::h394Pre(rdram, ctx);
    }
    // E44 Boot-C last-writer readout (dev-only, default off): at the
    // item-0 walk into func_394ED0. Self-gated on s1 (reg 17).
    if (sourcePc == ps2_e43_trace::kH394Source &&
        targetPc == ps2_e43_trace::kH394Target)
    {
        ps2_e44_trace::noteItem0Walk(ctx, (ctx != nullptr) ? getRegU32(ctx, 17) : 0u);
    }
    targetFn(rdram, ctx, this);
    if (e43h394)
    {
        ps2_e43_trace::h394Post(ctx);
    }
    mpegTrace.finish(m_memory.gs().vsyncTick.load());
    noteCardCall("mc-return");

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    if (ctx->pc == entryPc)
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, 4096);
    copyVu0StateToContext(m_vu0.state(), ctx);
    // E53: dev-only VU0 start log (PS2X_E53_VU0_LOG=1, default off): caller
    // pc, start, cycles, and an FNV-1a of VU0 data memory after the run so an
    // upload that lands shows up as a changing hash. First 64 starts, then
    // every 256th.
    {
        static const bool e53Log = std::getenv("PS2X_E53_VU0_LOG") != nullptr;
        if (e53Log)
        {
            static std::atomic<uint64_t> e53Count{0};
            const uint64_t n = e53Count.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 64u || (n % 256u) == 0u)
            {
                uint32_t h = 2166136261u;
                for (uint32_t i = 0; i < PS2_VU0_DATA_SIZE; ++i)
                    h = (h ^ vu0Data[i]) * 16777619u;
                std::fprintf(stderr, "[E53] vu0start n=%llu caller=0x%x startPC=0x%x cycles=%llu budget_hit=%d vu0data_fnv=%08x\n",
                             static_cast<unsigned long long>(n), ctx->pc, startPC,
                             static_cast<unsigned long long>(m_vu0.state().cycles),
                             m_vu0.state().cycles >= 4096u ? 1 : 0, h);
            }
        }
    }
    // E44 Part-2 VU0 call trace (dev-only, default off). m_cycle was
    // reset by reset() above, so state().cycles is this call's usage.
    if (ps2_e44_trace::enabled())
    {
        const uint64_t used = m_vu0.state().cycles;
        ps2_e44_trace::noteVu0Call(ctx, startPC, used, used >= 4096u,
                                   m_vu0.state().vi[1], m_vu0.state().vi[2]);
    }
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::vu1StartMicroProgramFromEe(R5900Context *ctx, uint32_t cmsar1)
{
    // PCSX2 vu1ExecMicro(addr): TPC = addr (instruction index), run from TPC*8
    // with the VIF1 TOP/ITOP the VU sees through XTOP/XITOP.
    const uint32_t startPC = (cmsar1 & 0x7FFu) << 3;
    VIFRegisters &vif1 = m_memory.vif1_regs;
    m_vu1.state().dBitEnabled = (ctx->vu0_fbrst & (1u << 10)) != 0u;
    m_vu1.state().tBitEnabled = (ctx->vu0_fbrst & (1u << 11)) != 0u;
    m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                  m_gs, &m_memory, startPC, vif1.top, vif1.itop, 65536);
    ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & ~0x0600u) |
                        (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                        (m_vu1.state().stoppedByT ? 0x0400u : 0u);
    static std::atomic<uint32_t> s_e53Cmsar1Starts{0};
    const uint32_t n = s_e53Cmsar1Starts.fetch_add(1, std::memory_order_relaxed);
    if (n < 8u)
        std::fprintf(stderr, "[E53] CTC2 CMSAR1 VU1 start #%u startPC=0x%x\n", n + 1u, startPC);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
        {
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
            if (ps2_e41_trace::plantArmed()) // E41 plant watch
            {
                char src[32];
                std::snprintf(src, sizeof(src), "src=0x%x", oldAddr);
                ps2_e41_trace::notePlantRange(ps2_e41_trace::lastVsyncTick(), newAddr,
                                              copyBytes, rdram, "heap-realloc", src, 0u);
            }
        }
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

namespace
{
    // GB2 Part 7: matches any vaddr in the GS priv range (Part 3 matched
    // CSR/SIGLBLID offsets only; Part 5/K proved the trigger read is a
    // non-CSR priv addr, so the drain covers the whole range). All guest
    // priv loads flow through PS2Runtime::Load* — no fast-path hole
    // (Ps2IsSpecialAddress covers PS2_GS_PRIV_REG_BASE/SIZE and both the
    // constant-MMIO translator path and the READ* macros route special
    // addresses here). (A phys-mask-only check over-matches scratchpad
    // 0x70001000/0x70001080; the range check excludes it.)
    inline bool gb2IsGsPrivReg(uint32_t vaddr)
    {
        return (vaddr - PS2_GS_PRIV_REG_BASE) < PS2_GS_PRIV_REG_SIZE;
    }
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        // Part 7: drain-only (read8 doesn't serve the priv range, so
        // there is no meaningful value to log).
        if (gb2IsGsPrivReg(vaddr))
        {
            if (ps2_pk::privDrainEnabled() && m_gs.queueEnabled())
                m_gs.drainQueue();
            return m_memory.read8(vaddr);
        }
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        // Part 7: drain-only (read16 doesn't serve the priv range).
        if (gb2IsGsPrivReg(vaddr))
        {
            if (ps2_pk::privDrainEnabled() && m_gs.queueEnabled())
                m_gs.drainQueue();
            return m_memory.read16(vaddr);
        }
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        if (gb2IsGsPrivReg(vaddr))
        {
            if (ps2_pk::privDrainEnabled() && m_gs.queueEnabled())
                m_gs.drainQueue();
            const uint32_t value = m_memory.read32(vaddr);
            ps2_pk::notePrivRead(m_memory.gs().vsyncTick.load(std::memory_order_relaxed), value,
                                 ctx ? ctx->pc : 0u, vaddr);
            return value;
        }
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        if (gb2IsGsPrivReg(vaddr))
        {
            if (ps2_pk::privDrainEnabled() && m_gs.queueEnabled())
                m_gs.drainQueue();
            const uint64_t value = m_memory.read64(vaddr);
            ps2_pk::notePrivRead(m_memory.gs().vsyncTick.load(std::memory_order_relaxed), value,
                                 ctx ? ctx->pc : 0u, vaddr);
            return value;
        }
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        // Part 7: drain-only (read128 returns zero outside RAM areas).
        if (gb2IsGsPrivReg(vaddr))
        {
            if (ps2_pk::privDrainEnabled() && m_gs.queueEnabled())
                m_gs.drainQueue();
            return m_memory.read128(vaddr);
        }
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    if (ps2DiagWatchEnabled())
    {
        diagWatchReportImpl(rdram, vaddr, 1u, value, 0u, ctx, this, 1u);
    }
    try
    {
        // E44 scratchpad write watch (dev-only, default off).
        const bool e44 = ps2_e44_trace::storeArmed(vaddr, 1u);
        ps2_e44_trace::detail::ScopedMemSuppress e44Suppress(e44);
        m_memory.write8(vaddr, value);
        if (e44)
        {
            ps2_e44_trace::noteStore(rdram, ctx, vaddr, 1u, __func__);
        }
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    if (ps2DiagWatchEnabled())
    {
        diagWatchReportImpl(rdram, vaddr, 2u, value, 0u, ctx, this, 1u);
    }
    try
    {
        // E44 scratchpad write watch (dev-only, default off).
        const bool e44 = ps2_e44_trace::storeArmed(vaddr, 2u);
        ps2_e44_trace::detail::ScopedMemSuppress e44Suppress(e44);
        m_memory.write16(vaddr, value);
        if (e44)
        {
            ps2_e44_trace::noteStore(rdram, ctx, vaddr, 2u, __func__);
        }
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    if (ps2DiagWatchEnabled())
    {
        diagWatchReportImpl(rdram, vaddr, 4u, value, 0u, ctx, this, 1u);
    }
    try
    {
        // E44 scratchpad write watch (dev-only, default off).
        const bool e44 = ps2_e44_trace::storeArmed(vaddr, 4u);
        ps2_e44_trace::detail::ScopedMemSuppress e44Suppress(e44);
        m_memory.write32(vaddr, value);
        if (e44)
        {
            ps2_e44_trace::noteStore(rdram, ctx, vaddr, 4u, __func__);
        }
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    if (ps2DiagWatchEnabled())
    {
        diagWatchReportImpl(rdram, vaddr, 8u, value, 0u, ctx, this, 1u);
    }
    try
    {
        // E44 scratchpad write watch (dev-only, default off).
        const bool e44 = ps2_e44_trace::storeArmed(vaddr, 8u);
        ps2_e44_trace::detail::ScopedMemSuppress e44Suppress(e44);
        m_memory.write64(vaddr, value);
        if (e44)
        {
            ps2_e44_trace::noteStore(rdram, ctx, vaddr, 8u, __func__);
        }
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    if (ps2DiagWatchEnabled())
    {
        diagWatchReportImpl(rdram, vaddr, 16u, _parts[0], _parts[1], ctx, this, 1u);
    }
    try
    {
        if (vaddr == 0x10005000u && ps2_e7::enabled())
            ps2_e7::event(m_memory.gs().vsyncTick.load(), "cpu-fifo", "pc=0x%x ra=0x%x lo=0x%llx hi=0x%llx mask=%u", ctx ? ctx->pc : 0u, ctx ? getRegU32(ctx,31) : 0u, static_cast<unsigned long long>(_parts[0]), static_cast<unsigned long long>(_parts[1]), m_memory.isPath3Masked());
        // E44 scratchpad write watch (dev-only, default off).
        const bool e44 = ps2_e44_trace::storeArmed(vaddr, 16u);
        ps2_e44_trace::detail::ScopedMemSuppress e44Suppress(e44);
        m_memory.write128(vaddr, value);
        if (e44)
        {
            ps2_e44_trace::noteStore(rdram, ctx, vaddr, 16u, __func__);
        }
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    return m_eeScheduler->checkpointDue(cycles);
}

uint32_t PS2Runtime::readEeCount(R5900Context *ctx) noexcept
{
    return m_eeScheduler->readCount(ctx);
}

void PS2Runtime::writeEeCount(R5900Context *ctx, uint32_t value) noexcept
{
    m_eeScheduler->writeCount(ctx, value);
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (ps2_e41_trace::plantArmed()) // E41 plant watch
        ps2_e41_trace::notePlantRange(ps2_e41_trace::lastVsyncTick(), guestAddress,
                                      sizeof(handler), rdram, "irq-handler-install",
                                      "handler", 0u);
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
        if (ps2_e41_trace::plantArmed()) // E41 plant watch
            ps2_e41_trace::notePlantRange(ps2_e41_trace::lastVsyncTick(), address,
                                          sizeof(zero), rdram, "irq-handler-clear",
                                          "zero", 0u);
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        if (ps2_e41_trace::plantArmed()) // E41 plant watch
            ps2_e41_trace::notePlantRange(ps2_e41_trace::lastVsyncTick(), guestAddress,
                                          sizeof(handler), rdram, "irq-handler-restore",
                                          "handler", 0u);
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);

    std::atomic<bool> gameThreadFinished{false};

    // F4-2b: PS2X_GAME_THREAD_STACK_KB=N (default-off test knob) creates the
    // game thread with an N-KiB stack, to prove the VU1 pair chain is flat:
    // nesting chains overflow small stacks, tail-call chains don't care.
    // Unset/empty/0 = plain std::thread exactly as before.
    const long gameStackKb = [] {
        const char *env = std::getenv("PS2X_GAME_THREAD_STACK_KB");
        if (env == nullptr || env[0] == '\0')
            return 0L;
        return std::strtol(env, nullptr, 10);
    }();

    std::function<void()> gameMain = [&]()
    {
        ThreadNaming::SetCurrentThreadName("GameThread");
        // N11: PS2X_GAME_THREAD_CPUS="6,7" pins this thread to itself (no
        // privilege needed); unset/empty = no change. Unconditional stderr
        // line (RUNTIME_LOG compiles out of release builds).
        if (const char *affinityCpus = std::getenv("PS2X_GAME_THREAD_CPUS"))
        {
            if (affinityCpus[0] != '\0')
            {
                const int rc = ps2x::pinCurrentThreadToCpus(ps2x::parseCpuList(affinityCpus));
                std::fprintf(stderr, "[affinity] game thread cpus=%s rc=%d\n", affinityCpus, rc);
            }
        }
        try
        {
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        gameThreadFinished.store(true, std::memory_order_release);
    };

    bool gameUsesPthread = false;
    std::thread gameThread;
#if defined(__unix__) || defined(__APPLE__)
    pthread_t gamePthread;
    if (gameStackKb > 0)
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        const bool attrOk =
            pthread_attr_setstacksize(&attr, static_cast<size_t>(gameStackKb) * 1024u) == 0;
        const bool createOk =
            attrOk &&
            pthread_create(&gamePthread, &attr,
                           +[](void *p) -> void * {
                (*static_cast<std::function<void()> *>(p))();
                return nullptr;
            },
                           &gameMain) == 0;
        pthread_attr_destroy(&attr);
        if (createOk)
        {
            gameUsesPthread = true;
            std::fprintf(stderr, "[stack] game thread stack=%ld KiB (PS2X_GAME_THREAD_STACK_KB)\n",
                         gameStackKb);
        }
        else
            std::fprintf(stderr, "[stack] PS2X_GAME_THREAD_STACK_KB=%ld rejected; using default stack\n",
                         gameStackKb);
    }
#else
    if (gameStackKb > 0)
        std::fprintf(stderr, "[stack] PS2X_GAME_THREAD_STACK_KB unsupported on this platform; using default\n");
#endif
    if (!gameUsesPthread)
        gameThread = std::thread(std::move(gameMain));

    uint64_t tick = 0;
    // I25: PS2X_VSYNC_RATE_LOG=1 prints guest vsyncs per wall second every
    // 5 s (diagnostic; off by default, one getenv at loop start).
    const bool vsyncRateLog = [] {
        const char *env = std::getenv("PS2X_VSYNC_RATE_LOG");
        return env && env[0] == '1';
    }();
    const bool vpadWanted = virtualPadWanted();
    bool vpadLastPadConnected = true; // forces the first [vpad] line when the overlay shows
    const std::vector<ps2x::vpad::TestTouch> vpadTestTouches =
        ps2x::vpad::parseTestTouches(std::getenv("PS2X_VPAD_TEST_TOUCHES")); // DEV-ONLY
    const std::vector<ps2x::vpad::TestTap> vpadTestTaps =
        ps2x::vpad::parseTestTap(std::getenv("PS2X_VPAD_TEST_TAP")); // DEV-ONLY
    ps2x::vpad::StickState vpadStick;                                       // I32: floating-stick anchor, carried across frames
    float vpadTestStickX = 0.0f, vpadTestStickY = 0.0f;
    const bool vpadTestStick =
        ps2x::vpad::parseTestStick(std::getenv("PS2X_VPAD_TEST_STICK"), vpadTestStickX, vpadTestStickY); // DEV-ONLY
    if (vpadTestStick)
    {
        std::fprintf(stderr, "[vpad] test stick lx=%.3f ly=%.3f\n", vpadTestStickX, vpadTestStickY);
    }
    if (!vpadWanted)
    {
        std::fprintf(stderr, "[vpad] off (PS2X_VIRTUAL_PAD)\n");
    }
    const bool anamorphic = g_ssx3WidescreenActive.load(std::memory_order_acquire) &&
                            g_ssx3WidescreenMode.load(std::memory_order_relaxed) == 2u;
    const ps2x::present::Aspect presentAspect = ps2x::present::aspectFromEnv(std::getenv("PS2X_ASPECT"), anamorphic);
    const ps2x::present::Filter presentFilter = ps2x::present::filterFromEnv(std::getenv("PS2X_PRESENT_FILTER"));
    int appliedFilter = -1;
    unsigned int appliedFilterTexId = 0u;
    auto vsyncRateWall = std::chrono::steady_clock::now();
    uint64_t vsyncRateTick = m_memory.gs().vsyncTick.load();
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        if (vsyncRateLog)
        {
            const auto now = std::chrono::steady_clock::now();
            const double secs = std::chrono::duration<double>(now - vsyncRateWall).count();
            if (secs >= 5.0)
            {
                const uint64_t vt = m_memory.gs().vsyncTick.load();
                const double rate = static_cast<double>(vt - vsyncRateTick) / secs;
                std::fprintf(stderr, "[vsync-rate] tick=%llu rate=%.2f/s (%.3fx of 59.94)\n",
                             static_cast<unsigned long long>(vt), rate, rate / 59.94);
#if defined(__APPLE__)
                static const bool s_threadCpuLog = [] {
                    const char *v = std::getenv("PS2X_THREAD_CPU_LOG");
                    return v && std::strcmp(v, "1") == 0;
                }();
                if (s_threadCpuLog)
                    logThreadCpu(vt);
#endif
                vsyncRateWall = now;
                vsyncRateTick = vt;
            }
        }
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);
            }
        });
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        UploadFrame(frameTex, this, presentWidth, presentHeight);

#if defined(PS2X_IOS)
        ps2x::ios::syncWindowSize();
        std::fprintf(stderr, "[ios-render] screen=%dx%d render=%dx%d\n",
                     GetScreenWidth(), GetScreenHeight(), GetRenderWidth(), GetRenderHeight());
#endif
        BeginDrawing();
        ClearBackground(BLACK);
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        // I26 (G46 §4): 4:3 display aspect (PS2X_ASPECT=native keeps the
        // pixel aspect) and bilinear filtering unless the drawable-pixel
        // scale is whole on both axes (PS2X_PRESENT_FILTER=point|bilinear).
        const ps2x::present::Rect pr =
            ps2x::present::presentRect(screenWidth, screenHeight, srcWidth, srcHeight, presentAspect);
        const float dpiScale = (screenWidth > 0.0f) ? static_cast<float>(GetRenderWidth()) / screenWidth : 1.0f;
        const int wantFilter = ps2x::present::useBilinear(presentFilter, pr.w * dpiScale / srcWidth, pr.h * dpiScale / srcHeight)
                                   ? TEXTURE_FILTER_BILINEAR
                                   : TEXTURE_FILTER_POINT;
        if (wantFilter != appliedFilter || frameTex.id != appliedFilterTexId)
        {
            SetTextureFilter(frameTex, wantFilter);
            appliedFilter = wantFilter;
            appliedFilterTexId = frameTex.id; // HR1: UploadFrame may re-create the texture
        }
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        const Rectangle dstRect{pr.x, pr.y, pr.w, pr.h};
        DrawTexturePro(frameTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        // I26: virtual controls, hidden while a connected game controller is in use.
        const bool vpadPadConnected = vpadWanted && gamepadInUse();
        if (vpadWanted && vpadPadConnected != vpadLastPadConnected)
        {
            int padIndex = -1;
            for (int i = 0; i < 4 && padIndex < 0; ++i)
            {
                if (IsGamepadAvailable(i))
                    padIndex = i;
            }
            const char *padName = padIndex >= 0 ? GetGamepadName(padIndex) : nullptr;
            std::fprintf(stderr, "[vpad] on=1 pad_in_use=%d first_pad=%d name=\"%s\" -> overlay %s\n", vpadPadConnected ? 1 : 0,
                         padIndex, padName ? padName : "", vpadPadConnected ? "hidden" : "shown");
            vpadLastPadConnected = vpadPadConnected;
        }
        // IN2: the render thread publishes the vpad + raylib button union
        // to the pad latch every host frame (host ~60 Hz vs guest reads at
        // guest speed); readState consumes one presented mask per guest
        // read. PS2X_PAD_LATCH=0 keeps the pre-IN2 liveMask publish.
        const bool padLatchOn = ps2x::padlatch::latchEnabled();
        const uint16_t raylibPressed = padLatchOn ? ps2xSampleRaylibPad().pressed : 0u;
        if (vpadWanted && !vpadPadConnected)
        {
            const ps2x::vpad::Layout layout = ps2x::vpad::makeLayout(screenWidth, screenHeight);
            float touchX[8];
            float touchY[8];
            int touches = virtualPadTouches(touchX, touchY, 8, screenWidth, screenHeight);
            touches = ps2x::vpad::activeTestTouches(vpadTestTouches, m_memory.gs().vsyncTick.load(), screenWidth,
                                                    screenHeight, touchX, touchY, touches, 8);
            uint16_t pressed = ps2x::vpad::pressedMask(layout, touchX, touchY, touches);
            pressed = static_cast<uint16_t>(pressed | ps2x::vpad::activeTestTap(vpadTestTaps, ps2x::padlatch::wallMs()));
            if (padLatchOn)
                ps2x::padlatch::sharedLatch().publish(static_cast<uint16_t>(pressed | raylibPressed));
            else
                ps2x::vpad::liveMask().store(pressed, std::memory_order_relaxed);
            ps2x::vpad::StickVec stick = ps2x::vpad::updateStick(vpadStick, layout, touchX, touchY, touches);
            if (vpadTestStick)
            {
                stick.active = true; // drawn deflected at the rest position
                stick.ax = layout.stickRestX;
                stick.ay = layout.stickRestY;
                stick.x = vpadTestStickX;
                stick.y = vpadTestStickY;
            }
            uint8_t stickLX = 0x80u, stickLY = 0x80u;
            ps2x::vpad::stickBytes(stick, stickLX, stickLY);
            ps2x::vpad::liveStick().store(static_cast<uint16_t>(stickLX | (stickLY << 8)), std::memory_order_relaxed);
            drawVirtualPad(layout, pressed, stick);
        }
        else if (vpadWanted)
        {
            if (padLatchOn)
                ps2x::padlatch::sharedLatch().publish(raylibPressed);
            else
                ps2x::vpad::liveMask().store(0u, std::memory_order_relaxed);
            ps2x::vpad::liveStick().store(ps2x::vpad::kStickNoOverride, std::memory_order_relaxed);
        }
        else if (padLatchOn)
        {
            // Overlay off (desktop default): raylib buttons still feed the latch.
            ps2x::padlatch::sharedLatch().publish(raylibPressed);
        }
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        EndDrawing();

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();
    if (gameUsesPthread)
    {
#if defined(__unix__) || defined(__APPLE__)
        pthread_join(gamePthread, nullptr);
#endif
    }
    else if (gameThread.joinable())
    {
        gameThread.join();
    }

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    UnloadTexture(frameTex);
    CloseWindow();

    RUNTIME_LOG("[run] exiting loop");

    // The game thread and final host diagnostic producers have finished.
    // main uses _Exit after run(), so close observations here; the destructor
    // retains its safe fallback (shutdown closes the shared sink once).
    ps2_e15::closure(m_memory.gs().vsyncTick.load());
    ps2_e7::shutdown(m_memory.gs().vsyncTick.load());
}
