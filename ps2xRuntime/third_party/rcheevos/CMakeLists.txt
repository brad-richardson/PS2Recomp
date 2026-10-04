# ACH2: vendored rcheevos condition engine + game hash (no rc_client).
# Pin: RetroAchievements/rcheevos f87c0de (see README.ach2.md). Compiled on
# Mac, iOS and Android as part of ps2_runtime. Pure C, no sockets.
add_library(ps2x_rcheevos STATIC
    src/rcheevos/alloc.c
    src/rcheevos/condition.c
    src/rcheevos/condset.c
    src/rcheevos/consoleinfo.c
    src/rcheevos/format.c
    src/rcheevos/lboard.c
    src/rcheevos/memref.c
    src/rcheevos/operand.c
    src/rcheevos/rc_validate.c
    src/rcheevos/richpresence.c
    src/rcheevos/runtime_progress.c
    src/rcheevos/runtime.c
    src/rcheevos/trigger.c
    src/rcheevos/value.c
    src/rc_compat.c
    src/rc_util.c
    src/rc_version.c
    src/rhash/aes.c
    src/rhash/cdreader.c
    src/rhash/hash_disc.c
    src/rhash/hash_encrypted.c
    src/rhash/hash_rom.c
    src/rhash/hash_zip.c
    src/rhash/hash.c
    src/rhash/md5.c
)

# Public: ps2_ach.cpp and the suite include rc_runtime.h / rc_hash.h.
# Private: the engine's own internal headers (rc_internal.h, rc_compat.h).
target_include_directories(ps2x_rcheevos PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_include_directories(ps2x_rcheevos PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rcheevos"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rhash"
)

set_target_properties(ps2x_rcheevos PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)

if(NOT "${CMAKE_BUILD_TYPE}" MATCHES "Debug")
    if(MSVC)
        target_compile_options(ps2x_rcheevos PRIVATE /O2)
    else()
        target_compile_options(ps2x_rcheevos PRIVATE -O2)
    endif()
endif()
