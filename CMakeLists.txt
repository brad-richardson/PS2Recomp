cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VITASDK})
    set(PS2X_VITA_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake")
    if(EXISTS "${PS2X_VITA_TOOLCHAIN_FILE}")
        set(CMAKE_TOOLCHAIN_FILE "${PS2X_VITA_TOOLCHAIN_FILE}" CACHE PATH
            "Toolchain file used for cross-compiling" FORCE)
        message(STATUS "Using VitaSDK toolchain from VITASDK: ${CMAKE_TOOLCHAIN_FILE}")
    endif()
endif()

project(PS2RetroX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PS2X_BUILD_RECOMP "Build ps2xRecomp" ON)
option(PS2X_BUILD_RUNTIME "Build ps2xRuntime" ON)
option(PS2X_BUILD_ANALYZER "Build ps2xAnalyzer" ON)
option(PS2X_BUILD_TEST "Build ps2xTest" ON)
option(PS2X_BUILD_STUDIO "Build ps2xStudio" ON)

if(ANDROID)
    message(STATUS "Android target detected, building runtime only")
    set(PS2X_BUILD_RECOMP OFF)
    set(PS2X_BUILD_ANALYZER OFF)
    set(PS2X_BUILD_TEST OFF)
    set(PS2X_BUILD_STUDIO OFF)
endif()

set(PS2X_IS_ARM_TARGET OFF)
set(PS2X_IS_AARCH64_TARGET OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    set(PS2X_IS_ARM_TARGET ON)
    set(PS2X_IS_AARCH64_TARGET ON)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm|^ARM")
    set(PS2X_IS_ARM_TARGET ON)
elseif(CMAKE_OSX_ARCHITECTURES MATCHES "arm64|aarch64")
    # The Xcode generator leaves CMAKE_SYSTEM_PROCESSOR unset; honor the
    # requested Apple architectures instead so sse2neon still fetches.
    message(STATUS "CMAKE_SYSTEM_PROCESSOR unset; inferring ARM target from CMAKE_OSX_ARCHITECTURES")
    set(PS2X_IS_ARM_TARGET ON)
    set(PS2X_IS_AARCH64_TARGET ON)
elseif(CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|watchOS|visionOS)$")
    # Apple mobile OSes are ARM-only; assume ARM when the toolchain names
    # no processor rather than silently skipping sse2neon.
    message(STATUS "CMAKE_SYSTEM_PROCESSOR unset; inferring ARM target from CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}")
    set(PS2X_IS_ARM_TARGET ON)
    set(PS2X_IS_AARCH64_TARGET ON)
endif()

if((CMAKE_C_COMPILER MATCHES "arm-vita-eabi") OR
   (CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi"))
    set(PS2X_IS_ARM_TARGET ON)
endif()

if(NOT PS2X_IS_ARM_TARGET AND NOT CMAKE_SYSTEM_PROCESSOR AND CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "PS2X: CMAKE_SYSTEM_PROCESSOR is unset while cross-compiling and no ARM target could be inferred; set CMAKE_SYSTEM_PROCESSOR in the toolchain file")
endif()

if(PS2X_IS_ARM_TARGET)
    message(STATUS "ARM target detected, fetching sse2neon")

    include(FetchContent)
    FetchContent_Declare(
        sse2neon
        GIT_REPOSITORY https://github.com/DLTcollab/sse2neon.git
        GIT_TAG v1.9.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(sse2neon)

    include_directories(${sse2neon_SOURCE_DIR})
    add_compile_definitions(USE_SSE2NEON)

    if(PS2X_IS_AARCH64_TARGET AND APPLE)
        # macOS ARM64 already uses optimal defaults
        message(STATUS "macOS ARM64 - using default compiler flags")
    elseif(PS2X_IS_AARCH64_TARGET AND MSVC)
        # Windows ARM64 - MSVC already uses optimal defaults
        message(STATUS "Windows ARM64 (MSVC) - using default compiler flags")
    elseif(PS2X_IS_AARCH64_TARGET)
        # Linux ARM64 - add NEON flags
        message(STATUS "Non-Apple ARM64 - adding NEON compiler flags")

        # Try to enable crypto and CRC extensions if supported. Exactly one
        # -march per command line: add_compile_options lands after
        # CMAKE_CXX_FLAGS, so the old unconditional plain flag defeated the
        # crypto flag on every C++ TU (last wins) and C TUs never got it.
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-march=armv8-a+fp+simd+crypto+crc" COMPILER_SUPPORTS_CRYPTO_CRC)
        if(COMPILER_SUPPORTS_CRYPTO_CRC)
            add_compile_options(-march=armv8-a+fp+simd+crypto+crc)
            message(STATUS "Crypto and CRC extensions enabled")
        else()
            add_compile_options(-march=armv8-a+fp+simd)
        endif()
    endif()
endif()

# LX1c: the runtime and generated code use SSSE3/SSE4.1 intrinsics
# (_mm_shuffle_epi8, _mm_blendv_ps, _mm_insert_epi8), but x86 defaulted to
# the SSE2 baseline and failed to compile. MSVC x64 is untouched (its
# /arch spelling differs; no MSVC x86 lane exists to validate it).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(STATUS "x86-64 GCC/Clang - enabling SSE4.1 (runtime SSSE3/SSE4.1 intrinsics)")
    add_compile_options(-msse4.1)
endif()

# E53: the EE FPU and VU round the product of a multiply-add before the add
# (PCSX2 FPU.cpp MADD_S). Clang/GCC fuse a*b+c written in one expression into
# a hardware FMA by default, which changes guest math; keep them separate.
if(NOT MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off>)
endif()

# PG5 (PG2 GEN lineage): PGO instrumented build (Android only;
# -Pps2xPgoGenerate=ON, default OFF; an instrumented APK never ships).
# Plain -fprofile-generate: the profile path is not baked in, it comes from
# wrap.sh's LLVM_PROFILE_FILE (gradle adds wrap.sh only for GEN builds; NDK
# r28's %c continuous mode produced unmergeable files, PG1). The host ends
# Odin runs with force-stop (SIGKILL, no atexit flush), so a helper thread
# flushes periodically: pgo_flush.c lives outside the fork, staged on the
# build host (-Pps2xPgoHelperDir=/path/to/dir). Whole-archive, since nothing
# references the helper and the linker would drop its constructor.
if(ANDROID AND PS2X_PGO_GENERATE)
    if(PS2X_PGO_DATA)
        message(FATAL_ERROR "PG5: PS2X_PGO_GENERATE and PS2X_PGO_DATA are exclusive")
    endif()
    add_compile_options(-fprofile-generate)
    add_link_options(-fprofile-generate)
    message(STATUS "PG5: -fprofile-generate (instrumented APK)")
endif()

# OF1 (PG1 USE lineage): PGO use build (Android; -Pps2xPgoData=/path/to/profile.profdata):
# optimize with the merged profile. Merge it on the build host with the NDK's
# own llvm-profdata (the same clang must generate, merge and use — a profile
# from a different clang, e.g. Mac Homebrew clang 23, is version-rejected).
if(ANDROID AND PS2X_PGO_DATA)
    add_compile_options(-fprofile-use=${PS2X_PGO_DATA})
    add_link_options(-fprofile-use=${PS2X_PGO_DATA})
    message(STATUS "OF1: -fprofile-use=${PS2X_PGO_DATA}")
endif()

if(PS2X_BUILD_RECOMP)
    add_subdirectory("ps2xRecomp")
endif()

if(PS2X_BUILD_RUNTIME)
    add_subdirectory("ps2xIOP")
    add_subdirectory("ps2xRuntime")
endif()

# HAL1: app-local libhardware shim from source (Android only). The 'hardware'
# target builds libhardware.so, which the APK packages from this build, not
# from -Pps2xJniLibsDir (a jniLibs copy is a hard gradle error).
if(ANDROID)
    add_subdirectory("android/hwcompat")
endif()

if(PS2X_BUILD_ANALYZER)
    add_subdirectory("ps2xAnalyzer")
endif()

if(PS2X_BUILD_TEST)
    add_subdirectory("ps2xTest")
endif()

if(PS2X_BUILD_STUDIO)
    add_subdirectory("ps2xStudio")
endif()

# PG5: periodic-flush helper for the instrumented APK (see the GEN block above).
if(ANDROID AND PS2X_PGO_GENERATE AND PS2X_PGO_HELPER_DIR)
    add_library(pg2_pgo_flush STATIC ${PS2X_PGO_HELPER_DIR}/pgo_flush.c)
    target_link_libraries(ps2EntryRunner PRIVATE
        -Wl,--whole-archive pg2_pgo_flush -Wl,--no-whole-archive)
    message(STATUS "PG5: pgo_flush helper from ${PS2X_PGO_HELPER_DIR}")
endif()
