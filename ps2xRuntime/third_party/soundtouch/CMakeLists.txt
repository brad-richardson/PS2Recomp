# AT1: vendored SoundTouch 2.4.1 (LGPL-2.1, applied as GPL per section 3;
# see README.at1.md). Static library, mirroring PCSX2's
# 3rdparty/soundtouch/CMakeLists.txt file list and flags.
add_library(ps2x_soundtouch STATIC
    source/SoundTouch/AAFilter.cpp
    source/SoundTouch/BPMDetect.cpp
    source/SoundTouch/cpu_detect_x86.cpp
    source/SoundTouch/FIFOSampleBuffer.cpp
    source/SoundTouch/FIRFilter.cpp
    source/SoundTouch/InterpolateCubic.cpp
    source/SoundTouch/InterpolateLinear.cpp
    source/SoundTouch/InterpolateShannon.cpp
    source/SoundTouch/mmx_optimized.cpp
    source/SoundTouch/PeakFinder.cpp
    source/SoundTouch/RateTransposer.cpp
    source/SoundTouch/SoundTouch.cpp
    source/SoundTouch/sse_optimized.cpp
    source/SoundTouch/TDStretch.cpp
)

target_include_directories(ps2x_soundtouch PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/soundtouch"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/SoundTouch"
)
target_compile_definitions(ps2x_soundtouch PUBLIC
    SOUNDTOUCH_FLOAT_SAMPLES
    ST_NO_EXCEPTION_HANDLING
)
set_target_properties(ps2x_soundtouch PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

if(NOT "${CMAKE_BUILD_TYPE}" MATCHES "Debug")
    if(MSVC)
        target_compile_options(ps2x_soundtouch PRIVATE /O2 /fp:fast)
    else()
        target_compile_options(ps2x_soundtouch PRIVATE -O3 -ffast-math)
    endif()
endif()

# Keep third-party warnings out of the build log.
if(NOT MSVC)
    target_compile_options(ps2x_soundtouch PRIVATE -w)
endif()
