// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

class PS2Memory;
struct VU1State;

namespace ps2_microvu {
// Android + Mac, default-off. The private library is loaded only when selected.
// SS4: saveReady reports "" at an E-bit job boundary (a save is exact) or a
// deferral reason mid-chain; resetForLoad drops all live JIT state after a
// state load so the next run re-seeds from the loaded VU1State + VU memories.
bool configure(bool mtvu_threaded, std::string& error);
bool selected();
void shutdown();
void run(PS2Memory& memory, uint8_t* data, VU1State& state,
         uint32_t start_pc, bool resume, uint32_t top, uint32_t itop,
         uint32_t fbrst, uint32_t budget);
std::string saveReady(const VU1State& state);
bool resetForLoad(std::string& error);
}
