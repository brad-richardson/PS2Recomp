cmake_minimum_required(VERSION 3.21)

project(ps2xTest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(PS2X_IOP_ENABLE_PLUGINS AND (WIN32 OR (UNIX AND NOT APPLE)))
    add_library(ps2_iop_fake_plugin MODULE
        src/fake_iop_plugin.cpp
    )
    add_library(ps2_iop_bad_abi_plugin MODULE
        src/fake_iop_bad_abi.c
    )
    add_library(ps2_iop_missing_symbol_plugin MODULE
        src/fake_iop_missing_symbol.cpp
    )

    foreach(plugin_target IN ITEMS
            ps2_iop_fake_plugin
            ps2_iop_bad_abi_plugin
            ps2_iop_missing_symbol_plugin)
        target_compile_features(${plugin_target} PRIVATE cxx_std_20)
        target_include_directories(${plugin_target} PRIVATE
            ${CMAKE_SOURCE_DIR}/ps2xIOP/include
        )
        set_target_properties(${plugin_target} PROPERTIES
            PREFIX ""
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/iop_test_plugins"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/iop_test_plugins"
        )
    endforeach()
endif()

# Static library with test logic (no main), used by ps2xStudio
add_library(ps2_test_function_table OBJECT
    src/test_function_table.cpp
)

target_include_directories(ps2_test_function_table PRIVATE
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/include
    ${CMAKE_SOURCE_DIR}/ps2xIOP/include
)

add_library(ps2_test_lib STATIC
    src/code_generator_tests.cpp
    src/r5900_decoder_tests.cpp
    src/elf_analyzer_tests.cpp
    src/pad_input_tests.cpp
    src/ps2_present_fallback_tests.cpp
    src/ps2_virtual_pad_tests.cpp
    src/ps2_env_file_tests.cpp
    src/ps2_android_env_tests.cpp
    src/ps2_thread_affinity_tests.cpp
    src/ps2_runtime_io_tests.cpp
    src/ps2_runtime_kernel_tests.cpp
    src/ps2_runtime_interrupt_tests.cpp
    src/ps2_memory_tests.cpp
    src/ps2_vu1_tests.cpp
    src/ps2_vu_tests.cpp
    src/ps2_gs_tests.cpp
    src/ps2_gs_queue_tests.cpp
    src/ps2_gs_replay_tests.cpp
    src/ps2_gs_shadow_tests.cpp
    src/ps2_iop_tests.cpp
    src/ps2_sif_rpc_tests.cpp
    src/ps2_snd_tests.cpp
    src/ps2_sif_dma_tests.cpp
    src/ps2_recompiler_tests.cpp
    src/ps2_runtime_expansion_tests.cpp
    src/ps2_gfx_stats_tests.cpp
    src/ps2_vif_mpg_log_tests.cpp
    src/ps2_vu1_trace_tests.cpp
    src/ps2_vu1_entry_trace_tests.cpp
    src/ps2_fpu_semantics_tests.cpp
    src/ps2_fpu_cop2_audit_tests.cpp
    src/ps2_mmi_interleave_tests.cpp
    src/ps2_lwu_tests.cpp
    src/ps2_ee_count_tests.cpp
    src/ps2_vsync_pacer_tests.cpp
)

if(PS2X_ENABLE_DIAG_TAPS)
    target_sources(ps2_test_lib PRIVATE
        src/ps2_mpg_src_trace_tests.cpp
        src/ps2_e41_trace_tests.cpp
        src/ps2_e43_trace_tests.cpp
        src/ps2_e44_trace_tests.cpp
    )
endif()

# E52: the audit tests execute the generator's own translation of each
# encoding; e52_snippet_gen writes it into a header at build time.
add_executable(e52_snippet_gen tools/e52_snippet_gen.cpp)
target_include_directories(e52_snippet_gen PRIVATE ${CMAKE_SOURCE_DIR}/ps2xRecomp/include)
target_link_libraries(e52_snippet_gen PRIVATE ps2_recomp_lib)
set(E52_SNIPPETS_DIR ${CMAKE_CURRENT_BINARY_DIR}/e52_generated)
add_custom_command(
    OUTPUT ${E52_SNIPPETS_DIR}/e52_snippets.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${E52_SNIPPETS_DIR}
    COMMAND $<TARGET_FILE:e52_snippet_gen> > ${E52_SNIPPETS_DIR}/e52_snippets.h
    DEPENDS e52_snippet_gen
    VERBATIM)
add_custom_target(e52_snippets DEPENDS ${E52_SNIPPETS_DIR}/e52_snippets.h)
add_executable(e54d_lwu_snippet_gen tools/e54d_lwu_snippet_gen.cpp)
target_include_directories(e54d_lwu_snippet_gen PRIVATE ${CMAKE_SOURCE_DIR}/ps2xRecomp/include)
target_link_libraries(e54d_lwu_snippet_gen PRIVATE ps2_recomp_lib)
set(E54D_SNIPPETS_DIR ${CMAKE_CURRENT_BINARY_DIR}/e54d_generated)
add_custom_command(
    OUTPUT ${E54D_SNIPPETS_DIR}/e54d_lwu_snippets.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${E54D_SNIPPETS_DIR}
    COMMAND $<TARGET_FILE:e54d_lwu_snippet_gen> > ${E54D_SNIPPETS_DIR}/e54d_lwu_snippets.h
    DEPENDS e54d_lwu_snippet_gen
    VERBATIM)
add_custom_target(e54d_lwu_snippets DEPENDS ${E54D_SNIPPETS_DIR}/e54d_lwu_snippets.h)
# E53: the audit tests set the EE FP mode at run time; keep clang from folding
# their constant inputs at compile time in the default rounding mode.
if(NOT MSVC)
    set_source_files_properties(src/ps2_fpu_cop2_audit_tests.cpp PROPERTIES COMPILE_OPTIONS "-frounding-math")
endif()

target_include_directories(ps2_test_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${E52_SNIPPETS_DIR}
    ${E54D_SNIPPETS_DIR}
    ${CMAKE_SOURCE_DIR}/ps2xRecomp/include
    ${CMAKE_SOURCE_DIR}/ps2xAnalyzer/include
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/include
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib
)

target_link_libraries(ps2_test_lib PRIVATE
    ps2_recomp_lib
    ps2_analyzer_lib
    ps2_runtime
    ps2_iop
)

add_dependencies(ps2_test_lib e52_snippets)
add_dependencies(ps2_test_lib e54d_lwu_snippets)

if(UNIX AND NOT APPLE)
    target_link_libraries(ps2_test_lib PRIVATE ${CMAKE_DL_LIBS})
endif()

if(TARGET ps2_iop_fake_plugin)
    target_compile_definitions(ps2_test_lib PRIVATE
        PS2X_TEST_IOP_PLUGIN_DIR="$<TARGET_FILE_DIR:ps2_iop_fake_plugin>"
    )
    add_dependencies(ps2_test_lib
        ps2_iop_fake_plugin
        ps2_iop_bad_abi_plugin
        ps2_iop_missing_symbol_plugin
    )
endif()

add_executable(ps2x_tests
    src/main.cpp
    $<TARGET_OBJECTS:ps2_test_function_table>
)

option(PRINT_GENERATED_CODE "Print generated code in tests" OFF)
if(PRINT_GENERATED_CODE)

    target_compile_definitions(ps2x_tests PRIVATE PRINT_GENERATED_CODE)
    target_compile_definitions(ps2_test_lib PRIVATE PRINT_GENERATED_CODE)
endif()

target_include_directories(ps2x_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/ps2xRecomp/include
    ${CMAKE_SOURCE_DIR}/ps2xAnalyzer/include
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/include
)

target_link_libraries(ps2x_tests PRIVATE
    ps2_test_lib
    ps2_recomp_lib
    ps2_analyzer_lib
    ps2_runtime
)

# VU/GS tests intentionally instantiate large interpreter state.
# Windows executables default to a much smaller stack than Linux, and
# sanitizer/debug instrumentation can push these test frames over 1 MiB.
if(MSVC)
    target_link_options(ps2x_tests PRIVATE "/STACK:8388608")
elseif(MINGW)
    target_link_options(ps2x_tests PRIVATE "--stack,8388608")
endif()

if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(ps2x_tests)
endif()

include("${CMAKE_SOURCE_DIR}/ps2xRuntime/cmake/ReleaseMode.cmake")

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_test_lib)
    EnableFastReleaseMode(ps2x_tests)
endif()
