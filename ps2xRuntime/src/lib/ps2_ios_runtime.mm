// I25: iOS env-file loader + SDL/UIKit glue (Objective-C++). Compiled only
// for iOS (PS2X_IS_IOS in ps2xRuntime/CMakeLists.txt).
#include "ps2_ios_runtime.h"
#include "ps2_env_file.h"

// SDL_MAIN_HANDLED: no main->SDL_main rename here (only main.cpp owns main).
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#import <UIKit/UIKit.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

extern char **environ;

namespace
{
std::vector<std::pair<std::string, std::string>> readEnvFile(const std::filesystem::path &path)
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
    return ps2x::parseEnvFileContent(content.str());
}

// Settings.bundle switch "autoRoute". Unset (Settings never opened) = on.
bool autoRouteEnabled()
{
    Boolean valid = false;
    const Boolean value = CFPreferencesGetAppBooleanValue(CFSTR("autoRoute"), kCFPreferencesCurrentApplication, &valid);
    return !valid || value;
}
} // namespace

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

    const auto merged = ps2x::mergeEnvLayers(
        {readEnvFile(bundle / "ps2x.env"), readEnvFile(documents / "ps2x.env")}, launcherKeys, vars);
    for (const auto &[key, value] : merged)
    {
        setenv(key.c_str(), value.c_str(), 1);
        std::fprintf(stderr, "[ios-env] set %s=%s\n", key.c_str(), value.c_str());
    }
    for (const auto &key : launcherKeys)
    {
        std::fprintf(stderr, "[ios-env] launcher kept %s\n", key.c_str());
    }

    if (launcherKeys.count("PS2X_PAD_SCRIPT") == 0 && !autoRouteEnabled())
    {
        unsetenv("PS2X_PAD_SCRIPT");
        std::fprintf(stderr, "[ios-env] Settings: Auto-route off -> PS2X_PAD_SCRIPT cleared\n");
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
} // namespace ps2x::ios
