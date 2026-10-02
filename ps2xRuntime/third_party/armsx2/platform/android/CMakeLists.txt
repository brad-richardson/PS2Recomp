# AX4b (SP02): GE1 (+ microVU/OM1) Android top-level project.
# Home: PS2Recomp ps2xRuntime/third_party/armsx2/platform/android (was
# ssx3-ge1/android-platform on the PCSX2 fork). Assembled by build.sh android
# (or bradflix_ge1_lib.sh with the source switched):
#   <asm>/vendor-pcsx2 (ARMSX2 tree, armsx2-ssx3-one), <asm>/adapter (ge1),
#   [<asm>/mv2-adapter, <asm>/om1-adapter, <asm>/om1rt-adapter when staged],
#   <asm>/android-platform (this dir + the ARMSX2 cmake/3rdparty bundle).
cmake_minimum_required(VERSION 3.22)
project(GE1Android C CXX)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(TOP_CMAKE_WAS_SOURCED TRUE)
set(ENABLE_QT_UI OFF CACHE BOOL "" FORCE)
set(USE_VULKAN ON CACHE BOOL "" FORCE)
set(BAKE_SHADERS_IN_CPP OFF CACHE BOOL "" FORCE)
# CN1A (ARMSX2 base): the adapter links the bundled shaderc_combined itself, so the core must not go
# looking for Shaderc::shaderc_shared (pcsx2/CMakeLists.txt). LSFG and adrenotools stay off (their
# defaults): LSFG is never shipped, and Turnip loads through our GE1_VK_TURNIP HAL seam.
set(ARMSX2_USE_BUNDLED_SHADERC ON CACHE BOOL "" FORCE)
# BF2: Android GE1 release candidates (match the ARMSX2 release subset):
# LTO on the hot EE/VU/GS core sources (cf. NP1 -Bsymbolic on the runner).
# Output-only (GS; readback off). Default ON for the GE1 recipe; the mv2
# recipe configures with -DLTO_PCSX2_CORE=OFF (PB20: the LTO new-microVU
# build blacks Snowdream on Turnip — same sources without LTO draw, GE1N1
# §4; LTO vs -Bsymbolic unsplit, so mv2 drops both).
if(NOT DEFINED LTO_PCSX2_CORE)
  set(LTO_PCSX2_CORE ON CACHE BOOL "Enable LTO/IPO/LTCG on the subset of pcsx2 that benefits most from it but not anything else")
endif()
# BF2: local binding of intra-.so calls on ge1_gs only (was a global
# add_link_options; PB20 scopes it — mv2_microvu must not get -Bsymbolic,
# same Snowdream-black recipe as LTO, GE1N1 §4). Applied after the adapter
# subdir defines the target.

# PB20: the mv2 bridge compiles the recompiler-test harness TUs (StubHost +
# RecompilerTestEnvironment), which need PCSX2_RECOMPILER_TESTS (same setting
# as platform/mac). Scoped to mv2-staged configures so GE1-only builds keep
# their exact flags (staged GE1 6b09bf14 byte-stable). Must precede the
# pcsx2 subdir. The mv recipe also passes -DENABLE_RECOMPILER_TEST_HOOKS=ON
# (same value; harmless).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../mv2-adapter/CMakeLists.txt")
  set(ENABLE_RECOMPILER_TEST_HOOKS ON CACHE BOOL "" FORCE)
endif()

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
# header-only libretro-headers target before pcsx2/ (as in platforms/android/.../cpp/CMakeLists.txt).
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor-pcsx2/3rdparty/libretro" "${CMAKE_BINARY_DIR}/3rdparty/libretro")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor-pcsx2/pcsx2" "${CMAKE_BINARY_DIR}/pcsx2")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../adapter" "${CMAKE_BINARY_DIR}/ge1-adapter")

# AX4b: upstream CHD-sibling stub for the GE1 lib (no JNI layer provides it;
# see ax4b_android_stubs.cpp). Sourced from ../adapter: the bradflix recipe stages the
# adapter dir whole but only this file's CMakeLists from the platform dir.
if(ANDROID)
  add_library(ax4b_android_stubs STATIC "${CMAKE_CURRENT_SOURCE_DIR}/../adapter/ax4b_android_stubs.cpp")
  target_link_libraries(ge1_gs PRIVATE ax4b_android_stubs)
  target_link_options(ge1_gs PRIVATE -Wl,-Bsymbolic)
endif()

# AX4b: the mv2 bridge rides the same vendor tree (it links shaderc_combined
# from the Android bundle). Staged only by the mv recipe; the ge1 recipe
# (bradflix_ge1_lib.sh) does not stage it, so this is an EXISTS guard, never
# a fatal error.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../mv2-adapter/CMakeLists.txt")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../mv2-adapter" "${CMAKE_BINARY_DIR}/mv2-adapter")
  # PB19/PB20: mv2 pulls ChdFileReader through the harness TUs (StubHost +
  # RecompilerTestEnvironment); give it the same stub (promoted from the PB19
  # lane patch tooling/mv2-android-stub-link.patch).
  if(ANDROID AND TARGET ax4b_android_stubs AND TARGET mv2_microvu)
    target_link_libraries(mv2_microvu PRIVATE ax4b_android_stubs)
  endif()
endif()

# AX4b: OM1 record/runtime tools only with private capture inputs (never a
# fatal error without them; cf. the guarded om1 root hook on armsx2-ssx3-one).
if(DEFINED OM1_CAPTURE_INCLUDE)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../om1-adapter" "${CMAKE_BINARY_DIR}/om1-adapter")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../om1rt-adapter" "${CMAKE_BINARY_DIR}/om1rt-adapter")
endif()
