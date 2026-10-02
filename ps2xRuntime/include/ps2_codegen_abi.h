// HL3: codegen ABI version shared by the generator (ps2xRecomp
// function_table_emitter.cpp, which stamps it into register_functions.cpp)
// and the runtime guard (ps2_codegen_abi_guard.cpp, linked into
// ps2EntryRunner for game-object builds). Bump on any codegen/runtime
// contract change; the register pin changes with it, forcing a re-pin.
#pragma once

#include <cstdint>

namespace ps2_codegen_abi
{
// 1 == splitsites-v1: plain READ32 carries no split120 hook; the generator
// emits READ32_SPLIT only at the 12 sites in ps2_ts2_splitsites.h.
inline constexpr uint32_t kVersion = 1u;
} // namespace ps2_codegen_abi

// Defined by the generated register_functions.cpp (NOT by the runtime, the
// in-tree stub table, or the test stub table).
extern const uint32_t g_ps2xCodegenAbiVersion;
