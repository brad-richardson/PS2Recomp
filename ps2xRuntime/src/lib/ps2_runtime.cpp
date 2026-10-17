#include "ps2_runtime.h"
#include "ps2_hle_pools.h"
#include "ps2_ssx3_patch_grow_state.h"
#include "ps2_mtvu.h"
#include "ps2_microvu.h"
#include "ps2_native_world.h"
#include "ps2_net_dirtysock.h"
#include "ps2_net_link.h" // np-link-1: delay-lockstep client (default off)
#include "ps2_np_rider.h" // np-fix-1 (F7/F8): rider script state, verbatim move
#include "ps2_net_protocol.h" // online-default-1: 120-only policy switch
#include "ps2_android_toast.h"
#include "ps2_e7.h"
#include "ps2_e41_trace.h"
#include "ps2_fh1_full120.h"
#include "ps2_ssx3_course_manifest.h"
#include "ps2_ssx3_lod.h"
#include "ps2_ssx3_tricky_dj.h"
#include "ps2_ssx3_tricky_hud.h"
#include "ps2_ssx3_tricky_layer.h"
#include "ps2_ssx3_tricky_mc.h"
#include "ps2_ssx3_tricky_menu.h"
#include "ps2_ssx3_tricky_list.h"
#include "ps2_tk_font.h" // tk-menu-2: shared FEFONT module (upload primary, Neuropol fallback)
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include "ps2_ssx3_tricky_song.h"
#include "ps2_ssx3_tricky_gems.h"
#include "ps2_ssx3_tricky_resetvol.h"
#include "ps2_ssx3_tricky_preview.h"
#include "ps2_netsync.h" // Quick Play netplay (PS2X_NETSYNC, default off)
#include "ps2_net_stock120.h" // online-stock-2 stock race state sync (PS2X_NET_STOCK120, default off)
#include <filesystem>
#include "ps2_log.h"
#include "ps2_android_pause.h"
#include "ps2_present_fallback.h"
#include "ps2_present_geometry.h"
#include "ps2_pad_latch.h"
#include "ps2_controller_map.h"
#include "ps2_input_diag.h"
#include "ps2_adpf.h"
#include "ps2_perf_log.h"
#include "ps2_vsync_lock.h"
#include "ps2_vsync_pacer.h"
#include "ps2_virtual_pad.h"
#include "ps2_ui_toast.h"
#include "ps2_settings_overlay.h"
#include "runtime/ps2_pad.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "runtime/game_module.h"
#include "ps2_turbo_boot.h" // BTS1: boot turbo yield latches
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_headless_backend.h" // HDL1: no-raster referee backend
#include "runtime/gs/gs_stream_capture.h"
#include "runtime/gs/ps2_gs_external_backend.h"
#include "runtime/gs/ps2_present_share.h"
#include "runtime/gs/ps2_present_owner.h"
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

#if defined(__ANDROID__)
// android-touch-1b: OS-level gamepad presence via InputManager. raylib only
// reports a pad after its first input event, so a paired-but-idle pad (the
// Odin's controls before the first press) would otherwise show the overlay.
// Returns false on any JNI failure (fail-safe: the raylib/in-use polls below
// still hide once the pad is used, so an error can only show, never hide).
// Source constants are stable platform API values (InputDevice#SOURCE_GAMEPAD
// 0x400, #SOURCE_JOYSTICK 0x1000000).
bool androidInputManagerGamepad()
{
    struct android_app *app = GetAndroidApp();
    if (!app || !app->activity || !app->activity->vm)
        return false;
    JNIEnv *env = nullptr;
    char name[16] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    JavaVMAttachArgs args = {JNI_VERSION_1_6, name[0] ? name : nullptr, nullptr};
    if (app->activity->vm->AttachCurrentThread(&env, &args) != JNI_OK || !env)
        return false;
    bool found = false;
    jobject act = app->activity->clazz;
    jclass actCls = env->GetObjectClass(act);
    jmethodID getSys =
        actCls ? env->GetMethodID(actCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : nullptr;
    jclass ctxCls = env->FindClass("android/content/Context");
    jfieldID inputKey =
        ctxCls ? env->GetStaticFieldID(ctxCls, "INPUT_SERVICE", "Ljava/lang/String;") : nullptr;
    jstring key = (ctxCls && inputKey) ? (jstring)env->GetStaticObjectField(ctxCls, inputKey) : nullptr;
    jobject im = (getSys && key) ? env->CallObjectMethod(act, getSys, key) : nullptr;
    if (im && !env->ExceptionCheck())
    {
        jclass imCls = env->GetObjectClass(im);
        jmethodID getIdsArr = imCls ? env->GetMethodID(imCls, "getInputDeviceIds", "()[I") : nullptr;
        jmethodID getDev =
            imCls ? env->GetMethodID(imCls, "getInputDevice", "(I)Landroid/view/InputDevice;") : nullptr;
        jintArray ids = (getIdsArr && !env->ExceptionCheck()) ? (jintArray)env->CallObjectMethod(im, getIdsArr) : nullptr;
        if (ids && !env->ExceptionCheck())
        {
            const jsize n = env->GetArrayLength(ids);
            jint *buf = env->GetIntArrayElements(ids, nullptr);
            if (buf)
            {
                for (jsize i = 0; i < n && !found; ++i)
                {
                    jobject dev = getDev ? env->CallObjectMethod(im, getDev, buf[i]) : nullptr;
                    if (dev && !env->ExceptionCheck())
                    {
                        jclass devCls = env->GetObjectClass(dev);
                        jmethodID getSources =
                            devCls ? env->GetMethodID(devCls, "getSources", "()I") : nullptr;
                        const jint sources =
                            getSources ? env->CallIntMethod(dev, getSources) : 0;
                        if (!env->ExceptionCheck() && (sources & (0x400 | 0x1000000)) != 0)
                            found = true;
                        if (devCls)
                            env->DeleteLocalRef(devCls);
                        env->DeleteLocalRef(dev);
                    }
                    if (env->ExceptionCheck())
                        env->ExceptionClear();
                }
                env->ReleaseIntArrayElements(ids, buf, JNI_ABORT);
            }
            env->DeleteLocalRef(ids);
        }
        if (imCls)
            env->DeleteLocalRef(imCls);
        env->DeleteLocalRef(im);
    }
    if (key)
        env->DeleteLocalRef(key);
    if (ctxCls)
        env->DeleteLocalRef(ctxCls);
    if (actCls)
        env->DeleteLocalRef(actCls);
    if (env->ExceptionCheck())
        env->ExceptionClear();
    return found;
}
#endif
#if defined(__ANDROID__)
// panel-polish-1: display safe insets in physical pixels (Brad 10-15: the
// perf overlay must clear rounded corners and the camera cutout). JNI shape
// mirrors ap1HideSystemBars above. Root system-bars insets maxed per side
// with the DisplayCutout safe insets (immersive mode zeroes the bars; the
// cutout side survives). Queried at most 1/s and on window change; zeros on
// any failure (fail-static: the overlay draws where it always did). The
// caller converts to raylib screen coordinates (this helper sits above the
// mid-file includes that declare GetScreenWidth/GetRenderWidth).
static bool androidSafeInsets(float &left, float &top, float &right, float &bottom)
{
    left = top = right = bottom = 0.0f;
    struct android_app *app = GetAndroidApp();
    if (!app || !app->activity || !app->activity->vm || !app->window)
        return false;
    static float cL = 0.0f, cT = 0.0f, cR = 0.0f, cB = 0.0f;
    static bool have = false;
    static void *lastWindow = nullptr;
    static auto lastQuery = std::chrono::steady_clock::now() - std::chrono::hours(24);
    const auto now = std::chrono::steady_clock::now();
    if (have && lastWindow == app->window && now - lastQuery < std::chrono::seconds(1))
    {
        left = cL;
        top = cT;
        right = cR;
        bottom = cB;
        return true;
    }
    JNIEnv *env = nullptr;
    char name[16] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    JavaVMAttachArgs args = {JNI_VERSION_1_6, name[0] ? name : nullptr, nullptr};
    if (app->activity->vm->AttachCurrentThread(&env, &args) != JNI_OK || !env)
        return false;
    auto failed = [&] {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return false;
    };
    bool got = false;
    float pxL = 0, pxT = 0, pxR = 0, pxB = 0;
    jobject act = app->activity->clazz;
    jclass actCls = env->GetObjectClass(act);
    jmethodID getWindow =
        actCls ? env->GetMethodID(actCls, "getWindow", "()Landroid/view/Window;") : nullptr;
    jobject win = (getWindow && !env->ExceptionCheck()) ? env->CallObjectMethod(act, getWindow)
                                                        : nullptr;
    jclass winCls = (win && !env->ExceptionCheck()) ? env->GetObjectClass(win) : nullptr;
    jmethodID getDecor =
        winCls ? env->GetMethodID(winCls, "getDecorView", "()Landroid/view/View;") : nullptr;
    jobject view = (getDecor && !env->ExceptionCheck()) ? env->CallObjectMethod(win, getDecor)
                                                        : nullptr;
    jclass viewCls = (view && !env->ExceptionCheck()) ? env->GetObjectClass(view) : nullptr;
    jmethodID getRootInsets =
        viewCls ? env->GetMethodID(viewCls, "getRootWindowInsets", "()Landroid/view/WindowInsets;")
                : nullptr;
    jobject insets = (getRootInsets && !env->ExceptionCheck())
                         ? env->CallObjectMethod(view, getRootInsets)
                         : nullptr;
    if (insets && !env->ExceptionCheck())
    {
        jclass insetsCls = env->GetObjectClass(insets);
        if (android_get_device_api_level() >= 30)
        {
            jclass typeCls = env->FindClass("android/view/WindowInsets$Type");
            jmethodID systemBars =
                typeCls ? env->GetStaticMethodID(typeCls, "systemBars", "()I") : nullptr;
            const jint types =
                (typeCls && systemBars && !env->ExceptionCheck())
                    ? env->CallStaticIntMethod(typeCls, systemBars)
                    : 0;
            jmethodID getInsets =
                insetsCls ? env->GetMethodID(insetsCls, "getInsets", "(I)Landroid/graphics/Insets;")
                          : nullptr;
            jobject bars = (getInsets && types != 0 && !env->ExceptionCheck())
                               ? env->CallObjectMethod(insets, getInsets, types)
                               : nullptr;
            if (bars && !env->ExceptionCheck())
            {
                jclass barsCls = env->GetObjectClass(bars);
                jfieldID fL = barsCls ? env->GetFieldID(barsCls, "left", "I") : nullptr;
                jfieldID fT = barsCls ? env->GetFieldID(barsCls, "top", "I") : nullptr;
                jfieldID fR = barsCls ? env->GetFieldID(barsCls, "right", "I") : nullptr;
                jfieldID fB = barsCls ? env->GetFieldID(barsCls, "bottom", "I") : nullptr;
                if (fL && fT && fR && fB && !env->ExceptionCheck())
                {
                    pxL = static_cast<float>(env->GetIntField(bars, fL));
                    pxT = static_cast<float>(env->GetIntField(bars, fT));
                    pxR = static_cast<float>(env->GetIntField(bars, fR));
                    pxB = static_cast<float>(env->GetIntField(bars, fB));
                    got = !env->ExceptionCheck();
                }
                if (barsCls)
                    env->DeleteLocalRef(barsCls);
                env->DeleteLocalRef(bars);
            }
            if (typeCls)
                env->DeleteLocalRef(typeCls);
        }
        else
        {
            jmethodID gL = env->GetMethodID(insetsCls, "getSystemWindowInsetLeft", "()I");
            jmethodID gT = env->GetMethodID(insetsCls, "getSystemWindowInsetTop", "()I");
            jmethodID gR = env->GetMethodID(insetsCls, "getSystemWindowInsetRight", "()I");
            jmethodID gB = env->GetMethodID(insetsCls, "getSystemWindowInsetBottom", "()I");
            if (gL && gT && gR && gB && !env->ExceptionCheck())
            {
                pxL = static_cast<float>(env->CallIntMethod(insets, gL));
                pxT = static_cast<float>(env->CallIntMethod(insets, gT));
                pxR = static_cast<float>(env->CallIntMethod(insets, gR));
                pxB = static_cast<float>(env->CallIntMethod(insets, gB));
                got = !env->ExceptionCheck();
            }
        }
        if (got)
        {
            // DisplayCutout (API 28+; our minSdk is 29): the camera side.
            jmethodID getCutout = env->GetMethodID(
                insetsCls, "getDisplayCutout", "()Landroid/view/DisplayCutout;");
            jobject cutout = (getCutout && !env->ExceptionCheck())
                                 ? env->CallObjectMethod(insets, getCutout)
                                 : nullptr;
            if (cutout && !env->ExceptionCheck())
            {
                jclass cutCls = env->GetObjectClass(cutout);
                jmethodID sL = cutCls ? env->GetMethodID(cutCls, "getSafeInsetLeft", "()I") : nullptr;
                jmethodID sT = cutCls ? env->GetMethodID(cutCls, "getSafeInsetTop", "()I") : nullptr;
                jmethodID sR =
                    cutCls ? env->GetMethodID(cutCls, "getSafeInsetRight", "()I") : nullptr;
                jmethodID sB =
                    cutCls ? env->GetMethodID(cutCls, "getSafeInsetBottom", "()I") : nullptr;
                if (sL && sT && sR && sB && !env->ExceptionCheck())
                {
                    const float cutL = static_cast<float>(env->CallIntMethod(cutout, sL));
                    const float cutT = static_cast<float>(env->CallIntMethod(cutout, sT));
                    const float cutR = static_cast<float>(env->CallIntMethod(cutout, sR));
                    const float cutB = static_cast<float>(env->CallIntMethod(cutout, sB));
                    if (!env->ExceptionCheck())
                    {
                        pxL = std::max(pxL, cutL);
                        pxT = std::max(pxT, cutT);
                        pxR = std::max(pxR, cutR);
                        pxB = std::max(pxB, cutB);
                    }
                }
                if (cutCls)
                    env->DeleteLocalRef(cutCls);
                env->DeleteLocalRef(cutout);
            }
            if (env->ExceptionCheck())
                env->ExceptionClear();
        }
        if (insetsCls)
            env->DeleteLocalRef(insetsCls);
    }
    if (viewCls)
        env->DeleteLocalRef(viewCls);
    if (view)
        env->DeleteLocalRef(view);
    if (winCls)
        env->DeleteLocalRef(winCls);
    if (win)
        env->DeleteLocalRef(win);
    if (actCls)
        env->DeleteLocalRef(actCls);
    if (insets)
        env->DeleteLocalRef(insets);
    if (!got)
        return failed();
    cL = pxL;
    cT = pxT;
    cR = pxR;
    cB = pxB;
    lastWindow = app->window;
    lastQuery = now;
    have = true;
    left = cL;
    top = cT;
    right = cR;
    bottom = cB;
    return true;
}
#endif
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
    if (cmd == APP_CMD_PAUSE || (cmd == APP_CMD_STOP && !ps2x::hostPause::paused()))
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
#if !defined(PS2X_IOS)
// SET2: flush the draw batch before the panel screenshot (same pattern as above).
extern "C" void rlDrawRenderBatchActive(void);
#endif
#if defined(__APPLE__) && !defined(PS2X_IOS)
// mac-speed-1: manual GLFW decl (no GLFW headers in this TU); defined by the
// raylib-bundled GLFW the runner links.
extern "C" void glfwSwapInterval(int interval);
#endif
#include "ps2_e7.h"
#include "ps2_e15.h"
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
#if defined(__ANDROID__)
#include "ps2_android_env.h" // SET2: settingsEnvFilePath for the panel
#include <cerrno>            // SET2: errno in the settings-write log
#endif

#include <iostream>
#include <stdexcept>
#include <fstream>
#include <algorithm>
#include <array>
#include <utility>
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
#include "ps2_session_telemetry.h" // TEL2
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#if defined(__APPLE__) && !defined(PS2X_IOS)
#include <unistd.h> // SET2: execve for the Mac restart
#include <cerrno>
extern char **environ; // SET2: global scope (an in-function extern mangles)
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

    // GPK1: PS2X_MTVU_GS_BATCH=1 stages the MTVU-GIF thread's GS commands
    // and publishes them under one queue-mutex round (default off; needs
    // the diet's deferred wakes). Read once, at GS setup.
    bool gsBatchRequested()
    {
        const char *env = std::getenv("PS2X_MTVU_GS_BATCH");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }

    // GSB1: PS2X_GS_GIF_BATCH=1 concatenates the MTVU-GIF thread's GIF
    // packets into one GifBatch command per batch (default off; needs the
    // handoff diet's deferred wakes, like GPK1). Read once, at GS setup.
    bool gsGifBatchRequested()
    {
        const char *env = std::getenv("PS2X_GS_GIF_BATCH");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }

    // GSB1: PS2X_GS_GIF_BATCH_BYTES caps one batch (default 65536; fixed,
    // no sweep). Unset/empty/invalid keeps the default; otherwise clamped
    // to [1 KiB, 16 MiB]. Read once, at GS setup.
    size_t gsGifBatchBytes()
    {
        constexpr size_t kDefault = GsWorker::kGifBatchDefaultBytes;
        const char *env = std::getenv("PS2X_GS_GIF_BATCH_BYTES");
        if (!env || !*env)
            return kDefault;
        char *end = nullptr;
        const unsigned long v = std::strtoul(env, &end, 10);
        if (end == env || *end != '\0' || v == 0ul)
            return kDefault;
        size_t out = static_cast<size_t>(v);
        if (out < 1024u)
            out = 1024u;
        if (out > 16u * 1024u * 1024u)
            out = 16u * 1024u * 1024u;
        return out;
    }

    // GSB2: PS2X_GS_GIF_ARENA=1 writes XGKICK/DIRECT payloads and native
    // records once, straight into pooled batch arenas (default off; needs
    // PS2X_GS_GIF_BATCH=1, like GSB1 needs the diet). Read once, at GS setup.
    bool gsGifArenaRequested()
    {
        const char *env = std::getenv("PS2X_GS_GIF_ARENA");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }

    // PKB1: PS2X_PKB=0 restores today's exact path (zero-fill + memcpy,
    // diet-gated pool, one-by-one pops, per-packet releases). Unset or "1"
    // turns the three PKB1 items on. Read once, at GS setup.
    bool pkbRequested()
    {
        const char *env = std::getenv("PS2X_PKB");
        return env == nullptr || std::strcmp(env, "1") == 0;
    }

    // GF1 H4: with the diet, the GS queue holds 8192 descriptors instead of
    // 1024 (the 16 MiB byte cap stays), so the unit runs ahead through the GS
    // worker's GPU waits instead of blocking. Host bound only.
    size_t gsQueueDescriptors()
    {
        if (!gsHandoffDietRequested())
            return 0u;
        return 8192u;
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
    // TK54: the skip above is not "not drawn" on every caller path. In
    // sub_0038B0F8 all four call sites store the returned slot (sh v0,
    // 0xC(patch)) and, with slot < 0, skip only func_38C788 (DMA block
    // build); they still call the per-frame chain append func_38CA70 (sites
    // 0x38BF18/0x38BFA0/0x38C018: slot in a2, `lh 0xC(s0)`) or func_38CE20
    // (site 0x38C0D0: slot in a1). Neither checks the slot: record -1 (12 B
    // before the records) and DMA block -1 (0x100 B before [r+0x440+4*type])
    // go into the frame's VIF1 chain as a CALL to stale bytes. TK54 (Aloha
    // results, t13700-13790): every frame with a type-0 skip released only
    // 10-46 of ~110 masked PATH3 scene uploads (the rest of the scene chain
    // never ran: scenery drops to sky), the leftover uploads sat ahead of the
    // results font upload, and the text then sampled a world texture in the
    // shared streaming slot (TBP 11017). With the guard armed, a negative
    // slot now skips those two calls too, so the patch is just not drawn for
    // the frame, as TK15 intended. Their return values are unused (the
    // loops continue). Stock never pops an empty stack, so a negative slot
    // only exists after a guard skip.
    constexpr uint32_t kSsx3PatchChainAppend = 0x0038CA70u;
    constexpr uint32_t kSsx3PatchChainAppendCE = 0x0038CE20u;
    std::atomic<uint64_t> g_ssx3PatchChainSkips{0u};

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
    // runtime HLE pools and the HNG1 arena (base 0x01F33400),
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
    ps2_ssx3_patch_grow::State g_ssx3PatchGrow;

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

    uint32_t ssx3PatchGrowBase()
    {
        // KNC4: PS2X_SSX3_PATCH_CACHE_GROW_BASE removed (never set); the arena end is the base.
        static const uint32_t base = ps2_hle_pools::kHleArenaBase + ps2_hle_pools::kHleArenaBytes;
        return base;
    }

    bool ssx3PatchCacheGrow(uint8_t *rdram, R5900Context *ctx, uint32_t cache)
    {
        auto &g = g_ssx3PatchGrow;
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
        const uint32_t base = ssx3PatchGrowBase();
        auto al = [](uint32_t v) { return (v + 0xFFu) & ~0xFFu; };
        uint32_t at = base & ~0xFFu;
        auto take = [&](uint32_t bytes) { const uint32_t a = at; at += al(bytes); return a; };
        const uint32_t nRec = take(newCap * 12u), nStk = take(newCap * 4u), nA = take(newCap * 0x1B0u),
                       nB = take(newCap * 0x120u), nC = take(newCap * 0x120u), nDma = take(newCap * 0x100u),
                       nSecBuf = take(newSec * 0x1B0u), nSecStk = take(newSec * 4u);
        const uint32_t lo = base & ~0xFFu, hi = at;
        if (lo < ps2_hle_pools::kHleArenaBase + ps2_hle_pools::kHleArenaBytes || hi > PS2_RAM_SIZE)
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
        auto &g = g_ssx3PatchGrow;
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
        auto &g = g_ssx3PatchGrow;
        if (ssx3PatchGrowCap() == 0u)
            return;
        if (g.active)
            std::fprintf(stderr, "[ssx3-patch-grow] re-init cache=0x%x with %zu blocks unfreed; reset\n",
                         getRegU32(ctx, 4), g.moved.size());
        g = ps2_ssx3_patch_grow::State{};
    }

    bool ssx3PatchCacheGuardOn()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_SSX3_PATCH_CACHE_GUARD");
            const bool v = e && e[0] == '1';
            if (v)
                std::fprintf(stderr, "[ssx3-patch-guard] armed (0x3747A0 returns -1 when a slot/buffer stack it pops is empty; "
                                     "0x38CA70/0x38CE20 skip a negative slot)\n");
            return v;
        }();
        return on;
    }

    bool ssx3PatchChainGuard(R5900Context *ctx, uint32_t targetPc)
    {
        if (!ctx || !ssx3PatchCacheGuardOn())
            return false;
        const int32_t slot = static_cast<int16_t>(getRegU32(ctx, targetPc == kSsx3PatchChainAppend ? 6 : 5) & 0xFFFFu);
        if (slot >= 0)
            return false;
        const uint64_t n = g_ssx3PatchChainSkips.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 4u || (n & (n - 1u)) == 0u)
            std::fprintf(stderr, "[ssx3-patch-guard] chain skip #%llu fn=0x%x slot=%d\n", static_cast<unsigned long long>(n), targetPc, slot);
        return true;
    }

    bool ssx3PatchCacheGuard(uint8_t *rdram, R5900Context *ctx)
    {
        const bool on = ssx3PatchCacheGuardOn();
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
        uint64_t sunk = 0u;
        uint64_t walkCut = 0u;
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
        if (mode == 0 || !rdram || !ctx)
            return;
        int site = -1;
        const uint32_t tb = ssx3DrawTableBase(rdram, ctx, sourcePc, site);
        if (site < 0)
            return;
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
    }

    // At the reset call (a0 = b): the previous frame's final counts.
    void ssx3DrawTableFrame(const uint8_t *rdram, const R5900Context *ctx, uint64_t tick)
    {
        const int mode = ssx3DrawTableMode();
        if (mode != 2 || !rdram || !ctx)
            return;
        Ssx3DrawTableState &st = g_ssx3Draw;
        const uint32_t b = getRegU32(ctx, 4);
        const int32_t tex = ssx3ReadS32(rdram, b + 0x51480u);
        const int32_t pool = ssx3ReadS32(rdram, b + 0x57494u);
        if (pool > kSsx3DrawPoolCap || tex > kSsx3DrawTexCap)
        {
            const uint64_t n = ++st.poolOver;
            if (n <= 4u || (n & (n - 1u)) == 0u)
                std::fprintf(stderr, "[ssx3-drawtable-guard] pool over cap #%llu t=%llu bucket=%d/%d tex=%d/%d\n",
                             static_cast<unsigned long long>(n), static_cast<unsigned long long>(tick),
                             pool, kSsx3DrawPoolCap, tex, kSsx3DrawTexCap);
        }
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

    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    // TK39: 0x01F00000, or 0x01F31000 under PS2X_HLE_POOLS_LOW=1.
    inline uint32_t guestHeapHardLimit() { return ps2_hle_pools::heapCeiling(); }

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

    void copyVu0ContextToStateImpl(const R5900Context *ctx, VuState &state, bool lean)
    {
        if (!lean)
        {
            std::memset(&state, 0, sizeof(state));
        }
        else
        {
            // VZ1: skip the 664-byte clear. Every byte the import assigns is
            // written below; beginProgram() assigns pc/ebit/halt/stopped/top/
            // itop/branch*/vf0 before run() reads them, and run() publishes
            // cycles at the end. Only cycles (observed by the idle path and
            // the census tap) and the struct padding (serialized raw by
            // VuSavestate::saveCore, and covered by the sizeof(VuState)==664
            // format guard) still need zeroing here. Gap bounds derive from
            // offsetof, never from hard-coded byte counts.
            state.cycles = 0;
            char *bytes = reinterpret_cast<char *>(&state);
            constexpr size_t kPad0Begin = offsetof(VuState, stoppedByT) + sizeof(state.stoppedByT);
            constexpr size_t kPad0End = offsetof(VuState, top);
            static_assert(kPad0End > kPad0Begin, "VZ1: VuState tail layout changed (pad0)");
            std::memset(bytes + kPad0Begin, 0, kPad0End - kPad0Begin);
            constexpr size_t kPad1Begin = offsetof(VuState, branchPending) + sizeof(state.branchPending);
            constexpr size_t kPad1End = offsetof(VuState, branchTarget);
            static_assert(kPad1End > kPad1Begin, "VZ1: VuState tail layout changed (pad1)");
            std::memset(bytes + kPad1Begin, 0, kPad1End - kPad1Begin);
            constexpr size_t kTailBegin = offsetof(VuState, branchDelay) + sizeof(state.branchDelay);
            constexpr size_t kTailEnd = sizeof(state);
            static_assert(kTailEnd > kTailBegin, "VZ1: VuState tail layout changed (tail)");
            std::memset(bytes + kTailBegin, 0, kTailEnd - kTailBegin);
        }
        // The import-assigned prefix (vf..status) must stay dense: no padding
        // may hide between two assigned fields, or the lean path above would
        // leave stale bytes where the exact path leaves zeros.
        static_assert(offsetof(VuState, status) + sizeof(VuState::status) == offsetof(VuState, cycles),
                      "VZ1: VuState prefix layout changed; re-audit the lean clear");

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
        if (ps2_guest_exception_transfer::activeContext == ctx)
            throw ps2_guest_exception_transfer::Raised{};
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

}

// VZ1: VU0 lean entry (PS2X_VU0_LEAN_ENTRY=0 restores the exact path;
// default on once gated).
bool PS2Runtime::vu0LeanEntryEnabled()
{
    static const bool enabled = []
    {
        const char *value = std::getenv("PS2X_VU0_LEAN_ENTRY");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

void PS2Runtime::importVu0Context(const R5900Context *ctx, VuState &state, bool lean)
{
    copyVu0ContextToStateImpl(ctx, state, lean);
}

// FH1 full120 clock fix (full120 slice 2: the `ssx3-full120-clock` row,
// its apply function and the interval wrapper moved verbatim to
// games/ssx3/module/src/fh1_clock.cpp).


// BTS1: turbo-boot screen-lookup latch (PS2X_TURBO_BOOT=1, default off;
// knob off wraps nothing). The wrapper reads argument registers and host
// latches only, then calls the original unchanged, so the det-hash cannot
// move. It latches the first 06title screen-table lookup; the turbo yields
// a fixed offset later (or on any pad input, or at the hard cap).
namespace
{
PS2Runtime::RecompiledFunction g_turboBootScreenLookup = nullptr;

void turboBootScreenLookupWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx != nullptr)
    {
        const uint64_t tick =
            runtime ? runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u;
        ps2_turbo_boot::noteScreenLookup(getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6),
                                         getRegU32(ctx, 7), tick);
    }
    if (g_turboBootScreenLookup != nullptr)
        g_turboBootScreenLookup(rdram, ctx, runtime);
}

void applyTurboBoot(PS2Runtime &runtime)
{
    if (!ps2_turbo_boot::enabled())
        return;
    g_turboBootScreenLookup = runtime.lookupFunction(ps2_turbo_boot::kScreenLookupPc);
    if (g_turboBootScreenLookup == nullptr ||
        !runtime.replaceFunction(ps2_turbo_boot::kScreenLookupPc, &turboBootScreenLookupWrapper))
    {
        g_turboBootScreenLookup = nullptr;
        std::fprintf(stderr, "[turbo-boot] refused: no function at 0x397cc0; "
                             "FE.LUI/input/cap latches only\n");
    }
    ps2_turbo_boot::arm();
}
} // namespace
PS2_REGISTER_GAME_OVERRIDE("ssx3-turbo-boot", "SLUS_207.72", 0x00100008u, 0u, applyTurboBoot);

// snd-seam-1 Part 5: the TK11 Tricky-menu override lives in the game module
// now (games/ssx3/module/src/tricky_menu.cpp, row "ssx3-tricky-menu").

// snd-seam-1 Part 5 slice 2: the stock-meter suppression override lives in
// the game module now (games/ssx3/module/src/stock_meter.cpp, row
// "ssx3-tricky-stock-meter").

// tk-audio-2: Tricky-gated stock DJ/PA commentary drops
// (PS2X_SSX3_TRICKY_DJ_GATE=1, tk-audio-1 step 3). The guest posts stock
// radio/PA speech events through the speech-queue post function at 0x2B1458
// (ps2_ssx3_tricky_hud::kSpeechPostFunc): DJ_Artist_Intro (0x20cf),
// PA_Venue_Intro (0x20bb) and DJ_Radio_Big_Intro (0x20e5, gated via the
// header). On Tricky courses the wrapper drops the gated posts before the
// game queues them; the caller observes v0=0 (rejected), the same value the
// queue returns when it cannot accept a post. The venue post is exempt while
// the mode installs an ENGLISH replacement (its content is then the Tricky
// call, detected by alias row, never by course name). Default off: nothing is
// wrapped and every post calls the original (det-identical). Chains with the MC
// translator wrapper in either registration order (predecessor chaining); a
// different ELF (post-func word) refuses and keeps stock speech. tk-meter-1's
// stock-meter suppression (game module row "ssx3-tricky-stock-meter") is untouched.
namespace
{
    PS2Runtime::RecompiledFunction g_tkDjPostPrev = nullptr;
    bool g_tkDjGate = false; // PS2X_SSX3_TRICKY_DJ_GATE=1 (tk-audio-1 step 3)
    uint64_t g_tkDjDropped = 0u;
    bool g_tkVenuePassLogged = false;

    void tkDjPostWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a2 = (ctx != nullptr) ? getRegU32(ctx, 6) : 0u;
        const ps2_ssx3_course::Mode *mode = nullptr;
        if (rdram != nullptr)
        {
            ps2_ssx3_course::Modes &ms = ps2_ssx3_course::courseModes();
            const size_t cur = ms.armed ? ps2_ssx3_course::modeCurrent(ms, rdram) : 0u;
            if (cur != 0u)
                mode = &ms.modes[cur - 1u];
        }
        if (g_tkDjGate && mode != nullptr &&
            ps2_ssx3_tricky_dj::shouldDropPost(a2, mode, true))
        {
            if (ctx != nullptr)
                ctx->r[2] = _mm_set_epi64x(0, 0);
            ++g_tkDjDropped;
            const uint64_t tick = (runtime != nullptr)
                                      ? static_cast<uint64_t>(runtime->memory().gs().vsyncTick.load(
                                            std::memory_order_relaxed))
                                      : 0ull;
            std::fprintf(stderr, "[ssx3-tricky-dj] dropped post a2=0x%x tick=%llu total=%llu\n",
                         a2, static_cast<unsigned long long>(tick),
                         static_cast<unsigned long long>(g_tkDjDropped));
            return;
        }
        if (a2 == ps2_ssx3_tricky_dj::kPaVenueIntroEvent && mode != nullptr &&
            ps2_ssx3_tricky_dj::modeHasVenueAlias(mode) && !g_tkVenuePassLogged)
        {
            g_tkVenuePassLogged = true;
            const uint64_t tick = (runtime != nullptr)
                                      ? static_cast<uint64_t>(runtime->memory().gs().vsyncTick.load(
                                            std::memory_order_relaxed))
                                      : 0ull;
            std::fprintf(stderr, "[ssx3-tricky-dj] venue post passes (ENGLISH alias) tick=%llu\n",
                         static_cast<unsigned long long>(tick));
        }
        if (g_tkDjPostPrev)
            g_tkDjPostPrev(rdram, ctx, runtime);
    }

    void applyTrickyDjGate(PS2Runtime &runtime)
    {
        g_tkDjGate = ps2_ssx3_tricky_dj::gateEnabled();
        if (!g_tkDjGate)
            return;
        constexpr uint32_t kPc = ps2_ssx3_tricky_hud::kSpeechPostFunc;
        constexpr uint32_t kWord = 0x0080582du; // daddu $t3,$a0,$zero @0x2b1458
        const uint8_t *ram = runtime.memory().getRDRAM();
        uint32_t word = 0u;
        if (ram)
            std::memcpy(&word, ram + (kPc & PS2_RAM_MASK), 4u);
        if (!ram || word != kWord)
        {
            std::fprintf(stderr,
                         "[ssx3-tricky-dj] refused: 0x%x reads 0x%08x, expected 0x%08x; stock speech kept\n",
                         kPc, word, kWord);
            return;
        }
        PS2Runtime::RecompiledFunction prev = runtime.lookupFunction(kPc);
        if (!prev || prev == &tkDjPostWrapper)
            return;
        g_tkDjPostPrev = prev;
        if (!runtime.replaceFunction(kPc, &tkDjPostWrapper))
        {
            std::fprintf(stderr, "[ssx3-tricky-dj] cannot wrap 0x%x\n", kPc);
            std::abort();
        }
        std::fprintf(stderr, "[ssx3-tricky-dj] speech post wrapped at 0x%x (djgate=1)\n", kPc);
    }
}

PS2_REGISTER_GAME_OVERRIDE("ssx3-tricky-dj-gate",
                           "SLUS_207.72",
                           0x00100008u,
                           0u,
                           applyTrickyDjGate);

// snd-seam-1 Part 5 slice 3: the draw-distance override lives in the game
// module now (games/ssx3/module/src/lod_scale.cpp, row "ssx3-lod-scale").

// online-v0-1: Quick Play netplay entry (PS2X_NETSYNC=1; see ps2_netsync.h).
// The main-menu Online row (widget "5online", state 0x31) runs the Single
// Event case instead (the TK11 accept rewrite: item+0x18 = 9 before the
// handler reads it) and starts a Quick Play session; a main-menu build ends
// a pre-race one; the Map fill hands the navigator its controller. The
// wrappers chain onto whatever is installed (the Tricky menu wraps the same
// three entries), so either override may apply first. Knob unset: nothing
// is wrapped.
namespace
{
    PS2Runtime::RecompiledFunction g_nsMenuBuild = nullptr;
    PS2Runtime::RecompiledFunction g_nsMenuEvent = nullptr;
    PS2Runtime::RecompiledFunction g_nsMapFill = nullptr;

    void nsMenuBuildWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_netsync::onMainMenuBuild(runtime ? runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u);
        g_nsMenuBuild(rdram, ctx, runtime);
    }

    void nsMenuEventWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        using namespace ps2_ssx3_tricky;
        const uint32_t item = getRegU32(ctx, 5) & 0x1FFFFFFFu;
        if (getRegU32(ctx, 6) == kEventAccept && item && item + 0x40u < PS2_RAM_SIZE &&
            ps2_ssx3_tricky::rd32(rdram, item + kItemWidgetHash) == nameHash("5online"))
        {
            const uint32_t st = ps2_ssx3_tricky::rd32(rdram, item + kItemState);
            const uint64_t tick = runtime ? runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u;
            if (st == kStateOnline && ps2_netsync::onOnlineAccept(tick))
            {
                ps2_ssx3_tricky::wr32(rdram, item + kItemState, kStateSingleEvent);
                ps2_net::firstRunNotice();
                std::fprintf(stderr, "[netsync] main menu: Quick Play -> Single Event (item 0x%06x state 0x%x -> 0x%x)\n",
                             item, st, kStateSingleEvent);
            }
        }
        g_nsMenuEvent(rdram, ctx, runtime);
    }

    void nsMapFillWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_netsync::onMapFill(getRegU32(ctx, 4));
        g_nsMapFill(rdram, ctx, runtime);
    }

    void applyNetsyncOnline(PS2Runtime &runtime)
    {
        using namespace ps2_ssx3_tricky;
        if (!ps2_netsync::knobs().set)
            return;
        const uint8_t *ram = runtime.memory().getRDRAM();
        struct Hook
        {
            uint32_t pc;
            uint32_t word;
            PS2Runtime::RecompiledFunction *orig;
            PS2Runtime::RecompiledFunction wrap;
        };
        const Hook hooks[] = {{kMainMenuItemState, 0x27BDFFD0u, &g_nsMenuBuild, &nsMenuBuildWrapper},
                              {kMainMenuEvent, 0x27BDFFC0u, &g_nsMenuEvent, &nsMenuEventWrapper},
                              {kMapFill, 0x27BDFCC0u, &g_nsMapFill, &nsMapFillWrapper}};
        for (const Hook &h : hooks)
            if (!ram || ps2_ssx3_tricky::rd32(ram, h.pc) != h.word || !(*h.orig = runtime.lookupFunction(h.pc)))
            {
                std::fprintf(stderr, "[netsync] refused: entry 0x%x does not match; Quick Play menu off\n", h.pc);
                return;
            }
        for (const Hook &h : hooks)
            if (!runtime.replaceFunction(h.pc, h.wrap))
            {
                std::fprintf(stderr, "[netsync] cannot wrap 0x%x\n", h.pc);
                std::abort();
            }
        std::fprintf(stderr, "[netsync] armed: Online -> Quick Play (Single Event, %s)\n",
                     ps2_netsync::menuOn() ? "pairing on" : "pairing off at 60 / no server");
    }
}

PS2_REGISTER_GAME_OVERRIDE("ssx3-netsync-online",
                           "SLUS_207.72",
                           0x00100008u,
                           0u,
                           applyNetsyncOnline);

// tk-announcer-1 Part 2: queue-time Tricky MC translator. Separate override
// on the same post function so it chains with the uber-post (and DJ-gate)
// wrappers in either registration order.
namespace
{
PS2Runtime::RecompiledFunction g_mcPostPrev = nullptr;
struct McEmitImpl
{
    PS2Runtime::RecompiledFunction prev = nullptr;
    PS2Runtime *runtime = nullptr;
};
McEmitImpl g_mcEmitImpl;

void setGuestRegU32(R5900Context *ctx, int reg, uint32_t value)
{
    ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
}

// Scheduler-side emitter: one synthesized guest post with no live ctx.
void mcEmitPost(void *impl, uint8_t *rdram, uint32_t a0, uint32_t a2, uint8_t t0, uint64_t tick)
{
    (void)tick;
    auto *e = static_cast<McEmitImpl *>(impl);
    if (e == nullptr || e->prev == nullptr || e->runtime == nullptr || rdram == nullptr)
        return;
    R5900Context synth{};
    setGuestRegU32(&synth, 4, a0);
    setGuestRegU32(&synth, 6, a2);
    setGuestRegU32(&synth, 8, t0);
    e->prev(rdram, &synth, e->runtime);
}

uint64_t mcTickNow(PS2Runtime *runtime)
{
    if (runtime == nullptr)
        return 0ull;
    return static_cast<uint64_t>(runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed));
}

void tkMcPostWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    using namespace ps2_ssx3_tricky_mc;
    const uint32_t a2 = getRegU32(ctx, 6);
    if (g_mcPostPrev == nullptr)
        return;
    // Part 3c: routed posts (race lines, retired stock beds) are classified
    // by the translator; start cues (venue, radio, artist, greeting) have
    // no routes and pass through untouched — only naturals through replaced
    // banks voice.
    ps2_ssx3_course::Modes &ms = ps2_ssx3_course::courseModes();
    if (!ms.armed || ps2_ssx3_course::modeCurrent(ms, rdram) == 0u)
    {
        g_mcPostPrev(rdram, ctx, runtime);
        return;
    }
    const Route *route = findRoute(a2);
    if (route == nullptr)
    {
        g_mcPostPrev(rdram, ctx, runtime);
        return;
    }
    const Rule *rule = findRule(route->mcEvent);
    if (rule == nullptr)
    {
        g_mcPostPrev(rdram, ctx, runtime);
        return;
    }
    const uint64_t tick = mcTickNow(runtime);
    TranslatorState &s = translatorState();
    const uint32_t rr = ps2_ssx3_tricky_hud::resolveChainR(rdram, PS2_RAM_SIZE);
    noteBoundary(s, static_cast<uint64_t>(rr), tick);
    Decision d = decide(s, *route, *rule, tick, false);
    if (d.verdict == Verdict::Drop)
    {
        // Retired stock beds stay silent (v0=0 kills the fetch); the owner
        // function after the post still runs its own logic.
        std::fprintf(stderr, "[ssx3-tricky-mc] tick=%llu a2=0x%x mc=0x%x DROP (silence)\n",
                     static_cast<unsigned long long>(tick), a2, route->mcEvent);
        setReturnU32(ctx, 0u);
        return;
    }
    if (d.outEvent != a2)
        setGuestRegU32(ctx, 6, d.outEvent);
    if (d.outT0 != 0u)
        setGuestRegU32(ctx, 8, d.outT0);
    g_mcPostPrev(rdram, ctx, runtime);
    if (getRegU32(ctx, 2) != 0u)
    {
        noteAccepted(s, route->mcEvent, tick);
        std::fprintf(stderr, "[ssx3-tricky-mc] tick=%llu a2=0x%x -> 0x%x ACCEPT\n",
                     static_cast<unsigned long long>(tick), a2, d.outEvent);
    }
}

void applyTrickyMcPost(PS2Runtime &runtime)
{
    const char *env = std::getenv("PS2X_SSX3_TRICKY_MC");
    if (!env || env[0] != '1')
        return; // Default off: the original runs for every post (det-identical)
    constexpr uint32_t kPc = ps2_ssx3_tricky_hud::kSpeechPostFunc;
    PS2Runtime::RecompiledFunction prev = runtime.lookupFunction(kPc);
    if (prev == nullptr)
        return;
    // Chain: whatever is installed (uber-post wrapper, DJ gate, or the
    // original) becomes our predecessor.
    if (prev == &tkMcPostWrapper)
        return;
    g_mcPostPrev = prev;
    g_mcEmitImpl.prev = prev;
    g_mcEmitImpl.runtime = &runtime;
    runtime.replaceFunction(kPc, &tkMcPostWrapper);
    std::fprintf(stderr, "[ssx3-tricky-mc] translator armed at 0x%x\n", kPc);
    ps2_ssx3_tricky_mc::PollControl &poll = ps2_ssx3_tricky_mc::pollControl();
    poll.armed = true;
    poll.emit = &mcEmitPost;
    poll.emitImpl = &g_mcEmitImpl;
}
} // namespace

PS2_REGISTER_GAME_OVERRIDE("ssx3-tricky-mc",
                           "SLUS_207.72",
                           0x00100008u,
                           0u,
                           applyTrickyMcPost);


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
    // APH1 DEV-ONLY (same class as PS2X_PAD_SCRIPT, default off):
    // PS2X_TK12_CLIP=FROM,TO,STEP also dumps every STEP-th guest tick in
    // [FROM, TO] (one frame per tick) for a review clip.
    static const std::array<unsigned long long, 3> tk12Clip = [] {
        std::array<unsigned long long, 3> c{0ull, 0ull, 0ull};
        if (const char *e = std::getenv("PS2X_TK12_CLIP"))
            if (std::sscanf(e, "%llu,%llu,%llu", &c[0], &c[1], &c[2]) != 3)
                c = {0ull, 0ull, 0ull};
        return c;
    }();
    static uint64_t tk12LastClipTick = ~0ull;
    const bool tk12Take = tk12Clip[2] != 0ull && tick >= tk12Clip[0] && tick <= tk12Clip[1] &&
                          (tick - tk12Clip[0]) % tk12Clip[2] == 0ull && tick != tk12LastClipTick;
    if (tk12Take)
        tk12LastClipTick = tick;
    else if (selectedTicks[0] != 0u)
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

// TK43a: Tricky meter reskin (output-only). Composed into the latched host
// frame before dump/upload, so dumps and presents match. TKL1: consumes the
// EE's immutable packet (tick + epoch) and never touches RDRAM, so the
// det-hash cannot move. Active only on a non-Stock course-manifest mode:
// SSX 3 courses skip untouched.
std::atomic<uint8_t> g_mgx1HudLap{0};
std::atomic<uint64_t> g_mgx1HudTick{0};

// MGX1: small output-only lap badge on the Tricky Megaplex race. The guest
// hook publishes a lap and tick; a stale publication cannot bleed into the
// next course. Digits are drawn into the same final frame as the Tricky meter.
void mgx1LapOverlay(uint8_t *rgba, uint32_t width, uint32_t height, uint64_t tick)
{
    const uint8_t lap = g_mgx1HudLap.load(std::memory_order_relaxed);
    const uint64_t published = g_mgx1HudTick.load(std::memory_order_relaxed);
    if (!rgba || lap < 1u || lap > 3u || tick < published || tick - published > 120u ||
        width < 320u || height < 240u)
        return;
    ps2_ssx3_tricky_layer::PresentationPacket packet;
    if (!ps2_ssx3_tricky_layer::latestPacket(packet) ||
        packet.epoch != ps2_ssx3_tricky_layer::epoch() || !packet.draw)
        return;
    const int scale = std::max(1, static_cast<int>(width / 640u));
    const int x0 = static_cast<int>(width / 2u) - 55 * scale;
    const int y0 = 48 * scale;
    auto pixel = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) return;
        uint8_t *p = rgba + (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4u;
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };
    for (int y = 0; y < 25 * scale; ++y)
        for (int x = 0; x < 110 * scale; ++x)
            pixel(x0 + x, y0 + y, (y < 2 * scale || y >= 23 * scale) ? 66 : 14,
                  (y < 2 * scale || y >= 23 * scale) ? 176 : 20,
                  (y < 2 * scale || y >= 23 * scale) ? 216 : 32);
    const char label[8] = {'L', 'A', 'P', ' ', static_cast<char>('0' + lap), '/', '3', '\0'};
    auto glyph = [](char c) -> const uint8_t * {
        static constexpr uint8_t L[7] = {16,16,16,16,16,16,31};
        static constexpr uint8_t A[7] = {14,17,17,31,17,17,17};
        static constexpr uint8_t P[7] = {30,17,17,30,16,16,16};
        static constexpr uint8_t one[7] = {4,12,4,4,4,4,14};
        static constexpr uint8_t two[7] = {14,17,1,2,4,8,31};
        static constexpr uint8_t three[7] = {30,1,1,14,1,1,30};
        static constexpr uint8_t slash[7] = {1,2,2,4,8,8,16};
        switch (c) { case 'L': return L; case 'A': return A; case 'P': return P;
                     case '1': return one; case '2': return two; case '3': return three;
                     case '/': return slash; default: return nullptr; }
    };
    for (int i = 0; i < 7; ++i)
    {
        const uint8_t *rows = glyph(label[i]);
        if (!rows) continue;
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 5; ++x)
                if (rows[y] & (1u << (4 - x)))
                    for (int yy = 0; yy < 2 * scale; ++yy)
                        for (int xx = 0; xx < 2 * scale; ++xx)
                            pixel(x0 + (11 + i * 13 + x * 2) * scale + xx,
                                  y0 + 5 * scale + y * 2 * scale + yy, 240, 246, 252);
    }
}

void trickyHudOverlay(PS2Runtime *rt, uint8_t *rgba, uint32_t width, uint32_t height, uint64_t tick)
{
    // TKL1: the GL call site. The state decision is the layer's per-VBlank
    // packet, shared with the VK/AHB call site (queuePendingAhb); the atlas
    // pointer outlives the lock (the atlas is immutable once ok). A stale
    // (pre-load/pre-exit) packet is skipped.
    if (!rt || !rgba || width == 0u || height == 0u)
        return;
    ps2_ssx3_tricky_layer::PresentationPacket p;
    if (!ps2_ssx3_tricky_layer::config().hud || !ps2_ssx3_tricky_layer::latestPacket(p))
        return;
    if (p.epoch != ps2_ssx3_tricky_layer::epoch())
        return;
    if (!p.draw || !p.atlas)
        return;
    ps2_ssx3_tricky_hud::composeOverlay(rgba, static_cast<int>(width), static_cast<int>(height), *p.atlas,
                                       p.fill, p.full, tick, p.splashUntil, p.litLetters, p.flashUntil);
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
#elif defined(__ANDROID__)
    // android-touch-1: phones without a controller need touch controls, so
    // the overlay defaults on like iOS (the Touch panel row forces on/off,
    // persisted as PS2X_VIRTUAL_PAD). Visibility is decided per frame
    // below (auto-hides when a gamepad is present).
    return ps2x::vpad::enabledFromEnv(env);
#else
    return env && env[0] == '1'; // desktop: dev-only, the mouse is the finger
#endif
}

// android-touch-1: any physical gamepad present (no press needed). Auto
// mode shows the overlay iff this is false, so the Odin (built-in
// controller) defaults to hidden while the A14 (no controller) shows it.
#if defined(__ANDROID__)
bool gamepadConnected()
{
    for (int i = 0; i < 4; ++i)
    {
        if (IsGamepadAvailable(i))
            return true;
    }
    return false;
}
#endif

// A connected controller hides the overlay once it has been used (any
// button, or a stick past half-way) since it connected. The iOS Simulator
// reports an always-connected device named "Gamepad" with nothing attached,
// so "connected" alone would hide the overlay for good (see
// iosRealControllerPresent below for the connected-but-unused rule).
bool gamepadInUse()
{
    static bool s_used = false;
    // ctrl-harden-1: a stale raylib slot (Android disconnect, OS pad gone)
    // must not stick "used" on: no device, no use.
    if (ps2xOsGamepadStaleGone())
    {
        s_used = false;
        return false;
    }
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

#if defined(PS2X_IOS)
// ctrl-harden-1: a connected REAL controller hides the overlay immediately
// (Touch = Auto), before its first press. The Simulator's fake
// always-connected "Gamepad" never hides on its own (first real press
// still hides via gamepadInUse, as before).
bool iosRealControllerPresent()
{
    for (int i = 0; i < 4; ++i)
    {
        if (!IsGamepadAvailable(i))
            continue;
        if (ps2x::ctrlmap::hidesOverlay(GetGamepadName(i)))
            return true;
    }
    return false;
}
#endif

int virtualPadTouches(ps2x::vpad::TouchPoint *ts, int max, float screenWidth, float screenHeight)
{
#if defined(PS2X_IOS)
    // IN4: a connected-but-unused controller still leaves the overlay path
    // live (gamepadInUse needs a press first); the merge drops multi-peer
    // (tap-pair) records while any controller is connected, so Brad's
    // two-finger marker never reaches the guest.
    bool controllerConnected = false;
    for (int i = 0; i < 8 && !controllerConnected; ++i)
        controllerConnected = IsGamepadAvailable(i);
    int64_t ids[8];
    float xs[8], ys[8];
    const int n = ps2x::ios::touchPoints(ids, xs, ys, max < 8 ? max : 8, controllerConnected);
    for (int i = 0; i < n; ++i)
    {
        ts[i].id = ids[i];
        ts[i].x = xs[i] * screenWidth;
        ts[i].y = ys[i] * screenHeight;
    }
    return n;
#elif defined(__ANDROID__)
    // Android: the touchscreen drives the overlay (vpad) on
    // Android; the mouse fallback below never fires on a phone. Positions
    // are pixels, matching the mouse branch.
    (void)screenWidth;
    (void)screenHeight;
    if (max < 1)
        return 0;
    int n = GetTouchPointCount();
    if (n > max)
        n = max;
    for (int i = 0; i < n; ++i)
    {
        const Vector2 p = GetTouchPosition(i);
        ts[i].id = GetTouchPointId(i);
        ts[i].x = p.x;
        ts[i].y = p.y;
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

// IN4: publish to the IN2 latch, noting host-word transitions for the input
// diagnostics log (PS2X_INPUT_DIAG=1; transition only, zero cost off).
void publishPad(uint16_t mask, uint64_t tick)
{
    ps2x::inputdiag::notePublish(mask, tick);
    ps2x::padlatch::sharedLatch().publish(mask);
}

// ctrl-harden-1: publish pad 2's mask to its own latch (second controller
// only; the render loop passes zeros while the panel, the Tricky list or
// the background pause owns the pad). No per-frame diag line for port 1.
void publishPadPort1(uint16_t mask, uint64_t /*tick*/)
{
    ps2x::padlatch::sharedLatchPort1().publish(mask);
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

// np-rider-1 (SRVP1 P0b candidate): drive one AI rider from a tick-stamped
// remote script. Diagnostic knobs, all default off. At the 0x111728 pre-call
// boundary (called from 0x121068 after the rider's own source provider has
// produced its two-word command), the target AI slot's 8-byte record is
// replaced by the script entry for this vsync tick. The AI provider keeps
// running as a shadow every tick; reversion is tick-stamped. Never touches
// vtable, flags, roster slot, controller, source descriptor or NPC state;
// no ctl+0xDE8 swap, no players[0] write. Reset (9), finish (10) and 13 are
// never replaced. Knob off: this code is unreachable (DSP1 filter lists
// 0x111728 only when enabled) and guest state is untouched.
// np-fix-1 (F7/F8): rider script state lives in ps2_np_rider.h (verbatim move
// of the namespace below in the B0 scaffold; strictness + generation added by
// the fix). Kept here as a comment so the move is reviewable as behavior-neutral.

#if defined(__ANDROID__) || defined(PS2X_IOS)
// ASV1: the choice is made before the game thread starts, so a rejected or
// "Start fresh" slot cannot advance the guest even one tick. Touch and pad
// input both work here; Resume is the selected/default action.
bool asv1AskToResume(bool &closed)
{
    closed = false;
    bool resumeSelected = true;
    int lastTouchId = -1;
    std::fprintf(stderr, "[autosave] prompt: Resume where you left off / Start fresh (default Resume)\n");
    while (!WindowShouldClose())
    {
        if (ps2x::hostPause::paused())
        {
            PollInputEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        const int w = GetScreenWidth(), h = GetScreenHeight();
        const int buttonW = std::min(w / 3, 430), buttonH = std::max(64, h / 10);
        const int gap = std::max(24, w / 32);
        const int x = (w - 2 * buttonW - gap) / 2, y = h / 2;
        const Rectangle resumeBox{static_cast<float>(x), static_cast<float>(y),
                                  static_cast<float>(buttonW), static_cast<float>(buttonH)};
        const Rectangle freshBox{static_cast<float>(x + buttonW + gap), static_cast<float>(y),
                                 static_cast<float>(buttonW), static_cast<float>(buttonH)};
        if (IsKeyPressed(KEY_LEFT))
            resumeSelected = true;
        if (IsKeyPressed(KEY_RIGHT))
            resumeSelected = false;
        if (IsKeyPressed(KEY_ENTER) || IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            return resumeSelected;
        if (GetTouchPointCount() > 0)
        {
            const int id = GetTouchPointId(0);
            if (id != lastTouchId)
            {
                const Vector2 p = GetTouchPosition(0);
                if (CheckCollisionPointRec(p, resumeBox))
                    return true;
                if (CheckCollisionPointRec(p, freshBox))
                    return false;
            }
            lastTouchId = id;
        }
        else
            lastTouchId = -1;
        BeginDrawing();
        ClearBackground(Color{19, 25, 36, 255});
        const char *title = "Resume where you left off?";
        const int titleSize = std::max(28, h / 18);
        DrawText(title, (w - MeasureText(title, titleSize)) / 2, h / 3, titleSize, RAYWHITE);
        DrawRectangleRec(resumeBox, resumeSelected ? Color{62, 132, 91, 255} : Color{48, 61, 74, 255});
        DrawRectangleRec(freshBox, resumeSelected ? Color{48, 61, 74, 255} : Color{62, 132, 91, 255});
        const int labelSize = std::max(20, h / 28);
        const char *resume = "Resume", *fresh = "Start fresh";
        DrawText(resume, x + (buttonW - MeasureText(resume, labelSize)) / 2,
                 y + (buttonH - labelSize) / 2, labelSize, RAYWHITE);
        DrawText(fresh, x + buttonW + gap + (buttonW - MeasureText(fresh, labelSize)) / 2,
                 y + (buttonH - labelSize) / 2, labelSize, RAYWHITE);
        EndDrawing();
    }
    closed = true;
    return false;
}
#endif

// ASV1: the launch/background autosave slot path (host-only). iOS Documents
// wins (PS2X_IOS_DOCUMENTS, set by the iOS launcher: the bundle elfDirectory
// is read-only); elsewhere it sits next to the memory card, never under
// elfDirectory (on Mac det boots that is the pinned CD tree, PI1 read-only).
// The leaf is fixed ("autosave.ps2xstate"); the pure rule is unit-tested in
// ps2_virtual_pad.h so the Android on-disk path cannot drift.
std::string autosaveSlotPath()
{
    const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
    return ps2x::vpad::autosaveSlotPathFor(std::getenv("PS2X_IOS_DOCUMENTS"), paths.mcRoot.string(),
                                           paths.elfDirectory.string());
}

} // namespace

// SET2: in-game settings overlay (design B). Knob PS2X_SETTINGS_OVERLAY=1
// (default off in code; on in play envs once Brad approves). Select+Start
// (Select-first, ~150 ms window) pauses the guest through the hostPause gate
// and opens a host-drawn panel (Mode / Resolution / Perf overlay /
// On-device upscale). Touch: hold SELECT + tap START on the overlay, or
// two-finger-tap with no overlay up (set2-polish-1: no gear dot).
// Guest-invisible: the panel owns the pad while
// open (publish-zero) and draws only over the presented frame.
namespace
{
bool set2OverlayOn()
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_SETTINGS_OVERLAY");
        return v && std::string(v) == "1";
    }();
    return on;
}

// True while the panel holds the pause gate (so BG1's resume path doesn't
// steal it when the app backgrounds with the panel open).
std::atomic<bool> &set2HoldsPause()
{
    static std::atomic<bool> holds{false};
    return holds;
}

// PS2X_SET2_TEST_PAD=start:mask:dur,... (diag, host-frame addressed):
// ORs mask (hex, PS2 button bits) into the sampled pad on host frames
// [start, start+dur). The only way to drive the chord on headless Mac boots.
struct Set2TestSpan
{
    uint64_t start = 0;
    uint64_t dur = 0;
    uint16_t mask = 0;
};

std::vector<Set2TestSpan> set2TestSpans()
{
    static const std::vector<Set2TestSpan> spans = [] {
        std::vector<Set2TestSpan> out;
        const char *env = std::getenv("PS2X_SET2_TEST_PAD");
        if (!env || !env[0])
            return out;
        const char *p = env;
        while (*p)
        {
            char *end = nullptr;
            const unsigned long long start = std::strtoull(p, &end, 10);
            if (end == p || *end != ':')
                break;
            p = end + 1;
            const unsigned long mask = std::strtoul(p, &end, 0);
            if (end == p || *end != ':')
                break;
            p = end + 1;
            const unsigned long long dur = std::strtoull(p, &end, 10);
            if (end == p || mask > 0xffffu || dur == 0u)
                break;
            out.push_back(Set2TestSpan{start, dur, static_cast<uint16_t>(mask)});
            p = (*end == ',') ? end + 1 : end;
            if (*p == '\0')
                break;
            if (*(p - 1) != ',')
                break;
        }
        std::fprintf(stderr, "[set2] test-pad spans=%zu (%s)\n", out.size(), env);
        return out;
    }();
    return spans;
}

uint16_t set2TestMask(const std::vector<Set2TestSpan> &spans, uint64_t hostFrame)
{
    uint16_t mask = 0u;
    for (const Set2TestSpan &s : spans)
    {
        if (hostFrame >= s.start && hostFrame < s.start + s.dur)
            mask = static_cast<uint16_t>(mask | s.mask);
    }
    return mask;
}

// The user settings layer (read last at boot; written by the panel).
// Android: <files dir>/settings.env. iOS: <Documents>/settings.env. The Mac
// has no settings file: Mode/Resolution apply through the relaunch env.
std::string set2SettingsPath()
{
#if defined(__ANDROID__)
    // This TU cannot see PS2X_DEFAULT_BOOT_ELF (PRIVATE on ps2EntryRunner);
    // the accessor lives in the TU that can (ps2_android_runtime.cpp).
    return ps2x::settingsEnvFilePath();
#elif defined(PS2X_IOS)
    if (const char *docs = std::getenv("PS2X_IOS_DOCUMENTS"); docs && docs[0] != '\0')
        return std::string(docs) + "/settings.env";
    return std::string();
#else
    return std::string();
#endif
}

ps2x::set2::SettingsPairs set2ReadSettings()
{
    const std::string path = set2SettingsPath();
    if (path.empty())
        return {};
    std::ifstream file(path);
    if (!file.is_open())
        return {};
    std::ostringstream content;
    content << file.rdbuf();
    return ps2x::set2::parseSettingsEnv(content.str());
}

// Atomic write (tmp + rename) of the owned keys. False when there is no
// settings file on this platform or the write fails.
bool set2WriteSettings(const ps2x::set2::SettingsPairs &pairs)
{
    const std::string path = set2SettingsPath();
    if (path.empty())
    {
        std::fprintf(stderr, "[set2] settings write: no path on this platform\n");
        return false;
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file.is_open())
        {
            std::fprintf(stderr, "[set2] settings write: cannot open %s (%s)\n", tmp.c_str(),
                         std::strerror(errno));
            return false;
        }
        file << ps2x::set2::formatSettingsEnv(pairs);
        file.flush();
        if (!file.good())
        {
            std::fprintf(stderr, "[set2] settings write: failed writing %s\n", tmp.c_str());
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        std::fprintf(stderr, "[set2] settings write: rename failed (%s)\n", ec.message().c_str());
        return false;
    }
    std::fprintf(stderr, "[set2] settings written %s (%zu keys)\n", path.c_str(), pairs.size());
    return true;
}

bool set2Effective120()
{
#if defined(PS2X_IOS)
    return ps2x::ios::target120();
#elif defined(__ANDROID__)
    const char *hz = std::getenv("PS2X_LAUNCH_HZ");
    return hz && std::string(hz) == "120";
#else
    const char *v = std::getenv("PS2X_SSX3_FULL120");
    return v && std::string(v) == "events";
#endif
}

#if defined(__ANDROID__)
// Relaunch through the matching launcher alias (.Launch120/.Launch60),
// then kill this process (the Java side does both; modelled on UX1's toast
// JNI: attach, resolve via the activity ClassLoader, clear exceptions).
bool set2AndroidRelaunch(const char *target)
{
    struct android_app *app = GetAndroidApp();
    if (!app || !app->activity || !app->activity->vm)
        return false;
    JNIEnv *env = nullptr;
    JavaVMAttachArgs args = {JNI_VERSION_1_6, nullptr, nullptr};
    if (app->activity->vm->AttachCurrentThread(&env, &args) != JNI_OK || !env)
        return false;
    bool launched = false;
    jobject act = app->activity->clazz;
    jclass actCls = env->GetObjectClass(act);
    jmethodID getLoader =
        actCls ? env->GetMethodID(actCls, "getClassLoader", "()Ljava/lang/ClassLoader;") : nullptr;
    jobject loader = (actCls && getLoader) ? env->CallObjectMethod(act, getLoader) : nullptr;
    jclass loaderCls = loader ? env->GetObjectClass(loader) : nullptr;
    jmethodID loadClass = loaderCls ? env->GetMethodID(loaderCls, "loadClass",
                                                       "(Ljava/lang/String;)Ljava/lang/Class;")
                                    : nullptr;
    jstring className = loadClass ? env->NewStringUTF("com.ps2x.runner.Set2Relaunch") : nullptr;
    jclass relaunchCls =
        (loader && className) ? static_cast<jclass>(env->CallObjectMethod(loader, loadClass, className))
                              : nullptr;
    jmethodID toHz = relaunchCls ? env->GetStaticMethodID(relaunchCls, "toHz",
                                                           "(Landroid/app/Activity;Ljava/lang/String;)V")
                                 : nullptr;
    jstring jtarget = (relaunchCls && toHz) ? env->NewStringUTF(target) : nullptr;
    if (jtarget && !env->ExceptionCheck())
    {
        env->CallStaticVoidMethod(relaunchCls, toHz, act, jtarget);
        launched = !env->ExceptionCheck(); // on success the process dies inside toHz
    }
    if (env->ExceptionCheck())
        env->ExceptionClear();
    if (jtarget)
        env->DeleteLocalRef(jtarget);
    if (className)
        env->DeleteLocalRef(className);
    if (relaunchCls)
        env->DeleteLocalRef(relaunchCls);
    if (loaderCls)
        env->DeleteLocalRef(loaderCls);
    if (loader)
        env->DeleteLocalRef(loader);
    if (actCls)
        env->DeleteLocalRef(actCls);
    return launched;
}
#endif

} // namespace

#if defined(__APPLE__) && !defined(PS2X_IOS)
namespace
{
int g_set2Argc = 0;
char **g_set2Argv = nullptr;
} // namespace

// Stashed by main() so the panel's Restart can re-exec the runner.
void ps2x::set2::set2NoteExecArgs(int argc, char **argv)
{
    g_set2Argc = argc;
    g_set2Argv = argv;
}

namespace
{
// Re-exec the runner with Mode/Resolution applied through the env (the Mac
// has no settings file). Returns false when exec fails (caller stays open).
bool set2MacRestart(bool want120, const char *upscaleOrNull)
{
    if (g_set2Argc < 1 || !g_set2Argv || !g_set2Argv[0])
        return false;
    std::vector<std::string> keep;
    for (char **e = environ; e && *e; ++e)
    {
        const std::string entry(*e);
        // Fresh boot: new Mode/Resolution, and the diag test spans
        // + shot must not fire again (else the reboot would re-open and
        // loop).
        if (entry.compare(0, 18, "PS2X_SSX3_FULL120=") == 0 || entry.compare(0, 12, "GE1_UPSCALE=") == 0 ||
            entry.compare(0, 19, "PS2X_SET2_TEST_PAD=") == 0 ||
            entry.compare(0, 15, "PS2X_SET2_SHOT=") == 0)
            continue;
        keep.push_back(entry);
    }
    keep.push_back(want120 ? "PS2X_SSX3_FULL120=events" : "PS2X_SSX3_FULL120=");
    if (upscaleOrNull && upscaleOrNull[0] != '\0')
        keep.push_back(std::string("GE1_UPSCALE=") + upscaleOrNull);
    std::vector<char *> envp;
    for (std::string &s : keep)
        envp.push_back(s.data());
    envp.push_back(nullptr);
    std::fprintf(stderr, "[set2] mac re-exec %s (120=%d upscale=%s)\n", g_set2Argv[0],
                 want120 ? 1 : 0, upscaleOrNull && upscaleOrNull[0] ? upscaleOrNull : "(default)");
    // Flush: exec keeps thefds but a buffered line could be lost on failure paths.
    std::fflush(nullptr);
    execve(g_set2Argv[0], g_set2Argv, envp.data());
    std::fprintf(stderr, "[set2] mac re-exec failed: %s\n", std::strerror(errno));
    return false;
}
} // namespace
#endif

// HR1: main-thread present cost split, reported by PS2X_THREAD_CPU_LOG=1.
static std::atomic<uint64_t> g_hr1LatchNs{0}, g_hr1UploadNs{0}, g_hr1Uploads{0};
#if defined(PS2X_IOS)
// HR1 spike (PS2X_PRESENT_ZERO_COPY=1 on iOS): the shared GLES texture to draw
// instead of frameTex; id 0 = use frameTex.
static Texture2D g_hr1ShareTex{};

// PSO1 (PRV1 §3): presenter side of the shared-surface ownership. Main
// (GL) thread only. The witness records the producer write count of the
// slot whose pixels are being shown; a change before the read is submitted
// (both modes) or before its fence completes (ownership) is an unsafe reuse.
namespace pso1
{
struct Stress
{
    uint32_t everyN = 0u;   // delay every Nth newly acquired frame (0 = off)
    uint32_t delayMs = 0u;  // after acquisition, before the quad is submitted
    uint32_t retireEvery = 1u; // poll read fences every Nth host iteration
};
static Stress stressFromEnv()
{
    Stress st;
    // PS2X_PRESENT_OWN_STRESS=<everyN>:<delayMs>[:<retireEvery>]
    if (const char *v = std::getenv("PS2X_PRESENT_OWN_STRESS"))
    {
        unsigned a = 0u, b = 0u, c = 1u;
        if (std::sscanf(v, "%u:%u:%u", &a, &b, &c) >= 2)
        {
            st.everyN = a;
            st.delayMs = b;
            st.retireEvery = c == 0u ? 1u : c;
        }
    }
    return st;
}
static const Stress &stress()
{
    static const Stress st = stressFromEnv();
    return st;
}
static int g_shownSlot = -1;          // slot of the frame being drawn (witness)
static uint64_t g_shownWrites = 0u;   // its producer write count at acquisition
static uint64_t g_shownSeq = 0u;
static bool g_shownNew = false;       // this iteration acquired a new frame
static uint64_t g_shownTick = 0u;
static uint64_t g_newAcquires = 0u;
static uint64_t g_pendingSlotFence = 0u; // last read's fence (ownership)
static int g_pendingSlot = -1;
static uint64_t g_pendingWrites = 0u;
static uint64_t g_unsafeAtDraw = 0u;  // slot rewritten between acquire and draw
static uint64_t g_unsafeAtRetire = 0u;// slot rewritten before the read's fence completed
static uint64_t g_stressDelays = 0u;
static uint64_t g_iter = 0u;
static uint64_t g_lastLogNs = 0u;

static bool logOn()
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_PRESENT_OWN_LOG");
        if (v && *v)
            return std::strcmp(v, "1") == 0;
        return ps2x_present_share::ownershipEnabled() || stress().everyN != 0u;
    }();
    return on;
}

static void noteShown(int slot, uint64_t seq, uint64_t tick)
{
    g_shownNew = (seq != g_shownSeq) || slot != g_shownSlot;
    if (g_shownNew)
    {
        g_shownSlot = slot;
        g_shownSeq = seq;
        g_shownTick = tick;
        g_shownWrites = ps2x_present_own::witnessWrites(slot);
        ++g_newAcquires;
    }
}

// Poll read fences (ownership only) and retire the slots they cover.
static void pollRetire()
{
    if (!ps2x_present_share::ownershipEnabled())
        return;
    ++g_iter;
    if (stress().retireEvery > 1u && (g_iter % stress().retireEvery) != 0u)
        return;
    const uint64_t done = ps2x_present_share::pollReadFences();
    if (g_pendingSlot >= 0 && done >= g_pendingSlotFence)
    {
        if (ps2x_present_own::witnessWrites(g_pendingSlot) != g_pendingWrites)
            ++g_unsafeAtRetire;
        g_pendingSlot = -1;
    }
    ps2x_present_own::sharedPool().retireReadsThrough(done, ps2x::perflog::steadyNs());
}

// Before the shared quad is submitted (stress delay + draw-time witness).
static void beforeDraw()
{
    if (g_shownNew && stress().everyN != 0u && (g_newAcquires % stress().everyN) == 0u)
    {
        ++g_stressDelays;
        std::this_thread::sleep_for(std::chrono::milliseconds(stress().delayMs));
    }
}

// After the shared quad's batch flush: draw-time witness, read fence.
static void afterDraw()
{
    if (g_shownSlot >= 0 && ps2x_present_own::witnessWrites(g_shownSlot) != g_shownWrites)
        ++g_unsafeAtDraw;
    if (!ps2x_present_share::ownershipEnabled())
        return;
    const uint64_t fence = ps2x_present_share::submitReadFence();
    ps2x_present_own::sharedPool().noteRead(fence, ps2x::perflog::steadyNs());
    g_pendingSlot = g_shownSlot;
    g_pendingSlotFence = fence;
    g_pendingWrites = g_shownWrites;
}

static void maybeLog(bool force = false)
{
    if (!logOn())
        return;
    const uint64_t now = ps2x::perflog::steadyNs();
    if (!force && now - g_lastLogNs < 10'000'000'000ull)
        return;
    g_lastLogNs = now;
    const ps2x_present_own::Counters c = ps2x_present_own::sharedPool().counters();
    const ps2x_present_share::FenceStats f = ps2x_present_share::fenceStats();
    std::vector<uint64_t> produce, readyAge, hold, readLat;
    for (const ps2x_present_own::FrameTimes &t : ps2x_present_own::sharedPool().times())
    {
        if (t.completeNs > t.reserveNs && t.reserveNs)
            produce.push_back(t.completeNs - t.reserveNs);
        if (t.acquireNs > t.completeNs && t.completeNs)
            readyAge.push_back(t.acquireNs - t.completeNs);
        if (t.freeNs > t.acquireNs && t.acquireNs)
            hold.push_back(t.freeNs - t.acquireNs);
        if (t.freeNs > t.lastReadNs && t.lastReadNs)
            readLat.push_back(t.freeNs - t.lastReadNs);
    }
    auto us = [](uint64_t ns) { return static_cast<unsigned long long>(ns / 1000u); };
    using ps2x_present_own::percentile;
    std::fprintf(stderr,
                 "[present-own] own=%d gen=%llu reserves=%llu noslot=%llu ok=%llu failed=%llu stale=%llu ooo=%llu "
                 "superseded=%llu acquires=%llu repeats=%llu reads=%llu retired=%llu retired_now=%llu "
                 "pool_unsafe=%llu unsafe_draw=%llu unsafe_retire=%llu new_frames=%llu stress_delays=%llu "
                 "fences=%llu/%llu pending=%llu fence_fail=%llu finish=%llu overflow_finish=%llu apple_sync=%d "
                 "produce_us p50=%llu max=%llu ready_age_us p50=%llu max=%llu hold_us p50=%llu p99=%llu "
                 "read_retire_us p50=%llu p99=%llu max=%llu n=%zu\n",
                 ps2x_present_share::ownershipEnabled() ? 1 : 0, (unsigned long long)c.generation,
                 (unsigned long long)c.reserves, (unsigned long long)c.noSlot, (unsigned long long)c.completesOk,
                 (unsigned long long)c.completesFailed, (unsigned long long)c.staleGen,
                 (unsigned long long)c.outOfOrder, (unsigned long long)c.superseded,
                 (unsigned long long)c.acquires, (unsigned long long)c.repeats, (unsigned long long)c.reads,
                 (unsigned long long)c.retired, (unsigned long long)c.retiredImmediate,
                 (unsigned long long)c.unsafe, (unsigned long long)g_unsafeAtDraw,
                 (unsigned long long)g_unsafeAtRetire, (unsigned long long)g_newAcquires,
                 (unsigned long long)g_stressDelays, (unsigned long long)f.completed,
                 (unsigned long long)f.submitted, (unsigned long long)f.pending, (unsigned long long)f.failures,
                 (unsigned long long)f.finishFallbacks, (unsigned long long)f.overflowFinishes, f.appleSync ? 1 : 0,
                 us(percentile(produce, 0.5)), us(percentile(produce, 1.0)), us(percentile(readyAge, 0.5)),
                 us(percentile(readyAge, 1.0)), us(percentile(hold, 0.5)), us(percentile(hold, 0.99)),
                 us(percentile(readLat, 0.5)), us(percentile(readLat, 0.99)), us(percentile(readLat, 1.0)),
                 produce.size());
}

// Diagnostic (PS2X_PRESENT_CAPTURE_TICKS=a,b,c + PS2X_PRESENT_CAPTURE_DIR),
// tick-locked (PSO1 Part 2): the capture gate (ps2_present_share.h) stops
// exports around the pending target T, so the shown frame settles on T with
// no newer export reserved. Read the final drawable back before the swap
// after glFinish; the slot's producer write count must still equal the one
// at acquisition (else the file is tagged -unsafe). A target that is not
// shown within 500 ms of the gate engaging (its export was dropped) is
// logged as missed and skipped.
static void maybeCapture()
{
    static const char *dir = std::getenv("PS2X_PRESENT_CAPTURE_DIR");
    const uint64_t target = ps2x_present_share::captureGatePending();
    if (!dir || !*dir || target == 0u || g_shownSlot < 0)
        return;
    if (g_shownTick == target)
    {
        rlDrawRenderBatchActive();
        ps2x_present_share::finishGl();
        const bool safe = ps2x_present_own::witnessWrites(g_shownSlot) == g_shownWrites;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = std::string(dir) + "/cap-t" + std::to_string(target) + (safe ? "" : "-unsafe") +
                                 ".ppm";
        ps2x_present_share::captureDrawable(GetRenderWidth(), GetRenderHeight(), path.c_str());
        std::fprintf(stderr, "[present-own] capture tick=%llu safe=%d slot=%d seq=%llu\n",
                     (unsigned long long)target, safe ? 1 : 0, g_shownSlot, (unsigned long long)g_shownSeq);
        ps2x_present_share::captureGateDone(target);
        return;
    }
    const uint64_t since = ps2x_present_share::captureGateBlockedSinceNs();
    if (g_shownTick > target || (since != 0u && ps2x::perflog::steadyNs() - since > 500'000'000ull))
    {
        std::fprintf(stderr, "[present-own] capture tick=%llu missed shown=%llu\n", (unsigned long long)target,
                     (unsigned long long)g_shownTick);
        ps2x_present_share::captureGateDone(target);
    }
}
} // namespace pso1
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

#if defined(__ANDROID__) || defined(PS2X_IOS)
namespace
{
// BG1: the live runtime for the pause hook (set in initialize(), before the
// game thread starts; the hook runs on the main thread). ASV1: the same gate
// now serves the iOS willResignActive/DidEnterBackground observers.
PS2Runtime *g_bg1Runtime = nullptr;
std::atomic<bool> g_bg1GuestStarted{false};

void bg1OnPause()
{
    // ASV1: save while the EE can still reach the scheduler's safe point.
    // BG1's gate closes only after this bounded wait. A slow or deferred
    // save leaves the previous atomic slot intact.
    const char *autosave = std::getenv("PS2X_AUTOSAVE");
    if (g_bg1GuestStarted.load(std::memory_order_acquire) && !ps2x::hostPause::paused() &&
        autosave && std::strcmp(autosave, "1") == 0)
    {
        const std::string path = autosaveSlotPath();
        if (ps2_savestate::requestAutoSave(path))
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
            while (ps2_savestate::quickSavePending() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (ps2_savestate::quickSavePending())
                std::fprintf(stderr, "[autosave] background deadline: previous slot retained\n");
        }
    }
    if (!ps2x::hostPause::requestPause())
        return;
    const uint64_t tick =
        g_bg1Runtime ? g_bg1Runtime->eeScheduler().currentVSyncTick() : 0u;
    ps2_snd_audio_output::pausePlayback();
    // Bounded: a game thread stuck mid-frame must not hang the app-cmd pump.
    const bool acked = !g_bg1GuestStarted.load(std::memory_order_acquire) ||
                       ps2x::hostPause::waitGateAck(2000);
    if (g_bg1Runtime)
        g_bg1Runtime->gs().flushExternalCaches();
    // PT2 Part 2a: kill-proof the stage rings (a swipe from the switcher is a
    // SIGKILL; the shutdown dump never runs). Once per pause (requestPause
    // above is the guard), main thread, lock-free ring reads — the gated game
    // thread never stalls on it. Bounded: entries since the last flush (the
    // 60 s timer bounds it) into a fresh tail-pause file.
    ps2x::perflog::flushTail("pause");
    // IN4: input-diagnostics ring flush (same kill-proofing; no-op unless on).
    ps2x::inputdiag::flush("pause");
    // PR3: the pad recorder's open span + stdio buffer (same kill-proofing).
    ps2_stubs::padRecordFlushNow("pause");
    // TEL2: coverage dump + session line while the game thread is gated.
    ps2x::tel::onAppPause(tick);
    std::fprintf(stderr, "[bg1] paused at tick=%llu gate_acked=%d\n",
                 static_cast<unsigned long long>(tick), acked ? 1 : 0);
}

void bg1OnResume()
{
    if (set2HoldsPause().load(std::memory_order_acquire))
    {
        // SET2: the panel holds the pause (backgrounded while open); the
        // guest stays frozen until the panel closes.
        std::fprintf(stderr, "[bg1] resume skipped: the settings panel holds the pause\n");
        return;
    }
    if (!ps2x::hostPause::requestResume())
        return;
    // The gate wakes the game thread first; the ring still holds its
    // pause-time fill, so the resumed stream drains real frames, no gap.
    ps2_snd_audio_output::resumePlayback();
    const uint64_t tick =
        g_bg1Runtime ? g_bg1Runtime->eeScheduler().currentVSyncTick() : 0u;
    std::fprintf(stderr, "[bg1] resumed at tick=%llu\n", static_cast<unsigned long long>(tick));
    ps2x::tel::onAppResume(tick); // TEL2
}
} // namespace
#endif

#if defined(PS2X_IOS)
namespace ps2x::ios
{
// ASV1: the iOS shell's lifecycle hooks (ps2_ios_runtime.mm observes
// UIApplicationWillResignActive / DidEnterBackground / DidBecomeActive). The
// autosave and the BG1 pause gate are the same path as Android's
// APP_CMD_PAUSE/RESUME; a second background notification finds the guest
// already paused and coalesces.
void onAppBackground()
{
    bg1OnPause();
}

void onAppForeground()
{
    bg1OnResume();
}
} // namespace ps2x::ios
#endif

static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    // TKL1: no RDRAM publication (the TK43e publishRdram is gone): the GL
    // and VK/AHB overlay call sites consume the EE's immutable packet.
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
        // PL3: the latched frame is this iteration's presented frame (the
        // latch wall stands in for its guest-vsync wall for frame age).
        ps2x::perflog::noteFrameAvailable(currentTick, ps2x::perflog::steadyNs(), 0u);
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
                outWidth = s_lastWidth = shared.width;
                outHeight = s_lastHeight = shared.height;
                s_hasUploadedFrame = true;
                // PL3: guest tick through the mailbox (see SharedFrame).
                ps2x::perflog::noteFrameAvailable(shared.tick, shared.submitWallNs, shared.seq);
                return;
            }
        }
    }
#endif
#if defined(PS2X_IOS)
    if (ps2x_present_share::enabled() && ps2x_present_share::ownershipEnabled())
    {
        // PSO1: retire completed reads, then atomically acquire the newest
        // READY frame (or repeat CURRENT). The slot stays reserved until a
        // newer frame replaces it and its last read's fence completes.
        pso1::pollRetire();
        pso1::maybeLog();
        ps2x_present_own::FrameInfo info;
        int slot = -1;
        if (ps2x_present_own::sharedPool().acquire(&info, &slot, ps2x::perflog::steadyNs()) !=
            ps2x_present_own::Acquire::None)
        {
            const ps2x_present_share::SharedFrame shared{info.surface, info.width,       info.height, info.seq,
                                                         info.tick,    info.submitWallNs, slot};
            const unsigned int id = ps2x_present_share::acquireTexture(shared);
            if (id != 0u)
            {
                g_hr1ShareTex = Texture2D{id, static_cast<int>(shared.width), static_cast<int>(shared.height), 1,
                                          PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                pso1::noteShown(slot, shared.seq, shared.tick);
                outWidth = s_lastWidth = shared.width;
                outHeight = s_lastHeight = shared.height;
                s_hasUploadedFrame = true;
                // PL3: guest tick through the mailbox (see SharedFrame).
                ps2x::perflog::noteFrameAvailable(shared.tick, shared.submitWallNs, shared.seq);
                return;
            }
            ps2x_present_own::sharedPool().dropCurrent(ps2x::perflog::steadyNs());
        }
        g_hr1ShareTex = Texture2D{};
        pso1::g_shownSlot = -1;
    }
    else if (ps2x_present_share::enabled())
    {
        pso1::maybeLog();
        ps2x_present_share::SharedFrame shared;
        if (ps2x_present_share::latest(shared))
        {
            const unsigned int id = ps2x_present_share::acquireTexture(shared);
            if (id != 0u)
            {
                pso1::noteShown(shared.slot, shared.seq, shared.tick);
                g_hr1ShareTex = Texture2D{id, static_cast<int>(shared.width), static_cast<int>(shared.height), 1,
                                          PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                outWidth = s_lastWidth = shared.width;
                outHeight = s_lastHeight = shared.height;
                s_hasUploadedFrame = true;
                // PL3: guest tick through the mailbox (see SharedFrame).
                ps2x::perflog::noteFrameAvailable(shared.tick, shared.submitWallNs, shared.seq);
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
        // I26: black while no guest frame exists.
        static const ps2x::FallbackRgba s_fallback = ps2x::fallbackFrameColor();
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
        trickyHudOverlay(rt, s_scratch.data(), width, height, currentTick); // TK43a: output-only reskin
        mgx1LapOverlay(s_scratch.data(), width, height, currentTick);
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

namespace ps2_ssx3_patch_grow
{
Layout layout() { return {ssx3PatchGrowBase(), ssx3PatchGrowCap()}; }
State snapshot() { return g_ssx3PatchGrow; }
bool valid(const State &state)
{
    if (state.moved.size() > 8u || (state.active != !state.moved.empty()) ||
        (state.active && (state.cache == 0u || state.refused)))
        return false;
    for (size_t i = 0; i < state.moved.size(); ++i)
    {
        const auto [to, from] = state.moved[i];
        const uint32_t phys = to & PS2_RAM_MASK;
        if (phys < (ssx3PatchGrowBase() & ~0xFFu) || phys >= PS2_RAM_SIZE ||
            (from & PS2_RAM_MASK) >= ps2_hle_pools::heapCeiling())
            return false;
        for (size_t j = 0; j < i; ++j)
            if (state.moved[j].first == to)
                return false;
    }
    return true;
}
bool restore(const State &state)
{
    if (!valid(state))
        return false;
    g_ssx3PatchGrow = state;
    return true;
}
void reset() { g_ssx3PatchGrow = State{}; }
} // namespace ps2_ssx3_patch_grow

PS2Runtime::PS2Runtime()
{
    ps2_ssx3_patch_grow::reset();
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
    // DSP1: PS2X_EE_DISPATCH_FAST (default on; 0 = the exact paths).
    // SPR1: split120 (the only mode with half-mode restarts) is retired,
    // so the fast checkpoint follows the knob on every path.
    ps2_dsp1::g_fast = ps2_dsp1::knobFromEnv();
    ps2_dsp1::g_fastCheckpoint = ps2_dsp1::g_fast;
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);

    m_eeScheduler = std::make_unique<EeScheduler>(*this);

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
    m_hleArenaBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(guestHeapHardLimit(), PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    // P1f: keep invocation stacks in kernel-reserved low RAM ([0x80000,
    // 0x100000)), below the ELF image and guest heap and far from every
    // guest thread stack (which grows down from the RAM top).
    m_asyncCallbackStackFloor = ps2_hle_pools::callbackStackFloor();
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
    ps2_fh1_jcam2::lazyShutdown();
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

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModule(path, arguments, argumentSize);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->stopModule(moduleId, result);
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
    // HLE IOP mode: every bind gets a server descriptor, as before upstream #244.
    return m_hleIopMode || m_iopSubsystem->canBindRpc(sid);
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

void PS2Runtime::advanceIopEeCycles(uint64_t eeCycles) noexcept
{
    if (m_hleIopMode)
        return;
    m_iopSubsystem->runEeCycles(eeCycles);
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
    // RBF1: HLE mode keeps the pre-rebase unconditional core-service routing
    // (MCSERV, dbcman, libsd); emulator mode keeps the module gate. Every
    // reset path runs through here (init, ELF load, run(), SifInitRpc), so
    // the mode cannot be lost by a transport reset.
    m_iopSubsystem->setHleCoreServices(m_hleIopMode);
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    return m_iopSubsystem ? m_iopSubsystem->allocateMemory(size, alignment) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    return m_iopSubsystem && m_iopSubsystem->freeMemory(address);
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->readMemory(address, destination, size);
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->writeMemory(address, source, size);
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->zeroMemory(address, size);
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->isMemoryRange(address, size);
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
    // HDL1: PS2X_GS_BACKEND=headless installs the no-raster referee
    // backend. The frontend still decodes every packet for guest-visible
    // state; only rasterization, texture upload, VRAM maintenance,
    // presents and the window are skipped. Every other value (including
    // unset) keeps the previous backend selection exactly.
    if (ps2x_gs_headless::requested())
    {
        m_gs.setRasterBackend(ps2x_gs_headless::create());
        std::cerr << "[gs:headless] no-raster backend selected (PS2X_GS_BACKEND=headless)" << std::endl;
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
                                                const bool note = m_gs.rawGifBackendActive();
                                                m_gs.processGIFPacketWithPath(path, note, bytes);
                                            });
        // H3: on the unit thread, wake the GS worker once per 64 queued
        // commands (or wakeBytes) and at every unit job end, instead of once
        // per GIF drain (one futex wake per XGKICK).
        const uint32_t wakeCmds = 64u;
        const size_t wakeBytes = 256u * 1024u;
        m_gs.setWorkerDeferredWakes(wakeCmds, wakeBytes);
        ps2_mtvu::jobEndFn() = [this]() { m_gs.flushWorkerWake(); };
        // GPK1: staged publish from the GIF stage (same commands, same
        // order, same admission; one queue-mutex round per stage).
        if (gsBatchRequested())
        {
            GsWorker::setStagedPublish(true);
            ps2_mtvu::gifPublishFn() = []() { GsWorker::flushStaged(); };
            std::cerr << "[gs:handoff] GPK1 staged publish on (PS2X_MTVU_GS_BATCH=1)" << std::endl;
        }
        // GSB1: GIF batch command from the GIF stage (same packets, same
        // order, one command per batch; supersedes GPK1 staging on the
        // batching thread when both are on).
        const bool gifBatch = gsGifBatchRequested();
        if (gifBatch)
        {
            const size_t batchBytes = gsGifBatchBytes();
            GsWorker::setGifBatch(true, batchBytes);
            ps2_mtvu::gifPublishFn() = []()
            {
                GsWorker::flushStaged();
                GsWorker::flushGifBatch();
            };
            std::cerr << "[gs:handoff] GSB1 GIF batch on (PS2X_GS_GIF_BATCH=1, bytes="
                      << batchBytes << ")" << std::endl;
        }
        // GSB2: zero-copy GIF batches (same packets, same order, views into
        // pooled arenas instead of a second copy). Needs the batch above;
        // without it the knob stays inert.
        if (gsGifArenaRequested())
        {
            if (!gifBatch)
            {
                std::cerr << "[gs:handoff] GSB2 GIF arena inert (PS2X_GS_GIF_ARENA=1 needs "
                             "PS2X_GS_GIF_BATCH=1)"
                          << std::endl;
            }
            else
            {
                m_gifArbiter.setArenaPool(GsGifArenaPool::create());
                m_gifArbiter.setProcessViewFn([this](GifPathId path, GsGifArenaRef arena,
                                                     uint32_t off, uint32_t len)
                                              {
                                                  const bool note = m_gs.rawGifBackendActive();
                                                  m_gs.processViewWithPath(path, note, std::move(arena),
                                                                           off, len);
                                              });
                GsWorker::setGifArena(true);
                std::cerr << "[gs:handoff] GSB2 GIF arena on (PS2X_GS_GIF_ARENA=1)" << std::endl;
            }
        }
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
    // PKB1 (PS2X_PKB=1, default on once gated; 0 = today's exact path):
    // MTVU/GS packet buffer ownership. Item 1: no zero-fill before a full
    // copy (arbiter submit + staged copy). Item 2: pooled per-packet buffers
    // (lifetime ends at the GS worker's release; copies only, so live guest
    // or VU memory is never aliased across its next write). Item 3: batched
    // worker pops + batched pool releases per wake. Pure refactor: same
    // bytes in the same order (det IDENTICAL); FIFO, RPC and JobEnd
    // boundaries and the JobEnd/Drain/Call flush points are untouched.
    // Independent of the diet block above (re-running its pool/pop-batch
    // setters is idempotent).
    if (pkbRequested())
    {
        m_gifArbiter.setNoZeroFill(true);
        m_gs.setPacketPoolEnabled(true);
        m_gifArbiter.setPacketPool(&m_gs.packetPool());
        m_gs.setWorkerPopBatch(GsWorker::kPopBatch);
        m_gs.setReleaseBatching(true);
        std::cerr << "[gs:pkb] on (PS2X_PKB=1): no-zero-fill copies, pooled packet buffers, pop batch="
                  << GsWorker::kPopBatch << ", batched releases" << std::endl;
    }
    // The listener runs before each packet's process call (same thread,
    // synchronous drain), so draws kicked while processing land on this
    // packet's path.
    m_gifArbiter.setPacketListener([this, handoffDiet](GifPathId path, uint32_t size)
                                   {
                                       (void)size;
                                       // GB3 Part 2: a raw-GIF backend needs every packet's path.
                                       // GF1 H1: with the diet the path rides in the packet command.
                                       if (!handoffDiet && m_gs.rawGifBackendActive())
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
    ps2_mtvu::configure(ps2_e7::enabled() || ps2x_gs_capture::enabled() || ps2_e41_trace::armed());
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
                                             // NRT1: a natively served static-world job skips VU1.
                                             if (ps2_native_world::beforeVu1(m_memory, startPC, top, itop))
                                                 return;
                                             // OM1: a MISS falls through to the static restart below.
                                             const bool served = ps2_microvu::run(m_memory, m_memory.getVU1Data(), m_vu1.state(),
                                                                                  startPC, false, top, itop, fbrst, 65536);
                                             ps2_native_world::afterVu1();
                                             if (served)
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
                                                              // NRT1: a natively served static-world job skips VU1.
                                                              if (ps2_native_world::beforeVu1(m_memory, startPC, top, itop))
                                                                  return;
                                                              // OM1: a MISS restarts statically here; the
                                                              // post-sync VPU_STAT update below is shared.
                                                              const bool served = ps2_microvu::run(m_memory, m_memory.getVU1Data(),
                                                                                                   m_vu1.state(), startPC, false,
                                                                                                   top, itop, fbrst, 65536);
                                                              ps2_native_world::afterVu1();
                                                              if (!served)
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
                                             ps2_native_world::onResume(); // NRT1: counted, expected never
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
                                                              ps2_native_world::onResume(); // NRT1
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
        // TEL2: coverage map + session log (knobs off = nothing allocated);
        // before the game thread starts.
        ps2x::tel::init(g_ps2RecompiledFunctionTableBase, g_ps2RecompiledFunctionTableSlotCount);
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
        // HDL1: headless referee opens no window (PS2X_GS_BACKEND=headless).
        // IsWindowReady() stays false, so teardown skips CloseWindow.
        if (ps2x_gs_headless::requested())
            std::cerr << "[gs:headless] no window (PS2X_GS_BACKEND=headless)" << std::endl;
        else
            InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
#endif
#if defined(PS2X_IOS)
        ps2x::ios::syncWindowSize();
        SetTraceLogLevel(LOG_ERROR);
        // ASV1: register the runtime for the iOS background pause hook (the
        // observers installed in main() call ps2x::ios::onAppBackground()).
        g_bg1Runtime = this;
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
        // NP0A: exercise 60/120 host present cadence on the Mac without
        // changing the ordinary desktop loop. Guest VBlank remains paced by
        // the scheduler; this only changes the raylib present loop.
        // mac-speed-1: FULL120 events also presents at displayHz (uncaps the
        // 60 host loop that gated every Air cell at 60.00); stock unchanged.
        SetTargetFPS((ps2_savestate::canonicalEnabled() || set2Effective120())
                         ? ps2x_present_vk::displayHz()
                         : 60);
        // mac-speed-1 Part 2 (PS2X_LAB_UNCAPPED=1, lab-only): drop the host
        // present waits (raylib limiter, GL vsync swap) so unpaced lab
        // boots measure true headroom. Default off; guest state untouched.
        if (ps2_vsync_pacer::labUncappedFromEnv(std::getenv("PS2X_LAB_UNCAPPED")))
        {
            SetTargetFPS(0);
#if defined(__APPLE__) && !defined(PS2X_IOS)
            glfwSwapInterval(0);
#endif
        }
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
            const uint32_t hardLimit = std::min(guestHeapHardLimit(), PS2_RAM_SIZE);
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
        m_asyncCallbackStackFloor = ps2_hle_pools::callbackStackFloor();
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
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    m_iopSubsystem->reset();

    m_hleIopMode = false;
    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);
    // KNC4: PS2X_IOP_MODE override removed (unset since IOPE1, 10-02); the game override above decides.
    std::fprintf(stderr, "[iop-mode] %s\n", m_hleIopMode ? "hle" : "emulator");
    // RBF1: the ELF-load reset above predates the mode decision; assert the
    // HLE core-service routing for the decided mode (run() re-asserts it via
    // resetIop() on every boot, as do SifInitRpc resets).
    m_iopSubsystem->setHleCoreServices(m_hleIopMode);

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
    ps2x::tel::noteOverride(address); // TEL2: init-time record for the coverage dump
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
    {
        std::lock_guard<std::mutex> lock(m_coverageMutex);
        ++m_unknownSyscallCounts[id];
    }
    ps2x::tel::noteUnknownSyscall(id); // TEL2
}

void PS2Runtime::noteUnhandledRpc(uint32_t sid, uint32_t function)
{
    {
        std::lock_guard<std::mutex> lock(m_coverageMutex);
        ++m_unhandledRpcCounts[(static_cast<uint64_t>(sid) << 32u) | function];
    }
    ps2x::tel::noteUnhandledRpc(sid, function); // TEL2
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
    ps2x::tel::noteMissingFunction(targetPc, sourcePc); // TEL2
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);
    const bool shouldPrint = policy == MissingFunctionPolicy::Stop || firstReport;

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

// DSP1: dispatch fast path (PS2X_EE_DISPATCH_FAST, default on; 0 = exact path only).
//
// dispatchGuestBranchFull below is the exact path. In the common case
// (no split mode, census, park or diag period; FH1 hooks in HK1 table mode;
// no draw/spatial restore pending; neither PC listed by any hook; a generated
// function at the target), it performs only these steps: an HK1 table miss
// (no side effects, HK1 census §1), no target hook match, ctx->pc = target,
// checkpointDue(kGuestDispatchCycles), then for calls the table function,
// the FH11 post-call fix when armed, and the stop/unwind/fall-through checks.
// The lean front does exactly those steps and hands every other case to the
// full path before any mutation, so charges, service points, hook calls and
// order, binding reads and resume behaviour are unchanged.
//
// "Listed by any hook" is a 4096-bit filter per side (source, target), a
// superset of every PC the dispatch-side hooks compare against: the target
// hooks below (patch cache, guest free, draw table/pool, spatial guard, draw
// reset, widescreen) and, with FH1 reachable, HK1's interest table for the
// current guest-active state. A filter hit runs the full path (exact).
namespace
{
constexpr uint32_t kDspFilterBits = 4096u;

inline uint32_t dspBit(uint32_t pc) noexcept
{
    return (pc >> 2) & (kDspFilterBits - 1u);
}

struct DspState
{
    int armed = -1;           // -1: not computed, 0: full path only, 1: lean front
    bool fh1 = false;         // FH1 onBranch reachable (enabled() || tapOn())
    bool always = false;      // HK1 always-run (diagnostic taps): full path
    bool guestActive = false; // ps2_fh1::g_guestActive the filter was built for
    uint64_t src[kDspFilterBits / 64u] = {};
    uint64_t tgt[kDspFilterBits / 64u] = {};
};
DspState g_dsp;

inline bool dspTest(const uint64_t *bits, uint32_t pc) noexcept
{
    const uint32_t i = dspBit(pc);
    return ((bits[i >> 6] >> (i & 63u)) & 1u) != 0u;
}

inline void dspSet(uint64_t *bits, uint32_t pc) noexcept
{
    const uint32_t i = dspBit(pc);
    bits[i >> 6] |= uint64_t{1} << (i & 63u);
}

__attribute__((noinline, cold)) void dspBuildFilter()
{
    std::fill(std::begin(g_dsp.src), std::end(g_dsp.src), 0u);
    std::fill(std::begin(g_dsp.tgt), std::end(g_dsp.tgt), 0u);
    // dispatchGuestBranchFull's own target hooks (call kinds only; listed for
    // every kind, a superset).
    for (uint32_t pc : {kSsx3PatchCacheAlloc, kSsx3PatchChainAppend, kSsx3PatchChainAppendCE, kSsx3GuestFree, kSsx3PatchCacheInit, kSsx3DrawKeyCall,
                        kSsx3DrawLookup, kSsx3DrawTexLookup, kSsx3SpatialItems12, kSsx3SpatialItems34,
                        kSsx3SpatialInside1, kSsx3SpatialInside3, kSsx3DrawReset, 0x00228C08u,
                        0x001216E0u, 0x00117838u}) // TK45c gems
        dspSet(g_dsp.tgt, pc);
    const char *megaLaps = std::getenv("PS2X_SSX3_MEGA_LAPS");
    if (megaLaps && std::strcmp(megaLaps, "1") == 0)
    {
        dspSet(g_dsp.tgt, 0x00125108u); // finish
        dspSet(g_dsp.tgt, 0x00112FB0u); // rider route progress
    }
    // np-rider-1: route the 0x111728 input-consumer call through the full
    // (exact) path only while the remote-script knob is on, so the hook
    // above sees every per-rider call. Knob off leaves the filter untouched.
    const char *npRider = std::getenv("PS2X_NP_RIDER_ENABLE");
    if (npRider && std::strcmp(npRider, "1") == 0)
        dspSet(g_dsp.tgt, 0x00111728u);
    // np-link-1: the same input-consumer call carries the commit-gated
    // remote drive. Knob off leaves the filter untouched.
    const char *npLink = std::getenv("PS2X_NP_LINK_ENABLE");
    if (npLink && std::strcmp(npLink, "1") == 0)
        dspSet(g_dsp.tgt, 0x00111728u);
    // Quick Play netplay: input-consumer hook for the ghost drive.
    // Knob off leaves the filter untouched.
    {
        const char *sync = std::getenv("PS2X_NETSYNC");
        if (sync && (std::strcmp(sync, "1") == 0 || std::strcmp(sync, "on") == 0))
            dspSet(g_dsp.tgt, 0x00111728u);
    }
    g_dsp.always = false;
    g_dsp.guestActive = ps2_fh1::g_guestActive;
    if (g_dsp.fh1)
    {
        // The same table ps2_fh1::onBranch consults (same build trigger).
        const ps2_fh1::HookInterest &hi = ps2_fh1::activeHookInterest();
        g_dsp.always = hi.always;
        for (uint32_t pc : hi.src)
            dspSet(g_dsp.src, pc);
        for (uint32_t pc : hi.tgt)
            dspSet(g_dsp.tgt, pc);
    }
}

__attribute__((noinline, cold)) bool dspArm()
{
    bool on = !PS2X_ENABLE_DIAG_TAPS; // lookupFunction's dispatch history
    // KNC1P2 B3 deleted the TS2 counting diag (PS2X_TS2_HALFLOAD_COUNT,
    // PS2X_TS2_CASE_COUNT): counting is unconditionally off. SPR1 deleted
    // the split-mode product check with split120.
    on = on && diagPeriodMs() == 0u;
    g_dsp.fh1 = ps2_fh1::enabled() || ps2_fh1::tapOn();
    on = on && (!g_dsp.fh1 || ps2_fh1::hookPreclassify()); // chain mode runs every hook
    if (on)
        dspBuildFilter();
    g_dsp.armed = on ? 1 : 0;
    return on;
}

// The generated function the lean front may call, or nullptr for the full
// path. Pure apart from the filter (re)build.
inline PS2Runtime::RecompiledFunction dspFastTarget(uint32_t targetPc, uint32_t sourcePc)
{
    if (g_dsp.armed != 1 && (g_dsp.armed == 0 || !dspArm()))
        return nullptr;
    if (g_ssx3DrawPending.load(std::memory_order_relaxed) || g_ssx3SpatialPending.load(std::memory_order_relaxed))
        return nullptr;
    if (g_dsp.fh1 && g_dsp.guestActive != ps2_fh1::g_guestActive)
        dspBuildFilter(); // guestFlip: HK1 rebuilds its table on the same trigger
    if (g_dsp.always || dspTest(g_dsp.tgt, targetPc) || dspTest(g_dsp.src, sourcePc))
        return nullptr;
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(targetPc, slot))
        return nullptr;
    ps2x::tel::coverSlot(slot); // TEL2 (one relaxed load when off)
    return g_ps2RecompiledFunctionTable[slot];
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
    if (m_fh32Preview)
    {
        if (!ctx || !m_fh32PreviewBudget || m_fh32PreviewFailed)
        { m_fh32PreviewFailed = true; return false; }
        --m_fh32PreviewBudget;
        ctx->pc = targetPc;
        if (kind == GuestBranchKind::Return) return true;
        if (kind == GuestBranchKind::DirectJump || kind == GuestBranchKind::IndirectJump)
            return false; // private trampoline resumes the destination
        if (kind != GuestBranchKind::DirectCall && kind != GuestBranchKind::IndirectCall)
        { m_fh32PreviewFailed = true; return false; }
        RecompiledFunction preview = lookupFunction(targetPc);
        if (!preview) { m_fh32PreviewFailed = true; return false; }
        preview(rdram, ctx, this);
        if (m_fh32PreviewFailed || ctx->pc != fallthroughPc) return false;
        return true;
    }
    RecompiledFunction fn = nullptr;
    if (!ps2_dsp1::g_fast || !m_eeScheduler || (fn = dspFastTarget(targetPc, sourcePc)) == nullptr)
    {
        return dispatchGuestBranchFull(rdram, ctx, targetPc, sourcePc, fallthroughPc, kind, debugName);
    }
    ctx->pc = targetPc;
    bool due;
    PS2X_DSP1_INLINE_CALL due = m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles);
    if (due)
    {
        ps2_fh1::postUnwindRestore(rdram);
        markGuestUnwind();
        return false;
    }
    if (kind != GuestBranchKind::DirectCall && kind != GuestBranchKind::IndirectCall)
    {
        return false; // ctx->pc == targetPc, as the full path leaves it
    }
    fn(rdram, ctx, this);
    if (ps2_fh1::g_postArmed || ps2_fh1::g_pubArmed || ps2_fh1::g_normcapArmed) // NRM1: normcap shares it too
    {
        ps2_fh1::onReturn(rdram, ctx, targetPc, !isStopRequested() && ctx->pc != 0u && !guestUnwindPending());
    }
    if (isStopRequested() || ctx->pc == 0u || guestUnwindPending())
    {
        return false;
    }
    if (ctx->pc == targetPc)
    {
        ctx->pc = fallthroughPc;
    }
    return ctx->pc == fallthroughPc;
}

__attribute__((noinline)) bool PS2Runtime::dispatchGuestBranchFull(uint8_t *rdram,
                                                                   R5900Context *ctx,
                                                                   uint32_t targetPc,
                                                                   uint32_t sourcePc,
                                                                   uint32_t fallthroughPc,
                                                                   GuestBranchKind kind,
                                                                   const char *debugName)
{
    // Quick Play netplay: race start, STATE, ghost show (no-op unless PS2X_NETSYNC=1).
    ps2_netsync::onTick(rdram, m_memory.gs().vsyncTick.load(std::memory_order_relaxed));
    // online-stock-2: stock online race state sync (no-op unless PS2X_NET_STOCK120=1).
    ps2_net_stock120::onTick(rdram, m_memory.gs().vsyncTick.load(std::memory_order_relaxed));
    struct Mgx1LapSlot { uint32_t rider = 0u; uint32_t clock = 0u; uint32_t ignored = 0u; uint8_t lap = 1u; bool beforeCheckpoint = false; bool armed = false; bool reseed = false; bool finished = false; };
    static Mgx1LapSlot mgx1Laps[6];
    static const bool mgx1MegaLaps = [] {
        const char *v = std::getenv("PS2X_SSX3_MEGA_LAPS");
        return v && std::strcmp(v, "1") == 0;
    }();
    // TK18's imported host checkpoint: donor event 11 at 28736.9 becomes
    // 0.30817997 of host track 0 (51285.99 units), or 15805.3 units.
    if (targetPc == 0x112fb0u && mgx1MegaLaps && ssx3ReadS32(rdram, 0x535c08u) == 15 &&
        ps2_ssx3_course::modeCurrent(ps2_ssx3_course::courseModes(), rdram) != 0u &&
        ps2_ssx3_course::rdStr(rdram, ps2_ssx3_course::kEventBase + 15u * ps2_ssx3_course::kEventStride + 68u,
                                16u) == "MEGA")
    {
        const uint32_t r = getRegU32(ctx, 4) & PS2_RAM_MASK;
        if (r != 0u && r + 0x870u < PS2_RAM_SIZE)
        {
            const uint32_t slot = static_cast<uint32_t>(ssx3ReadS32(rdram, r + 0x86cu));
            if (slot < 6u)
            {
                Mgx1LapSlot &ls = mgx1Laps[slot];
                if (ls.rider != r) ls = Mgx1LapSlot{};
                uint32_t raceClock = 0u;
                const uint32_t b = ps2_ssx3_tricky_hud::resolveChainB(rdram, PS2_RAM_SIZE);
                if (b != 0u && ps2_ssx3_tricky_hud::readGuestU32(rdram, PS2_RAM_SIZE, b, 0xcu, raceClock))
                {
                    if (raceClock < ls.clock) ls = Mgx1LapSlot{};
                    ls.clock = raceClock;
                }
                ls.rider = r;
                if (slot == 0u)
                {
                    g_mgx1HudLap.store(ls.finished ? 0u : ls.lap, std::memory_order_relaxed);
                    g_mgx1HudTick.store(m_memory.gs().vsyncTick.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
                if (ls.reseed)
                {
                    ls.reseed = false;
                    targetPc = 0x112180u;
                    SET_GPR_U32(ctx, 5, 1u);
                    std::fprintf(stderr, "[mgx1-lap] t=%llu slot=%u route reattach after placement\n",
                                 (unsigned long long)m_memory.gs().vsyncTick.load(std::memory_order_relaxed), slot);
                }
                else if (!ls.armed)
                {
                    float rem = 0.0f, origin = 0.0f;
                    std::memcpy(&rem, rdram + r + 0x4d4u, 4);
                    std::memcpy(&origin, rdram + r + 0x4d8u, 4);
                    const float progressed = origin - rem;
                    if (std::isfinite(progressed) && progressed >= -1000.0f && progressed < 15805.3f)
                        ls.beforeCheckpoint = true;
                    else if (ls.beforeCheckpoint && std::isfinite(progressed) &&
                             progressed >= 15805.3f && progressed < 35000.0f)
                    {
                        ls.armed = true;
                        ls.beforeCheckpoint = false;
                        std::fprintf(stderr, "[mgx1-lap] t=%llu slot=%u donor-checkpoint-11 armed progress=%.1f\n",
                                     (unsigned long long)m_memory.gs().vsyncTick.load(std::memory_order_relaxed), slot, progressed);
                    }
                }
            }
        }
    }
    // MGX1 proof: Megaplex runs three laps. On the first two race-line
    // crossings, place the rider at the course's own rolling start region.
    // The event finish is left intact for the third crossing. This knob is
    // default-off and requires the active Tricky mode and MEGA event row.
    if (targetPc == 0x125108u && sourcePc == 0x10e70cu &&
        (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall))
    {
        if (mgx1MegaLaps && ssx3ReadS32(rdram, 0x535c08u) == 15 &&
            ps2_ssx3_course::modeCurrent(ps2_ssx3_course::courseModes(), rdram) != 0u &&
            ps2_ssx3_course::rdStr(rdram, ps2_ssx3_course::kEventBase + 15u * ps2_ssx3_course::kEventStride + 68u,
                                    16u) == "MEGA")
        {
            const uint32_t r = getRegU32(ctx, 4) & PS2_RAM_MASK;
            const uint32_t slot = r && r + 0x878u < PS2_RAM_SIZE ? static_cast<uint32_t>(ssx3ReadS32(rdram, r + 0x86cu)) : 6u;
            uint32_t clock = 0u;
            const uint32_t b = ps2_ssx3_tricky_hud::resolveChainB(rdram, PS2_RAM_SIZE);
            if (b != 0u)
                ps2_ssx3_tricky_hud::readGuestU32(rdram, PS2_RAM_SIZE, b, 0xcu, clock);
            if (slot < 6u && r + 0x4dcu < PS2_RAM_SIZE)
            {
                Mgx1LapSlot &ls = mgx1Laps[slot];
                if (ls.rider != r || clock < ls.clock)
                    ls = Mgx1LapSlot{};
                ls.rider = r;
                ls.clock = clock;
                if (!ls.armed)
                {
                    ctx->pc = fallthroughPc;
                    if (ls.ignored++ < 3u || ls.ignored % 120u == 0u)
                        std::fprintf(stderr, "[mgx1-lap] t=%llu slot=%u finish ignored before checkpoint n=%u\n",
                                     (unsigned long long)m_memory.gs().vsyncTick.load(std::memory_order_relaxed), slot, ls.ignored);
                    return true;
                }
                ls.armed = false;
                if (ls.lap < 3u)
                {
                    ++ls.lap;
                    ls.beforeCheckpoint = false;
                    ls.reseed = true;
                    // SSX 3's region placement handles pose, camera, velocity,
                    // contact caches and course start. Arguments (rider, 0, 1).
                    targetPc = 0x11de60u;
                    SET_GPR_U32(ctx, 4, getRegU32(ctx, 4));
                    SET_GPR_U32(ctx, 5, 0u);
                    SET_GPR_U32(ctx, 6, 1u);
                    std::fprintf(stderr, "[mgx1-lap] t=%llu slot=%u LAP %u/3 return-to-top\n",
                                 (unsigned long long)m_memory.gs().vsyncTick.load(std::memory_order_relaxed),
                                 slot, (unsigned)ls.lap);
                }
                else
                {
                    ls.finished = true;
                    if (slot == 0u) g_mgx1HudLap.store(0u, std::memory_order_relaxed);
                    std::fprintf(stderr, "[mgx1-lap] t=%llu slot=%u FINISH lap=3\n",
                                 (unsigned long long)m_memory.gs().vsyncTick.load(std::memory_order_relaxed), slot);
                }
            }
        }
    }
    // RSV1: one Tricky-only donor-volume test per rider update. Redirect this
    // call into SSX 3's existing course-reset entry; that guest routine owns
    // the fade, route respawn, camera, and contact state. Stock mode and the
    // default-off knob leave both the call and guest memory untouched.
    if (sourcePc == ps2_ssx3_tricky_resetvol::kRiderUpdateSource &&
        targetPc == ps2_ssx3_tricky_resetvol::kRiderUpdateTarget &&
        (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall) &&
        ps2_ssx3_tricky_resetvol::enabled())
    {
        ps2_ssx3_course::Modes &modes = ps2_ssx3_course::courseModes();
        const bool tricky = modes.armed && ps2_ssx3_course::modeCurrent(modes, rdram) != 0u;
        const int event = ssx3ReadS32(rdram, 0x535c08u);
        const std::string archive = event >= 0 && event < 32
            ? ps2_ssx3_course::rdStr(rdram, ps2_ssx3_course::kEventBase +
                                   static_cast<uint32_t>(event) * ps2_ssx3_course::kEventStride + 68u, 16u)
            : std::string();
        const int hit = ps2_ssx3_tricky_resetvol::poll(
            ps2_ssx3_tricky_resetvol::state(), ps2_ssx3_tricky_resetvol::table(),
            rdram, PS2_RAM_SIZE, tricky, event, archive, getRegU32(ctx, 4));
        if (hit >= 0)
        {
            targetPc = ps2_ssx3_tricky_resetvol::kResetEntry;
            std::fprintf(stderr, "[rsv1] RESET t=%llu event=%d archive=%s rider=%08x donor=%d\n",
                         static_cast<unsigned long long>(m_memory.gs().vsyncTick.load(std::memory_order_relaxed)),
                         event, archive.c_str(), getRegU32(ctx, 4), hit);
        }
    }
    // np-link-1 (SRVP1 P1a; default off): commit-gated remote drive of one
    // AI slot at the same 0x111728 pre-call boundary. Local inputs are
    // applied only via the echoed relay commit (waitCommit), never direct.
    // Logs [np-link] rows only when a live commit replaces AI words.
    if (targetPc == ps2_np_rider::kInputConsumer &&
        (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall) &&
        ps2x::nplink::enabled())
    {
        const uint64_t linkTick = m_memory.gs().vsyncTick.load(std::memory_order_relaxed);
        if (linkTick >= ps2x::nplink::firstTick())
        {
            ps2x::nplink::onTick(linkTick);
            ps2x::nplink::Commit commit;
            if (ps2x::nplink::waitCommit(linkTick, commit) && commit.bLive)
            {
                const uint32_t ctl = getRegU32(ctx, 4) & PS2_RAM_MASK;
                const uint32_t rec = getRegU32(ctx, 5) & PS2_RAM_MASK;
                if (ctl + 0xDF0u <= PS2_RAM_SIZE && rec + 8u <= PS2_RAM_SIZE)
                {
                    uint32_t actor = 0u, de4 = 0u, w0 = 0u, w1 = 0u, slot = 6u, srcDesc = 0u;
                    bool valid = false;
                    std::memcpy(&actor, rdram + ctl + 0x18u, 4u);
                    actor &= PS2_RAM_MASK;
                    std::memcpy(&de4, rdram + ctl + 0xDE4u, 4u);
                    std::memcpy(&srcDesc, rdram + ctl + 0xDE8u, 4u);
                    std::memcpy(&w0, rdram + rec, 4u);
                    std::memcpy(&w1, rdram + rec + 4u, 4u);
                    if (actor + 0x870u <= PS2_RAM_SIZE)
                    {
                        int32_t s = 0;
                        std::memcpy(&s, rdram + actor + 0x86Cu, 4u);
                        if (s >= 0 && s <= 5)
                        {
                            slot = static_cast<uint32_t>(s);
                            valid = true;
                        }
                    }
                    const int wantSlot = ps2x::nplink::riderSlot();
                    const bool isTarget = valid && slot == static_cast<uint32_t>(wantSlot < 0 ? 6 : wantSlot);
                    if (isTarget)
                    {
                        const bool protectedState = de4 == 9u || de4 == 10u || de4 == 13u;
                        const bool aiOwned = srcDesc == ps2_np_rider::kAiSourceDesc;
                        if (!protectedState && aiOwned && commit.de4 == de4)
                        {
                            std::memcpy(rdram + rec, &commit.w0, 4u);
                            std::memcpy(rdram + rec + 4u, &commit.w1, 4u);
                            // np-fix-1 F12: relay-commit applies join the
                            // canonical transcript like script applies.
                            ps2_savestate::canonicalRecordRider(linkTick, slot, 0u, de4,
                                                                "link-applied", true);
                            std::fprintf(stderr,
                                         "[np-link] tick=%llu slot=%u de4=%u ai=%08x,%08x out=%08x,%08x applied\n",
                                         (unsigned long long)linkTick, slot, de4, w0, w1, commit.w0,
                                         commit.w1);
                        }
                        else
                        {
                            // day-fix-1 L5: refusals leave a row too, so a
                            // transcript with no applies is distinguishable
                            // from one that never saw the link.
                            ps2_savestate::canonicalRecordRider(linkTick, slot, 0u, de4,
                                                                "link-refused", false);
                        }
                    }
                }
            }
        }
    }
    // np-rider-1 (SRVP1 P0b candidate; default off): replace one AI slot's
    // 8-byte command record at the 0x111728 pre-call boundary with the
    // tick-stamped remote script. Listed in the DSP1 target filter above
    // only when enabled.
    if (targetPc == ps2_np_rider::kInputConsumer &&
        (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall))
    {
        ps2_np_rider::loadOnce();
        const ps2_np_rider::Config &cfg = ps2_np_rider::config();
        if (cfg.enabled)
        {
            const uint64_t tick = m_memory.gs().vsyncTick.load(std::memory_order_relaxed);
            const uint32_t ctl = getRegU32(ctx, 4) & PS2_RAM_MASK;
            const uint32_t rec = getRegU32(ctx, 5) & PS2_RAM_MASK;
            if (ctl + 0xDF0u <= PS2_RAM_SIZE && rec + 8u <= PS2_RAM_SIZE)
            {
                uint32_t actor = 0u, de4 = 0u, w0 = 0u, w1 = 0u, slot = 6u, srcDesc = 0u;
                bool valid = false;
                std::memcpy(&actor, rdram + ctl + 0x18u, 4u);
                actor &= PS2_RAM_MASK;
                std::memcpy(&de4, rdram + ctl + 0xDE4u, 4u);
                std::memcpy(&srcDesc, rdram + ctl + 0xDE8u, 4u);
                std::memcpy(&w0, rdram + rec, 4u);
                std::memcpy(&w1, rdram + rec + 4u, 4u);
                if (actor + 0x870u <= PS2_RAM_SIZE)
                {
                    int32_t s = 0;
                    std::memcpy(&s, rdram + actor + 0x86Cu, 4u);
                    if (s >= 0 && s <= 5)
                    {
                        slot = static_cast<uint32_t>(s);
                        valid = true;
                    }
                }
                const bool isTarget = valid && slot == static_cast<uint32_t>(cfg.slot < 0 ? 6 : cfg.slot);
                if (valid && (slot == 0u || isTarget))
                {
                    // np-fix-1 F7/F8: strict decision with session/actor
                    // generation, cursor re-derivation and per-call
                    // ordinals. stderr format is unchanged; the decision
                    // also goes to the canonical transcript (F12).
                    static ps2_np_rider::Session hookSession;
                    const bool aiOwned = srcDesc == ps2_np_rider::kAiSourceDesc;
                    const ps2_np_rider::Decision d = ps2_np_rider::decide(
                        hookSession, cfg, tick, isTarget, actor, de4, aiOwned, w0, w1);
                    if (d.applied)
                    {
                        std::memcpy(rdram + rec, &d.outW0, 4u);
                        std::memcpy(rdram + rec + 4u, &d.outW1, 4u);
                    }
                    ps2_savestate::canonicalRecordRider(tick, slot, d.ordinal, de4, d.why,
                                                        d.applied);
                    std::fprintf(stderr,
                                 "[np-rider] tick=%llu slot=%u ctl=%08x de4=%u ai=%08x,%08x out=%08x,%08x %s\n",
                                 (unsigned long long)tick, slot, getRegU32(ctx, 4), de4, w0, w1,
                                 d.applied ? d.outW0 : w0, d.applied ? d.outW1 : w1, d.why);
                }
            }
        }
    }
    // Quick Play netplay (PS2X_NETSYNC, default off): ghost drive at the
    // same input-consumer boundary. Listed in the DSP1 target filter above
    // only while PS2X_NETSYNC=1; counters go to the periodic [netsync] line.
    // (online-v0-1: its own block. The train-28 landing sat inside the
    // np-rider decision above, so it only ran with PS2X_NP_RIDER_ENABLE=1.)
    if (targetPc == 0x00111728u &&
        (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall))
    {
        const uint32_t syncCtl = getRegU32(ctx, 4) & PS2_RAM_MASK;
        const uint32_t syncRec = getRegU32(ctx, 5) & PS2_RAM_MASK;
        uint32_t liveW0 = 0u, liveW1 = 0u;
        const char *why = "off";
        if (syncCtl + 0xDF0u <= PS2_RAM_SIZE && syncRec + 8u <= PS2_RAM_SIZE &&
            ps2_netsync::onInput(m_memory.gs().vsyncTick.load(std::memory_order_relaxed), syncCtl, syncRec,
                                 rdram, liveW0, liveW1, why))
        {
            std::memcpy(rdram + syncRec, &liveW0, 4u);
            std::memcpy(rdram + syncRec + 4u, &liveW1, 4u);
        }
    }
    // TK45c: Tricky gem multipliers (PS2X_SSX3_TRICKY_GEMS=1; default off).
    // Boundary hook = the per-rider half-step pickup poll (player-1 only);
    // bank-clear hook clears +0x1c4 with +0x18. Listed in the DSP1 target
    // filter above. No nested guest calls (no speech callout: REPORT §callout).
    if ((kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall) &&
        ps2_tk45c::enabled())
    {
        const uint64_t gemsTick = m_memory.gs().vsyncTick.load(std::memory_order_relaxed);
        if (sourcePc == 0x128ddcu && targetPc == 0x1216e0u)
        {
            ps2_ssx3_course::Modes &gemsMs = ps2_ssx3_course::courseModes();
            const bool gemsTricky =
                gemsMs.armed && ps2_ssx3_course::modeCurrent(gemsMs, rdram) != 0u;
            ps2_tk45c::poll(ps2_tk45c::state(), ps2_tk45c::table(), rdram, PS2_RAM_SIZE, gemsTick,
                            getRegU32(ctx, 4), gemsTricky, ps2_tk45c::spinEnabled());
        }
        if (targetPc == 0x117838u)
        {
            uint32_t gemsClock = 0u;
            const uint32_t gemsB = ps2_ssx3_tricky_hud::resolveChainB(rdram, PS2_RAM_SIZE);
            if (gemsB != 0u)
                ps2_ssx3_tricky_hud::readGuestU32(rdram, PS2_RAM_SIZE, gemsB, 0xcu, gemsClock);
            ps2_tk45c::onBankClear(rdram, PS2_RAM_SIZE, gemsTick, getRegU32(ctx, 4), gemsClock, sourcePc);
        }
    }
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
    // FH1: full120 manager patch at the init hook + env-only tap counts.
    if ((ps2_fh1::enabled() || ps2_fh1::tapOn()) && ps2_fh1::onBranch(rdram, ctx, sourcePc, targetPc, this))
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
    if ((targetPc == kSsx3PatchChainAppend || targetPc == kSsx3PatchChainAppendCE) &&
        (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall) &&
        ssx3PatchChainGuard(ctx, targetPc))
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
        // split-input-1 #7: the SSX 3 widescreen-default policy lives in
        // the game module (0x228C08 applies the video choice after
        // defaults, menu edits and profile loads). Null (or no module)
        // keeps the game default.
        if (const ps2_game_module::Module *wsModule = ps2_game_module::installed();
            wsModule != nullptr && wsModule->noteWidescreenDispatch != nullptr)
        {
            wsModule->noteWidescreenDispatch(rdram, sourcePc);
        }
    }

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        ps2_fh1::postUnwindRestore(rdram);
        markGuestUnwind();
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
    // P1c HLE stub/call histogram at the register_functions.cpp binding
    // lookup, per-period and cleared. Already gated on the diag period.
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

    ps2x::tel::coverPc(targetPc); // TEL2 (one relaxed load when off)
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
    if (ps2_fh1::g_postArmed || ps2_fh1::g_pubArmed || ps2_fh1::g_normcapArmed) // NRM1: normcap shares it too
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


// split-input-1 #3 (Part 2): SSX 3's VU0 visibility tests live in the game
// module now (Ssx3VisNative.h); the export helper below is generic VU0
// state plumbing, so its definition stays here. The runtime never includes
// the game header (seam direction); the declaration is repeated verbatim.
namespace ps2_ssx3_vis_native
{
    void exportVu0StateForTest(const VuState &state, R5900Context *ctx);
}

void ps2_ssx3_vis_native::exportVu0StateForTest(const VuState &state, R5900Context *ctx)
{
    copyVu0StateToContext(state, ctx);
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

    // split-input-1 #3 (Part 2): SSX 3's VU0 visibility tests run in the
    // game module (default on, PS2X_SSX3_VIS_NATIVE); without it the VU0
    // engine runs: the exact/reference path. Same order as the moved block:
    // pc match, knob mode, image match, then serve (mode 1) or the
    // check-mode snapshot pair around the engine run below.
    bool visCheck = false;
    uint32_t visPc = 0;
    const ps2_game_module::Module *visModule = ps2_game_module::installed();
    const bool visHooked = visModule != nullptr && visModule->visNativeMatchesPc != nullptr &&
                           visModule->visNativeMode != nullptr && visModule->visNativeImageMatches != nullptr &&
                           visModule->visNativeRun != nullptr && visModule->visNativeCheckBegin != nullptr &&
                           visModule->visNativeCheckEnd != nullptr;
    if (visHooked && visModule->visNativeMatchesPc(startPC))
    {
        const int visMode = visModule->visNativeMode();
        if (visMode != 0 && visModule->visNativeImageMatches(vu0Code, m_memory.getVU0CodeGeneration()))
        {
            visPc = startPC;
            if (visMode == 1)
            {
                visModule->visNativeRun(ctx, vu0Data, startPC);
                return;
            }
            visModule->visNativeCheckBegin(ctx, vu0Data, startPC);
            visCheck = true;
        }
    }

    // VR3: importVu0Context rewrites all of m_state and execute() resets
    // the scheduler, so only reset()'s cycle/count part is live here.
    // VZ1: the import takes the lean path unless PS2X_VU0_LEAN_ENTRY=0.
    m_vu0.resetForVu0Start();
    importVu0Context(ctx, m_vu0.state(), vu0LeanEntryEnabled());
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, 4096);
    copyVu0StateToContext(m_vu0.state(), ctx);
    if (visCheck)
        visModule->visNativeCheckEnd(ctx, visPc);
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
    const uint32_t hardLimit = std::min(guestHeapHardLimit(), PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(guestHeapHardLimit(), PS2_RAM_SIZE);
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

uint32_t PS2Runtime::guestMallocHle(uint32_t size, uint32_t alignment)
{
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (m_hleArenaBlocks.empty())
            m_hleArenaBlocks.push_back({ps2_hle_pools::kHleArenaBase, ps2_hle_pools::kHleArenaBytes, true});
        // Same first-fit allocator over the arena's own block list.
        const uint32_t heapEnd = m_guestHeapEnd;
        m_guestHeapBlocks.swap(m_hleArenaBlocks);
        const uint32_t addr = allocateGuestBlockLocked(size, alignment);
        m_guestHeapBlocks.swap(m_hleArenaBlocks);
        m_guestHeapEnd = heapEnd;
        if (addr != 0u)
            return addr;
    }
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
        std::fprintf(stderr, "[hle-arena] full (size=0x%x); allocation refused\n", size);
    return 0u;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    const uint32_t phys = guestAddr & PS2_RAM_MASK;
    if (phys - ps2_hle_pools::kHleArenaBase < ps2_hle_pools::kHleArenaBytes)
    {
        m_guestHeapBlocks.swap(m_hleArenaBlocks);
        freeGuestBlockLocked(guestAddr);
        m_guestHeapBlocks.swap(m_hleArenaBlocks);
        return;
    }
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

bool PS2Runtime::eeCheckpointDueSlow(uint32_t cycles) noexcept
{
    if (!m_eeScheduler->checkpointDue(cycles))
    {
        return false;
    }
    markGuestUnwind();
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

namespace
{
// panel-polish-1: perf-overlay origin pad (raylib screen coordinates)
// clearing the display cutout and rounded corners. Android: live JNI root +
// cutout insets; iOS: UIKit safeAreaInsets cached on the main thread;
// elsewhere zero (fail-static: the overlay draws where it always did).
void set2SafePad(float &left, float &top)
{
    left = 0.0f;
    top = 0.0f;
#if defined(__ANDROID__)
    float right = 0.0f, bottom = 0.0f;
    if (androidSafeInsets(left, top, right, bottom))
    {
        // Physical pixels -> raylib screen coordinates (same space DrawText
        // uses; the mid-file includes above declare these).
        const float screenW = static_cast<float>(GetScreenWidth());
        const float dpi =
            (screenW > 0.0f) ? static_cast<float>(GetRenderWidth()) / screenW : 1.0f;
        if (dpi > 0.0f)
        {
            left /= dpi;
            top /= dpi;
        }
    }
#elif defined(PS2X_IOS)
    float right = 0.0f, bottom = 0.0f;
    (void)ps2x::ios::safeInsets(left, top, right, bottom);
#endif
}
} // namespace

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
    // HDL1: headless opens no window, so there is no GL context to load
    // the frame texture into; the draw block below is skipped entirely.
    const bool headlessNoRaster = ps2x_gs_headless::requested();
    Texture2D frameTex = {0};
    if (!headlessNoRaster)
        frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);

    std::string asv1ColdLoadPath;
#if defined(__ANDROID__) || defined(PS2X_IOS)
    const char *asv1Enabled = std::getenv("PS2X_AUTOSAVE");
    if (asv1Enabled && std::strcmp(asv1Enabled, "1") == 0 &&
        ps2_savestate::config().loadPath.empty())
    {
        if (ps2_net::enabled())
            std::fprintf(stderr, "[autosave] launch skipped: online mode enabled\n");
        else
        {
            const std::string slot = autosaveSlotPath();
            std::error_code ec;
            if (std::filesystem::exists(slot, ec) && !ec)
            {
                uint64_t savedWall = 0u;
                std::string reason;
                if (!ps2_savestate::autosaveHeaderEligible(slot, savedWall, reason))
                {
                    std::fprintf(stderr, "[autosave] launch refused: %s\n", reason.c_str());
                    ps2x::postQuickStatusToast("Saved session unavailable: build or settings changed");
                }
                else
                {
                    const std::time_t wallNow = std::time(nullptr);
                    const uint64_t now = wallNow > 0 ? static_cast<uint64_t>(wallNow) : 0u;
                    const auto choice = ps2_savestate::autoResumeDecision(
                        savedWall, now, ps2_savestate::autoResumeThresholdSeconds());
                    if (choice == ps2_savestate::AutoResumeDecision::Silent)
                    {
                        std::fprintf(stderr, "[autosave] auto-resume age=%llu s\n",
                                     static_cast<unsigned long long>(now - savedWall));
                        asv1ColdLoadPath = slot;
                    }
                    else
                    {
                        bool closed = false;
                        const bool resume = asv1AskToResume(closed);
                        if (closed)
                        {
                            UnloadTexture(frameTex);
                            CloseWindow();
                            return;
                        }
                        if (resume)
                            asv1ColdLoadPath = slot;
                        std::fprintf(stderr, "[autosave] prompt choice=%s\n", resume ? "resume" : "fresh");
                    }
                }
            }
        }
    }
#endif

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
#if defined(__ANDROID__) || defined(PS2X_IOS)
            else if (!asv1ColdLoadPath.empty())
            {
                std::string note, error;
                if (ps2_savestate::loadQuick(*this, asv1ColdLoadPath, note, error))
                {
                    std::fprintf(stderr, "[autosave] cold-load ok: %s\n", note.c_str());
                    ps2_savestate::noteQuickStatus(note);
                }
                else
                {
                    // Full validation precedes mutation; a bad slot leaves
                    // the fresh machine intact and the slot available for
                    // investigation rather than deleting it.
                    std::fprintf(stderr, "[autosave] cold-load refused: %s\n", error.c_str());
                    ps2x::postQuickStatusToast("Saved session could not be resumed");
                }
            }
#endif
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

#if defined(__ANDROID__) || defined(PS2X_IOS)
    g_bg1GuestStarted.store(true, std::memory_order_release);
#endif
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
    bool vpadWanted = virtualPadWanted();
    // android-touch-1: Touch panel row (Auto/On/Off), live like Perf.
    // Auto (PS2X_VIRTUAL_PAD unset) shows the overlay only when no gamepad
    // is present; On ("1")/Off ("0") force it. vpadWanted tracks
    // (mode != Off) so the existing branches keep working.
    ps2x::set2::Touch vpadTouchMode = [] {
        const char *v = std::getenv("PS2X_VIRTUAL_PAD");
        return ps2x::set2::touchFromSetting(v ? v : "");
    }();
    PSChordState ds1Chord; // SELECT+L3 save / SELECT+R3 load, carried across frames
    // SET2: settings panel state, carried across frames. Knob off: the chord
    // path below is one bool check per frame (plus the always-on perf tick
    // sample, one atomic load + one clock).
    const bool set2On = set2OverlayOn();
    const std::vector<Set2TestSpan> set2Test = set2On ? set2TestSpans() : std::vector<Set2TestSpan>();
    const char *set2ShotPath = set2On ? std::getenv("PS2X_SET2_SHOT") : nullptr;
    if (set2On)
        std::fprintf(stderr, "[set2] overlay on (shot=%s)\n",
                     (set2ShotPath && set2ShotPath[0]) ? set2ShotPath : "(none)");
    ps2x::set2::ChordState set2Chord;
    // set2-polish-1 (Brad 10-16): the chord also listens to the touch
    // SELECT/START buttons (previous host frame's vpad mask), with the same
    // hold-back applied to the touch path before it publishes.
    uint16_t set2VpadStrip = 0u;
    uint16_t set2VpadAdd = 0u;
    ps2x::set2::NavState set2Nav;
    bool set2Open = false;
    bool set2Eff120 = false;   // effective values, sampled at open
    std::string set2EffUpscale; // effective GE1_UPSCALE (or the platform default)
    unsigned set2MeasuredH = 0;  // panel-polish-1: last real game-frame height px
    ps2x::set2::Res set2DefRes = ps2x::set2::Res::Screen; // (default) marker + preselect (correction 4)
    uint64_t set2HostFrame = 0;
    uint16_t set2PrevUnion = 0; // nav edge detect
    ps2x::set2::SwallowState set2Swallow; // close-press overhang, stripped until release
    std::vector<int64_t> set2Down; // touch ids currently on the panel/dot
    std::vector<int64_t> set2HeldTouches; // touches still down at close, masked until release
    std::vector<int64_t> set2MenuIds; // touch ids seen while closed (a held finger never re-opens)
    int set2ShotIn = -1;           // host frames until the panel screenshot (-1 idle)
    bool set2Perf = false;         // live perf-overlay flag (env default at boot)
    bool set2Upscale = false;      // live on-device-upscale flag (env default at boot)
    if (set2On)
    {
        const char *perf = std::getenv("PS2X_SETTINGS_PERF");
        set2Perf = perf && std::string(perf) == "1";
        set2Nav.perf = set2Perf;
        const char *upscale = std::getenv("GE1_ONDEVICE_UPSCALE");
        set2Upscale = upscale && std::string(upscale) == "1";
        set2Nav.upscale = set2Upscale;
        set2Nav.touch = vpadTouchMode;
        const char *menubtn = std::getenv("PS2X_MENU_BUTTON");
        set2Nav.menubtn = ps2x::set2::menuBtnFromSetting(menubtn ? menubtn : "");
    }
    // tk-menu-1 Part 2: the Tricky course list. Knob off: one cached-bool
    // check per host frame in the block below; the guest is untouched.
    const bool tkListOn = ps2_ssx3_tricky_list::knob();
    ps2_ssx3_tricky_list::ListState tkList;
    bool tkOpen = false;
    int tkOpenDeb = 0;  // consecutive on-target frames (open at 4)
    int tkShutDeb = 0;  // consecutive off-target frames (close at 12)
    bool tkLatchedOn = false; // reopen needs a NEW Map entry (stale words can't)
    uint64_t tkLatchGen = 0;
    uint16_t tkPrevUnion = 0; // list nav edge detect (physical + test spans)
    std::vector<int64_t> tkDown; // touch ids currently on the list
    // Lane shots (PS2X_TK_MENU_2_SHOT=<prefix>): the host present, overlay
    // included, every 300 overlay frames while the list is open. Headless
    // Mini evidence for the sheet/video; remove with the lane.
    const char *tkShotPrefix = std::getenv("PS2X_TK_MENU_2_SHOT");
    if (tkShotPrefix && !tkShotPrefix[0])
        tkShotPrefix = nullptr;
    if (tkListOn)
    {
        const char *tkListEnv = std::getenv("PS2X_SSX3_TRICKY_LIST");
        const char *tkMenuEnv = std::getenv("PS2X_SSX3_TRICKY_MENU");
        std::fprintf(stderr, "[tklist] overlay on (LIST=%s MENU=%s)\n", tkListEnv ? tkListEnv : "unset",
                     tkMenuEnv ? tkMenuEnv : "unset");
    }
    // SET2 perf sampler: always-on lightweight vs/s + frame-time (one tick
    // load + one clock per host frame; the line draws only when perf is on).
    auto set2PerfWinStart = std::chrono::steady_clock::now();
    uint64_t set2PerfWinTick = m_memory.gs().vsyncTick.load();
    double set2PerfVs = 0.0;
    double set2PerfMs = 0.0;
    auto set2PerfLastFrame = std::chrono::steady_clock::now();
    // DS1: hash the runner for save/load identity on a background thread
    // while the game boots (a ~200 MB pass; the first quick-save would
    // otherwise hitch on it). The cache mutex guards the result.
    std::thread([] { ps2_savestate::warmRunnerSha(); }).detach();
    bool vpadLastPadConnected = true; // forces the first [vpad] line when the overlay shows
    bool vpadFirstFrame = true;       // android-touch-1: evaluate underlay on frame one either way
#if defined(__ANDROID__)
    bool vpadImPresent = false; // android-touch-1b: cached InputManager presence
    int vpadImCache = 0;        // ... re-queried every 30 host frames (~0.5 s)
#endif
    const std::vector<ps2x::vpad::TestTouch> vpadTestTouches =
        ps2x::vpad::parseTestTouches(std::getenv("PS2X_VPAD_TEST_TOUCHES")); // DEV-ONLY
    ps2x::vpad::PadState vpadPad; // VT1: touch ownership + stick anchor, carried across frames
    uint16_t vpadPressedPrev = 0u; // set2-polish-1: last frame's touch mask, for the panel chord
    // VT3: PS2X_VPAD_SHOULDER_BAND=0 restores the VT1 hit test (1.0x discs,
    // no shoulder band, no slide-on); unset or anything but "0" = on.
    const bool vpadShoulderBand = ps2x::vpad::enabledFromEnv(std::getenv("PS2X_VPAD_SHOULDER_BAND"));
    if (!vpadWanted)
    {
        std::fprintf(stderr, "[vpad] off (PS2X_VIRTUAL_PAD)\n");
    }
    // split-input-1 #7: the anamorphic default lives in the game module
    // (armed by the ssx3-widescreen-default row); without it the game
    // default stands.
    const ps2_game_module::Module *wsAnamorphicModule = ps2_game_module::installed();
    const bool anamorphic = wsAnamorphicModule != nullptr &&
                            wsAnamorphicModule->widescreenAnamorphic != nullptr &&
                            wsAnamorphicModule->widescreenAnamorphic();
    const ps2x::present::Aspect presentAspect = ps2x::present::aspectFromEnv(std::getenv("PS2X_ASPECT"), anamorphic);
    const ps2x::present::Filter presentFilter = ps2x::present::filterFromEnv(std::getenv("PS2X_PRESENT_FILTER"));
    int appliedFilter = -1;
    unsigned int appliedFilterTexId = 0u;
    auto vsyncRateWall = std::chrono::steady_clock::now();
    uint64_t vsyncRateTick = m_memory.gs().vsyncTick.load();
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
#if defined(__ANDROID__) || defined(PS2X_IOS)
        // BG1: backgrounded (or pre-focus pause): no guest, no presents, no
        // swapping. PollInputEvents blocks in the looper once raylib is
        // disabled (backgrounded); while still enabled, sleep so a pause
        // without focus loss doesn't spin on an unchanged frame.
        // SET2: exempt the panel pause (any platform): the loop must keep
        // running to draw the panel over the frozen frame.
        if (ps2x::hostPause::paused() && !set2Open)
        {
            PollInputEvents(); // delivers APP_CMD_RESUME / DESTROY (iOS: pumps SDL)
            if (WindowShouldClose())
            {
                requestStop();
                break;
            }
            // android-touch-1: a backgrounding (or focus loss) with fingers
            // down must not stick buttons: the touch list is gone on resume,
            // so drop ownership now and clear the latch while the guest is
            // frozen (no reads happen paused, so this is just the release).
            vpadPad = ps2x::vpad::PadState{};
            vpadPressedPrev = 0u; // set2-polish-1: no stale touch chord across the pause
            publishPad(0, m_memory.gs().vsyncTick.load());
            publishPadPort1(0, m_memory.gs().vsyncTick.load());
            ps2x::vpad::liveStick().store(ps2x::vpad::kStickNoOverride, std::memory_order_relaxed);
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
        // SET2 perf sampler: one tick load + one clock per host frame (the
        // line draws only when the perf overlay is on).
        {
            const auto set2Now = std::chrono::steady_clock::now();
            const double set2FrameMs =
                std::chrono::duration<double, std::milli>(set2Now - set2PerfLastFrame).count();
            set2PerfLastFrame = set2Now;
            set2PerfMs += (std::min(set2FrameMs, 1000.0) - set2PerfMs) * 0.05;
            if (std::chrono::duration<double>(set2Now - set2PerfWinStart).count() >= 1.0)
            {
                const uint64_t vt = m_memory.gs().vsyncTick.load();
                const double winS =
                    std::chrono::duration<double>(set2Now - set2PerfWinStart).count();
                set2PerfVs = static_cast<double>(vt - set2PerfWinTick) / winS;
                set2PerfWinTick = vt;
                set2PerfWinStart = set2Now;
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
        // HDL1: headless referee: no latch, no upload, no draw, no input
        // poll, no swap. The guest runs on the game thread; scripted pad
        // input needs nothing from the host window. The loop condition
        // still joins on game-thread completion and stop requests.
        if (headlessNoRaster)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        UploadFrame(frameTex, this, presentWidth, presentHeight);

#if defined(PS2X_IOS)
        ps2x::ios::syncWindowSize();
        ps2x::ios::pollSafeInsets(); // panel-polish-1: safe-area refresh (throttled inside)
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
            pso1::beforeDraw();
            DrawTexturePro(g_hr1ShareTex, srcRect, shareDst, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            rlDrawRenderBatchActive();
            pso1::afterDraw();
            rlEnableColorBlend();
        }
        else
#endif
#if defined(__ANDROID__)
        if (!ps2x_present_vk::active()) // VK1: the game is on the SurfaceControl layer above
#endif
        DrawTexturePro(frameTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        // I26: virtual controls, hidden while a connected game controller is in use.
        // android-touch-1: Touch=On forces the overlay even with a pad;
        // otherwise it hides when a pad would drive the game. On Android
        // that is presence (the Odin always enumerates its built-in
        // controller, so it defaults to hidden); elsewhere a press is
        // required first (the iOS Simulator reports a fake Gamepad).
        const bool vpadForcedOn = (vpadTouchMode == ps2x::set2::Touch::On);
#if defined(__ANDROID__)
        // android-touch-1b: OS presence (cached) OR raylib-connected OR
        // pad-in-use: the overlay never shows while any physical pad
        // exists, even before its first press (Odin default hidden).
        if (--vpadImCache <= 0)
        {
            vpadImPresent = androidInputManagerGamepad();
            vpadImCache = 30;
        }
        const bool vpadPadHides = vpadImPresent || gamepadConnected() || gamepadInUse();
        // ctrl-harden-1: feed the sampler's stale-slot guard (Android
        // disconnect with held buttons) from this OS-level poll.
        ps2xNoteOsGamepadPresent(vpadImPresent);
        // ... and refresh soon when raylib still claims a pad the OS no
        // longer lists (disconnect detected; heals within ~5 host frames
        // instead of the full 30-frame cache window).
        if (!vpadImPresent && gamepadConnected() && vpadImCache > 5)
            vpadImCache = 5;
#else
        // ctrl-harden-1: a connected real controller hides the overlay at
        // once (Touch = Auto); the Simulator fake needs a first press.
        const bool vpadPadHides =
#if defined(PS2X_IOS)
            iosRealControllerPresent() ||
#endif
            gamepadInUse();
#endif
        const bool vpadPadConnected = vpadWanted && !vpadForcedOn && vpadPadHides;
#if defined(__ANDROID__)
        // android-touch-1c: the present path follows the overlay every
        // frame (deduped in the ledger): panel open or overlay shown →
        // underlaid translucent GL (VK child below, hole punched);
        // otherwise VK above, GL skipped (fast path). Touch=Off keeps GL
        // composited via set2On so the panel stays reachable; the Odin
        // default path (Auto+pad, panel closed) is unaffected (fast).
        // set2DoOpen re-asserts true with its geometry wait; set2DoClose
        // needs no call (next frame's rule already covers it).
        // set2-polish-1 (Brad 10-16): the perf overlay draws whenever it is
        // enabled, independent of the panel — so Perf on also holds the
        // underlay (same composition as the open panel, no dim). Perf
        // defaults off, so the play fast path is unchanged until enabled.
        // settings-res-1: the "..." menu button shows by default on every
        // device (Odin included), so Menu=Shown also holds the underlay —
        // the Odin default path now composites instead of taking the fast
        // path (host-side only; det-identical). Hiding the button via the
        // Menu button row restores the fast path.
        ps2x_present_vk::setUnderlay(set2Open || (set2On && set2Perf) ||
                                     (set2On && ps2x::set2::menuButtonShown(set2Nav.menubtn)) ||
                                     (vpadWanted ? !vpadPadConnected : set2On));
#endif
        // First frame always evaluates (Odin starts hidden on the fast
        // path; a phone starts shown underlaid).
        if (vpadWanted && (vpadFirstFrame || vpadPadConnected != vpadLastPadConnected))
        {
            vpadFirstFrame = false;
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
        // read.
        // SET2: the diag test spans inject here, as if from a real pad, so
        // they flow through DS1 and the hold-back exactly like physical
        // input (a forwarded test Select must reach the published mask).
        if (set2On)
            ++set2HostFrame;
        const uint16_t set2Inject =
            set2On ? set2TestMask(set2Test, set2HostFrame) : static_cast<uint16_t>(0u);
        const uint16_t ds1TrueRaw =
            static_cast<uint16_t>(ps2xSampleRaylibPad().pressed | set2Inject);
        // ctrl-harden-1: pad 2 follows the second controller only (no
        // touch, no keyboard, no test-span inject, no DS1/SET2 chords —
        // those stay on pad 1, whose controller owns the settings chord).
        const uint16_t ds1Port1Raw = ps2xSampleRaylibPadPort(1).pressed;
        // SET2: while the panel is open the panel owns the pad (DS1 suppressed).
        const uint16_t ds1RawPressed = set2Open ? 0u : ds1TrueRaw;
        uint16_t raylibPressed = ds1RawPressed;
        // DS1: quick-save/load chord, edge-triggered. The chord is stripped from the guest
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
        // SET2: chord (Select-first + Start edge, ~150 ms window) with Select
        // hold-back, + panel nav/open/close. Runs on the post-DS1 mask, so a
        // DS1 chord (Select already stripped) stays idle here. Knob off: the
        // outer bool check only.
        auto set2SampleTouches = [&](ps2x::vpad::TouchPoint *out) {
            // Real touches + the DEV-ONLY synthetic test touches (the same
            // merge as the vpad branch, so edge tracking stays coherent
            // when the panel opens/closes around a held test touch).
            const int n = virtualPadTouches(out, 8, screenWidth, screenHeight);
            return ps2x::vpad::activeTestTouchesWithIds(vpadTestTouches,
                                                        m_memory.gs().vsyncTick.load(), screenWidth,
                                                        screenHeight, out, n, 8);
        };
        auto set2DoOpen = [&] {
            set2Eff120 = set2Effective120();
            if (const char *u = std::getenv("GE1_UPSCALE"); u && u[0] != '\0')
                set2EffUpscale = u;
            else
                set2EffUpscale = "1x";
            set2Nav.sel = ps2x::set2::Row::Mode;
            set2Nav.mode = set2Eff120 ? ps2x::set2::Mode::M120 : ps2x::set2::Mode::M60;
            set2Nav.runMode = set2Nav.mode;
            // Correction 4: the (default) marker and the preselect come from
            // the device rules — the public tier rule on public builds, the
            // play env rules (Android Screen, iOS 2x) otherwise.
            {
                const char *pub = std::getenv("PS2X_PUBLIC_APP");
                const bool publicApp = pub && std::string(pub) == "1";
                std::uint64_t ram = 0;
                if (const char *rt = std::getenv("PS2X_PUBLIC_RAM_BYTES"); rt && rt[0] != '\0')
                    ram = std::strtoull(rt, nullptr, 10);
#if defined(PS2X_IOS)
                constexpr bool isiOS = true;
#else
                constexpr bool isiOS = false;
#endif
                set2DefRes = ps2x::set2::resDefaultRow(publicApp, isiOS, ram,
                                                       static_cast<int>(screenWidth),
                                                       static_cast<int>(screenHeight));
            }
            set2Nav.res = set2DefRes;
            for (const auto &kv : set2ReadSettings())
            {
                if (kv.first == "GE1_UPSCALE")
                    set2Nav.res = ps2x::set2::resFromSetting(kv.second, set2DefRes);
                else if (kv.first == "PS2X_MENU_BUTTON")
                    set2Nav.menubtn = ps2x::set2::menuBtnFromSetting(kv.second);
            }
            set2Nav.runRes = set2Nav.res;
            set2Nav.nativeH = set2MeasuredH;
            set2Nav.perf = set2Perf;
            set2Nav.upscale = set2Upscale;
            set2Nav.touch = vpadTouchMode;
            set2Nav.restartArmed = false;
#if defined(__ANDROID__)
            // The panel needs the GL overlay visible over the VK layer. The Z
            // applies in queue(), so wait for one geometry apply while the
            // guest still runs: pausing first would freeze the queue with the
            // reorder pending (leg3 open#1 hid the panel for 8+ s).
            ps2x_present_vk::setUnderlay(true);
            if (ps2x_present_vk::active())
            {
                const uint64_t set2G0 = ps2x_present_vk::geometrySeq();
                const auto set2T0 = std::chrono::steady_clock::now();
                while (ps2x_present_vk::geometrySeq() == set2G0 &&
                       std::chrono::steady_clock::now() - set2T0 < std::chrono::milliseconds(500))
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
#endif
            ps2_snd_audio_output::pausePlayback();
            const uint64_t openTick = m_memory.gs().vsyncTick.load();
            ps2x::padlatch::setPanelOpenAll(true);
            ps2x::hostPause::requestPause();
            const bool acked = ps2x::hostPause::waitGateAck(2000);
            set2HoldsPause().store(true, std::memory_order_release);
            set2Open = true;
            // Seed: touches already down (incl. the tap/chord that opened us)
            // must not edge in the open branch next frame.
            ps2x::vpad::TouchPoint seed[8];
            const int nseed = set2SampleTouches(seed);
            set2Down.clear();
            for (int i = 0; i < nseed; ++i)
                set2Down.push_back(seed[i].id);
            if (set2ShotPath && set2ShotPath[0] != '\0')
                set2ShotIn = 45;
            std::fprintf(stderr, "[set2] open guestTick=%llu gateAck=%d mode=%s upscale=%s\n",
                         static_cast<unsigned long long>(openTick), acked ? 1 : 0,
                         set2Eff120 ? "120" : "60", set2EffUpscale.c_str());
        };
        auto set2DoClose = [&] {
            set2Open = false;
            // Swallow the close-press overhang: buttons down at close (the
            // closing Triangle/chord, a held nav X) stay stripped until
            // released, else they would fire in the game on resume.
            set2Swallow.mask = ds1TrueRaw;
            // Seed like open: touches still down (a resting finger, a long
            // test hold) must not edge on the next frame, else the panel
            // would instantly re-open (iPad legA caught this).
            {
                ps2x::vpad::TouchPoint seed[8];
                const int nseed = set2SampleTouches(seed);
                set2Down.clear();
                set2HeldTouches.clear();
                for (int i = 0; i < nseed; ++i)
                {
                    set2Down.push_back(seed[i].id);
                    set2HeldTouches.push_back(seed[i].id);
                }
            }
            ps2x::padlatch::setPanelOpenAll(false);
            set2HoldsPause().store(false, std::memory_order_release);
            ps2x::hostPause::requestResume();
            ps2_snd_audio_output::resumePlayback();
            // android-touch-1c: no underlay call here (it used to force
            // false and strand a shown overlay with GL skipped): the
            // per-frame rule above re-asserts next frame.
            // NOTE: no set2Down.clear() here — the seed above must survive
            // (a stale clear undid the seed and re-opened the panel).
            set2ShotIn = -1;
            std::fprintf(stderr, "[set2] close guestTick=%llu\n",
                         static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()));
        };
        auto set2DoRestart = [&] {
            const bool want120 = set2Nav.mode == ps2x::set2::Mode::M120;
            const char *upscale = ps2x::set2::resToUpscale(set2Nav.res); // "" = platform default
            std::fprintf(stderr, "[set2] restart want120=%d upscale=%s\n", want120 ? 1 : 0,
                         upscale[0] ? upscale : "(default)");
#if defined(PS2X_IOS)
            auto pairs = set2ReadSettings();
            pairs = ps2x::set2::upsertSettings(pairs, "GE1_UPSCALE", upscale);
            pairs = ps2x::set2::upsertSettings(pairs, "PS2X_SETTINGS_PERF", set2Perf ? "1" : "0");
            if (!set2WriteSettings(pairs))
            {
                std::fprintf(stderr, "[set2] restart: settings write failed, staying open\n");
                ps2x::ui::toast("Restart failed (settings)", 3.0f);
                return;
            }
            ps2x::ios::setTarget120(want120);
            // iOS cannot relaunch itself: quit and let the user tap the icon
            // (turbo boot brings the menus back in ~1 s).
            std::fprintf(stderr, "[set2] restart: quitting, relaunch the app to apply\n");
            ps2_stubs::padRecordFlushNow("set2-restart");
            std::fflush(nullptr);
            std::_Exit(0);
#elif defined(__ANDROID__)
            auto pairs = set2ReadSettings();
            pairs = ps2x::set2::upsertSettings(pairs, "GE1_UPSCALE", upscale);
            pairs = ps2x::set2::upsertSettings(pairs, "PS2X_SETTINGS_PERF", set2Perf ? "1" : "0");
            if (!set2WriteSettings(pairs))
            {
                std::fprintf(stderr, "[set2] restart: settings write failed, staying open\n");
                ps2x::ui::toast("Restart failed (settings)", 3.0f);
                return;
            }
            if (!set2AndroidRelaunch(want120 ? "120" : "60"))
            {
                std::fprintf(stderr, "[set2] restart: relaunch failed, staying open\n");
                ps2x::ui::toast("Restart failed", 3.0f);
            }
            // On success the process dies inside toHz.
#else
#if defined(__APPLE__)
            if (!set2MacRestart(want120, upscale))
            {
                std::fprintf(stderr, "[set2] restart: re-exec failed, staying open\n");
                ps2x::ui::toast("Restart failed", 3.0f);
            }
#else
            // Desktop Linux has no settings-panel re-exec path.
            (void)want120;
            (void)upscale;
            std::fprintf(stderr, "[set2] restart: unsupported on this host, staying open\n");
            ps2x::ui::toast("Restart failed", 3.0f);
#endif
#endif
        };
        if (set2On)
        {
            if (!set2Test.empty() && (set2HostFrame % 600u) == 0u)
                std::fprintf(stderr, "[set2] hostFrame=%llu guestTick=%llu\n",
                             static_cast<unsigned long long>(set2HostFrame),
                             static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()));
            // Nav union: post-DS1 physical + the touch buttons when closed
            // (set2-polish-1: hold SELECT + tap START on the overlay is the
            // chord, one host frame behind the touch), true physical when
            // open (DS1 suppressed). The test spans are already in both
            // (injected at the sample site above).
            const uint16_t set2Union =
                set2Open ? ds1TrueRaw : static_cast<uint16_t>(raylibPressed | vpadPressedPrev);
            const int64_t set2NowMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            const bool set2WasHolding = set2Chord.holding;
            const int set2WasPulse = set2Chord.pulseLeft;
            const ps2x::set2::ChordOut set2ChordOut = ps2x::set2::chordStep(
                set2Chord, (set2Union & ps2x::vpad::kSelect) != 0u,
                (set2Union & ps2x::vpad::kStart) != 0u, set2NowMs);
            if (!set2Open)
            {
                if (set2ChordOut.stripSelect)
                    raylibPressed = static_cast<uint16_t>(raylibPressed & ~ps2x::vpad::kSelect);
                if (set2ChordOut.stripStart)
                    raylibPressed = static_cast<uint16_t>(raylibPressed & ~ps2x::vpad::kStart);
                if (set2ChordOut.addSelect)
                    raylibPressed = static_cast<uint16_t>(raylibPressed | ps2x::vpad::kSelect);
                // set2-polish-1: carry the same hold-back for the touch
                // path (applied at the vpad publish below, like the
                // close-press mask, so a touch Select never leaks ahead).
                set2VpadStrip = static_cast<uint16_t>(
                    (set2ChordOut.stripSelect ? ps2x::vpad::kSelect : 0u) |
                    (set2ChordOut.stripStart ? ps2x::vpad::kStart : 0u));
                set2VpadAdd = set2ChordOut.addSelect ? ps2x::vpad::kSelect : static_cast<uint16_t>(0u);
                // The close-press overhang drops out button by button as each
                // releases; new presses pass (they were never in the mask).
                const uint16_t set2Swallowed = ps2x::set2::swallowStep(set2Swallow, set2Union);
                if (set2Swallowed != 0u)
                    raylibPressed = static_cast<uint16_t>(raylibPressed & ~set2Swallowed);
                if (set2WasHolding && !set2Chord.holding && !set2ChordOut.openEdge &&
                    (set2Union & ps2x::vpad::kSelect) != 0u)
                    std::fprintf(stderr,
                                 "[set2] select forwarded (lone, +%dms) hostFrame=%llu guestTick=%llu\n",
                                 ps2x::set2::kChordWindowMs,
                                 static_cast<unsigned long long>(set2HostFrame),
                                 static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()));
                if (set2WasPulse == 0 && set2Chord.pulseLeft > 0)
                    std::fprintf(stderr, "[set2] select pulse hostFrame=%llu guestTick=%llu\n",
                                 static_cast<unsigned long long>(set2HostFrame),
                                 static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()));
                if (set2ChordOut.openEdge)
                    set2DoOpen();
            }
            else
            {
                const uint16_t set2Edge = static_cast<uint16_t>(set2Union & ~set2PrevUnion);
                const ps2x::set2::NavOut navOut = ps2x::set2::navStep(
                    set2Nav, (set2Edge & ps2x::vpad::kUp) != 0u,
                    (set2Edge & ps2x::vpad::kDown) != 0u,
                    (set2Edge & ps2x::vpad::kCross) != 0u,
                    (set2Edge & ps2x::vpad::kTriangle) != 0u, set2ChordOut.openEdge);
                if (navOut.action == ps2x::set2::Action::Changed &&
                    set2Nav.sel == ps2x::set2::Row::Perf)
                {
                    // Perf is live: apply + persist immediately.
                    set2Perf = set2Nav.perf;
                    auto pairs = set2ReadSettings();
                    pairs = ps2x::set2::upsertSettings(pairs, "PS2X_SETTINGS_PERF",
                                                       set2Perf ? "1" : "0");
                    set2WriteSettings(pairs); // false on the Mac (live-only there)
                }
                if (navOut.action == ps2x::set2::Action::Changed &&
                    set2Nav.sel == ps2x::set2::Row::Upscale)
                {
                    // REM3 P11 Phase 2: Upscale is live like Perf. The ordered
                    // host-GS-worker command carries it (FIFO after prior
                    // packets); the vendor applies + purges, no reboot.
                    set2Upscale = set2Nav.upscale;
                    m_gs.setOndeviceUpscale(set2Upscale);
                    auto pairs = set2ReadSettings();
                    pairs = ps2x::set2::upsertSettings(pairs, "GE1_ONDEVICE_UPSCALE",
                                                       set2Upscale ? "1" : "0");
                    set2WriteSettings(pairs); // false on the Mac (live-only there)
                }
                if (navOut.action == ps2x::set2::Action::Changed &&
                    set2Nav.sel == ps2x::set2::Row::Touch)
                {
                    // android-touch-1: Touch is live like Perf. Auto shows
                    // the overlay only with no gamepad; On/Off force it.
                    vpadTouchMode = set2Nav.touch;
                    vpadWanted = (vpadTouchMode != ps2x::set2::Touch::Off);
                    auto pairs = set2ReadSettings();
                    pairs = ps2x::set2::upsertSettings(pairs, "PS2X_VIRTUAL_PAD",
                                                       ps2x::set2::touchToSetting(vpadTouchMode));
                    set2WriteSettings(pairs); // false on the Mac (live-only there)
                }
                if (navOut.action == ps2x::set2::Action::Changed &&
                    set2Nav.sel == ps2x::set2::Row::MenuBtn)
                {
                    // settings-res-1: the "..." button is live like Perf
                    // (no restart; the next frame's draw + underlay read it).
                    auto pairs = set2ReadSettings();
                    pairs = ps2x::set2::upsertSettings(pairs, "PS2X_MENU_BUTTON",
                                                       ps2x::set2::menuBtnToSetting(set2Nav.menubtn));
                    set2WriteSettings(pairs); // false on the Mac (live-only there)
                }
                if (navOut.action != ps2x::set2::Action::None)
                    std::fprintf(stderr, "[set2] nav action=%d sel=%d mode=%d res=%d perf=%d upscale=%d touch=%d menubtn=%d\n",
                                 static_cast<int>(navOut.action), static_cast<int>(set2Nav.sel),
                                 static_cast<int>(set2Nav.mode), static_cast<int>(set2Nav.res),
                                 set2Nav.perf ? 1 : 0, set2Nav.upscale ? 1 : 0,
                                 static_cast<int>(set2Nav.touch), static_cast<int>(set2Nav.menubtn));
                if (navOut.action == ps2x::set2::Action::Close)
                    set2DoClose();
                else if (navOut.action == ps2x::set2::Action::DoRestart)
                    set2DoRestart();
            }
            set2PrevUnion = set2Union;
        }
        // tk-menu-1 Part 2: the Tricky course list. Knob off: the outer
        // bool only. While open the list owns the pad (the guest gets
        // zeros; navigator presses merge on the game thread): physical and
        // test-span buttons drive the list, never the guest.
        uint16_t tkGuestMask = 0u;
        bool tkPublishOwn = false;
        if (tkListOn && !set2Open)
        {
            namespace tkl = ps2_ssx3_tricky_list;
            const uint64_t tkTick = m_memory.gs().vsyncTick.load(std::memory_order_relaxed);
            bool tkMapLive = false;
            tkl::FeView tkView;
            if (ps2_ssx3_tricky::state().on && ps2_ssx3_tricky::state().tricky)
            {
                if (uint8_t *tkRam = m_memory.getRDRAM())
                {
                    tkView = tkl::readView(tkRam, ps2_ssx3_course::kRamSize,
                                           ps2_ssx3_tricky::state().mapController);
                    tkMapLive = tkView.mapLive;
                }
            }
            // Refined target (m7): the Map words go stale after the handoff
            // (sub stays 2 with a dead widget through loading, race and
            // results), so a live Event widget is required on screen 2. The
            // open/close evaluation suspends while a drive owns the guest.
            const tkl::DriveProgress tkProgEarly = tkl::driveProgress();
            const bool tkTarget =
                tkMapLive && (tkView.screen != 2 || tkView.row >= 0) &&
                (!tkLatchedOn || ps2_ssx3_tricky::state().mapEntries != tkLatchGen);
            if (!tkProgEarly.driving)
            {
                if (tkTarget)
                {
                    tkShutDeb = 0;
                    if (!tkOpen && ++tkOpenDeb >= 4)
                    {
                        tkOpen = true;
                        tkLatchedOn = false;
                        tkList.step = 0; // every open starts on the course list
                        tkList.mode = 0;
                        std::fprintf(stderr, "[tklist] tick=%llu open scr=%d peak=%u type=%u entries=%llu\n",
                                     static_cast<unsigned long long>(tkTick), tkView.screen, tkView.peak,
                                     tkView.type,
                                     static_cast<unsigned long long>(
                                         ps2_ssx3_tricky::state().mapEntries));
                    }
                }
                else
                {
                    tkOpenDeb = 0;
                    if (tkOpen && ++tkShutDeb >= 12)
                    {
                        tkOpen = false;
                        tkl::driveDisarm();
                        tkDown.clear();
                        tkLatchedOn = true;
                        tkLatchGen = ps2_ssx3_tricky::state().mapEntries;
                        std::fprintf(stderr, "[tklist] tick=%llu close\n",
                                     static_cast<unsigned long long>(tkTick));
                    }
                }
            }
            if (tkOpen)
            {
                tkPublishOwn = true; // zeros (driving edges merge on the game thread)
                const tkl::DriveProgress prog = tkl::driveProgress();
                if (prog.done)
                {
                    // Done either way closes the overlay and latches reopen
                    // on a new Map entry (m7: the words stay stale through
                    // loading/race/results; only a fresh entry re-arms).
                    tkl::driveDisarm();
                    if (tkOpen)
                    {
                        tkOpen = false;
                        tkDown.clear();
                        tkLatchedOn = true;
                        tkLatchGen = ps2_ssx3_tricky::state().mapEntries;
                        std::fprintf(stderr, "[tklist] tick=%llu close (%s)\n",
                                     static_cast<unsigned long long>(tkTick),
                                     prog.handedOff ? "handoff" : "backout");
                    }
                }
                else if (prog.aborted)
                {
                    tkl::driveDisarm();
                    ps2x::ui::toast("Tricky list: couldn't reach it", 3.0f);
                    std::fprintf(stderr, "[tklist] tick=%llu aborted after %d presses\n",
                                 static_cast<unsigned long long>(tkTick), prog.presses);
                }
                else if (prog.driving)
                {
                    // Any fresh press cancels the drive (the user takes over).
                    // Diagnostic: name the edge source (script span, tap echo,
                    // or walker) offline from tick + bits + list state. A real
                    // player's Back press must never be swallowed: no guard
                    // here, logging only.
                    const uint16_t tkFresh = static_cast<uint16_t>(ds1TrueRaw & ~tkPrevUnion);
                    if (tkFresh != 0u)
                    {
                        tkl::driveDisarm();
                        const tkl::CourseOption &tkCancelGoal = tkl::listGoal(tkList);
                        std::fprintf(stderr,
                                     "[tklist] tick=%llu cancel edge fresh=0x%04x prev=0x%04x "
                                     "goal=(%u,%u,%u evmap=%d) list(step%d sel%d mode%d)\n",
                                     static_cast<unsigned long long>(tkTick), tkFresh, tkPrevUnion,
                                     tkCancelGoal.peak, tkCancelGoal.type, tkCancelGoal.row,
                                     tkCancelGoal.map.event, tkList.step, tkList.sel, tkList.mode);
                    }
                }
                else if (tkTarget)
                {
                    // Browsing: pad edges drive the list (only on a live Map
                    // target; stale words arm nothing). Two steps: Cross on a
                    // course opens its mode list (single-mode courses ride
                    // directly); Cross on a mode rides; Triangle steps back
                    // (out of the overlay on step 0). Left/Right do nothing.
                    const auto tkArmRide = [&] {
                        const tkl::Course &c = tkl::kCourses[tkList.sel];
                        const tkl::CourseOption &tkGoal = tkl::listGoal(tkList);
                        tkl::driveArm(tkGoal, c.event, tkTick);
                        // Part 3: a mapped mode rides the donor event.
                        const int tkEv = (tkGoal.map.event >= 0) ? tkGoal.map.event : c.event;
                        std::fprintf(stderr, "[tklist] tick=%llu ride %s [%s] (peak %u type %u row %u ev %d)\n",
                                     static_cast<unsigned long long>(tkTick), c.name,
                                     c.modeName[tkList.step == 1 ? tkList.mode : 0], tkGoal.peak,
                                     tkGoal.type, tkGoal.row, tkEv);
                    };
                    const auto tkCross = [&] {
                        const tkl::Course &c = tkl::kCourses[tkList.sel];
                        if (tkList.step == 0 && c.nOpts > 1)
                        {
                            tkList.step = 1;
                            tkList.mode = 0;
                        }
                        else
                            tkArmRide();
                    };
                    const auto tkTriangle = [&] {
                        if (tkList.step == 1)
                        {
                            tkList.step = 0;
                            tkList.mode = 0;
                        }
                        else
                            tkl::driveArmBackOut(tkTick);
                    };
                    const uint16_t tkEdge = static_cast<uint16_t>(ds1TrueRaw & ~tkPrevUnion);
                    if (tkEdge & ps2x::vpad::kUp)
                        tkl::listMove(tkList, true);
                    else if (tkEdge & ps2x::vpad::kDown)
                        tkl::listMove(tkList, false);
                    if (tkEdge & ps2x::vpad::kTriangle)
                        tkTriangle();
                    else if (tkEdge & ps2x::vpad::kCross)
                        tkCross();
                    // Touch: row taps highlight (a tap on the highlighted
                    // row advances), select/back legends act per step.
                    const tkl::ListGeom tkGeom = tkl::listLayout(screenWidth, screenHeight);
                    ps2x::vpad::TouchPoint tkts[8];
                    const int tkn = set2SampleTouches(tkts);
                    std::vector<int64_t> tkStill;
                    for (int i = 0; i < tkn; ++i)
                    {
                        tkStill.push_back(tkts[i].id);
                        if (std::find(tkDown.begin(), tkDown.end(), tkts[i].id) != tkDown.end())
                            continue;
                        tkDown.push_back(tkts[i].id);
                        const int hit = tkl::listHit(tkGeom, tkts[i].x, tkts[i].y);
                        if (tkList.step == 0)
                        {
                            if (hit >= 0 && hit <= 9)
                            {
                                if (tkList.sel == hit)
                                    tkCross();
                                else
                                    tkList.sel = static_cast<uint8_t>(hit);
                            }
                            else if (hit == 11)
                                tkCross();
                            else if (hit == 12)
                                tkTriangle();
                        }
                        else
                        {
                            const int nModes = tkl::kCourses[tkList.sel].nOpts;
                            if (hit >= 0 && hit < nModes)
                            {
                                if (tkList.mode == hit)
                                    tkArmRide();
                                else
                                    tkList.mode = static_cast<uint8_t>(hit);
                            }
                            else if (hit == 11)
                                tkArmRide();
                            else if (hit == 12)
                                tkTriangle();
                        }
                    }
                    std::vector<int64_t> tkLeft;
                    for (int64_t id : tkDown)
                        if (std::find(tkStill.begin(), tkStill.end(), id) != tkStill.end())
                            tkLeft.push_back(id);
                    tkDown = tkLeft;
                }
                tkPrevUnion = ds1TrueRaw;
            }
            else
                tkPrevUnion = ds1TrueRaw;
        }
        else if (tkListOn)
        {
            // The settings panel took over (or the knob block was skipped):
            // track the union so no stale edges fire, and close the list
            // (its drive disarms; the guest is paused under the panel).
            tkPrevUnion = ds1TrueRaw;
            if (tkOpen)
            {
                tkOpen = false;
                ps2_ssx3_tricky_list::driveDisarm();
                tkDown.clear();
            }
        }
        // SET2: no vpad path this frame (Odin play, desktop, or a controller
        // in use). set2-polish-1 (Brad 10-16): the gear dot is gone — a
        // two-finger tap opens the panel instead (touches here never reach
        // the guest, so gameplay cannot trip it). This keeps Touch=Off
        // phones (no overlay, no pad) able to reach the panel at all.
        if (set2On && !set2Open && !(vpadWanted && !vpadPadConnected))
        {
            ps2x::vpad::TouchPoint set2ts[8];
            const int set2n = set2SampleTouches(set2ts);
            if (set2n >= 2)
                set2DoOpen();
            else if (ps2x::set2::menuButtonShown(set2Nav.menubtn))
            {
                // settings-res-1: a fresh tap on the "..." button (top
                // middle) opens the panel. Touches here never reach the
                // guest; held ids are tracked below so a finger down
                // across close never re-opens.
                const auto set2mb = ps2x::set2::menuButtonLayout(screenWidth, screenHeight);
                for (int i = 0; i < set2n && !set2Open; ++i)
                {
                    if (ps2x::set2::menuButtonHit(set2mb, set2ts[i].x, set2ts[i].y) &&
                        std::find(set2MenuIds.begin(), set2MenuIds.end(), set2ts[i].id) ==
                            set2MenuIds.end())
                        set2DoOpen();
                }
            }
            set2MenuIds.clear();
            for (int i = 0; i < set2n; ++i)
                set2MenuIds.push_back(set2ts[i].id);
        }
        if (tkPublishOwn)
        {
            // tk-menu-1: the list owns the pad. The guest gets zeros (the
            // navigator's presses merge on the game thread); the overlay
            // draws full-screen over the presented frame, never into it.
            publishPad(0, m_memory.gs().vsyncTick.load());
            ps2x::vpad::liveStick().store(ps2x::vpad::kStickNoOverride, std::memory_order_relaxed);
            if (!skipGl)
            {
                namespace tkl = ps2_ssx3_tricky_list;
                const tkl::ListGeom tkg = tkl::listLayout(screenWidth, screenHeight);
                const int tkTitle = std::max(18, static_cast<int>(screenHeight / 24.0f));
                const int tkRow = std::max(14, static_cast<int>(screenHeight / 32.0f));
                const int tkSmall = std::max(12, static_cast<int>(screenHeight / 44.0f));
                // Staged art atlas (PS2X_SSX3_TRICKY_LIST_ART): disc-only art
                // (FE_1 mountain + TP previews) baked offline by
                // tools/ssx3/build_tk_list_art.py (TKLIST1, art only, no
                // font strings). Missing file = flat shapes; text always
                // comes from the font module below, never the atlas.
                struct TkAtlasRect { float x, y, w, h; };
                static Texture2D tkAtlasTex = {0};
                static std::string tkAtlasPixels;
                static std::map<std::string, TkAtlasRect> tkAtlasRects;
                static bool tkAtlasTried = false;
                if (!tkAtlasTried)
                {
                    tkAtlasTried = true;
                    if (const char *art = std::getenv("PS2X_SSX3_TRICKY_LIST_ART"))
                    {
                        std::ifstream f(art, std::ios::binary);
                        const std::string blob((std::istreambuf_iterator<char>(f)),
                                               std::istreambuf_iterator<char>());
                        const uint8_t *d = reinterpret_cast<const uint8_t *>(blob.data());
                        const size_t n = blob.size();
                        auto u32 = [&](size_t o) -> uint32_t {
                            uint32_t v = 0;
                            if (o + 4 <= n)
                                std::memcpy(&v, d + o, 4);
                            return v;
                        };
                        if (n >= 20 && std::memcmp(d, "TKLIST1\0", 8) == 0)
                        {
                            const uint32_t aw = u32(8), ah = u32(12), nr = u32(16);
                            const size_t px = 20 + static_cast<size_t>(nr) * 40;
                            if (aw >= 64 && aw <= 4096 && ah >= 64 && ah <= 4096 && nr > 0 && nr <= 256 &&
                                px <= n && px + static_cast<size_t>(aw) * ah * 4 == n)
                            {
                                bool ok = true;
                                std::map<std::string, TkAtlasRect> rects;
                                for (uint32_t i = 0; ok && i < nr; ++i)
                                {
                                    const size_t ro = 20 + static_cast<size_t>(i) * 40;
                                    const char *nm = reinterpret_cast<const char *>(d + ro);
                                    if (std::memchr(nm, '\0', 24) == nullptr)
                                    {
                                        ok = false;
                                        break;
                                    }
                                    const float rx = static_cast<float>(u32(ro + 24));
                                    const float ry = static_cast<float>(u32(ro + 28));
                                    const float rw = static_cast<float>(u32(ro + 32));
                                    const float rh = static_cast<float>(u32(ro + 36));
                                    if (rw <= 0 || rh <= 0 || rx < 0 || ry < 0 || rx + rw > aw || ry + rh > ah)
                                    {
                                        ok = false;
                                        break;
                                    }
                                    rects[nm] = TkAtlasRect{rx, ry, rw, rh};
                                }
                                if (ok && rects.count("moun"))
                                {
                                    tkAtlasPixels.assign(reinterpret_cast<const char *>(d + px),
                                                         static_cast<size_t>(aw) * ah * 4);
                                    Image img{};
                                    img.data = tkAtlasPixels.data();
                                    img.width = static_cast<int>(aw);
                                    img.height = static_cast<int>(ah);
                                    img.mipmaps = 1;
                                    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                                    tkAtlasTex = LoadTextureFromImage(img);
                                    tkAtlasRects = rects;
                                }
                            }
                        }
                    }
                }
                const auto tkSpr = [&](const char *name, TkAtlasRect &r) -> bool {
                    const auto it = tkAtlasRects.find(name);
                    if (it == tkAtlasRects.end())
                        return false;
                    r = it->second;
                    return true;
                };
                const bool tkArt = (tkAtlasTex.id != 0u);
                // tk-menu-2 font (shared module, ps2_tk_font.h): staged
                // FEFONT primary (TKFONT1 baked beside the ART atlas as
                // tkfont.bin: stock 1x raster, raster-edge 4x when upscale
                // is on), Neuropol CC0 TTF fallback, raylib default last
                // resort. One knob (ART) covers both staged files.
                namespace tkf = ps2_tk_font;
                static tkf::Baked tkFontBaked;
                static Texture2D tkFontTex1 = {0}, tkFontTex4 = {0};
                static std::string tkFontPx1, tkFontPx4;
                static bool tkFontTried = false;
                static Font tkNeuro = {0};
                static bool tkNeuroTried = false, tkNeuroOk = false, tkNeuroLogged = false;
                if (!tkFontTried)
                {
                    tkFontTried = true;
                    // tkfont.bin sits beside the ART atlas (same bake dir).
                    std::string fd;
                    if (const char *art = std::getenv("PS2X_SSX3_TRICKY_LIST_ART"))
                    {
                        std::string a(art);
                        const size_t sl = a.find_last_of('/');
                        fd = (sl == std::string::npos ? std::string("tkfont.bin")
                                                      : a.substr(0, sl) + "/tkfont.bin");
                    }
                    if (!fd.empty())
                    {
                        std::ifstream f(fd, std::ios::binary);
                        const std::string blob((std::istreambuf_iterator<char>(f)),
                                               std::istreambuf_iterator<char>());
                        tkf::Baked b;
                        if (tkf::parseTkFont(reinterpret_cast<const uint8_t *>(blob.data()), blob.size(), b))
                        {
                            tkFontBaked = b;
                            tkFontPx1 = b.px1;
                            tkFontPx4 = b.px4;
                            Image i1{};
                            i1.data = tkFontPx1.data();
                            i1.width = static_cast<int>(b.w1);
                            i1.height = static_cast<int>(b.h1);
                            i1.mipmaps = 1;
                            i1.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                            tkFontTex1 = LoadTextureFromImage(i1);
                            Image i4{};
                            i4.data = tkFontPx4.data();
                            i4.width = static_cast<int>(b.w4);
                            i4.height = static_cast<int>(b.h4);
                            i4.mipmaps = 1;
                            i4.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                            tkFontTex4 = LoadTextureFromImage(i4);
                        }
                        else
                            std::fprintf(stderr, "[tklist] font data unparseable, Neuropol fallback\n");
                    }
                }
                if (!tkNeuroTried)
                {
                    tkNeuroTried = true;
                    std::string found;
                    std::vector<std::string> tkDirs;
#if defined(__APPLE__)
                    {
                        char buf[4096];
                        uint32_t s = sizeof(buf);
                        if (_NSGetExecutablePath(buf, &s) == 0)
                        {
                            std::string p(buf);
                            const size_t sl = p.find_last_of('/');
                            tkDirs.push_back(sl == std::string::npos ? "." : p.substr(0, sl));
                        }
                    }
#elif defined(__linux__) && !defined(__ANDROID__)
                    {
                        std::error_code ec;
                        auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
                        if (!ec)
                            tkDirs.push_back(p.parent_path().string());
                    }
#endif
                    tkDirs.push_back(".");
                    const char *cands[] = {"fonts/Neuropol-CC0.otf", "Neuropol-CC0.otf",
                                           "ps2xRuntime/resources/fonts/Neuropol-CC0.otf",
                                           "../ps2xRuntime/resources/fonts/Neuropol-CC0.otf"};
                    for (const std::string &d : tkDirs)
                        for (const char *c : cands)
                        {
                            const std::string p = d + "/" + c;
                            std::ifstream f(p, std::ios::binary);
                            if (f.good())
                            {
                                found = p;
                                break;
                            }
                        }
                    if (!found.empty())
                    {
                        tkNeuro = LoadFontEx(found.c_str(), 128, nullptr, 0);
                        tkNeuroOk = (tkNeuro.glyphCount > 0 && tkNeuro.texture.id != 0u);
                    }
                    if (!tkNeuroOk && !tkNeuroLogged)
                    {
                        tkNeuroLogged = true;
                        std::fprintf(stderr, "[tklist] Neuropol missing, raylib default font\n");
                    }
                }
                const char *tkOdu = std::getenv("GE1_ONDEVICE_UPSCALE");
                const bool tkUpscale = (tkOdu && std::strcmp(tkOdu, "1") == 0);
                const bool tkUseUp = tkFontBaked.ok && (tkFontTex1.id != 0u || tkFontTex4.id != 0u);
                // Upload-font quad draw (face + down/right shadow, in-game
                // layering); returns drawn width. yTop = top of the ink band.
                const auto tkUpDrawK = [&](const char *s, float x, float yTop, float hPx, float k,
                                           float topRef, Color tint, bool right) -> float {
                    if (k <= 0.0f)
                        return 0.0f;
                    const tkf::Metrics m = tkf::measure(tkFontBaked, s);
                    float pen = right ? x - m.width * k : x;
                    const float x0 = pen;
                    Texture2D &tex = (tkUpscale && tkFontTex4.id != 0u) ? tkFontTex4 : tkFontTex1;
                    const float mult = (&tex == &tkFontTex4) ? 4.0f : 1.0f;
                    const float sh = std::max(1.0f, std::round(hPx / 16.0f));
                    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p)
                    {
                        const tkf::Glyph *g = tkf::findGlyph(tkFontBaked, *p);
                        if (!g)
                            continue;
                        if (g->w > 0 && g->h > 0)
                        {
                            const Rectangle src{static_cast<float>(g->x) * mult, static_cast<float>(g->y) * mult,
                                                static_cast<float>(g->w) * mult, static_cast<float>(g->h) * mult};
                            const Rectangle dst{pen + static_cast<float>(g->xoff) * k,
                                                yTop + (static_cast<float>(g->yoff) - topRef) * k,
                                                static_cast<float>(g->w) * k, static_cast<float>(g->h) * k};
                            const Rectangle dsh{dst.x + sh, dst.y + sh, dst.width, dst.height};
                            DrawTexturePro(tex, src, dsh, Vector2{0, 0}, 0.0f, Color{0, 0, 0, 160});
                            DrawTexturePro(tex, src, dst, Vector2{0, 0}, 0.0f, tint);
                        }
                        pen += static_cast<float>(g->adv) * k;
                    }
                    return pen - x0;
                };
                const auto tkUpDraw = [&](const char *s, float x, float yTop, float hPx, Color tint,
                                          bool right) -> float {
                    const tkf::Metrics m = tkf::measure(tkFontBaked, s);
                    return tkUpDrawK(s, x, yTop, hPx, tkf::scaleFor(m, hPx), m.top, tint, right);
                };
                const auto tkTxDraw = [&](const char *s, float x, float yTop, float hPx, Color tint,
                                          bool right) -> float {
                    if (tkNeuroOk)
                    {
                        const float sp = hPx * 0.06f;
                        const Vector2 sz = MeasureTextEx(tkNeuro, s, hPx, sp);
                        DrawTextEx(tkNeuro, s, Vector2{right ? x - sz.x : x, yTop}, hPx, sp, tint);
                        return sz.x;
                    }
                    const int px = static_cast<int>(hPx + 0.5f);
                    const int w = MeasureText(s, px);
                    DrawText(s, static_cast<int>(right ? x - static_cast<float>(w) : x),
                             static_cast<int>(yTop + 0.5f), px, tint);
                    return static_cast<float>(w);
                };
                const auto tkDraw = [&](const char *s, float x, float yTop, float hPx, Color tint,
                                        bool right) -> float {
                    return tkUseUp ? tkUpDraw(s, x, yTop, hPx, tint, right)
                                   : tkTxDraw(s, x, yTop, hPx, tint, right);
                };
                // Part 2 iOS: the FBO path below drops every draw on iOS GLES
                // (browse5: open fired, zero pixels composited, no shot file);
                // direct draws composite there (vpad/SET2 prove it), and the
                // device screenshot is the oracle. Mac keeps the FBO: the
                // mini is headless, so host screenshots come from this
                // texture (blitted below), not the window.
#if defined(PS2X_IOS)
                const bool tkDirect = true;
#else
                const bool tkDirect = false;
#endif
                {
                    static bool tkDrawLogged = false;
                    if (!tkDrawLogged)
                    {
                        tkDrawLogged = true;
                        std::fprintf(stderr,
                                     "[tklist] draw %s %dx%d art=%d font=%d neuro=%d upscale=%d\n",
                                     tkDirect ? "direct" : "fbo", static_cast<int>(screenWidth),
                                     static_cast<int>(screenHeight), tkArt ? 1 : 0,
                                     tkUseUp ? 1 : 0, tkNeuroOk ? 1 : 0, tkUpscale ? 1 : 0);
                    }
                }
                const int tkTw = static_cast<int>(screenWidth), tkTh = static_cast<int>(screenHeight);
                static RenderTexture2D tkTex = {0};
                static int tkTexW = 0, tkTexH = 0;
                if (!tkDirect && (tkTex.id == 0u || tkTexW != tkTw || tkTexH != tkTh))
                {
                    if (tkTex.id != 0u)
                        UnloadRenderTexture(tkTex);
                    tkTex = LoadRenderTexture(tkTw, tkTh);
                    tkTexW = tkTw;
                    tkTexH = tkTh;
                }
                if (!tkDirect)
                    BeginTextureMode(tkTex);
                // Backdrop: staged disc mountain art, dimmed for legibility.
                if (tkArt)
                {
                    TkAtlasRect tkm;
                    if (tkSpr("moun", tkm))
                        DrawTexturePro(tkAtlasTex, Rectangle{tkm.x, tkm.y, tkm.w, tkm.h},
                                       Rectangle{0, 0, screenWidth, screenHeight}, Vector2{0, 0}, 0.0f,
                                       WHITE);
                    else
                        DrawRectangle(0, 0, static_cast<int>(screenWidth), static_cast<int>(screenHeight),
                                      Color{10, 18, 38, 255});
                    DrawRectangle(0, 0, static_cast<int>(screenWidth), static_cast<int>(screenHeight),
                                  Color{6, 10, 24, 120});
                }
                else
                {
                    DrawRectangle(0, 0, static_cast<int>(screenWidth), static_cast<int>(screenHeight),
                                  Color{10, 18, 38, 255});
                    DrawRectangle(0, 0, static_cast<int>(screenWidth),
                                  static_cast<int>(tkg.titleY + tkg.titleH), Color{16, 28, 56, 255});
                }
                // Title bar (stock bar colour 154,62,2 sampled D t21250).
                // Part 2: title and subtitle each get their own band inside
                // the bar (title 8..48%, subtitle 52..76% of the bar height),
                // so nothing overlaps, spills, or clips at any window size.
                // Text via the font module (FEFONT upload primary, Neuropol
                // fallback); near-white face like the stock selected row.
                DrawRectangle(static_cast<int>(tkg.titleX), static_cast<int>(tkg.titleY),
                              static_cast<int>(tkg.titleW), static_cast<int>(tkg.titleH), Color{154, 62, 2, 255});
                {
                    // Step 1 re-titles the same bar (geometry unchanged).
                    const char *tkTitle = (tkList.step == 1) ? "Select Mode" : "Select Event";
                    const char *tkSub = (tkList.step == 1) ? tkl::kCourses[tkList.sel].name
                                                           : "SSX Tricky courses in Tricky order";
                    tkDraw(tkTitle, tkg.titleX + 12, tkg.titleY + tkg.titleH * 0.08f,
                           tkg.titleH * 0.40f, Color{250, 248, 244, 255}, false);
                    tkDraw(tkSub, tkg.titleX + 14,
                           tkg.titleY + tkg.titleH * 0.52f, tkg.titleH * 0.24f,
                           Color{200, 210, 225, 255}, false);
                }
                // Course rows, Part 2 amendment: step 0 lists the 10 course
                // names (ONE uniform size, left-aligned, no tags); step 1
                // lists the course's own modes top-aligned (data-driven:
                // whatever modeName holds). Glyph colour matches stock: the
                // selected row's near-white face (sampled D t21250: face
                // pixels > 225 on the orange bar); unselected rows a dimmer
                // white on the dark boxes (stock's dark-on-light rows don't
                // apply to this dark-row design).
                const auto tkW = [&](const char *s, float hPx) -> float {
                    if (tkUseUp)
                    {
                        const tkf::Metrics m = tkf::measure(tkFontBaked, s);
                        return m.width * tkf::scaleFor(m, hPx);
                    }
                    if (tkNeuroOk)
                        return MeasureTextEx(tkNeuro, s, hPx, hPx * 0.06f).x;
                    const int px = static_cast<int>(hPx + 0.5f);
                    return static_cast<float>(MeasureText(s, px));
                };
                const tkl::Course &tkStepCourse = tkl::kCourses[tkList.sel];
                const char *tkRows[10] = {};
                int tkNRows = 0;
                int tkRowSel = 0;
                if (tkList.step == 0)
                {
                    for (int i = 0; i < 10; ++i)
                        tkRows[i] = tkl::kCourses[i].name;
                    tkNRows = 10;
                    tkRowSel = tkList.sel;
                }
                else
                {
                    for (int k = 0; k < tkStepCourse.nOpts; ++k)
                        tkRows[k] = tkStepCourse.modeName[k] ? tkStepCourse.modeName[k] : "?";
                    tkNRows = tkStepCourse.nOpts;
                    tkRowSel = tkList.mode;
                }
                // Uniform rows: one string-independent scale for every row, so a
                // descender ('y' in "Elysium Alps") cannot shrink its row next
                // to descender-free neighbours ("Garibaldi"). Same top
                // reference keeps baselines aligned; title/pane/help keep the
                // per-string fit (unchanged pixels there).
                float tkRowS = tkg.rowH * 0.58f;
                float tkRowTop = 0.0f;
                if (tkUseUp)
                {
                    float tkRowBot = 10.0f;
                    tkf::refBand(tkFontBaked, tkRowTop, tkRowBot);
                }
                for (int pass = 0; pass < 2; ++pass)
                {
                    const float tkRowK = tkUseUp ? tkf::scaleUniform(tkFontBaked, tkRowS) : 0.0f;
                    float nameW = 0.0f;
                    for (int i = 0; i < tkNRows; ++i)
                    {
                        if (tkUseUp)
                            nameW = std::max(nameW,
                                             tkf::measure(tkFontBaked, tkRows[i]).width * tkRowK);
                        else
                            nameW = std::max(nameW, tkW(tkRows[i], tkRowS));
                    }
                    const float avail = tkg.rowW - 20.0f;
                    if (nameW > avail && nameW > 0.0f)
                        tkRowS *= avail / nameW;
                }
                const float tkRowK = tkUseUp ? tkf::scaleUniform(tkFontBaked, tkRowS) : 0.0f;
                for (int i = 0; i < tkNRows; ++i)
                {
                    const bool sel = (tkRowSel == i);
                    DrawRectangle(static_cast<int>(tkg.rowX), static_cast<int>(tkg.rowY[i]),
                                  static_cast<int>(tkg.rowW), static_cast<int>(tkg.rowH) - 2,
                                  sel ? Color{154, 62, 2, 255} : Color{24, 36, 62, 255});
                    const Color tkInk = sel ? Color{250, 248, 244, 255} : Color{205, 212, 224, 255};
                    const float tkRy = tkg.rowY[i] + (tkg.rowH - 2 - tkRowS) * 0.5f;
                    if (tkUseUp)
                        tkUpDrawK(tkRows[i], tkg.rowX + 10, tkRy, tkRowS, tkRowK, tkRowTop,
                                  tkInk, false);
                    else
                        tkDraw(tkRows[i], tkg.rowX + 10, tkRy, tkRowS, tkInk, false);
                }
                // Preview/event pane.
                DrawRectangle(static_cast<int>(tkg.paneX), static_cast<int>(tkg.paneY),
                              static_cast<int>(tkg.paneW), static_cast<int>(tkg.paneH), Color{16, 28, 56, 255});
                DrawRectangleLines(static_cast<int>(tkg.paneX), static_cast<int>(tkg.paneY),
                                   static_cast<int>(tkg.paneW), static_cast<int>(tkg.paneH), Color{154, 62, 2, 255});
                {
                    const tkl::Course &c = tkl::kCourses[tkList.sel];
                    // Step 0 names every mode the course has (data-driven);
                    // step 1 the highlighted one.
                    std::string tkModeList;
                    for (int m = 0; m < c.nOpts; ++m)
                    {
                        if (m)
                            tkModeList += " / ";
                        tkModeList += c.modeName[m] ? c.modeName[m] : "?";
                    }
                    const char *etype = (tkList.step == 1 && c.modeName[tkList.mode])
                                            ? c.modeName[tkList.mode]
                                            : tkModeList.c_str();
                    // Preview quad (Megaplex sel 8: text card, no preview).
                    float tkPy = tkg.paneY + 8;
                    char tkpn[16];
                    std::snprintf(tkpn, sizeof(tkpn), "prev%d", tkList.sel);
                    TkAtlasRect tkpr;
                    const float tkq = std::min(tkg.paneW - 20, tkg.paneH * 0.40f);
                    if (tkArt && tkSpr(tkpn, tkpr))
                        DrawTexturePro(tkAtlasTex, Rectangle{tkpr.x, tkpr.y, tkpr.w, tkpr.h},
                                       Rectangle{tkg.paneX + 10, tkPy, tkq, tkq}, Vector2{0, 0}, 0.0f,
                                       WHITE);
                    else
                        tkDraw("No preview on disc", tkg.paneX + 10, tkPy + tkq * 0.4f,
                               static_cast<float>(tkSmall) * 1.6f, Color{200, 210, 225, 255}, false);
                    tkPy += tkq + 6;
                    const float tkEh = static_cast<float>(tkRow) * 0.9f;
                    tkDraw(etype, tkg.paneX + 10, tkPy, tkEh, Color{255, 170, 60, 255}, false);
                    tkPy += tkEh;
                    const float tkCh = static_cast<float>(tkTitle) * 0.8f;
                    tkDraw(c.name, tkg.paneX + 10, tkPy + 2, tkCh, RAYWHITE, false);
                    // No Peak/event line (Brad 10-16: not needed) and no
                    // driving/debug line in the default render (the lane
                    // diag knobs are gone with tk-menu-1).
                }
                // Help bar (font module; was baked legend sprites).
                tkDraw(tkList.step == 1 ? "X Ride   Triangle Courses   Up/Down pick mode"
                                        : "X Select   Triangle Previous   Up/Down pick course",
                       tkg.titleX, screenHeight * 0.905f, static_cast<float>(tkSmall), RAYWHITE, false);
                if (!tkDirect)
                {
                    EndTextureMode();
                    DrawTextureRec(tkTex.texture,
                                   Rectangle{0.0f, 0.0f, static_cast<float>(tkTw), static_cast<float>(-tkTh)},
                                   Vector2{0.0f, 0.0f}, WHITE);
                }
                if (tkShotPrefix && !tkDirect)
                {
                    static uint64_t tkShotTick = 0;
                    if ((tkShotTick++ % 300u) == 0u)
                    {
                        char shotPath[512];
                        std::snprintf(shotPath, sizeof(shotPath), "%s-%llu.png", tkShotPrefix,
                                      static_cast<unsigned long long>(tkShotTick));
                        Image tkShotImg = LoadImageFromTexture(tkTex.texture);
                        ImageFlipVertical(&tkShotImg);
                        ExportImage(tkShotImg, shotPath);
                        UnloadImage(tkShotImg);
                    }
                }
            }
        }
        else if (set2Open)
        {
            // SET2: the panel owns the pad. Touches hit-test the panel (row
            // taps act as X on that row, taps outside close); nothing
            // reaches the guest (publish-zero, frozen anyway).
            const ps2x::set2::PanelGeom set2g = ps2x::set2::panelLayout(screenWidth, screenHeight);
            ps2x::vpad::TouchPoint set2ts[8];
            const int set2n = set2SampleTouches(set2ts);
            std::vector<int64_t> set2Still;
            for (int i = 0; i < set2n && set2Open; ++i)
            {
                set2Still.push_back(set2ts[i].id);
                if (std::find(set2Down.begin(), set2Down.end(), set2ts[i].id) != set2Down.end())
                    continue;
                set2Down.push_back(set2ts[i].id);
                const int hit = ps2x::set2::panelHit(set2g, true, set2ts[i].x, set2ts[i].y);
                if (hit >= 0 && hit < ps2x::set2::kRowCount)
                {
                    // settings-res-1: tapping a different row selects it and
                    // disarms (like pad Up/Down); tapping the armed dirty row
                    // again is the restart confirm (navStep decides).
                    if (hit != static_cast<int>(set2Nav.sel))
                    {
                        set2Nav.sel = static_cast<ps2x::set2::Row>(hit);
                        set2Nav.restartArmed = false;
                    }
                    const ps2x::set2::NavOut tapOut =
                        ps2x::set2::navStep(set2Nav, false, false, true, false, false);
                    if (tapOut.action == ps2x::set2::Action::Changed &&
                        set2Nav.sel == ps2x::set2::Row::Perf)
                    {
                        set2Perf = set2Nav.perf;
                        auto pairs = set2ReadSettings();
                        pairs = ps2x::set2::upsertSettings(pairs, "PS2X_SETTINGS_PERF",
                                                           set2Perf ? "1" : "0");
                        set2WriteSettings(pairs);
                    }
                    if (tapOut.action == ps2x::set2::Action::Changed &&
                        set2Nav.sel == ps2x::set2::Row::Upscale)
                    {
                        // REM3 P11 Phase 2: touch tap, same live command as the pad path.
                        set2Upscale = set2Nav.upscale;
                        m_gs.setOndeviceUpscale(set2Upscale);
                        auto pairs = set2ReadSettings();
                        pairs = ps2x::set2::upsertSettings(pairs, "GE1_ONDEVICE_UPSCALE",
                                                           set2Upscale ? "1" : "0");
                        set2WriteSettings(pairs);
                    }
                    if (tapOut.action == ps2x::set2::Action::Changed &&
                        set2Nav.sel == ps2x::set2::Row::Touch)
                    {
                        // android-touch-1: touch tap, same live apply as the pad path.
                        vpadTouchMode = set2Nav.touch;
                        vpadWanted = (vpadTouchMode != ps2x::set2::Touch::Off);
                        auto pairs = set2ReadSettings();
                        pairs = ps2x::set2::upsertSettings(pairs, "PS2X_VIRTUAL_PAD",
                                                           ps2x::set2::touchToSetting(vpadTouchMode));
                        set2WriteSettings(pairs);
                    }
                    if (tapOut.action == ps2x::set2::Action::Changed &&
                        set2Nav.sel == ps2x::set2::Row::MenuBtn)
                    {
                        // settings-res-1: touch tap, same live persist as the pad path.
                        auto pairs = set2ReadSettings();
                        pairs = ps2x::set2::upsertSettings(pairs, "PS2X_MENU_BUTTON",
                                                           ps2x::set2::menuBtnToSetting(set2Nav.menubtn));
                        set2WriteSettings(pairs);
                    }
                    std::fprintf(stderr, "[set2] tap row=%d action=%d\n", hit,
                                 static_cast<int>(tapOut.action));
                    if (tapOut.action == ps2x::set2::Action::Close)
                        set2DoClose();
                    else if (tapOut.action == ps2x::set2::Action::DoRestart)
                        set2DoRestart();
                }
                else
                    set2DoClose(); // taps outside the rows close
            }
            if (set2Open)
            {
                std::vector<int64_t> set2Left;
                for (int64_t id : set2Down)
                    if (std::find(set2Still.begin(), set2Still.end(), id) != set2Still.end())
                        set2Left.push_back(id);
                set2Down = set2Left;
            }
            publishPad(0, m_memory.gs().vsyncTick.load());
            ps2x::vpad::liveStick().store(ps2x::vpad::kStickNoOverride, std::memory_order_relaxed);
            vpadPressedPrev = 0u; // set2-polish-1: panel owns the pad while open
        }
        else if (vpadWanted && !vpadPadConnected)
        {
            const ps2x::vpad::Layout layout = ps2x::vpad::makeLayout(screenWidth, screenHeight);
            ps2x::vpad::TouchPoint vpadTouches[8];
            int touches = virtualPadTouches(vpadTouches, 8, screenWidth, screenHeight);
            touches = ps2x::vpad::activeTestTouchesWithIds(vpadTestTouches, m_memory.gs().vsyncTick.load(), screenWidth,
                                                           screenHeight, vpadTouches, touches, 8);
            // A finger held while the panel closes must not become a fresh
            // virtual-pad press. Keep its ID masked until it leaves the screen.
            if (set2On && !set2HeldTouches.empty())
            {
                std::vector<int64_t> stillHeld;
                int kept = 0;
                for (int i = 0; i < touches; ++i)
                {
                    const int64_t id = vpadTouches[i].id;
                    if (std::find(set2HeldTouches.begin(), set2HeldTouches.end(), id) !=
                        set2HeldTouches.end())
                        stillHeld.push_back(id);
                    else
                        vpadTouches[kept++] = vpadTouches[i];
                }
                touches = kept;
                set2HeldTouches = stillHeld;
            }
            // settings-res-1: the "..." menu button (top middle) opens the
            // panel. Fresh taps on it never reach the pad (no stick anchor,
            // no press); held ids are tracked so a finger down across close
            // never re-opens.
            if (set2On && !set2Open)
            {
                const auto set2mb = ps2x::set2::menuButtonLayout(screenWidth, screenHeight);
                const bool menuShown = ps2x::set2::menuButtonShown(set2Nav.menubtn);
                std::vector<int64_t> menuIds;
                int kept = 0;
                for (int i = 0; i < touches; ++i)
                {
                    menuIds.push_back(vpadTouches[i].id);
                    if (menuShown &&
                        ps2x::set2::menuButtonHit(set2mb, vpadTouches[i].x, vpadTouches[i].y))
                    {
                        if (std::find(set2MenuIds.begin(), set2MenuIds.end(), vpadTouches[i].id) ==
                            set2MenuIds.end())
                            set2DoOpen();
                        if (set2Open)
                            break;
                        continue;
                    }
                    vpadTouches[kept++] = vpadTouches[i];
                }
                touches = kept;
                set2MenuIds = menuIds;
            }
            ps2x::vpad::PadFrame vpadFrame =
                ps2x::vpad::updatePad(vpadPad, layout, vpadTouches, touches, vpadShoulderBand);
            // set2-polish-1 (Brad 10-16): the gear dot is gone — every touch
            // here drives the game, and hold-SELECT + tap-START on the
            // overlay is the panel chord (fed from last frame's mask above).
            // The chord's hold-back applies here like on the physical path.
            uint16_t pressed = static_cast<uint16_t>(
                (vpadFrame.pressed & ~set2VpadStrip) | set2VpadAdd);
            // Raw (pre-hold-back) for the chord union above: the union reads
            // the pre-strip mask on the physical path too, so a held touch
            // SELECT sustains the window instead of pulsing every frame.
            vpadPressedPrev = vpadFrame.pressed;
            // IN4: touch-source layer (transition only, zero cost off).
            ps2x::inputdiag::noteVpad(pressed, m_memory.gs().vsyncTick.load());
            publishPad(static_cast<uint16_t>(pressed | raylibPressed), m_memory.gs().vsyncTick.load());
            ps2x::vpad::StickVec stick = vpadFrame.stick;
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
            publishPad(raylibPressed, m_memory.gs().vsyncTick.load());
            ps2x::vpad::liveStick().store(ps2x::vpad::kStickNoOverride, std::memory_order_relaxed);
            vpadPressedPrev = 0u; // set2-polish-1: overlay hidden, no touch chord
        }
        else
        {
            // Overlay off (desktop default): raylib buttons still feed the latch.
            publishPad(raylibPressed, m_memory.gs().vsyncTick.load());
            vpadPressedPrev = 0u; // set2-polish-1: no overlay, no touch chord
        }
        // ctrl-harden-1: pad 2's latch follows the second controller. The
        // panel and the Tricky list own both pads while open (zeros here;
        // their branches publish zero to pad 1 above and fall through).
        publishPadPort1((tkPublishOwn || set2Open) ? static_cast<uint16_t>(0u) : ds1Port1Raw,
                        m_memory.gs().vsyncTick.load());
        // IN4: input-diagnostics per-frame (SDL watch install, 1 s stat line,
        // 5 s timer flush; no-ops unless PS2X_INPUT_DIAG=1, except the
        // once-only watch install that Part A touches need).
        ps2x::inputdiag::pollHost(m_memory.gs().vsyncTick.load());
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
        // Reusable toast (ps2x::ui::toast). Same host-drawn placement as the DS1 line, one row lower.
        if (!skipGl)
        {
            std::string toastMsg;
            if (ps2x::ui::pollToast(toastMsg) && !toastMsg.empty())
            {
                if (toastMsg.size() > 120u)
                    toastMsg.resize(120u);
                const int toastFont = std::max(16, static_cast<int>(screenHeight / 36.0f));
                const int toastTw = MeasureText(toastMsg.c_str(), toastFont);
                const int toastX = static_cast<int>(screenWidth / 2.0f) - toastTw / 2;
                const int toastY = static_cast<int>(screenHeight * 0.12f);
#if defined(__ANDROID__)
                if (vkUnder)
                {
                    rlSetBlendFactorsSeparate(0x0302 /*SRC_ALPHA*/, 0x0303 /*ONE_MINUS_SRC_ALPHA*/,
                                              1 /*ONE*/, 0x0303, 0x8006 /*FUNC_ADD*/, 0x8006);
                    BeginBlendMode(BLEND_CUSTOM_SEPARATE);
                }
#endif
                DrawRectangle(toastX - 12, toastY - 8, toastTw + 24, toastFont + 16, Color{0, 0, 0, 160});
                DrawText(toastMsg.c_str(), toastX, toastY, toastFont, Color{255, 255, 255, 230});
#if defined(__ANDROID__)
                if (vkUnder)
                    EndBlendMode();
#endif
            }
            // online-default-1: first-run privacy notice card (multi-line,
            // posted once per install). Below the toast, same host-drawn
            // placement; never into the guest frame.
            std::string noticeMsg;
            if (ps2x::ui::pollNotice(noticeMsg) && !noticeMsg.empty())
            {
                std::vector<std::string> noticeLines;
                {
                    size_t pos = 0u;
                    for (size_t i = 0u; i < ps2x::ui::kNoticeMaxLines; ++i)
                    {
                        const size_t end = noticeMsg.find('\n', pos);
                        noticeLines.push_back(noticeMsg.substr(pos, end == std::string::npos
                                                                       ? end
                                                                       : end - pos));
                        if (end == std::string::npos)
                            break;
                        pos = end + 1u;
                    }
                }
                const int noticeFont = std::max(14, static_cast<int>(screenHeight / 44.0f));
                int noticeW = 0;
                for (const std::string &line : noticeLines)
                    noticeW = std::max(noticeW, MeasureText(line.c_str(), noticeFont));
                const int noticeH =
                    static_cast<int>(noticeLines.size()) * (noticeFont + 6) + 26;
                const int noticeX = static_cast<int>(screenWidth / 2.0f) - noticeW / 2;
                const int noticeY = static_cast<int>(screenHeight * 0.20f);
#if defined(__ANDROID__)
                if (vkUnder)
                {
                    rlSetBlendFactorsSeparate(0x0302 /*SRC_ALPHA*/, 0x0303 /*ONE_MINUS_SRC_ALPHA*/,
                                              1 /*ONE*/, 0x0303, 0x8006 /*FUNC_ADD*/, 0x8006);
                    BeginBlendMode(BLEND_CUSTOM_SEPARATE);
                }
#endif
                DrawRectangle(noticeX - 14, noticeY - 10, noticeW + 28, noticeH, Color{8, 12, 20, 220});
                DrawRectangleLines(noticeX - 14, noticeY - 10, noticeW + 28, noticeH,
                                   Color{255, 170, 80, 255});
                for (size_t i = 0u; i < noticeLines.size(); ++i)
                    DrawText(noticeLines[i].c_str(), noticeX, noticeY + static_cast<int>(i) * (noticeFont + 6),
                             noticeFont, Color{255, 255, 255, 235});
#if defined(__ANDROID__)
                if (vkUnder)
                    EndBlendMode();
#endif
            }
        }
        // SET2: settings panel (open) / perf line, drawn by the host over
        // the presented frame (never into the guest frame). set2-polish-1
        // (Brad 10-16): no gear dot — a closed panel draws nothing here
        // except the perf line, and the chord is the only opener.
        if (set2On && !skipGl)
        {
#if defined(__ANDROID__)
            if (vkUnder)
            {
                rlSetBlendFactorsSeparate(0x0302 /*SRC_ALPHA*/, 0x0303 /*ONE_MINUS_SRC_ALPHA*/,
                                          1 /*ONE*/, 0x0303, 0x8006 /*FUNC_ADD*/, 0x8006);
                BeginBlendMode(BLEND_CUSTOM_SEPARATE);
            }
#endif
            // panel-polish-1: track the real game-frame height in pixels for
            // the Resolution row (never 448 x scale). The latched frame on
            // GL/iOS is real pixels; on Android-VK the CPU frame is a
            // placeholder, so the last queued AHB (the actual render target)
            // wins there instead.
            {
#if defined(__ANDROID__)
                const bool set2Vk = ps2x_present_vk::active();
#else
                const bool set2Vk = false;
#endif
                if (!set2Vk && presentHeight != 0u)
                    set2MeasuredH = presentHeight;
#if defined(__ANDROID__)
                if (set2Vk)
                {
                    uint32_t set2Qw = 0, set2Qh = 0;
                    if (ps2x_present_vk::lastShownSize(set2Qw, set2Qh) && set2Qw != 0u &&
                        set2Qh != 0u)
                        set2MeasuredH = std::min(set2Qw, set2Qh);
                }
#endif
            }
            const ps2x::set2::PanelGeom set2dg = ps2x::set2::panelLayout(screenWidth, screenHeight);
            if (set2Open)
            {
#if defined(__ANDROID__)
                // The play window is opaque, so the underlaid game cannot
                // show through: dim the full window (the game is paused).
                if (vkUnder)
                    DrawRectangle(0, 0, static_cast<int>(screenWidth), static_cast<int>(screenHeight),
                                  Color{0, 0, 0, 215});
                else
#endif
                    DrawRectangle(0, 0, static_cast<int>(screenWidth), static_cast<int>(screenHeight),
                                  Color{0, 0, 0, 120});
                DrawRectangle(static_cast<int>(set2dg.px), static_cast<int>(set2dg.py),
                              static_cast<int>(set2dg.pw), static_cast<int>(set2dg.ph),
                              Color{8, 12, 20, 232});
                DrawRectangleLines(static_cast<int>(set2dg.px), static_cast<int>(set2dg.py),
                                   static_cast<int>(set2dg.pw), static_cast<int>(set2dg.ph),
                                   Color{255, 140, 40, 255});
                const int titleFont = std::max(16, static_cast<int>(screenHeight / 26.0f));
                const int rowFont = std::max(12, static_cast<int>(screenHeight / 34.0f));
                // panel-polish-1 (Brad 10-15): plain title; the game still
                // pauses while the panel is open.
                DrawText("Settings", static_cast<int>(set2dg.px) + 14,
                         static_cast<int>(set2dg.py) + 10, titleFont, Color{255, 170, 80, 255});
                // settings-res-1: Mode/Resolution stay pending-until-restart
                // with no Restart row. A dirty row shows "(restart)"; while
                // armed it shows the confirm hint instead (a second X on
                // that row restarts). ASCII "-" (the em dash is not in the
                // panel font).
                const bool isModeDirty = ps2x::set2::modeDirty(set2Nav);
                const bool isResDirty = ps2x::set2::resDirty(set2Nav);
                const char *restartHint = "Restart to apply - press X again";
                std::string resVal =
                    ps2x::set2::resOptionLabel(set2Nav.res, set2Nav.nativeH, set2DefRes);
                if (set2Nav.restartArmed && isResDirty)
                    resVal = restartHint;
                else if (isResDirty)
                    resVal += "  (restart)";
                std::string modeVal(set2Nav.mode == ps2x::set2::Mode::M120 ? "120" : "60");
                if (set2Nav.restartArmed && isModeDirty)
                    modeVal = restartHint;
                else if (isModeDirty)
                    modeVal += "  (restart)";
                const std::string perfVal(set2Nav.perf ? "on" : "off");
                const std::string upscaleVal(set2Nav.upscale ? "on" : "off");
                const std::string touchVal(ps2x::set2::touchName(set2Nav.touch));
                const std::string menubtnVal(ps2x::set2::menuBtnName(set2Nav.menubtn));
                for (int i = 0; i < ps2x::set2::kRowCount; ++i)
                {
                    if (static_cast<int>(set2Nav.sel) == i)
                        DrawRectangle(static_cast<int>(set2dg.rowX) - 6, static_cast<int>(set2dg.rowY[i]),
                                      static_cast<int>(set2dg.rowW) + 12, static_cast<int>(set2dg.rowH),
                                      Color{255, 140, 40, 48});
                    const Color rowCol = static_cast<int>(set2Nav.sel) == i
                                             ? Color{255, 200, 130, 255}
                                             : Color{230, 235, 245, 255};
                    const int rowY = static_cast<int>(set2dg.rowY[i]) +
                                     (static_cast<int>(set2dg.rowH) - rowFont) / 2;
                    const ps2x::set2::Row row = static_cast<ps2x::set2::Row>(i);
                    // Two columns: short label left, value right-aligned, so
                    // every label shows in full on Odin, iPad and iPhone.
                    const char *label = ps2x::set2::rowLabel(row);
                    const std::string *value = &modeVal;
                    if (row == ps2x::set2::Row::Res)
                        value = &resVal;
                    else if (row == ps2x::set2::Row::Perf)
                        value = &perfVal;
                    else if (row == ps2x::set2::Row::Upscale)
                        value = &upscaleVal;
                    else if (row == ps2x::set2::Row::Touch)
                        value = &touchVal;
                    else if (row == ps2x::set2::Row::MenuBtn)
                        value = &menubtnVal;
                    DrawText(label, static_cast<int>(set2dg.rowX), rowY, rowFont, rowCol);
                    const int valW = MeasureText(value->c_str(), rowFont);
                    DrawText(value->c_str(),
                             static_cast<int>(set2dg.rowX + set2dg.rowW) - valW, rowY, rowFont,
                             rowCol);
                }
                // settings-res-1: the Menu button row explains the chord, so
                // hiding the button never strands the user.
                const char *hint = (set2Nav.sel == ps2x::set2::Row::MenuBtn)
                                       ? "Select + Start opens settings"
                                       : "Up/Down move - X change - Triangle/chord close";
                DrawText(hint, static_cast<int>(set2dg.px) + 14,
                         static_cast<int>(set2dg.py + set2dg.ph) - rowFont - 8, rowFont,
                         Color{150, 160, 175, 255});
            }
            if (!set2Open && ps2x::set2::menuButtonShown(set2Nav.menubtn))
            {
                // settings-res-1: the "..." menu button, top middle, drawn
                // dim like the pad. ASCII dots: the ellipsis glyph (U+2026)
                // is not in raylib's default font (U+0000..U+00FF only).
                const auto set2mb = ps2x::set2::menuButtonLayout(screenWidth, screenHeight);
                const int mbFont = std::max(12, static_cast<int>(screenHeight / 40.0f));
                const int mbTw = MeasureText("...", mbFont);
                DrawText("...", static_cast<int>(set2mb.x + (set2mb.w - static_cast<float>(mbTw)) * 0.5f),
                         static_cast<int>(set2mb.y + (set2mb.h - static_cast<float>(mbFont)) * 0.5f),
                         mbFont, Color{255, 255, 255, 70});
            }
            if (set2Perf)
            {
                // panel-polish-1 (Brad 10-15): plain "120 fps" + frame ms
                // (vs/s is guest vsyncs/game updates per second). Kept inside
                // the display safe area (cutout-aware insets below) so it
                // never sits in a rounded corner or under a camera cutout.
                char line[64];
                std::snprintf(line, sizeof(line), "%.0f fps  %.1f ms", set2PerfVs, set2PerfMs);
                const int perfFont = std::max(12, static_cast<int>(screenHeight / 40.0f));
                const int perfTw = MeasureText(line, perfFont);
                float set2SafeL = 0.0f, set2SafeT = 0.0f;
                set2SafePad(set2SafeL, set2SafeT);
                const int perfOx = static_cast<int>(8.0f + set2SafeL);
                const int perfOy = static_cast<int>(8.0f + set2SafeT);
                DrawRectangle(perfOx, perfOy, perfTw + 16, perfFont + 10, Color{0, 0, 0, 160});
                DrawText(line, perfOx + 8, perfOy + 4, perfFont, Color{140, 255, 140, 230});
            }
#if defined(__ANDROID__)
            if (vkUnder)
                EndBlendMode();
#endif
            // SET2 screenshot (diag PS2X_SET2_SHOT): one host-composited frame
            // shortly after open. Inside the Begin/EndDrawing span (SET1).
            if (set2ShotIn > 0 && --set2ShotIn == 0 && set2ShotPath && set2ShotPath[0] != '\0')
            {
                rlDrawRenderBatchActive(); // flush the queued draws before the readback
                TakeScreenshot(set2ShotPath);
                std::fprintf(stderr, "[set2] shot %s hostFrame=%llu guestTick=%llu\n", set2ShotPath,
                             static_cast<unsigned long long>(set2HostFrame),
                             static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()));
            }
        }
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        if (!skipGl)
        {
#if defined(PS2X_IOS)
            if (g_hr1ShareTex.id != 0u)
                pso1::maybeCapture();
#endif
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
                {
                    ps2x::perflog::notePresent();
                    // PL3: unique displayed frame against this iteration's
                    // staged frame (latch tick or mailbox publication).
                    ps2x::perflog::notePresentedFrame();
                }
            }
            // IN4: input-diagnostics present count (no-op unless on).
            ps2x::inputdiag::notePresent();
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
#if defined(__ANDROID__) || defined(PS2X_IOS)
    g_bg1GuestStarted.store(false, std::memory_order_release);
#endif

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    // HDL1: headless never created the frame texture nor the window.
    if (!headlessNoRaster)
    {
        UnloadTexture(frameTex);
        CloseWindow();
    }

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
    ps2x::tel::onExit(m_memory.gs().vsyncTick.load()); // TEL2: final coverage dump
    // IN4: input-diagnostics ring dump (no-op unless on).
    ps2x::inputdiag::flush("shutdown");
}
