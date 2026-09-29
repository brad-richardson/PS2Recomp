// VK1 prototype (PS2X_PRESENT_VULKAN=1, Android): SurfaceControl side of the
// Vulkan present. See runtime/gs/ps2_present_vk.h. VK2: the bookkeeping lives
// in ps2x_present_vk::Ledger; this file is its NDK platform and the JNI/diag glue.
#include "runtime/gs/ps2_present_vk.h"
#include "ps2_perf_log.h"
#include "runtime/gs/ps2_present_vk_ledger.h"

#include <android/hardware_buffer.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/rect.h>
#include <dirent.h>
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
    // DP1: optional frame-rate votes (API 30/31+), dlsym'd like the rest so
    // minSdk 29 still loads. Never part of `ok`: 60 Hz needs none of them.
    void (*setFrameRateWin)(ANativeWindow *window, float frameRate, int8_t compatibility) = nullptr; // API 30+
    void (*setFrameRateTx)(TX *tx, SC *sc, float frameRate, int8_t compatibility,
                           int8_t changeFrameRateStrategy) = nullptr; // API 31+
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
        get(a.setFrameRateWin, "ANativeWindow_setFrameRate"); // optional (DP1)
        get(a.setFrameRateTx, "ASurfaceTransaction_setFrameRate"); // optional (DP1)
        std::fprintf(stderr, "[present-vk] SurfaceControl API %s\n", a.ok ? "ok" : "MISSING (API < 29?)");
        std::fprintf(stderr, "[present-vk] frame-rate API: window=%s tx=%s\n",
                     a.setFrameRateWin ? "ok" : "MISSING", a.setFrameRateTx ? "ok" : "MISSING");
        return a;
    }();
    return s;
}

constexpr int8_t kVisibilityShow = 1;   // ASURFACE_TRANSACTION_VISIBILITY_SHOW
constexpr int8_t kTransparencyOpaque = 2; // ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE
// DP1: ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE and
// ASURFACE_TRANSACTION_CHANGE_FRAME_RATE_ALWAYS (declared here, not from the
// NDK headers, so minSdk 29 still compiles; both calls are dlsym'd).
constexpr int8_t kFrameRateFixedSource = 1;
constexpr int8_t kFrameRateAlways = 1;

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
            // DP1: vote 120 on the game layer itself (geometry is applied on
            // the first queue of a window, then only on size changes).
            if (ps2x_present_vk::displayHz() == 120 && a.setFrameRateTx)
                a.setFrameRateTx(tx, sc, 120.0f, kFrameRateFixedSource, kFrameRateAlways);
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

// Open fds of this process (/proc/self/fd minus the directory's own fd): the
// release APK is not debuggable, so adb cannot list /proc/<pid>/fd (VK2 ST1).
int openFdCount()
{
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;
    int n = 0;
    while (const dirent *e = readdir(d))
        n += e->d_name[0] != '.';
    closedir(d);
    return n - 1;
}

// Main-thread logging state only (layer size from the DecorView).
struct WindowLog
{
    int layerW = 0, layerH = 0;
};
WindowLog g_winLog;

// DP1: force the 120 Hz display mode for this window through
// WindowManager.LayoutParams.preferredDisplayModeId (API 23+, so no dlsym
// needed). Picks the first supported mode at 119-121 Hz. Returns the chosen
// mode id, or 0 when no mode fits or any JNI step failed.
int applyPreferredDisplayMode120(ANativeActivity *activity)
{
    if (!activity || !activity->vm)
        return 0;
    JNIEnv *env = nullptr;
    char name[16] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    JavaVMAttachArgs args = {JNI_VERSION_1_6, name[0] ? name : nullptr, nullptr};
    if (activity->vm->AttachCurrentThread(&env, &args) != JNI_OK || !env)
        return 0;
    int chosenId = 0;
    float chosenRate = 0.0f;
    jobject act = activity->clazz;
    if (jclass actCls = env->GetObjectClass(act))
    {
        // Display.Mode[] modes = getWindowManager().getDefaultDisplay().getSupportedModes();
        const jmethodID getWM = env->GetMethodID(actCls, "getWindowManager", "()Landroid/view/WindowManager;");
        jobject wm = (getWM && !env->ExceptionCheck()) ? env->CallObjectMethod(act, getWM) : nullptr;
        if (wm && !env->ExceptionCheck())
        {
            jclass wmCls = env->GetObjectClass(wm);
            const jmethodID getDisplay =
                wmCls ? env->GetMethodID(wmCls, "getDefaultDisplay", "()Landroid/view/Display;") : nullptr;
            jobject display = (getDisplay && !env->ExceptionCheck()) ? env->CallObjectMethod(wm, getDisplay) : nullptr;
            if (display && !env->ExceptionCheck())
            {
                jclass dispCls = env->GetObjectClass(display);
                const jmethodID getModes = dispCls ? env->GetMethodID(dispCls, "getSupportedModes",
                                                                     "()[Landroid/view/Display$Mode;")
                                                   : nullptr;
                auto modes = (getModes && !env->ExceptionCheck())
                                 ? static_cast<jobjectArray>(env->CallObjectMethod(display, getModes))
                                 : nullptr;
                if (modes && !env->ExceptionCheck())
                {
                    const jsize n = env->GetArrayLength(modes);
                    for (jsize i = 0; i < n && !env->ExceptionCheck(); ++i)
                    {
                        jobject mode = env->GetObjectArrayElement(modes, i);
                        if (!mode || env->ExceptionCheck())
                            break;
                        jclass modeCls = env->GetObjectClass(mode);
                        const jmethodID getRate = modeCls ? env->GetMethodID(modeCls, "getRefreshRate", "()F") : nullptr;
                        const jmethodID getId = modeCls ? env->GetMethodID(modeCls, "getModeId", "()I") : nullptr;
                        if (getRate && getId)
                        {
                            const float rate = env->CallFloatMethod(mode, getRate);
                            const int id = env->CallIntMethod(mode, getId);
                            if (!env->ExceptionCheck())
                            {
                                std::fprintf(stderr, "[present-vk] DP1 display mode id=%d %.2f Hz\n", id, rate);
                                if (chosenId == 0 && rate >= 119.0f && rate <= 121.0f)
                                {
                                    chosenId = id;
                                    chosenRate = rate;
                                }
                            }
                        }
                        if (modeCls)
                            env->DeleteLocalRef(modeCls);
                        env->DeleteLocalRef(mode);
                    }
                    env->DeleteLocalRef(modes);
                }
                if (dispCls)
                    env->DeleteLocalRef(dispCls);
                env->DeleteLocalRef(display);
            }
            if (wmCls)
                env->DeleteLocalRef(wmCls);
            env->DeleteLocalRef(wm);
        }
        // getWindow().getAttributes().preferredDisplayModeId = id; getWindow().setAttributes(lp);
        if (chosenId != 0 && !env->ExceptionCheck())
        {
            const jmethodID getWindow = env->GetMethodID(actCls, "getWindow", "()Landroid/view/Window;");
            jobject win = (getWindow && !env->ExceptionCheck()) ? env->CallObjectMethod(act, getWindow) : nullptr;
            if (win && !env->ExceptionCheck())
            {
                jclass winCls = env->GetObjectClass(win);
                const jmethodID getAttrs = winCls ? env->GetMethodID(winCls, "getAttributes",
                                                                    "()Landroid/view/WindowManager$LayoutParams;")
                                                  : nullptr;
                jobject lp = (getAttrs && !env->ExceptionCheck()) ? env->CallObjectMethod(win, getAttrs) : nullptr;
                if (lp && !env->ExceptionCheck())
                {
                    jclass lpCls = env->GetObjectClass(lp);
                    const jfieldID modeIdField =
                        lpCls ? env->GetFieldID(lpCls, "preferredDisplayModeId", "I") : nullptr;
                    const jmethodID setAttrs =
                        (winCls && modeIdField && !env->ExceptionCheck())
                            ? env->GetMethodID(winCls, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V")
                            : nullptr;
                    if (modeIdField && setAttrs && !env->ExceptionCheck())
                    {
                        env->SetIntField(lp, modeIdField, chosenId);
                        env->CallVoidMethod(win, setAttrs, lp);
                    }
                    if (env->ExceptionCheck())
                        chosenId = 0;
                    if (lpCls)
                        env->DeleteLocalRef(lpCls);
                    env->DeleteLocalRef(lp);
                }
                else
                    chosenId = 0;
                if (winCls)
                    env->DeleteLocalRef(winCls);
                env->DeleteLocalRef(win);
            }
            else
                chosenId = 0;
        }
        env->DeleteLocalRef(actCls);
    }
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        chosenId = 0;
    }
    std::fprintf(stderr, "[present-vk] DP1 preferredDisplayModeId=%d (%.2f Hz)%s\n", chosenId, chosenRate,
                 chosenId ? "" : " FAILED");
    return chosenId;
}

// DP1: request the 120 Hz panel on a new parent window: a fixed-source 120
// vote on the window itself plus the forced 120 display mode. Called only when
// PS2X_DISPLAY_HZ=120; the 60 path deliberately touches nothing.
void requestDisplay120(ANativeWindow *window, ANativeActivity *activity)
{
    const Api &a = api();
    if (a.setFrameRateWin)
    {
        a.setFrameRateWin(window, 120.0f, kFrameRateFixedSource);
        std::fprintf(stderr, "[present-vk] DP1 window frame rate 120 (fixed source)\n");
    }
    else
    {
        std::fprintf(stderr, "[present-vk] DP1 window frame rate NOT set (API < 30?)\n");
    }
    applyPreferredDisplayMode120(activity);
}

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
        std::fprintf(stderr, "[present-vk] child layer %s on window %p (%dx%d buffers), layer %dx%d, gen %u\n",
                     ledger().broken() ? "NOT made (fallback)" : "made", static_cast<void *>(window),
                     ANativeWindow_getWidth(window), ANativeWindow_getHeight(window), g_winLog.layerW, g_winLog.layerH,
                     ledger().windowGeneration());
        // DP1: one 120 Hz request per new window (nothing at all on the 60 path).
        if (ps2x_present_vk::displayHz() == 120)
            requestDisplay120(window, activity);
    }
    // VK2: the ledger's counts at every window change (lifecycle stress receipts).
    char stats[1024];
    appendStats(stats, sizeof(stats));
    std::fprintf(stderr, "[present-vk] window-change stats%s proc_fds=%d\n", stats, openFdCount());
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

bool queue(uint64_t id, uint32_t w, uint32_t h)
{
    // PT2: a shown buffer is the present event on this path (the main loop
    // skips GL swaps once the layer is live, so EndDrawing never fires).
    // notePresent is thread-safe (GS worker calls it); the GL-path call is
    // skipped while active(), so each frame counts exactly once.
    const bool shown = ledger().queue(id, w, h);
    if (shown)
        ps2x::perflog::notePresent();
    return shown;
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
