// VK1 prototype (PS2X_PRESENT_VULKAN=1, Android): SurfaceControl side of the
// Vulkan present. See runtime/gs/ps2_present_vk.h.
#include "runtime/gs/ps2_present_vk.h"
#include "ps2_present_geometry.h"

#include <android/hardware_buffer.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/rect.h>
#include <dlfcn.h>
#include <jni.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace
{
// API 29+ NDK entry points, resolved from libandroid.so so minSdk 28 still
// loads. Opaque types are declared here to stay independent of the NDK's
// availability guards.
struct SC; // ASurfaceControl
struct TX; // ASurfaceTransaction
struct TS; // ASurfaceTransactionStats
using OnComplete = void (*)(void *context, TS *stats);
struct Api
{
    SC *(*createFromWindow)(ANativeWindow *parent, const char *name) = nullptr;
    void (*release)(SC *) = nullptr;
    TX *(*txCreate)() = nullptr;
    void (*txDelete)(TX *) = nullptr;
    void (*txApply)(TX *) = nullptr;
    void (*setBuffer)(TX *, SC *, AHardwareBuffer *, int acquireFenceFd) = nullptr;
    void (*setGeometry)(TX *, SC *, const ARect &src, const ARect &dst, int32_t transform) = nullptr;
    void (*setZOrder)(TX *, SC *, int32_t z) = nullptr;
    void (*setVisibility)(TX *, SC *, int8_t visibility) = nullptr;
    void (*setBufferTransparency)(TX *, SC *, int8_t transparency) = nullptr;
    void (*setOnComplete)(TX *, void *context, OnComplete func) = nullptr;
    void (*reparent)(TX *, SC *, SC *newParent) = nullptr;
    int (*getPreviousReleaseFenceFd)(TS *, SC *) = nullptr;
    void (*getASurfaceControls)(TS *, SC ***outList, size_t *outSize) = nullptr;
    void (*releaseASurfaceControls)(SC **list) = nullptr;
    int64_t (*getLatchTime)(TS *) = nullptr;
    bool ok = false;
};

const Api &api()
{
    static const Api s = [] {
        Api a;
        void *lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib)
            return a;
        auto get = [&](auto &fn, const char *name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            return fn != nullptr;
        };
        a.ok = get(a.createFromWindow, "ASurfaceControl_createFromWindow") &&
               get(a.release, "ASurfaceControl_release") && get(a.txCreate, "ASurfaceTransaction_create") &&
               get(a.txDelete, "ASurfaceTransaction_delete") && get(a.txApply, "ASurfaceTransaction_apply") &&
               get(a.setBuffer, "ASurfaceTransaction_setBuffer") &&
               get(a.setGeometry, "ASurfaceTransaction_setGeometry") &&
               get(a.setZOrder, "ASurfaceTransaction_setZOrder") &&
               get(a.setVisibility, "ASurfaceTransaction_setVisibility") &&
               get(a.setBufferTransparency, "ASurfaceTransaction_setBufferTransparency") &&
               get(a.setOnComplete, "ASurfaceTransaction_setOnComplete") &&
               get(a.reparent, "ASurfaceTransaction_reparent") &&
               get(a.getPreviousReleaseFenceFd, "ASurfaceTransactionStats_getPreviousReleaseFenceFd") &&
               get(a.getASurfaceControls, "ASurfaceTransactionStats_getASurfaceControls") &&
               get(a.releaseASurfaceControls, "ASurfaceTransactionStats_releaseASurfaceControls") &&
               get(a.getLatchTime, "ASurfaceTransactionStats_getLatchTime");
        std::fprintf(stderr, "[present-vk] SurfaceControl API %s\n", a.ok ? "ok" : "MISSING (API < 29?)");
        return a;
    }();
    return s;
}

constexpr int8_t kVisibilityShow = 1;   // ASURFACE_TRANSACTION_VISIBILITY_SHOW
constexpr int8_t kTransparencyOpaque = 2; // ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE

struct BufState
{
    bool pending = false; // queued, release fence not delivered yet
    int releaseFd = -1;
};

struct Sink
{
    std::mutex m;
    std::condition_variable cv;
    ANativeWindow *window = nullptr;
    SC *sc = nullptr;
    std::vector<SC *> graveyard; // detached layers (callbacks may still name them)
    int layerW = 0, layerH = 0;
    int bufW = 0, bufH = 0; // parent's buffer size: the child's coordinate space
    int aspect = 0;
    bool geometrySet = false;
    uint32_t lastW = 0, lastH = 0;
    AHardwareBuffer *lastQueued = nullptr;
    std::unordered_map<AHardwareBuffer *, BufState> bufs;
    // counters
    uint64_t queued = 0, dropped = 0, releaseTimeouts = 0, callbacks = 0;
    uint64_t releaseWaitNs = 0, applyNs = 0;
    int64_t lastLatch = 0;
    uint64_t latchIntervals = 0;
    int64_t latchIntervalSumNs = 0;
};

Sink &sink()
{
    static Sink s;
    return s;
}

std::atomic<bool> g_active{false};

struct TxContext
{
    SC *sc;
    AHardwareBuffer *prev; // the buffer this transaction replaced
};

void onComplete(void *context, TS *stats)
{
    auto *ctx = static_cast<TxContext *>(context);
    const Api &a = api();
    int fd = -1;
    bool present = false;
    SC **list = nullptr;
    size_t n = 0;
    a.getASurfaceControls(stats, &list, &n);
    for (size_t i = 0; i < n; ++i)
        present = present || list[i] == ctx->sc;
    if (list)
        a.releaseASurfaceControls(list);
    if (present && ctx->prev)
        fd = a.getPreviousReleaseFenceFd(stats, ctx->sc);
    const int64_t latch = a.getLatchTime(stats);
    Sink &s = sink();
    {
        std::lock_guard<std::mutex> lock(s.m);
        ++s.callbacks;
        if (latch > 0)
        {
            if (s.lastLatch > 0 && latch > s.lastLatch)
            {
                s.latchIntervalSumNs += latch - s.lastLatch;
                ++s.latchIntervals;
            }
            s.lastLatch = latch;
        }
        if (ctx->prev)
        {
            BufState &b = s.bufs[ctx->prev];
            if (b.releaseFd >= 0)
                close(b.releaseFd);
            b.releaseFd = fd;
            b.pending = false;
        }
        else if (fd >= 0)
            close(fd);
    }
    s.cv.notify_all();
    delete ctx;
}

// Window size in layer pixels: DecorView width/height through JNI (the
// window's buffers are raylib's render size, which SurfaceFlinger scales;
// child layers are placed in layer space). 0 until the view is laid out.
bool queryLayerSize(ANativeActivity *activity, int &w, int &h)
{
    if (!activity || !activity->vm)
        return false;
    JNIEnv *env = nullptr;
    if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env)
        return false;
    bool ok = false;
    jobject act = activity->clazz;
    jclass actCls = env->GetObjectClass(act);
    jmethodID getWindow = env->GetMethodID(actCls, "getWindow", "()Landroid/view/Window;");
    jobject win = getWindow ? env->CallObjectMethod(act, getWindow) : nullptr;
    if (win && !env->ExceptionCheck())
    {
        jclass winCls = env->GetObjectClass(win);
        jmethodID getDecor = env->GetMethodID(winCls, "getDecorView", "()Landroid/view/View;");
        jobject view = getDecor ? env->CallObjectMethod(win, getDecor) : nullptr;
        if (view && !env->ExceptionCheck())
        {
            jclass viewCls = env->GetObjectClass(view);
            jmethodID gw = env->GetMethodID(viewCls, "getWidth", "()I");
            jmethodID gh = env->GetMethodID(viewCls, "getHeight", "()I");
            if (gw && gh)
            {
                w = env->CallIntMethod(view, gw);
                h = env->CallIntMethod(view, gh);
                ok = !env->ExceptionCheck() && w > 0 && h > 0;
            }
            env->DeleteLocalRef(viewCls);
            env->DeleteLocalRef(view);
        }
        env->DeleteLocalRef(winCls);
        env->DeleteLocalRef(win);
    }
    if (env->ExceptionCheck())
        env->ExceptionClear();
    env->DeleteLocalRef(actCls);
    return ok;
}

uint64_t fnv(const uint8_t *p, size_t n, uint64_t h = 1469598103934665603ull)
{
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

void writePpm(const std::string &path, const std::vector<uint8_t> &rgb, uint32_t w, uint32_t h)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
}
} // namespace

namespace ps2x_present_vk
{
bool enabled()
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_PRESENT_VULKAN");
        return v && std::strcmp(v, "1") == 0 && api().ok;
    }();
    return on;
}

bool active()
{
    return g_active.load(std::memory_order_acquire);
}

void detachLocked(Sink &s, const char *why)
{
    if (!s.sc)
        return;
    const Api &a = api();
    TX *tx = a.txCreate();
    a.reparent(tx, s.sc, nullptr);
    a.txApply(tx);
    a.txDelete(tx);
    s.graveyard.push_back(s.sc);
    s.sc = nullptr;
    std::fprintf(stderr, "[present-vk] %s: child layer detached\n", why);
}

void windowLost()
{
    if (!enabled())
        return;
    Sink &s = sink();
    std::lock_guard<std::mutex> lock(s.m);
    detachLocked(s, "APP_CMD_TERM_WINDOW");
    s.window = nullptr;
    s.geometrySet = false;
}

void setHostWindow(ANativeWindow *window, ANativeActivity *activity, int aspect, int bufferW, int bufferH)
{
    if (!enabled())
        return;
    const Api &a = api();
    Sink &s = sink();
    std::lock_guard<std::mutex> lock(s.m);
    if (aspect != s.aspect)
    {
        s.aspect = aspect;
        s.geometrySet = false;
    }
    if (bufferW > 0 && bufferH > 0 && (bufferW != s.bufW || bufferH != s.bufH))
    {
        s.bufW = bufferW;
        s.bufH = bufferH;
        s.geometrySet = false;
        std::fprintf(stderr, "[present-vk] parent buffer %dx%d\n", bufferW, bufferH);
    }
    if (window == s.window)
    {
        if (window && s.layerW <= 0)
        {
            int w = 0, h = 0;
            if (queryLayerSize(activity, w, h))
            {
                s.layerW = w;
                s.layerH = h;
                s.geometrySet = false;
                std::fprintf(stderr, "[present-vk] layer size %dx%d (decor)\n", w, h);
            }
        }
        return;
    }
    detachLocked(s, "window changed");
    // Buffers on a detached layer are never released; don't wait for them.
    for (auto &kv : s.bufs)
    {
        kv.second.pending = false;
        if (kv.second.releaseFd >= 0)
            close(kv.second.releaseFd);
        kv.second.releaseFd = -1;
    }
    s.lastQueued = nullptr;
    s.window = window;
    s.layerW = s.layerH = 0;
    s.geometrySet = false;
    if (window)
    {
        s.sc = a.createFromWindow(window, "ps2x-game");
        int w = 0, h = 0;
        if (queryLayerSize(activity, w, h))
        {
            s.layerW = w;
            s.layerH = h;
        }
        std::fprintf(stderr, "[present-vk] child layer %p on window %p (%dx%d buffers), layer %dx%d\n",
                     static_cast<void *>(s.sc), static_cast<void *>(window), ANativeWindow_getWidth(window),
                     ANativeWindow_getHeight(window), s.layerW, s.layerH);
    }
    s.cv.notify_all();
}

AHardwareBuffer *allocateBuffer(uint32_t w, uint32_t h)
{
    AHardwareBuffer_Desc desc = {};
    desc.width = w;
    desc.height = h;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
    AHardwareBuffer *buf = nullptr;
    const int rc = AHardwareBuffer_allocate(&desc, &buf);
    AHardwareBuffer_Desc got = {};
    if (rc == 0 && buf)
        AHardwareBuffer_describe(buf, &got);
    std::fprintf(stderr, "[present-vk] AHB alloc %ux%u rc=%d stride=%u usage=0x%llx\n", w, h, rc, got.stride,
                 static_cast<unsigned long long>(got.usage));
    if (rc != 0)
        return nullptr;
    Sink &s = sink();
    std::lock_guard<std::mutex> lock(s.m);
    s.bufs[buf] = BufState{};
    return buf;
}

void releaseBuffer(AHardwareBuffer *buffer)
{
    if (!buffer)
        return;
    Sink &s = sink();
    {
        std::lock_guard<std::mutex> lock(s.m);
        auto it = s.bufs.find(buffer);
        if (it != s.bufs.end())
        {
            if (it->second.releaseFd >= 0)
                close(it->second.releaseFd);
            s.bufs.erase(it);
        }
        if (s.lastQueued == buffer)
            s.lastQueued = nullptr;
    }
    AHardwareBuffer_release(buffer); // SurfaceFlinger keeps its own reference while it shows it
}

bool waitReusable(AHardwareBuffer *buffer, int timeoutMs)
{
    Sink &s = sink();
    const auto t0 = std::chrono::steady_clock::now();
    int fd = -1;
    bool ok = true;
    {
        std::unique_lock<std::mutex> lock(s.m);
        ok = s.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !s.bufs[buffer].pending; });
        BufState &b = s.bufs[buffer];
        fd = b.releaseFd;
        b.releaseFd = -1;
        if (!ok)
        {
            b.pending = false;
            ++s.releaseTimeouts;
        }
    }
    if (fd >= 0)
    {
        pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, timeoutMs) <= 0)
            ok = false;
        close(fd);
    }
    const auto t1 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(s.m);
    s.releaseWaitNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return ok;
}

bool queue(AHardwareBuffer *buffer, uint32_t w, uint32_t h)
{
    const Api &a = api();
    Sink &s = sink();
    std::lock_guard<std::mutex> lock(s.m);
    if (!s.sc || s.bufW <= 0 || s.bufH <= 0)
    {
        ++s.dropped;
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    TX *tx = a.txCreate();
    a.setBuffer(tx, s.sc, buffer, -1); // the caller waited for the GPU fence
    if (!s.geometrySet || w != s.lastW || h != s.lastH)
    {
        // Parent buffer space (V1: display pixels here were scaled again by
        // the parent's 796x448 -> 1920x1080 buffer scaling: a zoomed picture).
        const ps2x::present::Rect r = ps2x::present::presentRect(
            static_cast<float>(s.bufW), static_cast<float>(s.bufH), static_cast<float>(w), static_cast<float>(h),
            static_cast<ps2x::present::Aspect>(s.aspect));
        const ARect src = {0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)};
        const ARect dst = {static_cast<int32_t>(r.x + 0.5f), static_cast<int32_t>(r.y + 0.5f),
                           static_cast<int32_t>(r.x + r.w + 0.5f), static_cast<int32_t>(r.y + r.h + 0.5f)};
        a.setGeometry(tx, s.sc, src, dst, 0);
        a.setZOrder(tx, s.sc, 1); // above raylib's GL window (nothing is drawn over the game on the Odin)
        a.setVisibility(tx, s.sc, kVisibilityShow);
        a.setBufferTransparency(tx, s.sc, kTransparencyOpaque); // PS2 alpha is not display alpha
        std::fprintf(stderr,
                     "[present-vk] geometry src %ux%u -> dst [%d,%d %d,%d] in parent buffer %dx%d (window %dx%d) aspect=%d\n",
                     w, h, dst.left, dst.top, dst.right, dst.bottom, s.bufW, s.bufH, s.layerW, s.layerH, s.aspect);
        s.geometrySet = true;
        s.lastW = w;
        s.lastH = h;
    }
    a.setOnComplete(tx, new TxContext{s.sc, s.lastQueued}, onComplete);
    a.txApply(tx);
    a.txDelete(tx);
    s.bufs[buffer].pending = true;
    s.lastQueued = buffer;
    ++s.queued;
    s.applyNs += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    if (s.queued == 1u)
        std::fprintf(stderr, "[present-vk] first buffer queued %ux%u\n", w, h);
    g_active.store(true, std::memory_order_release);
    return true;
}

long compareBuffer(AHardwareBuffer *buffer, const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t tick,
                   const char *dumpDir)
{
    AHardwareBuffer_Desc desc = {};
    AHardwareBuffer_describe(buffer, &desc);
    void *ptr = nullptr;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr, &ptr) != 0 || !ptr)
    {
        std::fprintf(stderr, "[present-vk] compare tick=%llu: AHardwareBuffer_lock failed\n",
                     static_cast<unsigned long long>(tick));
        return -1;
    }
    std::vector<uint8_t> a(static_cast<size_t>(w) * h * 3u), b(a.size());
    long diff = 0;
    const auto *src = static_cast<const uint8_t *>(ptr);
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t *ra = src + static_cast<size_t>(y) * desc.stride * 4u;
        const uint8_t *rb = rgba + static_cast<size_t>(y) * w * 4u;
        for (uint32_t x = 0; x < w; ++x)
        {
            uint8_t *pa = &a[(static_cast<size_t>(y) * w + x) * 3u];
            uint8_t *pb = &b[(static_cast<size_t>(y) * w + x) * 3u];
            std::memcpy(pa, ra + x * 4u, 3u);
            std::memcpy(pb, rb + x * 4u, 3u);
            diff += std::memcmp(pa, pb, 3u) != 0;
        }
    }
    AHardwareBuffer_unlock(buffer, nullptr);
    std::fprintf(stderr,
                 "[present-vk] compare tick=%llu %ux%u stride=%u ahb_rgb=%016llx readback_rgb=%016llx diff_px=%ld\n",
                 static_cast<unsigned long long>(tick), w, h, desc.stride,
                 static_cast<unsigned long long>(fnv(a.data(), a.size())),
                 static_cast<unsigned long long>(fnv(b.data(), b.size())), diff);
    if (dumpDir && *dumpDir)
    {
        const std::string base = std::string(dumpDir) + "/vk-t" + std::to_string(tick);
        writePpm(base + "-ahb.ppm", a, w, h);
        writePpm(base + "-readback.ppm", b, w, h);
    }
    return diff;
}

void appendStats(char *out, unsigned size)
{
    Sink &s = sink();
    std::lock_guard<std::mutex> lock(s.m);
    const double waitMs = s.queued ? (static_cast<double>(s.releaseWaitNs) / 1e6) / s.queued : 0.0;
    const double applyMs = s.queued ? (static_cast<double>(s.applyNs) / 1e6) / s.queued : 0.0;
    const double latchMs = s.latchIntervals ? (static_cast<double>(s.latchIntervalSumNs) / 1e6) / s.latchIntervals : 0.0;
    std::snprintf(out, size,
                  " vk_queued=%llu vk_dropped=%llu vk_callbacks=%llu vk_release_timeouts=%llu vk_release_wait_ms_avg=%.3f "
                  "vk_apply_ms_avg=%.3f vk_latch_interval_ms_avg=%.2f",
                  static_cast<unsigned long long>(s.queued), static_cast<unsigned long long>(s.dropped),
                  static_cast<unsigned long long>(s.callbacks), static_cast<unsigned long long>(s.releaseTimeouts),
                  waitMs, applyMs, latchMs);
}
} // namespace ps2x_present_vk
