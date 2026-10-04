#pragma once
// ACH2: local achievements (rcheevos engine, first-unlock toast, local
// persistence). No RA server contact: rc_client is not vendored.
//
// - Engine: vendored rcheevos rc_runtime (third_party/rcheevos), driven at
//   the stock tick (ACH1 §4c): every VBlank at divisor 1, every other VBlank
//   in full-120 (frameForTick/StockGate below; FH1 counters).
// - Off on Tricky courses: PS2X_CD_OVERLAY, PS2X_SSX3_COURSE_PICKER=1 or
//   PS2X_SSX3_TRICKY_MENU=1 (Brad 10-03; ACH1 §4d).
// - Set: local RA patch-JSON file next to the memory card
//   (achievements-set.json), PS2X_ACH_SET override. No set data ships in
//   git or the app. Only Flags 3, IDs < 101000000 (the server-injected
//   'Warning: Unknown Emulator' pseudo-achievement is Flags 3 and must be
//   skipped by ID). Leaderboards are parsed and ignored (out of scope).
// - State: achievements-state.json beside the set (unlocked ids + first
//   unix timestamps). Unlocks are permanent; re-triggers never toast.
//   Memory-card files are never touched.
// - Toast on first unlock: the shared ps2x::ui::toast (title + description),
//   drawn in the overlay pass (Android also mirrors to the Java Toast).
//   The [ach] unlocked log line stays as the record.
// - Knob: PS2X_ACHIEVEMENTS=1, default off. Knob off = one getenv per
//   VBlank, no guest effect. The engine only READS guest memory, so the
//   det hash is identical with it running.
//
// Platform-neutral; no rcheevos includes here (they stay in ps2_ach.cpp and
// the test TU).

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace ps2_ach
{

// PS2X_ACHIEVEMENTS=1 (read live so tests can flip it; one getenv/VBlank).
inline bool knobOn()
{
    const char *v = std::getenv("PS2X_ACHIEVEMENTS");
    return v && v[0] == '1' && v[1] == '\0';
}

// True when a Tricky course may be live (ACH1 §4d): a CD overlay dir is set
// or the course picker / Tricky menu knob is on. Implemented in ps2_ach.cpp
// (it includes the knob owners' headers); the env convention is
// PS2X_CD_OVERLAY=<nonempty>, PS2X_SSX3_COURSE_PICKER=1,
// PS2X_SSX3_TRICKY_MENU=1.
bool trickyActive();

// ---- Stock-tick gate (pure; ACH1 §4c) ------------------------------------
// The stock-frame index for a VBlank: divisor 1 advances every VBlank,
// divisor 2 every other one. In full120=events mode the FH1 stock-time
// accumulator (elapsed stock half-periods) is the index source, so the
// enter/exit flips need no anchor bookkeeping; otherwise the VBlank tick
// (halved at divisor 2) is.
inline uint64_t frameForTick(uint64_t tick, uint32_t divisor, uint64_t stockHalfExact, bool useStockHalf)
{
    if (useStockHalf)
        return stockHalfExact / 2u;
    if (divisor == 2u)
        return (tick + 1u) / 2u; // stock frames start at 1 in every mode
    return tick;
}

// Remembers the last evaluated stock frame; due() is true once per frame.
class StockGate
{
  public:
    StockGate() : lastFrame_(0), haveFrame_(false), evals_(0) {}
    bool due(uint64_t frame)
    {
        if (haveFrame_ && frame == lastFrame_)
            return false;
        haveFrame_ = true;
        lastFrame_ = frame;
        ++evals_;
        return true;
    }
    uint64_t evals() const { return evals_; }
    void reset()
    {
        lastFrame_ = 0;
        haveFrame_ = false;
        evals_ = 0;
    }

  private:
    uint64_t lastFrame_;
    bool haveFrame_;
    uint64_t evals_;
};

// ---- Guest memory view (pure) --------------------------------------------
// The rcheevos PS2 map (consoleinfo.c): RDRAM 0x00000000-0x01FFFFFF,
// scratchpad at 0x02000000-0x02003FFF. No IOP region for PS2.
inline constexpr uint32_t kRdramSize = 32u * 1024u * 1024u;
inline constexpr uint32_t kScratchBase = 0x02000000u;
inline constexpr uint32_t kScratchSize = 16u * 1024u;

struct MemView
{
    const uint8_t *rdram = nullptr;   // 32 MiB, may be null (reads yield 0)
    const uint8_t *scratch = nullptr; // 16 KiB, may be null (reads yield 0)
};

// Copy up to numBytes at the PS2-map address into buffer; returns bytes
// copied (0 past the mapped regions, clamped at region ends).
uint32_t readGuestMemory(uint32_t address, uint8_t *buffer, uint32_t numBytes, const MemView &view);

// ---- Set file -------------------------------------------------------------
struct Entry
{
    uint32_t id = 0;
    std::string title;
    std::string desc;
    std::string memaddr;
    uint32_t points = 0;
};

// Parse RA patch JSON (Achievements as a list — the fetched set shape — or
// a dict keyed by id; keys ID/Title/Description/MemAddr/Points/Flags, case
// tolerant). Keeps Flags 3 with id < 101000000 and a non-empty MemAddr;
// counts the rest in skipped. Leaderboards are ignored. False on malformed
// JSON (err explains).
bool parseSetJson(const std::string &text, std::vector<Entry> &out, uint32_t &skipped, std::string &err);

// ---- State file -----------------------------------------------------------
// id -> first-unlock unix time. Rendered {"unlocked":{"<id>":<ts>,...}}.
using UnlockMap = std::map<uint32_t, uint64_t>;
std::string renderStateJson(const UnlockMap &m);
bool parseStateJson(const std::string &text, UnlockMap &out);
bool loadStateFile(const std::string &path, UnlockMap &out); // missing file = empty, true
bool saveStateFile(const std::string &path, const UnlockMap &m);

// ---- Paths ----------------------------------------------------------------
inline constexpr const char *kSetFileName = "achievements-set.json";
inline constexpr const char *kStateFileName = "achievements-state.json";

// Directory holding the memory card (parent of mcRoot, or elfDir when mcRoot
// is empty meaning <elfDir>/mc0), + kSetFileName.
std::string defaultSetPath(const std::string &mcRoot, const std::string &elfDir);
// PS2X_ACH_SET wins when non-empty, else defaultSetPath().
std::string setPathFromEnv(const std::string &mcRoot, const std::string &elfDir);
// kStateFileName beside the set file.
std::string statePathForSet(const std::string &setPath);

// ---- Runtime (EE thread; implemented in ps2_ach.cpp) ----------------------
// Evaluate due stock frames against this VBlank's guest memory; unlock,
// toast and persist. Read-only wrt the guest.
void onVBlankTick(uint64_t tick, const uint8_t *rdram, const uint8_t *scratch);

// Gate counters (monotonic within the process; resetForTest clears).
uint64_t evalsTotal();
uint64_t vblanksSeen();
uint64_t unlocksTotal();
uint32_t activeCount();
bool trickyLatched(); // true once a Tricky-off VBlank was observed
void resetForTest();

} // namespace ps2_ach
