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

set(PS2X_IS_IOS OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PS2X_IS_IOS ON)
endif()

option(PS2X_VITA_CREATE_PACKAGE "Create Vita eboot/vpk targets" ON)
set(PS2X_VITA_APP_NAME "PS2 Retro X" CACHE STRING "Display name for the Vita bubble")
set(PS2X_VITA_TITLEID "RANJ00001" CACHE STRING "9-character Vita title id")
set(PS2X_VITA_VERSION "01.00" CACHE STRING "Vita app version")
set(PS2X_DEFAULT_BOOT_ELF "" CACHE STRING "Guest ELF path passed directly to main() when argv is unavailable")

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
    src/lib/ps2_iop_host.cpp
    src/lib/ps2_memory.cpp
    src/lib/ps2_pad.cpp
    src/lib/ps2_runtime.cpp
    src/lib/ps2_savestate.cpp
    src/lib/ps2_vif1_interpreter.cpp
    src/lib/vu/ps2_vu1_core.cpp
    src/lib/vu/ps2_vu1_upper.cpp
    src/lib/vu/ps2_vu1_recomp.cpp
    src/lib/vu/ps2_vu1_lower.cpp
    src/lib/games_database.cpp
)

# TL1 Part 1b: the N8D7M5 word-watch formerly compiled into
# gs_replay_core.cpp behind PS2X_GS_REPLAY_WORD_WATCH is removed; the
# replay core has no probe-code dependency.
if(PS2X_ENABLE_RUNTIME_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2_RUNTIME_LOGS=1
    )
endif()

# E40-E44 guest-memory watch taps in ps2_runtime_macros.h. Keep them out of
# every generated function by default on all platforms; dev lanes opt in.
option(PS2X_ENABLE_DIAG_TAPS "Compile the E40-E44 guest-memory watch taps into the READ/WRITE macros" OFF)
if(PS2X_ENABLE_DIAG_TAPS)
    target_compile_definitions(ps2_runtime PUBLIC PS2X_ENABLE_DIAG_TAPS=1)
else()
    target_compile_definitions(ps2_runtime PUBLIC PS2X_ENABLE_DIAG_TAPS=0)
endif()

option(PS2X_ENABLE_DET_HASH_TAP "Compile the bounded deterministic VBlank XXH64 tap" OFF)
if(PS2X_ENABLE_DET_HASH_TAP)
    target_compile_definitions(ps2_runtime PUBLIC PS2X_ENABLE_DET_HASH_TAP=1)
else()
    target_compile_definitions(ps2_runtime PUBLIC PS2X_ENABLE_DET_HASH_TAP=0)
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

# G44 synchronous paraLLEl shadow (GPU GS backend pathfinder). Env-gated at
# runtime (PS2X_GS_SHADOW=parallel); unset = zero behavior change. The backend
# itself is only linked when PS2X_GS_SHADOW_PARALLEL=ON with a LOCAL
# parallel-gs source dir (G43 working copy); no network fetch of GS sources.
option(PS2X_GS_SHADOW_PARALLEL "Build the paraLLEl-GS in-process shadow backend (G44)" OFF)
set(PS2X_PARALLEL_GS_SOURCE_DIR "" CACHE PATH "Local parallel-gs source dir for the G44 shadow backend")

add_library(ps2_gs_shadow STATIC
    src/lib/gs/ps2_gs_shadow.cpp
    src/lib/gs/ps2_gs_parallel_backend.cpp # GB3 Part 2: live backend (same paraLLEl wiring)
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
    target_link_libraries(ps2_gs_shadow PRIVATE android log)
endif()
target_include_directories(ps2_gs_shadow PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
# C++17: matches the Granite objects and keeps std::is_pod (used by
# parallel-gs headers) available; the rest of the fork stays on C++20.
set_target_properties(ps2_gs_shadow PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
if(TARGET raylib)
    target_link_libraries(ps2_gs_shadow PRIVATE raylib)
    target_compile_definitions(ps2_gs_shadow PRIVATE PS2X_SHADOW_HAVE_RAYLIB=1)
endif()

if(PS2X_GS_SHADOW_PARALLEL)
    if(NOT PS2X_PARALLEL_GS_SOURCE_DIR OR NOT EXISTS "${PS2X_PARALLEL_GS_SOURCE_DIR}/gs/gs_interface.hpp")
        message(FATAL_ERROR "PS2X_GS_SHADOW_PARALLEL=ON requires PS2X_PARALLEL_GS_SOURCE_DIR pointing at a local parallel-gs checkout (G43 working copy)")
    endif()
    # Standalone: headless-appropriate (null platform, no tools), embedded
    # slangmosh shaders, no runtime shader compiler.
    set(PARALLEL_GS_STANDALONE ON CACHE BOOL "" FORCE)
    add_subdirectory("${PS2X_PARALLEL_GS_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/parallel-gs-shadow" EXCLUDE_FROM_ALL)
    target_link_libraries(ps2_gs_shadow PUBLIC parallel-gs)
    # PUBLIC so consumers (incl. the suite) can branch on backend presence.
    target_compile_definitions(ps2_gs_shadow PUBLIC PS2X_HAS_PARALLEL_SHADOW=1)
    message(STATUS "PS2X: G44 parallel-gs shadow backend ON (standalone, from ${PS2X_PARALLEL_GS_SOURCE_DIR})")
endif()

target_link_libraries(ps2_runtime PUBLIC ps2_gs_shadow)

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

target_link_libraries(ps2_runtime PUBLIC ps2_host_backend ffmpeg ps2_iop)
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
    target_link_libraries(ps2EntryRunner PRIVATE ps2_game_objects)
endif()

# VR1: generated VU1 images (vu1_<hash>.cpp from PS2X_VU1_RECOMP_DUMP). They are
# derived from game data, so they live outside the repo like the game codegen.
# Compiled straight into the runner so their static registrations are linked.
set(PS2X_VU1_RECOMP_DIR "" CACHE PATH "Directory of generated VU1 images (VR1); empty = interpreter only")
if(PS2X_VU1_RECOMP_DIR)
    file(GLOB PS2X_VU1_RECOMP_SOURCES CONFIGURE_DEPENDS "${PS2X_VU1_RECOMP_DIR}/vu1_*.cpp")
    list(FILTER PS2X_VU1_RECOMP_SOURCES EXCLUDE REGEX "/\\._")
    list(LENGTH PS2X_VU1_RECOMP_SOURCES PS2X_VU1_RECOMP_SOURCE_COUNT)
    message(STATUS "PS2X: VU1 recomp: ${PS2X_VU1_RECOMP_SOURCE_COUNT} images from ${PS2X_VU1_RECOMP_DIR}")
    # F4: each image is a giant TU (2,048 chained pair functions); cap how many
    # compile at once so small build boxes don't OOM. Only Ninja honors pools.
    # Belt and suspenders: the per-source form is dropped by Android's CMake
    # 3.22.1 (pool defined, 0 edges use it) while desktop CMake honors it, so
    # the owning target carries the pool too. That constrains the whole (small)
    # runner target on generators that honor it; compiled output is unchanged.
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS ps2x_vu1_gen=2)
    set_target_properties(ps2EntryRunner PROPERTIES JOB_POOL_COMPILE ps2x_vu1_gen)
    target_sources(ps2EntryRunner PRIVATE ${PS2X_VU1_RECOMP_SOURCES})
    set_source_files_properties(${PS2X_VU1_RECOMP_SOURCES} PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION ON
        SKIP_PRECOMPILE_HEADERS ON
        JOB_POOL_COMPILE ps2x_vu1_gen
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu;${CMAKE_CURRENT_SOURCE_DIR}/src/lib")
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

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
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
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_android_runtime.cpp)
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
