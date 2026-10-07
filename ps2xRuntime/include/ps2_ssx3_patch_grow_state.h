#pragma once

#include <cstdint>
#include <utility>
#include <vector>

// TK34's relocated cache contains guest pointers in RAM and a host-side
// reverse map used when the game frees those pointers. Both are one state.
namespace ps2_ssx3_patch_grow
{
struct State
{
    bool active = false;
    bool refused = false;
    uint32_t cache = 0u;
    std::vector<std::pair<uint32_t, uint32_t>> moved; // relocated -> original
};

struct Layout
{
    uint32_t base = 0u;
    uint32_t capacity = 0u;
};

Layout layout();
State snapshot();
bool valid(const State &state);
bool restore(const State &state);
void reset();
} // namespace ps2_ssx3_patch_grow
