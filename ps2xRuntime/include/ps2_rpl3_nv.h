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

    // Scheduling gates: host-timed event batching splits cycle-due batches by
    // wall deadlines (a VBlank withheld while a SoundTick fires, with a guest
    // slice between), which is wall-dependent and guest-visible. Live notes
    // every split batch; gated replay fires the recorded subset instead of
    // host-gating. Ticks/cycles are the live call's; replay matches gates by
    // exact eeCycle (same guest => same slice boundaries).
    struct SchedEvent
    {
        uint32_t type = 0; // EeEventType as a number (mnemonic below)
        uint32_t id = 0;
        uint64_t cycle = 0; // deadlineCycle
    };

    // One mnemonic per EeEventType (ee_scheduler.h order: Stop=0,
    // VBlankStart=1, VBlankEnd=2, Dmac=3, ExternalWake=4, Alarm=5,
    // SoundTick=6). Single source; EeScheduler.cpp static_asserts the order.
    inline char schedTypeChar(uint32_t type)
    {
        switch (type)
        {
        case 0u:
            return 'X';
        case 1u:
            return 'S';
        case 2u:
            return 'E';
        case 3u:
            return 'D';
        case 4u:
            return 'W';
        case 5u:
            return 'A';
        case 6u:
            return 'T';
        default:
            return '?';
        }
    }

    inline bool schedCharType(char c, uint32_t &type)
    {
        switch (c)
        {
        case 'X':
            type = 0u;
            return true;
        case 'S':
            type = 1u;
            return true;
        case 'E':
            type = 2u;
            return true;
        case 'D':
            type = 3u;
            return true;
        case 'W':
            type = 4u;
            return true;
        case 'A':
            type = 5u;
            return true;
        case 'T':
            type = 6u;
            return true;
        default:
            return false;
        }
    }

    // Record one split batch: the fired subset of the cycle-due set at this
    // call (withheldCount = cycle-due but host-future, for diagnosis).
    void noteSched(uint64_t tick, uint64_t cycle, const SchedEvent *fired, size_t firedCount,
                   size_t withheldCount);
    // Replay: if the next gate matches this call's eeCycle, mark the fired
    // subset of the current cycle-due list and return true. False = no gate
    // (the caller runs its existing branch). Stale gates (cycle < current,
    // i.e. already diverged) are dropped with one warning.
    bool takeSched(uint64_t cycle, const SchedEvent *due, size_t dueCount, bool *fire);
    // Idle-path race gates: waitForEvent picks the timer-vs-event winner
    // by host order in play but cycle order in det; the two disagree
    // ~10k times per session (SSX3 idles constantly), jumping to different
    // cycles. The served side is noted when the cycle rule would not pick
    // it (agree=false); gated replay (det=1) forces the recorded winner.
    // The filter also keeps det recordings gateless and chains exact.
    struct IdleGate
    {
        uint64_t cycle = 0; // eeCycle at the waitForEvent call
        uint64_t timerCycle = 0;
        uint64_t eventCycle = 0;
        char winner = 0; // 'T' or 'E'
    };
    void noteIdle(uint64_t tick, uint64_t cycle, uint64_t timerCycle, uint64_t eventCycle,
                  char winner, bool agree);
    // Replay: if the next idle gate matches this call, set winner and return
    // true. False = decide by the existing rule.
    bool takeIdle(uint64_t cycle, uint64_t timerCycle, uint64_t eventCycle, char &winner);
} // namespace ps2_rpl3
