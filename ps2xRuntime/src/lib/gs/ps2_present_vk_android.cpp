// VK1 prototype (PS2X_PRESENT_VULKAN=1, Android): SurfaceControl side of the
// Vulkan present. See runtime/gs/ps2_present_vk.h. VK2: the bookkeeping lives
// in ps2x_present_vk::Ledger; this file is its NDK platform and the JNI/diag glue.
#include "runtime/gs/ps2_present_vk.h"
#include "runtime/gs/ps2_present_vk_ledger.h"

#include <android/hardware_buffer.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/rect.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
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

using ps2x_present_vk::Ledger;
using ps2x_present_vk::LayerGeometry;

void onComplete(void *context, TS *stats);

// The ledger's platform: one NDK transaction per call, completion token as the
// callback context (a number, never a pointer to free).
struct NdkPlatform final : ps2x_present_vk::Platform
{
    void *createLayer(void *window) override
    {
        return api().createFromWindow(static_cast<ANativeWindow *>(window), "ps2x-game");
    }
    void releaseLayer(void *layer) override { api().release(static_cast<SC *>(layer)); }
    void applyBuffer(void *layer, void *buffer, const LayerGeometry *g, uint64_t token) override
    {
        const Api &a = api();
        SC *sc = static_cast<SC *>(layer);
        TX *tx = a.txCreate();
        a.setBuffer(tx, sc, static_cast<AHardwareBuffer *>(buffer), -1); // the caller waited for the GPU fence
        if (g)
        {
            const ARect src = {0, 0, g->srcW, g->srcH};
            const ARect dst = {g->left, g->top, g->right, g->bottom};
            a.setGeometry(tx, sc, src, dst, 0);
            a.setZOrder(tx, sc, g->z);
            a.setVisibility(tx, sc, kVisibilityShow);
            a.setBufferTransparency(tx, sc, kTransparencyOpaque); // PS2 alpha is not display alpha
        }
        a.setOnComplete(tx, reinterpret_cast<void *>(static_cast<uintptr_t>(token)), onComplete);
        a.txApply(tx);
        a.txDelete(tx);
    }
    void applyDetach(void *layer, uint64_t token) override
    {
        const Api &a = api();
        TX *tx = a.txCreate();
        a.reparent(tx, static_cast<SC *>(layer), nullptr);
        // Completion registered: removing the layer from the tree reports the
        // release of the buffer it was showing (RV5 B2).
        a.setOnComplete(tx, reinterpret_cast<void *>(static_cast<uintptr_t>(token)), onComplete);
        a.txApply(tx);
        a.txDelete(tx);
    }
    void releaseBuffer(void *buffer) override { AHardwareBuffer_release(static_cast<AHardwareBuffer *>(buffer)); }
    int dupFd(int fd) override { return fcntl(fd, F_DUPFD_CLOEXEC, 0); }
    void closeFd(int fd) override { close(fd); }
    int pollFd(int fd, int timeoutMs, short &revents, int &err) override
    {
        pollfd p = {fd, POLLIN, 0};
        const int r = poll(&p, 1, timeoutMs);
        err = r < 0 ? errno : 0;
        revents = p.revents;
        return r;
    }
};

Ledger &ledger()
{
    // Never destroyed: completions can arrive on a binder thread during exit.
    static Ledger *l = new Ledger(*new NdkPlatform());
    return *l;
}

// Main-thread logging state only (layer size from the DecorView).
struct WindowLog
{
    int layerW = 0, layerH = 0;
};
WindowLog g_winLog;

void onComplete(void *context, TS *stats)
{
    const uint64_t token = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context));
    const Api &a = api();
    Ledger &l = ledger();
    // The ledger keeps the layer alive until this token completes.
    SC *sc = static_cast<SC *>(l.tokenLayer(token));
    bool inStats = false;
    SC **list = nullptr;
    size_t n = 0;
    a.getASurfaceControls(stats, &list, &n);
    for (size_t i = 0; i < n; ++i)
        inStats = inStats || (sc && list[i] == sc);
    if (list)
        a.releaseASurfaceControls(list);
    const int fd = inStats ? a.getPreviousReleaseFenceFd(stats, sc) : -1; // ours to close (the ledger does)
    l.complete(token, inStats, fd, a.getLatchTime(stats));
}

// Window size in layer pixels: DecorView width/height through JNI (the
// window's buffers are raylib's render size, which SurfaceFlinger scales;
// child layers are placed in layer space). 0 until the view is laid out.
bool queryLayerSize(ANativeActivity *activity, int &w, int &h)
{
    if (!activity || !activity->vm)
        return false;
    JNIEnv *env = nullptr;
    // Keep the thread's name: an unnamed attach renames it "Thread-N", which
    // hides the main thread from per-thread profiles that key on the name.
    char name[16] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    JavaVMAttachArgs args = {JNI_VERSION_1_6, name[0] ? name : nullptr, nullptr};
    if (activity->vm->AttachCurrentThread(&env, &args) != JNI_OK || !env)
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
        if (v && std::strcmp(v, "0") == 0)
        {
            std::fprintf(stderr, "[present-vk] off (PS2X_PRESENT_VULKAN=0): GL present\n");
            return false;
        }
        return api().ok; // default on; API < 29 keeps the GL path
    }();
    return on;
}

bool active() { return enabled() && ledger().active(); }
bool broken() { return ledger().broken(); }
void fallBack(const char *why) { ledger().fallBack(why); }
void setUnderlay(bool under) { ledger().setUnderlay(under); }
bool underlay() { return ledger().underlay(); }
bool gameRect(int &left, int &top, int &right, int &bottom) { return ledger().gameRect(left, top, right, bottom); }
uint32_t windowGeneration() { return ledger().windowGeneration(); }
bool layerLive() { return ledger().layerLive(); }

void windowLost()
{
    if (!enabled())
        return;
    ledger().windowLost();
}

void setHostWindow(ANativeWindow *window, ANativeActivity *activity, int aspect, int bufferW, int bufferH)
{
    if (!enabled())
        return;
    if (!ledger().setWindow(window, aspect, bufferW, bufferH))
    {
        if (window && g_winLog.layerW <= 0)
        {
            int w = 0, h = 0;
            if (queryLayerSize(activity, w, h))
            {
                g_winLog.layerW = w;
                g_winLog.layerH = h;
                std::fprintf(stderr, "[present-vk] layer size %dx%d (decor)\n", w, h);
            }
        }
        return;
    }
    g_winLog.layerW = g_winLog.layerH = 0;
    if (window)
    {
        int w = 0, h = 0;
        if (queryLayerSize(activity, w, h))
        {
            g_winLog.layerW = w;
            g_winLog.layerH = h;
        }
        const Ledger::Counts c = ledger().counts();
        std::fprintf(stderr,
                     "[present-vk] child layer %s on window %p (%dx%d buffers), layer %dx%d, gen %u, layers live %u "
                     "(detached awaiting completion %u)\n",
                     ledger().broken() ? "NOT made (fallback)" : "made", static_cast<void *>(window),
                     ANativeWindow_getWidth(window), ANativeWindow_getHeight(window), g_winLog.layerW, g_winLog.layerH,
                     ledger().windowGeneration(), c.liveLayers, c.retiredLayers);
    }
}

uint64_t allocateBuffer(uint32_t w, uint32_t h, AHardwareBuffer **out)
{
    *out = nullptr;
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
    if (rc != 0 || !buf)
    {
        std::fprintf(stderr, "[present-vk] AHB alloc %ux%u rc=%d\n", w, h, rc);
        return 0;
    }
    const uint64_t id = ledger().registerBuffer(buf);
    std::fprintf(stderr, "[present-vk] AHB alloc %ux%u rc=%d stride=%u usage=0x%llx id=%llu epoch=%u\n", w, h, rc,
                 got.stride, static_cast<unsigned long long>(got.usage), static_cast<unsigned long long>(id),
                 ledger().bufferEpoch(id));
    *out = buf;
    return id;
}

void retireBuffer(uint64_t id)
{
    if (id != 0u)
        ledger().retireBuffer(id);
}

uint32_t poolEpoch() { return ledger().epoch(); }
uint32_t bufferEpoch(uint64_t id) { return ledger().bufferEpoch(id); }

Pick pickReusable(const uint64_t *ids, int n, int start, int timeoutMs)
{
    const Ledger::Pick p = ledger().pick(ids, n, -1, start, timeoutMs);
    Pick out;
    out.index = p.index;
    out.giveUp = p.giveUp;
    return out;
}

bool queue(uint64_t id, uint32_t w, uint32_t h) { return ledger().queue(id, w, h); }

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
    const Ledger::Counts c = ledger().counts();
    const double waitMs = c.queued ? (static_cast<double>(c.waitNs) / 1e6) / c.queued : 0.0;
    const double applyMs = c.queued ? (static_cast<double>(c.applyNs) / 1e6) / c.queued : 0.0;
    const double latchMs = c.latchIntervals ? (static_cast<double>(c.latchIntervalSumNs) / 1e6) / c.latchIntervals : 0.0;
    auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    std::snprintf(out, size,
                  " vk_queued=%llu vk_dropped=%llu vk_skipped=%llu vk_callbacks=%llu vk_release_timeouts=%llu"
                  " vk_cb_timeouts=%llu vk_fence_timeouts=%llu vk_fence_errors=%llu vk_eintr=%llu vk_stale_cb=%llu"
                  " vk_absent_cb=%llu vk_refused=%llu vk_layers=%u vk_layers_detached=%u vk_layers_made=%llu"
                  " vk_layers_released=%llu vk_bufs=%u vk_bufs_released=%llu vk_fences_held=%u vk_tokens=%u"
                  " vk_release_wait_ms_avg=%.3f vk_apply_ms_avg=%.3f vk_latch_interval_ms_avg=%.2f",
                  u(c.queued), u(c.dropped), u(c.skipped), u(c.callbacks), u(c.cbTimeouts + c.fenceTimeouts),
                  u(c.cbTimeouts), u(c.fenceTimeouts), u(c.fenceErrors), u(c.eintr), u(c.staleCallbacks),
                  u(c.absentCallbacks), u(c.refusedQueues), c.liveLayers, c.retiredLayers, u(c.layersCreated),
                  u(c.layersReleased), c.records, u(c.buffersReleased), c.openFences, c.tokens, waitMs, applyMs,
                  latchMs);
}
} // namespace ps2x_present_vk
