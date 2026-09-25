#pragma once

#include <cstdint>

struct R5900Context;
class PS2Runtime;

// SSX 3 0x3E3968 file-table comparator with R5900 64-bit branch semantics
// (see ssx3_file_key_compare.cpp). Registered as a game override in
// ps2_runtime.cpp.
namespace ps2_ssx3_file_key
{
    void compare(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void apply(PS2Runtime &runtime);
}
