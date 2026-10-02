// HL3: fail-loud codegen ABI guard. Game-object builds (PS2X_GAME_CODEGEN_DIR
// set) link this TU into ps2EntryRunner; it references the ABI marker the
// generator stamps into register_functions.cpp, so a pre-HL3 generated tree
// fails to LINK instead of silently dropping the split120 hook, and it
// FATALs at startup if a marker value ever mismatches.
#include "ps2_codegen_abi.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
struct Ps2xCodegenAbiGuard
{
    Ps2xCodegenAbiGuard()
    {
        // The load feeds a conditional abort, so no optimizer may fold the
        // reference away without the definition: a tree without the marker
        // is a link error, not a silent boot.
        if (g_ps2xCodegenAbiVersion != ps2_codegen_abi::kVersion)
        {
            std::fprintf(stderr,
                         "FATAL: codegen ABI mismatch: generated=%u runtime=%u "
                         "(stale PS2X_GAME_CODEGEN_DIR? regenerate with ps2_recomp)\n",
                         g_ps2xCodegenAbiVersion, ps2_codegen_abi::kVersion);
            std::abort();
        }
    }
};

static const Ps2xCodegenAbiGuard g_ps2xCodegenAbiGuard;
} // namespace
