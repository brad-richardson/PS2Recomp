# AX4b (SP02): GE1 + microVU Mac top-level project.
# Home: PS2Recomp ps2xRuntime/third_party/armsx2/platform/mac. Assembled by
# build.sh mac:
#   <asm>/vendor-pcsx2 (ARMSX2 tree, armsx2-ssx3-one), <asm>/adapter (ge1),
#   <asm>/om1rt-adapter, plus pcsx2/tests/3rdparty/common/cmake links for the
#   adapters' ../ refs, <asm>/mac-platform (a copy of this dir).
# Produces: ge1_gs (libge1_gs.dylib) + ge1_replay, and, with the pinned OM1
# runtime inputs, om1rt-adapter's mv2_microvu (libmv2_microvu.dylib, the Mac
# det recipe: stale-bridge sources + microVU-arm64.cpp recompiled EMBED=1).
# Flags mirror the GE1S Mac recipe (Metal + MoltenVK) and microvu_libs.sh mac.
cmake_minimum_required(VERSION 3.22)
project(ARMSX2Mac C CXX OBJC OBJCXX ASM)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../vendor-pcsx2/cmake")
set(TOP_CMAKE_WAS_SOURCED TRUE)
set(ENABLE_QT_UI OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_QT_DEBUGGER OFF CACHE BOOL "" FORCE)
set(ENABLE_SDL_FRONTEND OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBRETRO OFF CACHE BOOL "" FORCE)
set(ENABLE_EERUNNER OFF CACHE BOOL "" FORCE)
set(ENABLE_GSRUNNER OFF CACHE BOOL "" FORCE)
set(ENABLE_VURUNNER OFF CACHE BOOL "" FORCE)
# GE1S: Vulkan on (MoltenVK) for the VK GS path next to Metal.
set(USE_VULKAN ON CACHE BOOL "" FORCE)
# microVU bench harness sources need the recompiler test hooks.
set(ENABLE_RECOMPILER_TEST_HOOKS ON CACHE BOOL "" FORCE)
# CN1A (ARMSX2 base): the adapter links the bundled shaderc_combined itself.
set(ARMSX2_USE_BUNDLED_SHADERC ON CACHE BOOL "" FORCE)

include(Pcsx2Utils)
check_no_parenthesis_in_path()
detect_operating_system()
detect_compiler()
include(BuildParameters)
include(SearchForStuff)
get_git_version_info()
write_svnrev_h()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor-pcsx2/common" "${CMAKE_BINARY_DIR}/common")
# CN1A: ARMSX2's Vulkan GS sources include VKLibretro.cpp unconditionally and need the
# header-only libretro-headers target before pcsx2/.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor-pcsx2/3rdparty/libretro" "${CMAKE_BINARY_DIR}/3rdparty/libretro")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor-pcsx2/pcsx2" "${CMAKE_BINARY_DIR}/pcsx2")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../adapter" "${CMAKE_BINARY_DIR}/ge1-adapter")

# AX4b: the Mac det microVU comes from the om1rt recipe (pinned OM1RT_* inputs,
# never committed). Guarded, never a fatal error without them.
if(DEFINED OM1RT_WORKTREE)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../om1rt-adapter" "${CMAKE_BINARY_DIR}/om1rt-adapter")
endif()

# AX4b: OM1 record tool only with private capture inputs (never fatal).
if(DEFINED OM1_CAPTURE_INCLUDE)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../om1-adapter" "${CMAKE_BINARY_DIR}/om1-adapter")
endif()
