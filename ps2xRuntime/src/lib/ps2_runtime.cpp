#include "ps2_runtime.h"
#include "ps2_ts2_split60.h"
#include "ps2_mtvu.h"
#include "ps2_microvu.h"
#include "ps2_e4.h"
#include "ps2_e7.h"
#include "ps2_mpg_src_trace.h"
#include "ps2_pk.h"
#include "ps2_rr1_alpha_tap.h"
#include "ps2_uv1_counters.h"
#include "ps2_vif_mpg_log.h"
#include "ps2_vq.h"
#include "ps2_e3.h"
#include "ps2_e41_trace.h"
#include "ps2_e43_trace.h"
#include "ps2_e44_trace.h"
#include "ps2_gfx_stats.h"
#include "ps2_fh1_full120.h"
#include "ps2_ssx3_course_manifest.h"
#include "ps2_ssx3_lod.h"
#include "ps2_ssx3_tricky_menu.h"
#include "ps2_log.h"
#include "ps2_android_pause.h"
#include "ps2_park_snapshot.h"
#include "ps2_present_fallback.h"
#include "ps2_present_geometry.h"
#include "ps2_pad_latch.h"
#include "ps2_adpf.h"
#include "ps2_perf_log.h"
#include "ps2_vsync_lock.h"
#include "ps2_virtual_pad.h"
#include "runtime/ps2_pad.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_stream_capture.h"
#include "runtime/gs/ps2_gs_external_backend.h"
#include "runtime/gs/ps2_present_share.h"
#include "runtime/gs/ps2_present_vk_ledger.h" // DP1: displayHz() (pure; same value on every platform)
#if defined(__ANDROID__)
#include "runtime/gs/ps2_present_vk.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/api-level.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <pthread.h>
extern "C" struct android_app *GetAndroidApp(void); // raylib rcore_android.c
namespace
{
// AP1: immersive full-screen for the NativeActivity (raylib sets no system-UI
// visibility, so the gesture-bar handle draws over the game). API 30+:
// WindowInsetsController.hide(systemBars()) with transient-bars-by-swipe (a
// swipe reveals the bars, which then auto-hide); API 29 (our minSdk): the
// legacy IMMERSIVE_STICKY systemUiVisibility flags. The system clears it on
// window/focus changes, so it is re-applied on INIT_WINDOW and GAINED_FOCUS.
bool ap1HideSystemBars(struct android_app *app)
{
    if (!app || !app->activity || !app->activity->vm)
        return false;
    JNIEnv *env = nullptr;
    // Keep the thread's name: an unnamed attach renames it "Thread-N" (as in
    // the Vulkan sink's queryLayerSize).
    char name[16] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    JavaVMAttachArgs args = {JNI_VERSION_1_6, name[0] ? name : nullptr, nullptr};
    if (app->activity->vm->AttachCurrentThread(&env, &args) != JNI_OK || !env)
        return false;
    bool applied = false;
    const char *mode = "";
    jobject act = app->activity->clazz;
    jclass actCls = env->GetObjectClass(act);
    jmethodID getWindow = actCls ? env->GetMethodID(actCls, "getWindow", "()Landroid/view/Window;") : nullptr;
    jobject win = getWindow ? env->CallObjectMethod(act, getWindow) : nullptr;
    if (win && !env->ExceptionCheck())
    {
        jclass winCls = env->GetObjectClass(win);
        jmethodID getDecor = winCls ? env->GetMethodID(winCls, "getDecorView", "()Landroid/view/View;") : nullptr;
        jobject view = getDecor ? env->CallObjectMethod(win, getDecor) : nullptr;
        if (view && !env->ExceptionCheck())
        {
            jclass viewCls = env->GetObjectClass(view);
            if (android_get_device_api_level() >= 30)
            {
                jmethodID getController =
                    viewCls ? env->GetMethodID(viewCls, "getWindowInsetsController",
                                               "()Landroid/view/WindowInsetsController;")
                            : nullptr;
                jobject controller = getController ? env->CallObjectMethod(view, getController) : nullptr;
                if (controller && !env->ExceptionCheck())
                {
                    jclass typeCls = env->FindClass("android/view/WindowInsets$Type");
                    jmethodID systemBars =
                        typeCls ? env->GetStaticMethodID(typeCls, "systemBars", "()I") : nullptr;
                    const jint types =
                        (typeCls && systemBars) ? env->CallStaticIntMethod(typeCls, systemBars) : 0;
                    jclass ctlCls = env->GetObjectClass(controller);
                    // AP1 Part 2: hide(int) returns void (the (I)L… lookup
                    // never resolves, which is why Part 1 never applied).
                    jmethodID hide = ctlCls ? env->GetMethodID(ctlCls, "hide", "(I)V") : nullptr;
                    jmethodID setBehavior =
                        ctlCls ? env->GetMethodID(ctlCls, "setSystemBarsBehavior", "(I)V") : nullptr;
                    if (!env->ExceptionCheck() && types != 0 && hide && setBehavior)
                    {
                        env->CallVoidMethod(controller, setBehavior, 2); // BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                        env->CallVoidMethod(controller, hide, types);
                        applied = !env->ExceptionCheck();
                        mode = "insets-transient";
                    }
                    if (ctlCls)
                        env->DeleteLocalRef(ctlCls);
                    if (typeCls)
                        env->DeleteLocalRef(typeCls);
                    env->DeleteLocalRef(controller);
                }
            }
            else
            {
                jmethodID setVis =
                    viewCls ? env->GetMethodID(viewCls, "setSystemUiVisibility", "(I)V") : nullptr;
                if (setVis && !env->ExceptionCheck())
                {
                    // IMMERSIVE_STICKY | LAYOUT_STABLE | LAYOUT_HIDE_NAVIGATION |
                    // LAYOUT_FULLSCREEN | HIDE_NAVIGATION | FULLSCREEN.
                    env->CallVoidMethod(view, setVis, 0x1706);
                    applied = !env->ExceptionCheck();
                    mode = "systemUiVisibility";
                }
            }
            if (viewCls)
                env->DeleteLocalRef(viewCls);
            env->DeleteLocalRef(view);
        }
        if (winCls)
            env->DeleteLocalRef(winCls);
        env->DeleteLocalRef(win);
    }
    if (env->ExceptionCheck())
        env->ExceptionClear();
    if (actCls)
        env->DeleteLocalRef(actCls);
    std::fprintf(stderr, "[immersive] %s%s%s\n", applied ? "applied (" : "NOT applied",
                 applied ? mode : "", applied ? ")" : "");
    return applied;
}
// VK1: raylib's app-command handler, wrapped so the SurfaceControl child is
// detached before the window goes (TERM_WINDOW) and remade after it returns
// (the ANativeWindow pointer can be reused across background/foreground).
// AP1: also re-applies immersive mode (the wrapper is installed on the GL
// path too, where the TERM_WINDOW call is a no-op).
void (*g_vk1RaylibOnAppCmd)(struct android_app *, int32_t) = nullptr;
// BG1: pause-on-background transitions (defined below; they need the runtime).
void bg1OnPause();
void bg1OnResume();
void vk1OnAppCmd(struct android_app *app, int32_t cmd)
{
    if (cmd == APP_CMD_TERM_WINDOW)
        ps2x_present_vk::windowLost();
    if (cmd == APP_CMD_INIT_WINDOW || cmd == APP_CMD_GAINED_FOCUS)
        ap1HideSystemBars(app);
    // BG1: APP_CMD_PAUSE is the first background signal (before LOST_FOCUS /
    // STOP / TERM_WINDOW); APP_CMD_RESUME precedes GAINED_FOCUS on return.
    // Both run on the main thread inside raylib's PollInputEvents. The pause
    // waits for the game thread's gate ack (bounded) and flushes GS caches.
    if (cmd == APP_CMD_PAUSE)
        bg1OnPause();
    else if (cmd == APP_CMD_RESUME)
        bg1OnResume();
    if (g_vk1RaylibOnAppCmd)
        g_vk1RaylibOnAppCmd(app, cmd);
}
} // namespace
#endif
#if defined(__ANDROID__)
// VK1 Part 2: premultiplied-correct blending for the overlay drawn over a
// transparent GL window (raylib is built as C; rlgl.h is not included here).
extern "C" void rlSetBlendFactorsSeparate(int glSrcRGB, int glDstRGB, int glSrcAlpha, int glDstAlpha, int glEqRGB,
                                          int glEqAlpha);
#endif
#if defined(PS2X_IOS)
// HR1 spike: blend off around the shared-texture quad. Declared here because
// rlgl.h redefines raylib.h types in this TU (raylib is built as C).
extern "C" {
void rlDrawRenderBatchActive(void);
void rlDisableColorBlend(void);
void rlEnableColorBlend(void);
}
#endif
#include "ps2_e7.h"
#include "ps2_e15.h"
#include "ps2_pk.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ee_guest_unwind.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "Kernel/Stubs/Pad.h"
#include "ps2_snd_audio_output.h"
#include "ps2_thread_affinity.h"
#include "ps2_host_backend.h"
#include "ps2_iop_host.h"
#include "runtime/ps2_savestate.h"
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
#include <map>
#include <mutex>
#define XXH_NO_XXH32
#define XXH_NO_XXH3
#define XXH_INLINE_ALL
#include "runtime/third_party/xxhash.h"
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
    // GF1: PS2X_GS_HANDOFF_DIET=1 turns on the cheaper unit -> GS worker
    // handoff (default off). Read once, at GS setup.
    bool gsHandoffDietRequested()
    {
        const char *env = std::getenv("PS2X_GS_HANDOFF_DIET");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }

    // GF1 H4: with the diet, the GS queue holds 8192 descriptors instead of
    // 1024 (the 16 MiB byte cap stays), so the unit runs ahead through the GS
    // worker's GPU waits instead of blocking. PS2X_GS_QUEUE_DESC overrides
    // (e.g. 1024 to measure H1-H3 alone). Host bound only. 0 = default.
    size_t gsQueueDescriptors()
    {
        if (!gsHandoffDietRequested())
            return 0u;
        size_t desc = 8192u;
        if (const char *env = std::getenv("PS2X_GS_QUEUE_DESC"))
        {
            const long v = std::strtol(env, nullptr, 10);
            if (v >= 16 && v <= 1048576)
                desc = static_cast<size_t>(v);
        }
        return desc;
    }

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

    // TK15: SSX 3 terrain patch cache guard (PS2X_SSX3_PATCH_CACHE_GUARD=1,
    // default off; guest-affecting, only when the cache is full). 0x3747A0
    // (cache a0, patch a1, type a2) pops a tessellation slot from the type's
    // free stack (count [a0+0x3C+4*type], capacities 900/220/140). With the
    // stack empty it takes slot -1 and still writes the slot's grids and
    // slot record before their buffers, which corrupts the neighbouring free
    // stacks and then patch link words (TK15: imported Tricky terrain puts
    // 900+ far patches in view). The four callers (0x38BEE0, 0x38BF68,
    // 0x38BFE0, 0x38C098) already treat a negative slot as "not drawn this
    // frame" and retry next frame, so return -1 without running the body.
    // TK17: with t0 != 0 (types 1/2, patch [+0xC] flag) the body also pops a
    // second buffer from another free stack (count [a0+0x1D8+4*type], stack
    // [a0+0x1CC+4*type], buffers [a0+0x1B4+4*type] + index*0xC0/0x1B0/0x300)
    // with no empty check at all: at count 0 it reads stack[-1] as the index
    // (type 1: 0x251) and func_374518 writes that buffer, which lands on the
    // patch packet blocks (TK17: Elysium t4368 overwrote type-0 slot 474's
    // DMA tag, and VU1 ran away on the garbage). Same skip for that case.
    constexpr uint32_t kSsx3PatchCacheAlloc = 0x003747A0u;
    std::atomic<uint64_t> g_ssx3PatchCacheSkips{0u};
    std::atomic<uint64_t> g_ssx3PatchCacheSkips2{0u};

    // TK34: grow the type-1 cache (PS2X_SSX3_PATCH_CACHE_GROW=1, default
    // off; guest-affecting only once the stock 220 slots run out). Imported
    // Tricky terrain (Mesablanca mine, TK33 §2) wants more than 220 type-1
    // slots: a slot stays held two sweeps after its patch leaves the visible
    // list (sub_00374B38 clears mark bits 0x4 then 0x8, frees on the third),
    // so turnover overruns the stack while the list itself is <= 220.
    // Everything per slot is a heap block sized by the stock capacities:
    // sub_0038AF30 calls the initializer sub_00372B78(cache = r+0x10, 900,
    // 220, 140, second pools 0, 146, 93) and allocates the 0x100-B DMA blocks
    // (cap*0x100 at r+0x440..0x448, copies at r+0x44C..0x454). For type 1
    // (cache c): records 12 B [c+0x10] (copy c+0x1C), free stack [c+0x28]
    // (copy c+0x34, count c+0x40), buffers 0x1B0/0x120/0x120 B at
    // [c+0x4C]/[c+0x58]/[c+0x64], second pool 0x1B0 B [c+0x1B8] with stack
    // [c+0x1C4] (copy c+0x1D0, count c+0x1DC), caps [c+4] and [c+0x1AC].
    // Readers use the copies and base+slot*stride; the sweep loops to [c+4].
    // Growth is lazy, so RAM stays byte-identical to stock until the first
    // empty pop: then the arrays are copied into a free region above the
    // runtime HLE pools (default 0x01F31400, PS2X_SSX3_PATCH_CACHE_GROW_BASE),
    // the new slots are pushed and every pointer above is repointed. The old
    // blocks stay intact (a slot's DMA block is built once at allocation and
    // may hold REF tags to its old buffers; an in-flight chain may call them),
    // and free() (0x317E98) of a relocated pointer is handed the original
    // heap block, so the game's deinit frees exactly what it allocated. The
    // region must read all-zero and end >= 32 KB below the current sp (the
    // main stack sits at the top of RAM: sp 0x1FF7790 in a Mesablanca race),
    // else growth is refused and the TK15 guard (if armed) keeps skipping.
    constexpr uint32_t kSsx3PatchCacheInit = 0x00372B78u;
    constexpr uint32_t kSsx3GuestFree = 0x00317E98u;
    struct Ssx3PatchGrow
    {
        bool active = false;
        bool refused = false;
        uint32_t cache = 0u;
        std::vector<std::pair<uint32_t, uint32_t>> moved; // relocated -> original
    };
    Ssx3PatchGrow g_ssx3PatchGrow;

    uint32_t ssx3PatchGrowCap()
    {
        static const uint32_t cap = [] {
            const char *e = std::getenv("PS2X_SSX3_PATCH_CACHE_GROW");
            if (!e || !*e || std::strcmp(e, "0") == 0)
                return 0u;
            uint32_t v = std::strcmp(e, "1") == 0 ? 440u : static_cast<uint32_t>(std::strtoul(e, nullptr, 0));
            if (v <= 220u || v > 2048u)
            {
                std::fprintf(stderr, "[ssx3-patch-grow] refused PS2X_SSX3_PATCH_CACHE_GROW=%s (1 or 221..2048)\n", e);
                return 0u;
            }
            std::fprintf(stderr, "[ssx3-patch-grow] armed (type-1 cache 220 -> %u on the first empty pop)\n", v);
            return v;
        }();
        return cap;
    }

    bool ssx3PatchCacheGrow(uint8_t *rdram, R5900Context *ctx, uint32_t cache)
    {
        Ssx3PatchGrow &g = g_ssx3PatchGrow;
        const uint32_t newCap = ssx3PatchGrowCap();
        if (newCap == 0u || g.active || g.refused)
            return false;
        auto rd = [&](uint32_t a) {
            uint32_t v = 0u;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4u);
            return v;
        };
        auto wr = [&](uint32_t a, uint32_t v) { std::memcpy(rdram + (a & PS2_RAM_MASK), &v, 4u); };
        auto refuse = [&](const char *why, uint32_t a, uint32_t b) {
            g.refused = true;
            std::fprintf(stderr, "[ssx3-patch-grow] refused: %s (0x%x 0x%x)\n", why, a, b);
            return false;
        };
        const uint32_t c = cache;
        const uint32_t r = c - 0x10u;
        if ((c & PS2_RAM_MASK) + 0x1E4u > PS2_RAM_SIZE || (c & 3u))
            return refuse("cache", c, 0u);
        const uint32_t cap = rd(c + 4u), secCap = rd(c + 0x1ACu);
        const uint32_t rec = rd(c + 0x10u), stk = rd(c + 0x28u), bufA = rd(c + 0x4Cu), bufB = rd(c + 0x58u),
                       bufC = rd(c + 0x64u), dma = rd(r + 0x444u), secBuf = rd(c + 0x1B8u), secStk = rd(c + 0x1C4u);
        if (rec != rd(c + 0x1Cu) || stk != rd(c + 0x34u) || dma != rd(r + 0x450u) || secStk != rd(c + 0x1D0u))
            return refuse("pointer copies differ", rec, stk);
        if (cap == 0u || cap >= newCap || secCap > 1024u)
            return refuse("capacity", cap, secCap);
        const uint32_t count = rd(c + 0x40u), secCount = rd(c + 0x1DCu);
        if (count > cap || secCount > secCap)
            return refuse("stack count", count, secCount);
        const uint32_t newSec = secCap ? (secCap * newCap + cap - 1u) / cap : 0u;
        static const uint32_t base = [] {
            const char *e = std::getenv("PS2X_SSX3_PATCH_CACHE_GROW_BASE");
            return e && *e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0x01F31400u;
        }();
        auto al = [](uint32_t v) { return (v + 0xFFu) & ~0xFFu; };
        uint32_t at = base & ~0xFFu;
        auto take = [&](uint32_t bytes) { const uint32_t a = at; at += al(bytes); return a; };
        const uint32_t nRec = take(newCap * 12u), nStk = take(newCap * 4u), nA = take(newCap * 0x1B0u),
                       nB = take(newCap * 0x120u), nC = take(newCap * 0x120u), nDma = take(newCap * 0x100u),
                       nSecBuf = take(newSec * 0x1B0u), nSecStk = take(newSec * 4u);
        const uint32_t lo = base & ~0xFFu, hi = at;
        if (lo < 0x01F31300u || hi > PS2_RAM_SIZE)
            return refuse("region bounds", lo, hi);
        const uint32_t sp = getRegU32(ctx, 29) & PS2_RAM_MASK;
        if (sp >= lo && sp < hi + 0x8000u)
            return refuse("region holds the stack", sp, hi);
        for (uint32_t a = lo; a < hi; a += 4u)
            if (rd(a) != 0u)
                return refuse("region not zero", a, rd(a));
        const uint32_t high = rec & ~PS2_RAM_MASK; // keep the game's address segment
        auto cp = [&](uint32_t dst, uint32_t src, uint32_t bytes) {
            if (bytes)
                std::memcpy(rdram + (dst & PS2_RAM_MASK), rdram + (src & PS2_RAM_MASK), bytes);
        };
        cp(nRec, rec, cap * 12u);
        for (uint32_t i = cap; i < newCap; ++i)
            wr(nRec + i * 12u + 8u, 0xFFFFFFFFu); // init: records zero, +8 = -1
        cp(nStk, stk, count * 4u);
        for (uint32_t i = 0; i < newCap - cap; ++i)
            wr(nStk + (count + i) * 4u, cap + i);
        cp(nA, bufA, cap * 0x1B0u);
        cp(nB, bufB, cap * 0x120u);
        cp(nC, bufC, cap * 0x120u);
        cp(nDma, dma, cap * 0x100u);
        cp(nSecBuf, secBuf, secCap * 0x1B0u);
        cp(nSecStk, secStk, secCount * 4u);
        for (uint32_t i = 0; i < newSec - secCap; ++i)
            wr(nSecStk + (secCount + i) * 4u, secCap + i);
        wr(c + 4u, newCap);
        wr(c + 0x40u, count + (newCap - cap));
        wr(c + 0x1ACu, newSec);
        wr(c + 0x1DCu, secCount + (newSec - secCap));
        const std::pair<uint32_t, uint32_t> moves[] = {
            {c + 0x10u, rec}, {c + 0x1Cu, rec}, {c + 0x28u, stk}, {c + 0x34u, stk}, {c + 0x4Cu, bufA},
            {c + 0x58u, bufB}, {c + 0x64u, bufC}, {r + 0x444u, dma}, {r + 0x450u, dma}, {c + 0x1B8u, secBuf},
            {c + 0x1C4u, secStk}, {c + 0x1D0u, secStk}};
        const uint32_t dst[] = {nRec, nRec, nStk, nStk, nA, nB, nC, nDma, nDma, nSecBuf, nSecStk, nSecStk};
        g.moved.clear();
        for (size_t i = 0; i < std::size(moves); ++i)
        {
            const uint32_t to = high | dst[i];
            wr(moves[i].first, to);
            if (i == 1u || i == 3u || i == 8u || i == 11u)
                continue; // copies of the field before; one free each
            if (moves[i].second != 0u)
                g.moved.emplace_back(to, moves[i].second);
        }
        g.active = true;
        g.cache = c;
        std::fprintf(stderr,
                     "[ssx3-patch-grow] grown cache=0x%x type1 %u->%u (count %u->%u) second %u->%u (count %u->%u) "
                     "region=0x%x..0x%x sp=0x%x patch=0x%x\n",
                     c, cap, newCap, count, count + (newCap - cap), secCap, newSec, secCount,
                     secCount + (newSec - secCap), lo, hi, sp, getRegU32(ctx, 5));
        return true;
    }

    // TK34: free() of a relocated block frees the original; re-init forgets.
    void ssx3PatchGrowFree(R5900Context *ctx)
    {
        Ssx3PatchGrow &g = g_ssx3PatchGrow;
        if (!g.active || !ctx)
            return;
        const uint32_t p = getRegU32(ctx, 4);
        for (auto it = g.moved.begin(); it != g.moved.end(); ++it)
        {
            if (it->first != p)
                continue;
            SET_GPR_U32(ctx, 4, it->second);
            g.moved.erase(it);
            if (g.moved.empty())
            {
                g.active = false;
                std::fprintf(stderr, "[ssx3-patch-grow] released cache=0x%x (originals freed)\n", g.cache);
            }
            return;
        }
    }

    void ssx3PatchGrowInit(R5900Context *ctx)
    {
        Ssx3PatchGrow &g = g_ssx3PatchGrow;
        if (ssx3PatchGrowCap() == 0u)
            return;
        if (g.active)
            std::fprintf(stderr, "[ssx3-patch-grow] re-init cache=0x%x with %zu blocks unfreed; reset\n",
                         getRegU32(ctx, 4), g.moved.size());
        g = Ssx3PatchGrow{};
    }

    bool ssx3PatchCacheGuard(uint8_t *rdram, R5900Context *ctx)
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_SSX3_PATCH_CACHE_GUARD");
            const bool v = e && e[0] == '1';
            if (v)
                std::fprintf(stderr, "[ssx3-patch-guard] armed (0x3747A0 returns -1 when a slot/buffer stack it pops is empty)\n");
            return v;
        }();
        const bool grow = ssx3PatchGrowCap() != 0u;
        if ((!on && !grow) || !rdram || !ctx)
            return false;
        const uint32_t type = getRegU32(ctx, 6);
        if (type > 2u)
            return false;
        const uint32_t countAddr = (getRegU32(ctx, 4) + 0x3Cu + 4u * type) & PS2_RAM_MASK;
        if (countAddr > PS2_RAM_SIZE - 4u)
            return false;
        int32_t count = 0;
        std::memcpy(&count, rdram + countAddr, 4u);
        if (grow && type == 1u && g_ssx3PatchGrow.active)
        {
            // Demand after growth: report each new low of the free count by 10s.
            static int32_t low = 1 << 30;
            if (count < low - 9 || (count == 0 && low != 0))
            {
                low = count;
                std::fprintf(stderr, "[ssx3-patch-grow] type1 free low=%d (in use %d of %u)\n", count,
                             static_cast<int32_t>(ssx3PatchGrowCap()) - count, ssx3PatchGrowCap());
            }
        }
        if (count > 0)
        {
            if (getRegU32(ctx, 8) == 0u)
                return false;
            const uint32_t count2Addr = (getRegU32(ctx, 4) + 0x1D8u + 4u * type) & PS2_RAM_MASK;
            if (count2Addr > PS2_RAM_SIZE - 4u)
                return false;
            int32_t count2 = 0;
            std::memcpy(&count2, rdram + count2Addr, 4u);
            if (count2 > 0)
                return false;
            if (grow && type == 1u && ssx3PatchCacheGrow(rdram, ctx, getRegU32(ctx, 4)))
                return false;
            if (!on)
                return false;
            SET_GPR_S32(ctx, 2, -1);
            const uint64_t n2 = g_ssx3PatchCacheSkips2.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n2 <= 4u || (n2 & (n2 - 1u)) == 0u)
                std::fprintf(stderr, "[ssx3-patch-guard] skip2 #%llu type=%u count2=%d patch=0x%x\n",
                             static_cast<unsigned long long>(n2), type, count2, getRegU32(ctx, 5));
            return true;
        }
        if (grow && type == 1u && ssx3PatchCacheGrow(rdram, ctx, getRegU32(ctx, 4)))
            return false;
        if (!on)
            return false;
        SET_GPR_S32(ctx, 2, -1);
        const uint64_t n = g_ssx3PatchCacheSkips.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 4u || (n & (n - 1u)) == 0u)
            std::fprintf(stderr, "[ssx3-patch-guard] skip #%llu type=%u count=%d patch=0x%x\n",
                         static_cast<unsigned long long>(n), type, count, getRegU32(ctx, 5));
        return true;
    }

    // TK15c: SSX 3 draw-bucket table guard (PS2X_SSX3_DRAWTABLE_GUARD=1,
    // default off; guest-affecting, only when the table is full). Sibling of
    // the patch-cache guard above (different function, same budget class).
    // sub_00362DE8 appends {key, head} entries to the per-frame draw table
    // at [s4+count*8+0x7CA8] (count at [s4+0x7CA4], s4 = frame+0x60000) with
    // no bound check; the table holds 1024 entries. Imported Tricky terrain
    // (Elysium Alps) puts 1024+ buckets in view: entry 1024 lands on the
    // per-item struct that sub_00363C20 rewrites per item, producing GIF-like
    // garbage items (0x8804018C) and a fatal GS VRAM alloc (TK15b: count
    // 0x400 at t2473, up to 0x40C). Stock courses never exceed 1024.
    // Every append iteration calls func_364240 (which returns the bucket key
    // in v0) immediately before the hash/append sequence, from 0x36307C
    // (loop 1) and 0x363350 (loop 2); the fallthrough runs the append
    // unconditionally, so the hook observes but never skips the call
    // (generated code continues inline at the fallthrough after a handled
    // dispatch; a pc redirect would be clobbered). When the count has
    // reached the cap, write back cap-1: the iteration's own append then
    // reuses slot 1023 for a genuine {key, head} pair and the count returns
    // to the cap. Extra buckets are dropped by eviction for the frame, the
    // table never exceeds 1024, and every stored entry is genuine.
    // TK22: PS2X_SSX3_DRAWTABLE_GUARD=2 refuses the append at all five
    // append sites (=1 stays TK15c above). The table is not the only writer
    // set: per frame (base b = the a0 of the reset sub_00362CC8) the game
    // keeps items at b+0x80 (count [b], bounded by the game at 0xA28),
    // texture records at b+0x51484 (0x18 B, count [b+0x51480], cap 1024,
    // hash heads b+0x674A0 chained via +0x14, sub_00394ED0), bucket records
    // at b+0x57498 (0x20 B, count [b+0x57494], 2048 fit before the hash
    // heads; heads b+0x678A0 chained via +0x1C, lookup sub_00395000) and the
    // {key, head} table at b+0x67CA8 (count [b+0x67CA4], 1024). A new bucket
    // takes a record, then func_364240 (the key), then the hash insert and
    // the table append, at five sites: sub_00362DE8 loops 1/2, sub_0037E120
    // (base s2) and sub_0038B0F8 twice (base [sp+0x234]; s6 = b+0x60000 at
    // the second). TK15c covered only the first two. At the cap, =2 lets the
    // append land on slot 1023 (count written back to 1023) after saving
    // that entry, and puts the saved entry and count 1024 back at the next
    // guest dispatch outside func_364240. That dispatch always comes before
    // any table reader: every reader is in sub_00363490/sub_00364050, which
    // run after a call. The refused record stays in the bucket hash, so
    // later items of that bucket find it, chain onto it and are not drawn
    // this frame. The first 1024 buckets are drawn, the table never
    // exceeds 1024, and every stored entry is genuine.
    // The bucket pool needs its own bound (TK22 s1: Elysium with scenery
    // reaches 2127 records; record 2048 on lands on the texture and bucket
    // hash heads, so sub_00395000 walks a cycle, and record 2112 zeroes the
    // table count). Under =2 the pool stops at 2046 real records; records
    // 2046 (dummy) and 2047 (sink) are reserved. With the pool full, the five
    // bucket lookups are answered on the host (the same read-only walk,
    // without the move-to-front): a hit returns the bucket, a miss returns
    // the sink, so the caller takes its found path and allocates nothing.
    // The sink is never in the hash or the table, so items chained onto it
    // are dropped for the frame. A continuation item ([item+0x1E] != 0) in
    // sub_00362DE8 allocates without a lookup; at its sub_00394ED0 call the
    // pool count is pinned to 2047 (the allocation lands on the sink) and
    // prev to the dummy (so the sink is never linked into a drawn chain and
    // never takes the top-level append path).
    // PS2X_SSX3_DRAWTABLE_STATS=1 (observation only) logs the end-of-frame
    // counts at the reset call.
    constexpr uint32_t kSsx3DrawKeyCall = 0x00364240u;
    constexpr uint32_t kSsx3DrawKeyEnd = 0x00364360u;
    constexpr uint32_t kSsx3DrawAppendLoop1Call = 0x0036307Cu;
    constexpr uint32_t kSsx3DrawAppendLoop2Call = 0x00363350u;
    constexpr uint32_t kSsx3DrawAppendModelCall = 0x0037FED8u;
    constexpr uint32_t kSsx3DrawAppendPatch1Call = 0x0038C218u;
    constexpr uint32_t kSsx3DrawAppendPatch2Call = 0x0038C3E8u;
    constexpr uint32_t kSsx3DrawReset = 0x00362CC8u;
    constexpr int32_t kSsx3DrawTableCap = 1024;
    constexpr int32_t kSsx3DrawPoolCap = 2048;
    constexpr int32_t kSsx3DrawPoolReal = 2046; // records 2046 (dummy) and 2047 (sink) reserved under =2
    constexpr uint32_t kSsx3DrawLookup = 0x00395000u;
    constexpr uint32_t kSsx3DrawTexLookup = 0x00394ED0u;
    constexpr uint32_t kSsx3DrawTexLoop1Call = 0x00362F68u;
    constexpr uint32_t kSsx3DrawTexLoop2Call = 0x00363238u;
    constexpr int32_t kSsx3DrawTexCap = 1024;
    std::atomic<uint64_t> g_ssx3DrawTableClamps{0u};

    struct Ssx3DrawTableState
    {
        bool pending = false;
        uint32_t pendingTable = 0u;
        uint32_t savedKey = 0u;
        uint32_t savedHead = 0u;
        uint64_t refusals = 0u;
        uint64_t restoreOdd = 0u;
        uint64_t poolOver = 0u;
        // Stats, per frame (cleared at the reset call).
        uint32_t siteAppends[5] = {};
        uint32_t frameRefusals = 0u;
        uint32_t frameSunk = 0u;
        uint64_t sunk = 0u;
        uint64_t walkCut = 0u;
        uint64_t frames = 0u;
        uint64_t loggedFrames = 0u;
        uint64_t windowTick = 0u;
        int32_t winMax[5] = {};
        uint32_t winAppends = 0u;
    };
    Ssx3DrawTableState g_ssx3Draw;
    std::atomic<bool> g_ssx3DrawPending{false};

    int ssx3DrawTableMode()
    {
        static const int mode = [] {
            const char *e = std::getenv("PS2X_SSX3_DRAWTABLE_GUARD");
            const int m = (e && e[0] == '1') ? 1 : (e && e[0] == '2') ? 2 : 0;
            if (m == 1)
                std::fprintf(stderr, "[ssx3-drawtable-guard] armed (draw table in 0x362DE8 clamps at 1024 entries)\n");
            else if (m == 2)
                std::fprintf(stderr, "[ssx3-drawtable-guard] armed mode 2 (all five append sites refuse at 1024 entries)\n");
            return m;
        }();
        return mode;
    }

    bool ssx3DrawTableStatsOn()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_SSX3_DRAWTABLE_STATS");
            const bool v = e && e[0] == '1';
            if (v)
                std::fprintf(stderr, "[ssx3-drawtable-stats] armed (end-of-frame counts at the 0x362CC8 reset)\n");
            return v;
        }();
        return on;
    }

    int32_t ssx3ReadS32(const uint8_t *rdram, uint32_t addr)
    {
        addr &= PS2_RAM_MASK;
        if (addr > PS2_RAM_SIZE - 4u)
            return 0;
        int32_t v = 0;
        std::memcpy(&v, rdram + addr, 4u);
        return v;
    }

    void ssx3WriteU32(uint8_t *rdram, uint32_t addr, uint32_t v)
    {
        addr &= PS2_RAM_MASK;
        if (addr > PS2_RAM_SIZE - 4u)
            return;
        std::memcpy(rdram + addr, &v, 4u);
    }

    // Table base (b + 0x60000) at each append site's func_364240 call, or 0.
    uint32_t ssx3DrawTableBase(const uint8_t *rdram, const R5900Context *ctx, uint32_t sourcePc, int &site)
    {
        switch (sourcePc)
        {
        case kSsx3DrawAppendLoop1Call: site = 0; return getRegU32(ctx, 20);
        case kSsx3DrawAppendLoop2Call: site = 1; return getRegU32(ctx, 20);
        case kSsx3DrawAppendModelCall: site = 2; return getRegU32(ctx, 18) + 0x60000u;
        case kSsx3DrawAppendPatch1Call:
            site = 3;
            return static_cast<uint32_t>(ssx3ReadS32(rdram, getRegU32(ctx, 29) + 0x234u)) + 0x60000u;
        case kSsx3DrawAppendPatch2Call: site = 4; return getRegU32(ctx, 22);
        default: site = -1; return 0u;
        }
    }

    void ssx3DrawTableRestore(uint8_t *rdram)
    {
        Ssx3DrawTableState &st = g_ssx3Draw;
        const uint32_t tb = st.pendingTable;
        const int32_t count = ssx3ReadS32(rdram, tb + 0x7CA4u);
        if (count != kSsx3DrawTableCap && ++st.restoreOdd <= 8u)
            std::fprintf(stderr, "[ssx3-drawtable-guard] restore saw count=%d (expected %d)\n", count, kSsx3DrawTableCap);
        const uint32_t slot = tb + 0x7CA8u + 8u * static_cast<uint32_t>(kSsx3DrawTableCap - 1);
        ssx3WriteU32(rdram, slot, st.savedKey);
        ssx3WriteU32(rdram, slot + 4u, st.savedHead);
        ssx3WriteU32(rdram, tb + 0x7CA4u, static_cast<uint32_t>(kSsx3DrawTableCap));
        st.pending = false;
        g_ssx3DrawPending.store(false, std::memory_order_relaxed);
    }

    void ssx3DrawTableGuard(uint8_t *rdram, R5900Context *ctx, uint32_t sourcePc)
    {
        const int mode = ssx3DrawTableMode();
        const bool stats = ssx3DrawTableStatsOn();
        if ((mode == 0 && !stats) || !rdram || !ctx)
            return;
        int site = -1;
        const uint32_t tb = ssx3DrawTableBase(rdram, ctx, sourcePc, site);
        if (site < 0)
            return;
        if (stats)
            ++g_ssx3Draw.siteAppends[site];
        if (mode == 1)
        {
            if (site > 1)
                return;
            const uint32_t countAddr = (tb + 0x7CA4u) & PS2_RAM_MASK;
            if (countAddr > PS2_RAM_SIZE - 4u)
                return;
            int32_t count = 0;
            std::memcpy(&count, rdram + countAddr, 4u);
            if (count < kSsx3DrawTableCap)
                return;
            const int32_t clamped = kSsx3DrawTableCap - 1;
            std::memcpy(rdram + countAddr, &clamped, 4u);
            const uint64_t n = g_ssx3DrawTableClamps.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 4u || (n & (n - 1u)) == 0u)
                std::fprintf(stderr, "[ssx3-drawtable-guard] clamp #%llu loop=%c count=%d\n",
                             static_cast<unsigned long long>(n), (sourcePc == kSsx3DrawAppendLoop1Call) ? '1' : '2', count);
            return;
        }
        if (mode != 2)
            return;
        const int32_t count = ssx3ReadS32(rdram, tb + 0x7CA4u);
        if (count < kSsx3DrawTableCap)
            return;
        Ssx3DrawTableState &st = g_ssx3Draw;
        const uint32_t slot = tb + 0x7CA8u + 8u * static_cast<uint32_t>(kSsx3DrawTableCap - 1);
        st.pendingTable = tb;
        st.savedKey = static_cast<uint32_t>(ssx3ReadS32(rdram, slot));
        st.savedHead = static_cast<uint32_t>(ssx3ReadS32(rdram, slot + 4u));
        st.pending = true;
        g_ssx3DrawPending.store(true, std::memory_order_relaxed);
        ssx3WriteU32(rdram, tb + 0x7CA4u, static_cast<uint32_t>(kSsx3DrawTableCap - 1));
        ++st.frameRefusals;
        const uint64_t n = ++st.refusals;
        if (n <= 4u || (n & (n - 1u)) == 0u)
            std::fprintf(stderr, "[ssx3-drawtable-guard] refuse #%llu site=%d count=%d\n",
                         static_cast<unsigned long long>(n), site, count);
    }

    uint32_t ssx3DrawPoolRecord(uint32_t b, int32_t index)
    {
        return b + 0x57498u + 0x20u * static_cast<uint32_t>(index);
    }

    bool ssx3DrawLookupSite(uint32_t sourcePc)
    {
        return sourcePc == 0x00363008u || sourcePc == 0x003632DCu || sourcePc == 0x0037FE7Cu ||
               sourcePc == 0x0038C1ACu || sourcePc == 0x0038C380u;
    }

    // =2, a call to sub_00395000 (a0 = b, a1 = tex record, a2 = packed,
    // a3 = hash, t0 = [item+0x1C]). Returns true when the call is answered
    // on the host (pool full): v0 = the matching bucket or the sink.
    bool ssx3DrawPoolLookup(uint8_t *rdram, R5900Context *ctx, uint32_t sourcePc)
    {
        if (ssx3DrawTableMode() != 2 || !rdram || !ctx || !ssx3DrawLookupSite(sourcePc))
            return false;
        const uint32_t b = getRegU32(ctx, 4);
        const int32_t pool = ssx3ReadS32(rdram, b + 0x57494u);
        if (pool < kSsx3DrawPoolReal)
            return false;
        Ssx3DrawTableState &st = g_ssx3Draw;
        const uint32_t key0 = getRegU32(ctx, 5);
        const uint32_t key1 = getRegU32(ctx, 6);
        const uint32_t key2 = getRegU32(ctx, 8);
        uint32_t rec = static_cast<uint32_t>(ssx3ReadS32(rdram, b + 0x678A0u + 4u * (getRegU32(ctx, 7) & 0xFFu)));
        uint32_t found = 0u;
        for (uint32_t steps = 0u; rec != 0u; ++steps)
        {
            if (steps >= 4096u)
            {
                if (++st.walkCut <= 8u)
                    std::fprintf(stderr, "[ssx3-drawtable-guard] lookup walk cut at 4096 steps (hash 0x%x)\n",
                                 getRegU32(ctx, 7) & 0xFFu);
                break;
            }
            if (ssx3ReadS32(rdram, rec + 0x14u) == 0 &&
                static_cast<uint32_t>(ssx3ReadS32(rdram, rec + 4u)) == key1 &&
                static_cast<uint32_t>(ssx3ReadS32(rdram, rec + 8u)) == key2 &&
                static_cast<uint32_t>(ssx3ReadS32(rdram, rec)) == key0)
            {
                found = rec;
                break;
            }
            rec = static_cast<uint32_t>(ssx3ReadS32(rdram, rec + 0x1Cu));
        }
        if (found == 0u)
        {
            found = ssx3DrawPoolRecord(b, kSsx3DrawPoolCap - 1);
            ssx3WriteU32(rdram, found + 0x0Cu, 0u);
            ssx3WriteU32(rdram, found + 0x10u, 0u);
            ssx3WriteU32(rdram, found + 0x18u, 0u);
            ++st.frameSunk;
            const uint64_t n = ++st.sunk;
            if (n <= 4u || (n & (n - 1u)) == 0u)
                std::fprintf(stderr, "[ssx3-drawtable-guard] pool full: sink #%llu site=0x%x pool=%d\n",
                             static_cast<unsigned long long>(n), sourcePc, pool);
        }
        SET_GPR_U32(ctx, 2, found);
        return true;
    }

    // =2, a call to sub_00394ED0 from sub_00362DE8 (s1 = item, s4 = b + 0x60000).
    void ssx3DrawPoolPin(uint8_t *rdram, R5900Context *ctx, uint32_t sourcePc)
    {
        if (sourcePc != kSsx3DrawTexLoop1Call && sourcePc != kSsx3DrawTexLoop2Call)
            return;
        if (ssx3DrawTableMode() != 2 || !rdram || !ctx)
            return;
        const uint32_t b = getRegU32(ctx, 4);
        const int32_t pool = ssx3ReadS32(rdram, b + 0x57494u);
        if (pool < kSsx3DrawPoolReal)
            return;
        ssx3WriteU32(rdram, b + 0x57494u, static_cast<uint32_t>(kSsx3DrawPoolCap - 1));
        const uint32_t item = getRegU32(ctx, 17) & PS2_RAM_MASK;
        if (item > PS2_RAM_SIZE - 0x20u)
            return;
        int16_t cont = 0;
        std::memcpy(&cont, rdram + item + 0x1Eu, 2u);
        if (cont == 0)
            return;
        ssx3WriteU32(rdram, getRegU32(ctx, 20) + 0x7CA0u, ssx3DrawPoolRecord(b, kSsx3DrawPoolReal));
        ++g_ssx3Draw.frameSunk;
    }

    // At the reset call (a0 = b): the previous frame's final counts.
    void ssx3DrawTableFrame(const uint8_t *rdram, const R5900Context *ctx, uint64_t tick)
    {
        const int mode = ssx3DrawTableMode();
        const bool stats = ssx3DrawTableStatsOn();
        if ((mode != 2 && !stats) || !rdram || !ctx)
            return;
        Ssx3DrawTableState &st = g_ssx3Draw;
        const uint32_t b = getRegU32(ctx, 4);
        const int32_t items = ssx3ReadS32(rdram, b);
        const int32_t tex = ssx3ReadS32(rdram, b + 0x51480u);
        const int32_t texStatic = ssx3ReadS32(rdram, b + 0x57484u);
        const int32_t pool = ssx3ReadS32(rdram, b + 0x57494u);
        const int32_t table = ssx3ReadS32(rdram, b + 0x67CA4u);
        if (mode == 2 && (pool > kSsx3DrawPoolCap || tex > kSsx3DrawTexCap))
        {
            const uint64_t n = ++st.poolOver;
            if (n <= 4u || (n & (n - 1u)) == 0u)
                std::fprintf(stderr, "[ssx3-drawtable-guard] pool over cap #%llu t=%llu bucket=%d/%d tex=%d/%d\n",
                             static_cast<unsigned long long>(n), static_cast<unsigned long long>(tick),
                             pool, kSsx3DrawPoolCap, tex, kSsx3DrawTexCap);
        }
        if (!stats)
            return;
        uint32_t appends = 0u;
        for (uint32_t a : st.siteAppends)
            appends += a;
        ++st.frames;
        const bool over = appends > static_cast<uint32_t>(kSsx3DrawTableCap) || pool > kSsx3DrawPoolCap ||
                          tex > kSsx3DrawTexCap || table > kSsx3DrawTableCap;
        if (over)
        {
            const uint64_t n = ++st.loggedFrames;
            if (n <= 64u || (n & 63u) == 0u)
                std::fprintf(stderr,
                             "[ssx3-drawtable-stats] over #%llu t=%llu b=0x%x items=%d tex=%d(static %d) pool=%d table=%d "
                             "appends=%u (%u/%u/%u/%u/%u) refused=%u sunk=%u\n",
                             static_cast<unsigned long long>(n), static_cast<unsigned long long>(tick), b, items, tex,
                             texStatic, pool, table, appends, st.siteAppends[0], st.siteAppends[1], st.siteAppends[2],
                             st.siteAppends[3], st.siteAppends[4], st.frameRefusals, st.frameSunk);
        }
        const int32_t vals[5] = {items, tex, pool, table, static_cast<int32_t>(appends)};
        for (int i = 0; i < 5; ++i)
            st.winMax[i] = std::max(st.winMax[i], vals[i]);
        if (tick >= st.windowTick + 300u)
        {
            std::fprintf(stderr, "[ssx3-drawtable-stats] window t=%llu frames=%llu max items=%d tex=%d pool=%d table=%d appends=%d\n",
                         static_cast<unsigned long long>(tick), static_cast<unsigned long long>(st.frames),
                         st.winMax[0], st.winMax[1], st.winMax[2], st.winMax[3], st.winMax[4]);
            st.windowTick = tick;
            for (int32_t &m : st.winMax)
                m = 0;
        }
        for (uint32_t &a : st.siteAppends)
            a = 0u;
        st.frameRefusals = 0u;
        st.frameSunk = 0u;
    }

    // TK38: SSX 3 camera spatial-query list guard (PS2X_SSX3_SPATIAL_LIST_GUARD=1,
    // default off; guest-affecting only once a list is full). sub_0022ADD8
    // builds a query object q on the stack and fills fixed-size lists with
    // no bound check (TK37): inside nodes {node, mask} at q+0x98 (count
    // q+0x94, 512 x 8 B), straddling nodes at q+0x109C (count q+0x1098, 512
    // x 8 B), then item lists of 4-B entries: list 1 q+0x20A0 (count
    // q+0x209C, 2048), list 2 q+0x40A4 (q+0x40A0, 1024), list 3 q+0x50AC
    // (q+0x50A8, 2048), list 4 q+0x70B0 (q+0x70AC, 512). Imported Tricky
    // courses put all instances in one location, so a wide view straddles
    // more than 512 tree nodes (Elysium: 517); entry 512 lands on list 1's
    // count and the later appends write random RAM (TK37 JALR).
    // Appends happen inline, so the guard works at the calls around them:
    // - the tree walk (inside 0x22AABC, straddle 0x22AC54) recurses by goto
    //   inside the one generated sub_0022A830, so no append reaches a
    //   dispatch. A straddle overrun only writes q+0x209C..: list 1-4 counts,
    //   entries and the pending-item words, which sub_0022ADD8 zeroed before
    //   the walk and nothing reads or writes until it ends. At the first call
    //   after the walk (0x22AF88 -> sub_0022A698) the straddle count is
    //   clamped to 512 (the nodes past it are dropped) and those fields are
    //   zeroed again. That holds while the overrun stays inside q (count <=
    //   3557). An inside overrun is not repairable (entry 512 lands on the
    //   straddle count mid-walk); it is only reported.
    // - one append per call: sub_00229FC8 (list 1 0x22A0EC or list 2
    //   0x22A114) and sub_0022A128 (list 3 or 4). At entry, for each of its
    //   lists that is full, save slot cap-1 and write count cap-1, as the
    //   TK22 draw table does: an append lands on slot cap-1 and the count
    //   returns to cap. The next guest dispatch (the next item's call; returns
    //   and the jump tables don't dispatch) puts the saved entry and count cap
    //   back. If the count reads cap there, the append happened and was
    //   refused (a drop). The first cap entries are kept and every stored
    //   entry is genuine.
    // A dropped node's instances miss that query's visit (no draw/visit for
    // one frame). Stock
    // SSX 3 stays far below every cap, where the guard only reads.
    struct Ssx3SpatialList
    {
        const char *name;
        uint32_t countOff;
        uint32_t entryOff;
        uint32_t stride;
        int32_t cap;
    };
    constexpr Ssx3SpatialList kSsx3SpatialLists[] = {
        {"inside", 0x94u, 0x98u, 8u, 512},
        {"straddle", 0x1098u, 0x109Cu, 8u, 512},
        {"list1", 0x209Cu, 0x20A0u, 4u, 2048},
        {"list2", 0x40A0u, 0x40A4u, 4u, 1024},
        {"list3", 0x50A8u, 0x50ACu, 4u, 2048},
        {"list4", 0x70ACu, 0x70B0u, 4u, 512},
    };
    constexpr uint32_t kSsx3SpatialWalkEndCall = 0x0022AF88u;
    // Fields sub_0022ADD8 zeroes before the walk that a straddle overrun can reach.
    constexpr uint32_t kSsx3QueryZeroed[] = {0x209Cu, 0x40A0u, 0x50A4u, 0x50A8u, 0x70ACu, 0x78B0u,
                                             0x78B4u, 0x7AB8u, 0x7ABCu, 0x7BC0u, 0x7FC4u};
    constexpr int32_t kSsx3StraddleRepairMax = static_cast<int32_t>((0x7FC8u - 0x109Cu) / 8u);
    constexpr uint32_t kSsx3SpatialItems12 = 0x00229FC8u;
    constexpr uint32_t kSsx3SpatialItems34 = 0x0022A128u;
    constexpr uint32_t kSsx3SpatialInside1 = 0x0022A5A0u;
    constexpr uint32_t kSsx3SpatialInside3 = 0x0022A698u;

    struct Ssx3SpatialBorrow
    {
        uint32_t q = 0u;
        int list = -1;
        uint32_t saved[2] = {};
    };
    struct Ssx3SpatialState
    {
        Ssx3SpatialBorrow borrow[2];
        int borrows = 0;
        uint64_t drops[6] = {};
        uint64_t nodeCuts[6] = {};
        int32_t high[6] = {};
        uint64_t restoreOdd = 0u;
    };
    Ssx3SpatialState g_ssx3Spatial;
    std::atomic<bool> g_ssx3SpatialPending{false};

    bool ssx3SpatialGuardOn()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_SSX3_SPATIAL_LIST_GUARD");
            const bool v = e && e[0] == '1';
            if (v)
                std::fprintf(stderr, "[ssx3-spatial-guard] armed (camera query lists refuse appends at capacity: "
                                     "inside/straddle 512, list1 2048, list2 1024, list3 2048, list4 512)\n");
            return v;
        }();
        return on;
    }

    void ssx3SpatialHigh(int list, int32_t count)
    {
        // Observation: report each new high-water mark by 64s.
        int32_t &h = g_ssx3Spatial.high[list];
        if (count < h + 64 && !(count >= kSsx3SpatialLists[list].cap && h < kSsx3SpatialLists[list].cap))
            return;
        h = count;
        std::fprintf(stderr, "[ssx3-spatial-guard] high list=%s count=%d cap=%d\n", kSsx3SpatialLists[list].name,
                     count, kSsx3SpatialLists[list].cap);
    }

    void ssx3SpatialRestore(uint8_t *rdram)
    {
        Ssx3SpatialState &st = g_ssx3Spatial;
        for (int i = 0; i < st.borrows; ++i)
        {
            const Ssx3SpatialBorrow &b = st.borrow[i];
            const Ssx3SpatialList &l = kSsx3SpatialLists[b.list];
            const int32_t count = ssx3ReadS32(rdram, b.q + l.countOff);
            if (count == l.cap)
            {
                const uint64_t n = ++st.drops[b.list];
                if (n <= 4u || (n & (n - 1u)) == 0u)
                    std::fprintf(stderr, "[ssx3-spatial-guard] list=%s drops=%llu\n", l.name,
                                 static_cast<unsigned long long>(n));
            }
            else if (count != l.cap - 1 && ++st.restoreOdd <= 8u)
            {
                std::fprintf(stderr, "[ssx3-spatial-guard] restore saw list=%s count=%d (expected %d or %d)\n", l.name,
                             count, l.cap - 1, l.cap);
            }
            const uint32_t slot = b.q + l.entryOff + l.stride * static_cast<uint32_t>(l.cap - 1);
            ssx3WriteU32(rdram, slot, b.saved[0]);
            if (l.stride == 8u)
                ssx3WriteU32(rdram, slot + 4u, b.saved[1]);
            ssx3WriteU32(rdram, b.q + l.countOff, static_cast<uint32_t>(l.cap));
        }
        st.borrows = 0;
        g_ssx3SpatialPending.store(false, std::memory_order_relaxed);
    }

    void ssx3SpatialBorrowIfFull(uint8_t *rdram, uint32_t q, int list)
    {
        const Ssx3SpatialList &l = kSsx3SpatialLists[list];
        const int32_t count = ssx3ReadS32(rdram, q + l.countOff);
        ssx3SpatialHigh(list, count);
        if (count < l.cap)
            return;
        if (count > l.cap)
        {
            // Already past capacity (an unguarded writer): leave it alone.
            if (++g_ssx3Spatial.restoreOdd <= 8u)
                std::fprintf(stderr, "[ssx3-spatial-guard] list=%s count=%d over cap %d at entry\n", l.name, count,
                             l.cap);
            return;
        }
        Ssx3SpatialState &st = g_ssx3Spatial;
        Ssx3SpatialBorrow &b = st.borrow[st.borrows++];
        b.q = q;
        b.list = list;
        const uint32_t slot = q + l.entryOff + l.stride * static_cast<uint32_t>(l.cap - 1);
        b.saved[0] = static_cast<uint32_t>(ssx3ReadS32(rdram, slot));
        b.saved[1] = l.stride == 8u ? static_cast<uint32_t>(ssx3ReadS32(rdram, slot + 4u)) : 0u;
        ssx3WriteU32(rdram, q + l.countOff, static_cast<uint32_t>(l.cap - 1));
        g_ssx3SpatialPending.store(true, std::memory_order_relaxed);
    }

    void ssx3SpatialCutInside(uint8_t *rdram, R5900Context *ctx, int list, uint32_t chainOff)
    {
        const Ssx3SpatialList &l = kSsx3SpatialLists[list];
        const uint32_t q = getRegU32(ctx, 4);
        const uint32_t entries = getRegU32(ctx, 5);
        const int32_t n = static_cast<int32_t>(getRegU32(ctx, 6));
        if (n <= 0)
            return;
        const int32_t count = ssx3ReadS32(rdram, q + l.countOff);
        int64_t room = static_cast<int64_t>(l.cap) - count;
        int32_t keep = 0;
        uint32_t steps = 0u;
        for (; keep < n; ++keep)
        {
            const uint32_t node = static_cast<uint32_t>(ssx3ReadS32(rdram, entries + 8u * static_cast<uint32_t>(keep)));
            int64_t len = 0;
            for (uint32_t item = static_cast<uint32_t>(ssx3ReadS32(rdram, node + chainOff)); item != 0u && len <= room;
                 item = static_cast<uint32_t>(ssx3ReadS32(rdram, item)))
            {
                ++len;
                if (++steps > 65536u)
                {
                    len = room + 1; // runaway chain: treat as not fitting
                    break;
                }
            }
            if (len > room)
                break;
            room -= len;
        }
        ssx3SpatialHigh(list, count);
        if (keep == n)
            return;
        SET_GPR_S32(ctx, 6, keep);
        const uint64_t c = g_ssx3Spatial.nodeCuts[list] += static_cast<uint64_t>(n - keep);
        const uint64_t calls = ++g_ssx3Spatial.drops[list];
        if (calls <= 4u || (calls & (calls - 1u)) == 0u)
            std::fprintf(stderr, "[ssx3-spatial-guard] list=%s drops=%llu nodes_cut=%d of %d (count=%d, total %llu)\n",
                         l.name, static_cast<unsigned long long>(calls), n - keep, n, count,
                         static_cast<unsigned long long>(c));
    }

    void ssx3SpatialRepairWalk(uint8_t *rdram, uint32_t q)
    {
        Ssx3SpatialState &st = g_ssx3Spatial;
        const int32_t inside = ssx3ReadS32(rdram, q + kSsx3SpatialLists[0].countOff);
        const int32_t straddle = ssx3ReadS32(rdram, q + kSsx3SpatialLists[1].countOff);
        ssx3SpatialHigh(0, inside);
        ssx3SpatialHigh(1, straddle);
        if (inside > kSsx3SpatialLists[0].cap || straddle < 0 || straddle > kSsx3StraddleRepairMax)
        {
            if (++st.restoreOdd <= 8u)
                std::fprintf(stderr, "[ssx3-spatial-guard] unrepairable walk q=0x%x inside=%d straddle=%d\n", q, inside,
                             straddle);
            return;
        }
        if (straddle <= kSsx3SpatialLists[1].cap)
            return;
        st.nodeCuts[1] += static_cast<uint64_t>(straddle - kSsx3SpatialLists[1].cap);
        const uint64_t n = ++st.drops[1];
        if (n <= 4u || (n & (n - 1u)) == 0u)
            std::fprintf(stderr, "[ssx3-spatial-guard] list=straddle drops=%llu count=%d clamped to %d (nodes cut total %llu)\n",
                         static_cast<unsigned long long>(n), straddle, kSsx3SpatialLists[1].cap,
                         static_cast<unsigned long long>(st.nodeCuts[1]));
        ssx3WriteU32(rdram, q + kSsx3SpatialLists[1].countOff, static_cast<uint32_t>(kSsx3SpatialLists[1].cap));
        for (uint32_t off : kSsx3QueryZeroed)
            ssx3WriteU32(rdram, q + off, 0u);
    }

    void ssx3SpatialGuard(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t sourcePc)
    {
        if (!rdram || !ctx)
            return;
        switch (targetPc)
        {
        case kSsx3SpatialItems12:
            ssx3SpatialBorrowIfFull(rdram, getRegU32(ctx, 4), 2);
            ssx3SpatialBorrowIfFull(rdram, getRegU32(ctx, 4), 3);
            return;
        case kSsx3SpatialItems34:
            ssx3SpatialBorrowIfFull(rdram, getRegU32(ctx, 4), 4);
            ssx3SpatialBorrowIfFull(rdram, getRegU32(ctx, 4), 5);
            return;
        case kSsx3SpatialInside1: ssx3SpatialCutInside(rdram, ctx, 2, 0x20u); return;
        case kSsx3SpatialInside3:
            if (sourcePc == kSsx3SpatialWalkEndCall)
                ssx3SpatialRepairWalk(rdram, getRegU32(ctx, 4));
            ssx3SpatialCutInside(rdram, ctx, 4, 0x24u);
            return;
        default: return;
        }
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

    // HP3 F1: only lookupFunction calls this, and only in taps builds.
#if PS2X_ENABLE_DIAG_TAPS
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
#endif

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

    void copyVu0ContextToState(const R5900Context *ctx, VuState &state)
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

    void copyVu0StateToContext(const VuState &state, R5900Context *ctx)
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

// FH1: full120 clock fix (PS2X_SSX3_FULL120_FIX=clock) wraps the leaf
// 0x3a7058 so its interval is in stock wakes (ps2_fh1::clockPostHook). A
// table wrapper covers direct dispatches and checkpoint resumes alike; the
// leaf has no checkpoint, so it always returns to ra.
namespace
{
    PS2Runtime::RecompiledFunction g_fh1Interval = nullptr;

    void fh1IntervalWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        g_fh1Interval(rdram, ctx, runtime);
        if (ctx->pc == ra && !ps2_guest_unwind::pending())
            ps2_fh1::clockPostHook(rdram, ctx, ps2_fh1::kInterval);
    }

    void applyFh1Full120(PS2Runtime &runtime)
    {
        if (!ps2_fh1::clockFix())
            return;
        g_fh1Interval = runtime.lookupFunction(ps2_fh1::kInterval);
        if (!g_fh1Interval || !runtime.replaceFunction(ps2_fh1::kInterval, &fh1IntervalWrapper))
        {
            std::fprintf(stderr, "fh1-full120-refused cannot wrap 0x%x\n", ps2_fh1::kInterval);
            std::abort();
        }
        std::fprintf(stderr, "fh1-full120 clock: 0x%x wrapped (interval in stock wakes)\n", ps2_fh1::kInterval);
    }
}

PS2_REGISTER_GAME_OVERRIDE("ssx3-full120-clock",
                           "SLUS_207.72",
                           0x00100008u,
                           0u,
                           applyFh1Full120);

// TK11: Tricky mode frontend entry (PS2X_SSX3_TRICKY_MENU=1, default off;
// ps2_ssx3_tricky_menu.h). Every wrapper acts at its function's entry (or,
// for the scePadRead glue, after a stub that never calls guest code), so an
// EE checkpoint unwind never skips hook work. Knob off: nothing is wrapped.
namespace
{
    PS2Runtime::RecompiledFunction g_tkMenuBuild = nullptr;
    PS2Runtime::RecompiledFunction g_tkMenuEvent = nullptr;
    PS2Runtime::RecompiledFunction g_tkMapFill = nullptr;
    PS2Runtime::RecompiledFunction g_tkPadGlue = nullptr;
    PS2Runtime::RecompiledFunction g_tkLuiLoad = nullptr;
    PS2Runtime::RecompiledFunction g_tkTrampoline = nullptr;
    __m128i g_tkV0{}; // scePadRead's return registers across a list refresh
    __m128i g_tkV1{};

    void tkLog(PS2Runtime *runtime, const std::string &msg)
    {
        if (msg.empty())
            return;
        const unsigned long long tick =
            runtime ? static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed))
                    : 0ull;
        std::fprintf(stderr, "[ssx3-tricky] tick=%llu %s\n", tick, msg.c_str());
    }

    void tkSetGpr(R5900Context *ctx, int reg, uint32_t value)
    {
        ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
    }

    // Patches every CMNAMER copy in RAM (the game reads it once at boot).
    void tkPatchLabel(uint8_t *rdram, PS2Runtime *runtime)
    {
        using namespace ps2_ssx3_tricky;
        State &s = state();
        for (uint32_t a = findLoc(rdram, PS2_RAM_SIZE, kCmnAmer); a; a = findLoc(rdram, PS2_RAM_SIZE, kCmnAmer, a + 4u))
        {
            std::string msg;
            const int r = patchLabel(rdram, PS2_RAM_SIZE, a, msg);
            if (r < 0)
                s.labelRefused = true;
            else
                s.labelDone = true;
            tkLog(runtime, msg);
        }
    }

    // Main-menu build (0x194D18 runs once per row as the menu appears):
    // leaving Single Event or quitting to the title lands here, so Tricky
    // mode ends and the Map controller is forgotten.
    void tkMenuBuildWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        using namespace ps2_ssx3_tricky;
        State &s = state();
        s.mapController = 0u;
        if (s.tricky)
        {
            std::string msg;
            modeSet(rdram, false, msg);
            s.tricky = false;
            tkLog(runtime, msg.empty() ? "mode: Tricky off (main menu)" : msg);
        }
        // The build calls this once per row: scan FEAMER (reloaded at each
        // frontend entry) once per tick.
        static uint64_t helpTick = ~0ull;
        const uint64_t tick = runtime ? runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u;
        for (uint32_t a = tick == helpTick ? 0u : findLoc(rdram, PS2_RAM_SIZE, kFeAmer); a;
             a = findLoc(rdram, PS2_RAM_SIZE, kFeAmer, a + 4u))
        {
            std::string msg;
            patchHelp(rdram, PS2_RAM_SIZE, a, msg);
            tkLog(runtime, msg);
        }
        helpTick = tick;
        g_tkMenuBuild(rdram, ctx, runtime);
    }

    // Main-menu event handler (this, item, event): on X, Online takes the
    // Single Event state and turns Tricky mode on; any other row turns it off.
    void tkMenuEventWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        using namespace ps2_ssx3_tricky;
        State &s = state();
        const uint32_t item = getRegU32(ctx, 5) & 0x1FFFFFFFu;
        if (getRegU32(ctx, 6) == kEventAccept && item && item + 0x40u < PS2_RAM_SIZE)
        {
            const uint32_t st = ps2_ssx3_tricky::rd32(rdram, item + kItemState);
            bool online = false;
            const uint32_t next = onMainMenuAccept(ps2_ssx3_tricky::rd32(rdram, item + kItemWidgetHash), st, online);
            std::string msg;
            if (online)
            {
                ps2_ssx3_tricky::wr32(rdram, item + kItemState, next);
                s.tricky = true;
                char buf[96];
                std::snprintf(buf, sizeof(buf), "main menu: Tricky Courses -> Single Event (item 0x%06x state 0x%x -> 0x%x)",
                              item, st, next);
                tkLog(runtime, buf);
                modeSet(rdram, true, msg);
                ps2_savestate::noteQuickStatus("Tricky Courses");
            }
            else if (s.tricky)
            {
                modeSet(rdram, false, msg);
                s.tricky = false;
            }
            tkLog(runtime, msg);
        }
        g_tkMenuEvent(rdram, ctx, runtime);
    }

    void tkMapFillWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_ssx3_tricky::state().mapController = getRegU32(ctx, 4);
        g_tkMapFill(rdram, ctx, runtime);
    }

    // scePadRead glue: the stub (TK7/TK9 chord inside) sets pc = ra and never
    // calls guest code. After a chord changed the rows while the Map screen is known,
    // return into 0x200DE8(controller) with ra = the trampoline instead.
    void tkPadGlueWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        using namespace ps2_ssx3_tricky;
        static uint8_t rows[kRowsBytes + kTopoBytes];
        const bool armed = rowsArmed();
        if (armed)
        {
            std::memcpy(rows, rdram + ps2_ssx3_course::kEventBase, kRowsBytes);
            std::memcpy(rows + kRowsBytes, rdram + ps2_ssx3_course::kTopoBase, kTopoBytes);
        }
        g_tkPadGlue(rdram, ctx, runtime);
        State &s = state();
        if (!s.labelDone && !s.labelRefused)
            tkPatchLabel(rdram, runtime);
        if (ps2_ssx3_course::appUpdateFn(rdram) == ps2_ssx3_course::kGameUpdate)
        {
            s.mapController = 0u;
            return;
        }
        if (!armed || s.redirect || !s.mapController ||
            (std::memcmp(rows, rdram + ps2_ssx3_course::kEventBase, kRowsBytes) == 0 &&
             std::memcmp(rows + kRowsBytes, rdram + ps2_ssx3_course::kTopoBase, kTopoBytes) == 0))
            return;
        s.redirect = true;
        s.returnPc = ctx->pc;
        s.sp = getRegU32(ctx, 29);
        g_tkV0 = ctx->r[2];
        g_tkV1 = ctx->r[3];
        tkSetGpr(ctx, 31, kTrampoline);
        tkSetGpr(ctx, 4, s.mapController);
        ctx->pc = kMapFill;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "list refresh: 0x%x(0x%x), then back to 0x%x", kMapFill, s.mapController,
                      s.returnPc);
        tkLog(runtime, buf);
    }

    void tkTrampolineWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        using namespace ps2_ssx3_tricky;
        State &s = state();
        if (s.redirect && getRegU32(ctx, 29) == s.sp)
        {
            ctx->r[2] = g_tkV0;
            ctx->r[3] = g_tkV1;
            tkSetGpr(ctx, 31, s.returnPc);
            ctx->pc = s.returnPc;
            s.redirect = false;
            ++s.refreshes;
            tkLog(runtime, "list refreshed #" + std::to_string(s.refreshes));
            return;
        }
        tkLog(runtime, "trampoline reached without a pending refresh (sp mismatch or no redirect)");
        g_tkTrampoline(rdram, ctx, runtime);
    }

    void tkLuiLoadWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        tkPatchLabel(rdram, runtime);
        g_tkLuiLoad(rdram, ctx, runtime);
    }

    void applyTrickyMenu(PS2Runtime &runtime)
    {
        using namespace ps2_ssx3_tricky;
        if (!knob())
            return;
        const uint8_t *ram = runtime.memory().getRDRAM();
        for (const EntryWord &w : kEntryWords)
            if (!ram || ps2_ssx3_tricky::rd32(ram, w.pc) != w.word)
            {
                std::fprintf(stderr, "[ssx3-tricky] refused: 0x%x reads 0x%08x, expected 0x%08x; nothing wrapped\n",
                             w.pc, ram ? ps2_ssx3_tricky::rd32(ram, w.pc) : 0u, w.word);
                return;
            }
        struct Hook
        {
            uint32_t pc;
            PS2Runtime::RecompiledFunction *orig;
            PS2Runtime::RecompiledFunction wrap;
        };
        const Hook hooks[] = {{kMainMenuItemState, &g_tkMenuBuild, &tkMenuBuildWrapper},
                              {kMainMenuEvent, &g_tkMenuEvent, &tkMenuEventWrapper},
                              {kMapFill, &g_tkMapFill, &tkMapFillWrapper},
                              {kPadReadGlue, &g_tkPadGlue, &tkPadGlueWrapper},
                              {kLuiLoad, &g_tkLuiLoad, &tkLuiLoadWrapper},
                              {kTrampoline, &g_tkTrampoline, &tkTrampolineWrapper}};
        for (const Hook &h : hooks)
            if (!(*h.orig = runtime.lookupFunction(h.pc)))
            {
                std::fprintf(stderr, "[ssx3-tricky] refused: no function at 0x%x; nothing wrapped\n", h.pc);
                return;
            }
        for (const Hook &h : hooks)
            if (!runtime.replaceFunction(h.pc, h.wrap))
            {
                std::fprintf(stderr, "[ssx3-tricky] cannot wrap 0x%x\n", h.pc);
                std::abort();
            }
        state().on = true;
        std::fprintf(stderr, "[ssx3-tricky] armed: Online -> Tricky Courses, Select Event refresh after picker switches\n");
    }
}

PS2_REGISTER_GAME_OVERRIDE("ssx3-tricky-menu",
                           "SLUS_207.72",
                           0x00100008u,
                           0u,
                           applyTrickyMenu);

// LOD1: draw distance (PS2X_SSX3_LOD_SCALE=<f>, default off;
// ps2_ssx3_lod.h). The wrapper scales the far argument ($f14) at the entry
// of the renderer's projection setter and calls the original, so an EE
// checkpoint inside the setter resumes as usual. Knob off: nothing is wrapped.
namespace
{
    PS2Runtime::RecompiledFunction g_lodSetPerspective = nullptr;

    void lodSetPerspectiveWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->f[14] *= ps2_ssx3_lod::scale();
        g_lodSetPerspective(rdram, ctx, runtime);
    }

    void applyLodScale(PS2Runtime &runtime)
    {
        const float s = ps2_ssx3_lod::scale();
        if (s == 0.0f)
            return;
        if (s < 0.0f)
        {
            std::fprintf(stderr, "[ssx3-lod] refused PS2X_SSX3_LOD_SCALE=%s (want %g..%g)\n",
                         std::getenv("PS2X_SSX3_LOD_SCALE"), static_cast<double>(ps2_ssx3_lod::kMinScale),
                         static_cast<double>(ps2_ssx3_lod::kMaxScale));
            std::abort();
        }
        g_lodSetPerspective = runtime.lookupFunction(ps2_ssx3_lod::kSetPerspective);
        if (!g_lodSetPerspective || !runtime.replaceFunction(ps2_ssx3_lod::kSetPerspective, &lodSetPerspectiveWrapper))
        {
            std::fprintf(stderr, "[ssx3-lod] cannot wrap 0x%x\n", ps2_ssx3_lod::kSetPerspective);
            std::abort();
        }
        std::fprintf(stderr, "[ssx3-lod] armed: far plane x%g at 0x%x (streaming and fog stock)\n",
                     static_cast<double>(s), ps2_ssx3_lod::kSetPerspective);
    }
}

PS2_REGISTER_GAME_OVERRIDE("ssx3-lod-scale",
                           "SLUS_207.72",
                           0x00100008u,
                           0u,
                           applyLodScale);

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
    // Gate captures can request up to 24 bounded snapshots without encoding a
    // PNG on every present. Each target accepts the first frame within the
    // following two guest seconds, since present and vsync can be out of phase.
    static const std::array<uint64_t, 24> selectedTicks = [] {
        std::array<uint64_t, 24> ticks{};
        if (const char *env = std::getenv("PS2X_FRAME_DUMP_ONCE_TICKS"))
        {
            const char *next = env;
            size_t count = 0;
            while (*next && count < ticks.size())
            {
                char *end = nullptr;
                const unsigned long long value = std::strtoull(next, &end, 10);
                if (end == next || value == 0u) break;
                ticks[count++] = static_cast<uint64_t>(value);
                if (*end != ',') break;
                next = end + 1;
            }
        }
        return ticks;
    }();
    if (selectedTicks[0] != 0u)
    {
        static std::array<bool, 24> captured{};
        bool selected = false;
        for (size_t i = 0; i < selectedTicks.size(); ++i)
        {
            if (selectedTicks[i] != 0u && !captured[i] &&
                tick >= selectedTicks[i] && tick < selectedTicks[i] + 120u)
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
    if (selectedTicks[0] != 0u) keep = true;
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
    // UX1: build then emit once (was a std::cerr chain; spliced against
    // [rb2] in burst legs). Bytes identical.
    std::ostringstream dumpLine;
    dumpLine << "[frame:dump] seq=" << seq << " tick=" << tick << " size=" << width << "x" << height
             << " fbp=" << displayFbp << "/" << sourceFbp << " fallback=" << (fallback ? 1 : 0) << " fnv1a="
             << std::hex << hash;
    ps2_log::emitLine(dumpLine.str());
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

int virtualPadTouches(ps2x::vpad::TouchPoint *ts, int max, float screenWidth, float screenHeight)
{
#if defined(PS2X_IOS)
    int64_t ids[8];
    float xs[8], ys[8];
    const int n = ps2x::ios::touchPoints(ids, xs, ys, max < 8 ? max : 8);
    for (int i = 0; i < n; ++i)
    {
        ts[i].id = ids[i];
        ts[i].x = xs[i] * screenWidth;
        ts[i].y = ys[i] * screenHeight;
    }
    return n;
#else
    (void)screenWidth;
    (void)screenHeight;
    if (max < 1 || !IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        return 0;
    }
    ts[0].id = 0;
    ts[0].x = static_cast<float>(GetMouseX());
    ts[0].y = static_cast<float>(GetMouseY());
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

// DS1: quick-save slot for the live card root (mirrors getMcRootPath port 0:
// PS2X_MC_ROOT, else <elf dir>/mc0).
std::string ds1QuickSlotPath()
{
    const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
    const std::string mcRoot =
        paths.mcRoot.empty() ? (paths.elfDirectory / "mc0").string() : paths.mcRoot.string();
    return ps2_savestate::quickSlotPath(paths.elfDirectory.string(), mcRoot);
}

void ds1FireQuickSave()
{
    const std::string slot = ds1QuickSlotPath();
    if (!ps2_savestate::requestQuickSave(slot))
    {
        if (ps2_savestate::quickRequestPending())
            ps2_savestate::noteQuickStatus("busy (save/load already pending)");
        else
            ps2_savestate::noteQuickStatus("quick-save failed (see log)");
    }
}

void ds1FireQuickLoad()
{
    const std::string slot = ds1QuickSlotPath();
    std::error_code ec;
    if (!std::filesystem::exists(slot, ec) || ec)
    {
        std::fprintf(stderr, "[savestate] quick-load: no quicksave in slot %s\n", slot.c_str());
        ps2_savestate::noteQuickStatus("no quicksave in this slot yet");
        return;
    }
    if (!ps2_savestate::requestQuickLoad(slot))
    {
        if (ps2_savestate::quickRequestPending())
            ps2_savestate::noteQuickStatus("busy (save/load already pending)");
        else
            ps2_savestate::noteQuickStatus("quick-load failed (see log)");
    }
}

// DS1 DEV-ONLY scheduled chord (host testing without a gamepad):
// PS2X_SAVESTATE_HOTKEY_AT="2000:save,2600:load" fires each op once at the
// first present with vsyncTick >= tick. Malformed entries are ignored.
struct Ds1Hotkey
{
    uint64_t tick = 0u;
    bool save = true;
    bool fired = false;
};
std::vector<Ds1Hotkey> parseDs1Hotkeys(const char *env)
{
    std::vector<Ds1Hotkey> out;
    if (!env || !env[0])
        return out;
    std::string text(env);
    size_t begin = 0u;
    while (begin <= text.size())
    {
        const size_t end = text.find(',', begin);
        const std::string item = text.substr(begin, end == std::string::npos ? end : end - begin);
        const size_t colon = item.find(':');
        if (colon != std::string::npos)
        {
            const uint64_t tick = std::strtoull(item.substr(0, colon).c_str(), nullptr, 10);
            const std::string op = item.substr(colon + 1u);
            if (tick != 0u && (op == "save" || op == "load"))
                out.push_back(Ds1Hotkey{tick, op == "save", false});
            else
                std::fprintf(stderr, "[savestate] ignoring malformed HOTKEY_AT entry '%s'\n", item.c_str());
        }
        else if (!item.empty())
            std::fprintf(stderr, "[savestate] ignoring malformed HOTKEY_AT entry '%s'\n", item.c_str());
        if (end == std::string::npos)
            break;
        begin = end + 1u;
    }
    return out;
}
} // namespace

// HR1: main-thread present cost split, reported by PS2X_THREAD_CPU_LOG=1.
static std::atomic<uint64_t> g_hr1LatchNs{0}, g_hr1UploadNs{0}, g_hr1Uploads{0};
#if defined(PS2X_IOS)
// HR1 spike (PS2X_PRESENT_ZERO_COPY=1 on iOS): the shared GLES texture to draw
// instead of frameTex; id 0 = use frameTex.
static Texture2D g_hr1ShareTex{};
#endif

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

#if defined(__ANDROID__)
namespace
{
// BG1: the live runtime for the pause hook (set in initialize(), before the
// game thread starts; the hook runs on the main thread).
PS2Runtime *g_bg1Runtime = nullptr;

void bg1OnPause()
{
    if (!ps2x::androidPause::requestPause())
        return;
    const uint64_t tick =
        g_bg1Runtime ? g_bg1Runtime->eeScheduler().currentVSyncTick() : 0u;
    ps2_snd_audio_output::pausePlayback();
    // Bounded: a game thread stuck mid-frame must not hang the app-cmd pump.
    const bool acked = ps2x::androidPause::waitGateAck(2000);
    if (g_bg1Runtime)
        g_bg1Runtime->gs().flushExternalCaches();
    // PT2 Part 2a: kill-proof the stage rings (a swipe from the switcher is a
    // SIGKILL; the shutdown dump never runs). Once per pause (requestPause
    // above is the guard), main thread, lock-free ring reads — the gated game
    // thread never stalls on it. Bounded: entries since the last flush (the
    // 60 s timer bounds it) into a fresh tail-pause file.
    ps2x::perflog::flushTail("pause");
    // PR3: the pad recorder's open span + stdio buffer (same kill-proofing).
    ps2_stubs::padRecordFlushNow("pause");
    std::fprintf(stderr, "[bg1] paused at tick=%llu gate_acked=%d\n",
                 static_cast<unsigned long long>(tick), acked ? 1 : 0);
}

void bg1OnResume()
{
    if (!ps2x::androidPause::requestResume())
        return;
    // The gate wakes the game thread first; the ring still holds its
    // pause-time fill, so the resumed stream drains real frames, no gap.
    ps2_snd_audio_output::resumePlayback();
    const uint64_t tick =
        g_bg1Runtime ? g_bg1Runtime->eeScheduler().currentVSyncTick() : 0u;
    std::fprintf(stderr, "[bg1] resumed at tick=%llu\n", static_cast<unsigned long long>(tick));
}
} // namespace
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
#if defined(__ANDROID__)
    // FH6 (PS2X_PRESENT_PER_VSYNC=1): the GS worker queues every guest frame
    // itself once the layer is live, so the per-iteration latch RPC (queued
    // behind the worker's backlog and the previous export's GPU fence) is
    // skipped; the first latch still runs so the layer path starts as before.
    static const bool s_perVsyncPresent = [] {
        const char *v = std::getenv("PS2X_PRESENT_PER_VSYNC");
        return v && std::strcmp(v, "1") == 0;
    }();
    if (s_perVsyncPresent && s_hasUploadedFrame && ps2x_present_vk::active() && ps2x_present_vk::layerLive())
        return;
#endif
#if defined(PS2X_IOS)
    // IQ1 (FH6 on iOS): with PS2X_PRESENT_PER_VSYNC=1 the GS worker exports
    // every guest frame into the IOSurface mailbox itself, so once a shared
    // frame has been shown the latch RPC is skipped; the share block below
    // still picks up the newest published frame every iteration.
    static const bool s_iosPerVsyncPresent = [] {
        const char *v = std::getenv("PS2X_PRESENT_PER_VSYNC");
        return v && std::strcmp(v, "1") == 0;
    }();
    const bool iosSkipLatch = s_iosPerVsyncPresent && s_hasUploadedFrame && g_hr1ShareTex.id != 0u;
#else
    const bool iosSkipLatch = false;
#endif
    if (needsLatch && !iosSkipLatch)
    {
        rt->gs().latchHostPresentationFrame();
        hr1Timer.t1 = std::chrono::steady_clock::now();
        ps2x::perflog::noteLatch(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(hr1Timer.t1 - hr1T0).count()));
        hr1Timer.latched = true;
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame && !iosSkipLatch)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

#if defined(__ANDROID__)
    // VK1 prototype (PS2X_PRESENT_VULKAN=1): the latch above made the backend
    // queue the frame on the SurfaceControl layer; nothing to copy or upload.
    if (ps2x_present_vk::active())
    {
        s_hasUploadedFrame = true;
        return;
    }
#endif
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
#if defined(PS2X_IOS)
    if (ps2x_present_share::enabled())
    {
        ps2x_present_share::SharedFrame shared;
        if (ps2x_present_share::latest(shared))
        {
            const unsigned int id = ps2x_present_share::acquireTexture(shared);
            if (id != 0u)
            {
                g_hr1ShareTex = Texture2D{id, static_cast<int>(shared.width), static_cast<int>(shared.height), 1,
                                          PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                outWidth = s_lastWidth = shared.width;
                outHeight = s_lastHeight = shared.height;
                s_hasUploadedFrame = true;
                return;
            }
        }
        g_hr1ShareTex = Texture2D{};
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
    // CP4-fix: same class as MT1 below; a runtime's MPEG non-stream index
    // entries must not outlive it (see
    // invalidateMpegNonStreamDeliveriesForRuntime).
    ps2_stubs::invalidateMpegNonStreamDeliveriesForRuntime(this);
    // MT1: queued unit work may reference this runtime; the process-wide
    // hooks must not outlive it (tests create many runtimes).
    ps2_mtvu::syncAll();
    ps2_mtvu::stopVifStage(); // VPL2: join the VIF thread (feeds the GIF stage)
    ps2_mtvu::stopGifStage(); // VPL1: join the GIF thread before its hooks go
    ps2_mtvu::setDtFallbackFn({});
    ps2_mtvu::fbrstFn() = {};
    ps2_mtvu::jobEndFn() = {};
    ps2_microvu::shutdown();
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
            m_gs.setQueueEnabled(true, gsQueueDescriptors());
            std::cerr << "[gs:queue] enabled (PS2X_GS_QUEUE=1): CPU backend on GS worker thread"
                      << std::endl;
        }
    }
    // GE2: PS2X_GS_BACKEND=external = the external-GS backend (GE1 when
    // PS2X_GS_EXTERNAL_LIBRARY names it), fed by the queue (forced on: every
    // backend call runs on the one GS worker thread). Unset or any other value
    // keeps the CPU backend. CN2b: the paraLLEl-GS value (parallel) is gone.
    if (ps2x_gs_external::requested() && !m_gs.wantsGuestVsync())
    {
        if (!ps2x_gs_external::available())
        {
            std::cerr << "[gs:external] PS2X_GS_BACKEND=external requested, but this build has no "
                         "external backend; staying on the current backend"
                      << std::endl;
        }
        else
        {
            if (!m_gs.queueEnabled())
            {
                m_gs.setQueueEnabled(true, gsQueueDescriptors());
                std::cerr << "[gs:queue] enabled (forced by PS2X_GS_BACKEND=external)" << std::endl;
            }
            m_gs.setRasterBackend(ps2x_gs_external::create(&m_memory.gs()));
            std::cerr << "[gs:external] live backend selected (PS2X_GS_BACKEND=external)" << std::endl;
        }
    }
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    { m_gs.processGIFPacket(data, size); });
    // GF1 (PS2X_GS_HANDOFF_DIET=1, default off): a cheaper unit -> GS worker
    // handoff. H1: the path note rides in the packet's own command. Host
    // transport only: the worker executes the same commands in the same order.
    const bool handoffDiet = gsHandoffDietRequested();
    if (handoffDiet)
    {
        m_gifArbiter.setProcessPathPacketFn([this](GifPathId path, std::vector<uint8_t> &bytes)
                                            {
                                                const bool note = ps2_gfx_stats::enabled() || m_gs.rawGifBackendActive();
                                                m_gs.processGIFPacketWithPath(path, note, bytes);
                                            });
        // H3: on the unit thread, wake the GS worker once per wakeCmds queued
        // commands (or wakeBytes) and at every unit job end, instead of once
        // per GIF drain (one futex wake per XGKICK). PS2X_GS_WAKE_CMDS tunes.
        uint32_t wakeCmds = 64u;
        if (const char *env = std::getenv("PS2X_GS_WAKE_CMDS"))
        {
            const long v = std::strtol(env, nullptr, 10);
            if (v >= 0 && v <= 65536)
                wakeCmds = static_cast<uint32_t>(v);
        }
        const size_t wakeBytes = 256u * 1024u;
        m_gs.setWorkerDeferredWakes(wakeCmds, wakeBytes);
        if (wakeCmds != 0u)
            ps2_mtvu::jobEndFn() = [this]() { m_gs.flushWorkerWake(); };
        // MP1 L2: lean handoff (sleep-aware notifies, one worker lock per pop,
        // lock-free unit drain batches). Wake timing only: same commands, same
        // order. Needs the deferred wakes (the job-end flush above); always on
        // with them (CU4 B1: the PS2X_GS_LEAN_HANDOFF=0 pre-MP1 path is deleted).
        // GP4 H5: pooled packet buffers (producers acquire, the worker
        // releases after execute). Same bytes, same order; alloc-free only.
        m_gs.setPacketPoolEnabled(true);
        m_gifArbiter.setPacketPool(&m_gs.packetPool());
        // MP1 L3: allocation-lean handoff (pool keeps larger buffers with
        // caps for the in-flight depth; drain skips the identity sort).
        // Same bytes, same order (CU4 B1: the PS2X_GS_ALLOC_LEAN=0 pre-MP1
        // path is deleted).
        // GP4 H6: worker pops up to kPopBatch commands per mutex round
        // (FIFO order preserved; knob-off pops one-by-one as before).
        m_gs.setWorkerPopBatch(GsWorker::kPopBatch);
        std::cerr << "[gs:handoff] diet on (PS2X_GS_HANDOFF_DIET=1): H1 one command per packet, H2 moved bytes, "
                  << "H3 deferred wakes cmds=" << wakeCmds << " bytes=" << wakeBytes
                  << ", H4 queue descriptors=" << gsQueueDescriptors()
                   << ", H5 pooled packet buffers, H6 pop batch=" << GsWorker::kPopBatch
                   << ", MP1 lean=1 alloc-lean=1" << std::endl;
    }
    // MP2 census: UNPACK fast-path vs fallback per format + GIF bytes per
    // path (PS2X_MP2_CENSUS=1, default off). Logged, never hashed; MTVU
    // stays threaded (this flag is not in configure()'s diag list).
    if (const char *env = std::getenv("PS2X_MP2_CENSUS"))
    {
        const bool on = env[0] == '1' && env[1] == '\0';
        ps2_mtvu::setMP2Census(on);
        if (on)
            std::cerr << "[mtvu] mp2 census on (PS2X_MP2_CENSUS=1)" << std::endl;
    }
    // E33: per-path GIF census + GS draw attribution. The listener runs
    // before each packet's process call (same thread, synchronous drain),
    // so draws kicked while processing land on this packet's path. One
    // relaxed check per packet when stats are off.
    m_gifArbiter.setPacketListener([this, handoffDiet](GifPathId path, uint32_t size)
                                   {
                                       const bool stats = ps2_gfx_stats::enabled();
                                       if (stats)
                                       {
                                           ps2_gfx_stats::noteGifPacket(path, size);
                                       }
                                       // GB3 Part 2: a raw-GIF backend needs every packet's path.
                                       // GF1 H1: with the diet the path rides in the packet command.
                                       if (!handoffDiet && (stats || m_gs.rawGifBackendActive()))
                                       {
                                           m_gs.noteGifPath(path);
                                       }
                                   });
    m_memory.setGifArbiter(&m_gifArbiter);
    // MT1 map #10: CFC2 VPU_STAT / CTC2 FBRST are inline in generated code, so
    // VIF1 work runs inline (after a sync) when the kicking context has VU1
    // D/T stops enabled or pending; otherwise every VU1 run leaves bits 9/10
    // clear and the callbacks' VPU_STAT write is a no-op.
    ps2_mtvu::setDtFallbackFn([this]()
                              {
                                  const R5900Context *c = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                  if (!c)
                                      c = &m_cpuContext;
                                  return (c->vu0_fbrst & 0x0C00u) != 0u || (c->vu0_vpu_stat & 0x0600u) != 0u; });
    ps2_mtvu::fbrstFn() = [this]()
    {
        const R5900Context *c = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
        if (!c)
            c = &m_cpuContext;
        return c->vu0_fbrst;
    };
    // MT1: PS2X_MTVU=1 runs the unit on its own thread unless a dev trace
    // that shares state with unit code is armed (those need the inline path).
    // PS2X_PKLOG stays allowed: its packet log is atomic-indexed and locked,
    // so the [pk] sequence (idx, fnv, len, src) is a GS-stream comparator.
    ps2_mtvu::configure(ps2_e7::enabled() || ps2_rr1::alphaTapOn() || ps2_rr1::evOn() ||
                        ps2_mpg_src_trace::enabled() || ps2_gfx_stats::enabled() || ps2x_gs_capture::enabled() ||
                        ps2_vif_mpg_log::enabled() || ps2_e44_trace::enabled() || ps2_e43_trace::enabled() ||
                        ps2_e41_trace::armed() || ps2_uv1_vif_fmt::enabled() || ps2_uv1_dma_stall::enabled() ||
                        ps2_e4::enabled() || ps2_vq::enabled());
    // VPL1: PS2X_MTVU_GIF_STAGE=1 (default off) moves the unit's GIF submit
    // (arbiter + GS-worker handoff) onto its own thread behind an ordered op
    // ring; the unit keeps VIF1/VU1 and the byte copies. Needs threaded MTVU
    // and the GS worker queue. local/research/VPL1/REPORT.md.
    if (const char *env = std::getenv("PS2X_MTVU_GIF_STAGE"))
    {
        if (std::strcmp(env, "1") == 0)
        {
            const bool ok = ps2_mtvu::threaded() && m_gs.queueEnabled();
            if (ok)
                ps2_mtvu::startGifStage([this](ps2_mtvu::GifOp &op) { m_memory.execGifStageOp(op); });
            std::cerr << "[mtvu] gif-stage " << (ok ? "on" : "refused")
                      << " (PS2X_MTVU_GIF_STAGE=1" << (ok ? "" : "; needs PS2X_MTVU=1 threaded and the GS worker queue")
                      << ")" << std::endl;
        }
    }
    // VPL2: PS2X_MTVU_VIF_STAGE=1 (default off) parses the unit's VIF1
    // streams on their own thread ahead of VU1 execution; UNPACK/MPG writes,
    // MSCAL/MSCNT, GIF submits and MSKPATH3 reach the MTVU thread as an
    // ordered record log. Needs the VPL1 GIF stage. local/research/VPL2/REPORT.md.
    if (const char *env = std::getenv("PS2X_MTVU_VIF_STAGE"))
    {
        if (std::strcmp(env, "1") == 0)
        {
            const bool ok = ps2_mtvu::gifStageOn() && !ps2_mtvu::vifLog().running;
            if (ok)
                ps2_mtvu::startVifStage(&PS2Memory::execVifStageRec, &m_memory);
            std::cerr << "[mtvu] vif-stage " << (ok ? "on" : "refused")
                      << " (PS2X_MTVU_VIF_STAGE=1" << (ok ? "" : "; needs PS2X_MTVU_GIF_STAGE=1 on")
                      << ")" << std::endl;
        }
    }
    {
        std::string microvuError;
        if (!ps2_microvu::configure(ps2_mtvu::threaded(), microvuError))
        {
            std::cerr << "[microvu] " << microvuError << std::endl;
            return false;
        }
        ps2_microvu::adoptData(m_memory); // MP1 L1 (no-op unless the library offers it)
    }
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     if (ps2_mtvu::onWorker())
                                     {
                                         // MT1: on the unit worker, D/T enables come from
                                         // the kick's FBRST snapshot; the D/T rule keeps the
                                         // stop bits 0, so VPU_STAT (EE-owned) is not written.
                                         const uint32_t fbrst = ps2_mtvu::jobFbrst();
                                         m_vu1.state().dBitEnabled = (fbrst & (1u << 10)) != 0u;
                                         m_vu1.state().tBitEnabled = (fbrst & (1u << 11)) != 0u;
                                         if (ps2_microvu::selected())
                                         {
                                             // OM1: a MISS falls through to the static restart below.
                                             if (ps2_microvu::run(m_memory, m_memory.getVU1Data(), m_vu1.state(),
                                                                 startPC, false, top, itop, fbrst, 65536))
                                                 return;
                                         }
                                         m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                       m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                       m_gs, &m_memory, startPC, top, itop, 65536);
                                         return;
                                     }
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     if (ps2_microvu::selected())
                                     {
                                         const uint32_t fbrst = cpuContext->vu0_fbrst;
                                         ps2_mtvu::submit([this, startPC, top, itop, fbrst]
                                                          {
                                                              // OM1: a MISS restarts statically here; the
                                                              // post-sync VPU_STAT update below is shared.
                                                              if (!ps2_microvu::run(m_memory, m_memory.getVU1Data(),
                                                                                    m_vu1.state(), startPC, false,
                                                                                    top, itop, fbrst, 65536))
                                                                  m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                                                m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                                                m_gs, &m_memory, startPC, top, itop, 65536);
                                                          }, 0, fbrst);
                                         ps2_mtvu::syncAll(ps2_mtvu::Reason::DtFallback);
                                         cpuContext->vu0_vpu_stat =
                                             (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                             (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                             (m_vu1.state().stoppedByT ? 0x0400u : 0u);
                                         return;
                                     }
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, top, itop, 65536);
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     if (ps2_mtvu::onWorker())
                                     {
                                         const uint32_t fbrst = ps2_mtvu::jobFbrst(); // MT1: see MSCAL
                                         m_vu1.state().dBitEnabled = (fbrst & (1u << 10)) != 0u;
                                         m_vu1.state().tBitEnabled = (fbrst & (1u << 11)) != 0u;
                                         if (ps2_microvu::selected())
                                         {
                                             // OM1: a MISS falls through to the static restart below.
                                             if (ps2_microvu::run(m_memory, m_memory.getVU1Data(), m_vu1.state(),
                                                                 0, true, top, itop, fbrst, 65536))
                                                 return;
                                         }
                                         m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                      m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                      m_gs, &m_memory, top, itop, 65536);
                                         return;
                                     }
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     if (ps2_microvu::selected())
                                     {
                                         const uint32_t fbrst = cpuContext->vu0_fbrst;
                                         ps2_mtvu::submit([this, top, itop, fbrst]
                                                          {
                                                              // OM1: a MISS restarts statically here; the
                                                              // post-sync VPU_STAT update below is shared.
                                                              if (!ps2_microvu::run(m_memory, m_memory.getVU1Data(),
                                                                                    m_vu1.state(), 0, true,
                                                                                    top, itop, fbrst, 65536))
                                                                  m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                                               m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                                               m_gs, &m_memory, top, itop, 65536);
                                                          }, 0, fbrst);
                                         ps2_mtvu::syncAll(ps2_mtvu::Reason::DtFallback);
                                         cpuContext->vu0_vpu_stat =
                                             (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                             (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                             (m_vu1.state().stoppedByT ? 0x0400u : 0u);
                                         return;
                                     }
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
#if defined(__ANDROID__)
        // VK1 Part 2: with the Vulkan layer and an overlay (virtual pad), the
        // layer goes UNDER the GL window, which must then be translucent: RGBA
        // window format before raylib creates its surface (raylib's EGL config
        // gets alpha from the configure-time patch).
        if (ps2x_present_vk::enabled() && virtualPadWanted())
        {
            if (struct android_app *app = GetAndroidApp(); app && app->activity)
                ANativeActivity_setWindowFormat(app->activity, WINDOW_FORMAT_RGBA_8888);
            ps2x_present_vk::setUnderlay(true);
        }
#endif
#if defined(__ANDROID__)
        // AP1: start raylib at the display size (0 takes the native window
        // size): with 640x448 the GL fallback letterboxes into raylib's
        // canvas and shows a bordered 1544x868 box, while at the window size
        // the same presentRect fills 1920x1080 at 16:9. The vpad layout
        // scales by window height and touch is in screen coordinates, so both
        // follow; the Vulkan child rect is in buffer pixels either way.
        InitWindow(0, 0, title);
        // AP1: the window exists now (raylib waits for INIT_WINDOW); later
        // windows re-apply through the app-command wrapper above.
        ap1HideSystemBars(GetAndroidApp());
        // BG1: register the runtime for the pause hook and install the
        // app-command wrapper now (not on the first present iteration), so a
        // backgrounding during boot still pauses.
        g_bg1Runtime = this;
        if (struct android_app *initApp = GetAndroidApp();
            initApp && initApp->onAppCmd != vk1OnAppCmd)
        {
            g_vk1RaylibOnAppCmd = initApp->onAppCmd;
            initApp->onAppCmd = vk1OnAppCmd;
        }
#else
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
#endif
#if defined(PS2X_IOS)
        ps2x::ios::syncWindowSize();
        SetTraceLogLevel(LOG_ERROR);
#endif
#if defined(PS2X_IOS)
        // IA1: PS2X_IOS_AUDIO_SESSION=playback|ambient|off. playback (default)
        // is IB3's behaviour; ambient mixes with other apps and does not claim
        // the route; off never opens the host audio device (the guest SND
        // model still runs, the PCM ring overflows host-side).
        const char *iosAudioEnv = std::getenv("PS2X_IOS_AUDIO_SESSION");
        const std::string iosAudio = iosAudioEnv ? iosAudioEnv : "playback";
        const bool iosAudioOff = iosAudio == "off";
        std::cerr << "[audio] PS2X_IOS_AUDIO_SESSION=" << (iosAudioOff || iosAudio == "ambient" ? iosAudio : "playback")
                  << '\n';
        if (!iosAudioOff)
#endif
        {
            InitAudioDevice();
#if defined(PS2X_IOS)
            // IB3 (AirPods): miniaudio's iOS default category drops Bluetooth
            // A2DP; Playback restores it (ps2x::ios::setAudioSession).
            ps2x::ios::setAudioSession(iosAudio == "ambient");
#endif
        }
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
        if (const char *sound = std::getenv("PS2X_SOUND"); sound && std::strcmp(sound, "1") == 0 &&
            !ps2_snd_audio_output::initialize())
            std::cerr << "[snd-output] unable to initialize host AudioStream\n";
#endif
#if defined(PS2X_IOS)
        // IQ1: PS2X_DISPLAY_HZ=120 asks for ProMotion and lets raylib's loop
        // run at 120 (EAGL present still paces it to the panel); 60 = as before.
        ps2x::ios::requestDisplayRate(ps2x_present_vk::displayHz());
        // IP6 (PS2X_VSYNC_LOCK=1): raylib's timer limiter and the vsync-
        // throttled swap both pace the loop and beat (iPhone 120: 112
        // presents/s; iPad 60: 59). Under the lock the cap sits 5 % above the
        // panel so the swap alone paces; the cap only binds if it never blocks.
        {
            const int loopHz = ps2x_present_vk::displayHz() == 120 ? 120 : 60;
            SetTargetFPS(ps2_vsync_lock::enabled() ? loopHz * 105 / 100 : loopHz);
        }
#else
        SetTargetFPS(60);
#endif
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
    ps2_savestate::setElfPath(elfPath); // SS1: header ELF pin

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

    // TK2: host course manifest (PS2X_SSX3_COURSE_MANIFEST, default off).
    ps2_ssx3_course::applyFromEnv(m_memory.getRDRAM());

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
// EE1: only the thread-id cell and SetThread stay unconditional (E7 thread
// attribution); the parse/emit/report machinery compiles out with
// PS2X_ENABLE_DIAG_WATCH=0.
namespace
{
    std::atomic<int> g_diagWatchThreadId{-999};
}

void ps2DiagWatchSetThread(int id)
{
    g_diagWatchThreadId.store(id, std::memory_order_relaxed);
}

#if PS2X_ENABLE_DIAG_WATCH
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
#endif // PS2X_ENABLE_DIAG_WATCH (EE1)

#if !PS2X_ENABLE_DIAG_WATCH
// EE1: the Store helpers still name the impl inside folded `if (false)`
// branches; keep a no-op so the TU parses in release builds.
static void diagWatchReportImpl(uint8_t *, uint32_t, uint32_t, uint64_t, uint64_t, const R5900Context *,
                                const PS2Runtime *, uint32_t)
{
}
#endif

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
// HP3 F1: the dispatch history feeds only the missing-function error
// trace; skip the thread_local push in speed builds (taps=0).
#if PS2X_ENABLE_DIAG_TAPS
    pushDispatchPc(address);
#endif

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

    // CU4 B7: the PS2X_DIAG_DRIVER_PROBE tunable is deleted (stale CU1 S3
    // tap). The driver-entry probe never fires.

    // CU4 B7: the PS2X_DIAG_394ED0 tunable is deleted (stale CU1 S3 tap).
    // The 394ED0 park probe never fires.
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
    // TK22: put back the draw-table slot a refused append borrowed.
    if (g_ssx3DrawPending.load(std::memory_order_relaxed) &&
        (sourcePc < kSsx3DrawKeyCall || sourcePc >= kSsx3DrawKeyEnd))
    {
        ssx3DrawTableRestore(rdram);
    }
    // TK38: put back the query-list slots a refused append borrowed.
    if (g_ssx3SpatialPending.load(std::memory_order_relaxed))
    {
        ssx3SpatialRestore(rdram);
    }
    // EE1P2: one product gate per dispatch; rider-pass detail (boundary
    // begin, helper tracking, prediction skip) runs only when a split mode
    // is armed. The g2b/observer calls below fold away in release builds.
    bool splitBoundary = false;
    if (ps2_ts2_split60::enabled())
    {
        splitBoundary = sourcePc == 0x128ddcu && targetPc == 0x1216e0u &&
                        kind == GuestBranchKind::DirectCall;
        if (splitBoundary) ps2_ts2_split60::begin(rdram, ctx);
        // TS3: an unconverted case in half 1 runs nothing for this rider;
        // finishIfContinuation keeps the half 1->0 flip on the last rider.
        if (splitBoundary && ps2_ts2_split60::skipSecondHalfUnconverted())
        {
            ctx->pc = fallthroughPc;
            ps2_ts2_split60::finishIfContinuation(ctx);
            return true;
        }
        ps2_ts2_split60::noteHelperCall(sourcePc, targetPc);
        if (ps2_ts2_split60::skipSecondHalfPrediction(rdram, ctx, sourcePc, targetPc))
        {
            ctx->pc = fallthroughPc;
            return true;
        }
    }
    // TS3: opt-in case/callback census, observation only, stock and split
    // modes. In release builds the gate is a constant false and folds away.
    if (ps2_ts2_split60::caseCountEnabled())
    {
        ps2_ts2_split60::noteTs3Counts(rdram, ctx, sourcePc, targetPc,
            kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall,
            kind == GuestBranchKind::IndirectCall || kind == GuestBranchKind::IndirectJump,
            m_memory.gs().vsyncTick.load());
    }
    // HL1: halfLoad call census. Env-gated (unlike the TS3 census it stays
    // live where TS2_DIAG compiles out).
    if (ps2_ts2_split60::countEnabled())
    {
        ps2_ts2_split60::countTick(m_memory.gs().vsyncTick.load(std::memory_order_relaxed));
    }
    // FH1: full120 manager patch at the init hook + env-only tap counts.
    if ((ps2_fh1::enabled() || ps2_fh1::tapOn()) && ps2_fh1::onBranch(rdram, ctx, sourcePc, targetPc))
    {
        ctx->pc = fallthroughPc;
        return true;
    }
    if (targetPc == kSsx3PatchCacheAlloc &&
        (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall) &&
        ssx3PatchCacheGuard(rdram, ctx))
    {
        ctx->pc = fallthroughPc;
        return true;
    }
    if (targetPc == kSsx3GuestFree && g_ssx3PatchGrow.active &&
        (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall))
    {
        ssx3PatchGrowFree(ctx);
    }
    if (targetPc == kSsx3PatchCacheInit &&
        (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall))
    {
        ssx3PatchGrowInit(ctx);
    }
    if (targetPc == kSsx3DrawKeyCall &&
        (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall))
    {
        ssx3DrawTableGuard(rdram, ctx, sourcePc);
    }
    if (targetPc == kSsx3DrawLookup &&
        (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall) &&
        ssx3DrawPoolLookup(rdram, ctx, sourcePc))
    {
        ctx->pc = fallthroughPc;
        return true;
    }
    if (targetPc == kSsx3DrawTexLookup &&
        (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall))
    {
        ssx3DrawPoolPin(rdram, ctx, sourcePc);
    }
    if ((targetPc == kSsx3SpatialItems12 || targetPc == kSsx3SpatialItems34 ||
         targetPc == kSsx3SpatialInside1 || targetPc == kSsx3SpatialInside3) &&
        (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall) && ssx3SpatialGuardOn())
    {
        ssx3SpatialGuard(rdram, ctx, targetPc, sourcePc);
    }
    if (targetPc == kSsx3DrawReset &&
        (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall))
    {
        ssx3DrawTableFrame(rdram, ctx, m_memory.gs().vsyncTick.load(std::memory_order_relaxed));
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
        ps2_guest_unwind::mark();
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

    // HP3 F2: s_diagCallTick was incremented here but never read
    // anywhere; removed (no observable change in any build).
    // T1: cumulative hot-pc tally. HP3 F3: opt-in behind the cached
    // park flag so speed builds pay one load+branch per dispatch; the
    // snapshot (EeScheduler park fill) reads it only when park is on.
    if (ps2_park::parkEnabled())
    {
        ps2_park::tallyDispatch(targetPc, (ctx != nullptr) ? getRegU32(ctx, 31) : 0u);
    }
    // P1c HLE stub/call histogram at the register_functions.cpp binding
    // lookup, per-period and cleared (the T1 tally above is cumulative
    // for the whole boot). Already gated on the diag period.
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

    // P1ad 394ED0 park probe: CU4 B7 deleted the tunable; the probe never
    // fires. Fresh dispatches only (checkpoint resumes do not re-dispatch).

    // E3b R1/R4 taps (read-only; CU4 B4 deleted the PS2X_E3_INV tunable so
    // ps2_e3::enabled() is statically false). No isCall gate:
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
    // HP3 F4: the E11/E12 card observation + E15 MPEG trace run a Trace
    // ctor/dtor (with a mutex round-trip in the off-state target check)
    // on every dispatch; compile them out of speed builds.
#if PS2X_ENABLE_DIAG_TAPS
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
#endif // PS2X_ENABLE_DIAG_TAPS (HP3 F4)
    targetFn(rdram, ctx, this);
    // FH11: full-120 post-call fixes (armed only by a pre-hook in onBranch).
    if (ps2_fh1::g_postArmed)
    {
        ps2_fh1::onReturn(rdram, ctx, targetPc, !isStopRequested() && ctx->pc != 0u && !ps2_guest_unwind::pending());
    }
#if PS2X_ENABLE_DIAG_TAPS
    mpegTrace.finish(m_memory.gs().vsyncTick.load());
    noteCardCall("mc-return");
#endif // PS2X_ENABLE_DIAG_TAPS (HP3 F4)

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    // PF1: the callee was suspended at a checkpoint, not returned; keep
    // unwinding even when its suspended pc happens to equal its entry.
    if (ps2_guest_unwind::pending())
    {
        return false;
    }

    if (ctx->pc == entryPc)
    {
        ctx->pc = fallthroughPc;
    }

    const bool returned = ctx->pc == fallthroughPc;
    if (splitBoundary && returned) ps2_ts2_split60::finishIfContinuation(ctx);
    return returned;
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

// VR3: dev-only VU0 census (PS2X_VR3_VU0_CENSUS=1, default off; one static
// branch per VU0 start when off). Per (code image, startPC): starts, VU0
// cycles, budget hits; VU0 code-generation changes seen at a start (each one
// rebuilds the decode cache) and whether the image bytes changed; wall time
// split into entry (reset + copy in), run and exit (copy out). Cumulative
// tables on stderr every 600 vsync ticks (FR1-R1 race window = 1800..2400).
// PS2X_VR3_VU0_IMAGE_DUMP=<dir> also writes each distinct 4 KiB code image as
// vu0_<xxh64>.bin (derived from game data: keep it outside the repo).
namespace vr3_vu0_census
{
    bool enabled()
    {
        static const bool on = []
        {
            const char *value = std::getenv("PS2X_VR3_VU0_CENSUS");
            return value != nullptr && value[0] == '1';
        }();
        return on;
    }

    struct ProgRow
    {
        uint64_t calls = 0;
        uint64_t cycles = 0;
        uint64_t maxCycles = 0;
        uint64_t budgetHits = 0;
    };

    struct State
    {
        std::mutex mutex;
        std::map<std::pair<uint64_t, uint32_t>, ProgRow> progs;
        std::map<uint32_t, uint64_t> callers;
        std::map<uint64_t, uint64_t> imageFirstTick;
        uint64_t lastGeneration = ~0ull;
        uint64_t hash = 0;
        uint64_t calls = 0;
        uint64_t vcallmsr = 0;
        uint64_t generationChanges = 0;
        uint64_t imageChanges = 0;
        uint64_t nsEntry = 0;
        uint64_t nsRun = 0;
        uint64_t nsExit = 0;
        uint64_t nextDumpTick = 600;
    };

    State &state()
    {
        static State s;
        return s;
    }

    std::atomic<uint64_t> pendingVcallmsr{0};

    uint64_t nowNs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }

    void note(const uint8_t *vu0Code, uint64_t generation, uint64_t tick, uint32_t startPC,
              uint32_t caller, uint64_t cycles, uint64_t nsEntry, uint64_t nsRun, uint64_t nsExit)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (generation != s.lastGeneration)
        {
            ++s.generationChanges;
            const uint64_t hash = XXH64(vu0Code, PS2_VU0_CODE_SIZE, 0);
            if (hash != s.hash || s.lastGeneration == ~0ull)
                ++s.imageChanges;
            s.hash = hash;
            s.lastGeneration = generation;
            if (s.imageFirstTick.emplace(hash, tick).second)
            {
                static const char *dir = std::getenv("PS2X_VR3_VU0_IMAGE_DUMP");
                if (dir != nullptr && dir[0] != '\0')
                {
                    char name[64];
                    std::snprintf(name, sizeof(name), "/vu0_%016llx.bin",
                                  static_cast<unsigned long long>(hash));
                    std::ofstream out(std::string(dir) + name, std::ios::binary);
                    out.write(reinterpret_cast<const char *>(vu0Code), PS2_VU0_CODE_SIZE);
                }
            }
        }
        ++s.calls;
        s.vcallmsr += pendingVcallmsr.exchange(0, std::memory_order_relaxed);
        ProgRow &row = s.progs[{s.hash, startPC}];
        ++row.calls;
        row.cycles += cycles;
        row.maxCycles = std::max(row.maxCycles, cycles);
        row.budgetHits += cycles >= 4096u ? 1u : 0u;
        ++s.callers[caller];
        s.nsEntry += nsEntry;
        s.nsRun += nsRun;
        s.nsExit += nsExit;
        if (tick < s.nextDumpTick)
            return;
        s.nextDumpTick = (tick / 600u + 1u) * 600u;
        uint64_t cyclesTotal = 0, budgetTotal = 0;
        for (const auto &entry : s.progs)
        {
            cyclesTotal += entry.second.cycles;
            budgetTotal += entry.second.budgetHits;
        }
        std::fprintf(stderr, "[vr3-vu0] tick=%llu calls=%llu vcallmsr=%llu gen_changes=%llu image_changes=%llu images=%zu progs=%zu cycles=%llu budget_hits=%llu ns_entry=%llu ns_run=%llu ns_exit=%llu\n",
                     static_cast<unsigned long long>(tick), static_cast<unsigned long long>(s.calls),
                     static_cast<unsigned long long>(s.vcallmsr),
                     static_cast<unsigned long long>(s.generationChanges),
                     static_cast<unsigned long long>(s.imageChanges), s.imageFirstTick.size(),
                     s.progs.size(), static_cast<unsigned long long>(cyclesTotal),
                     static_cast<unsigned long long>(budgetTotal),
                     static_cast<unsigned long long>(s.nsEntry), static_cast<unsigned long long>(s.nsRun),
                     static_cast<unsigned long long>(s.nsExit));
        for (const auto &entry : s.imageFirstTick)
            std::fprintf(stderr, "[vr3-vu0-image] tick=%llu hash=%016llx first_tick=%llu\n",
                         static_cast<unsigned long long>(tick),
                         static_cast<unsigned long long>(entry.first),
                         static_cast<unsigned long long>(entry.second));
        for (const auto &entry : s.progs)
            std::fprintf(stderr, "[vr3-vu0-prog] tick=%llu hash=%016llx start=0x%x calls=%llu cycles=%llu max=%llu budget_hits=%llu\n",
                         static_cast<unsigned long long>(tick),
                         static_cast<unsigned long long>(entry.first.first), entry.first.second,
                         static_cast<unsigned long long>(entry.second.calls),
                         static_cast<unsigned long long>(entry.second.cycles),
                         static_cast<unsigned long long>(entry.second.maxCycles),
                         static_cast<unsigned long long>(entry.second.budgetHits));
        for (const auto &entry : s.callers)
            std::fprintf(stderr, "[vr3-vu0-caller] tick=%llu ra=0x%x calls=%llu\n",
                         static_cast<unsigned long long>(tick), entry.first,
                         static_cast<unsigned long long>(entry.second));
    }
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

    const bool census = vr3_vu0_census::enabled();
    const uint64_t censusT0 = census ? vr3_vu0_census::nowNs() : 0u;
    // VR3: copyVu0ContextToState rewrites all of m_state and execute() resets
    // the scheduler, so only reset()'s cycle/count part is live here.
    m_vu0.resetForVu0Start();
    copyVu0ContextToState(ctx, m_vu0.state());
    const uint64_t censusT1 = census ? vr3_vu0_census::nowNs() : 0u;
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, 4096);
    const uint64_t censusT2 = census ? vr3_vu0_census::nowNs() : 0u;
    copyVu0StateToContext(m_vu0.state(), ctx);
    if (census)
    {
        const uint64_t censusT3 = vr3_vu0_census::nowNs();
        vr3_vu0_census::note(vu0Code, m_memory.getVU0CodeGeneration(), m_memory.gs().vsyncTick.load(),
                             startPC, getRegU32(ctx, 31), m_vu0.state().cycles,
                             censusT1 - censusT0, censusT2 - censusT1, censusT3 - censusT2);
    }
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
    // reset by resetForVu0Start() above, so state().cycles is this call's usage.
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
    if (vr3_vu0_census::enabled())
        vr3_vu0_census::pendingVcallmsr.fetch_add(1, std::memory_order_relaxed);
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::vu1StartMicroProgramFromEe(R5900Context *ctx, uint32_t cmsar1)
{
    // PCSX2 vu1ExecMicro(addr): TPC = addr (instruction index), run from TPC*8
    // with the VIF1 TOP/ITOP the VU sees through XTOP/XITOP.
    const uint32_t startPC = (cmsar1 & 0x7FFu) << 3;
    ps2_mtvu::sync(ps2_mtvu::Reason::Cmsar1); // MT1: runs inline on the EE
    VIFRegisters &vif1 = m_memory.vif1_regs;
    if (ps2_microvu::selected())
    {
        const uint32_t top = vif1.top, itop = vif1.itop, fbrst = ctx->vu0_fbrst;
        ps2_mtvu::submit([this, startPC, top, itop, fbrst]
                         {
                             // OM1: a MISS restarts statically here (D/T enables
                             // set as in the static branch below, which this
                             // microvu branch skips); the post-sync VPU_STAT
                             // update below is shared.
                             if (!ps2_microvu::run(m_memory, m_memory.getVU1Data(), m_vu1.state(),
                                                   startPC, false, top, itop, fbrst, 65536)) {
                                 m_vu1.state().dBitEnabled = (fbrst & (1u << 10)) != 0u;
                                 m_vu1.state().tBitEnabled = (fbrst & (1u << 11)) != 0u;
                                 m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                               m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                               m_gs, &m_memory, startPC, top, itop, 65536);
                             }
                         }, 0, fbrst);
        ps2_mtvu::syncAll(ps2_mtvu::Reason::Cmsar1);
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & ~0x0600u) |
                            (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                            (m_vu1.state().stoppedByT ? 0x0400u : 0u);
        return;
    }
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
    // GE3 Part 3: guest CSR address (same physical-mask form as
    // privReadReason). In EE-owned FINISH mode these loads never retire.
    inline bool ge3IsCsrReg(uint32_t vaddr)
    {
        return ((vaddr & 0x1FFFFFFFu) & ~7u) == (PS2_GS_PRIV_REG_BASE + 0x1000u);
    }
    // GE3 Part 5b (GT1): bounded one-shot log of guest PCs taking the
    // narrowed FINISH-only exemption (≤32 unique PCs; observer only, never
    // guest state). Lock-free fast path: relaxed scan of the recorded set;
    // the mutex runs only when appending a genuinely new PC.
    inline void ge3NoteFinishOnlyExempt(uint32_t pc)
    {
        static std::atomic<uint32_t> pcs[32]{};
        static std::atomic<uint32_t> count{0u};
        static std::mutex mutex;
        uint32_t n = count.load(std::memory_order_relaxed);
        for (uint32_t i = 0u; i < n && i < 32u; ++i)
        {
            if (pcs[i].load(std::memory_order_relaxed) == pc)
                return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        n = count.load(std::memory_order_relaxed);
        for (uint32_t i = 0u; i < n && i < 32u; ++i)
        {
            if (pcs[i].load(std::memory_order_relaxed) == pc)
                return;
        }
        if (n < 32u)
        {
            pcs[n].store(pc, std::memory_order_relaxed);
            count.store(n + 1u, std::memory_order_relaxed);
            std::fprintf(stderr, "[gs:finish-only] exempt pc=0x%08x\n", pc);
        }
    }
    // GE3 Part 5b (GT1): direct-mapped cache of the finish-only decision per
    // (pc, vaddr, bytes), so the consumer decode isn't re-run per priv load.
    // Single-word entries ((ukey << 1) | decision, 0 = invalid) make relaxed
    // access tear-free; a miss or collision only costs a recompute, never
    // correctness. Self-modifying the consumer is vanishingly rare and det
    // validation below covers the shipped tree.
    inline bool ge3FinishOnlyCached(const uint8_t *rdram, uint32_t pc, uint32_t vaddr, uint32_t bytes)
    {
        static constexpr uint32_t kBits = 6u;
        static constexpr uint32_t kSize = 1u << kBits;
        static std::atomic<uint64_t> cache[kSize]{};
        const uint64_t ukey = (static_cast<uint64_t>(pc) << 32) | (vaddr ^ bytes);
        const uint32_t idx = (pc ^ (vaddr >> 3) ^ bytes) & (kSize - 1u);
        const uint64_t hit = cache[idx].load(std::memory_order_relaxed);
        if (hit != 0u && (hit >> 1) == ukey)
            return (hit & 1u) != 0u;
        const bool decision = ps2_mtvu::privReadFinishOnly(rdram, pc, vaddr, bytes);
        cache[idx].store((ukey << 1) | (decision ? 1u : 0u), std::memory_order_relaxed);
        return decision;
    }
    // GE3 Part 6: episode state for the declarations in ps2_mtvu.h.
    static std::atomic<uint32_t> s_epClearPc{0u};
    static std::atomic<uint32_t> s_epSetter{0u};
    static std::atomic<uint32_t> s_epStreak{0u};
    static std::atomic<uint32_t> s_epLogged{0u};

    // GE3 Part 4: narrowed exemption — CSR read whose consuming mask provably
    // observes only FINISH. Logs the PC once, then frees the read.
    inline bool ge3FinishOnlyFree(const uint8_t *rdram, uint32_t pc, uint32_t vaddr, uint32_t bytes)
    {
        if (!ps2_mtvu::active() || !ge3IsCsrReg(vaddr))
            return false;
        if (!ge3FinishOnlyCached(rdram, pc, vaddr, bytes))
            return false;
        ge3NoteFinishOnlyExempt(pc);
        return true;
    }
}

// GE3 Part 6: definitions for the declarations in ps2_mtvu.h. Observer only;
// the capped log is the confirm counter for the diag legs. The episode state
// above is TU-local; these qualified definitions see it (same TU).
void ps2_mtvu::ge3EpOnClear(uint32_t clearPc)
{
    s_epClearPc.store(clearPc, std::memory_order_relaxed);
    s_epSetter.store(0u, std::memory_order_relaxed);
    s_epStreak.store(0u, std::memory_order_relaxed);
}
void ps2_mtvu::ge3EpOnSet(uint32_t kind)
{
    if (s_epClearPc.load(std::memory_order_relaxed) == 0u)
        return;
    uint32_t expected = 0u;
    s_epSetter.compare_exchange_strong(expected, kind, std::memory_order_relaxed);
}
void ps2_mtvu::ge3EpOnRead(bool finishSet)
{
    if (s_epClearPc.load(std::memory_order_relaxed) == 0u)
        return;
    if (!finishSet)
    {
        s_epStreak.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    const uint32_t pc = s_epClearPc.exchange(0u, std::memory_order_relaxed);
    if (pc == 0u)
        return;
    if (s_epLogged.fetch_add(1u, std::memory_order_relaxed) < 16u)
    {
        std::fprintf(stderr, "[gs:finish-probe] clearpc=0x%08x setter=%u iters=%u\n", pc,
                     s_epSetter.load(std::memory_order_relaxed),
                     s_epStreak.load(std::memory_order_relaxed));
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
            // MT1: sync before the CSR drain. R1 (threaded): a CSR load masked
            // away from the unit's bits neither waits nor drains.
            const ps2_mtvu::Reason mtvuReason = ps2_mtvu::active()
                ? ps2_mtvu::privReadReason(rdram, ctx ? ctx->pc : 0u, vaddr, 1u)
                : ps2_mtvu::Reason::GsPrivRead;
            const bool mtvuFree = mtvuReason == ps2_mtvu::Reason::GsPrivReadMasked && ps2_mtvu::threaded();
            // GE3 Part 4: narrowed to reads whose consuming mask provably
            // observes only FINISH (probe polls qualify; SIGNAL-touching
            // reads keep retiring).
            const bool csrFree = m_memory.finishTimingPcsx2() &&
                                 ge3FinishOnlyFree(rdram, ctx ? ctx->pc : 0u, vaddr, 1u);
            const ps2_mtvu::ExemptScope mtvuExempt(mtvuFree || csrFree);
            if (!mtvuFree && !csrFree)
            {
                ps2_mtvu::sync(mtvuReason, ctx ? ctx->pc : 0u);
                if (m_gs.queueEnabled())
                    m_gs.drainQueue();
            }
            uint8_t value8 = m_memory.read8(vaddr);
            // GE3 Part 6: a FINISH-only read that finds FINISH clear drains
            // the MTVU unit queue only (never GS worker/backend), then
            // re-reads; a set bit returns immediately.
            if (csrFree && (vaddr & 7u) == 0u)
            {
                if ((value8 & 0x2u) == 0u)
                {
                    ps2_mtvu::sync(ps2_mtvu::Reason::FinishPoll, ctx ? ctx->pc : 0u);
                    value8 = m_memory.read8(vaddr);
                }
                ps2_mtvu::ge3EpOnRead((value8 & 0x2u) != 0u);
            }
            return value8;
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
            // MT1: sync before the CSR drain. R1 (threaded): a CSR load masked
            // away from the unit's bits neither waits nor drains.
            const ps2_mtvu::Reason mtvuReason = ps2_mtvu::active()
                ? ps2_mtvu::privReadReason(rdram, ctx ? ctx->pc : 0u, vaddr, 2u)
                : ps2_mtvu::Reason::GsPrivRead;
            const bool mtvuFree = mtvuReason == ps2_mtvu::Reason::GsPrivReadMasked && ps2_mtvu::threaded();
            // GE3 Part 4: narrowed to reads whose consuming mask provably
            // observes only FINISH (probe polls qualify; SIGNAL-touching
            // reads keep retiring).
            const bool csrFree = m_memory.finishTimingPcsx2() &&
                                 ge3FinishOnlyFree(rdram, ctx ? ctx->pc : 0u, vaddr, 2u);
            const ps2_mtvu::ExemptScope mtvuExempt(mtvuFree || csrFree);
            if (!mtvuFree && !csrFree)
            {
                ps2_mtvu::sync(mtvuReason, ctx ? ctx->pc : 0u);
                if (m_gs.queueEnabled())
                    m_gs.drainQueue();
            }
            uint16_t value16 = m_memory.read16(vaddr);
            // GE3 Part 6: FINISH-only clear → unit-queue drain + re-read (see Load8).
            if (csrFree && (vaddr & 7u) == 0u)
            {
                if ((value16 & 0x2u) == 0u)
                {
                    ps2_mtvu::sync(ps2_mtvu::Reason::FinishPoll, ctx ? ctx->pc : 0u);
                    value16 = m_memory.read16(vaddr);
                }
                ps2_mtvu::ge3EpOnRead((value16 & 0x2u) != 0u);
            }
            return value16;
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
            // MT1: sync before the CSR drain. R1 (threaded): a CSR load masked
            // away from the unit's bits neither waits nor drains.
            const ps2_mtvu::Reason mtvuReason = ps2_mtvu::active()
                ? ps2_mtvu::privReadReason(rdram, ctx ? ctx->pc : 0u, vaddr, 4u)
                : ps2_mtvu::Reason::GsPrivRead;
            const bool mtvuFree = mtvuReason == ps2_mtvu::Reason::GsPrivReadMasked && ps2_mtvu::threaded();
            // GE3 Part 4: narrowed to reads whose consuming mask provably
            // observes only FINISH (probe polls qualify; SIGNAL-touching
            // reads keep retiring).
            const bool csrFree = m_memory.finishTimingPcsx2() &&
                                 ge3FinishOnlyFree(rdram, ctx ? ctx->pc : 0u, vaddr, 4u);
            const ps2_mtvu::ExemptScope mtvuExempt(mtvuFree || csrFree);
            if (!mtvuFree && !csrFree)
            {
                ps2_mtvu::sync(mtvuReason, ctx ? ctx->pc : 0u);
                if (m_gs.queueEnabled())
                    m_gs.drainQueue();
            }
            uint32_t value = m_memory.read32(vaddr);
            // GE3 Part 6: FINISH-only clear → unit-queue drain + re-read (see Load8).
            if (csrFree && (vaddr & 7u) == 0u)
            {
                if ((value & 0x2u) == 0u)
                {
                    ps2_mtvu::sync(ps2_mtvu::Reason::FinishPoll, ctx ? ctx->pc : 0u);
                    value = m_memory.read32(vaddr);
                }
                ps2_mtvu::ge3EpOnRead((value & 0x2u) != 0u);
            }
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
            // MT1: sync before the CSR drain. R1 (threaded): a CSR load masked
            // away from the unit's bits neither waits nor drains.
            const ps2_mtvu::Reason mtvuReason = ps2_mtvu::active()
                ? ps2_mtvu::privReadReason(rdram, ctx ? ctx->pc : 0u, vaddr, 8u)
                : ps2_mtvu::Reason::GsPrivRead;
            const bool mtvuFree = mtvuReason == ps2_mtvu::Reason::GsPrivReadMasked && ps2_mtvu::threaded();
            // GE3 Part 4: narrowed to reads whose consuming mask provably
            // observes only FINISH (probe polls qualify; SIGNAL-touching
            // reads keep retiring).
            const bool csrFree = m_memory.finishTimingPcsx2() &&
                                 ge3FinishOnlyFree(rdram, ctx ? ctx->pc : 0u, vaddr, 8u);
            const ps2_mtvu::ExemptScope mtvuExempt(mtvuFree || csrFree);
            if (!mtvuFree && !csrFree)
            {
                ps2_mtvu::sync(mtvuReason, ctx ? ctx->pc : 0u);
                if (m_gs.queueEnabled())
                    m_gs.drainQueue();
            }
            uint64_t value = m_memory.read64(vaddr);
            // GE3 Part 6: FINISH-only clear → unit-queue drain + re-read (see Load8).
            if (csrFree)
            {
                if ((value & 0x2u) == 0u)
                {
                    ps2_mtvu::sync(ps2_mtvu::Reason::FinishPoll, ctx ? ctx->pc : 0u);
                    value = m_memory.read64(vaddr);
                }
                ps2_mtvu::ge3EpOnRead((value & 0x2u) != 0u);
            }
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
            // MT1: sync before the CSR drain. R1 (threaded): a CSR load masked
            // away from the unit's bits neither waits nor drains.
            const ps2_mtvu::Reason mtvuReason = ps2_mtvu::active()
                ? ps2_mtvu::privReadReason(rdram, ctx ? ctx->pc : 0u, vaddr, 16u)
                : ps2_mtvu::Reason::GsPrivRead;
            const bool mtvuFree = mtvuReason == ps2_mtvu::Reason::GsPrivReadMasked && ps2_mtvu::threaded();
            // GE3 Part 4: narrowed to reads whose consuming mask provably
            // observes only FINISH (probe polls qualify; SIGNAL-touching
            // reads keep retiring).
            const bool csrFree = m_memory.finishTimingPcsx2() &&
                                 ge3FinishOnlyFree(rdram, ctx ? ctx->pc : 0u, vaddr, 16u);
            const ps2_mtvu::ExemptScope mtvuExempt(mtvuFree || csrFree);
            if (!mtvuFree && !csrFree)
            {
                ps2_mtvu::sync(mtvuReason, ctx ? ctx->pc : 0u);
                if (m_gs.queueEnabled())
                    m_gs.drainQueue();
            }
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
        m_memory.write8(vaddr, value);
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
        m_memory.write16(vaddr, value);
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
        m_memory.write32(vaddr, value, ctx ? ctx->pc : 0u);
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
        m_memory.write64(vaddr, value, ctx ? ctx->pc : 0u);
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
        m_memory.write128(vaddr, value);
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
    ps2_mtvu::sync(ps2_mtvu::Reason::NativeGif); // MT1: native GIF paths drive the GS directly

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
    // EE1P2: one product gate per checkpoint; the restart suppressor runs
    // only in split120 halves.
    if (ps2_ts2_split60::halfMode() && ps2_ts2_split60::consumeRestartCheckpoint()) return false;
    if (!m_eeScheduler->checkpointDue(cycles))
    {
        return false;
    }
    ps2_guest_unwind::mark();
    return true;
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
        // AD1: bind this thread's TID to its ADPF hint session (no-op unless
        // PS2X_ADPF=1; Android only).
        ps2x::adpf::noteThread(ps2x::adpf::Thread::Game);
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
            // SS1 (DEV, default off): PS2X_SAVESTATE_LOAD restores a saved
            // machine over the freshly initialized one, then run() resumes it.
            bool savestateOk = true;
            if (const std::string &ssLoad = ps2_savestate::config().loadPath; !ssLoad.empty())
            {
                std::string ssError;
                savestateOk = ps2_savestate::load(*this, ssLoad, ssError);
                if (!savestateOk)
                    std::fprintf(stderr, "[savestate] load refused: %s\n", ssError.c_str());
            }
            if (savestateOk)
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
    // GE1 M: a one-second receipt cadence reaches the screen validity gate
    // at t3000 even when guest time advances at 120 vsyncs/s.
    const double vsyncRatePeriod = [] {
        const char *env = std::getenv("PS2X_VSYNC_RATE_INTERVAL_S");
        return env && std::strcmp(env, "1") == 0 ? 1.0 : 5.0;
    }();
    // IP3: PS2X_PERF_LOG=1 appends one line per wall second to the perf log
    // (off = one bool check per frame, zero cost).
    const bool perfLog = ps2x::perflog::enabled();
    const bool vpadWanted = virtualPadWanted();
    PSChordState ds1Chord; // SELECT+L3 save / SELECT+R3 load, carried across frames
    std::vector<Ds1Hotkey> ds1Hotkeys =
        parseDs1Hotkeys(std::getenv("PS2X_SAVESTATE_HOTKEY_AT")); // DS1 DEV-ONLY scheduled chord
    // DS1: hash the runner for save/load identity on a background thread
    // while the game boots (a ~200 MB pass; the first quick-save would
    // otherwise hitch on it). The cache mutex guards the result.
    std::thread([] { ps2_savestate::warmRunnerSha(); }).detach();
    bool vpadLastPadConnected = true; // forces the first [vpad] line when the overlay shows
    const std::vector<ps2x::vpad::TestTouch> vpadTestTouches =
        ps2x::vpad::parseTestTouches(std::getenv("PS2X_VPAD_TEST_TOUCHES")); // DEV-ONLY
    const std::vector<ps2x::vpad::TestTap> vpadTestTaps =
        ps2x::vpad::parseTestTap(std::getenv("PS2X_VPAD_TEST_TAP")); // DEV-ONLY
    ps2x::vpad::PadState vpadPad; // VT1: touch ownership + stick anchor, carried across frames
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
#if defined(__ANDROID__)
        // BG1: backgrounded (or pre-focus pause): no guest, no presents, no
        // swapping. PollInputEvents blocks in the looper once raylib is
        // disabled (backgrounded); while still enabled, sleep so a pause
        // without focus loss doesn't spin on an unchanged frame.
        if (ps2x::androidPause::paused())
        {
            PollInputEvents(); // delivers APP_CMD_RESUME / DESTROY
            if (WindowShouldClose())
            {
                requestStop();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
#endif
        if (vsyncRateLog)
        {
            const auto now = std::chrono::steady_clock::now();
            const double secs = std::chrono::duration<double>(now - vsyncRateWall).count();
            if (secs >= vsyncRatePeriod)
            {
                const uint64_t vt = m_memory.gs().vsyncTick.load();
                const double rate = static_cast<double>(vt - vsyncRateTick) / secs;
                // FH1: full120 VBlanks are half-periods; report stock-equivalent vs/s.
                // FH5 events: stock-time accumulator delta (half-periods / 2).
                static uint64_t s_lastHalf = ps2_fh1::stockHalfTicks();
                const uint64_t half = ps2_fh1::stockHalfTicks();
                const double stockRate = ps2_fh1::eventsMode()
                                             ? static_cast<double>(half - s_lastHalf) / 2.0 / secs
                                             : rate / ps2_fh1::vblankDivisor();
                s_lastHalf = half;
                std::fprintf(stderr, "[vsync-rate] tick=%llu rate=%.2f/s (%.3fx of 59.94)%s\n",
                             static_cast<unsigned long long>(vt), stockRate, stockRate / 59.94,
                             ps2_fh1::eventsMode() ? (ps2_fh1::g_commitActive ? " full120-events:on" : " full120-events:off")
                                                     : ps2_fh1::enabled() ? " full120" : "");
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
        if (perfLog)
            ps2x::perflog::poll(m_memory.gs().vsyncTick.load());
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
#if defined(__ANDROID__)
        // AP1: the app-command wrapper is installed on the GL path too (it
        // re-applies immersive mode; the VK calls inside are no-ops there).
        if (struct android_app *wrapApp = GetAndroidApp();
            wrapApp && wrapApp->onAppCmd != vk1OnAppCmd)
        {
            g_vk1RaylibOnAppCmd = wrapApp->onAppCmd;
            wrapApp->onAppCmd = vk1OnAppCmd;
        }
        // AP1 Part 2: bounded retry until immersive applies (once a second
        // for 10 s per window, e.g. while the DecorView attaches); window
        // and focus changes re-apply through the wrapper above.
        {
            static uint32_t s_ap1Gen = 0u;
            static int s_ap1Tries = 0;
            static bool s_ap1Done = false;
            static std::chrono::steady_clock::time_point s_ap1Last{};
            const uint32_t gen = ps2x_present_vk::windowGeneration();
            if (gen != s_ap1Gen)
            {
                s_ap1Gen = gen;
                s_ap1Tries = 0;
                s_ap1Done = false;
            }
            if (!s_ap1Done && s_ap1Tries < 10)
            {
                const auto now = std::chrono::steady_clock::now();
                if (s_ap1Tries == 0 || now - s_ap1Last >= std::chrono::seconds(1))
                {
                    s_ap1Last = now;
                    ++s_ap1Tries;
                    s_ap1Done = ap1HideSystemBars(GetAndroidApp());
                }
            }
        }
        if (ps2x_present_vk::enabled())
        {
            struct android_app *app = GetAndroidApp();
            EGLint bufW = 0, bufH = 0; // raylib's window buffers (e.g. 796x448 scaled to 1920x1080)
            const EGLDisplay dpy = eglGetCurrentDisplay();
            const EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
            if (dpy != EGL_NO_DISPLAY && surf != EGL_NO_SURFACE)
            {
                eglQuerySurface(dpy, surf, EGL_WIDTH, &bufW);
                eglQuerySurface(dpy, surf, EGL_HEIGHT, &bufH);
            }
            ps2x_present_vk::setHostWindow(app ? app->window : nullptr, app ? app->activity : nullptr,
                                           static_cast<int>(presentAspect), bufW, bufH);
        }
#endif
        UploadFrame(frameTex, this, presentWidth, presentHeight);

#if defined(PS2X_IOS)
        ps2x::ios::syncWindowSize();
        {
            const int iosScreenW = GetScreenWidth();
            const int iosScreenH = GetScreenHeight();
            const int iosRenderW = GetRenderWidth();
            const int iosRenderH = GetRenderHeight();
            static bool s_iosRenderLogged = false;
            static int s_lastIosScreenW = 0;
            static int s_lastIosScreenH = 0;
            static int s_lastIosRenderW = 0;
            static int s_lastIosRenderH = 0;
            if (!s_iosRenderLogged || iosScreenW != s_lastIosScreenW || iosScreenH != s_lastIosScreenH ||
                iosRenderW != s_lastIosRenderW || iosRenderH != s_lastIosRenderH)
            {
                std::fprintf(stderr, "[ios-render] screen=%dx%d render=%dx%d\n",
                             iosScreenW, iosScreenH, iosRenderW, iosRenderH);
                s_iosRenderLogged = true;
                s_lastIosScreenW = iosScreenW;
                s_lastIosScreenH = iosScreenH;
                s_lastIosRenderW = iosRenderW;
                s_lastIosRenderH = iosRenderH;
            }
        }
#endif
#if defined(__ANDROID__)
        // VK1 Part 2: once the Vulkan layer shows the game above the whole GL
        // window and nothing is drawn over it, skip raylib's clear/swap (the
        // window is invisible); input polling and the pad latch below still run.
        const bool vkLive = ps2x_present_vk::active() && ps2x_present_vk::layerLive();
        const bool vkUnder = vkLive && ps2x_present_vk::underlay();
        static uint32_t s_glWindowGen = ~0u;
        static uint32_t s_glSwapsOnWindow = 0u;
        if (const uint32_t gen = ps2x_present_vk::windowGeneration(); gen != s_glWindowGen)
        {
            s_glWindowGen = gen;
            s_glSwapsOnWindow = 0u;
        }
        // Keep swapping until the window has GL buffers (its scaling carries the child).
        const bool skipGl = vkLive && !vkUnder && !m_debugUiInitialized && s_glSwapsOnWindow >= 3u;
#else
        const bool vkUnder = false;
        const bool skipGl = false;
#endif
        if (!skipGl)
        {
            BeginDrawing();
            ClearBackground(BLACK);
#if defined(__ANDROID__)
            // Under-layer: the game shows through a transparent hole in the GL
            // window, exactly over the child's rect; the bars stay opaque black.
            int gl = 0, gt = 0, gr = 0, gb = 0;
            EGLint surfH = 0;
            if (vkUnder && ps2x_present_vk::gameRect(gl, gt, gr, gb) &&
                eglQuerySurface(eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW), EGL_HEIGHT, &surfH) &&
                surfH > 0)
            {
                // The rect is in the window's buffer pixels, top-left origin; GL's
                // scissor is bottom-left. raylib has nothing batched yet here.
                glEnable(GL_SCISSOR_TEST);
                glScissor(gl, surfH - gb, gr - gl, gb - gt);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glDisable(GL_SCISSOR_TEST);
            }
#endif
        }
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
#if defined(PS2X_IOS)
        if (g_hr1ShareTex.id != 0u)
        {
            // HR1 spike: draw the shared texture opaque (PS2 alpha 0x80 would
            // halve it, DK1); GLES2 has no swizzle, so blend off for this quad.
            rlDrawRenderBatchActive();
            rlDisableColorBlend();
            // IX1: a display-sized export (PS2X_GE1_EXPORT_SIZE=display) is the
            // game rect in drawable pixels, give or take rounding; draw it 1:1
            // on whole pixels (no resample) instead of rescaling by a fraction.
            Rectangle shareDst = dstRect;
            if (dpiScale > 0.0f && std::fabs(pr.w * dpiScale - srcWidth) <= 2.0f &&
                std::fabs(pr.h * dpiScale - srcHeight) <= 2.0f)
            {
                const float renderW = screenWidth * dpiScale, renderH = screenHeight * dpiScale;
                shareDst = Rectangle{std::floor((renderW - srcWidth) * 0.5f) / dpiScale,
                                     std::floor((renderH - srcHeight) * 0.5f) / dpiScale, srcWidth / dpiScale,
                                     srcHeight / dpiScale};
            }
            DrawTexturePro(g_hr1ShareTex, srcRect, shareDst, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            rlDrawRenderBatchActive();
            rlEnableColorBlend();
        }
        else
#endif
#if defined(__ANDROID__)
        if (!ps2x_present_vk::active()) // VK1: the game is on the SurfaceControl layer above
#endif
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
        const uint16_t ds1RawPressed = ps2xSampleRaylibPad().pressed;
        uint16_t raylibPressed = padLatchOn ? ds1RawPressed : 0u;
        // DS1: quick-save/load chord, edge-triggered (the shell action runs
        // even when the latch is off). The chord is stripped from the guest
        // mask while engaged, so the game never sees it. Inert without a
        // physical gamepad (det boots unaffected: no buttons, no edges).
        {
            bool ds1SaveEdge = false, ds1LoadEdge = false;
            psChordStep(ds1Chord, (ds1RawPressed & ps2x::vpad::kSelect) != 0u,
                        (ds1RawPressed & ps2x::vpad::kL3) != 0u,
                        (ds1RawPressed & ps2x::vpad::kR3) != 0u, ds1SaveEdge, ds1LoadEdge);
            if (ds1SaveEdge)
                ds1FireQuickSave();
            if (ds1LoadEdge)
                ds1FireQuickLoad();
            if ((ds1RawPressed & ps2x::vpad::kSelect) != 0u &&
                (ds1RawPressed & (ps2x::vpad::kL3 | ps2x::vpad::kR3)) != 0u)
                raylibPressed = static_cast<uint16_t>(
                    raylibPressed & ~(ps2x::vpad::kSelect | ps2x::vpad::kL3 | ps2x::vpad::kR3));
        }
        // DS1 DEV-ONLY scheduled chord (same shell path as the gamepad).
        for (Ds1Hotkey &ds1Hk : ds1Hotkeys)
        {
            if (!ds1Hk.fired && m_memory.gs().vsyncTick.load() >= ds1Hk.tick)
            {
                ds1Hk.fired = true;
                if (ds1Hk.save)
                    ds1FireQuickSave();
                else
                    ds1FireQuickLoad();
            }
        }
        if (vpadWanted && !vpadPadConnected)
        {
            const ps2x::vpad::Layout layout = ps2x::vpad::makeLayout(screenWidth, screenHeight);
            ps2x::vpad::TouchPoint vpadTouches[8];
            int touches = virtualPadTouches(vpadTouches, 8, screenWidth, screenHeight);
            touches = ps2x::vpad::activeTestTouchesWithIds(vpadTestTouches, m_memory.gs().vsyncTick.load(), screenWidth,
                                                           screenHeight, vpadTouches, touches, 8);
            ps2x::vpad::PadFrame vpadFrame = ps2x::vpad::updatePad(vpadPad, layout, vpadTouches, touches);
            uint16_t pressed = vpadFrame.pressed;
            pressed = static_cast<uint16_t>(pressed | ps2x::vpad::activeTestTap(vpadTestTaps, ps2x::padlatch::wallMs()));
            if (padLatchOn)
                ps2x::padlatch::sharedLatch().publish(static_cast<uint16_t>(pressed | raylibPressed));
            else
                ps2x::vpad::liveMask().store(pressed, std::memory_order_relaxed);
            ps2x::vpad::StickVec stick = vpadFrame.stick;
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
#if defined(__ANDROID__)
            if (vkUnder)
            {
                // Premultiplied over a transparent window: rgb = src*a + dst*(1-a),
                // alpha = a + dst_a*(1-a) (plain BLEND_ALPHA would leave alpha = a^2).
                rlSetBlendFactorsSeparate(0x0302 /*SRC_ALPHA*/, 0x0303 /*ONE_MINUS_SRC_ALPHA*/, 1 /*ONE*/,
                                          0x0303, 0x8006 /*FUNC_ADD*/, 0x8006);
                BeginBlendMode(BLEND_CUSTOM_SEPARATE);
            }
#endif
            drawVirtualPad(layout, pressed, stick);
#if defined(__ANDROID__)
            if (vkUnder)
                EndBlendMode();
#endif
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
        // DS1: quick-save/load status line, drawn by the host over the
        // presented frame (never into the guest frame): "saving..." while a
        // save waits, else the last result for 4 s. Skipped with the rest of
        // GL when the VK layer presents above (the play config); there the
        // logcat lines and the slot sidecar are the record.
        if (!skipGl)
        {
            std::string ds1Msg;
            if (ps2_savestate::quickSavePending())
            {
                ds1Msg = "saving...";
            }
            else
            {
                uint64_t ds1AgeMs = 0u;
                if (!ps2_savestate::quickStatus(ds1Msg, ds1AgeMs) || ds1AgeMs >= 4000u)
                    ds1Msg.clear();
            }
            if (!ds1Msg.empty())
            {
                if (ds1Msg.size() > 100u)
                    ds1Msg.resize(100u);
                const int ds1Font = std::max(16, static_cast<int>(screenHeight / 36.0f));
                const int ds1Tw = MeasureText(ds1Msg.c_str(), ds1Font);
                const int ds1X = static_cast<int>(screenWidth / 2.0f) - ds1Tw / 2;
                const int ds1Y = static_cast<int>(screenHeight * 0.06f);
#if defined(__ANDROID__)
                if (vkUnder)
                {
                    rlSetBlendFactorsSeparate(0x0302 /*SRC_ALPHA*/, 0x0303 /*ONE_MINUS_SRC_ALPHA*/,
                                              1 /*ONE*/, 0x0303, 0x8006 /*FUNC_ADD*/, 0x8006);
                    BeginBlendMode(BLEND_CUSTOM_SEPARATE);
                }
#endif
                DrawRectangle(ds1X - 12, ds1Y - 8, ds1Tw + 24, ds1Font + 16, Color{0, 0, 0, 160});
                DrawText(ds1Msg.c_str(), ds1X, ds1Y, ds1Font, Color{255, 255, 255, 230});
#if defined(__ANDROID__)
                if (vkUnder)
                    EndBlendMode();
#endif
            }
        }
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        if (!skipGl)
        {
            EndDrawing();
            if (perfLog)
            {
                // PT2: the GL swap shows a new game frame only when the VK
                // child is not the presenter (its queue() counts presents, on
                // the GS worker). Counting both would double-count underlay.
                bool vkShows = false;
#if defined(__ANDROID__)
                vkShows = ps2x_present_vk::active();
#endif
                if (!vkShows)
                    ps2x::perflog::notePresent();
            }
#if defined(__ANDROID__)
            ++s_glSwapsOnWindow;
#endif
        }
        else
        {
            // What EndDrawing would do minus the swap: poll input (blocks while
            // the app is in the background). For throughput measurement, the
            // AHB sink may drop presents; poll faster than the display rate.
            PollInputEvents();
#if defined(__ANDROID__)
            static const bool s_unpacedPresent = [] {
                const char *v = std::getenv("PS2X_PRESENT_UNPACED");
                return v && std::strcmp(v, "1") == 0;
            }();
            if (vkLive && s_unpacedPresent)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            else
#endif
            {
                static auto s_nextFrame = std::chrono::steady_clock::now();
                // DP1: the skip-GL loop latches one guest frame per iteration, so it must run at the
                // panel rate (120 presents/s need a 120 Hz loop); 60 keeps the old period exactly.
                // The branch only runs with the VK layer live (Android); elsewhere skipGl is false.
                static const int s_framePeriodUs = ps2x_present_vk::displayHz() == 120 ? 8333 : 16667;
                s_nextFrame += std::chrono::microseconds(s_framePeriodUs);
                const auto now = std::chrono::steady_clock::now();
                if (s_nextFrame < now)
                    s_nextFrame = now;
                else
                    std::this_thread::sleep_until(s_nextFrame);
            }
        }

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
    // PT2: full-ring [perf-tail] dump (no-op unless the log is active; a
    // force-stop SIGKILLs past here, so the Odin reads the per-second lines).
    if (perfLog)
        ps2x::perflog::dumpTail();
}
