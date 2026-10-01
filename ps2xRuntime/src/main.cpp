#include "ps2_runtime.h"
#include "games_database.h"
#include "ps2_knobs.h"
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
#include "ps2_debug_panel.h"
#endif

#include "ps2_log.h"
#include "raylib.h"

#include <iostream>
#include <string>
#include <filesystem>
#include <exception>
#include <algorithm>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <cstring>
#if defined(__ANDROID__)
#endif
#endif

#if defined(PS2X_IOS)
#include "ps2_ios_runtime.h"
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
// I7: on iOS, SDL2main provides the real main() (UIKit app delegate +
// runloop) and SDL_main.h renames our main to SDL_main. Desktop-inert:
// TARGET_OS_IPHONE is 0 on macOS, and CMake links SDL2main only when
// PS2X_IS_IOS. Chain verified: SDL.h -> SDL_main.h -> SDL_stdinc.h ->
// SDL_config.h -> SDL_platform.h defines __IPHONEOS__ from
// TARGET_OS_IPHONE, which selects SDL_MAIN_NEEDED.
#if TARGET_OS_IPHONE
#include <SDL2/SDL.h>
#endif
#endif

namespace
{
#if defined(__ANDROID__)
    int g_logcatPipeFds[2]{-1, -1};
    std::thread g_logcatThread;

    void stopLogcatRedirect()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (g_logcatPipeFds[1] >= 0)
        {
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
        }
        if (g_logcatThread.joinable())
        {
            g_logcatThread.join();
        }
    }

    void redirectStdioToLogcat()
    {
        if (pipe(g_logcatPipeFds) != 0)
        {
            return;
        }

        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(g_logcatPipeFds[1], STDOUT_FILENO);
        dup2(g_logcatPipeFds[1], STDERR_FILENO);

        g_logcatThread = std::thread([]()
                                     {
                                         FILE *reader = fdopen(g_logcatPipeFds[0], "r");
                                         if (!reader)
                                         {
                                             return;
                                         }
                                         // CF2 S0: 8 KB so the ~3.7 KB [knobs] startup line lands as one
                                         // logcat entry (logd truncates past ~4 KB; the suite asserts the
                                         // bound). Chunked reads split multi-KB lines into unmarked
                                         // continuations that exact log compares can't rejoin safely.
                                         char line[8192];
                                         while (fgets(line, sizeof(line), reader))
                                         {
                                             size_t len = std::strlen(line);
                                             if (len > 0 && line[len - 1] == '\n')
                                             {
                                                 line[len - 1] = '\0';
                                             }
                                             __android_log_write(ANDROID_LOG_INFO, "ps2x", line);
                                         }
                                         fclose(reader);
                                         g_logcatPipeFds[0] = -1;
                                     });
        if (std::atexit(stopLogcatRedirect) != 0)
        {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
            if (g_logcatThread.joinable())
            {
                g_logcatThread.join();
            }
        }
    }
#endif

    // LG1: raylib's default TraceLog writes "INFO: ..." to stdout (desktop) or
    // logcat under its own tag (Android), interleaving with runtime lines. This
    // callback formats the same text ("LEVEL: message", levels below the
    // current threshold never reach a callback) and sends the finished line
    // through the shared line-atomic writer.
    void raylibTraceToLineWriter(int logLevel, const char *text, va_list args)
    {
        const char *prefix = "";
        switch (logLevel)
        {
        case LOG_TRACE: prefix = "TRACE: "; break;
        case LOG_DEBUG: prefix = "DEBUG: "; break;
        case LOG_INFO: prefix = "INFO: "; break;
        case LOG_WARNING: prefix = "WARNING: "; break;
        case LOG_ERROR: prefix = "ERROR: "; break;
        case LOG_FATAL: prefix = "FATAL: "; break;
        default: break;
        }
        char body[1024];
        std::vsnprintf(body, sizeof(body), text, args);
        ps2_log::emitLine(std::string(prefix) + body);
        if (logLevel == LOG_FATAL)
        {
            std::exit(EXIT_FAILURE);
        }
    }

    void setupTerminateLogger() // to help on release build crashs
    {
        std::set_terminate([]()
                           {
                               std::cerr << "[terminate] unhandled exception" << std::endl;
                               const std::exception_ptr ep = std::current_exception();
                               if (ep)
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::system_error &e)
                                   {
                                       std::cerr << "[terminate] std::system_error code=" << e.code().value()
                                                 << " category=" << e.code().category().name()
                                                 << " message=" << e.what() << std::endl;
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::cerr << "[terminate] std::exception: " << e.what() << std::endl;
                                   }
                                   catch (...)
                                   {
                                       std::cerr << "[terminate] non-std exception" << std::endl;
                                   }
                               }
                               std::abort(); });
    }

    std::string normalizeGameId(const std::string &folderName)
    {
        std::string result = folderName;

        size_t underscore = result.find('_');
        if (underscore != std::string::npos)
            result[underscore] = '-';

        size_t dot = result.find('.');
        if (dot != std::string::npos)
            result.erase(dot, 1);

        std::ranges::transform(result, result.begin(), [](unsigned char character)
                               { return static_cast<char>(std::toupper(character)); });

        return result;
    }

    std::filesystem::path getExecutablePath(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        {
            std::cout << "Using argv boot path" << std::endl;
            return std::filesystem::path(argv[1]);
        }
#if defined(PS2X_IOS)
        // I25: home-screen launches pass no argv; the bundled ps2x.env names
        // the ELF (PS2X_BOOT_ELF=${BUNDLE}/SLUS_207.72).
        if (const char *bootElf = std::getenv("PS2X_BOOT_ELF"); bootElf && bootElf[0] != '\0')
        {
            std::cout << "Using PS2X_BOOT_ELF boot path" << std::endl;
            return std::filesystem::path(bootElf);
        }
#endif
#if defined(PS2X_DEFAULT_BOOT_ELF)
        std::cout << "Using default boot file" << std::endl;
        const std::filesystem::path configuredPath = std::filesystem::path(PS2X_DEFAULT_BOOT_ELF);
#if defined(PLATFORM_VITA)
        return configuredPath;
#endif
        if (configuredPath.is_absolute())
        {
            return configuredPath;
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
#else
        throw std::runtime_error("Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.");
#endif
    }
}

int main(int argc, char *argv[])
{
#if defined(__ANDROID__)
    redirectStdioToLogcat();
#endif
    // LG1: line-atomic cerr/cout + raylib TraceLog, before any thread starts.
    ps2_log::installLineAtomicLogging();
    SetTraceLogCallback(raylibTraceToLineWriter);
#if defined(PS2X_IOS)
    ps2x::ios::prepareEnvironment(argc > 0 ? argv[0] : nullptr);
#endif
    setupTerminateLogger();

    // CF2 S0: always-on resolved-knob line. After the env loaders (Android
    // static init, iOS prepareEnvironment above), before every boot path
    // (replay/test/game) so all modes carry it.
    ps2x::cf2DumpKnobs();

    try
    {
        std::filesystem::path pathObj = getExecutablePath(argc, argv);

        std::string filePathStr = pathObj.string();
        std::string elfName = pathObj.filename().string();
        std::string normalizedId = normalizeGameId(elfName);

        std::string windowTitle = "PS2-Recomp | ";
        const char *gameName = getGameName(normalizedId);

#if !defined(PLATFORM_VITA)
        if (gameName)
        {
            windowTitle += std::string(gameName) + " | " + elfName;
        }
        else
#endif
        {
            windowTitle += elfName;
        }

        PS2Runtime runtime;
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
        // This hook is to prevent leak rlimgui deps to recompiler etc
        PS2DebugPanel debugPanel;
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->initialize();
            },
            [](PS2Runtime &rt, void *userData)
            {
                static_cast<PS2DebugPanel *>(userData)->draw(rt);
            },
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->shutdown();
            },
            &debugPanel);
#endif
        if (!runtime.initialize(windowTitle.c_str()))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }

        if (!runtime.loadELF(filePathStr))
        {
            std::cerr << "Failed to load ELF file: " << filePathStr << std::endl;
            return 1;
        }

        if (const char *cdImageEnv = std::getenv("PS2X_CD_IMAGE"))
        {
            if (cdImageEnv[0] != '\0')
            {
                PS2Runtime::IoPaths ioPaths = PS2Runtime::getIoPaths();
                ioPaths.cdImage = std::filesystem::path(cdImageEnv);
                PS2Runtime::setIoPaths(ioPaths);
            }
        }

        // I25: writable memory-card root (the iOS bundle is read-only, so the
        // default <elf dir>/mc0 can't be written there). mc1 is its sibling.
        if (const char *mcRootEnv = std::getenv("PS2X_MC_ROOT"))
        {
            if (mcRootEnv[0] != '\0')
            {
                PS2Runtime::IoPaths ioPaths = PS2Runtime::getIoPaths();
                ioPaths.mcRoot = std::filesystem::path(mcRootEnv);
                PS2Runtime::setIoPaths(ioPaths);
            }
        }

        runtime.run();
        // main exits with _Exit, which bypasses PS2Runtime's destructor.
        // Emit the per-target coverage summary on this normal return path.
        runtime.printMissingFunctionCounts();

#ifdef _DEBUG
        ps2_log::print_saved_location();
#endif
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[main] fatal exception: unknown" << std::endl;
    }

    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}

// I7: keep SDL_main.h's main->SDL_main rename inside this TU: ps2EntryRunner
// builds with unity build, so without this the rename would leak into any
// sibling TU batched after main.cpp. (No sibling uses `main` today; this is
// insurance while P-lane concurrently edits src/runner/.)
#if defined(__APPLE__)
#if TARGET_OS_IPHONE
#undef main
#endif
#endif
