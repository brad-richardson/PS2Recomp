// I25: iOS env-file loader + SDL/UIKit glue (Objective-C++). Compiled only
// for iOS (PS2X_IS_IOS in ps2xRuntime/CMakeLists.txt).
#include "ps2_ios_runtime.h"
#include "ps2_env_file.h"
#include "ps2_knobs.h"
#include "ps2_record_env.h"
#include "ps2_vsync_lock.h"
#include "ps2_vsync_pacer.h"

// SDL_MAIN_HANDLED: no main->SDL_main rename here (only main.cpp owns main).
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

extern char **environ;

namespace
{
std::vector<std::pair<std::string, std::string>> readEnvFile(const std::filesystem::path &path,
                                                             std::string *rawOut = nullptr)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        std::fprintf(stderr, "[ios-env] no %s\n", path.c_str());
        return {};
    }
    std::ostringstream content;
    content << file.rdbuf();
    std::fprintf(stderr, "[ios-env] read %s\n", path.c_str());
    if (rawOut)
        *rawOut = content.str();
    return ps2x::parseEnvFileContent(content.str());
}

// Settings.bundle switches. Unset (Settings never opened) = on.
bool settingEnabled(CFStringRef key)
{
    Boolean valid = false;
    const Boolean value = CFPreferencesGetAppBooleanValue(key, kCFPreferencesCurrentApplication, &valid);
    return !valid || value;
}

// IG1: unset = off (Settings.bundle DefaultValue is display-only; iOS never
// registers it), so a fresh install waits at the title for real input. Test
// launches pass PS2X_PAD_SCRIPT in the launch env (kept over this switch) or
// set the pref.
bool autoRouteEnabled()
{
    Boolean valid = false;
    const Boolean value = CFPreferencesGetAppBooleanValue(CFSTR("autoRoute"), kCFPreferencesCurrentApplication, &valid);
    return valid && value;
}

// IQ1: Settings.bundle "Target 120 fps" switch. Unset = off (unlike the
// switches above), so a fresh install keeps today's 60 behaviour.
bool target120Enabled()
{
    Boolean valid = false;
    const Boolean value = CFPreferencesGetAppBooleanValue(CFSTR("target120"), kCFPreferencesCurrentApplication, &valid);
    return valid && value;
}
} // namespace

// IQ1: the display link carries the 120 Hz frame-rate request.
// IP6 (PS2X_VSYNC_LOCK=1): its callback also feeds PX1's grid tracker with the
// panel's vsync instants (link.timestamp, mapped from CACurrentMediaTime to
// the pacer's steady clock), the iOS stand-in for SurfaceFlinger latch times.
// No post time is passed, so the slot phase stays at mid-period. The callback
// runs when SDL pumps the main run loop; late delivery is harmless because
// every timestamp lies on the vsync grid.
@interface PS2XDisplayRateTarget : NSObject
- (void)tick:(CADisplayLink *)link;
@end
@implementation PS2XDisplayRateTarget
- (void)tick:(CADisplayLink *)link
{
    if (!ps2_vsync_lock::enabled())
        return;
    const int64_t ageNs = static_cast<int64_t>(std::llround((CACurrentMediaTime() - link.timestamp) * 1e9));
    ps2_vsync_lock::noteLatch(0, ps2_vsync_pacer::steadyNowNs() - ageNs);
}
@end

namespace ps2x::ios
{
void prepareEnvironment(const char *argv0)
{
    std::set<std::string> launcherKeys;
    for (char **e = environ; e && *e; ++e)
    {
        const std::string entry(*e);
        const size_t eq = entry.find('=');
        if (eq != std::string::npos && entry.compare(0, 5, "PS2X_") == 0)
        {
            launcherKeys.insert(entry.substr(0, eq));
        }
    }

    // SDL_GetBasePath is the .app root on iOS (works before SDL_Init).
    std::filesystem::path bundle = argv0 ? std::filesystem::path(argv0).parent_path() : std::filesystem::path();
    if (char *base = SDL_GetBasePath())
    {
        bundle = std::filesystem::path(base).lexically_normal();
        if (bundle.filename().empty())
        {
            bundle = bundle.parent_path();
        }
        SDL_free(base);
    }
    const char *home = std::getenv("HOME");
    const std::filesystem::path documents = home ? std::filesystem::path(home) / "Documents" : std::filesystem::path();
    const std::map<std::string, std::string> vars{
        {"BUNDLE", bundle.string()},
        {"DOCUMENTS", documents.string()},
    };

    std::string bundleRaw, presetRaw, documentsRaw;
    // IQ1: Settings > Target 120 fps layers the bundled full-120 preset over
    // the bundled env; Documents/ps2x.env still wins over both.
    const bool target120 = target120Enabled();
    std::vector<std::pair<std::string, std::string>> preset;
    if (target120)
    {
        preset = readEnvFile(bundle / "full120.env", &presetRaw);
        std::fprintf(stderr, "[ios-env] Settings: Target 120 on -> full120.env layer keys=%zu\n", preset.size());
    }
    const auto merged = ps2x::mergeEnvLayers(
        {readEnvFile(bundle / "ps2x.env", &bundleRaw), preset, readEnvFile(documents / "ps2x.env", &documentsRaw)},
        launcherKeys, vars);
    // PL1: stash the raw bytes' hash for the padrec header's env_sha (both
    // layers, NUL-separated; empty when neither file exists). IQ1: the preset
    // joins as a third field only when the switch is on (off keeps the hash).
    if (!bundleRaw.empty() || !documentsRaw.empty() || !presetRaw.empty())
    {
        std::string both = bundleRaw + '\0' + documentsRaw;
        if (target120)
            both += '\0' + presetRaw;
        ps2x::setRecordedEnvFileHash(ps2x::fnv1a64Hex(both));
    }
    for (const auto &[key, value] : merged)
    {
        setenv(key.c_str(), value.c_str(), 1);
        std::fprintf(stderr, "[ios-env] set %s=%s\n", key.c_str(), value.c_str());
    }
    for (const auto &key : launcherKeys)
    {
        std::fprintf(stderr, "[ios-env] launcher kept %s\n", key.c_str());
    }
    // CFG1 Part 2: compiled iOS play defaults (D1/D4, AU13 sinc, CF4-explicit
    // readback off) for keys neither the env layers nor the launcher set.
    // PS2X_PROFILE=reference disables them (exact knob-off path).
    {
        const size_t applied = ps2x::cf2ApplyIosDefaults();
        std::fprintf(stderr, "[ios-env] compiled defaults applied: %u\n", static_cast<unsigned>(applied));
    }

    if (launcherKeys.count("PS2X_PAD_SCRIPT") == 0 && !autoRouteEnabled())
    {
        unsetenv("PS2X_PAD_SCRIPT");
        std::fprintf(stderr, "[ios-env] Settings: Auto-route off -> PS2X_PAD_SCRIPT cleared\n");
    }

    // I26: Settings > Virtual controls (on-screen pad; hidden anyway while
    // a game controller is connected). Off -> PS2X_VIRTUAL_PAD=0.
    if (launcherKeys.count("PS2X_VIRTUAL_PAD") == 0 && !settingEnabled(CFSTR("virtualControls")))
    {
        setenv("PS2X_VIRTUAL_PAD", "0", 1);
        std::fprintf(stderr, "[ios-env] Settings: Virtual controls off -> PS2X_VIRTUAL_PAD=0\n");
    }

    if (const char *mc = std::getenv("PS2X_MC_ROOT"); mc && mc[0] != '\0')
    {
        // mc1 is the sibling of mc0 (MemoryCard.cpp getMcRootPath).
        std::error_code ec;
        const std::filesystem::path mc0(mc);
        std::filesystem::create_directories(mc0, ec);
        std::filesystem::create_directories(mc0.parent_path() / "mc1", ec);
    }

    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    int pw = 0, ph = 0;
    panelSize(pw, ph); // IX1: cache it on the main thread (the GS opens on its worker)
}

bool panelSize(int &w, int &h)
{
    // IX1: UIScreen.nativeBounds is the panel in physical pixels, portrait-up
    // whatever the orientation; the app is landscape-only, so the long side
    // is the width. Read once on the main thread (prepareEnvironment) and
    // cached, so the GS worker can ask before the window exists.
    static int s_w = 0, s_h = 0;
    if (s_w <= 0 && [NSThread isMainThread])
    {
        const CGRect native = UIScreen.mainScreen.nativeBounds;
        const int a = static_cast<int>(native.size.width + 0.5), b = static_cast<int>(native.size.height + 0.5);
        s_w = std::max(a, b);
        s_h = std::min(a, b);
        std::fprintf(stderr, "[ios-display] panel %dx%d (nativeScale %.2f)\n", s_w, s_h,
                     static_cast<double>(UIScreen.mainScreen.nativeScale));
    }
    if (s_w <= 0 || s_h <= 0)
        return false;
    w = s_w;
    h = s_h;
    return true;
}

void syncWindowSize()
{
    static bool attached = false;
    static int lastW = -1;
    static int lastH = -1;
    SDL_Window *window = SDL_GL_GetCurrentWindow();
    if (!window)
    {
        return;
    }
    if (!attached)
    {
        // SDL 2.32 creates its UIWindow without a UIWindowScene. iOS 27
        // requires the scene manifest (I7), and a scene-less window is
        // never shown: the guest renders but the screen stays black. Attach
        // it to the app's window scene; retried each frame until the scene
        // has connected.
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_UIKIT && info.info.uikit.window)
        {
            UIWindow *uiWindow = info.info.uikit.window;
            if (uiWindow.windowScene == nil)
            {
                for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
                {
                    if ([scene isKindOfClass:[UIWindowScene class]])
                    {
                        uiWindow.windowScene = (UIWindowScene *)scene;
                        [uiWindow makeKeyAndVisible];
                        break;
                    }
                }
            }
            attached = uiWindow.windowScene != nil;
            if (attached)
            {
                std::fprintf(stderr, "[ios-window] attached to window scene\n");
            }
        }
    }
    // raylib only learns the window size from SIZE_CHANGED events, and
    // UIKit sizes (and rotates) the window itself: forward every change.
    int w = 0;
    int h = 0;
    SDL_GetWindowSize(window, &w, &h);
    if (w == lastW && h == lastH)
    {
        return;
    }
    lastW = w;
    lastH = h;
    int dw = 0;
    int dh = 0;
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    std::fprintf(stderr, "[ios-window] window=%dx%d drawable=%dx%d\n", w, h, dw, dh);
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
    event.window.windowID = SDL_GetWindowID(window);
    event.window.data1 = w;
    event.window.data2 = h;
    SDL_PushEvent(&event);
}

void requestDisplayRate(int hz)
{
    static CADisplayLink *s_link = nil;
    static PS2XDisplayRateTarget *s_target = nil;
    const NSInteger maxFps = UIScreen.mainScreen.maximumFramesPerSecond;
    // IP6: the vsync lock needs the link at any rate (a 60 Hz panel included).
    const bool lock = ps2_vsync_lock::enabled();
    if ((hz <= 60 && !lock) || s_link != nil)
    {
        std::fprintf(stderr, "[ios-display] hz=%d max_fps=%ld (no request)\n", hz, static_cast<long>(maxFps));
        return;
    }
    if (lock)
    {
        const int panelHz = static_cast<int>(std::min<NSInteger>(maxFps, hz > 60 ? hz : 60));
        ps2_vsync_lock::setNominalPeriod(1e9 / std::max(panelHz, 1));
        std::fprintf(stderr, "[ios-display] vsync-lock on: display-link grid feeds the pacer (nominal %d Hz)\n",
                     panelHz);
    }
    s_target = [[PS2XDisplayRateTarget alloc] init];
    s_link = [CADisplayLink displayLinkWithTarget:s_target selector:@selector(tick:)];
    const float want = static_cast<float>(hz > 60 ? hz : 60);
    s_link.preferredFrameRateRange = CAFrameRateRangeMake(want, want, want);
    [s_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    std::fprintf(stderr, "[ios-display] hz=%d max_fps=%ld display-link range=%.0f requested\n", hz,
                 static_cast<long>(maxFps), want);
}

std::string perfDeviceState()
{
    char buf[96];
    const NSInteger thermal = static_cast<NSInteger>(NSProcessInfo.processInfo.thermalState);
    const int lpm = NSProcessInfo.processInfo.lowPowerModeEnabled ? 1 : 0;
    UIDevice *device = UIDevice.currentDevice;
    if (!device.batteryMonitoringEnabled)
    {
        device.batteryMonitoringEnabled = YES;
    }
    const float level = device.batteryLevel; // -1 when unknown
    const int batt = level < 0.0f ? -1 : static_cast<int>(level * 100.0f + 0.5f);
    const UIDeviceBatteryState state = device.batteryState;
    const int chg = (state == UIDeviceBatteryStateCharging || state == UIDeviceBatteryStateFull) ? 1 : 0;
    std::snprintf(buf, sizeof(buf), "thermal=%ld lpm=%d batt=%d chg=%d", static_cast<long>(thermal), lpm, batt, chg);
    return buf;
}

int touchPoints(int64_t *ids, float *xs, float *ys, int max)
{
    int n = 0;
    const int devices = SDL_GetNumTouchDevices();
    for (int d = 0; d < devices && n < max; ++d)
    {
        const SDL_TouchID id = SDL_GetTouchDevice(d);
        const int fingers = SDL_GetNumTouchFingers(id);
        for (int f = 0; f < fingers && n < max; ++f)
        {
            if (const SDL_Finger *finger = SDL_GetTouchFinger(id, f))
            {
                ids[n] = static_cast<int64_t>(finger->id);
                xs[n] = finger->x;
                ys[n] = finger->y;
                ++n;
            }
        }
    }
    return n;
}

void setAudioSession(bool ambient)
{
    // IB3 (AirPods): miniaudio's iOS context init tries PlayAndRecord +
    // DefaultToSpeaker with no Bluetooth options (miniaudio.h
    // ma_context_init__coreaudio, the ma_ios_session_category_default
    // branch), so game audio routes to the speaker and AirPods drop. Those
    // are the only setCategory calls in miniaudio.h, so overriding once,
    // after InitAudioDevice, sticks. No options: plain game playback.
    AVAudioSession *session = AVAudioSession.sharedInstance;
    // IP6: what miniaudio's device init saw (it fixes its device rate from
    // session.sampleRate at init and never re-reads it).
    std::fprintf(stderr, "[audio] at device init: category=%s %s\n", session.category.UTF8String,
                 audioSessionState().c_str());
    NSError *error = nil;
    // IA1: Ambient mixes with other apps' audio and does not interrupt them
    // or claim the route (so no AirPods auto-switch); Playback is primary.
    AVAudioSessionCategory category = ambient ? AVAudioSessionCategoryAmbient : AVAudioSessionCategoryPlayback;
    if (![session setCategory:category error:&error])
    {
        std::fprintf(stderr, "[audio] AVAudioSession %s FAILED: %s\n", ambient ? "Ambient" : "Playback",
                     error.localizedDescription.UTF8String);
        return;
    }
    std::fprintf(stderr, "[audio] AVAudioSession category=%s %s\n", session.category.UTF8String,
                 audioSessionState().c_str());
}

std::string audioSessionState()
{
    AVAudioSession *session = AVAudioSession.sharedInstance;
    std::string route;
    for (AVAudioSessionPortDescription *port in session.currentRoute.outputs)
    {
        if (!route.empty())
            route += '+';
        route += port.portType.UTF8String;
    }
    char buf[192];
    std::snprintf(buf, sizeof(buf), "hw_rate=%.0f io_ms=%.2f out_ms=%.2f route=%s", session.sampleRate,
                  session.IOBufferDuration * 1000.0, session.outputLatency * 1000.0,
                  route.empty() ? "none" : route.c_str());
    return buf;
}
} // namespace ps2x::ios
