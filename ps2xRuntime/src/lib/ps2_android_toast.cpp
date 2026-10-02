#if defined(__ANDROID__)

// UX1: quick-save/load Toast (DS1 gap §2). JNI shape mirrors AP1's
// ap1HideSystemBars (ps2_runtime.cpp): raylib's android_app gives the
// NativeActivity + JavaVM, the worker attaches with its thread name kept,
// calls into Java, cleans local refs, clears exceptions. Differences:
// (1) the worker is a detached per-toast thread, so it MUST Detach before
// exit (AP1 runs on long-lived threads and stays attached); (2) the helper
// class is app code, and FindClass from an attached thread uses the system
// loader, so it resolves via the activity's ClassLoader (the standard
// recipe); (3) the Java side (Ux1Toast.show) hops to the UI thread itself,
// so native just posts and returns.

#include "ps2_android_toast.h"

#include <android_native_app_glue.h>

#include <jni.h>
#include <pthread.h>

#include <cstdio>
#include <string>
#include <thread>

extern "C" struct android_app *GetAndroidApp(void); // raylib rcore_android.c

namespace
{

void showToastOnWorker(std::string message)
{
    struct android_app *app = GetAndroidApp();
    if (!app || !app->activity || !app->activity->vm)
        return;
    JNIEnv *env = nullptr;
    char name[16] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    JavaVMAttachArgs args = {JNI_VERSION_1_6, name[0] ? name : nullptr, nullptr};
    if (app->activity->vm->AttachCurrentThread(&env, &args) != JNI_OK || !env)
        return;
    bool shown = false;
    jobject act = app->activity->clazz;
    jclass actCls = env->GetObjectClass(act);
    jmethodID getLoader =
        actCls ? env->GetMethodID(actCls, "getClassLoader", "()Ljava/lang/ClassLoader;") : nullptr;
    jobject loader = (actCls && getLoader) ? env->CallObjectMethod(act, getLoader) : nullptr;
    jclass loaderCls = loader ? env->GetObjectClass(loader) : nullptr;
    jmethodID loadClass = loaderCls ? env->GetMethodID(loaderCls, "loadClass",
                                                       "(Ljava/lang/String;)Ljava/lang/Class;")
                                    : nullptr;
    jstring className = loadClass ? env->NewStringUTF("com.ps2x.runner.Ux1Toast") : nullptr;
    jclass toastCls =
        (loader && className) ? static_cast<jclass>(env->CallObjectMethod(loader, loadClass, className))
                              : nullptr;
    jmethodID show = toastCls ? env->GetStaticMethodID(
                                    toastCls, "show", "(Landroid/app/Activity;Ljava/lang/String;)V")
                              : nullptr;
    jstring text = (toastCls && show) ? env->NewStringUTF(message.c_str()) : nullptr;
    if (text && !env->ExceptionCheck())
    {
        env->CallStaticVoidMethod(toastCls, show, act, text);
        shown = !env->ExceptionCheck();
    }
    if (env->ExceptionCheck())
        env->ExceptionClear();
    if (text)
        env->DeleteLocalRef(text);
    if (className)
        env->DeleteLocalRef(className);
    if (toastCls)
        env->DeleteLocalRef(toastCls);
    if (loaderCls)
        env->DeleteLocalRef(loaderCls);
    if (loader)
        env->DeleteLocalRef(loader);
    if (actCls)
        env->DeleteLocalRef(actCls);
    app->activity->vm->DetachCurrentThread();
    if (!shown)
        std::fprintf(stderr, "[toast] NOT shown: %s\n", message.c_str());
}

} // namespace

namespace ps2x
{

void postQuickStatusToast(const std::string &message)
{
    if (message.empty())
        return;
    // Rare (user-initiated save/load only): a detached worker per toast is
    // cheaper than a persistent notifier thread's lifecycle, and the
    // caller never waits on the attach, the JNI call or the UI thread.
    std::thread(showToastOnWorker, message).detach();
}

} // namespace ps2x

#endif // __ANDROID__
