# Public Android Vulkan GS + microVU build; assembly supplied by ../build.py.
cmake_minimum_required(VERSION 3.22)
project(SSX3AndroidLibraries C CXX ASM)

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
# Android uses Vulkan and locally source-built shaderc.
set(USE_VULKAN ON CACHE BOOL "" FORCE)
# microVU bench harness sources need the recompiler test hooks.
set(ENABLE_RECOMPILER_TEST_HOOKS ON CACHE BOOL "" FORCE)
# CN1A (ARMSX2 base): the adapter links the bundled shaderc_combined itself.
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
# Supply libretro headers if the vendor dependency search has not added them.
if(NOT TARGET libretro-headers)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor-pcsx2/3rdparty/libretro" "${CMAKE_BINARY_DIR}/libretro-headers")
endif()
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor-pcsx2/pcsx2" "${CMAKE_BINARY_DIR}/pcsx2")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../adapter" "${CMAKE_BINARY_DIR}/ge1-adapter")



# Existing adapters name shaderc_combined; the public dependency recipe builds that archive.
add_library(shaderc_combined UNKNOWN IMPORTED)
set_target_properties(shaderc_combined PROPERTIES IMPORTED_LOCATION "${SHADERC_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SHADERC_INCLUDE_DIR}")
add_library(android_chd_stubs STATIC "${CMAKE_CURRENT_SOURCE_DIR}/../adapter/ax4b_android_stubs.cpp")
target_link_libraries(ge1_gs PRIVATE android_chd_stubs)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../mv2-adapter" "${CMAKE_BINARY_DIR}/mv2-adapter")
target_link_libraries(mv2_microvu PRIVATE android_chd_stubs)
