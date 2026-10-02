# VK1 Part 2: raylib's Android EGL config asks for R8G8B8 and no alpha, so the
# window buffers are RGBX (opaque). With the Vulkan present layer under the GL
# window (virtual pad on), the window must carry alpha. Adds EGL_ALPHA_SIZE 8;
# with the default opaque window format the alpha is ignored by SurfaceFlinger,
# so the GL-only path looks the same. Applied only by the Android runtime CMake
# path; the exact source hash pins it to raylib c1ab645ca298a2801097931d1079b10ff7eb9df8 (5.5).
set(_source "${PS2X_RAYLIB_SOURCE_DIR}/src/platforms/rcore_android.c")
if(NOT EXISTS "${_source}")
    message(FATAL_ERROR "PS2X: raylib Android source missing: ${_source}")
endif()

file(READ "${_source}" _contents)
set(_original [=[        EGL_BLUE_SIZE, 8,           // BLUE color bit depth (alternative: 5)
]=])
set(_patched [=[        EGL_BLUE_SIZE, 8,           // BLUE color bit depth (alternative: 5)
        EGL_ALPHA_SIZE, 8,          // PS2X VK1: alpha for a translucent window over the Vulkan layer
]=])
string(FIND "${_contents}" "${_patched}" _already_patched)
if(NOT _already_patched EQUAL -1)
    message(STATUS "PS2X: pinned raylib Android EGL alpha patch already applied")
    return()
endif()

file(SHA256 "${_source}" _hash)
if(NOT _hash STREQUAL "a17a8c75ffa9710dec042769191f07d0352af7de0c197adbcfa883d78d2234d8")
    message(FATAL_ERROR "PS2X: unrecognized raylib Android source hash ${_hash}")
endif()
string(FIND "${_contents}" "${_original}" _old_position)
if(_old_position EQUAL -1)
    message(FATAL_ERROR "PS2X: pinned raylib Android EGL config block missing")
endif()
string(REPLACE "${_original}" "${_patched}" _contents "${_contents}")
file(WRITE "${_source}" "${_contents}")
message(STATUS "PS2X: patched pinned raylib Android EGL config (alpha 8)")
