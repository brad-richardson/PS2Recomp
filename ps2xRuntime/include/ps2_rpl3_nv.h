#pragma once

// RPL3: recorded host values for bit-exact session replay.
//
// A play session (PS2X_DETERMINISTIC unset) is a pure function of the build,
// the env, the card snapshot, the pad stream, and a small set of host values
// the guest reads: the CD-clock bytes (sceCdReadClock, the cBxPseudoRng seed
// source), card dates (sceMcGetDir), and the OSD timezone. The padrec v3
// recorder appends each served value as a `# NV ...` comment line (the pad
// script parser skips full-line '#' comments, so v3 replays as a script
// unchanged); replay serves the recorded values back in order.
//
// Record and replay are independent and keyed off different state, so a v3
// replay can itself record a new v3 (the served values are what gets noted):
//   record: active when PS2X_PAD_RECORD/PS2X_PAD_RECORD_DIR is set. note*
//     calls buffer until the padrec file arms (first pad read), then append
//     inline with a flush per line (SIGTERM-safe; no finalize step).
//   replay: loads lazily from the PS2X_PAD_SCRIPT @file at the first take*
//     call (boot RTC reads run before the first pad read, so the pad script's
//     own lazy load is too late). Files without `# NV ` lines stay inactive
//     and every take* returns false: v2 files replay exactly as before.
// Play (no pad script) never takes: the guest path is one extra call plus a
// relaxed check per host read (2 RTC reads + 1 TZ read + rare GetDirs).
//
// No new knob: both sides key off existing env (record path / script path).

#include <cstddef>
#include <cstdint>

namespace ps2_rpl3
{
    constexpr uint64_t kTickUnknown = ~0ull; // formatted as '*' (OSD init has no tick)

    // Record side: the value just served to the guest. tick/ra are
    // informational only; replay serves purely sequentially.
    void noteRtc(uint64_t tick, uint32_t ra, const uint8_t bytes[8]);
    // One GetDir call: `sets` holds entryCount entries in build order, 12
    // uint16 each (create sec,min,hour,day,mon,year then modify), `served`
    // how many the guest saw (maxEntries truncation). Serve order matches:
    // writeMcDateTime runs create,modify per entry in build order.
    void noteMc(uint64_t tick, const char *query, const uint16_t *sets, size_t entryCount,
                size_t served);
    void noteTz(uint64_t tick, int minutes);

    // Replay side: serve the next recorded value; false = none recorded (the
    // caller falls back to existing behavior). takeTz is sticky: the single
    // recorded offset serves every call.
    bool takeRtc(uint8_t out[8]);
    bool takeMcDate(uint16_t out[6]);
    bool takeTz(int &out);

    // Test seam: drop all record/replay state (the env-gated record switch
    // re-reads on the next note* call).
    void resetForTest();
} // namespace ps2_rpl3
