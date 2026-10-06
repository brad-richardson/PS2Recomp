// TEL2: play telemetry runtime (see ps2_session_telemetry.h for formats and costs).
#include "ps2_session_telemetry.h"

#include "ps2_build_id.h"
#include "ps2_knobs.h"
#include "ps2_ssx3_course_manifest.h"
#include "ps2_ssx3_tricky_hud.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace ps2x::tel
{
namespace
{
// TK1 §3.1/3.2 (local/research/TK1/REPORT.md): current-course struct BSS
// 0x535BC8, +0x40 = current event index, +0x48/+0x49 mode bytes (+0x49
// indexes the game-type table: World Circuit / Quick Play / Multiplayer).
// Event records: 0x43D950 + idx*100 (+4 display name[32], +52 location
// code, +68 archive[16]); discipline table 0x442820 (23 x {event, disc}).
constexpr uint32_t kCurCourse = 0x535BC8u;
constexpr uint32_t kCurEventOff = 0x40u;
constexpr uint32_t kEventDiscTable = 0x442820u;
constexpr uint32_t kEventCount = 23u;

struct State
{
    std::mutex mu; // session file, budgets, CT1 maps, overrides
    bool initDone = false;
    bool cov = false;
    bool ses = false;
    std::string covDir, sesDir, sessionId, build, sesPath;
    uint64_t envHash = 0u;
    std::chrono::steady_clock::time_point t0;
    std::FILE *ses_f = nullptr;
    bool sesWriteFailed = false;
    bool covWriteFailed = false;
    std::atomic<uint8_t> *bytes = nullptr;
    uint32_t base = 0u, slots = 0u;
    std::vector<uint32_t> overrides;
    std::map<uint64_t, uint64_t> missing, syscalls, rpcs;
    ErrorBudget errors;
    HitchBudget hitches;
    // Race (EE thread owns the tracker; perf accumulators under mu).
    RaceTracker race;
    bool raceSaw120 = false;
    std::chrono::steady_clock::time_point raceWall;
    std::atomic<bool> raceRunning{false};
    uint64_t raceWindows = 0u, raceHitches = 0u;
    double raceVsSum = 0.0, raceVsMin = 1e9;
    uint64_t races = 0u;
    double resumeAtS = -1.0; // last app resume (its first windows span the pause)
    // Dumper thread.
    std::mutex dmu;
    std::condition_variable dcv;
    bool dumpRequested = false;
    std::atomic<uint64_t> lastTick{0};
    std::mutex dumpMu; // serializes coverage writes (thread vs pause/exit)
};

State &st()
{
    static State *s = new State(); // never destroyed (the dumper is detached)
    return *s;
}

bool readDirKnob(const char *name, std::string &out)
{
    const char *v = std::getenv(name);
    if (!v || !v[0] || (v[0] == '0' && v[1] == '\0'))
        return false;
    out = v;
    return true;
}

double nowS(const State &s)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - s.t0).count();
}

std::string utcIso()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string makeSessionId()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
#if !defined(_WIN32)
    const long pid = static_cast<long>(::getpid());
#else
    const long pid = 0;
#endif
    return std::string(buf) + "-" + std::to_string(pid);
}

// Caller holds s.mu. One flushed line; failures are swallowed (one note).
void sesLineLocked(State &s, const std::string &body)
{
    if (!s.ses || !s.ses_f)
        return;
    char head[64];
    std::snprintf(head, sizeof(head), "t=%.1f tick=%llu ", nowS(s),
                  static_cast<unsigned long long>(s.lastTick.load(std::memory_order_relaxed)));
    const std::string line = std::string(head) + body + "\n";
    if (std::fwrite(line.data(), 1, line.size(), s.ses_f) != line.size() || std::fflush(s.ses_f) != 0)
    {
        if (!s.sesWriteFailed)
        {
            s.sesWriteFailed = true;
            std::fprintf(stderr, "[tel] session log write failed (%s); further lines may be lost\n",
                         s.sesPath.c_str());
        }
    }
}

void sesLine(const std::string &body)
{
    State &s = st();
    if (!s.ses)
        return;
    std::lock_guard<std::mutex> lock(s.mu);
    sesLineLocked(s, body);
}

std::vector<CountedId> toCounted(const std::map<uint64_t, uint64_t> &m)
{
    std::vector<CountedId> v;
    v.reserve(m.size());
    for (const auto &kv : m)
        v.push_back({kv.first, kv.second});
    return v;
}

// Atomic coverage write: temp file + rename. Never throws.
void dumpCoverage(const char *reason)
{
    State &s = st();
    if (!s.cov || !s.bytes)
        return;
    std::lock_guard<std::mutex> dlock(s.dumpMu);
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> snap(s.slots);
    for (uint32_t i = 0; i < s.slots; ++i)
        snap[i] = s.bytes[i].load(std::memory_order_relaxed);
    CoverageMeta m;
    m.session = s.sessionId;
    m.build = s.build;
    m.envHash = s.envHash;
    m.reason = reason;
    m.tick = s.lastTick.load(std::memory_order_relaxed);
    m.wall = utcIso();
    m.uptimeS = static_cast<uint64_t>(nowS(s));
    std::vector<uint32_t> ov;
    std::vector<CountedId> missing, syscalls, rpcs;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        ov = s.overrides;
        missing = toCounted(s.missing);
        syscalls = toCounted(s.syscalls);
        rpcs = toCounted(s.rpcs);
    }
    std::sort(ov.begin(), ov.end());
    ov.erase(std::unique(ov.begin(), ov.end()), ov.end());
    const std::string body = formatCoverage(m, s.base, snap.data(), s.slots, ov, missing, syscalls, rpcs);
    const std::string finalPath = s.covDir + "/cov-" + s.sessionId + ".txt";
    const std::string tmpPath = s.covDir + "/.cov-" + s.sessionId + ".txt.tmp";
    bool ok = false;
    if (std::FILE *f = std::fopen(tmpPath.c_str(), "wb"))
    {
        ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
        ok = (std::fflush(f) == 0) && ok;
#if !defined(_WIN32)
        if (ok)
            ::fsync(::fileno(f));
#endif
        ok = (std::fclose(f) == 0) && ok;
        if (ok)
            ok = std::rename(tmpPath.c_str(), finalPath.c_str()) == 0;
        if (!ok)
            std::remove(tmpPath.c_str());
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    uint32_t hits = 0u;
    for (uint8_t b : snap)
        hits += b ? 1u : 0u;
    if (!ok && !s.covWriteFailed)
    {
        s.covWriteFailed = true;
        std::fprintf(stderr, "[tel] coverage write failed (%s)\n", finalPath.c_str());
    }
    char line[160];
    std::snprintf(line, sizeof(line), "coverage reason=%s hits=%u bytes=%zu ms=%.1f ok=%d", reason, hits,
                  body.size(), ms, ok ? 1 : 0);
    sesLine(line);
}

void heartbeat()
{
    State &s = st();
    if (!s.ses)
        return;
    sesLine(std::string("alive wall=") + utcIso());
}

void dumperLoop()
{
    State &s = st();
    auto next = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;)
    {
        bool requested = false;
        {
            std::unique_lock<std::mutex> lock(s.dmu);
            s.dcv.wait_until(lock, next, [&] { return s.dumpRequested; });
            requested = s.dumpRequested;
            s.dumpRequested = false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!requested && now < next)
            continue;
        if (now >= next)
            next = now + std::chrono::seconds(300);
        dumpCoverage(requested ? "request" : "periodic");
        if (!requested)
            heartbeat();
    }
}

bool ensureDir(const std::string &dir)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return std::filesystem::is_directory(dir, ec);
}

uint32_t rd32(const uint8_t *ram, size_t ramSize, uint32_t addr, bool &ok)
{
    const uint32_t a = addr & ps2_ssx3_tricky_hud::kRamMask;
    if (static_cast<size_t>(a) + 4u > ramSize)
    {
        ok = false;
        return 0u;
    }
    uint32_t v = 0u;
    std::memcpy(&v, ram + a, 4);
    ok = true;
    return v;
}

void logRaceStart(State &s, uint64_t tick, const uint8_t *ram, size_t ramSize, bool fh1Events, uint32_t clock)
{
    bool ok = false;
    const uint32_t ev = rd32(ram, ramSize, kCurCourse + kCurEventOff, ok);
    std::string name = "?", code = "?", archive = "?";
    uint32_t disc = 0xffu;
    if (ok && ev < kEventCount)
    {
        const uint32_t rec = ps2_ssx3_course::kEventBase + ev * ps2_ssx3_course::kEventStride;
        name = guestStr(ram, ramSize, rec + 4u, 32u);
        code = guestStr(ram, ramSize, rec + 52u, 8u);
        archive = guestStr(ram, ramSize, rec + 68u, 16u);
        for (uint32_t i = 0; i < kEventCount; ++i)
        {
            bool ok2 = false;
            const uint32_t e = rd32(ram, ramSize, kEventDiscTable + i * 8u, ok2);
            if (ok2 && e == ev)
            {
                disc = rd32(ram, ramSize, kEventDiscTable + i * 8u + 4u, ok2);
                break;
            }
        }
    }
    const uint8_t mode48 = ram[(kCurCourse + 0x48u) & ps2_ssx3_tricky_hud::kRamMask];
    const uint8_t game49 = ram[(kCurCourse + 0x49u) & ps2_ssx3_tricky_hud::kRamMask];
    std::string tricky = "stock";
    {
        const ps2_ssx3_course::Modes &ms = ps2_ssx3_course::courseModes();
        if (ms.armed)
        {
            const size_t cur = ps2_ssx3_course::modeCurrent(ms, ram);
            if (cur != 0u && cur <= ms.modes.size())
                tricky = ms.modes[cur - 1u].name;
        }
    }
    const char *sim = std::getenv("PS2X_SSX3_SIM_MODE");
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "race-start n=%llu event=%d name=\"%s\" loc=\"%s\" archive=\"%s\" disc=%d mode_b=%u game_b=%u "
                  "sim=%s full120=%s course_mode=\"%s\" race_time=%u wall=%s",
                  static_cast<unsigned long long>(s.races), ok ? static_cast<int>(ev) : -1, name.c_str(),
                  code.c_str(), archive.c_str(), static_cast<int>(disc == 0xffu ? -1 : static_cast<int>(disc)),
                  static_cast<unsigned>(mode48), static_cast<unsigned>(game49), sim && sim[0] ? sim : "unset",
                  fh1Events ? "events" : "off", tricky.c_str(), clock, utcIso().c_str());
    (void)tick;
    sesLineLocked(s, buf);
}

} // namespace

bool coverageOn()
{
    return st().cov;
}

bool sessionOn()
{
    return st().ses;
}

void init(uint32_t tableBase, uint32_t slotCount)
{
    State &s = st();
    {
        std::lock_guard<std::mutex> lock(s.mu);
        if (s.initDone)
            return;
        s.initDone = true;
    }
    s.t0 = std::chrono::steady_clock::now();
    const bool wantCov = readDirKnob("PS2X_COVERAGE_OUT", s.covDir);
    const bool wantSes = readDirKnob("PS2X_SESSION_LOG", s.sesDir);
    if (!wantCov && !wantSes)
        return;
    s.sessionId = makeSessionId();
    s.build = ps2x::buildId();
    const std::string knobs = ps2x::cf2BuildKnobsLine();
    s.envHash = fnv1a64(knobs.data(), knobs.size());
    if (wantCov && slotCount != 0u)
    {
        if (ensureDir(s.covDir))
        {
            s.bytes = new (std::nothrow) std::atomic<uint8_t>[slotCount];
            if (s.bytes)
            {
                for (uint32_t i = 0; i < slotCount; ++i)
                    s.bytes[i].store(0u, std::memory_order_relaxed);
                s.base = tableBase;
                s.slots = slotCount;
                g_covBase = tableBase;
                g_covSlots = slotCount;
                g_covBytes.store(s.bytes, std::memory_order_release);
                s.cov = true;
            }
        }
        else
        {
            std::fprintf(stderr, "[tel] coverage dir unusable: %s (coverage off)\n", s.covDir.c_str());
        }
    }
    if (wantSes)
    {
        if (ensureDir(s.sesDir))
        {
            s.sesPath = s.sesDir + "/session-" + s.sessionId + ".log";
            s.ses_f = std::fopen(s.sesPath.c_str(), "ab");
            s.ses = s.ses_f != nullptr;
        }
        if (!s.ses)
            std::fprintf(stderr, "[tel] session log unusable: %s (session log off)\n", s.sesDir.c_str());
    }
    if (s.ses)
    {
        std::lock_guard<std::mutex> lock(s.mu);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "# ps2x-session v1 session=%s build=%s env_hash=%016llx start=%s cov=%d",
                      s.sessionId.c_str(), s.build.c_str(), static_cast<unsigned long long>(s.envHash),
                      utcIso().c_str(), s.cov ? 1 : 0);
        sesLineLocked(s, buf);
        sesLineLocked(s, knobs);
    }
    std::fprintf(stderr, "[tel] session=%s coverage=%s session_log=%s\n", s.sessionId.c_str(),
                 s.cov ? s.covDir.c_str() : "off", s.ses ? s.sesPath.c_str() : "off");
    try
    {
        std::thread(dumperLoop).detach();
    }
    catch (...)
    {
        std::fprintf(stderr, "[tel] dumper thread failed to start (periodic dumps off)\n");
    }
}

void onVBlank(uint64_t tick, const uint8_t *rdram, size_t ramSize, bool fh1Events, bool fh1GuestActive)
{
    State &s = st();
    if (!s.ses)
        return;
    s.lastTick.store(tick, std::memory_order_relaxed);
    if (!rdram || ramSize == 0u)
        return;
    const uint32_t b = ps2_ssx3_tricky_hud::resolveChainB(rdram, ramSize);
    bool clockOk = false;
    uint32_t clock = 0u;
    if (b != 0u)
        clock = rd32(rdram, ramSize, b + ps2_ssx3_tricky_hud::kRaceClockOff, clockOk);
    RaceTracker &r = s.race;
    const RaceTracker::State before = r.st;
    const uint32_t e = r.step(tick, b, clockOk, clock);
    if (r.st == RaceTracker::Running && fh1GuestActive)
        s.raceSaw120 = true;
    // Perf windows count toward the race only while its clock runs.
    s.raceRunning.store(r.st == RaceTracker::Running, std::memory_order_relaxed);
    if (e == kEdgeNone)
        return;
    std::lock_guard<std::mutex> lock(s.mu);
    char buf[320];
    if (e & kEdgeEnd)
    {
        const double durS =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - s.raceWall).count();
        std::snprintf(buf, sizeof(buf),
                      "race-end n=%llu reason=%s ticks=%llu wall_s=%.1f last_stop_tick=%llu race_time_at_stop=%u "
                      "was=%s guest120_seen=%d perf_windows=%llu vs_mean=%.2f vs_min=%.2f hitches=%llu",
                      static_cast<unsigned long long>(s.races), r.endReason,
                      static_cast<unsigned long long>(tick - r.startTick), durS,
                      static_cast<unsigned long long>(r.stopTick), r.stopClock,
                      before == RaceTracker::Stopped ? "stopped" : "running", s.raceSaw120 ? 1 : 0,
                      static_cast<unsigned long long>(s.raceWindows),
                      s.raceWindows ? s.raceVsSum / static_cast<double>(s.raceWindows) : 0.0,
                      s.raceWindows ? s.raceVsMin : 0.0, static_cast<unsigned long long>(s.raceHitches));
        sesLineLocked(s, buf);
    }
    if (e & kEdgeStart)
    {
        ++s.races;
        s.raceSaw120 = fh1GuestActive;
        s.raceWall = std::chrono::steady_clock::now();
        s.raceWindows = s.raceHitches = 0u;
        s.raceVsSum = 0.0;
        s.raceVsMin = 1e9;
        logRaceStart(s, tick, rdram, ramSize, fh1Events, clock);
    }
    if (e & kEdgeStop)
    {
        std::snprintf(buf, sizeof(buf), "race-stop n=%llu race_time=%u clock_tick=%llu",
                      static_cast<unsigned long long>(s.races), r.stopClock,
                      static_cast<unsigned long long>(r.stopTick));
        sesLineLocked(s, buf);
    }
    if (e & kEdgeResume)
    {
        std::snprintf(buf, sizeof(buf), "race-resume n=%llu race_time=%u", static_cast<unsigned long long>(s.races),
                      clock);
        sesLineLocked(s, buf);
    }
}

void onCoverageTick(uint64_t tick)
{
    State &s = st();
    s.lastTick.store(tick, std::memory_order_relaxed);
    dumpCoverage("tick");
}

void onAppPause(uint64_t tick)
{
    State &s = st();
    if (!s.cov && !s.ses)
        return;
    s.lastTick.store(tick, std::memory_order_relaxed);
    sesLine("app-pause");
    dumpCoverage("pause");
}

void onAppResume(uint64_t tick)
{
    State &s = st();
    if (!s.ses)
        return;
    s.lastTick.store(tick, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(s.mu);
        s.resumeAtS = nowS(s);
    }
    sesLine("app-resume");
}

void onExit(uint64_t tick)
{
    State &s = st();
    if (!s.cov && !s.ses)
        return;
    s.lastTick.store(tick, std::memory_order_relaxed);
    dumpCoverage("exit");
    sesLine("exit");
}

void noteOverride(uint32_t pc)
{
    State &s = st();
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.overrides.size() < 4096u)
        s.overrides.push_back(pc);
}

namespace
{
// kind: "error" (logcat FATAL/JALR/refused lines, missing functions) or
// "ct1" (unknown syscalls / unhandled RPCs: often benign boot probes).
void noteError(State &s, const std::string &line, const char *kind = "error")
{
    uint32_t count = 0u;
    if (!s.errors.admit(line.c_str(), count))
        return;
    std::string body = std::string(kind) + " n=" + std::to_string(count) + " \"";
    for (size_t i = 0; i < line.size() && i < 240u; ++i)
        body += (line[i] == '"' || line[i] == '\n') ? '\'' : line[i];
    body += "\"";
    if (s.errors.suppressed)
        body += " suppressed_total=" + std::to_string(s.errors.suppressed);
    sesLineLocked(s, body);
}
} // namespace

void noteMissingFunction(uint32_t targetPc, uint32_t sourcePc)
{
    State &s = st();
    if (!s.cov && !s.ses)
        return;
    std::lock_guard<std::mutex> lock(s.mu);
    ++s.missing[targetPc];
    if (s.ses)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "missing-function target=0x%x source=0x%x", targetPc, sourcePc);
        noteError(s, buf);
    }
}

void noteUnknownSyscall(uint32_t id)
{
    State &s = st();
    if (!s.cov && !s.ses)
        return;
    std::lock_guard<std::mutex> lock(s.mu);
    if (++s.syscalls[id] == 1u && s.ses)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "unknown-syscall id=0x%x", id);
        noteError(s, buf, "ct1");
    }
}

void noteUnhandledRpc(uint32_t sid, uint32_t function)
{
    State &s = st();
    if (!s.cov && !s.ses)
        return;
    std::lock_guard<std::mutex> lock(s.mu);
    if (++s.rpcs[(static_cast<uint64_t>(sid) << 32u) | function] == 1u && s.ses)
    {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "unhandled-rpc sid=0x%x fn=0x%x", sid, function);
        noteError(s, buf, "ct1");
    }
}

void noteLogLine(const char *line)
{
    State &s = st();
    if (!s.ses || !isErrorLine(line))
        return;
    std::lock_guard<std::mutex> lock(s.mu);
    noteError(s, line);
}

void notePerfWindow(uint64_t tick, double vsyncsPerS, double maxGapMs, const StageMax *stages, size_t count)
{
    State &s = st();
    if (!s.ses)
        return;
    (void)tick;
    std::lock_guard<std::mutex> lock(s.mu);
    const bool racing = s.raceRunning.load(std::memory_order_relaxed);
    if (racing)
    {
        ++s.raceWindows;
        s.raceVsSum += vsyncsPerS;
        if (vsyncsPerS < s.raceVsMin)
            s.raceVsMin = vsyncsPerS;
    }
    if (maxGapMs < kHitchMs)
        return;
    if (s.resumeAtS >= 0.0 && nowS(s) - s.resumeAtS < 2.5)
        return; // the present gap spans the app pause, not a hitch
    if (racing)
        ++s.raceHitches;
    const uint64_t droppedBefore = s.hitches.dropped;
    if (!s.hitches.admit(nowS(s)))
        return;
    std::string top;
    const std::string cells = formatHitchStages(stages, count, top);
    char head[160];
    std::snprintf(head, sizeof(head), "hitch gap_ms=%.1f vs=%.2f race=%d top=%s", maxGapMs, vsyncsPerS,
                  racing ? 1 : 0, top.c_str());
    std::string body = std::string(head) + " stages=\"" + cells + "\"";
    if (droppedBefore)
        body += " dropped_total=" + std::to_string(droppedBefore);
    sesLineLocked(s, body);
}

} // namespace ps2x::tel
