#pragma once

#include "ps2_runtime.h"
#include "ps2_vsync_pacer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <setjmp.h>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

// GT3: POSIX hosts arm the transfer jump with sigsetjmp (see m_transferJmp).
#if defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__)
#define PS2X_EE_SIGJMP 1
#else
#define PS2X_EE_SIGJMP 0
#endif

// This exception is the EE equivalent of a longjmp to the dispatcher.  It is
// not an error and must only be caught at EeScheduler::run().
struct EeDispatcherTransfer final
{
};

enum class EeThreadStatus : uint8_t
{
    Running,
    Ready,
    Waiting,
    WaitingSuspended,
    Suspended,
    Dormant,
};

enum class EeWaitReason : uint8_t
{
    None,
    Sleep,
    Semaphore,
    EventFlag,
    VSync,
    External,
    Mpeg,
};

struct EeSemaphoreWait
{
    int id = 0;
};

struct EeEventFlagWait
{
    int id = 0;
    uint32_t bits = 0;
    uint32_t mode = 0;
    uint32_t resultAddress = 0;
};

struct EeVSyncWait
{
    uint64_t afterTick = 0;
    int fixedResult = -1;
};

struct EeExternalWait
{
    uint32_t type = 0;
    uint64_t token = 0;
};

using EeWaitPayload = std::variant<std::monostate,
                                   EeSemaphoreWait,
                                   EeEventFlagWait,
                                   EeVSyncWait,
                                   EeExternalWait>;

// SS1 save states: names a completion closure so a restored run can rebuild
// it (ps2_savestate::registerCompletionFactory). kind 0 = untagged, which
// defers a save while that closure is live.
struct EeCompletionTag
{
    uint32_t kind = 0;
    uint32_t args[4] = {};
};

struct EeWaitState
{
    EeWaitReason reason = EeWaitReason::None;
    EeWaitPayload payload{};
    std::function<void(R5900Context &)> completion;
    EeCompletionTag tag{};
};

enum class GuestInvocationKind : uint8_t
{
    Interrupt,
    Alarm,
    GsCallback,
    RpcCallback,
    SyscallOverride,
    ExitHandler,
    HleCall,
    SifCommand,
};

struct GuestInvocation
{
    GuestInvocationKind kind = GuestInvocationKind::Interrupt;
    uint64_t sequence = 0;
    uint64_t tag = 0;
    R5900Context context{};
    std::function<void(const R5900Context &, R5900Context &)> onComplete;
};

struct GuestThread
{
    int id = 0;
    R5900Context context{};
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    uint32_t arg = 0;
    int initialPriority = 0;
    int currentPriority = 0;
    EeThreadStatus status = EeThreadStatus::Dormant;
    int suspendCount = 0;
    uint32_t wakeupCount = 0;
    bool ownsStack = false;
    uint32_t tlsBase = 0;
    EeWaitState wait{};
    std::function<void(R5900Context &)> resumeCompletion;
    EeCompletionTag resumeTag{};
    std::vector<GuestInvocation> invocations;

    [[nodiscard]] R5900Context &activeContext()
    {
        return invocations.empty() ? context : invocations.back().context;
    }

    [[nodiscard]] const R5900Context &activeContext() const
    {
        return invocations.empty() ? context : invocations.back().context;
    }
};

struct EeSemaphore
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    int initCount = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    std::deque<int> waiters;
};

struct EeEventFlag
{
    int id = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    uint32_t initBits = 0;
    uint32_t bits = 0;
    std::deque<int> waiters;
};

struct EeAlarm
{
    int id = 0;
    uint16_t ticks = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
};

struct EeIrqHandler
{
    int id = 0;
    uint32_t cause = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
    bool enabled = true;
    int order = 0;
};

struct EeThreadSnapshot
{
    int id = 0;
    uint32_t pc = 0;
    uint32_t ra = 0;
    uint32_t sp = 0;
    uint32_t contextGp = 0;
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    int initialPriority = 0;
    int currentPriority = 0;
    EeThreadStatus status = EeThreadStatus::Dormant;
    EeWaitReason waitReason = EeWaitReason::None;
    int waitId = 0;
    int suspendCount = 0;
    uint32_t wakeupCount = 0;
    uint32_t invocationDepth = 0;
};

struct EeSemaphoreSnapshot
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    uint32_t waiters = 0;
};

struct EeEventFlagSnapshot
{
    int id = 0;
    uint32_t bits = 0;
    uint32_t initBits = 0;
    uint32_t attr = 0;
    uint32_t waiters = 0;
};

struct EeKernelSnapshot
{
    uint64_t sequence = 0;
    uint64_t eeCycle = 0;
    uint64_t sliceEndCycle = 0;
    uint64_t nextEventCycle = 0;
    int runningThreadId = 0;
    std::vector<EeThreadSnapshot> threads;
    std::vector<EeSemaphoreSnapshot> semaphores;
    std::vector<EeEventFlagSnapshot> eventFlags;
};

enum class EeEventType : uint8_t
{
    Stop,
    VBlankStart,
    VBlankEnd,
    Dmac,
    ExternalWake,
    Alarm,
    SoundTick,
};

struct EeEvent
{
    EeEventType type = EeEventType::ExternalWake;
    uint32_t id = 0;
    uint64_t value = 0;
};

struct EeThreadCreateParams
{
    uint32_t attr = 0;
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    int priority = 0;
    uint32_t option = 0;
};

class EeScheduler
{
    friend struct EeSchedulerTestAccess;
    friend struct EeSchedulerSavestate;
public:
    static constexpr int kMainThreadId = 1;
    static constexpr int kFirstThreadId = 2;
    static constexpr int kLastThreadId = 255;
    static constexpr int kPriorityCount = 128;
    static constexpr uint64_t kEeClockHz = 294912000ull;
    static constexpr uint32_t kGeneratedCheckpointCycles = 32u;
    static constexpr uint32_t kGuestDispatchCycles = 8u;
    static constexpr uint64_t kDefaultTimeSliceCycles = 65536ull;

    // CTX1: sigsetjmp mask-save flag for PS2X_EE_SWITCH (run() reads it once).
    // fast (default, also unset/unknown) = 0: no signal-mask save. sigmask =
    // GT3's platform setjmp semantics (1 on Apple/bionic, 0 on glibc).
    static int transferSaveMaskFor(const char *mode);

    explicit EeScheduler(PS2Runtime &runtime);
    ~EeScheduler();

    EeScheduler(const EeScheduler &) = delete;
    EeScheduler &operator=(const EeScheduler &) = delete;

    void reset(uint8_t *rdram, const R5900Context &mainContext);
    void run();
    void requestStop();
    void postEvent(EeEvent event);
    // EX1: exact checkpoint fast path (RV17 Stage E). The no-event path
    // charges, publishes Count through the cached running thread, applies
    // the EE1 timer deferral and polls pending/stop/deadline/slice —
    // identical decisions and mutations to the legacy outline path. Any
    // slow condition (uncached thread, timer service due, slice expiry)
    // delegates to outline helpers; the first two bail with pristine
    // state so checkpointDueFull is bit-for-bit the legacy sequence.
    [[nodiscard]] bool checkpointDue(uint32_t cycles = kGeneratedCheckpointCycles) noexcept
    {
        // FH1: full120's EE clock multiplier charges executed code 1/EE_X
        // (shift 0 when the knob is off). Idle advances stay exact.
        cycles >>= m_eeClockShift;
        const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
        // A cached nullptr is a valid "no current thread" entry; only the
        // id+generation decide validity, so a threadless stretch stays on
        // the fast path after one Full refresh.
        if (m_runningThreadId != m_currentThreadId ||
            m_runningThreadGen != m_threadContainerGen)
        {
            return checkpointDueFull(cycles);
        }
        GuestThread *running = m_runningThread;
        if (m_runtime.memory().eeTimersFastWouldFire(elapsed))
        {
            return checkpointDueFull(cycles);
        }
        m_eeCycle += elapsed;
        if (running != nullptr)
        {
            running->activeContext().cop0_count =
                m_count + static_cast<uint32_t>(m_eeCycle - m_countCycle);
        }
        m_runtime.memory().eeTimersFastApply(elapsed);
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
        return checkpointSliceExpired(running);
    }
    void accountCycles(uint32_t cycles) noexcept;
    // Executor-only shared COP0 Count clock. A read charges at least one tick.
    uint32_t readCount(R5900Context *ctx) noexcept;
    void writeCount(R5900Context *ctx, uint32_t value) noexcept;
    [[nodiscard]] bool isExecutingGuest() const noexcept;

    // Kernel object API. All calls except postEvent/requestStop execute on the
    // EE executor and therefore need no host synchronization.
    void setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp);
    int createThread(const EeThreadCreateParams &params);
    int deleteThread(int id, uint32_t &ownedStack);
    int startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe);
    [[noreturn]] void exitCurrent(bool deleteThread);
    int terminateThread(int id, uint32_t &ownedStack, bool interruptSafe);
    int suspendThread(int id, bool interruptSafe);
    int resumeThread(int id, bool interruptSafe);
    void sleepCurrent();
    int wakeupThread(int id, bool interruptSafe);
    int cancelWakeup(int id);
    int changePriority(int id, int priority, bool interruptSafe, int &oldPriority);
    int rotateReadyQueue(int priority, bool interruptSafe);
    int releaseWait(int id, bool interruptSafe);
    void transferIfRequested(bool interruptSafe);

    int createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option);
    int deleteSemaphore(int id, bool interruptSafe);
    int signalSemaphore(int id, bool interruptSafe);
    int pollSemaphore(int id);
    void waitSemaphore(int id);

    int createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option);
    int deleteEventFlag(int id, bool interruptSafe);
    int setEventFlag(int id, uint32_t bits, bool interruptSafe);
    int clearEventFlag(int id, uint32_t mask);
    int pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits);
    void waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress);

    int setAlarm(uint16_t ticks, uint32_t handler, uint32_t argument, uint32_t gp, uint32_t sp);
    int cancelAlarm(int id);
    void queueInvocation(GuestInvocation invocation);
    void startSoundClock();
    [[noreturn]] void invokeCurrent(GuestInvocation invocation);
    [[noreturn]] void invokeCurrentSequence(std::vector<GuestInvocation> invocations);
    [[nodiscard]] bool hasInvocation(GuestInvocationKind kind, uint64_t tag) const;
    [[nodiscard]] uint32_t invocationStackTop();

    int addIrqHandler(bool dmac,
                      uint32_t cause,
                      uint32_t handler,
                      bool append,
                      uint32_t argument,
                      uint32_t gp,
                      uint32_t sp);
    int removeIrqHandler(bool dmac, uint32_t cause, int id);
    int setIrqHandlerEnabled(bool dmac, int id, bool enabled);
    int setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled);
    void dispatchIrq(bool dmac, uint32_t cause);
    void setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress);
    [[nodiscard]] uint64_t currentVSyncTick() const noexcept;
    [[nodiscard]] uint64_t currentEeCycle() const noexcept;
    // E40 Part-6: true while the executor runs a slice of an
    // Interrupt-kind invocation (guest IRQ handler). Read on the
    // executor thread only, like the existing internal uses.
    [[nodiscard]] bool insideInterrupt() const noexcept { return m_insideInterrupt; }
    uint32_t setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp);

    [[noreturn]] void waitVSync(uint64_t afterTick, int fixedResult = -1, std::function<void(R5900Context &)> completion = {});
    // SS1: same, with a tag that lets a save state rebuild the completion.
    [[noreturn]] void waitVSyncTagged(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion,
                                      EeCompletionTag tag);
    void completeVSync(uint64_t tick);
    void completeExternalWait(uint32_t type, uint64_t token, int result);
    [[noreturn]] void waitExternal(EeWaitReason reason, uint32_t type, uint64_t token, std::function<void(R5900Context &)> completion = {});

    [[nodiscard]] GuestThread *thread(int id);
    [[nodiscard]] const GuestThread *thread(int id) const;
    [[nodiscard]] EeSemaphore *semaphore(int id);
    [[nodiscard]] const EeSemaphore *semaphore(int id) const;
    [[nodiscard]] EeEventFlag *eventFlag(int id);
    [[nodiscard]] const EeEventFlag *eventFlag(int id) const;
    [[nodiscard]] GuestThread *currentThread();
    [[nodiscard]] const GuestThread *currentThread() const;
    [[nodiscard]] int currentThreadId() const noexcept;
    [[nodiscard]] R5900Context *currentContext();
    [[nodiscard]] uint8_t *rdram() const noexcept;

    // Direct syscall tests use the same main-thread record without starting a
    // second executor. Production execution calls reset() before run().
    void bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram);

    [[nodiscard]] EeKernelSnapshot snapshot() const;
    void publishSnapshot();

private:
    struct ScheduledEvent
    {
        uint64_t deadlineCycle = 0;
        std::chrono::steady_clock::time_point hostDeadline{};
        EeEvent event{};
        uint64_t sequence = 0;
    };

    void assertExecutor() const;
    [[nodiscard]] int allocateThreadId();
    GuestThread &acquireInvocationThread();
    void enqueueReady(GuestThread &thread, bool front = false);
    void removeReady(GuestThread &thread);
    [[nodiscard]] GuestThread *selectReady();
    void makeRunning(GuestThread &thread);
    void makeDormant(GuestThread &thread);
    void removeFromWaitObject(GuestThread &thread);
    // CP4: transfer out of guest code. Longjmps to run() when a guest call
    // is armed, else throws EeDispatcherTransfer (host-side callers, tests).
    [[noreturn]] void raiseTransfer();
    [[noreturn]] void blockCurrent(EeWaitState wait);
    void makeReady(GuestThread &thread, int result, bool interruptSafe);
    void requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe);
    void applyPendingPreemption();
    void processPendingEvents();
    void processDueDeadlines();
    void processEvent(const EeEvent &event);
#if PS2X_ENABLE_DET_HASH_TAP
    struct DetHashSnapshot
    {
        uint64_t rdram = 0, scratchpad = 0, vu1Data = 0, vu1Code = 0;
        uint64_t combined = 0, count = 0;
        bool valid = false;
    };
    static uint64_t parseDetHashEvery(const char *value, bool &invalid);
    DetHashSnapshot makeDetHashSnapshot() const;
    void emitDetHashTap();
    static std::atomic<uint32_t> s_detHashLines;
#endif
    void finishEventWaiters(EeEventFlag &flag, bool interruptSafe);
    [[nodiscard]] static bool eventCondition(uint32_t current, uint32_t requested, uint32_t mode);
    static int waitObjectId(const EeWaitState &wait);
    void writeGuestU32(uint32_t address, uint32_t value);
    void waitForEvent();
    // PT2: cut the per-tick GameThread tail entries (ee.busy/ee.cpu/ee.wait)
    // at a VBlank. Consumes m_perfGateNs/m_perfPaceNs/m_perfEventNs/m_perfEnqueueNs.
    void perfTailCutFrame(uint64_t tick);
    void scheduleEvent(uint64_t deadlineCycle, std::chrono::steady_clock::time_point hostDeadline, EeEvent event);
    void updateNextDeadline();
    [[nodiscard]] bool hasReadyAtOrAbovePriority(int priority) const;
    void renewTimeSlice();
    // EX1: outline checkpoint tails. checkpointDueFull is the legacy
    // checkpointDue sequence verbatim (plus running-thread cache refresh);
    // checkpointSliceExpired is its slice-expiry arm. Both run cold.
    bool checkpointDueFull(uint32_t cycles) noexcept;
    bool checkpointSliceExpired(GuestThread *running);
    void noteThreadContainerMutated() noexcept { ++m_threadContainerGen; }
    void copyMainContextToRuntime();
    void publishDebugContext(const R5900Context &context);
    void publishIdleDebugContext();

    PS2Runtime &m_runtime;
    // Benchmark mode: scheduled events and idle advancement use guest cycles.
    // ExternalWake carries no guest-cycle timestamp and is outside this
    // scheduled-event ordering guarantee.
    const bool m_eventClockCycles;
    const bool m_cycleOnlyEvents;
#if PS2X_ENABLE_DET_HASH_TAP
    uint64_t m_detHashEvery = 0;
#endif
    uint8_t *m_rdram = nullptr;
    std::array<std::deque<int>, kPriorityCount> m_readyQueues{};
    std::unordered_map<int, GuestThread> m_threads;
    std::unordered_map<int, EeSemaphore> m_semaphores;
    std::unordered_map<int, EeEventFlag> m_eventFlags;
    std::unordered_map<int, EeAlarm> m_alarms;
    std::unordered_map<int, EeIrqHandler> m_intcHandlers;
    std::unordered_map<int, EeIrqHandler> m_dmacHandlers;
    int m_nextThreadId = kFirstThreadId;
    int m_nextInvocationThreadId = -1;
    int m_nextSemaphoreId = 1;
    int m_nextEventFlagId = 1;
    int m_nextAlarmId = 1;
    int m_nextIntcHandlerId = 1;
    int m_nextDmacHandlerId = 1;
    int m_intcHeadOrder = 0;
    int m_intcTailOrder = 1000;
    int m_dmacHeadOrder = 0;
    int m_dmacTailOrder = 1000;
    uint32_t m_enabledIntcMask = 0xFFFFFFFFu;
    uint32_t m_enabledDmacMask = 0xFFFFFFFFu;
    int m_currentThreadId = 0;
    // EX1: cached running thread for the checkpoint fast path. (ptr, id)
    // are valid only when id == m_currentThreadId and gen ==
    // m_threadContainerGen; ids are ring-reused, so the generation (bumped
    // on every m_threads mutation — clear/emplace/erase — plus savestate
    // load) guards against ABA. Transient: never serialized; reset and
    // load invalidate it. Executor thread only, like the container.
    GuestThread *m_runningThread = nullptr;
    int m_runningThreadId = -1;
    uint64_t m_runningThreadGen = 0;
    uint64_t m_threadContainerGen = 0;
    bool m_rescheduleRequested = false;
    bool m_timeSliceExpired = false;
    bool m_insideInterrupt = false;
    bool m_soundClockStarted = false;
    uint32_t m_pendingEeTimerInterrupts = 0;
    uint64_t m_eeCycle = 0;
    uint32_t m_eeClockShift = 0; // FH1 (ps2_fh1::eeClockShift)
    uint64_t m_countCycle = 0;
    uint32_t m_count = 0;
    uint64_t m_sliceEndCycle = kDefaultTimeSliceCycles;
    std::thread::id m_executorThread{};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_guestExecuting{false};
    // CP4: non-exceptional transfer out of guest code (executor thread only,
    // set by run() around each guest call; run() is non-reentrant).
    // GT3: sigsetjmp with a run()-time mask-save flag that keeps each
    // platform's setjmp semantics (bionic/Apple save the mask, glibc doesn't).
    // CTX1: PS2X_EE_SWITCH=fast (default) clears it (transferSaveMaskFor).
#if PS2X_EE_SIGJMP
    sigjmp_buf m_transferJmp{};
#else
    std::jmp_buf m_transferJmp{};
#endif
    int m_transferSaveMask = 1;
    bool m_transferArmed = false;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_checkpointPending{false};
    uint32_t m_debugPublishCountdown = 0u;

    mutable std::mutex m_eventMutex;
    std::condition_variable m_eventCv;
    std::deque<EeEvent> m_events;
    std::vector<ScheduledEvent> m_deadlines;
    std::deque<GuestInvocation> m_pendingInvocations;
    uint64_t m_eventSequence = 0;
    uint64_t m_invocationSequence = 0;
    uint64_t m_vsyncTick = 0;
    // PT2: per-tick GameThread tail (PS2X_PERF_LOG=1; knob off is one bool
    // check per VBlank plus one per waitForEvent wait). Executor only.
    bool m_perfTail = false;
    bool m_perfHaveFrame = false;
    uint64_t m_perfFrameStartWall = 0;
    uint64_t m_perfFrameStartCpu = 0;
    uint64_t m_perfEventNs = 0;   // waitForEvent waits since the last cut
    uint64_t m_perfEnqueueNs = 0; // GS enqueue-queue-full waits since the last cut (PW2: split out)
    uint64_t m_perfMtvuNs = 0;  // threadedWaitNsTotal() at the last cut
    uint64_t m_perfMtvuVbNs = 0; // IP7: VBlank-reason part of it at the last cut
    uint64_t m_perfGateNs = 0;  // this VBlank's pause-gate sleep
    uint64_t m_perfPaceNs = 0;  // this VBlank's pacer sleep
    // FP1: wall-clock guest-vsync pacer (executor thread only). Default on;
    // PS2X_UNPACED=1 disables. Sleeps only; guest state untouched.
    ps2_vsync_pacer::Pacer m_vsyncPacer{};
    bool m_vsyncLocked = false; // PX1: last VBlank was paced by ps2_vsync_lock
    ps2_vsync_pacer::HostPaceConfig m_hostPace{};
    bool m_vsyncPace = true;
    uint32_t m_vsyncFlagAddress = 0;
    uint32_t m_vsyncTickAddress = 0;
    uint32_t m_gsVSyncCallback = 0;
    uint32_t m_gsVSyncCallbackGp = 0;
    uint32_t m_gsVSyncCallbackSp = 0;
    std::unordered_map<uint64_t, uint32_t> m_invocationStackTops;
    std::atomic<uint64_t> m_nextDeadlineCycle{0};

    mutable std::mutex m_snapshotMutex;
    EeKernelSnapshot m_snapshot;
    uint64_t m_snapshotSequence = 0;
};

// DSP1: inline eeCheckpointDue and the unwind mirror (needs EeScheduler complete).
#include "runtime/ee_dispatch_fast.h"
