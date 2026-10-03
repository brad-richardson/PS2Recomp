cmake_minimum_required(VERSION 3.20)

project(PS2Runtime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PS2X_ENABLE_RUNNER_UNITY_BUILD "Build ps2EntryRunner with CMake unity build" ON)
set(PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE 32 CACHE STRING "Unity build batch size for ps2EntryRunner")
option(PS2X_ENABLE_RUNNER_PCH "Precompile the heavy runtime headers for ps2EntryRunner" ON)
set(PS2X_GAME_CODEGEN_DIR "" CACHE PATH "ps2_recomp output dir for one game (register_functions.cpp + function .cpps + ps2_recompiled_*.h); when set, builds them as ps2_game_objects and drops the in-tree stub table")
option(PS2X_ENABLE_SCCACHE "Use sccache as compiler launcher when available" ON)
option(PS2X_DEV_NO_THINLTO "Dev builds only: skip ThinLTO IPO on the runner link chain (ps2_runtime, ps2EntryRunner) for faster incremental links; Release untouched" OFF)
# BF2: release flag candidates, all default-off (unset = byte-identical flags).
option(PS2X_GAME_IPO "BF2: ThinLTO IPO on ps2_game_objects in Release (Android + Mac)" OFF)
set(PS2X_ANDROID_MARCH "" CACHE STRING "BF2: Android-only -march value for the first-party runtime targets (empty = toolchain default)")

option(PS2X_ENABLE_RUNTIME_LOGS "Enable PS2 runtime logs" OFF)
option(PS2X_ENABLE_AGRESSIVE_LOGS "Enable very verbose/agressive PS2 runtime logs" OFF)
option(PS2X_ENABLE_IOP_RPC_TRACE "Log unhandled IOP/SIF RPC trace suggestions" ON)
option(PS2X_STRICT_RETURN_DIAGNOSTICS "Route generated JR $ra returns through runtime branch diagnostics" OFF)
option(PS2X_SHOW_WINDOWS_CONSOLE "Show a console window for ps2EntryRunner on Windows release builds" ON)
option(PS2X_ENABLE_DEBUG_UI "Build the desktop runtime debug UI" ON)

if(PS2X_ENABLE_SCCACHE)
    find_program(PS2X_SCCACHE_PROGRAM sccache)

    if(PS2X_SCCACHE_PROGRAM)
        if(CMAKE_C_COMPILER)
            set(CMAKE_C_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        endif()

        set(CMAKE_CXX_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        message(STATUS "Using sccache: ${PS2X_SCCACHE_PROGRAM}")
    else()
        message(STATUS "sccache not found; continuing without compiler launcher")
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReleaseMode.cmake")

set(PS2X_IS_VITA OFF)

if((CMAKE_C_COMPILER MATCHES "arm-vita-eabi") OR
    (CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi"))
    set(PS2X_IS_VITA ON)
endif()

set(PS2X_IS_ANDROID OFF)

if(ANDROID)
    set(PS2X_IS_ANDROID ON)
endif()

# BF2: apply PS2X_ANDROID_MARCH to one first-party target (Android only).
# Empty PS2X_ANDROID_MARCH (the default) is a no-op.
function(ps2x_bf2_android_march_target TGT)
    if(PS2X_IS_ANDROID AND PS2X_ANDROID_MARCH AND TARGET ${TGT})
        target_compile_options(${TGT} PRIVATE -march=${PS2X_ANDROID_MARCH})
    endif()
endfunction()
# BF2b: echo the effective release-flag values at configure time (visible in
# the bradflix APK build log; Mac default is OFF/empty).
message(STATUS "BF2: PS2X_GAME_IPO=${PS2X_GAME_IPO} PS2X_ANDROID_MARCH=${PS2X_ANDROID_MARCH}")

set(PS2X_IS_IOS OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PS2X_IS_IOS ON)
endif()

option(PS2X_VITA_CREATE_PACKAGE "Create Vita eboot/vpk targets" ON)
set(PS2X_VITA_APP_NAME "PS2 Retro X" CACHE STRING "Display name for the Vita bubble")
set(PS2X_VITA_TITLEID "RANJ00001" CACHE STRING "9-character Vita title id")
set(PS2X_VITA_VERSION "01.00" CACHE STRING "Vita app version")
set(PS2X_DEFAULT_BOOT_ELF "" CACHE STRING "Guest ELF path passed directly to main() when argv is unavailable")
# PL1: build id for the pad-recording header (fork SHA + dirty flag). Empty =
# auto-detect from git at configure time (worktree builds); archive exports
# (no .git) and explicit -DPS2X_BUILD_ID=<sha12> (bradflix drivers) skip it.
set(PS2X_BUILD_ID "" CACHE STRING "Build id baked into the binary (padrec header + PS2X-BUILD-ID marker)")
set(PS2X_BUILD_ID_RESOLVED "${PS2X_BUILD_ID}")
if(NOT PS2X_BUILD_ID_RESOLVED)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=12 HEAD
            OUTPUT_VARIABLE PS2X_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(PS2X_GIT_SHA MATCHES "^[0-9a-f]+$")
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" diff --quiet HEAD --
                RESULT_VARIABLE PS2X_GIT_DIRTY_RC ERROR_QUIET)
            set(PS2X_BUILD_ID_RESOLVED "${PS2X_GIT_SHA}")
            if(NOT PS2X_GIT_DIRTY_RC EQUAL 0)
                set(PS2X_BUILD_ID_RESOLVED "${PS2X_GIT_SHA}-dirty")
            endif()
        endif()
    endif()
endif()
# Quoteless transports (AGP passes arguments with no shell) can wrap the
# value in literal quotes; strip one pair before the charset check.
if(PS2X_BUILD_ID_RESOLVED MATCHES "^\".*\"$")
    string(REGEX REPLACE "^\"(.*)\"$" "\\1" PS2X_BUILD_ID_RESOLVED "${PS2X_BUILD_ID_RESOLVED}")
endif()
if(NOT PS2X_BUILD_ID_RESOLVED MATCHES "^[A-Za-z0-9_.,:+-]*$")
    message(STATUS "PS2X build id: rejected (bad charset), treating as unavailable")
    set(PS2X_BUILD_ID_RESOLVED "")
endif()
if(PS2X_BUILD_ID_RESOLVED)
    message(STATUS "PS2X build id: ${PS2X_BUILD_ID_RESOLVED}")
else()
    message(STATUS "PS2X build id: unavailable (no -DPS2X_BUILD_ID, no git)")
endif()

if(PS2X_IS_VITA)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "VITASDK is not defined. Configure inside the VitaSDK environment or pass the Vita toolchain file.")
    endif()

    set(PS2X_VITASDK "$ENV{VITASDK}")

    if(NOT EXISTS "${PS2X_VITASDK}/share/vita.cmake")
        message(FATAL_ERROR "Could not find vita.cmake under ${PS2X_VITASDK}/share.")
    endif()

    include("${PS2X_VITASDK}/share/vita.cmake" REQUIRED)
else()
    include(FetchContent)
    set(FETCHCONTENT_QUIET FALSE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

    if(PS2X_IS_ANDROID)
        set(PLATFORM "Android" CACHE STRING "" FORCE)
    elseif(PS2X_IS_IOS)
        # I4: iOS uses raylib's SDL backend (raylib-wall Option C) with GLES2.
        # SDL2 itself comes from a prebuilt SDL2-for-iOS via SDL2_DIR
        # passthrough (SDL2_DIR is a cache var, so it flows into the raylib
        # FetchContent configure): no FetchContent for SDL, keeping SDL's own
        # configure out of every runtime configure and reusing one proven
        # prebuilt bit-for-bit.
        set(PLATFORM "SDL" CACHE STRING "" FORCE)
        set(OPENGL_VERSION "ES 2.0" CACHE STRING "" FORCE)
        if(NOT SDL2_DIR)
            message(FATAL_ERROR "PS2X: iOS builds need -DSDL2_DIR=<SDL2-for-iOS install>/lib/cmake/SDL2 (prebuilt SDL2 release-2.32.10 for iphonesimulator)")
        endif()
    endif()

    FetchContent_Declare(
        raylib
        GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
        GIT_TAG "5.5" # we will migrate to 4.2.0 later, trust me it will be better
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(raylib)

    if(PS2X_IS_ANDROID)
        # VK1 Part 2: alpha in raylib's EGL config (translucent window over the
        # Vulkan present layer when the virtual pad is on). Hash-pinned like iOS.
        execute_process(
            COMMAND "${CMAKE_COMMAND}"
                "-DPS2X_RAYLIB_SOURCE_DIR=${raylib_SOURCE_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_raylib_android_egl_alpha.cmake"
            RESULT_VARIABLE PS2X_RAYLIB_EGL_ALPHA_PATCH_RESULT
        )
        if(NOT PS2X_RAYLIB_EGL_ALPHA_PATCH_RESULT EQUAL 0)
            message(FATAL_ERROR "PS2X: could not apply the pinned raylib Android EGL alpha patch")
        endif()
    endif()

    if(PS2X_IS_IOS)
        # Keep the pinned raylib checkout reproducible: patch its SDL2 DPI path
        # during configure, before raylib sources are compiled. The patch script
        # checks the source revision by file hash and is safe to rerun.
        execute_process(
            COMMAND "${CMAKE_COMMAND}"
                "-DPS2X_RAYLIB_SOURCE_DIR=${raylib_SOURCE_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_raylib_ios_sdl2_dpi.cmake"
            RESULT_VARIABLE PS2X_RAYLIB_DPI_PATCH_RESULT
        )
        if(NOT PS2X_RAYLIB_DPI_PATCH_RESULT EQUAL 0)
            message(FATAL_ERROR "PS2X: could not apply the pinned iOS raylib SDL2 DPI patch")
        endif()

        # I4: miniaudio (raylib raudio.c) includes AVFoundation on Apple
        # targets, so the raylib TU set compiles as ObjC. Scoped to the raylib
        # target on purpose: -x in CMAKE_C_FLAGS would also poison link lines.
        target_compile_options(raylib PRIVATE -x objective-c)
    endif()

if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_ANDROID AND NOT PS2X_IS_IOS)
        FetchContent_Declare(
            imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG "v1.92.7-docking"
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(imgui)

        if(NOT imgui_POPULATED)
            FetchContent_Populate(imgui)
        endif()

        set(PS2X_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}")

        add_library(imgui STATIC
            "${PS2X_IMGUI_SOURCE_DIR}/imgui.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
        )
        target_include_directories(imgui PUBLIC "${PS2X_IMGUI_SOURCE_DIR}")

        FetchContent_Declare(
            rlImGui
            GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
            GIT_TAG "Raylib_5_5"
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(rlImGui)

        if(NOT rlimgui_POPULATED)
            FetchContent_Populate(rlImGui)
        endif()

        set(PS2X_RLIMGUI_SOURCE_DIR "${rlimgui_SOURCE_DIR}")

        add_library(rlImGui STATIC
            "${PS2X_RLIMGUI_SOURCE_DIR}/rlImGui.cpp"
        )
        target_include_directories(rlImGui PUBLIC "${PS2X_RLIMGUI_SOURCE_DIR}")
        target_link_libraries(rlImGui PUBLIC raylib imgui)
    endif()
endif()

add_library(ps2_host_backend INTERFACE)

set(PS2X_DEFAULT_BOOT_ELF_DEFINE "")

if(PS2X_DEFAULT_BOOT_ELF)
    set(PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF}")
    string(REPLACE "\\" "\\\\" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
    string(REPLACE "\"" "\\\"" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
endif()

if(PS2X_IS_VITA)
    set(PS2X_VITA_PREFIX "${PS2X_VITASDK}/arm-vita-eabi")
    set(PS2X_VITA_EXTRA_INCLUDE_DIRS "" CACHE STRING "Extra include directories for Vita host dependencies")
    set(PS2X_VITA_LIBRARY_DIR "${PS2X_VITA_PREFIX}/lib")
    set(PS2X_VITA_REQUIRED_LIBRARY_NAMES
        raylib
        SDL2
        OpenSLES
        taihen_stub
        SceAppMgr_stub
        SceCtrl_stub
        SceGxm_stub
        SceCommonDialog_stub
        SceLibKernel_stub
        SceAudio_stub
        SceTouch_stub
        SceHid_stub
        SceMotion_stub
        SceSysmodule_stub
        SceIofilemgr_stub
        SceNetCtl_stub
        SceNet_stub
        SceDisplay_stub
        SceAppUtil_stub
        SceAudioIn_stub
        ScePower_stub
        SceProcessmgr_stub
        SceIme_stub
        libIMGEGL_stub_weak
        libgpu_es4_ext_stub_weak
        libGLESv2_stub_weak
        libGL_stub_weak
        libGLESv1_CM_stub_weak
    )

    set(PS2X_VITA_INCLUDE_DIRS
        "${PS2X_VITA_PREFIX}/include"
    )

    foreach(PS2X_VITA_OPTIONAL_INCLUDE_DIR IN ITEMS
        "${PS2X_VITA_PREFIX}/include/raylib"
        "${PS2X_VITA_PREFIX}/include/SDL2")
        if(EXISTS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
            list(APPEND PS2X_VITA_INCLUDE_DIRS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
        endif()
    endforeach()

    if(PS2X_VITA_EXTRA_INCLUDE_DIRS)
        list(APPEND PS2X_VITA_INCLUDE_DIRS ${PS2X_VITA_EXTRA_INCLUDE_DIRS})
    endif()

    if(NOT EXISTS "${PS2X_VITA_PREFIX}/include/SDL2/SDL.h")
        message(FATAL_ERROR
            "Missing SDL2 headers under ${PS2X_VITA_PREFIX}/include/SDL2. "
            "Quenom/raylib-5.5-vita requires SDL2 with PVR support installed into VitaSDK.")
    endif()

    if(NOT EXISTS "${PS2X_VITA_LIBRARY_DIR}")
        message(FATAL_ERROR "Missing Vita library directory: ${PS2X_VITA_LIBRARY_DIR}")
    endif()

    function(ps2x_find_vita_library out_var library_name)
        string(MAKE_C_IDENTIFIER "${library_name}" library_id)
        set(cache_var "PS2X_VITA_LIB_${library_id}")

        find_library(${cache_var}
            NAMES "${library_name}" "${library_name}.a" "lib${library_name}.a"
            PATHS "${PS2X_VITA_LIBRARY_DIR}"
            NO_DEFAULT_PATH
        )

        if(NOT ${cache_var})
            message(FATAL_ERROR
                "Could not find Vita library '${library_name}' under ${PS2X_VITA_LIBRARY_DIR}. "
                "Install Quenom/raylib-5.5-vita and its SDL2/PVR dependencies into VitaSDK first.")
        endif()

        set(${out_var} "${${cache_var}}" PARENT_SCOPE)
    endfunction()

    set(PS2X_VITA_RESOLVED_LIBRARIES "")

    foreach(PS2X_VITA_LIBRARY_NAME IN LISTS PS2X_VITA_REQUIRED_LIBRARY_NAMES)
        ps2x_find_vita_library(PS2X_VITA_LIBRARY_PATH "${PS2X_VITA_LIBRARY_NAME}")
        list(APPEND PS2X_VITA_RESOLVED_LIBRARIES "${PS2X_VITA_LIBRARY_PATH}")
    endforeach()

    target_compile_definitions(ps2_host_backend INTERFACE PLATFORM_VITA)
    target_include_directories(ps2_host_backend INTERFACE ${PS2X_VITA_INCLUDE_DIRS})
    target_link_libraries(ps2_host_backend INTERFACE
        ${PS2X_VITA_RESOLVED_LIBRARIES}
        m
        c
        pthread
    )
else()
    target_link_libraries(ps2_host_backend INTERFACE raylib)

    if(PS2X_IS_ANDROID)
        target_link_libraries(ps2_host_backend INTERFACE log android)
    endif()
endif()

if(PS2X_IS_ANDROID)
    set(PS2X_ENABLE_FFMPEG_DEFAULT OFF)
else()
    set(PS2X_ENABLE_FFMPEG_DEFAULT ON)
endif()

option(PS2X_ENABLE_FFMPEG "Enable FFmpeg-backed MPEG video decoding" ${PS2X_ENABLE_FFMPEG_DEFAULT})

if(NOT PS2X_ENABLE_FFMPEG)
    message(STATUS "FFmpeg disabled; MPEG video decode falls back to stub frames")
    add_library(ffmpeg INTERFACE)
elseif(WIN32)
    include(ExternalProject)

    set(FFMPEG_PREBUILT_TAG "n7.1-241205")
    set(FFMPEG_PREBUILT_URL
        "https://github.com/System233/ffmpeg-msvc-prebuilt/releases/download/${FFMPEG_PREBUILT_TAG}/ffmpeg-${FFMPEG_PREBUILT_TAG}-lgpl-amd64-shared.zip")

    set(FFMPEG_PREFIX_DIR "${CMAKE_BINARY_DIR}/ThirdParty/ffmpeg-prefix")
    set(FFMPEG_SOURCE_DIR "${FFMPEG_PREFIX_DIR}/src/ffmpeg_external")
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_SOURCE_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_SOURCE_DIR}/bin")
    set(FFMPEG_BIN_DIR "${FFMPEG_SOURCE_DIR}/bin")

    ExternalProject_Add(ffmpeg_external
        URL "${FFMPEG_PREBUILT_URL}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${FFMPEG_PREFIX_DIR}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib"
        "${FFMPEG_LIB_DIR}/swresample.lib"
        "${FFMPEG_LIB_DIR}/swscale.lib"
    )

    # Link against the import libraries shipped with the Windows DLLs.
    add_library(ffmpeg_avcodec STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avcodec.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avcodec ffmpeg_external)

    add_library(ffmpeg_avformat STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avformat.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avformat ffmpeg_external)

    add_library(ffmpeg_avutil STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avutil.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avutil ffmpeg_external)

    add_library(ffmpeg_swresample STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swresample.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swresample ffmpeg_external)

    add_library(ffmpeg_swscale STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swscale.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swscale ffmpeg_external)

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE
        ffmpeg_avcodec
        ffmpeg_avformat
        ffmpeg_avutil
        ffmpeg_swresample
        ffmpeg_swscale
        bcrypt
        secur32
        ws2_32
        user32
        advapi32
        ole32
        shell32
    )

    set(PS2X_FFMPEG_BIN_DIR "${FFMPEG_BIN_DIR}" CACHE INTERNAL "Directory containing FFmpeg runtime DLLs")
    set(PS2X_COPY_FFMPEG_DLLS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyFfmpegDlls.cmake"
        CACHE INTERNAL
        "Script used to stage FFmpeg runtime DLLs beside executables")
elseif(PS2X_IS_IOS)
    # I23: minimal FFmpeg 7.1.x subset for iOS, built outside the repo from
    # the I22 spike recipe (static arm64 archives + `make install` headers).
    # Only the three libraries MPEG.cpp calls are wired (avformat/swresample
    # are linked on other platforms but never referenced by MPEG.cpp).
    set(PS2X_FFMPEG_IOS_ROOT "" CACHE PATH
        "Prefix of a locally-built minimal FFmpeg 7.1.x for iOS (lib/libav*.a + include/). Recipe: ps2xRuntime/cmake/iOS-FFmpeg-7.1.1.md")
    if(NOT PS2X_FFMPEG_IOS_ROOT)
        message(FATAL_ERROR
            "PS2X_ENABLE_FFMPEG=ON for iOS requires -DPS2X_FFMPEG_IOS_ROOT=<prefix> "
            "pointing at a locally-built minimal FFmpeg 7.1.x (lib/libavcodec.a, lib/libavutil.a, "
            "lib/libswscale.a + include/). Recipe: ps2xRuntime/cmake/iOS-FFmpeg-7.1.1.md")
    endif()

    set(PS2X_FFMPEG_IOS_INCLUDE_DIR "${PS2X_FFMPEG_IOS_ROOT}/include")
    set(PS2X_FFMPEG_IOS_LIB_DIR "${PS2X_FFMPEG_IOS_ROOT}/lib")
    foreach(_ps2x_ffmpeg_ios_lib avcodec avutil swscale)
        if(NOT EXISTS "${PS2X_FFMPEG_IOS_LIB_DIR}/lib${_ps2x_ffmpeg_ios_lib}.a")
            message(FATAL_ERROR
                "PS2X_FFMPEG_IOS_ROOT is missing lib/lib${_ps2x_ffmpeg_ios_lib}.a "
                "(prefix: ${PS2X_FFMPEG_IOS_ROOT}). Rebuild the prefix per ps2xRuntime/cmake/iOS-FFmpeg-7.1.1.md")
        endif()
        if(NOT TARGET ffmpeg_${_ps2x_ffmpeg_ios_lib})
            add_library(ffmpeg_${_ps2x_ffmpeg_ios_lib} STATIC IMPORTED GLOBAL)
            set_target_properties(ffmpeg_${_ps2x_ffmpeg_ios_lib} PROPERTIES
                IMPORTED_LOCATION "${PS2X_FFMPEG_IOS_LIB_DIR}/lib${_ps2x_ffmpeg_ios_lib}.a"
                INTERFACE_INCLUDE_DIRECTORIES "${PS2X_FFMPEG_IOS_INCLUDE_DIR}"
            )
        endif()
    endforeach()

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE
        ffmpeg_avcodec
        ffmpeg_avutil
        ffmpeg_swscale
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
        libavcodec
        libavformat
        libavutil
        libswresample
        libswscale
    )

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

function(ps2x_stage_ffmpeg_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()

    if(NOT PS2X_FFMPEG_BIN_DIR OR NOT PS2X_COPY_FFMPEG_DLLS_SCRIPT)
        return()
    endif()

    if(TARGET ffmpeg_external)
        add_dependencies(${target_name} ffmpeg_external)
    endif()

    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${PS2X_FFMPEG_BIN_DIR}"
        "-Ddest_dir=$<TARGET_FILE_DIR:${target_name}>"
        -P "${PS2X_COPY_FFMPEG_DLLS_SCRIPT}"
        VERBATIM
    )
endfunction()

add_library(ps2_runtime STATIC
    src/lib/ps2_build_id.cpp
    src/lib/game_overrides.cpp
    src/lib/gs/ps2_gif_arbiter.cpp
    src/lib/gs/gs_stream_capture.cpp
    src/lib/gs/gs_worker.cpp
    src/lib/gs/gs_replay_core.cpp # N8D7M12 Part 1: shared GS replay core (no test harness)
    src/lib/ps2_audio.cpp
    src/lib/ps2_snd_audio_output.cpp
    src/lib/ps2_audio_vag.cpp
    src/lib/gs/ps2_gs_memory.cpp
    src/lib/gs/gs_frontend.cpp
    src/lib/gs/gs_cpu_backend.cpp
    src/lib/gs/ps2_gs_external_backend.cpp # GE2: external-GS shell (recording stub)
    src/lib/ps2_iop_host.cpp
    src/lib/ps2_memory.cpp
    src/lib/ps2_pad.cpp
    src/lib/ps2_perf_log.cpp
    src/lib/ps2_input_diag.cpp
    src/lib/ps2_rom_device.cpp
    src/lib/ps2_vfs.cpp
    src/lib/ps2_runtime.cpp
    src/lib/ps2_ssx3_vis_native.cpp
    src/lib/ps2_savestate.cpp
    src/lib/ps2_vif1_interpreter.cpp
    src/lib/vu/ps2_vu_core.cpp
    src/lib/vu/ps2_vu_upper.cpp
    src/lib/vu/ps2_vu_recomp.cpp
    src/lib/vu/ps2_vu_lower.cpp
    src/lib/vu/ps2_vu0.cpp
    src/lib/vu/ps2_vu1.cpp
    src/lib/vu/ps2_microvu.cpp
    src/lib/games_database.cpp
)

if(PS2X_ENABLE_RUNTIME_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2_RUNTIME_LOGS=1
    )
endif()

# PL1: PUBLIC so the pad recorder and the APK marker TU (and the suite, which
# links ps2_runtime) all see the same id. Always defined, possibly empty.
target_compile_definitions(ps2_runtime PUBLIC PS2X_BUILD_ID="${PS2X_BUILD_ID_RESOLVED}")

# E40-E44 guest-memory watch taps in ps2_runtime_macros.h. Keep them out of
# every generated function by default on all platforms; dev lanes opt in.
option(PS2X_ENABLE_DIAG_TAPS "Compile the E40-E44 guest-memory watch taps into the READ/WRITE macros" OFF)
if(PS2X_ENABLE_DIAG_TAPS)
    target_compile_definitions(ps2_runtime PUBLIC PS2X_ENABLE_DIAG_TAPS=1)
else()
    target_compile_definitions(ps2_runtime PUBLIC PS2X_ENABLE_DIAG_TAPS=0)
endif()

# EE1: P1f PS2X_DIAG_WATCH guest-RAM watchpoint in the WRITE macros and Store
# helpers. The out-of-line ps2DiagWatchEnabled() call survives in release
# builds (RV16 section 4: 0.23 ms/frame self on GameThread); keep it out of
# every generated function by default on all platforms, diag/det builds opt
# back in via the build scripts.
option(PS2X_ENABLE_DIAG_WATCH "Compile the PS2X_DIAG_WATCH guest-RAM watchpoint into stores" OFF)
if(PS2X_ENABLE_DIAG_WATCH)
    target_compile_definitions(ps2_runtime PUBLIC PS2X_ENABLE_DIAG_WATCH=1)
else()
    target_compile_definitions(ps2_runtime PUBLIC PS2X_ENABLE_DIAG_WATCH=0)
endif()

# EE1P2: TS2 observer/G2b diagnostic tracers. They hook every dispatch, load
# and store; keep them out of every generated function by default on all
# platforms, diag/det builds opt back in via the build scripts. The split120
# product path (ps2_ts2_split60 rider-pass continuation) stays always-on
# behind one runtime gate per hot path.
option(PS2X_ENABLE_TS2_DIAG "Compile the TS3 case census (PS2X_TS2_CASE_COUNT)" OFF)
if(PS2X_ENABLE_TS2_DIAG)
    target_compile_definitions(ps2_runtime PUBLIC PS2X_ENABLE_TS2_DIAG=1)
else()
    target_compile_definitions(ps2_runtime PUBLIC PS2X_ENABLE_TS2_DIAG=0)
endif()

option(PS2X_ENABLE_DET_HASH_TAP "Compile the bounded deterministic VBlank XXH64 tap" OFF)
if(PS2X_ENABLE_DET_HASH_TAP)
    target_compile_definitions(ps2_runtime PUBLIC PS2X_ENABLE_DET_HASH_TAP=1)
else()
    target_compile_definitions(ps2_runtime PUBLIC PS2X_ENABLE_DET_HASH_TAP=0)
endif()

# VR4 D1: vector form of the exact VU FMAC core (ps2_vu_fmac_simd.h), on by
# default (VR4 gate: bit-exact vs the scalar form, det-identical). The scalar
# form stays compiled as the reference and for -DPS2X_VU1_FMAC_SIMD=OFF.
# Needs clang/GCC vector extensions.
option(PS2X_VU1_FMAC_SIMD "Use the vector form of the exact VU FMAC core" ON)
if(PS2X_VU1_FMAC_SIMD AND MSVC)
    message(STATUS "PS2X: MSVC has no vector extensions; VU FMAC core stays scalar")
    set(PS2X_VU1_FMAC_SIMD OFF)
endif()
if(PS2X_VU1_FMAC_SIMD)
    target_compile_definitions(ps2_runtime PUBLIC PS2X_VU1_FMAC_SIMD=1)
else()
    target_compile_definitions(ps2_runtime PUBLIC PS2X_VU1_FMAC_SIMD=0)
endif()

option(PS2X_ENABLE_SBR_TRIPWIRE "Route signed-branch predicates through the SB1 32/64-bit tripwire" OFF)
if(PS2X_ENABLE_SBR_TRIPWIRE)
    target_compile_definitions(ps2_runtime PUBLIC PS2X_ENABLE_SBR_TRIPWIRE=1)
else()
    target_compile_definitions(ps2_runtime PUBLIC PS2X_ENABLE_SBR_TRIPWIRE=0)
endif()

if(PS2X_ENABLE_AGRESSIVE_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        AGRESSIVE_LOGS=1
        PS2_FUNCTION_LOG_TRACKER=1
    )
endif()

# Host present helpers for the GS backends (the target kept its G44 name;
# CN2b removed the paraLLEl-GS shadow/live backend and PS2X_GS_SHADOW_PARALLEL).
add_library(ps2_gs_shadow STATIC
    src/lib/gs/ps2_present_share.cpp # HR1 shared-frame mailbox (PS2X_PRESENT_ZERO_COPY; iOS GE1 export)
    src/lib/gs/ps2_present_vk_ledger.cpp # VK2: Android Vulkan present bookkeeping (pure, unit-tested)
    src/lib/gs/ps2_present_owner.cpp # PSO1: iOS shared-surface ownership (pure, unit-tested)
)
# HR1 prototype: GL side of PS2X_PRESENT_ZERO_COPY (macOS desktop only).
if(APPLE AND NOT PS2X_IS_IOS)
    target_sources(ps2_gs_shadow PRIVATE src/lib/gs/ps2_present_share_mac.cpp)
    target_link_libraries(ps2_gs_shadow PRIVATE "-framework OpenGL" "-framework IOSurface" "-framework CoreVideo")
elseif(PS2X_IS_IOS)
    target_sources(ps2_gs_shadow PRIVATE src/lib/gs/ps2_present_share_ios.mm)
    target_link_libraries(ps2_gs_shadow PRIVATE "-framework CoreVideo" "-framework OpenGLES" "-framework IOSurface")
elseif(PS2X_IS_ANDROID)
    # VK1 prototype: PS2X_PRESENT_VULKAN=1 (AHardwareBuffer + SurfaceControl).
    target_sources(ps2_gs_shadow PRIVATE src/lib/gs/ps2_present_vk_android.cpp)
    # The present path reads raylib's android_app (window, activity) like ps2_runtime.cpp.
    target_include_directories(ps2_gs_shadow PRIVATE ${ANDROID_NDK}/sources/android/native_app_glue)
    target_link_libraries(ps2_gs_shadow PRIVATE android log)
endif()
target_include_directories(ps2_gs_shadow PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
# C++17 (kept from the paraLLEl-GS era); the rest of the fork stays on C++20.
set_target_properties(ps2_gs_shadow PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
if(TARGET raylib)
    target_link_libraries(ps2_gs_shadow PRIVATE raylib)
endif()

target_link_libraries(ps2_runtime PUBLIC ps2_gs_shadow)
target_link_libraries(ps2_runtime PRIVATE ${CMAKE_DL_LIBS}) # GE1: load the swappable GS C API at runtime.

if(PS2X_ENABLE_IOP_RPC_TRACE)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=1
    )
else()
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=0
    )
endif()

if(PS2X_STRICT_RETURN_DIAGNOSTICS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_STRICT_RETURN_DIAGNOSTICS=1
    )
endif()

target_compile_definitions(ps2_runtime PRIVATE
    PS2X_HAS_FFMPEG=$<BOOL:${PS2X_ENABLE_FFMPEG}>
)

file(GLOB_RECURSE KERNEL_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel/*.cpp"
)

target_sources(ps2_runtime PRIVATE
    ${KERNEL_SRC_FILES}
)

file(GLOB RUNNER_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/*.cpp"
)

set(RUNNER_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/main.cpp")
set(ROOT_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

if(EXISTS "${RUNNER_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${RUNNER_MAIN_CPP}")
elseif(EXISTS "${ROOT_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${ROOT_MAIN_CPP}")
endif()

if(PS2X_GAME_CODEGEN_DIR)
    list(REMOVE_ITEM RUNNER_SRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/register_functions.cpp")
    # I21: E17 landed in-tree game objects (sub_*.cpp) beside the stub table.
    # The ps2_game_objects static lib already carries these symbols (unity
    # jumbo), so drop the in-tree copies to keep one definition per function.
    file(GLOB PS2X_IN_TREE_GAME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/sub_*.cpp")
    list(REMOVE_ITEM RUNNER_SRC_FILES ${PS2X_IN_TREE_GAME_SOURCES})
    list(LENGTH PS2X_IN_TREE_GAME_SOURCES PS2X_IN_TREE_GAME_SOURCE_COUNT)
    message(STATUS "PS2X: dropped ${PS2X_IN_TREE_GAME_SOURCE_COUNT} in-tree game sources (game objects from ${PS2X_GAME_CODEGEN_DIR})")
endif()

if(PS2X_IS_ANDROID)
    add_library(ps2EntryRunner SHARED
        ${RUNNER_SRC_FILES}
    )

    target_link_options(ps2EntryRunner PRIVATE "-Wl,--undefined=ANativeActivity_onCreate")
    # NP1: bind intra-.so calls locally (no PLT round-trip for our own
    # symbols). Android-only branch; JNI entry points stay exported.
    target_link_options(ps2EntryRunner PRIVATE "-Wl,-Bsymbolic")
else()
    add_executable(ps2EntryRunner
        ${RUNNER_SRC_FILES}
    )
endif()

if(PS2X_ENABLE_RUNNER_UNITY_BUILD)
    set_target_properties(ps2EntryRunner PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE "${PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE}"
    )
endif()

if(PS2X_ENABLE_RUNNER_PCH)
    set(PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime_macros.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime.h"
    )

    foreach(PS2X_RUNNER_GENERATED_HEADER IN ITEMS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_recompiled_functions.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_recompiled_stubs.h")
        if(EXISTS "${PS2X_RUNNER_GENERATED_HEADER}")
            list(APPEND PS2X_RUNNER_PCH_HEADERS "${PS2X_RUNNER_GENERATED_HEADER}")
        endif()
    endforeach()

    list(APPEND PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_syscalls.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_stubs.h"
    )

    target_precompile_headers(ps2EntryRunner PRIVATE ${PS2X_RUNNER_PCH_HEADERS})
endif()

if(PS2X_DEFAULT_BOOT_ELF_DEFINE)
    target_compile_definitions(ps2EntryRunner PRIVATE
        PS2X_DEFAULT_BOOT_ELF="${PS2X_DEFAULT_BOOT_ELF_DEFINE}"
    )
endif()

if(MSVC)
    target_compile_options(ps2EntryRunner PRIVATE /FS)
endif()

target_include_directories(ps2_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel
)
if(PS2X_IS_ANDROID)
    # VK1: ps2_runtime.cpp reads raylib's android_app (window, activity).
    target_include_directories(ps2_runtime PRIVATE ${ANDROID_NDK}/sources/android/native_app_glue)
endif()

# AT1: vendored SoundTouch 2.4.1 for pitch-preserving time-stretch
# (default on; PS2X_AUDIO_STRETCH=0 disables). Static, LGPL-2.1 applied
# as GPL per section 3.
add_subdirectory(third_party/soundtouch)

target_link_libraries(ps2_runtime PUBLIC ps2_host_backend ffmpeg ps2_iop ps2x_soundtouch)
target_link_libraries(ps2EntryRunner
    PRIVATE
    ps2_runtime
)

if(PS2X_GAME_CODEGEN_DIR)
    file(GLOB PS2X_GAME_SOURCES CONFIGURE_DEPENDS "${PS2X_GAME_CODEGEN_DIR}/*.cpp")
    list(FILTER PS2X_GAME_SOURCES EXCLUDE REGEX "/\\._")
    list(LENGTH PS2X_GAME_SOURCES PS2X_GAME_SOURCE_COUNT)
    message(STATUS "PS2X: game objects: ${PS2X_GAME_SOURCE_COUNT} sources from ${PS2X_GAME_CODEGEN_DIR}")
    add_library(ps2_game_objects STATIC ${PS2X_GAME_SOURCES})
    if(PS2X_ENABLE_RUNNER_UNITY_BUILD)
        set_target_properties(ps2_game_objects PROPERTIES
            UNITY_BUILD ON
            UNITY_BUILD_BATCH_SIZE "${PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE}"
        )
    endif()
    target_include_directories(ps2_game_objects PRIVATE "${PS2X_GAME_CODEGEN_DIR}")
    target_link_libraries(ps2_game_objects PUBLIC ps2_runtime)
    # BF2: optional ThinLTO on the generated game objects (Android + Mac).
    # Default OFF: unset = no property, Release flags byte-identical.
    if(PS2X_GAME_IPO AND IPO_SUPPORTED)
        # BF2c: alongside _RELEASE for the AGP RelWithDebInfo config (see ReleaseMode.cmake).
        set_property(TARGET ps2_game_objects PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET ps2_game_objects PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    endif()
    ps2x_bf2_android_march_target(ps2_game_objects)
    if(PS2X_IS_ANDROID AND NOT MSVC)
        # BF2 (extends IR1-Part2b): line tables on every Android config
        # (Release ships zero debuginfo today; RelWithDebInfo sorts this
        # after -g so it wins there). Debug info only, no codegen change.
        target_compile_options(ps2_game_objects PRIVATE -gline-tables-only)
    endif()
    target_link_libraries(ps2EntryRunner PRIVATE ps2_game_objects)
    # HL3: fail-loud codegen ABI guard — references the marker the generator
    # stamps into register_functions.cpp, so a stale generated tree fails to
    # link. Runner-target only (game-object builds): the in-tree stub table,
    # stub-mode runners and ps2x_tests never see this TU.
    target_sources(ps2EntryRunner PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/ps2_codegen_abi_guard.cpp")
endif()

# BF2: Android release line tables (always on; debug info only) and the
# optional -march (empty = no-op) on the first-party runtime targets. The
# VU0 images compile as ps2EntryRunner sources, so they ride its flags.
# Mac/iOS builds are untouched (PS2X_IS_ANDROID guard).
if(PS2X_IS_ANDROID AND NOT MSVC)
    foreach(_bf2_tgt ps2_runtime ps2_gs_shadow ps2EntryRunner)
        target_compile_options(${_bf2_tgt} PRIVATE -gline-tables-only)
    endforeach()
endif()
foreach(_bf2_tgt ps2_runtime ps2_gs_shadow ps2EntryRunner)
    ps2x_bf2_android_march_target(${_bf2_tgt})
endforeach()

# VR3: generated VU0 images (vu0_<hash>.cpp from PS2X_VU0_RECOMP_DUMP), used
# only with PS2X_VU0_RECOMP=1 at run time. Derived from game data, so they
# live outside the repo like the game codegen. (VX2: the VU1 images are gone.)
set(PS2X_VU0_RECOMP_DIR "" CACHE PATH "Directory of generated VU0 images (VR3); empty = interpreter only")
if(PS2X_VU0_RECOMP_DIR)
    # VX1: images for VU0Interpreter are vu0e_<hash>.cpp. Pre-VX1 vu0_<hash>.cpp
    # images name VU1Interpreter and are skipped (they may share the dir with
    # the new ones while older branches still build from it).
    file(GLOB PS2X_VU0_RECOMP_SOURCES CONFIGURE_DEPENDS "${PS2X_VU0_RECOMP_DIR}/vu0e_*.cpp")
    list(FILTER PS2X_VU0_RECOMP_SOURCES EXCLUDE REGEX "/\\._")
    list(LENGTH PS2X_VU0_RECOMP_SOURCES PS2X_VU0_RECOMP_SOURCE_COUNT)
    message(STATUS "PS2X: VU0 recomp: ${PS2X_VU0_RECOMP_SOURCE_COUNT} images from ${PS2X_VU0_RECOMP_DIR}")
    # VX1 transition: a pre-VX1 image (vu0_<hash>.cpp) with no vu0e_ twin is
    # converted at configure time into the build dir. The conversion is the
    # emitter's own VU0 rename (VuCore::emitRecompSource): only the class,
    # include and macro names change, so the generated code is the same.
    # Drop this once the shared VU0 image dirs hold vu0e_ files.
    file(GLOB PS2X_VU0_RECOMP_OLD_SOURCES CONFIGURE_DEPENDS "${PS2X_VU0_RECOMP_DIR}/vu0_*.cpp")
    list(FILTER PS2X_VU0_RECOMP_OLD_SOURCES EXCLUDE REGEX "/\\._")
    foreach(_vu0_old ${PS2X_VU0_RECOMP_OLD_SOURCES})
        get_filename_component(_vu0_name "${_vu0_old}" NAME)
        string(REGEX REPLACE "^vu0_" "vu0e_" _vu0_name "${_vu0_name}")
        if(EXISTS "${PS2X_VU0_RECOMP_DIR}/${_vu0_name}")
            continue()
        endif()
        file(READ "${_vu0_old}" _vu0_text)
        string(REPLACE "#include \"ps2_vu1_recomp_gen.h\"" "#include \"ps2_vu0_recomp_gen.h\"" _vu0_text "${_vu0_text}")
        string(REPLACE "using VU1 = VU1Interpreter;" "using VU0 = VU0Interpreter;" _vu0_text "${_vu0_text}")
        string(REPLACE "VU1::" "VU0::" _vu0_text "${_vu0_text}")
        string(REPLACE "(VU1 &vu," "(VU0 &vu," _vu0_text "${_vu0_text}")
        string(REPLACE "PS2X_VU1_MUSTTAIL" "PS2X_VU_MUSTTAIL" _vu0_text "${_vu0_text}")
        string(REPLACE "PS2X_VU1_NOINLINE" "PS2X_VU_NOINLINE" _vu0_text "${_vu0_text}")
        set(_vu0_out "${CMAKE_CURRENT_BINARY_DIR}/vu0e_compat/${_vu0_name}")
        set(_vu0_prev "")
        if(EXISTS "${_vu0_out}")
            file(READ "${_vu0_out}" _vu0_prev)
        endif()
        if(NOT _vu0_prev STREQUAL _vu0_text)
            file(WRITE "${_vu0_out}" "${_vu0_text}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_vu0_old}")
        list(APPEND PS2X_VU0_RECOMP_SOURCES "${_vu0_out}")
        message(STATUS "PS2X: VU0 recomp: converted pre-VX1 ${_vu0_old} -> ${_vu0_out}")
    endforeach()
    list(LENGTH PS2X_VU0_RECOMP_SOURCES PS2X_VU0_RECOMP_SOURCE_COUNT)
    # VX2: ps2xTest compiles the same list (vu0e_ + converted) for its
    # local-only game-image differential.
    set_property(GLOBAL PROPERTY PS2X_VU0_RECOMP_SOURCES_ALL ${PS2X_VU0_RECOMP_SOURCES})
    target_sources(ps2EntryRunner PRIVATE ${PS2X_VU0_RECOMP_SOURCES})
    set_source_files_properties(${PS2X_VU0_RECOMP_SOURCES} PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION ON
        SKIP_PRECOMPILE_HEADERS ON
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu;${CMAKE_CURRENT_SOURCE_DIR}/src/lib")
endif()

# OM1 P5: offline microVU core, statically linked (iOS app). PS2X_OM1_STAGE is
# a private dir with OfflineCore.{h,cpp}, OfflineBridge.cpp, OM1StubHost.cpp
# (hwIntcIrq abort stub), the recorded .s + *_tables.c pairs, and an `armsx2`
# symlink to the ARMSX2 tree (headers + tests/ctest StubHost).
# PS2X_OM1_LIB is a private iOS PCSX2 build dir (*.a glob, GI1's shape).
# Both or neither. Nothing ARMSX2-derived is committed here; the stage and
# the prebuilt lib stay private, like the game codegen.
set(PS2X_OM1_STAGE "" CACHE PATH "OM1 offline stage dir (core+tables+armsx2 symlink)")
set(PS2X_OM1_LIB "" CACHE PATH "OM1 iOS PCSX2 prebuilt dir (*.a)")
if((PS2X_OM1_STAGE AND NOT PS2X_OM1_LIB) OR (PS2X_OM1_LIB AND NOT PS2X_OM1_STAGE))
    message(FATAL_ERROR "PS2X: PS2X_OM1_STAGE and PS2X_OM1_LIB go together")
endif()
if(PS2X_OM1_STAGE)
    foreach(f OfflineCore.cpp OfflineBridge.cpp OM1StubHost.cpp)
        if(NOT EXISTS "${PS2X_OM1_STAGE}/${f}")
            message(FATAL_ERROR "PS2X: OM1 stage missing ${f}")
        endif()
    endforeach()
    if(NOT EXISTS "${PS2X_OM1_STAGE}/armsx2/pcsx2/Config.h")
        message(FATAL_ERROR "PS2X: OM1 stage needs armsx2/ -> ARMSX2 tree (pcsx2/Config.h)")
    endif()
    if(NOT EXISTS "${PS2X_OM1_STAGE}/armsx2/tests/ctest/core/StubHost.cpp")
        message(FATAL_ERROR "PS2X: OM1 stage armsx2/ lacks tests/ctest/core/StubHost.cpp")
    endif()
    enable_language(ASM)
    file(GLOB PS2X_OM1_SOURCES CONFIGURE_DEPENDS
        "${PS2X_OM1_STAGE}/OfflineCore.cpp"
        "${PS2X_OM1_STAGE}/OfflineBridge.cpp"
        "${PS2X_OM1_STAGE}/OM1StubHost.cpp"
        "${PS2X_OM1_STAGE}/armsx2/tests/ctest/core/StubHost.cpp"
        "${PS2X_OM1_STAGE}/*.s"
        "${PS2X_OM1_STAGE}/*_tables.c")
    list(FILTER PS2X_OM1_SOURCES EXCLUDE REGEX "/\\._")
    list(LENGTH PS2X_OM1_SOURCES PS2X_OM1_SOURCE_COUNT)
    message(STATUS "PS2X: OM1 offline: ${PS2X_OM1_SOURCE_COUNT} sources from ${PS2X_OM1_STAGE}")
    file(GLOB_RECURSE PS2X_OM1_ARCHIVES "${PS2X_OM1_LIB}/*.a")
    list(FILTER PS2X_OM1_ARCHIVES EXCLUDE REGEX "/\\._")
    list(FILTER PS2X_OM1_ARCHIVES EXCLUDE REGEX ".*/sdl3/.*\\.a$")
    list(LENGTH PS2X_OM1_ARCHIVES PS2X_OM1_ARCHIVE_COUNT)
    message(STATUS "PS2X: OM1 archives: ${PS2X_OM1_ARCHIVE_COUNT} from ${PS2X_OM1_LIB}")
    target_compile_definitions(ps2_runtime PRIVATE PS2X_MICROVU_STATIC=1)
    set_source_files_properties(${PS2X_OM1_SOURCES} PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION ON
        SKIP_PRECOMPILE_HEADERS ON
        COMPILE_DEFINITIONS PCSX2_RECOMPILER_TESTS
        # Quoted PCSX2 includes ("Config.h", ...) must win over same-spelled
        # runner headers on case-insensitive APFS (raylib's config.h shadows
        # "Config.h" via the target -I dirs, which precede per-source -I).
        COMPILE_OPTIONS "-iquote${PS2X_OM1_STAGE}/armsx2/pcsx2"
        INCLUDE_DIRECTORIES "${PS2X_OM1_STAGE};${PS2X_OM1_STAGE}/armsx2;${PS2X_OM1_STAGE}/armsx2/pcsx2;${PS2X_OM1_STAGE}/armsx2/3rdparty/imgui/include;${PS2X_OM1_STAGE}/armsx2/3rdparty/fmt/include;${PS2X_OM1_STAGE}/armsx2/3rdparty/fast_float/include;${PS2X_OM1_STAGE}/armsx2/3rdparty/rapidjson/include;${PS2X_OM1_STAGE}/armsx2/3rdparty/vixl/include;${PS2X_OM1_STAGE}/armsx2/3rdparty/include;${PS2X_OM1_LIB}/common/include")
    if(APPLE)
        # IB3: close the OM1 island behind `ld -r` (ps2x_om1_close.sh) instead
        # of linking the stage + OM1 archives raw. The OM1 and GE1 archive
        # sets are two full PCSX2 cores (~20k duplicate defined globals:
        # EmuFolders, CocoaTools, ...); raw linking lets OM1's copies win
        # (ARMSX2 DataRoot, upstream resources path) and crash-loops GE1 init
        # on device. The closed object keeps global only the 5 ps2x_microvu_*
        # C functions and the _om1* dlsym slots; everything else (ARMSX2 VU
        # state, EmuConfig, StringUtil, ...) resolves inside the island. The
        # script fails the build on any duplicate vs the GE1 archives, any
        # lost export, or any dlsym slot that stops resolving. Non-Apple
        # keeps the direct link (different linker, no GE1 consumer there).
        add_library(ps2x_om1_stage STATIC ${PS2X_OM1_SOURCES})
        set_target_properties(ps2x_om1_stage PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            UNITY_BUILD OFF)
        target_include_directories(ps2x_om1_stage PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        find_program(PS2X_OM1_NM nm)
        if(NOT PS2X_OM1_NM)
            message(FATAL_ERROR "PS2X: OM1 close needs nm on PATH")
        endif()
        if(CMAKE_OSX_SYSROOT)
            set(PS2X_OM1_XCRUN_SDK -sdk ${CMAKE_OSX_SYSROOT})
        else()
            set(PS2X_OM1_XCRUN_SDK "")
        endif()
        execute_process(COMMAND xcrun ${PS2X_OM1_XCRUN_SDK} -find ld
            OUTPUT_VARIABLE PS2X_OM1_LD OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE PS2X_OM1_LD_RES)
        if(NOT PS2X_OM1_LD_RES EQUAL 0)
            message(FATAL_ERROR "PS2X: OM1 close needs xcrun ld (sysroot '${CMAKE_OSX_SYSROOT}')")
        endif()
        # The GE1 link set for the duplicate check (same shape as the GI1
        # block below; empty in OM1-only builds, and the check skips itself).
        set(PS2X_OM1_GE1_CHECK "")
        if(PS2X_GE1_IOS_BUILD)
            file(GLOB_RECURSE PS2X_OM1_GE1_CHECK "${PS2X_GE1_IOS_BUILD}/*.a")
            list(FILTER PS2X_OM1_GE1_CHECK EXCLUDE REGEX "/\\._")
            list(FILTER PS2X_OM1_GE1_CHECK EXCLUDE REGEX ".*/sdl3/.*\\.a$")
        endif()
        set(PS2X_OM1_STAGE_CLOSED "${CMAKE_CURRENT_BINARY_DIR}/om1-close/stage_closed.o")
        set(PS2X_OM1_CLOSED "${CMAKE_CURRENT_BINARY_DIR}/om1-close/om1_closed.o")
        # $<TARGET_FILE:> under the Xcode generator spells the product with
        # an unexpanded $(EFFECTIVE_PLATFORM_NAME) that VERBATIM then
        # shell-escapes into a nonexistent path, so spell the product out:
        # single-config Release + the SDK we configured for pins it.
        if(CMAKE_OSX_SYSROOT MATCHES "iphonesimulator")
            set(PS2X_OM1_EPN "-iphonesimulator")
        elseif(CMAKE_OSX_SYSROOT MATCHES "iphoneos")
            set(PS2X_OM1_EPN "-iphoneos")
        else()
            set(PS2X_OM1_EPN "")
        endif()
        set(PS2X_OM1_STAGE_LIB
            "${CMAKE_CURRENT_BINARY_DIR}/Release${PS2X_OM1_EPN}/libps2x_om1_stage.a")
        add_custom_command(
            OUTPUT "${PS2X_OM1_STAGE_CLOSED}" "${PS2X_OM1_CLOSED}"
            COMMAND /bin/bash "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ps2x_om1_close.sh"
                "${PS2X_OM1_LD}" "${PS2X_OM1_NM}" "${CMAKE_AR}"
                "${PS2X_OM1_STAGE_CLOSED}" "${PS2X_OM1_CLOSED}" "${PS2X_OM1_STAGE}"
                "${PS2X_OM1_STAGE_LIB}" -- ${PS2X_OM1_ARCHIVES} -- ${PS2X_OM1_GE1_CHECK}
            DEPENDS ps2x_om1_stage ${PS2X_OM1_ARCHIVES}
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ps2x_om1_close.sh"
            COMMENT "PS2X: closing OM1 island (ld -r + duplicate check)"
            VERBATIM)
        add_custom_target(ps2x_om1_closed DEPENDS "${PS2X_OM1_CLOSED}")
        add_dependencies(ps2EntryRunner ps2x_om1_closed)
        target_link_libraries(ps2EntryRunner PRIVATE "${PS2X_OM1_CLOSED}")
    else()
        target_link_libraries(ps2EntryRunner PRIVATE ${PS2X_OM1_ARCHIVES})
        target_sources(ps2EntryRunner PRIVATE ${PS2X_OM1_SOURCES})
    endif()
    # SDL3 C-API stubs (gi1_sdl3_stubs.cpp, folded from GI1: the offline closure
    # pulls the same SDL input/audio/FF objects the GS closure does, and PCSX2's
    # own libSDL3.a stays excluded — its UIKit classes collide with the app's
    # SDL2 at launch. Never called on the OM1 path. The NOT TARGET guard keeps
    # combined GE1+OM1 app builds configuring (both blocks link the same lib).)
    if(NOT TARGET ps2x_gi1_sdl3_stubs)
        add_library(ps2x_gi1_sdl3_stubs STATIC src/lib/gi1_sdl3_stubs.cpp)
    endif()
    target_link_libraries(ps2EntryRunner PRIVATE ps2x_gi1_sdl3_stubs)
endif()
if(PS2X_OM1_STAGE AND PS2X_IS_IOS)
    # Frameworks the offline closure needs beyond what the app already links
    # (same pulled universe as the GS closure: Metal GS, cubeb audio, USB).
    # IB2: MetalFX ships only in the iphoneos SDK (no MetalFX.framework in
    # the sim SDK), so it links device-only; a33116c added it for the
    # on-device spatial scaler. Simulator restores OM1's proven pre-MetalFX link.
    if(NOT CMAKE_OSX_SYSROOT MATCHES "iphonesimulator")
        target_link_libraries(ps2EntryRunner PRIVATE "-framework MetalFX")
    endif()
    target_link_libraries(ps2EntryRunner PRIVATE
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
        "-framework CoreVideo"
        "-framework IOSurface"
        "-framework CoreMedia"
        "-framework AVFoundation"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework CoreMotion"
        "-framework GameController"
        "-framework CoreHaptics"
        "-framework CoreBluetooth"
        "-framework ImageIO"
        "-lz"
    )
endif()

if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_VITA AND NOT PS2X_IS_ANDROID AND NOT PS2X_IS_IOS)
    target_sources(ps2EntryRunner PRIVATE
        src/lib/ps2_debug_panel.cpp
    )
    target_link_libraries(ps2EntryRunner PRIVATE rlImGui imgui)
    target_compile_definitions(ps2EntryRunner PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

ps2x_stage_ffmpeg_runtime_dlls(ps2EntryRunner)

if(PS2X_IS_VITA)
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1") 
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

    if(PS2X_VITA_CREATE_PACKAGE)
        set(PS2X_VITA_VPK_FILES "")

        foreach(PS2X_VITA_MODULE IN ITEMS
            libgpu_es4_ext.suprx
            libIMGEGL.suprx
            libGLESv2.suprx
            libpvrPSP2_WSEGL.suprx
            libGL.suprx)
            set(PS2X_VITA_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vita/module/${PS2X_VITA_MODULE}")

            if(EXISTS "${PS2X_VITA_MODULE_PATH}")
                list(APPEND PS2X_VITA_VPK_FILES FILE "${PS2X_VITA_MODULE_PATH}" "module/${PS2X_VITA_MODULE}")
            else()
                message(WARNING
                    "Missing ${PS2X_VITA_MODULE_PATH}; the vpk will rely on ur0:data/external/ for it")
            endif()
        endforeach()

        vita_create_self(eboot.bin ps2EntryRunner UNSAFE)
        vita_create_vpk(ps2EntryRunner.vpk ${PS2X_VITA_TITLEID} eboot.bin
            VERSION ${PS2X_VITA_VERSION}
            NAME ${PS2X_VITA_APP_NAME}
            ${PS2X_VITA_VPK_FILES}
        )
    endif()
endif()

# IG1: multi-config generators (Xcode, the iOS build) leave CMAKE_BUILD_TYPE
# empty, so key on the generator too. Everything below is per-config already
# (INTERPROCEDURAL_OPTIMIZATION_RELEASE, $<CONFIG:...>), so single-config
# builds are unchanged.
get_property(PS2X_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR PS2X_MULTI_CONFIG)
    EnableFastReleaseMode(ps2_runtime)
    EnableFastReleaseMode(ps2EntryRunner)

    if(PS2X_DEV_NO_THINLTO)
        # Dev-only: undo the INTERPROCEDURAL_OPTIMIZATION_RELEASE set above so the
        # runner objects compile and link without ThinLTO (faster incremental
        # links). Default OFF: with the option unset this block is a no-op and
        # Release flags are byte-identical with or without this change.
        set_property(TARGET ps2_runtime PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
        set_property(TARGET ps2EntryRunner PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
    endif()

    if(WIN32 AND NOT PS2X_SHOW_WINDOWS_CONSOLE)
        if(MSVC)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/SUBSYSTEM:WINDOWS>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/ENTRY:mainCRTStartup>
            )
        elseif(MINGW)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-mwindows>
            )
        endif()
    endif()
endif()

# Work around WinAPI vs raylib symbol clash for CloseWindow on x64
if(MSVC)
    target_link_options(ps2EntryRunner PRIVATE "/FORCE:MULTIPLE")
endif()

# Vita: Work around for GCC 10's LTO plugin
if(PS2X_IS_VITA)
    target_link_options(ps2EntryRunner PRIVATE "-Wl,--allow-multiple-definition")
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_vita_runtime.cpp)
    set_source_files_properties(src/lib/ps2_vita_runtime.cpp PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION TRUE
        COMPILE_OPTIONS "-fno-lto"
    )
endif()

if(PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_android_runtime.cpp src/lib/ps2_android_toast.cpp)
    # UX1: the toast TU reads raylib's android_app like ps2_runtime.cpp (VK1).
    target_include_directories(ps2EntryRunner PRIVATE ${ANDROID_NDK}/sources/android/native_app_glue)
    # AD1: ADPF performance hints (API 33+ dlsym; Android-only TU on
    # ps2_runtime so the EE/MTVU/GS call sites link it).
    target_sources(ps2_runtime PRIVATE src/lib/ps2_adpf.cpp)
endif()

if(PS2X_IS_IOS)
    # I6: simctl install requires a CFBundleIdentifier (was: install exit
    # 13 "Missing bundle ID"). Xcode-generator iOS executables are always
    # bundles, so only the identifier is needed. Desktop-inert: PS2X_IS_IOS
    # is OFF when CMAKE_SYSTEM_NAME is Darwin.
    set_target_properties(ps2EntryRunner PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.ps2x.ps2entryrunner"
    )
    # I7: SDL video init fails without UIKit entry (I6 gap 1: WARNING: SDL:
    # Failed to initialize SDL + SEGV in rlLoadTexture). Link SDL2main, which
    # provides the real main(): UIKit app delegate + runloop that calls our
    # main (renamed to SDL_main by SDL_main.h, included from main.cpp under
    # TARGET_OS_IPHONE). libSDL2.a itself already links via raylib (PLATFORM
    # SDL); only the entry object was missing. Own find_package: raylib's is
    # scoped to its FetchContent subdir, and SDL2_DIR is guaranteed set by
    # the FATAL_ERROR above. Desktop-inert: PS2X_IS_IOS is OFF on Darwin.
    find_package(SDL2 REQUIRED)
    target_link_libraries(ps2EntryRunner PRIVATE SDL2::SDL2main)
    # I25: iOS startup glue (ps2x.env layers, Settings switch, SDL hints,
    # window-size sync). PS2X_IOS gates the call sites in main.cpp and
    # ps2_runtime.cpp; CoreFoundation for CFPreferences. Kept out of unity
    # batches so its SDL include can't leak into sibling TUs.
    target_sources(ps2_runtime PRIVATE src/lib/ps2_ios_runtime.mm)
    set_source_files_properties(src/lib/ps2_ios_runtime.mm PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    target_compile_definitions(ps2_runtime PUBLIC PS2X_IOS=1)
    target_link_libraries(ps2_runtime PRIVATE "-framework CoreFoundation")
    # I7b: the iOS 27 sim traps in UIKit scene setup before SDL_main runs
    # when the bundle has no scene manifest (EXC_BREAKPOINT in
    # __UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption;
    # I7 minprobe A traps / B runs). Point Xcode at our template:
    # verbatim copy of CMake's generated plist + the minimal manifest
    # (no scene delegate class; SDL's delegate handles it). The template
    # hardcodes the identifier (same value as GUI_IDENTIFIER above).
    # Desktop-inert: PS2X_IS_IOS-gated.
    set_target_properties(ps2EntryRunner PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist"
    )
    # IP1: universal device family so iPadOS runs full screen (was: Xcode default 1, iPhone compat box).
    set_target_properties(ps2EntryRunner PROPERTIES XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
    # IB3 (AirPods): AVAudioSession lives here; the OM1/GI1 blocks link it
    # too when they are on (duplicate -framework flags are harmless).
    target_link_libraries(ps2EntryRunner PRIVATE "-framework AVFoundation")
    # GI1: statically-linked PCSX2 GS (Metal) for iOS. PS2X_GE1_IOS_BUILD is
    # the ios-device or ios-sim build dir (matching this app's SDK) holding
    # libge1_gs.a + libPCSX2.a + dep archives; PS2X_GE1_PCSX2_ROOT is the
    # pcsx2 worktree (gi1 branch) for .metal sources and fonts. Unset =
    # today's behavior (no GE1 symbols; PS2X_GS_EXTERNAL_LIBRARY=builtin
    # refuses at runtime).
    set(PS2X_GE1_IOS_BUILD "" CACHE PATH "GI1: ios-platform build dir for this SDK")
    set(PS2X_GE1_PCSX2_ROOT "" CACHE PATH "GI1: pcsx2 worktree (gi1) for Metal shaders + fonts")
    if(PS2X_GE1_IOS_BUILD AND PS2X_GE1_PCSX2_ROOT)
        file(GLOB_RECURSE PS2X_GE1_ARCHIVES "${PS2X_GE1_IOS_BUILD}/*.a")
        # Drop PCSX2's own SDL3: the app links SDL2-for-iOS via raylib, and a
        # second SDL's UIKit ObjC classes crash at launch (duplicate
        # SDL_LifecycleObserver). The GS closure never references SDL3.
        list(FILTER PS2X_GE1_ARCHIVES EXCLUDE REGEX ".*/sdl3/.*\\.a$")
        list(LENGTH PS2X_GE1_ARCHIVES PS2X_GE1_ARCHIVE_COUNT)
        message(STATUS "PS2X: GI1 static GS: ${PS2X_GE1_ARCHIVE_COUNT} archives from ${PS2X_GE1_IOS_BUILD}")
        target_link_libraries(ps2EntryRunner PRIVATE ${PS2X_GE1_ARCHIVES})
        target_compile_definitions(ps2_runtime PRIVATE PS2X_GE1_STATIC=1 PS2X_GE1_STATIC_IOSURFACE=1)
        # Frameworks the GS closure needs beyond what the app already links.
        target_link_libraries(ps2EntryRunner PRIVATE
            "-framework Metal"
            "-framework MetalKit"
            "-framework QuartzCore"
            "-framework CoreVideo"
            "-framework IOSurface"
            "-framework CoreMedia"
            "-framework AVFoundation"
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework CoreMotion"
            "-framework GameController"
            "-framework CoreHaptics"
            "-framework CoreBluetooth"
            "-framework ImageIO"
            "-lz"
        )
        # The 8 PCSX2 Metal shaders as an explicit default.metallib: CMake's
        # Xcode generator does not attach .metal sources to the compile
        # phase, so compile them with xcrun (same set the desktop
        # generateMetallib() uses; iOS triple for this SDK, matching the
        # 17.0 deployment floor). PCSX2's loadMainLibrary picks the bundled
        # default.metallib up from the app resources.
        find_program(PS2X_XCRUN xcrun REQUIRED)
        file(GLOB PS2X_GE1_METAL "${PS2X_GE1_PCSX2_ROOT}/pcsx2/GS/Renderers/Metal/*.metal")
        list(LENGTH PS2X_GE1_METAL PS2X_GE1_METAL_COUNT)
        if(NOT PS2X_GE1_METAL_COUNT EQUAL 8)
            message(FATAL_ERROR "PS2X: GI1 expected 8 .metal shaders, found ${PS2X_GE1_METAL_COUNT}")
        endif()
        if(CMAKE_OSX_SYSROOT MATCHES "iphonesimulator")
            set(PS2X_GE1_METAL_TARGET "air64-apple-ios17.0-simulator")
        else()
            set(PS2X_GE1_METAL_TARGET "air64-apple-ios17.0")
        endif()
        set(PS2X_GE1_AIRDIR "${CMAKE_CURRENT_BINARY_DIR}/ge1-metallib")
        set(PS2X_GE1_AIRFILES "")
        foreach(PS2X_GE1_SHADER ${PS2X_GE1_METAL})
            get_filename_component(PS2X_GE1_SHADER_NAME "${PS2X_GE1_SHADER}" NAME)
            set(PS2X_GE1_AIR "${PS2X_GE1_AIRDIR}/${PS2X_GE1_SHADER_NAME}.air")
            list(APPEND PS2X_GE1_AIRFILES "${PS2X_GE1_AIR}")
            add_custom_command(OUTPUT "${PS2X_GE1_AIR}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${PS2X_GE1_AIRDIR}"
                COMMAND "${PS2X_XCRUN}" -sdk "${CMAKE_OSX_SYSROOT}" metal
                    -ffast-math -std=ios-metal2.4 -target "${PS2X_GE1_METAL_TARGET}"
                    -c "${PS2X_GE1_SHADER}" -o "${PS2X_GE1_AIR}"
                DEPENDS "${PS2X_GE1_SHADER}"
                COMMENT "GI1 metal ${PS2X_GE1_SHADER_NAME}"
                VERBATIM
            )
        endforeach()
        set(PS2X_GE1_METALLIB "${PS2X_GE1_AIRDIR}/default.metallib")
        add_custom_command(OUTPUT "${PS2X_GE1_METALLIB}"
            COMMAND "${PS2X_XCRUN}" -sdk "${CMAKE_OSX_SYSROOT}" metallib
                -o "${PS2X_GE1_METALLIB}" ${PS2X_GE1_AIRFILES}
            DEPENDS ${PS2X_GE1_AIRFILES}
            COMMENT "GI1 metallib default.metallib"
            VERBATIM
        )
        add_custom_target(ps2x_ge1_metallib DEPENDS "${PS2X_GE1_METALLIB}")
        add_dependencies(ps2EntryRunner ps2x_ge1_metallib)
        target_sources(ps2EntryRunner PRIVATE "${PS2X_GE1_METALLIB}")
        set_source_files_properties("${PS2X_GE1_METALLIB}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources"
        )
        # SDL3 C-API stubs (gi1_sdl3_stubs.cpp): PCSX2's own libSDL3.a is
        # excluded from PS2X_GE1_ARCHIVES (its UIKit classes collide with the
        # app's SDL2), so these definitions satisfy the SDL input/audio/FF
        # objects libPCSX2.a pulls in. Never called on the GS path.
        # IB2: NOT TARGET guard (mirrors the OM1 block above) — IB2 is the
        # first combined GE1+OM1 configure, and the OM1 block defines this
        # same lib first.
        if(NOT TARGET ps2x_gi1_sdl3_stubs)
            add_library(ps2x_gi1_sdl3_stubs STATIC src/lib/gi1_sdl3_stubs.cpp)
        endif()
        target_link_libraries(ps2EntryRunner PRIVATE ps2x_gi1_sdl3_stubs)
        # PCSX2 fonts at the bundle top level (flat iOS layout, found via
        # GetOverridableResourcePath("fonts/...")): Roboto-Regular (mapped by
        # the GE1 adapter), RobotoMono-Medium + fa-solid-900 + promptfont
        # (ImGuiManager::LoadFontData; missing fonts abort GS open).
        file(GLOB PS2X_GE1_FONTS
            "${PS2X_GE1_PCSX2_ROOT}/bin/resources/fonts/*.ttf"
            "${PS2X_GE1_PCSX2_ROOT}/bin/resources/fonts/*.otf")
        list(LENGTH PS2X_GE1_FONTS PS2X_GE1_FONT_COUNT)
        if(NOT PS2X_GE1_FONT_COUNT EQUAL 5)
            message(FATAL_ERROR "PS2X: GI1 expected 5 PCSX2 fonts, found ${PS2X_GE1_FONT_COUNT}")
        endif()
        target_sources(ps2EntryRunner PRIVATE ${PS2X_GE1_FONTS})
        set_source_files_properties(${PS2X_GE1_FONTS} PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources/fonts"
        )
    endif()
endif()

if(APPLE)
    # iOS/tvOS/watchOS executables are bundles: install() requires a BUNDLE
    # DESTINATION for them. No-op on macOS desktop (ps2EntryRunner is not a
    # MACOSX_BUNDLE there, so no extra install rule is generated).
    install(TARGETS ps2_runtime ps2EntryRunner
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        BUNDLE DESTINATION bin
    )
else()
    install(TARGETS ps2_runtime ps2EntryRunner
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
    )
endif()

if(WIN32 AND PS2X_ENABLE_FFMPEG)
    install(DIRECTORY "${FFMPEG_BIN_DIR}/"
        DESTINATION bin
        FILES_MATCHING
        PATTERN "*.dll"
    )
endif()

install(DIRECTORY include/
    DESTINATION include
)
