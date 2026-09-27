// OM1 Part 4: offline microVU core API (runtime-owned; the bridge calls it).
// Uppercase markers: OM1P4_CORE.
#pragma once

#include <cstdint>
#include <string>

enum OlcDisposition
{
    OLC_SERVED = 1, // job fully executed; bridge exports state
    OLC_MISS = 0,   // lookup/resolve/trap miss; live VU canonicalized,
                    // caller must restart the job in the static engine
};

// No-JIT bring-up (data mappings, mVUinit state, table install, trap/TEXT
// wiring). Arms the codegen-abort guards first; any compile attempt aborts.
bool olc_init(std::string& err);
void olc_shutdown();

// Execute-mirror core. TPC arrives as an INDEX (bridge/seed set it, as the
// bench seed did). On MISS the live VU is canonicalized (fresh-init state)
// so the next offline job starts clean; the caller restarts this job static.
OlcDisposition olc_execute(uint32_t budget);

// Counters for the det-gate miss/restart report.
void olc_stats(uint64_t& served, uint64_t& fallback, uint64_t& miss, uint64_t& parks,
               uint64_t& jumpMisses, uint64_t& fallMisses);
