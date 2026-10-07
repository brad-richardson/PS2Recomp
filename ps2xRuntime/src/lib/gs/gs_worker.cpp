#include "runtime/gs/gs_worker.h"
#include "ThreadNaming.h"
#include "ps2_adpf.h"
#include "ps2_mtvu.h"
#include "ps2_perf_log.h"
#include "ps2_thread_affinity.h"

#include <cstdio>
#include <cstdlib>

#include <chrono>
#include <cstdio>

namespace
{
// PT2 Part 2: per-thread enqueue-wait sink (see setEnqueueWaitSink).
thread_local uint64_t *t_enqueueWaitSink = nullptr;
// MP1 L2: this thread's local batch depth (see beginLocalBatch).
thread_local uint32_t t_localBatchDepth = 0;
// GPK1: staged publish (see GsWorker::setStagedPublish).
std::atomic<bool> s_stagedPublish{false};
std::atomic<uint64_t> s_stagedRounds{0}; // receipts: staged publish rounds
thread_local std::vector<GsCommand> t_staged;
thread_local GsWorker *t_stagedWorker = nullptr;
thread_local size_t t_stagedBytes = 0;

uint64_t steadyNowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
} // namespace

void GsWorker::beginLocalBatch()
{
    ++t_localBatchDepth;
}

void GsWorker::endLocalBatch()
{
    if (t_localBatchDepth != 0u)
        --t_localBatchDepth;
}

void GsWorker::setEnqueueWaitSink(uint64_t *sink)
{
    t_enqueueWaitSink = sink;
}

void GsWorker::setStagedPublish(bool on)
{
    s_stagedPublish.store(on, std::memory_order_relaxed);
}

void GsWorker::flushStaged()
{
    if (!t_staged.empty())
        t_stagedWorker->publishStaged();
}

void GsWorker::publishStaged()
{
    s_stagedRounds.fetch_add(1u, std::memory_order_relaxed);
    // Staged commands were local-batch enqueues: admit them as such.
    ++t_localBatchDepth;
    bool wake = false;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (GsCommand &cmd : t_staged)
        {
            if (!lock.owns_lock())
                lock.lock();
            wake |= admitLocked(lock, cmd);
        }
    }
    --t_localBatchDepth;
    t_staged.clear();
    t_stagedBytes = 0;
    if (wake)
    {
        m_wakes.fetch_add(1u, std::memory_order_relaxed);
        m_hasWork.notify_one();
    }
}

GsWorker::GsWorker(size_t maxDescriptors, size_t maxPayloadBytes, Handler handler)
    : m_handler(std::move(handler))
    , m_maxDescriptors(maxDescriptors != 0u ? maxDescriptors : kDefaultMaxDescriptors)
    , m_maxPayloadBytes(maxPayloadBytes != 0u ? maxPayloadBytes : kDefaultMaxPayloadBytes)
{
}

GsWorker::~GsWorker()
{
    stop();
}

void GsWorker::start()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running)
        return;
    m_stopRequested = false;
    m_running = true;
    m_thread = std::thread(&GsWorker::threadMain, this);
}

void GsWorker::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running)
            return;
        m_stopRequested = true;
    }
    m_hasWork.notify_all();
    m_hasSpace.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
    }
}

void GsWorker::enqueue(GsCommand cmd)
{
    // VPL1: with the GIF stage on, the unit's GS traffic leaves from the
    // MTVU-GIF thread; an enqueue on the MTVU thread bypassed the ops. Keep
    // it in stream order (every earlier op runs first) and count it.
    if (ps2_mtvu::gifStageOn() && ps2_mtvu::onWorker())
        ps2_mtvu::gifStageEscape();
    else if (ps2_mtvu::vifStageDefer()) // VPL2: counted (must read 0)
        ps2_mtvu::vifStageNoteEscape();
    if (s_stagedPublish.load(std::memory_order_relaxed))
    {
        // GPK1: stage a fire-and-forget local-batch command from the GIF
        // stage; anything else publishes this thread's stage first.
        if (t_localBatchDepth != 0u && !cmd.rpc && ps2_mtvu::onGifStage())
        {
            if (t_stagedWorker != this)
                flushStaged();
            t_stagedWorker = this;
            t_stagedBytes += cmd.payloadBytes();
            t_staged.push_back(std::move(cmd));
            if (t_staged.size() >= kStageMaxCommands || t_stagedBytes >= kStageMaxBytes)
                publishStaged();
            return;
        }
        flushStaged();
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    const bool wake = admitLocked(lock, cmd);
    if (!lock.owns_lock())
        return;
    lock.unlock();
    if (wake)
    {
        m_wakes.fetch_add(1u, std::memory_order_relaxed);
        m_hasWork.notify_one();
    }
}

bool GsWorker::admitLocked(std::unique_lock<std::mutex> &lock, GsCommand &cmd)
{
    const size_t bytes = cmd.payloadBytes();
    const bool hasRpc = cmd.rpc != nullptr;
    // Backpressure: a full ring blocks the producer (GIF FIFO-full stall).
    // An oversize single command bypasses the byte cap so it can always
    // make progress once the queue drains below it.
    const auto hasSpace = [&]
    {
        if (m_stopRequested)
            return true;
        if (m_queue.size() >= m_maxDescriptors)
            return false;
        if (bytes > m_maxPayloadBytes)
            return true;
        return m_queuedBytes + bytes <= m_maxPayloadBytes;
    };
    if (!hasSpace())
    {
        // GF1 H3: a producer about to block for space must not leave a wake
        // pending (batch-silent or deferred enqueues): only the worker can
        // make space. Host liveness only; queue order is unchanged.
        if (m_batchDepth == 0)
        {
            m_batchDirty = false;
            m_deferredSinceNs = 0;
        }
        // MP1 L2: lean mode wakes a sleeping worker only (a running one will
        // pop and see this producer in m_spaceWaiters).
        const bool wake = m_workerIdle;
        if (wake)
            m_workerIdle = false;
        if (wake)
        {
            m_wakes.fetch_add(1u, std::memory_order_relaxed);
            m_hasWork.notify_one();
        }
        // PT2 Part 2: time the backpressure wait into the caller's sink.
        uint64_t *const sink = t_enqueueWaitSink;
        const uint64_t waitT0 = sink ? ps2x::perflog::steadyNs() : 0u;
        ++m_spaceWaiters;
        m_hasSpace.wait(lock, hasSpace);
        --m_spaceWaiters;
        if (sink)
            *sink += ps2x::perflog::steadyNs() - waitT0;
    }
    if (m_stopRequested)
    {
        // Shutdown path only (GS destructor / queue disable, after producer
        // threads are joined): run inline so the call still takes effect.
        lock.unlock();
        m_handler(cmd);
        if (cmd.rpc)
            cmd.rpc->signal();
        return false;
    }
    m_queuedBytes += bytes;
    m_queue.push_back(std::move(cmd));
    m_enqueuedCount.fetch_add(1u, std::memory_order_relaxed);
    // MP1 L2 (c): a local (unit-thread) batch behaves like an open batch
    // whose endBatch(mayDefer) runs after every enqueue: wake once the
    // deferred-wake thresholds are reached, else keep the wake pending.
    const bool local = t_localBatchDepth != 0u && m_wakeCommands != 0u;
    // GF1 H3: with deferred wakes on, an RPC never rides a batch silently:
    // its caller waits for it (e.g. the main thread's present latch must
    // not wait for the unit's next flush).
    bool silent = (m_batchDepth > 0 || local) && !(m_wakeCommands != 0u && hasRpc);
    if (silent && local && m_batchDepth == 0 &&
        (m_queue.size() >= m_wakeCommands || m_queuedBytes >= m_wakeBytes))
        silent = false;
    if (silent)
    {
        m_batchDirty = true;
        if (local && m_batchDepth == 0)
        {
            if (m_deferredSinceNs == 0u)
                m_deferredSinceNs = steadyNowNs();
            m_deferred.fetch_add(1u, std::memory_order_relaxed);
        }
    }
    else if (m_batchDepth == 0)
    {
        // This notify also delivers any pending deferred wake.
        m_batchDirty = false;
        m_deferredSinceNs = 0;
    }
    // MP1 L2 (a): notifies a sleeping worker only.
    bool wake = !silent;
    if (wake)
    {
        wake = m_workerIdle;
        m_workerIdle = false;
    }
    return wake;
}

void GsWorker::beginBatch()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_batchDepth;
}

void GsWorker::endBatch(bool mayDefer)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_batchDepth == 0)
        return;
    bool notify = false;
    if (--m_batchDepth == 0 && m_batchDirty)
    {
        if (mayDefer && m_wakeCommands != 0u && m_queue.size() < m_wakeCommands && m_queuedBytes < m_wakeBytes)
        {
            // GF1 H3: keep the wake pending (m_batchDirty stays set).
            if (m_deferredSinceNs == 0u)
                m_deferredSinceNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
            m_deferred.fetch_add(1u, std::memory_order_relaxed);
        }
        else
        {
            notify = true;
            m_batchDirty = false;
            m_deferredSinceNs = 0;
        }
    }
    // MP1 L2 (a): notifies a sleeping worker only.
    if (notify)
    {
        notify = m_workerIdle;
        m_workerIdle = false;
    }
    lock.unlock();
    if (notify)
    {
        m_wakes.fetch_add(1u, std::memory_order_relaxed);
        m_hasWork.notify_one();
    }
}

void GsWorker::flushWake()
{
    if (t_stagedWorker == this)
        flushStaged(); // GPK1: publish before delivering the wake
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_batchDepth != 0 || !m_batchDirty)
        return;
    m_batchDirty = false;
    m_deferredSinceNs = 0;
    // MP1 L2 (a): notifies a sleeping worker only (a running one
    // drains to empty before it sleeps again).
    bool notify = m_workerIdle;
    m_workerIdle = false;
    lock.unlock();
    if (!notify)
        return;
    m_wakes.fetch_add(1u, std::memory_order_relaxed);
    m_hasWork.notify_one();
}

void GsWorker::setDeferredWakes(uint32_t wakeCommands, size_t wakeBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_wakeCommands = wakeCommands;
    m_wakeBytes = wakeBytes;
}

void GsWorker::setPopBatch(size_t n)
{
    if (n < 1u)
        n = 1u;
    if (n > kPopBatch)
        n = kPopBatch;
    m_popBatch = n;
}

size_t GsWorker::pendingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

size_t GsWorker::pendingBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queuedBytes;
}

bool GsWorker::isQuiescent() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.empty() && !m_executing;
}

void GsWorker::threadMain()
{
    ThreadNaming::SetCurrentThreadName("GsWorker");
    // AD1: bind this thread's TID to its ADPF hint session.
    ps2x::adpf::noteThread(ps2x::adpf::Thread::GsWorker);
    // PT2: per-tick busy time (handler only; queue-idle excluded). The frame
    // cuts at each in-stream GuestVsync (tick in regValue); knob off is one
    // predictable branch per command.
    // AD1: ADPF reuses this accounting, so it turns it on too.
    const bool tail = ps2x::perflog::enabled() || ps2x::adpf::enabled();
    uint64_t tailAccNs = 0;
    // MP1 L2 (b): clears m_executing inside the next pop lock.
    for (;;)
    {
        // GP4 H6: pop up to kPopBatch commands per mutex acquisition and run
        // them back-to-back. Same FIFO order as one-by-one; the space wake,
        // the executed count and m_executing bookkeeping move to per-batch.
        // Each RPC still signals right after its own command executes.
        GsCommand batch[kPopBatch];
        size_t batchSize = 0;
        bool deferOn = false;
        bool notifySpace = true;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_executing = false;
            deferOn = m_wakeCommands != 0u;
            const auto ready = [&] { return m_stopRequested || !m_queue.empty(); };
            if (m_wakeCommands == 0u)
            {
                // MP1 L2 (a): m_workerIdle tells lean producers to notify.
                while (!ready())
                {
                    m_workerIdle = true;
                    m_hasWork.wait(lock);
                }
                m_workerIdle = false;
            }
            else
            {
                // GF1 H3 watchdog: a deferred wake that stays pending for
                // 50 ms means a missed flush point. Count it (the first few
                // print) and run anyway, so a miss costs time, never output.
                while (!ready())
                {
                    m_workerIdle = true;
                    if (m_hasWork.wait_for(lock, std::chrono::milliseconds(100)) != std::cv_status::timeout ||
                        m_queue.empty() || m_batchDepth != 0 || !m_batchDirty || m_deferredSinceNs == 0u)
                        continue;
                    const uint64_t now = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
                    if (now - m_deferredSinceNs < 50000000u)
                        continue;
                    const uint64_t n = m_watchdog.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (n <= 8u)
                        std::fprintf(stderr, "[gs:handoff] WATCHDOG: deferred wake pending %.1f ms, %zu commands queued\n",
                                     static_cast<double>(now - m_deferredSinceNs) / 1e6, m_queue.size());
                }
                m_workerIdle = false;
            }
            if (m_queue.empty())
            {
                if (m_stopRequested)
                    return;
                continue;
            }
            const size_t popBatch = m_popBatch;
            while (batchSize < popBatch && !m_queue.empty())
            {
                batch[batchSize] = std::move(m_queue.front());
                m_queue.pop_front();
                m_queuedBytes -= batch[batchSize].payloadBytes();
                ++batchSize;
            }
            m_executing = true;
            if (m_batchDepth == 0 && m_batchDirty)
            {
                // GF1 H3: the worker is awake and drains to empty before it
                // sleeps again, so a pending deferred wake is moot.
                m_batchDirty = false;
                m_deferredSinceNs = 0;
            }
            // MP1 L2 (a): wakes space waiters only when there are some.
            notifySpace = m_spaceWaiters != 0u;
        }
        if (notifySpace)
            m_hasSpace.notify_all();
        for (size_t i = 0; i < batchSize; ++i)
        {
            const uint64_t t0 = tail ? ps2x::perflog::steadyNs() : 0u;
            m_handler(batch[i]);
            if (tail)
            {
                tailAccNs += ps2x::perflog::steadyNs() - t0;
                if (batch[i].kind == GsCmdKind::GuestVsync)
                {
                    ps2x::perflog::stageRing(ps2x::perflog::Stage::GsBusy)
                        .push(static_cast<uint32_t>(batch[i].regValue),
                              static_cast<float>(tailAccNs / 1e6));
                    // AD1: same busy figure feeds this thread's ADPF session.
                    ps2x::adpf::report(ps2x::adpf::Thread::GsWorker, tailAccNs);
                    tailAccNs = 0;
                }
            }
            if (batch[i].rpc)
                batch[i].rpc->signal();
        }
        const uint64_t executed = m_executedCount.fetch_add(batchSize, std::memory_order_relaxed) + batchSize;
        if (deferOn && (executed >> 18) != ((executed - batchSize) >> 18))
            std::fprintf(stderr, "[gs:handoff] executed=%llu wakes=%llu deferred=%llu watchdog=%llu staged_rounds=%llu\n",
                         static_cast<unsigned long long>(executed),
                         static_cast<unsigned long long>(m_wakes.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(m_deferred.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(m_watchdog.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(s_stagedRounds.load(std::memory_order_relaxed)));
    }
}
