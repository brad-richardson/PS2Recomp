# ci-e2e-1 synthetic e2e smoke harness. Added only when PS2X_E2E_OUT_DIR is
# set (gen_e2e_elf.py output dir: e2e_image.h + codegen/*.cpp). Links the
# recompiler output for the hand-rolled ELF against ps2_runtime (no game
# module, no disc) and boots it headless, one guest frame per vsync.
file(GLOB PS2X_E2E_SOURCES "${PS2X_E2E_OUT_DIR}/codegen/*.cpp")
list(FILTER PS2X_E2E_SOURCES EXCLUDE REGEX "/\\._")
if(NOT PS2X_E2E_SOURCES)
    message(FATAL_ERROR "PS2X_E2E_OUT_DIR=${PS2X_E2E_OUT_DIR} has no codegen/*.cpp; run gen_e2e_elf.py + ps2_recomp first")
endif()

add_executable(ps2xE2ESmoke
    ${CMAKE_CURRENT_SOURCE_DIR}/e2e_main.cpp
    ${PS2X_E2E_SOURCES})

target_include_directories(ps2xE2ESmoke PRIVATE
    ${PS2X_E2E_OUT_DIR}
    ${PS2X_E2E_OUT_DIR}/codegen
    ${CMAKE_SOURCE_DIR}/ps2xRecomp/include
    ${CMAKE_SOURCE_DIR}/ps2xAnalyzer/include
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/include
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib
    ${CMAKE_SOURCE_DIR}/ps2xIOP/include)

target_link_libraries(ps2xE2ESmoke PRIVATE
    ps2_runtime
    ps2_iop)
