#pragma once
// TEL2: low-cost play telemetry (both knobs default off; output-only, the
// guest never observes any of it).
//
// (a) PS2X_COVERAGE_OUT=<dir>: guest code coverage. Every dispatch target
//     PC (dispatchGuestBranch fast + full paths, the EE scheduler's resume
//     lookup) sets one byte in a map over the dense generated function table
//     (one byte per 4-byte slot, ~0.8 MB) on first entry: a relaxed load and,
//     the first time only, a relaxed store; no locks. Knob off = one relaxed
//     pointer load + a predicted branch per dispatch. A background thread
//     writes the cumulative set as <dir>/cov-<session>.txt (temp file +
//     rename, so a kill never leaves a torn file) at 60 s, then every 300 s,
//     on Android pause (synchronously, while the game thread is gated), at
//     the CT1 PS2X_COVERAGE_TICK vsync, and at run-loop exit. The file also
//     carries the CT1 sets (missing functions, unknown syscalls, unhandled
//     RPCs) and every replaceFunction override PC with whether it ran.
//     Format: see formatCoverage(); merge tool in ssx3 local/research/TEL2.
//
// (b) PS2X_SESSION_LOG=<dir>: one append-only text file per session,
//     <dir>/session-<session>.log, each line flushed as written:
//     header (build, env hash, knobs line), race start/stop/resume/end with
//     the event index/name/location/archive and mode (race clock [B+0xc],
//     the TK44 detector, read-only at VBlank), hitches (perf-log windows
//     with a present gap >= 50 ms, with that window's per-stage max ms),
//     error lines (FATAL/JALR/refused via the Android logcat tap; missing
//     functions, unknown syscalls, unhandled RPCs from the CT1 hooks), app
//     pause/resume and a 5-minute heartbeat. Budgets cap hitch and error
//     lines so a bad session stays a few KB per hour.
//
// Every write failure is swallowed (one stderr note); the game never waits
// on telemetry I/O except the pause-time dump, which runs while gated.
//
// The pure parts below (race tracker, error filter, budgets, formatting)
// are covered by ps2xTest/src/ps2_session_telemetry_tests.cpp.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ps2x::tel
{

// ---- Hot path: coverage marks -----------------------------------------------
// Written once in init() before any guest thread starts; never freed.
inline std::atomic<std::atomic<uint8_t> *> g_covBytes{nullptr};
inline uint32_t g_covBase = 0u;
inline uint32_t g_covSlots = 0u;

inline void coverSlot(uint32_t slot) noexcept
{
    std::atomic<uint8_t> *m = g_covBytes.load(std::memory_order_relaxed);
    if (__builtin_expect(m == nullptr, 1))
        return;
    if (slot >= g_covSlots)
        return;
    if (m[slot].load(std::memory_order_relaxed) == 0u)
        m[slot].store(1u, std::memory_order_relaxed);
}

inline void coverPc(uint32_t pc) noexcept
{
    std::atomic<uint8_t> *m = g_covBytes.load(std::memory_order_relaxed);
    if (__builtin_expect(m == nullptr, 1))
        return;
    const uint32_t slot = (pc - g_covBase) >> 2;
    if (slot >= g_covSlots)
        return;
    if (m[slot].load(std::memory_order_relaxed) == 0u)
        m[slot].store(1u, std::memory_order_relaxed);
}

// ---- Runtime API (ps2_session_telemetry.cpp) ------------------------------------------
// Reads both knobs once; allocates the map and starts the dumper thread when
// on. Call before the game thread starts.
void init(uint32_t tableBase, uint32_t slotCount);
bool coverageOn();
bool sessionOn();
// EE thread, every VBlank (after the tick increment). Read-only on rdram.
void onVBlank(uint64_t tick, const uint8_t *rdram, size_t ramSize, bool fh1Events, bool fh1GuestActive);
// CT1 tick reached: synchronous coverage dump.
void onCoverageTick(uint64_t tick);
void onAppPause(uint64_t tick);
void onAppResume(uint64_t tick);
void onExit(uint64_t tick);
// Init-time override record (any thread; always recorded, a few PCs).
void noteOverride(uint32_t pc);
// CT1 hooks (cold).
void noteMissingFunction(uint32_t targetPc, uint32_t sourcePc);
void noteUnknownSyscall(uint32_t id);
void noteUnhandledRpc(uint32_t sid, uint32_t function);
// Android logcat tap (every stdout/stderr line; cheap no-op when off).
void noteLogLine(const char *line);
// Perf-log window (main thread, once per wall second; PS2X_PERF_LOG=1).
struct StageMax
{
    const char *name;
    double maxMs; // -1 when the stage did not fire
};
void notePerfWindow(uint64_t tick, double vsyncsPerS, double maxGapMs, const StageMax *stages, size_t count);

// ---- Pure helpers ----------------------------------------------------------------

inline uint64_t fnv1a64(const char *data, size_t n, uint64_t h = 1469598103934665603ull)
{
    for (size_t i = 0; i < n; ++i)
    {
        h ^= static_cast<uint8_t>(data[i]);
        h *= 1099511628211ull;
    }
    return h;
}

// Race clock tracker over the HUD race time [B+0xc] (TK44: bumped once per
// race update GO..finish; frozen in pause, results, menus). Edges:
//   Start  - an Idle clock moves forward
//   Stop   - a running clock unchanged for more than kStopTicks
//   Resume - a stopped clock moves forward again
//   End    - the clock goes backwards (restart/reset), the race object
//            changes, or it disappears (reason says which)
enum Edge : uint32_t
{
    kEdgeNone = 0u,
    kEdgeStart = 1u,
    kEdgeStop = 2u,
    kEdgeResume = 4u,
    kEdgeEnd = 8u,
};

struct RaceTracker
{
    static constexpr uint64_t kStopTicks = 30u;
    enum State
    {
        Idle,
        Running,
        Stopped,
    };
    State st = Idle;
    bool init = false;
    uint32_t b = 0u;
    uint32_t last = 0u;
    uint64_t lastChangeTick = 0u;
    uint64_t startTick = 0u;
    uint64_t stopTick = 0u; // last Stop edge's tick (the last clock change)
    uint32_t stopClock = 0u;
    const char *endReason = "";

    // b == 0 or !clockOk: no race object readable this tick.
    uint32_t step(uint64_t tick, uint32_t raceObj, bool clockOk, uint32_t clock)
    {
        uint32_t e = kEdgeNone;
        if (raceObj == 0u || !clockOk)
        {
            if (st != Idle)
            {
                e |= kEdgeEnd;
                endReason = "gone";
                st = Idle;
            }
            init = false;
            return e;
        }
        if (!init || raceObj != b)
        {
            if (st != Idle)
            {
                e |= kEdgeEnd;
                endReason = "object";
                st = Idle;
            }
            init = true;
            b = raceObj;
            last = clock;
            lastChangeTick = tick;
            return e;
        }
        if (clock != last)
        {
            if (clock < last)
            {
                if (st != Idle)
                {
                    e |= kEdgeEnd;
                    endReason = "reset";
                    st = Idle;
                }
            }
            else if (st == Idle)
            {
                e |= kEdgeStart;
                startTick = tick;
                st = Running;
            }
            else if (st == Stopped)
            {
                e |= kEdgeResume;
                st = Running;
            }
            last = clock;
            lastChangeTick = tick;
            return e;
        }
        if (st == Running && tick - lastChangeTick > kStopTicks)
        {
            e |= kEdgeStop;
            st = Stopped;
            stopTick = lastChangeTick;
            stopClock = last;
        }
        return e;
    }
};

// True when `word` occurs in `line` as a standalone word: the char before is
// not [A-Za-z0-9_] and the char after is not [A-Za-z0-9_=], so counter keys
// such as "vk_refused=0" or "refused=3" never match (ACH4).
inline bool containsWord(const char *line, const char *word)
{
    const size_t n = std::strlen(word);
    auto isWordChar = [](char c)
    { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
    for (const char *p = std::strstr(line, word); p; p = std::strstr(p + 1, word))
    {
        const bool startOk = p == line || !isWordChar(p[-1]);
        const char after = p[n];
        const bool endOk = after == '\0' || (!isWordChar(after) && after != '=');
        if (startOk && endOk)
            return true;
    }
    return false;
}

// Error lines worth a session-log entry (Android logcat tap). Our own [tel]
// notes never match. "refused" counts only as a word (our refusal lines:
// "[ssx3-tricky] refused:", "fh1-full120-refused ...", "[cd-overlay]
// REFUSED:"), never inside a key=value stat name (ACH4: "[present-vk]
// window-change stats ... vk_refused=0" was a false error).
inline bool isErrorLine(const char *line)
{
    if (!line || !line[0])
        return false;
    if (std::strncmp(line, "[tel]", 5) == 0)
        return false;
    static const char *const kPats[] = {"FATAL", "JALR", "terminate called", "Abort", "SIGSEGV"};
    for (const char *p : kPats)
        if (std::strstr(line, p))
            return true;
    static const char *const kWords[] = {"refused", "REFUSED", "Refused"};
    for (const char *w : kWords)
        if (containsWord(line, w))
            return true;
    return false;
}

// Per-key dedupe: the first kPerKey occurrences of a key print, then it is
// only counted; at most kTotal error lines per session.
struct ErrorBudget
{
    static constexpr uint32_t kPerKey = 3u;
    static constexpr uint32_t kTotal = 200u;
    std::vector<std::pair<uint64_t, uint32_t>> seen; // key hash -> count
    uint32_t printed = 0u;
    uint64_t suppressed = 0u;

    // Key: the first 40 chars with digit runs folded (so "tick=123" variants
    // share a key). Returns the occurrence count (1-based) and whether to print.
    bool admit(const char *line, uint32_t &countOut)
    {
        char key[40];
        size_t n = 0;
        for (size_t i = 0; line[i] && n < sizeof(key); ++i)
        {
            const bool digit = line[i] >= '0' && line[i] <= '9';
            if (digit && n > 0 && key[n - 1] == '#')
                continue; // a digit run folds to one '#'
            key[n++] = digit ? '#' : line[i];
        }
        const uint64_t h = fnv1a64(key, n);
        uint32_t *count = nullptr;
        for (auto &kv : seen)
            if (kv.first == h)
                count = &kv.second;
        if (!count)
        {
            if (seen.size() >= 1024u)
            {
                ++suppressed;
                countOut = 0u;
                return false;
            }
            seen.emplace_back(h, 0u);
            count = &seen.back().second;
        }
        countOut = ++*count;
        if (*count > kPerKey || printed >= kTotal)
        {
            ++suppressed;
            return false;
        }
        ++printed;
        return true;
    }
};

// Token bucket for hitch lines: kBurst lines up front, then one per
// kRefillS seconds (60/hour); dropped windows are counted and reported on
// the next printed line. Worst case ~70 lines x ~200 B = ~14 KB/hour.
struct HitchBudget
{
    static constexpr double kBurst = 10.0;
    static constexpr double kRefillS = 60.0;
    double tokens = kBurst;
    double lastS = -1.0;
    uint64_t dropped = 0u;

    bool admit(double nowS)
    {
        if (lastS >= 0.0 && nowS > lastS)
        {
            tokens += (nowS - lastS) / kRefillS;
            if (tokens > kBurst)
                tokens = kBurst;
        }
        lastS = nowS;
        if (tokens >= 1.0)
        {
            tokens -= 1.0;
            return true;
        }
        ++dropped;
        return false;
    }
};

constexpr double kHitchMs = 50.0;
constexpr double kHitchCellMs = 5.0; // stages below this max are left out of the line

// "stage=max ..." for the stages whose window max reached kHitchCellMs, plus
// the largest work stage (waits excluded: ee.wait/ee.pace/ee.event/ee.enq/
// ee.mtvu* are symptoms; ee.cpu duplicates ee.busy).
inline std::string formatHitchStages(const StageMax *stages, size_t count, std::string &topOut)
{
    std::string s;
    double topMs = -1.0;
    topOut = "na";
    for (size_t i = 0; i < count; ++i)
    {
        if (stages[i].maxMs < kHitchCellMs || !stages[i].name)
            continue;
        char cell[64];
        std::snprintf(cell, sizeof(cell), "%s%s=%.1f", s.empty() ? "" : " ", stages[i].name, stages[i].maxMs);
        s += cell;
        const char *n = stages[i].name;
        const bool wait = std::strcmp(n, "ee.wait") == 0 || std::strcmp(n, "ee.pace") == 0 ||
                          std::strcmp(n, "ee.event") == 0 || std::strcmp(n, "ee.enq") == 0 ||
                          std::strncmp(n, "ee.mtvu", 7) == 0 || std::strcmp(n, "ee.cpu") == 0;
        if (!wait && stages[i].maxMs > topMs)
        {
            topMs = stages[i].maxMs;
            topOut = n;
        }
    }
    return s;
}

// Coverage file body (v1). Lines:
//   # ps2x-coverage v1
//   session=<id> build=<id> env_hash=<16 hex>
//   table base=0x<hex> slots=<n>
//   dump reason=<r> tick=<n> wall=<UTC ISO> uptime_s=<n>
//   hits=<n>
//   h <hex pc>               one per hit table slot, ascending
//   ov <hex pc> ran=<0|1>     one per replaceFunction override PC
//   missing <hex pc> hits=<n>
//   syscall <hex id> hits=<n>
//   rpc <hex sid> <hex fn> hits=<n>
//   end
struct CoverageMeta
{
    std::string session, build, wall, reason;
    uint64_t envHash = 0u;
    uint64_t tick = 0u;
    uint64_t uptimeS = 0u;
};

struct CountedId
{
    uint64_t id;
    uint64_t hits;
};

inline std::string formatCoverage(const CoverageMeta &m, uint32_t base, const uint8_t *bytes, uint32_t slots,
                                  const std::vector<uint32_t> &overrides, const std::vector<CountedId> &missing,
                                  const std::vector<CountedId> &syscalls, const std::vector<CountedId> &rpcs)
{
    std::string out;
    out.reserve(64u * 1024u);
    char buf[160];
    out += "# ps2x-coverage v1\n";
    std::snprintf(buf, sizeof(buf), "session=%s build=%s env_hash=%016llx\n", m.session.c_str(), m.build.c_str(),
                  static_cast<unsigned long long>(m.envHash));
    out += buf;
    std::snprintf(buf, sizeof(buf), "table base=0x%x slots=%u\n", base, slots);
    out += buf;
    std::snprintf(buf, sizeof(buf), "dump reason=%s tick=%llu wall=%s uptime_s=%llu\n", m.reason.c_str(),
                  static_cast<unsigned long long>(m.tick), m.wall.c_str(),
                  static_cast<unsigned long long>(m.uptimeS));
    out += buf;
    uint32_t hits = 0u;
    for (uint32_t i = 0; i < slots; ++i)
        hits += bytes[i] ? 1u : 0u;
    std::snprintf(buf, sizeof(buf), "hits=%u\n", hits);
    out += buf;
    for (uint32_t i = 0; i < slots; ++i)
    {
        if (!bytes[i])
            continue;
        std::snprintf(buf, sizeof(buf), "h %x\n", base + i * 4u);
        out += buf;
    }
    for (uint32_t pc : overrides)
    {
        const uint32_t slot = (pc - base) >> 2;
        const bool ran = slot < slots && bytes[slot];
        std::snprintf(buf, sizeof(buf), "ov %x ran=%d\n", pc, ran ? 1 : 0);
        out += buf;
    }
    for (const CountedId &c : missing)
    {
        std::snprintf(buf, sizeof(buf), "missing %llx hits=%llu\n", static_cast<unsigned long long>(c.id),
                      static_cast<unsigned long long>(c.hits));
        out += buf;
    }
    for (const CountedId &c : syscalls)
    {
        std::snprintf(buf, sizeof(buf), "syscall %llx hits=%llu\n", static_cast<unsigned long long>(c.id),
                      static_cast<unsigned long long>(c.hits));
        out += buf;
    }
    for (const CountedId &c : rpcs)
    {
        std::snprintf(buf, sizeof(buf), "rpc %llx %llx hits=%llu\n", static_cast<unsigned long long>(c.id >> 32u),
                      static_cast<unsigned long long>(c.id & 0xffffffffull), static_cast<unsigned long long>(c.hits));
        out += buf;
    }
    out += "end\n";
    return out;
}

// Bounded printable copy of guest bytes (stops at NUL; non-printables -> '?').
inline std::string guestStr(const uint8_t *ram, size_t ramSize, uint32_t addr, size_t maxLen)
{
    std::string s;
    for (size_t i = 0; i < maxLen; ++i)
    {
        const size_t a = static_cast<size_t>(addr) + i;
        if (a >= ramSize)
            break;
        const char c = static_cast<char>(ram[a]);
        if (c == '\0')
            break;
        s += (c >= 0x20 && c < 0x7f && c != '"') ? c : '?';
    }
    return s;
}

} // namespace ps2x::tel
