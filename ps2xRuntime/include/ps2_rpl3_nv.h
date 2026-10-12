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
    // call, plus the withheld identities (cycle-due but host-future).
    // RPL5: the line carries `h=` (withheld triples) after `f=`; replay
    // checks the withheld set, closing the RPL4 §7 gap (withheld counts
    // alone let a non-min pending divergence through: P5/P6 at 1720).
    void noteSched(uint64_t tick, uint64_t cycle, const SchedEvent *fired, size_t firedCount,
                   const SchedEvent *withheld, size_t withheldCount);
    // Replay: if the next gate matches this call's eeCycle, mark the fired
    // subset of the current cycle-due list and return true. False = no gate
    // (the caller runs its existing branch). Stale gates (cycle < current,
    // i.e. already diverged) are dropped with one warning.
    // RPL5: two pending-set checks run on every applied gate and warn once
    // each (the gate still applies; a warning means already diverged):
    // (1) the cycle-due count must equal fired + withheld (parsed `w`);
    // this catches a missing/extra pending on old files too (no `h=`);
    // (2) with `h=` present, the replay's non-fired due set must match the
    // recorded withheld identities exactly (multiset).
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

    // GS register timelines (RPL3-P2): CSR/SIGLBLID bits set by host-threaded
    // units (GIF SIGNAL/LABEL decode) are wall-timed; the guest vsync-polls
    // them and branches (F1/F3 bifurcated on one such branch at tick 1008,
    // eeCycle-identical until then, clocks never drifted). Record the full
    // u64 per guest read as a (tick, idx) timeline: first read per reg is
    // always noted (baseline), then value changes only, so spins cost
    // nothing. Replay serves the latest note at or before the read's
    // (tick, idx) (spins exit at the recorded idx, cycle-exact). idx counts
    // reads within a tick and resets on any tick change, symmetric on both
    // sides. No notes for a reg = never read = live (vacuous, v2-safe).
    constexpr uint32_t kGsRegCsr = 0u;
    constexpr uint32_t kGsRegSiglblid = 1u;
    constexpr uint32_t kGsRegCount = 2u;
    inline char gsRegChar(uint32_t reg)
    {
        return reg == kGsRegCsr ? 'C' : (reg == kGsRegSiglblid ? 'S' : '?');
    }
    struct GsRegNote
    {
        uint64_t tick = 0u;
        uint64_t idx = 0u;
        uint64_t value = 0u;
    };
    void noteGsReg(uint32_t reg, uint64_t tick, uint64_t value);
    // Serve the latest note at or before (tick, readIdx-implied). False =
    // no notes for this reg (live). Past the last note serves last-known
    // (sticky values; warn once). Tick behind served progress = diverged
    // (warn once, serve last-known).
    //
    // RPL4: deterministic-bit masking. CSR bits 15:14 (FIFO, HLE'd constant
    // EMPTY) and 13 (FIELD, tick parity from the scheduler's VBlank store)
    // never carry wall timing, so notes fire only on changes outside the
    // mask (volume: FIELD flips every tick) while the line keeps the full
    // value. `value` is in/out: live in, served out, where served is the
    // recorded value with the live masked bits (identical to the recorded
    // value when aligned; old full-timeline files replay bit-identically).
    // A serve that differs from live logs one line per reg (cap 32 total)
    // and counts toward gsregMismatchCount().
    constexpr uint64_t kGsRegMaskCsr = 0xE000ull;
    constexpr uint64_t kGsRegMaskSiglblid = 0ull;
    inline uint64_t gsRegMask(uint32_t reg)
    {
        return reg == kGsRegCsr ? kGsRegMaskCsr
                               : (reg == kGsRegSiglblid ? kGsRegMaskSiglblid : 0ull);
    }
    bool takeGsReg(uint32_t reg, uint64_t tick, uint64_t &value);
    uint64_t gsregMismatchCount();

    // RPL4: extended register timelines (xreg). Address-keyed value
    // timelines with the same baseline-plus-changes shape and (tick, idx)
    // service as gsreg (idx counts reads of that address within a tick,
    // symmetric both sides). All xreg addresses use mask 0 (no known
    // deterministic-bit subset). Files without `xreg` lines leave every
    // address live, so v3 files replay exactly as before. A serve that
    // differs from live logs the first mismatch per address (cap 256
    // lines total) and counts toward xregMismatchCount(): those lines
    // name which timeline actually mattered, and silence proves an
    // address replayed live-identical.
    // RPL5 record set: GS privileged registers only (worker-visible
    // state). EE-deterministic IO (GIF/VIF CHCR, timers, SPR, DMAC, ...)
    // is no longer noted (ps2_memory.cpp keeps takeXreg but drops
    // noteXreg on the IO paths): RPL4 §1b-c proved every IO value
    // EE-determined and six pairs replayed in-span silent. Replay still
    // serves any recorded address, so older full-xreg files replay
    // exactly.
    struct XRegNote
    {
        uint64_t tick = 0u;
        uint64_t idx = 0u;
        uint32_t addr = 0u;
        uint64_t value = 0u;
    };
    void noteXreg(uint32_t addr, uint64_t tick, uint64_t value);
    bool takeXreg(uint32_t addr, uint64_t tick, uint64_t &value);
    uint64_t xregMismatchCount();
    size_t xregNoteCount();
} // namespace ps2_rpl3
