// SS1 (DEV, default off): EeScheduler save-state section. Taken at the
// dispatcher (no guest frame on the host stack), right after an event pass.

#include "runtime/ee_scheduler.h"
#include "runtime/ps2_savestate.h"
#include "../ps2_savestate_internal.h"

#include <string>
#include <typeinfo>

using namespace ps2_savestate;

namespace
{
    void writeTag(Writer &w, const EeCompletionTag &tag)
    {
        w.u32(tag.kind);
        for (uint32_t a : tag.args)
            w.u32(a);
    }
    EeCompletionTag readTag(Reader &r)
    {
        EeCompletionTag tag{};
        tag.kind = r.u32();
        for (uint32_t &a : tag.args)
            a = r.u32();
        return tag;
    }

    template <typename Fn>
    std::string closureName(const Fn &fn)
    {
        return fn ? std::string(fn.target_type().name()) : std::string("none");
    }

    void writeWait(Writer &w, const EeWaitState &wait)
    {
        w.u8(static_cast<uint8_t>(wait.reason));
        w.u8(static_cast<uint8_t>(wait.payload.index()));
        switch (wait.payload.index())
        {
        case 1:
            w.pod(std::get<EeSemaphoreWait>(wait.payload));
            break;
        case 2:
            w.pod(std::get<EeEventFlagWait>(wait.payload));
            break;
        case 3:
            w.pod(std::get<EeVSyncWait>(wait.payload));
            break;
        case 4:
            w.pod(std::get<EeExternalWait>(wait.payload));
            break;
        default:
            break;
        }
        // A completion is saved as its tag (ready() refuses untagged ones).
        w.b(static_cast<bool>(wait.completion));
        writeTag(w, wait.completion ? wait.tag : EeCompletionTag{});
    }

    bool rebuild(Reader &r, const EeCompletionTag &tag, PS2Runtime &runtime, std::function<void(R5900Context &)> &out)
    {
        CompletionFactory factory = completionFactory(tag.kind);
        if (!factory)
            return r.fail("no completion factory for kind " + std::to_string(tag.kind));
        out = factory(tag.args, &runtime);
        return static_cast<bool>(out);
    }

    bool readWait(Reader &r, EeWaitState &wait, PS2Runtime &runtime)
    {
        wait = {};
        wait.reason = static_cast<EeWaitReason>(r.u8());
        switch (r.u8())
        {
        case 0:
            break;
        case 1:
        {
            EeSemaphoreWait v{};
            r.pod(v);
            wait.payload = v;
            break;
        }
        case 2:
        {
            EeEventFlagWait v{};
            r.pod(v);
            wait.payload = v;
            break;
        }
        case 3:
        {
            EeVSyncWait v{};
            r.pod(v);
            wait.payload = v;
            break;
        }
        case 4:
        {
            EeExternalWait v{};
            r.pod(v);
            wait.payload = v;
            break;
        }
        default:
            return r.fail("bad wait payload index");
        }
        const bool hasCompletion = r.b();
        wait.tag = readTag(r);
        if (hasCompletion)
            return rebuild(r, wait.tag, runtime, wait.completion);
        return r.ok();
    }

    void writeInvocation(Writer &w, const GuestInvocation &inv)
    {
        w.u8(static_cast<uint8_t>(inv.kind));
        w.u64(inv.sequence);
        w.u64(inv.tag);
        w.pod(inv.context);
    }
    void readInvocation(Reader &r, GuestInvocation &inv)
    {
        inv = {};
        inv.kind = static_cast<GuestInvocationKind>(r.u8());
        inv.sequence = r.u64();
        inv.tag = r.u64();
        r.pod(inv.context);
    }

    void writeIntDeque(Writer &w, const std::deque<int> &d) { writeDequePod(w, d); }
    bool readIntDeque(Reader &r, std::deque<int> &d) { return readDequePod(r, d); }
} // namespace

std::string EeSchedulerSavestate::ready(const EeScheduler &s)
{
    {
        std::lock_guard lock(s.m_eventMutex);
        if (!s.m_events.empty())
            return "host events queued";
        for (const GuestInvocation &inv : s.m_pendingInvocations)
            if (inv.onComplete)
                return "pending invocation with onComplete (" + closureName(inv.onComplete) + ")";
    }
    for (const auto &[id, t] : s.m_threads)
    {
        if (t.wait.completion && (t.wait.tag.kind == kCompletionNone || !completionFactory(t.wait.tag.kind)))
            return "thread " + std::to_string(id) + " untagged wait completion (" + closureName(t.wait.completion) + ")";
        if (t.resumeCompletion && (t.resumeTag.kind == kCompletionNone || !completionFactory(t.resumeTag.kind)))
            return "thread " + std::to_string(id) + " untagged resume completion (" + closureName(t.resumeCompletion) + ")";
        for (const GuestInvocation &inv : t.invocations)
            if (inv.onComplete)
                return "thread " + std::to_string(id) + " invocation onComplete kind=" +
                       std::to_string(static_cast<int>(inv.kind)) + " (" + closureName(inv.onComplete) + ")";
    }
    return {};
}

void EeSchedulerSavestate::save(const EeScheduler &s, Writer &w)
{
    for (const auto &queue : s.m_readyQueues)
        writeIntDeque(w, queue);
    writeOrdered(w, s.m_threads, [](Writer &ww, const auto &entry) {
        const GuestThread &t = entry.second;
        ww.pod(entry.first);
        ww.pod(t.id);
        ww.pod(t.context);
        for (uint32_t v : {t.entry, t.stack, t.stackSize, t.gp, t.attr, t.option, t.arg})
            ww.u32(v);
        ww.pod(t.initialPriority);
        ww.pod(t.currentPriority);
        ww.u8(static_cast<uint8_t>(t.status));
        ww.pod(t.suspendCount);
        ww.u32(t.wakeupCount);
        ww.b(t.ownsStack);
        ww.u32(t.tlsBase);
        writeWait(ww, t.wait);
        ww.b(static_cast<bool>(t.resumeCompletion));
        writeTag(ww, t.resumeCompletion ? t.resumeTag : EeCompletionTag{});
        ww.u64(t.invocations.size());
        for (const GuestInvocation &inv : t.invocations)
            writeInvocation(ww, inv);
    });
    writeOrdered(w, s.m_semaphores, [](Writer &ww, const auto &entry) {
        const EeSemaphore &v = entry.second;
        ww.pod(entry.first);
        for (int x : {v.id, v.count, v.maxCount, v.initCount})
            ww.pod(x);
        ww.u32(v.attr);
        ww.u32(v.option);
        writeIntDeque(ww, v.waiters);
    });
    writeOrdered(w, s.m_eventFlags, [](Writer &ww, const auto &entry) {
        const EeEventFlag &v = entry.second;
        ww.pod(entry.first);
        ww.pod(v.id);
        for (uint32_t x : {v.attr, v.option, v.initBits, v.bits})
            ww.u32(x);
        writeIntDeque(ww, v.waiters);
    });
    writeOrderedPod(w, s.m_alarms);
    writeOrderedPod(w, s.m_intcHandlers);
    writeOrderedPod(w, s.m_dmacHandlers);
    for (int v : {s.m_nextThreadId, s.m_nextInvocationThreadId, s.m_nextSemaphoreId, s.m_nextEventFlagId,
                  s.m_nextAlarmId, s.m_nextIntcHandlerId, s.m_nextDmacHandlerId, s.m_intcHeadOrder, s.m_intcTailOrder,
                  s.m_dmacHeadOrder, s.m_dmacTailOrder, s.m_currentThreadId})
        w.pod(v);
    w.u32(s.m_enabledIntcMask);
    w.u32(s.m_enabledDmacMask);
    for (bool v : {s.m_rescheduleRequested, s.m_timeSliceExpired, s.m_insideInterrupt, s.m_soundClockStarted})
        w.b(v);
    w.u32(s.m_pendingEeTimerInterrupts);
    w.u64(s.m_eeCycle);
    w.u64(s.m_countCycle);
    w.u32(s.m_count);
    w.u64(s.m_sliceEndCycle);
    w.b(s.m_checkpointPending.load());
    w.u32(s.m_debugPublishCountdown);
    {
        std::lock_guard lock(s.m_eventMutex);
        w.u64(s.m_events.size());
        for (const EeEvent &e : s.m_events)
            w.pod(e);
        const int64_t nowNs = steadyNowNs();
        w.u64(s.m_deadlines.size());
        for (const auto &d : s.m_deadlines)
        {
            w.u64(d.deadlineCycle);
            const int64_t hostNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       d.hostDeadline.time_since_epoch())
                                       .count();
            w.pod(static_cast<int64_t>(hostNs - nowNs)); // rebased on load
            w.pod(d.event);
            w.u64(d.sequence);
        }
        w.u64(s.m_pendingInvocations.size());
        for (const GuestInvocation &inv : s.m_pendingInvocations)
            writeInvocation(w, inv);
    }
    w.u64(s.m_eventSequence);
    w.u64(s.m_invocationSequence);
    w.u64(s.m_vsyncTick);
    for (uint32_t v : {s.m_vsyncFlagAddress, s.m_vsyncTickAddress, s.m_gsVSyncCallback, s.m_gsVSyncCallbackGp,
                       s.m_gsVSyncCallbackSp})
        w.u32(v);
    writeOrderedPod(w, s.m_invocationStackTops);
    w.u64(s.m_nextDeadlineCycle.load());
}

bool EeSchedulerSavestate::load(EeScheduler &s, Reader &r, PS2Runtime &runtime)
{
    for (auto &queue : s.m_readyQueues)
        readIntDeque(r, queue);
    readOrdered(r, s.m_threads, [&runtime](Reader &rr, auto &entry) {
        GuestThread &t = entry.second;
        rr.pod(entry.first);
        rr.pod(t.id);
        rr.pod(t.context);
        for (uint32_t *v : {&t.entry, &t.stack, &t.stackSize, &t.gp, &t.attr, &t.option, &t.arg})
            *v = rr.u32();
        rr.pod(t.initialPriority);
        rr.pod(t.currentPriority);
        t.status = static_cast<EeThreadStatus>(rr.u8());
        rr.pod(t.suspendCount);
        t.wakeupCount = rr.u32();
        t.ownsStack = rr.b();
        t.tlsBase = rr.u32();
        readWait(rr, t.wait, runtime);
        const bool hasResume = rr.b();
        t.resumeTag = readTag(rr);
        t.resumeCompletion = {};
        if (hasResume)
            rebuild(rr, t.resumeTag, runtime, t.resumeCompletion);
        t.invocations.resize(static_cast<size_t>(rr.count(1u << 16)));
        for (GuestInvocation &inv : t.invocations)
            readInvocation(rr, inv);
    });
    readOrdered(r, s.m_semaphores, [](Reader &rr, auto &entry) {
        EeSemaphore &v = entry.second;
        rr.pod(entry.first);
        for (int *x : {&v.id, &v.count, &v.maxCount, &v.initCount})
            rr.pod(*x);
        v.attr = rr.u32();
        v.option = rr.u32();
        readIntDeque(rr, v.waiters);
    });
    readOrdered(r, s.m_eventFlags, [](Reader &rr, auto &entry) {
        EeEventFlag &v = entry.second;
        rr.pod(entry.first);
        rr.pod(v.id);
        for (uint32_t *x : {&v.attr, &v.option, &v.initBits, &v.bits})
            *x = rr.u32();
        readIntDeque(rr, v.waiters);
    });
    readOrderedPod(r, s.m_alarms);
    readOrderedPod(r, s.m_intcHandlers);
    readOrderedPod(r, s.m_dmacHandlers);
    for (int *v : {&s.m_nextThreadId, &s.m_nextInvocationThreadId, &s.m_nextSemaphoreId, &s.m_nextEventFlagId,
                   &s.m_nextAlarmId, &s.m_nextIntcHandlerId, &s.m_nextDmacHandlerId, &s.m_intcHeadOrder,
                   &s.m_intcTailOrder, &s.m_dmacHeadOrder, &s.m_dmacTailOrder, &s.m_currentThreadId})
        r.pod(*v);
    s.m_enabledIntcMask = r.u32();
    s.m_enabledDmacMask = r.u32();
    for (bool *v : {&s.m_rescheduleRequested, &s.m_timeSliceExpired, &s.m_insideInterrupt, &s.m_soundClockStarted})
        *v = r.b();
    s.m_pendingEeTimerInterrupts = r.u32();
    s.m_eeCycle = r.u64();
    s.m_countCycle = r.u64();
    s.m_count = r.u32();
    s.m_sliceEndCycle = r.u64();
    s.m_checkpointPending.store(r.b());
    s.m_debugPublishCountdown = r.u32();
    {
        std::lock_guard lock(s.m_eventMutex);
        s.m_events.clear();
        const uint64_t events = r.count(1u << 20);
        for (uint64_t i = 0; i < events && r.ok(); ++i)
        {
            EeEvent e{};
            r.pod(e);
            s.m_events.push_back(e);
        }
        const int64_t nowNs = steadyNowNs();
        s.m_deadlines.resize(static_cast<size_t>(r.count(1u << 20)));
        for (auto &d : s.m_deadlines)
        {
            d.deadlineCycle = r.u64();
            int64_t offsetNs = 0;
            r.pod(offsetNs);
            d.hostDeadline = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(nowNs + offsetNs));
            r.pod(d.event);
            d.sequence = r.u64();
        }
        s.m_pendingInvocations.clear();
        const uint64_t pending = r.count(1u << 20);
        for (uint64_t i = 0; i < pending && r.ok(); ++i)
        {
            GuestInvocation inv{};
            readInvocation(r, inv);
            s.m_pendingInvocations.push_back(std::move(inv));
        }
    }
    s.m_eventSequence = r.u64();
    s.m_invocationSequence = r.u64();
    s.m_vsyncTick = r.u64();
    for (uint32_t *v : {&s.m_vsyncFlagAddress, &s.m_vsyncTickAddress, &s.m_gsVSyncCallback, &s.m_gsVSyncCallbackGp,
                        &s.m_gsVSyncCallbackSp})
        *v = r.u32();
    readOrderedPod(r, s.m_invocationStackTops);
    s.m_nextDeadlineCycle.store(r.u64());
    s.m_vsyncPacer = ps2_vsync_pacer::Pacer{}; // host time: re-anchors on the next vsync
    if (!r.ok())
        return false;
    s.publishSnapshot();
    return true;
}
