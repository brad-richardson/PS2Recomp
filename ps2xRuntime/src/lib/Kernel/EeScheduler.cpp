#include "runtime/ee_scheduler.h"
#include "ps2_e41_trace.h"
#include "ps2_mpg_src_trace.h"
#include "ps2_e3.h"
#include "ps2_e4.h"
#include "ps2_gfx_stats.h"
#include "ps2_vu1_entry_trace.h"
#include "ps2_vu1_trace.h"
#include "ps2_e15.h"
#include "ps2_snd_spike.h"

#include "ps2_log.h"
#include "ps2_park_snapshot.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_map>

// P1c histogram flushers owned by other translation units (defined in
// Kernel/Syscalls/Dispatcher.cpp and ps2_runtime.cpp). Called from the
// periodic tick below so quiet periods still emit blocks.
namespace ps2_syscalls
{
    void diagSyscallsPeriodicFlush();
}
void diagCallsPeriodicFlush();
namespace ps2_stubs
{
    uint32_t getCdCallbackStackTop();
}

namespace
{
    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    constexpr uint64_t kVBlankPeriodCycles = microsecondsToEeCycles(16667u);
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }

    // P1c steady-state diagnostics. Everything below is gated on
    // PS2X_DIAG_PERIOD_MS: unset/empty/0 means compiled in, nothing printed,
    // and callers pay only a counter increment plus a cached static check.
    uint64_t diagPeriodMs()
    {
        static const uint64_t period = [] {
            if (const char *env = std::getenv("PS2X_DIAG_PERIOD_MS"))
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
        return period;
    }

    uint64_t diagNowMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }

    // Shared printer for the Fix C thread table. Fix C callers pass "ee:idle";
    // the periodic P1c dump passes "diag:thread" and appends priority plus
    // the per-thread schedule count.
    void printEeThreadDiagLine(std::ostream &os, const char *prefix, const EeThreadSnapshot &thread)
    {
        os << "[" << prefix << "] id=" << thread.id
           << " status=" << static_cast<int>(thread.status)
           << " waitReason=" << static_cast<int>(thread.waitReason)
           << " waitId=" << thread.waitId << " pc=0x"
           << std::hex << thread.pc << std::dec
           << " entry=0x" << std::hex << thread.entry << std::dec;
    }

    // Tag mark for CD-completion invocations queued by queueCdCallback
    // (Kernel/Stubs/CD.cpp). Upper 32 bits are the 'CDCB' magic, lower 32
    // bits are the SCE callback function id.
    constexpr uint64_t kCdCallbackDiagTagBase = 0x4344434200000000ULL;
    constexpr uint64_t kCdCallbackDiagTagMask = 0xFFFFFFFF00000000ULL;

    bool isCdCallbackDiagTag(uint64_t tag)
    {
        return (tag & kCdCallbackDiagTagMask) == kCdCallbackDiagTagBase;
    }

    // Per-thread schedule counts since the last periodic dump.
    std::unordered_map<int, uint64_t> g_diagSchedCounts;

    // P1s sema delivery-mechanism diagnostics. Gated on PS2X_DIAG_SEMA
    // (unset/empty = compiled in, nothing printed, callers pay only a
    // cached static check). One line per signal/wait with the waker
    // context and the wake decision.
    bool diagSemaEnabled()
    {
        static const bool enabled = [] {
            const char *env = std::getenv("PS2X_DIAG_SEMA");
            return env != nullptr && env[0] != '\0';
        }();
        return enabled;
    }

    // P1u object-pointer capture for unknown-id waits. Gated on
    // PS2X_DIAG_SEMA_S0 (unset/empty = wait lines byte-identical to P1s,
    // callers pay only a cached static check).
    bool diagSemaS0Enabled()
    {
        static const bool enabled = [] {
            const char *env = std::getenv("PS2X_DIAG_SEMA_S0");
            return env != nullptr && env[0] != '\0';
        }();
        return enabled;
    }
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

// T1 park snapshot fill: runs on the executor inside run() when SIGTERM
// (or PARK_TIMEOUT_MS) fires. Reads live kernel state plus the always-on
// T1 tallies, writes ONE JSON + ONE table, then stops the boot on the
// SIGTERM path only (a timeout snapshot is mid-run, non-terminal).
namespace
{
const char *parkStatusName(EeThreadStatus status)
{
    switch (status)
    {
    case EeThreadStatus::Running:
        return "Running";
    case EeThreadStatus::Ready:
        return "Ready";
    case EeThreadStatus::Waiting:
        return "Waiting";
    case EeThreadStatus::WaitingSuspended:
        return "WaitingSuspended";
    case EeThreadStatus::Suspended:
        return "Suspended";
    case EeThreadStatus::Dormant:
        return "Dormant";
    default:
        return "Unknown";
    }
}

const char *parkWaitReasonName(EeWaitReason reason)
{
    switch (reason)
    {
    case EeWaitReason::None:
        return "None";
    case EeWaitReason::Sleep:
        return "Sleep";
    case EeWaitReason::Semaphore:
        return "Semaphore";
    case EeWaitReason::EventFlag:
        return "EventFlag";
    case EeWaitReason::VSync:
        return "VSync";
    case EeWaitReason::External:
        return "External";
    case EeWaitReason::Mpeg:
        return "Mpeg";
    default:
        return "Unknown";
    }
}
} // namespace

void parkSnapshotWriteOnce(EeScheduler &ee, PS2Runtime &runtime)
{
    if (!ps2_park::takeSnapshotTurn())
    {
        return;
    }
    ps2_park::ParkSnapshotData data;
    const EeKernelSnapshot snap = ee.snapshot();
    const std::map<int, uint64_t> &sched = ps2_park::schedCounts();
    for (const EeThreadSnapshot &t : snap.threads)
    {
        ps2_park::ParkThreadRow row;
        row.id = t.id;
        row.status = static_cast<int>(t.status);
        row.statusName = parkStatusName(t.status);
        row.waitReason = static_cast<int>(t.waitReason);
        row.waitReasonName = parkWaitReasonName(t.waitReason);
        row.waitId = t.waitId;
        row.pc = t.pc;
        row.entry = t.entry;
        row.priority = t.currentPriority;
        if (const auto it = sched.find(t.id); it != sched.end())
        {
            row.scheduled = it->second;
        }
        // Shallow ra chain: live $ra, then stacked invocation pcs
        // innermost-first (no guest-stack walk; see the T1 report).
        if (const GuestThread *live = ee.thread(t.id))
        {
            const R5900Context &active = live->activeContext();
            row.ra = getRegU32(&active, 31);
            row.sp = getRegU32(&active, 29);
            row.chain.push_back(row.ra);
            for (size_t i = live->invocations.size(); i > 0; --i)
            {
                row.chain.push_back(live->invocations[i - 1].context.pc);
            }
        }
        data.threads.push_back(std::move(row));
    }
    std::sort(data.threads.begin(), data.threads.end(),
              [](const ps2_park::ParkThreadRow &a, const ps2_park::ParkThreadRow &b) {
                  return a.id < b.id;
              });
    for (const EeSemaphoreSnapshot &s : snap.semaphores)
    {
        ps2_park::ParkSemaRow row;
        row.id = s.id;
        row.count = s.count;
        row.maxCount = s.maxCount;
        row.waiters = s.waiters;
        if (const EeSemaphore *live = ee.semaphore(s.id))
        {
            row.initCount = live->initCount;
        }
        data.semaphores.push_back(row);
    }
    std::sort(data.semaphores.begin(), data.semaphores.end(),
              [](const ps2_park::ParkSemaRow &a, const ps2_park::ParkSemaRow &b) {
                  return a.id < b.id;
              });
    data.creates = ps2_park::semaCreates();
    data.waitHist = ps2_park::semaWaitHist();
    data.signalHist = ps2_park::semaSignalHist();
    for (const auto &[pc, entry] : ps2_park::hotPcCounts())
    {
        ps2_park::ParkHotPc hot;
        hot.pc = pc;
        hot.count = entry.count;
        hot.firstRa = entry.firstRa;
        hot.lastRa = entry.lastRa;
        data.hotPc.push_back(hot);
    }
    std::sort(data.hotPc.begin(), data.hotPc.end(),
              [](const ps2_park::ParkHotPc &a, const ps2_park::ParkHotPc &b) {
                  return a.count > b.count;
              });
    for (const ps2_log::DropCensusRow &d : ps2_log::snapshotDropCensus())
    {
        ps2_park::ParkDrop drop;
        drop.site = d.site;
        drop.reason = d.reason;
        drop.count = d.count;
        data.drops.push_back(std::move(drop));
    }
    data.rpc = ps2_park::rpcEvents();
    data.rpcOverflow = ps2_park::rpcEventsOverflow();
    data.gs.kicks = ps2_park::gsKickCount().load(std::memory_order_relaxed);
    data.gs.kicksDrawing = ps2_park::gsKickDrawingCount().load(std::memory_order_relaxed);
    data.gs.gifPackets = ps2_park::gsGifPacketCount().load(std::memory_order_relaxed);
    data.gs.copyRegs = ps2_park::gsCopyRegCount().load(std::memory_order_relaxed);
    data.gs.dmaStarts = runtime.memory().dmaStartCount();
    data.gs.gifCopies = runtime.memory().gifCopyCount();
    data.gs.gsWrites = runtime.memory().gsWriteCount();
    data.gs.vifWrites = runtime.memory().vifWriteCount();
    data.sched = sched;
    if (!ps2_park::writeParkFiles(data, ps2_park::parkDir()))
    {
        std::cerr << "[diag:park] snapshot write FAILED dir=" << ps2_park::parkDir() << std::endl;
    }
    if (ps2_park::termRequested())
    {
        ee.requestStop();
    }
}

EeScheduler::~EeScheduler()
{
    requestStop();
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_soundClockStarted = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    scheduleEvent(m_eeCycle + kVBlankPeriodCycles,
                  std::chrono::steady_clock::now() + kVBlankPeriod,
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);
    // T1 park snapshot: install the SIGTERM handler once when enabled.
    ps2_park::installParkTermHandler();

    // P1c steady-state diagnostics state. When PS2X_DIAG_PERIOD_MS is unset
    // the per-iteration cost below is one counter increment plus a check.
    static uint64_t s_diagTick = 0;
    static uint64_t s_diagLastMs = 0;
    static uint64_t s_diagBlock = 0;

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        // T1: SIGTERM (or PARK_TIMEOUT_MS) writes ONE JSON + ONE table,
        // then stops the boot on the SIGTERM path only.
        if (ps2_park::parkSnapshotDue())
        {
            parkSnapshotWriteOnce(*this, m_runtime);
        }
        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        ++s_diagTick;
        const uint64_t diagPeriod = diagPeriodMs();
        if (diagPeriod != 0u)
        {
            const uint64_t diagNow = diagNowMs();
            if (s_diagLastMs == 0u)
            {
                s_diagLastMs = diagNow;
            }
            else if (diagNow - s_diagLastMs >= diagPeriod)
            {
                s_diagLastMs = diagNow;
                publishSnapshot();
                const EeKernelSnapshot diagSnap = snapshot();
                std::cerr << "[diag:threads] block=" << s_diagBlock++
                          << " threads=" << diagSnap.threads.size()
                          << " period_ms=" << diagPeriod << std::endl;
                for (const EeThreadSnapshot &diagThread : diagSnap.threads)
                {
                    uint64_t scheduled = 0u;
                    if (auto it = g_diagSchedCounts.find(diagThread.id); it != g_diagSchedCounts.end())
                    {
                        scheduled = it->second;
                    }
                    printEeThreadDiagLine(std::cerr, "diag:thread", diagThread);
                    std::cerr << " priority=" << diagThread.currentPriority
                              << " scheduled=" << scheduled << std::endl;
                }
                // I18: frame/GS observation on the existing P1c console tap.
                // Read-only relaxed-atomic reads under the same
                // PS2X_DIAG_PERIOD_MS gate; one line per period naming
                // vsync/GS/DMA progress past silent guest walls.
                std::cerr << "[diag:frame] block=" << (s_diagBlock - 1)
                          << " vsync=" << m_runtime.memory().gs().vsyncTick.load(std::memory_order_relaxed)
                          << " kicks=" << ps2_park::gsKickCount().load(std::memory_order_relaxed)
                          << " kicksDrawing=" << ps2_park::gsKickDrawingCount().load(std::memory_order_relaxed)
                          << " gif=" << ps2_park::gsGifPacketCount().load(std::memory_order_relaxed)
                          << " copyRegs=" << ps2_park::gsCopyRegCount().load(std::memory_order_relaxed)
                          << " dma=" << m_runtime.memory().dmaStartCount()
                          << " gifCopy=" << m_runtime.memory().gifCopyCount()
                          << " gsWrites=" << m_runtime.memory().gsWriteCount()
                          << " vifWrites=" << m_runtime.memory().vifWriteCount() << std::endl;
                // P1f stack map, same gate as the thread table.
                for (const auto &[stackId, stackThread] : m_threads)
                {
                    (void)stackId;
                    const uint32_t stackSp = getRegU32(&stackThread.activeContext(), 29);
                    std::cerr << "[diag:stacks] block=" << (s_diagBlock - 1)
                              << " thread id=" << stackThread.id
                              << " stack=0x" << std::hex << stackThread.stack
                              << " stackSize=0x" << stackThread.stackSize
                              << " sp=0x" << stackSp
                              << " entry=0x" << stackThread.entry
                              << " pc=0x" << stackThread.activeContext().pc << std::dec << std::endl;
                    for (size_t invIdx = 0; invIdx < stackThread.invocations.size(); ++invIdx)
                    {
                        const GuestInvocation &inv = stackThread.invocations[invIdx];
                        std::cerr << "[diag:stacks] block=" << (s_diagBlock - 1)
                                  << " active thread=" << stackThread.id
                                  << " idx=" << invIdx
                                  << " kind=" << static_cast<int>(inv.kind)
                                  << " pc=0x" << std::hex << inv.context.pc
                                  << " sp=0x" << getRegU32(&inv.context, 29) << std::dec << std::endl;
                    }
                }
                for (const auto &[invKey, invTop] : m_invocationStackTops)
                {
                    const uint32_t keyThread = static_cast<uint32_t>(invKey >> 32u);
                    const uint32_t keyDepth = static_cast<uint32_t>(invKey & 0xFFFFFFFFu);
                    std::cerr << "[diag:stacks] block=" << (s_diagBlock - 1)
                              << " invocation key=0x" << std::hex << invKey << std::dec
                              << " thread=" << keyThread
                              << " depth=" << keyDepth
                              << " top=0x" << std::hex << invTop << std::dec << std::endl;
                }
                std::cerr << "[diag:stacks] block=" << (s_diagBlock - 1)
                          << " cdCallbackStackTop=0x" << std::hex << ps2_stubs::getCdCallbackStackTop() << std::dec
                          << std::endl;
                for (const auto &[intcId, intcHandler] : m_intcHandlers)
                {
                    std::cerr << "[diag:stacks] block=" << (s_diagBlock - 1)
                              << " intc id=" << intcId
                              << " cause=" << intcHandler.cause
                              << " handler=0x" << std::hex << intcHandler.handler
                              << " sp=0x" << intcHandler.sp << std::dec << std::endl;
                }
                for (const auto &[dmacId, dmacHandler] : m_dmacHandlers)
                {
                    std::cerr << "[diag:stacks] block=" << (s_diagBlock - 1)
                              << " dmac id=" << dmacId
                              << " cause=" << dmacHandler.cause
                              << " handler=0x" << std::hex << dmacHandler.handler
                              << " sp=0x" << dmacHandler.sp << std::dec << std::endl;
                }
                for (const auto &[alarmId, alarm] : m_alarms)
                {
                    std::cerr << "[diag:stacks] block=" << (s_diagBlock - 1)
                              << " alarm id=" << alarmId
                              << " handler=0x" << std::hex << alarm.handler
                              << " sp=0x" << alarm.sp << std::dec << std::endl;
                }
                for (const GuestInvocation &pending : m_pendingInvocations)
                {
                    std::cerr << "[diag:stacks] block=" << (s_diagBlock - 1)
                              << " pending kind=" << static_cast<int>(pending.kind)
                              << " pc=0x" << std::hex << pending.context.pc
                              << " sp=0x" << getRegU32(&pending.context, 29) << std::dec << std::endl;
                }
                g_diagSchedCounts.clear();
                ps2_syscalls::diagSyscallsPeriodicFlush();
                diagCallsPeriodicFlush();
            }
        }

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            // Diagnostic idle dump (function statics keep this to run()).
            static auto idleSince = std::chrono::steady_clock::time_point{};
            static bool idleDumpPrinted = false;
            if (!next && m_pendingInvocations.empty())
            {
                publishSnapshot();
                const auto idleNow = std::chrono::steady_clock::now();
                if (idleSince == std::chrono::steady_clock::time_point{})
                {
                    idleSince = idleNow;
                }
                else if (!idleDumpPrinted &&
                         idleNow - idleSince >= std::chrono::seconds(3))
                {
                    idleDumpPrinted = true;
                    const EeKernelSnapshot idleSnap = snapshot();
                    std::cerr << "[ee:idle] no runnable thread for 3s; threads="
                              << idleSnap.threads.size() << std::endl;
                    for (const EeThreadSnapshot &idleThread : idleSnap.threads)
                    {
                        printEeThreadDiagLine(std::cerr, "ee:idle", idleThread);
                        std::cerr << std::endl;
                    }
                }
                waitForEvent();
                continue;
            }
            idleSince = std::chrono::steady_clock::time_point{};
            idleDumpPrinted = false;
            if (next)
            {
                makeRunning(*next);
                ps2_park::tallySched(next->id);
                if (diagPeriod != 0u)
                {
                    ++g_diagSchedCounts[next->id];
                }
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                ps2_park::tallySched(owner->id);
                if (diagPeriod != 0u)
                {
                    ++g_diagSchedCounts[owner->id];
                    if (isCdCallbackDiagTag(invocation.tag))
                    {
                        std::cerr << "[cd:callback] start func=" << (invocation.tag & 0xFFFFFFFFu)
                                  << " cb=0x" << std::hex << invocation.context.pc << std::dec << std::endl;
                    }
                }
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
        m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
        m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
        m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);

        if (context.pc == 0u)
        {
            if (!running->invocations.empty())
            {
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
                continue;
            }
            if (diagPeriod != 0u)
            {
                uint64_t dormantScheduled = 0u;
                if (auto it = g_diagSchedCounts.find(running->id); it != g_diagSchedCounts.end())
                {
                    dormantScheduled = it->second;
                }
                std::cerr << "[diag:dormant] id=" << running->id
                          << " entry=0x" << std::hex << running->entry
                          << " pc=0x" << context.pc
                          << " ra=0x" << getRegU32(&context, 31)
                          << " sp=0x" << getRegU32(&context, 29)
                          << " gp=0x" << getRegU32(&context, 28)
                          << " v0=0x" << getRegU32(&context, 2)
                          << " a0=0x" << getRegU32(&context, 4) << std::dec
                          << " scheduled=" << dormantScheduled
                          << " trace=" << m_runtime.formatDispatchHistory() << std::endl;
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            continue;
        }

        if (!m_pendingInvocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            if (diagPeriod != 0u && isCdCallbackDiagTag(invocation.tag))
            {
                std::cerr << "[cd:callback] start func=" << (invocation.tag & 0xFFFFFFFFu)
                          << " cb=0x" << std::hex << invocation.context.pc << std::dec << std::endl;
            }
            if (getRegU32(&invocation.context, 29) == 0u)
            {
                SET_GPR_U32(&invocation.context, 29, invocationStackTop());
            }
            running->invocations.push_back(std::move(invocation));
            continue;
        }

        if (!m_runtime.hasFunction(context.pc))
        {
            if (!running->invocations.empty())
            {
                // P1w: the queued invocation's entry has no table entry, so
                // its body never runs (P1s class). Trace the discard.
                char dropArgs[64];
                std::snprintf(dropArgs, sizeof(dropArgs), "pc=0x%x tid=%d depth=%zu",
                              context.pc, running->id, running->invocations.size());
                ps2_log::emitDrop("sched/invocation-dispatch", "no-table-entry", dropArgs);
                context.pc = 0u;
            }
            else
            {
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                if (diagPeriod != 0u)
                {
                    uint64_t dormantScheduled = 0u;
                    if (auto it = g_diagSchedCounts.find(running->id); it != g_diagSchedCounts.end())
                    {
                        dormantScheduled = it->second;
                    }
                    std::cerr << "[diag:dormant] id=" << running->id
                              << " entry=0x" << std::hex << running->entry
                              << " pc=0x" << context.pc
                              << " ra=0x" << getRegU32(&context, 31)
                              << " sp=0x" << getRegU32(&context, 29)
                              << " gp=0x" << getRegU32(&context, 28)
                              << " v0=0x" << getRegU32(&context, 2)
                              << " a0=0x" << getRegU32(&context, 4) << std::dec
                              << " scheduled=" << dormantScheduled
                              << " trace=" << m_runtime.formatDispatchHistory() << std::endl;
                }
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);

        if (checkpointDue(kGuestDispatchCycles))
        {
            continue;
        }

        try
        {
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            ps2_mpg_src_trace::noteSliceIrq(m_insideInterrupt);
            m_guestExecuting.store(true, std::memory_order_release);
            if (ps2DiagWatchEnabled())
            {
                ps2DiagWatchSetThread(m_currentThreadId);
            }
            if (ps2_e3::enabled())
            {
                ps2_e3::noteThread(m_currentThreadId);
            }
            ps2_e15::Trace mpegTrace("scheduler",m_vsyncTick,m_rdram,&context,context.pc,0u,m_currentThreadId);
            function(m_rdram, &context, &m_runtime);
            mpegTrace.finish(m_vsyncTick);
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
            ps2_mpg_src_trace::noteSliceIrq(false);
        }
        catch (const EeDispatcherTransfer &)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
            ps2_mpg_src_trace::noteSliceIrq(false);
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    accountCycles(cycles);

    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        return true;
    }

    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
    {
        m_checkpointPending.store(true, std::memory_order_release);
        return true;
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        return false;
    }

    const GuestThread *running = currentThread();
    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
    {
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        return true;
    }

    renewTimeSlice();
    return false;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    m_eeCycle += elapsed;
    m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(elapsed);
    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    if (params.priority < 1 || params.priority >= kPriorityCount)
    {
        char dropArgs[32];
        std::snprintf(dropArgs, sizeof(dropArgs), "priority=%d", params.priority);
        ps2_log::emitDrop("sched/createThread", "KE_ILLEGAL_PRIORITY", dropArgs);
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        ps2_log::emitDrop("sched/createThread", "KE_ERROR", "id-exhausted");
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        ps2_log::emitDrop("sched/deleteThread", "KE_ILLEGAL_THID");
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        ps2_log::emitDrop("sched/deleteThread", "KE_UNKNOWN_THID");
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        ps2_log::emitDrop("sched/deleteThread", "KE_NOT_DORMANT");
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        ps2_log::emitDrop("sched/startThread", "KE_UNKNOWN_THID");
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        ps2_log::emitDrop("sched/startThread", "KE_NOT_DORMANT");
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    if (diagPeriodMs() != 0u)
    {
        std::cerr << "[diag:start-thread] id=" << id
                  << " func=0x" << std::hex << target->entry
                  << " stack=0x" << target->stack
                  << " stack_size=0x" << target->stackSize
                  << " gp=0x" << target->gp << std::dec
                  << " priority=" << target->initialPriority
                  << " attr=0x" << std::hex << target->attr << std::dec
                  << " initial_sp=0x" << std::hex << stackTop << std::dec << std::endl;
    }
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        ps2_log::emitDrop("sched/terminateThread", "KE_ILLEGAL_THID");
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        ps2_log::emitDrop("sched/terminateThread", "KE_UNKNOWN_THID");
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        ps2_log::emitDrop("sched/terminateThread", "KE_DORMANT");
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        ps2_log::emitDrop("sched/suspendThread", "KE_UNKNOWN_THID");
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        ps2_log::emitDrop("sched/suspendThread", "KE_DORMANT");
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        ps2_log::emitDrop("sched/resumeThread", "KE_ILLEGAL_THID");
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        ps2_log::emitDrop("sched/resumeThread", "KE_UNKNOWN_THID");
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        ps2_log::emitDrop("sched/resumeThread", "KE_NOT_SUSPEND");
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        ps2_log::emitDrop("sched/wakeupThread", "KE_ILLEGAL_THID");
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        ps2_log::emitDrop("sched/wakeupThread", "KE_UNKNOWN_THID");
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        ps2_log::emitDrop("sched/wakeupThread", "KE_DORMANT");
        return KE_DORMANT;
    }
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        ps2_log::emitDrop("sched/cancelWakeup", "KE_UNKNOWN_THID");
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (priority < 1 || priority >= kPriorityCount)
    {
        ps2_log::emitDrop("sched/changePriority", "KE_ILLEGAL_PRIORITY");
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        ps2_log::emitDrop("sched/changePriority", "KE_UNKNOWN_THID");
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        ps2_log::emitDrop("sched/rotateReadyQueue", "KE_ILLEGAL_PRIORITY");
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        ps2_log::emitDrop("sched/releaseWait", "KE_ILLEGAL_THID");
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        ps2_log::emitDrop("sched/releaseWait", "KE_UNKNOWN_THID");
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        ps2_log::emitDrop("sched/releaseWait", "KE_NOT_WAIT");
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    // P1aa (amends P1v per the P1z kernel disassembly): stock EE CreateSema
    // rejects ONLY init<0 (+ id exhaustion); max is stored as-is with no
    // clamp and no validation, so negatives and init>max are accepted.
    if (initCount < 0)
    {
        char dropArgs[64];
        std::snprintf(dropArgs, sizeof(dropArgs), "init=%d max=%d",
                      initCount, maxCount);
        ps2_log::emitDrop("sched/createSemaphore", "KE_ERROR", dropArgs);
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        ps2_log::emitDrop("sched/createSemaphore", "KE_ERROR", "id-exhausted");
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    // T1: success-only create row (the miner filters [diag:sema-create]
    // to ret>0 the same way; noparam/failure lines stay log-only).
    {
        const GuestThread *creator = currentThread();
        const uint32_t creatorPc = creator ? creator->activeContext().pc : 0u;
        ps2_park::tallySemaCreate(id, m_currentThreadId, creatorPc, initCount, maxCount);
    }
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        char dropArgs[32];
        std::snprintf(dropArgs, sizeof(dropArgs), "id=%d", id);
        ps2_log::emitDrop("sched/deleteSemaphore", "KE_UNKNOWN_SEMID", dropArgs);
        // P12: stock DeleteSema returns plain -1 for unknown ids
        // (0x80004a94 b -> addiu v0,zero,-1; no -408 in KERNEL).
        return KE_ERROR;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    // T1: entry tally covers exactly the calls the [diag:sema] lines cover.
    {
        const GuestThread *parkWaker = currentThread();
        const uint32_t parkPc = parkWaker ? parkWaker->activeContext().pc : 0u;
        const uint32_t parkRa = parkWaker ? getRegU32(&parkWaker->activeContext(), 31) : 0u;
        ps2_park::tallySemaSignal(id, m_currentThreadId, parkPc, parkRa);
    }
    const bool semaDiag = diagSemaEnabled();
    const GuestThread *waker = semaDiag ? currentThread() : nullptr;
    const uint32_t wakerPc = waker ? waker->activeContext().pc : 0u;
    const uint32_t wakerRa = waker ? getRegU32(&waker->activeContext(), 31) : 0u;
    const size_t wakerInvDepth = waker ? waker->invocations.size() : 0u;
    const int wakerInvKind = (waker && !waker->invocations.empty())
                                 ? static_cast<int>(waker->invocations.back().kind)
                                 : -1;
    const uint64_t wakerInvTag = (waker && !waker->invocations.empty()) ? waker->invocations.back().tag : 0u;
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        if (semaDiag)
        {
            std::cerr << "[diag:sema] op=signal id=" << id << " count=-1->-1 waiters=0->0"
                      << " waker=" << m_currentThreadId << " pc=0x" << std::hex << wakerPc
                      << " ra=0x" << wakerRa << std::dec << " inInt=" << (m_insideInterrupt ? 1 : 0)
                      << " iSafe=" << (interruptSafe ? 1 : 0) << " invKind=" << wakerInvKind
                      << " invDepth=" << wakerInvDepth << " cbFunc=" << std::hex << wakerInvTag << std::dec
                      << " target=- tStatus=-1 tSusp=-1 result=" << KE_ERROR << std::endl;
        }
        char dropArgs[32];
        std::snprintf(dropArgs, sizeof(dropArgs), "id=%d", id);
        ps2_log::emitDrop("sched/signalSemaphore", "KE_UNKNOWN_SEMID", dropArgs);
        // P12: stock SignalSema returns plain -1 for unknown ids
        // (0x80004c04 b -> addiu v0,zero,-1; no -408 in KERNEL).
        return KE_ERROR;
    }
    const int countBefore = object->count;
    const size_t waitersBefore = object->waiters.size();
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        const int targetStatus = static_cast<int>(waiter->status);
        const int targetSusp = waiter->suspendCount;
        const int targetWaitReason = static_cast<int>(waiter->wait.reason);
        const int targetWaitId = waitObjectId(waiter->wait);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        if (semaDiag)
        {
            std::cerr << "[diag:sema] op=signal id=" << id << " count=" << countBefore << "->" << object->count
                      << " waiters=" << waitersBefore << "->" << object->waiters.size()
                      << " waker=" << m_currentThreadId << " pc=0x" << std::hex << wakerPc
                      << " ra=0x" << wakerRa << std::dec << " inInt=" << (m_insideInterrupt ? 1 : 0)
                      << " iSafe=" << (interruptSafe ? 1 : 0) << " invKind=" << wakerInvKind
                      << " invDepth=" << wakerInvDepth << " cbFunc=" << std::hex << wakerInvTag << std::dec
                      << " target=" << waiterId << " tStatus=" << targetStatus << " tSusp=" << targetSusp
                      << " tWaitReason=" << targetWaitReason << " tWaitId=" << targetWaitId
                      << " result=" << id << std::endl;
        }
        return id;
    }
    // P1aa (amends P1v per the P1z kernel disassembly): the EE signal path
    // has no OVF check, so waiter-less signals always ++count and return id.
    ++object->count;
    publishSnapshot();
    if (semaDiag)
    {
        std::cerr << "[diag:sema] op=signal id=" << id << " count=" << countBefore << "->" << object->count
                  << " waiters=" << waitersBefore << "->" << object->waiters.size()
                  << " waker=" << m_currentThreadId << " pc=0x" << std::hex << wakerPc
                  << " ra=0x" << wakerRa << std::dec << " inInt=" << (m_insideInterrupt ? 1 : 0)
                  << " iSafe=" << (interruptSafe ? 1 : 0) << " invKind=" << wakerInvKind
                  << " invDepth=" << wakerInvDepth << " cbFunc=" << std::hex << wakerInvTag << std::dec
                  << " target=- tStatus=-1 tSusp=-1 result=" << id << std::endl;
    }
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        char dropArgs[32];
        std::snprintf(dropArgs, sizeof(dropArgs), "id=%d", id);
        ps2_log::emitDrop("sched/pollSemaphore", "KE_UNKNOWN_SEMID", dropArgs);
        // P12: unknown ids miss with plain -1 too (PollSema 0x80004dc0:
        // invalid ids fall through to the same jr/addiu v0,zero,-1 pair
        // at 0x80004dcc; no -408 exists in KERNEL).
        return KE_ERROR;
    }
    // P10: the EE kernel misses with plain -1 (PollSema 0x80004dc0: blez at
    // 0x80004de4 falls back to the jr/addiu v0,zero,-1 pair; no -419 exists
    // in KERNEL), so return KE_ERROR, not KE_SEMA_ZERO. No [drop]: a miss is
    // a normal answer (P1w exclusion kept).
    if (object->count == 0)
    {
        return KE_ERROR;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    // T1: entry tally covers exactly the calls the [diag:sema] lines cover.
    {
        const GuestThread *parkSelf = currentThread();
        const uint32_t parkPc = parkSelf ? parkSelf->activeContext().pc : 0u;
        const uint32_t parkRa = parkSelf ? getRegU32(&parkSelf->activeContext(), 31) : 0u;
        ps2_park::tallySemaWait(id, m_currentThreadId, parkPc, parkRa);
    }
    const bool semaDiag = diagSemaEnabled();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        // P12: stock WaitSema returns plain -1 for unknown ids (0x80004d30
        // b -> addiu v0,zero,-1, passed through the wrapper which only
        // special-cases -2; no -408 in KERNEL). No park, like the kernel.
        setReturnS32(&self->activeContext(), KE_ERROR);
        if (semaDiag)
        {
            std::cerr << "[diag:sema] op=wait id=" << id << " count=-1->-1 parked=0"
                      << " waker=" << m_currentThreadId << " pc=0x" << std::hex << self->activeContext().pc
                      << " ra=0x" << getRegU32(&self->activeContext(), 31) << std::dec
                      << " inInt=" << (m_insideInterrupt ? 1 : 0)
                      << " result=" << KE_ERROR;
            if (diagSemaS0Enabled())
            {
                std::cerr << " s0=0x" << std::hex << getRegU32(&self->activeContext(), 16) << std::dec;
            }
            std::cerr << std::endl;
        }
        return;
    }
    if (object->count != 0)
    {
        const int countBefore = object->count;
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        if (semaDiag)
        {
            std::cerr << "[diag:sema] op=wait id=" << id << " count=" << countBefore << "->" << object->count
                      << " parked=0"
                      << " waker=" << m_currentThreadId << " pc=0x" << std::hex << self->activeContext().pc
                      << " ra=0x" << getRegU32(&self->activeContext(), 31) << std::dec
                      << " inInt=" << (m_insideInterrupt ? 1 : 0)
                      << " result=" << id << std::endl;
        }
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (semaDiag)
    {
        std::cerr << "[diag:sema] op=wait id=" << id << " count=0->0 parked=1"
                  << " waker=" << m_currentThreadId << " pc=0x" << std::hex << self->activeContext().pc
                  << " ra=0x" << getRegU32(&self->activeContext(), 31) << std::dec
                  << " inInt=" << (m_insideInterrupt ? 1 : 0)
                  << " waiters=" << object->waiters.size() << "->" << (object->waiters.size() + 1)
                  << " result=park" << std::endl;
    }
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        ps2_log::emitDrop("sched/createEventFlag", "KE_ERROR");
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        ps2_log::emitDrop("sched/deleteEventFlag", "KE_UNKNOWN_EVFID");
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        ps2_log::emitDrop("sched/setEventFlag", "KE_UNKNOWN_EVFID");
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        ps2_log::emitDrop("sched/clearEventFlag", "KE_UNKNOWN_EVFID");
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        ps2_log::emitDrop("sched/pollEventFlag", "KE_UNKNOWN_EVFID");
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        char dropArgs[64];
        std::snprintf(dropArgs, sizeof(dropArgs), "handler=0x%x ticks=%u", handler, ticks);
        ps2_log::emitDrop("sched/setAlarm", "KE_ERROR", dropArgs);
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        ps2_log::emitDrop("sched/setAlarm", "KE_ERROR", "id-exhausted");
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        char dropArgs[32];
        std::snprintf(dropArgs, sizeof(dropArgs), "id=%d", id);
        ps2_log::emitDrop("sched/cancelAlarm", "KE_ERROR", dropArgs);
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    ps2_e15::selection(m_vsyncTick,"queue",invocation.context.pc,&invocation.context,m_currentThreadId);
    invocation.sequence = ++m_invocationSequence;
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

void EeScheduler::startSoundClock()
{
    assertExecutor();
    if (m_soundClockStarted || !ps2_snd_spike::enabled())
        return;
    m_soundClockStarted = true;
    const uint64_t deadline = m_eeCycle + ps2_snd_spike::kTickCycles;
    scheduleEvent(deadline, std::chrono::steady_clock::now(),
                  EeEvent{EeEventType::SoundTick, 0u, deadline});
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    ps2_e15::selection(m_vsyncTick,"direct",invocation.context.pc,&invocation.context,m_currentThreadId);
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u)
    {
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        ps2_e15::selection(m_vsyncTick,"sequence",it->context.pc,&it->context,m_currentThreadId);
        if (getRegU32(&it->context, 29) == 0u)
        {
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    constexpr uint32_t kInvocationStackSize = 0x4000u;
    const uint32_t top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        char dropArgs[64];
        std::snprintf(dropArgs, sizeof(dropArgs), "dmac=%d cause=%u handler=0x%x id-exhausted",
                      dmac ? 1 : 0, cause, handler);
        ps2_log::emitDrop("sched/addIrqHandler", "KE_ERROR", dropArgs);
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    else
    {
        // P1w: remove of an unknown id/cause is ignored yet reports KE_OK.
        char dropArgs[64];
        std::snprintf(dropArgs, sizeof(dropArgs), "id=%d cause=%u dmac=%d ret=KE_OK",
                      id, cause, dmac ? 1 : 0);
        ps2_log::emitDrop("sched/removeIrqHandler", "unknown-id", dropArgs);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    else
    {
        // P1w: enable/disable of an unknown id is ignored yet reports KE_OK.
        char dropArgs[64];
        std::snprintf(dropArgs, sizeof(dropArgs), "id=%d dmac=%d enabled=%d ret=KE_OK",
                      id, dmac ? 1 : 0, enabled ? 1 : 0);
        ps2_log::emitDrop("sched/setIrqHandlerEnabled", "unknown-id", dropArgs);
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            mask |= 1u << cause;
        }
        else
        {
            mask &= ~(1u << cause);
        }
    }
    else
    {
        // P1w: out-of-range cause is ignored yet reported KE_OK. Trace it.
        char dropArgs[64];
        std::snprintf(dropArgs, sizeof(dropArgs), "cause=%u dmac=%d enabled=%d ret=KE_OK",
                      cause, dmac ? 1 : 0, enabled ? 1 : 0);
        ps2_log::emitDrop("sched/setIrqCauseEnabled", "cause-range", dropArgs);
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
        else if (handler.enabled && handler.cause == cause && handler.handler != 0u)
        {
            // P1w: enabled handler for a firing cause, but no table entry
            // (registration does not validate). The IRQ is dropped for it.
            char dropArgs[64];
            std::snprintf(dropArgs, sizeof(dropArgs), "handler=0x%x cause=%u id=%d",
                          handler.handler, cause, handler.id);
            ps2_log::emitDrop("sched/dispatchIrq", "no-table-entry", dropArgs);
        }
        else if (handler.enabled && handler.cause == cause)
        {
            // P1w: enabled handler for a firing cause with a null entry.
            char dropArgs[64];
            std::snprintf(dropArgs, sizeof(dropArgs), "cause=%u id=%d", cause, handler.id);
            ps2_log::emitDrop("sched/dispatchIrq", "null-handler", dropArgs);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    for (const EeIrqHandler &handler : matching)
    {
        // E40 Part-6: log every queued guest handler dispatch (T51 mirror).
        ps2_mpg_src_trace::noteIrq(m_vsyncTick, dmac, cause, handler.handler);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        // P1f: never run a handler on its registration-time thread sp (that
        // stack belongs to a live guest thread; the 0x3e4db8 prologue stored
        // zeros over thread 1's ra slot at 0x1ffff00). sp=0 makes the run()
        // dequeue path assign invocationStackTop() from the reserved region.
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            if (ps2DiagWatchEnabled())
            {
                uint32_t watchPc = 0u;
                uint32_t watchRa = 0u;
                uint32_t watchSp = 0u;
                if (const GuestThread *owner = currentThread())
                {
                    const R5900Context &actx = owner->activeContext();
                    watchPc = actx.pc;
                    watchRa = getRegU32(&actx, 31);
                    watchSp = getRegU32(&actx, 29);
                }
                ps2DiagWatchReportDirect(tickAddress, 8u, 0u, 0u, watchPc, m_currentThreadId, watchRa, watchSp);
                // E3b R2-direct row (pre-memcpy old value; legacy line above stays verbatim).
                if (ps2_e3::armed() && ps2_e3::storeOverlaps(tickAddress, 8u))
                {
                    uint64_t e3oldLo = 0u;
                    uint64_t e3oldHi = 0u;
                    ps2_e3::readOld(m_rdram, tickAddress, 8u, e3oldLo, e3oldHi);
                    ps2_e3::emitR2(2u, tickAddress, 8u, e3oldLo, e3oldHi, 0u, 0u, watchPc, m_currentThreadId,
                                   watchRa, watchSp);
                }
            }
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    if (ps2_e15::enabled() && type==1u)
        ps2_e7::event(m_vsyncTick,"mpeg-complete","type=%u token=0x%llx result=%d matched=%zu thread=%d",
            type,static_cast<unsigned long long>(token),result,completed.size(),m_currentThreadId);
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    if (ps2_e15::enabled() && reason==EeWaitReason::Mpeg)
    {
        const auto *c=currentContext();
        ps2_e7::event(m_vsyncTick,"mpeg-wait","type=%u token=0x%llx thread=%d pc=0x%x ra=0x%x sp=0x%x reason=%u",
            type,static_cast<unsigned long long>(token),m_currentThreadId,c?c->pc:0u,
            c?getRegU32(c,31):0u,c?getRegU32(c,29):0u,static_cast<unsigned>(reason));
    }
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot()
{
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        snapshot.pc = item.activeContext().pc;
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    renewTimeSlice();
}

void EeScheduler::makeDormant(GuestThread &item)
{
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    processDueDeadlines();
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = std::chrono::steady_clock::now();
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.deadlineCycle <= m_eeCycle &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                }
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            if (now < pacingDeadline)
            {
                m_eventCv.wait_until(lock, pacingDeadline, [this]()
                                     { return !m_events.empty() ||
                                              m_stopRequested.load(std::memory_order_acquire); });
                if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
                {
                    updateNextDeadline();
                    return;
                }
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle &&
                                                       item.hostDeadline <= pacedNow; });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                scheduleEvent(scheduled.deadlineCycle + kVBlankDurationCycles,
                              scheduled.hostDeadline + kVBlankDuration,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(scheduled.deadlineCycle + kVBlankPeriodCycles,
                              scheduled.hostDeadline + kVBlankPeriod,
                              EeEvent{EeEventType::VBlankStart, 0, 0});
            }
            processEvent(scheduled.event);
        }
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        ++m_vsyncTick;
        ps2_e3::noteVBlank(m_vsyncTick); // E3b frame stamp
        ps2_gfx_stats::noteVsync(m_vsyncTick); // E33 per-vsync census cut
        ps2_vu1_trace::noteVsync(m_vsyncTick); // E36 per-program trace window
        ps2_vu1_entry_trace::noteVsync(m_vsyncTick); // E37 entry trace window
        ps2_e41_trace::noteVsync(m_vsyncTick); // E41 cdread/plant vsync mirror
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        ps2_e4::noteVBlank(m_vsyncTick, m_runtime.gs(), m_runtime.memory().gs()); // E4 arm/freeze
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                // E3b R3d VBlank-tick tap (BYPASS path; the flag write above
                // FIRES via writeGuestU32 and is covered by R2-direct).
                ps2_e3::Tap e3tick = ps2_e3::tapBegin(m_rdram, m_vsyncTickAddress, sizeof(uint64_t));
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
                if (e3tick.active)
                {
                    char e3x[64];
                    std::snprintf(e3x, sizeof(e3x), "tick=%llu", static_cast<unsigned long long>(m_vsyncTick));
                    ps2_e3::tapEnd(std::move(e3tick), "vblank-tick", m_rdram, e3x);
                }
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        completeVSync(m_vsyncTick);
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        else if (m_gsVSyncCallback != 0u)
        {
            // P1w: a registered vsync callback with no table entry is
            // silently skipped every vsync. Trace the discard.
            char dropArgs[48];
            std::snprintf(dropArgs, sizeof(dropArgs), "cb=0x%x tick=%llu",
                          m_gsVSyncCallback, (unsigned long long)m_vsyncTick);
            ps2_log::emitDrop("sched/vsync-callback", "no-table-entry", dropArgs);
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        break;
    case EeEventType::SoundTick:
        ps2_snd_spike::onSoundTick(m_rdram, event.value, [this](GuestInvocation invocation)
                                   { queueInvocation(std::move(invocation)); });
        {
            const uint64_t deadline = event.value + ps2_snd_spike::kTickCycles;
            scheduleEvent(deadline, std::chrono::steady_clock::now(),
                          EeEvent{EeEventType::SoundTick, 0u, deadline});
        }
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        // P1f: same as dispatchIrq above; the SetAlarm caller sp belongs to
        // a live guest thread, so take sp from the reserved region instead.
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        // No drop line here: None/Sleep/VSync/External/Mpeg waits carry no
        // object id by design, so 0 is the correct answer, not a discard.
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    if (!m_rdram)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (physical > PS2_RAM_SIZE - sizeof(value))
    {
        // P1w: out-of-range result write is discarded. Trace it.
        char dropArgs[64];
        std::snprintf(dropArgs, sizeof(dropArgs), "addr=0x%x value=0x%x", address, value);
        ps2_log::emitDrop("sched/writeGuestU32", "addr-range", dropArgs);
        return;
    }
    if (ps2DiagWatchEnabled())
    {
        uint32_t watchPc = 0u;
        uint32_t watchRa = 0u;
        uint32_t watchSp = 0u;
        if (const GuestThread *owner = currentThread())
        {
            const R5900Context &actx = owner->activeContext();
            watchPc = actx.pc;
            watchRa = getRegU32(&actx, 31);
            watchSp = getRegU32(&actx, 29);
        }
        ps2DiagWatchReportDirect(address, 4u, value, 0u, watchPc, m_currentThreadId, watchRa, watchSp);
        // E3b R2-direct row (pre-memcpy old value; legacy line above stays verbatim).
        if (ps2_e3::armed() && ps2_e3::storeOverlaps(address, 4u))
        {
            uint64_t e3oldLo = 0u;
            uint64_t e3oldHi = 0u;
            ps2_e3::readOld(m_rdram, address, 4u, e3oldLo, e3oldHi);
            ps2_e3::emitR2(2u, address, 4u, e3oldLo, e3oldHi, value, 0u, watchPc, m_currentThreadId, watchRa,
                           watchSp);
        }
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
    }
    if (hasTimerDeadline)
    {
        const auto timerHostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(timerCycles);
        if (timerHostDeadline < hostDeadline)
        {
            deadlineCycle = m_eeCycle + timerCycles;
            hostDeadline = timerHostDeadline;
        }
    }

    const bool signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                               { return !m_events.empty() ||
                                                        m_stopRequested.load(std::memory_order_acquire); });
    if (!signaled)
    {
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}
