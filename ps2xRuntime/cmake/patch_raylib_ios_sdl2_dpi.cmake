# Applied only by the iOS runtime CMake path. The exact source hash pins this
# hook to raylib c1ab645ca298a2801097931d1079b10ff7eb9df8 (5.5).
set(_source "${PS2X_RAYLIB_SOURCE_DIR}/src/platforms/rcore_desktop_sdl.c")
if(NOT EXISTS "${_source}")
    message(FATAL_ERROR "PS2X: raylib SDL source missing: ${_source}")
endif()

file(READ "${_source}" _contents)
set(_original [=[    // TODO: Implement the window scale factor calculation manually.
    TRACELOG(LOG_WARNING, "GetWindowScaleDPI() not implemented on target platform");]=])
set(_patched [=[    // iOS SDL2 window coordinates are points; GL drawable coordinates are pixels.
#if defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
    if (platform.window != NULL)
    {
        int windowWidth = 0, windowHeight = 0;
        int drawableWidth = 0, drawableHeight = 0;
        SDL_GetWindowSize(platform.window, &windowWidth, &windowHeight);
        SDL_GL_GetDrawableSize(platform.window, &drawableWidth, &drawableHeight);
        if ((windowWidth > 0) && (windowHeight > 0) &&
            (drawableWidth > 0) && (drawableHeight > 0))
        {
            scale.x = (float)drawableWidth/(float)windowWidth;
            scale.y = (float)drawableHeight/(float)windowHeight;
        }
    }
#else
    TRACELOG(LOG_WARNING, "GetWindowScaleDPI() not implemented on target platform");
#endif]=])
string(FIND "${_contents}" "${_patched}" _already_patched)
if(NOT _already_patched EQUAL -1)
    message(STATUS "PS2X: pinned raylib iOS SDL2 DPI patch already applied")
    return()
endif()

file(SHA256 "${_source}" _hash)
if(NOT _hash STREQUAL "30db3e9f5c2d1c2bf656ba06a59b5cf8c740a3024a65134a6d5edf57cc18984d")
    message(FATAL_ERROR "PS2X: unrecognized raylib SDL source hash ${_hash}")
endif()
string(FIND "${_contents}" "${_original}" _old_position)
if(_old_position EQUAL -1)
    message(FATAL_ERROR "PS2X: pinned raylib SDL2 DPI block missing")
endif()
string(REPLACE "${_original}" "${_patched}" _contents "${_contents}")
file(WRITE "${_source}" "${_contents}")
message(STATUS "PS2X: patched pinned raylib iOS SDL2 DPI path")
