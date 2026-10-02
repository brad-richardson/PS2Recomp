#pragma once

// T1 park snapshot (PS2X_DIAG_PARK=1). Header-only, ps2_log.h-style: zero
// CMake changes, safe under the runner unity build (everything is
// ps2_park:: inline; no anonymous-namespace names).
//
// What it does: every future diagnosis brief's first hour, automated.
// Always-on counters (P1c-style: one increment per event, no printing)
// record sema wait/signal pcs, guest-dispatch hot pcs, SIF/RPC calls,
// GS packet/kick counts, per-thread schedule counts, and the [drop]
// census (the census map itself lives in ps2_log.h next to emitDrop).
// On SIGTERM (or PS2X_DIAG_PARK_TIMEOUT_MS after start) the scheduler
// loop writes ONE JSON + ONE human table and stops the boot.
//
// Threading: all tally bumps run on the EE executor thread (same as the
// P1c g_diagCallCounts precedent: no locks). The snapshot fill runs in
// EeScheduler::run(), also on the executor. GS counters are relaxed
// atomics to match gs_frontend.cpp convention.
//
// Schema: ps2x-park-snapshot/1 (see renderParkJson). tools/ladder_diff.py
// consumes it; local/research/T1/mine_snapshot.py mines the same schema
// from raw boot logs (source=miner, blind fields omitted).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ps2_park
{
inline constexpr const char *kSchema = "ps2x-park-snapshot/1";
inline constexpr const char *kJsonName = "park-snapshot.json";
inline constexpr const char *kTableName = "park-snapshot.txt";
inline constexpr size_t kMaxRpcEvents = 4096u;

// PS2X_DIAG_PARK: unset/empty = compiled in, nothing written (callers pay
// only the cached check at the snapshot poll point; counting is
// always-on so a snapshot is complete whenever the flag is on).
inline bool parkEnabled()
{
    static const bool enabled = [] {
        const char *env = std::getenv("PS2X_DIAG_PARK");
        return env != nullptr && env[0] != '\0';
    }();
    return enabled;
}

// Optional in-runtime timeout: write the snapshot once this many ms after
// install without stopping (the harness timeout arrives as SIGTERM and
// DOES stop). Unset/empty/0 = SIGTERM only.
inline uint64_t parkTimeoutMs()
{
    static const uint64_t timeout = [] {
        if (const char *env = std::getenv("PS2X_DIAG_PARK_TIMEOUT_MS"))
        {
            if (env[0] != '\0')
            {
                char *end = nullptr;
                const unsigned long long parsed = std::strtoull(env, &end, 10);
                if (end != env)
                {
                    return static_cast<uint64_t>(parsed);
                }
            }
        }
        return static_cast<uint64_t>(0);
    }();
    return timeout;
}

inline std::string parkDir()
{
    if (const char *env = std::getenv("PS2X_DIAG_PARK_DIR"))
    {
        if (env[0] != '\0')
        {
            return std::string(env);
        }
    }
    return std::string(".");
}

// ---------------------------------------------------------------- tallies

struct ParkHotPcEntry
{
    uint64_t count = 0;
    uint32_t firstRa = 0;
    uint32_t lastRa = 0;
};

inline std::unordered_map<uint32_t, ParkHotPcEntry> &hotPcCounts()
{
    static std::unordered_map<uint32_t, ParkHotPcEntry> counts;
    return counts;
}

inline void tallyDispatch(uint32_t targetPc, uint32_t ra)
{
    ParkHotPcEntry &entry = hotPcCounts()[targetPc];
    if (entry.count == 0u)
    {
        entry.firstRa = ra;
    }
    entry.lastRa = ra;
    ++entry.count;
}

struct ParkSemaOp
{
    int id = 0;
    int tid = 0;
    uint32_t pc = 0;
    uint32_t ra = 0;
};

inline std::map<int, std::map<uint32_t, uint64_t>> &semaWaitHist()
{
    static std::map<int, std::map<uint32_t, uint64_t>> hist;
    return hist;
}

inline std::map<int, std::map<uint32_t, uint64_t>> &semaSignalHist()
{
    static std::map<int, std::map<uint32_t, uint64_t>> hist;
    return hist;
}

// Fires once per wait/signal call at function entry, so the histogram
// covers exactly the same calls as the [diag:sema] lines (all three
// exits of each function print).
inline void tallySemaWait(int id, int tid, uint32_t pc, uint32_t ra)
{
    (void)tid;
    (void)ra;
    ++semaWaitHist()[id][pc];
}

inline void tallySemaSignal(int id, int tid, uint32_t pc, uint32_t ra)
{
    (void)tid;
    (void)ra;
    ++semaSignalHist()[id][pc];
}

struct ParkSemaCreate
{
    int id = 0;
    int tid = 0;
    uint32_t pc = 0;
    int initCount = 0;
    int maxCount = 0;
};

inline std::vector<ParkSemaCreate> &semaCreates()
{
    static std::vector<ParkSemaCreate> creates;
    return creates;
}

inline void tallySemaCreate(int id, int tid, uint32_t pc, int initCount, int maxCount)
{
    semaCreates().push_back(ParkSemaCreate{id, tid, pc, initCount, maxCount});
}

// SIF/RPC per-event rows. Field map per op (unused fields are 0/""):
//   load:    path + sid=module id (claimed = tracked ok)
//   bind:    sid + fno=mode (claimed = a server was already registered;
//            unclaimed = dummy server allocated so the bind loop proceeds)
//   call:    sid + fno=rpc number + send_size/recv_size (claimed = the HLE
//            handled it or a server function was dispatched)
//   sendcmd: sid=cid + send_size=packet size + recv_size=extra size
//            (claimed = the host acted on it: the SSX3 sregs handshake)
struct ParkRpcEvent
{
    std::string op;
    uint32_t sid = 0;
    uint32_t fno = 0;
    uint32_t sendSize = 0;
    uint32_t recvSize = 0;
    int tid = 0;
    bool claimed = false;
    std::string path;
};

inline std::vector<ParkRpcEvent> &rpcEvents()
{
    static std::vector<ParkRpcEvent> events;
    return events;
}

inline uint64_t &rpcEventsOverflow()
{
    static uint64_t overflow = 0;
    return overflow;
}

inline void tallyRpcEvent(ParkRpcEvent event)
{
    if (rpcEvents().size() >= kMaxRpcEvents)
    {
        ++rpcEventsOverflow();
        return;
    }
    rpcEvents().push_back(std::move(event));
}

inline std::atomic<uint64_t> &gsKickCount()
{
    static std::atomic<uint64_t> count{0};
    return count;
}

inline std::atomic<uint64_t> &gsKickDrawingCount()
{
    static std::atomic<uint64_t> count{0};
    return count;
}

inline std::atomic<uint64_t> &gsGifPacketCount()
{
    static std::atomic<uint64_t> count{0};
    return count;
}

inline std::atomic<uint64_t> &gsCopyRegCount()
{
    static std::atomic<uint64_t> count{0};
    return count;
}

// True counts (the [gs:kick]/[gs:gif]/[gs:copy-reg] log lines cap at
// 96/64/48, so the miner saturates where the emitter stays exact).
inline void tallyGsKick(bool drawing)
{
    gsKickCount().fetch_add(1u, std::memory_order_relaxed);
    if (drawing)
    {
        gsKickDrawingCount().fetch_add(1u, std::memory_order_relaxed);
    }
}

inline void tallyGsGif()
{
    gsGifPacketCount().fetch_add(1u, std::memory_order_relaxed);
}

inline void tallyGsCopyReg()
{
    gsCopyRegCount().fetch_add(1u, std::memory_order_relaxed);
}

inline std::map<int, uint64_t> &schedCounts()
{
    static std::map<int, uint64_t> counts;
    return counts;
}

// Fires at the same two sites as the per-block g_diagSchedCounts bumps,
// but unconditionally: the miner's per-block `scheduled=` sums must equal
// these exactly on the same boot.
inline void tallySched(int tid)
{
    ++schedCounts()[tid];
}

inline void resetTalliesForTesting()
{
    hotPcCounts().clear();
    semaWaitHist().clear();
    semaSignalHist().clear();
    semaCreates().clear();
    rpcEvents().clear();
    rpcEventsOverflow() = 0;
    gsKickCount().store(0u, std::memory_order_relaxed);
    gsKickDrawingCount().store(0u, std::memory_order_relaxed);
    gsGifPacketCount().store(0u, std::memory_order_relaxed);
    gsCopyRegCount().store(0u, std::memory_order_relaxed);
    schedCounts().clear();
}

// ------------------------------------------------------------ SIGTERM path

inline volatile std::sig_atomic_t &termFlag()
{
    static volatile std::sig_atomic_t flag = 0;
    return flag;
}

inline void requestSnapshotForTesting()
{
    termFlag() = 1;
}

#if defined(__unix__) || defined(__APPLE__)
inline void parkTermHandler(int)
{
    termFlag() = 1;
}
#endif

inline uint64_t parkSteadyMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

inline uint64_t &parkStartMs()
{
    static uint64_t start = 0;
    return start;
}

inline void installParkTermHandler()
{
    if (!parkEnabled())
    {
        return;
    }
    static bool installed = false;
    if (installed)
    {
        return;
    }
    installed = true;
    parkStartMs() = parkSteadyMs();
#if defined(__unix__) || defined(__APPLE__)
    struct sigaction action{};
    action.sa_handler = parkTermHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    (void)sigaction(SIGTERM, &action, nullptr);
#else
    (void)0;
#endif
}

inline bool termRequested()
{
    return termFlag() != 0;
}

inline bool parkSnapshotDue()
{
    if (!parkEnabled())
    {
        return false;
    }
    if (termRequested())
    {
        return true;
    }
    const uint64_t timeout = parkTimeoutMs();
    if (timeout != 0u && parkStartMs() != 0u)
    {
        return parkSteadyMs() - parkStartMs() >= timeout;
    }
    return false;
}

inline bool takeSnapshotTurn()
{
    static std::atomic<bool> taken{false};
    return !taken.exchange(true, std::memory_order_acq_rel);
}

// --------------------------------------------------------------- snapshot

struct ParkThreadRow
{
    int id = 0;
    int status = 0;
    std::string statusName;
    int waitReason = 0;
    std::string waitReasonName;
    int waitId = 0;
    uint32_t pc = 0;
    uint32_t ra = 0;
    uint32_t sp = 0;
    uint32_t entry = 0;
    int priority = 0;
    uint64_t scheduled = 0;
    std::vector<uint32_t> chain;
};

struct ParkSemaRow
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    int initCount = 0;
    uint32_t waiters = 0;
};

struct ParkHotPc
{
    uint32_t pc = 0;
    uint64_t count = 0;
    uint32_t firstRa = 0;
    uint32_t lastRa = 0;
};

struct ParkDrop
{
    std::string site;
    std::string reason;
    uint64_t count = 0;
};

struct ParkGs
{
    uint64_t kicks = 0;
    uint64_t kicksDrawing = 0;
    uint64_t gifPackets = 0;
    uint64_t copyRegs = 0;
    uint64_t dmaStarts = 0;
    uint64_t gifCopies = 0;
    uint64_t gsWrites = 0;
    uint64_t vifWrites = 0;
};

struct ParkSnapshotData
{
    std::string source = "emitter";
    std::vector<ParkThreadRow> threads;
    std::vector<ParkSemaRow> semaphores;
    std::vector<ParkSemaCreate> creates;
    std::map<int, std::map<uint32_t, uint64_t>> waitHist;
    std::map<int, std::map<uint32_t, uint64_t>> signalHist;
    std::vector<ParkHotPc> hotPc;
    std::vector<ParkDrop> drops;
    std::vector<ParkRpcEvent> rpc;
    uint64_t rpcOverflow = 0;
    ParkGs gs;
    std::map<int, uint64_t> sched;
};

inline std::string parkHex(uint32_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

inline std::string parkEscapeJson(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 2u);
    for (char ch : in)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20u)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += ch;
            }
            break;
        }
    }
    return out;
}

inline std::string renderParkJson(const ParkSnapshotData &data)
{
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"" << kSchema << "\",\n";
    out << "  \"source\": \"" << parkEscapeJson(data.source) << "\",\n";
    out << "  \"threads\": [";
    for (size_t i = 0; i < data.threads.size(); ++i)
    {
        const ParkThreadRow &t = data.threads[i];
        out << (i == 0 ? "\n" : ",\n");
        out << "    {\"id\": " << t.id
            << ", \"status\": " << t.status
            << ", \"status_name\": \"" << parkEscapeJson(t.statusName) << "\""
            << ", \"wait_reason\": " << t.waitReason
            << ", \"wait_reason_name\": \"" << parkEscapeJson(t.waitReasonName) << "\""
            << ", \"wait_id\": " << t.waitId
            << ", \"pc\": \"" << parkHex(t.pc) << "\""
            << ", \"ra\": \"" << parkHex(t.ra) << "\""
            << ", \"sp\": \"" << parkHex(t.sp) << "\""
            << ", \"entry\": \"" << parkHex(t.entry) << "\""
            << ", \"priority\": " << t.priority
            << ", \"scheduled\": " << t.scheduled
            << ", \"chain\": [";
        for (size_t c = 0; c < t.chain.size(); ++c)
        {
            out << (c == 0 ? "" : ", ") << "\"" << parkHex(t.chain[c]) << "\"";
        }
        out << "]}";
    }
    out << (data.threads.empty() ? "],\n" : "\n  ],\n");
    out << "  \"semaphores\": [";
    for (size_t i = 0; i < data.semaphores.size(); ++i)
    {
        const ParkSemaRow &s = data.semaphores[i];
        out << (i == 0 ? "\n" : ",\n");
        out << "    {\"id\": " << s.id << ", \"count\": " << s.count
            << ", \"max\": " << s.maxCount << ", \"init\": " << s.initCount
            << ", \"waiters\": " << s.waiters << "}";
    }
    out << (data.semaphores.empty() ? "],\n" : "\n  ],\n");
    out << "  \"sema_creates\": [";
    for (size_t i = 0; i < data.creates.size(); ++i)
    {
        const ParkSemaCreate &c = data.creates[i];
        out << (i == 0 ? "\n" : ",\n");
        out << "    {\"id\": " << c.id << ", \"tid\": " << c.tid
            << ", \"pc\": \"" << parkHex(c.pc) << "\""
            << ", \"init\": " << c.initCount << ", \"max\": " << c.maxCount << "}";
    }
    out << (data.creates.empty() ? "],\n" : "\n  ],\n");
    auto renderHist = [&out](const char *name, const std::map<int, std::map<uint32_t, uint64_t>> &hist) {
        out << "  \"" << name << "\": {";
        bool firstId = true;
        for (const auto &[id, pcs] : hist)
        {
            out << (firstId ? "\n" : ",\n");
            firstId = false;
            out << "    \"" << id << "\": {";
            bool firstPc = true;
            for (const auto &[pc, count] : pcs)
            {
                out << (firstPc ? "" : ", ");
                firstPc = false;
                out << "\"" << parkHex(pc) << "\": " << count;
            }
            out << "}";
        }
        out << (hist.empty() ? "},\n" : "\n  },\n");
    };
    renderHist("sema_wait_hist", data.waitHist);
    renderHist("sema_signal_hist", data.signalHist);
    out << "  \"hot_pc\": [";
    for (size_t i = 0; i < data.hotPc.size(); ++i)
    {
        const ParkHotPc &h = data.hotPc[i];
        out << (i == 0 ? "\n" : ",\n");
        out << "    {\"pc\": \"" << parkHex(h.pc)
            << "\", \"count\": " << h.count
            << ", \"first_ra\": \"" << parkHex(h.firstRa)
            << "\", \"last_ra\": \"" << parkHex(h.lastRa) << "\"}";
    }
    out << (data.hotPc.empty() ? "],\n" : "\n  ],\n");
    out << "  \"drops\": [";
    for (size_t i = 0; i < data.drops.size(); ++i)
    {
        const ParkDrop &d = data.drops[i];
        out << (i == 0 ? "\n" : ",\n");
        out << "    {\"site\": \"" << parkEscapeJson(d.site)
            << "\", \"reason\": \"" << parkEscapeJson(d.reason)
            << "\", \"count\": " << d.count << "}";
    }
    out << (data.drops.empty() ? "],\n" : "\n  ],\n");
    out << "  \"sif_rpc\": [";
    for (size_t i = 0; i < data.rpc.size(); ++i)
    {
        const ParkRpcEvent &e = data.rpc[i];
        out << (i == 0 ? "\n" : ",\n");
        out << "    {\"op\": \"" << parkEscapeJson(e.op)
            << "\", \"sid\": \"" << parkHex(e.sid)
            << "\", \"fno\": \"" << parkHex(e.fno)
            << "\", \"send_size\": " << e.sendSize
            << ", \"recv_size\": " << e.recvSize
            << ", \"tid\": " << e.tid
            << ", \"claimed\": " << (e.claimed ? "true" : "false")
            << ", \"path\": \"" << parkEscapeJson(e.path) << "\"}";
    }
    out << (data.rpc.empty() ? "],\n" : "\n  ],\n");
    out << "  \"sif_rpc_overflow\": " << data.rpcOverflow << ",\n";
    out << "  \"gs\": {\"kicks\": " << data.gs.kicks
        << ", \"kicks_drawing\": " << data.gs.kicksDrawing
        << ", \"gif_packets\": " << data.gs.gifPackets
        << ", \"copy_regs\": " << data.gs.copyRegs
        << ", \"dma_starts\": " << data.gs.dmaStarts
        << ", \"gif_copies\": " << data.gs.gifCopies
        << ", \"gs_writes\": " << data.gs.gsWrites
        << ", \"vif_writes\": " << data.gs.vifWrites << "},\n";
    out << "  \"sched_counts\": {";
    bool firstSched = true;
    for (const auto &[tid, count] : data.sched)
    {
        out << (firstSched ? "" : ", ");
        firstSched = false;
        out << "\"" << tid << "\": " << count;
    }
    out << "}\n}\n";
    return out.str();
}

inline std::string renderParkTable(const ParkSnapshotData &data)
{
    std::ostringstream out;
    out << "park snapshot (" << data.source << ", schema " << kSchema << ")\n";
    out << "threads (" << data.threads.size() << "):\n";
    for (const ParkThreadRow &t : data.threads)
    {
        out << "  id=" << t.id << " " << t.statusName
            << " wait=" << t.waitReasonName << ":" << t.waitId
            << " pc=" << parkHex(t.pc) << " ra=" << parkHex(t.ra)
            << " sp=" << parkHex(t.sp) << " entry=" << parkHex(t.entry)
            << " pri=" << t.priority << " sched=" << t.scheduled
            << " chain=[";
        for (size_t c = 0; c < t.chain.size(); ++c)
        {
            out << (c == 0 ? "" : " ") << parkHex(t.chain[c]);
        }
        out << "]\n";
    }
    out << "semaphores (" << data.semaphores.size() << "):\n";
    for (const ParkSemaRow &s : data.semaphores)
    {
        out << "  id=" << s.id << " count=" << s.count
            << " max=" << s.maxCount << " init=" << s.initCount
            << " waiters=" << s.waiters << "\n";
    }
    auto renderTableHist = [&out](const char *name, const std::map<int, std::map<uint32_t, uint64_t>> &hist) {
        out << name << ":\n";
        for (const auto &[id, pcs] : hist)
        {
            for (const auto &[pc, count] : pcs)
            {
                out << "  id=" << id << " pc=" << parkHex(pc) << " n=" << count << "\n";
            }
        }
    };
    renderTableHist("sema waits", data.waitHist);
    renderTableHist("sema signals", data.signalHist);
    out << "hot pc (top 30 of " << data.hotPc.size() << "):\n";
    for (size_t i = 0; i < data.hotPc.size() && i < 30u; ++i)
    {
        const ParkHotPc &h = data.hotPc[i];
        out << "  pc=" << parkHex(h.pc) << " n=" << h.count
            << " first_ra=" << parkHex(h.firstRa)
            << " last_ra=" << parkHex(h.lastRa) << "\n";
    }
    out << "drops (" << data.drops.size() << "):\n";
    for (const ParkDrop &d : data.drops)
    {
        out << "  " << d.site << " " << d.reason << " n=" << d.count << "\n";
    }
    out << "sif/rpc (" << data.rpc.size() << " events, overflow=" << data.rpcOverflow << "):\n";
    {
        std::map<std::string, std::map<std::string, uint64_t>> tally;
        for (const ParkRpcEvent &e : data.rpc)
        {
            tally[e.op][e.claimed ? "claimed" : "unclaimed"]++;
        }
        for (const auto &[op, sides] : tally)
        {
            for (const auto &[side, count] : sides)
            {
                out << "  " << op << " " << side << " n=" << count << "\n";
            }
        }
    }
    for (const ParkRpcEvent &e : data.rpc)
    {
        out << "  op=" << e.op << " sid=" << parkHex(e.sid)
            << " fno=" << parkHex(e.fno)
            << " send=" << e.sendSize << " recv=" << e.recvSize
            << " tid=" << e.tid << (e.claimed ? " claimed" : " unclaimed");
        if (!e.path.empty())
        {
            out << " path=" << e.path;
        }
        out << "\n";
    }
    out << "gs: kicks=" << data.gs.kicks
        << " drawing=" << data.gs.kicksDrawing
        << " gif=" << data.gs.gifPackets
        << " copy=" << data.gs.copyRegs
        << " dma=" << data.gs.dmaStarts
        << " gifcpy=" << data.gs.gifCopies
        << " gsw=" << data.gs.gsWrites
        << " vif=" << data.gs.vifWrites << "\n";
    return out.str();
}

inline bool writeParkFiles(const ParkSnapshotData &data, const std::string &dir)
{
    const std::string jsonPath = dir + "/" + kJsonName;
    const std::string tablePath = dir + "/" + kTableName;
    {
        std::ofstream json(jsonPath, std::ios::out | std::ios::trunc);
        if (!json.is_open())
        {
            return false;
        }
        json << renderParkJson(data);
        json.flush();
        if (!json)
        {
            return false;
        }
    }
    {
        std::ofstream table(tablePath, std::ios::out | std::ios::trunc);
        if (!table.is_open())
        {
            return false;
        }
        table << renderParkTable(data);
        table.flush();
        if (!table)
        {
            return false;
        }
    }
    std::cerr << "[diag:park] snapshot written json=" << jsonPath
              << " table=" << tablePath << std::endl;
    return true;
}

} // namespace ps2_park
