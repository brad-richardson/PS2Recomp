# NP0A P5: Linux arm64 (glibc) top-level project for the microVU JIT reference.
# Assembled by build.sh linux:
#   <asm>/vendor-pcsx2 (ARMSX2 tree, armsx2-ssx3-one), <asm>/om1rt-adapter,
#   plus pcsx2/tests/3rdparty/common/cmake links for the adapter's ../ refs,
#   <asm>/linux-platform (a copy of this dir).
# Produces om1rt-adapter's mv2_microvu (libmv2_microvu.so): the Linux port of
# the Mac M7 det recipe (microVU-arm64.cpp recompiled EMBED=1). GE1/Metal/
# Vulkan are not needed for the VU1 JIT and stay off.
cmake_minimum_required(VERSION 3.22)
project(ARMSX2Linux C CXX ASM)

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
set(USE_VULKAN OFF CACHE BOOL "" FORCE)
set(ENABLE_RECOMPILER_TEST_HOOKS ON CACHE BOOL "" FORCE)
set(ARMSX2_USE_BUNDLED_SHADERC OFF CACHE BOOL "" FORCE)

include(Pcsx2Utils)
check_no_parenthesis_in_path()
detect_operating_system()
detect_compiler()
include(BuildParameters)
include(SearchForStuff)
get_git_version_info()
write_svnrev_h()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor-pcsx2/common" "${CMAKE_BINARY_DIR}/common")
if(NOT TARGET libretro-headers)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor-pcsx2/3rdparty/libretro" "${CMAKE_BINARY_DIR}/libretro-headers")
endif()
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor-pcsx2/pcsx2" "${CMAKE_BINARY_DIR}/pcsx2")

if(DEFINED OM1RT_WORKTREE)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../om1rt-adapter" "${CMAKE_BINARY_DIR}/om1rt-adapter")
endif()
