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
// With PS2X_MICROVU_STATIC (iOS app) the offline core is linked in and only
// it is available (no dlopen).
// run() returns true when the library served the job; false (offline only)
// means MISS: the job was NOT run and the caller must restart it statically.
// SS5: run() continues a cycle-budget break in place (resume iterations until
// the job ends) so the guest result is budget-independent; past the continue
// cap the job returns truncated with the park flag set. saveReady reports
// kBudgetParkedReason while a break is parked. budgetBreaks() is the
// cumulative break count (a rate-limited stderr line per break).
inline constexpr const char* kBudgetParkedReason = "vu1: budget-parked";
bool configure(bool mtvu_threaded, std::string& error);
bool selected();
void shutdown();
bool run(PS2Memory& memory, uint8_t* data, VU1State& state,
         uint32_t start_pc, bool resume, uint32_t top, uint32_t itop,
         uint32_t fbrst, uint32_t budget);
std::string saveReady(const VU1State& state);
bool resetForLoad(std::string& error);
uint64_t budgetBreaks();
}
