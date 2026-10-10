#include "runtime/gs/gs_worker.h"
#include "ThreadNaming.h"
#include "ps2_adpf.h"
#include "ps2_mtvu.h"
#include "ps2_perf_log.h"
#include "ps2_thread_affinity.h"
#include "ps2_tls_model.h"

#include <cstdio>
#include <cstdlib>

#include <chrono>
#include <cstdio>

namespace
{
// PT2 Part 2: per-thread enqueue-wait sink (see setEnqueueWaitSink).
thread_local uint64_t *t_enqueueWaitSink PS2X_TLS_HOT = nullptr;
// MP1 L2: this thread's local batch depth (see beginLocalBatch).
thread_local uint32_t t_localBatchDepth PS2X_TLS_HOT = 0;
// GPK1: staged publish (see GsWorker::setStagedPublish).
std::atomic<bool> s_stagedPublish{false};
std::atomic<uint64_t> s_stagedRounds{0}; // receipts: staged publish rounds
thread_local std::vector<GsCommand> t_staged PS2X_TLS_HOT;
thread_local GsWorker *t_stagedWorker PS2X_TLS_HOT = nullptr;
thread_local size_t t_stagedBytes PS2X_TLS_HOT = 0;
// GSB1: GIF batch command (see GsWorker::setGifBatch). One pending batch
// per thread: concatenated bytes, the subs table, and the source vectors
// (returned to the pool in bulk at publish). Pending iff subs is non-empty.
std::atomic<bool> s_gifBatch{false};
std::atomic<size_t> s_gifBatchBytes{GsWorker::kGifBatchDefaultBytes};
std::atomic<uint64_t> s_batchRounds{0}; // receipts: batches published
std::atomic<uint64_t> s_batchedCmds{0}; // receipts: sub-packets batched
thread_local std::vector<uint8_t> t_batch PS2X_TLS_HOT;
thread_local std::vector<uint32_t> t_batchSubs PS2X_TLS_HOT;
thread_local std::vector<std::vector<uint8_t>> t_batchSources PS2X_TLS_HOT;
thread_local GsWorker *t_batchWorker PS2X_TLS_HOT = nullptr;
// GSB2: the same pending batch in arena mode. Views append here (no byte
// copy): one subs entry exactly as GSB1 builds it, one subOff entry pointing
// into t_batchArenas, and the accumulated view bytes for the cap rule. The
// two modes never mix in one batch (owned-byte packets enqueue alone while
// the arena is on), so t_batch stays empty here and vice versa.
std::atomic<bool> s_gifArena{false};
std::atomic<uint64_t> s_arenaViews{0}; // receipts: views appended to batches
thread_local std::vector<GsGifArenaRef> t_batchArenas PS2X_TLS_HOT;
thread_local std::vector<uint32_t> t_batchSubOff PS2X_TLS_HOT;
thread_local size_t t_batchViewBytes PS2X_TLS_HOT = 0u;
// GSW2: GS queue wake hysteresis (PS2X_GS_WAKE_LOWWATER=<n>, default off =
// 0/unset/invalid). In a GS-bound window the queue is full, so every pop
// batch freed a little space and did m_hasSpace.notify_all(), waking the
// blocked MTVU-GIF producer ~200x/update (~1.9 ms/update of futex +
// mutex-wake time on the Odin, GSW1 REPORT.md section 6). With a mark set,
// the worker wakes space waiters only once the queue has drained below <n>
// descriptors, or on worker idle / flushWake / Fence execution. Same
// commands, same order: only wake timing moves. Notified waiters re-check
// hasSpace, so a wake is never wrong, only early. Parsed per start() so
// tests can toggle it with the env between workers (production starts one
// worker).
std::atomic<size_t> s_spaceLowWater{0};

size_t parseSpaceLowWater()
{
    const char *env = std::getenv("PS2X_GS_WAKE_LOWWATER");
    if (!env || !*env)
        return 0u;
    char *end = nullptr;
    const unsigned long v = std::strtoul(env, &end, 10);
    if (end == env)
        return 0u;
    return static_cast<size_t>(v);
}

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

void GsWorker::setGifBatch(bool on, size_t maxBytes)
{
    s_gifBatchBytes.store(maxBytes != 0u ? maxBytes : kGifBatchDefaultBytes,
                          std::memory_order_relaxed);
    s_gifBatch.store(on, std::memory_order_relaxed);
}

void GsWorker::flushGifBatch()
{
    if (!t_batchSubs.empty())
        t_batchWorker->publishBatch();
}

void GsWorker::setBatchPool(GsPacketPool *pool)
{
    m_batchPool = pool;
}

uint64_t GsWorker::gifBatchRounds()
{
    return s_batchRounds.load(std::memory_order_relaxed);
}

uint64_t GsWorker::gifBatchedCmds()
{
    return s_batchedCmds.load(std::memory_order_relaxed);
}

void GsWorker::setGifArena(bool on)
{
    s_gifArena.store(on, std::memory_order_relaxed);
}

bool GsWorker::gifArenaOn()
{
    return s_gifArena.load(std::memory_order_relaxed);
}

uint64_t GsWorker::gifArenaViews()
{
    return s_arenaViews.load(std::memory_order_relaxed);
}

void GsWorker::publishBatch()
{
    if (t_batchSubs.empty())
        return;
    s_batchRounds.fetch_add(1u, std::memory_order_relaxed);
    s_batchedCmds.fetch_add(t_batchSubs.size(), std::memory_order_relaxed);
    // The sources are dead after the memcpy: return them to the pool under
    // one lock round, before the queue mutex (the worker never holds m_mutex
    // while pooling, so the order cannot invert).
    if (m_batchPool)
        m_batchPool->releaseBulk(t_batchSources);
    else
        t_batchSources.clear();
    GsCommand cmd;
    cmd.kind = GsCmdKind::GifBatch;
    if (!t_batchArenas.empty())
    {
        // GSB2: the batch is views (no contiguous bytes were built): the
        // arenas move with it and retire when the worker consumes it.
        cmd.arenas = std::move(t_batchArenas);
        cmd.subOff = std::move(t_batchSubOff);
        t_batchArenas.clear();
        t_batchSubOff.clear();
        t_batchViewBytes = 0u;
    }
    else
    {
        cmd.bytes = std::move(t_batch);
        t_batch.clear();
    }
    cmd.subs = std::move(t_batchSubs);
    t_batchSubs.clear();
    // Batched packets were local-batch enqueues: admit the batch as such.
    ++t_localBatchDepth;
    bool wake = false;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        wake = admitLocked(lock, cmd);
    }
    --t_localBatchDepth;
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
    // GSW2: (re-)read the wake low-water mark; 0 keeps the old every-pop notify.
    const size_t lowWater = parseSpaceLowWater();
    s_spaceLowWater.store(lowWater, std::memory_order_relaxed);
    if (lowWater != 0u)
        std::fprintf(stderr, "[gs:handoff] GSW2 wake low-water %zu descriptors\n", lowWater);
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
    if (s_gifBatch.load(std::memory_order_relaxed))
    {
        // GSB1: append a local-batch GifPacket from the GIF stage to this
        // thread's batch; anything else publishes the batch first, so the
        // RPC keeps its stream position (GPK1 staging is bypassed on the
        // batching thread; see the header). GSB2: while the arena is on,
        // owned-byte packets are never appended (they enqueue alone below,
        // as oversize ones do); views arrive through enqueueView.
        if (t_localBatchDepth != 0u && !cmd.rpc && cmd.kind == GsCmdKind::GifPacket &&
            ps2_mtvu::onGifStage() && !s_gifArena.load(std::memory_order_relaxed))
        {
            if (t_batchWorker != this)
                flushGifBatch();
            t_batchWorker = this;
            const size_t cap = s_gifBatchBytes.load(std::memory_order_relaxed);
            const size_t size = cmd.bytes.size();
            if (!t_batchSubs.empty() && t_batch.size() + size > cap)
                publishBatch();
            // Oversize (or unencodable) packets fall through and enqueue
            // alone, exactly as today; the pending batch went above.
            if (size <= cap && size <= kGsGifBatchSubLenMask)
            {
                if (t_batchSubs.empty())
                    t_batch.reserve(cap < kGifBatchDefaultBytes ? cap : kGifBatchDefaultBytes);
                const uint32_t entry =
                    ((cmd.u32a & kGsGifPacketHasPath) != 0u ? kGsGifBatchSubNote : 0u) |
                    ((static_cast<uint32_t>(cmd.pathId) & kGsGifBatchSubPathMask)
                     << kGsGifBatchSubPathShift) |
                    static_cast<uint32_t>(size);
                t_batchSubs.push_back(entry);
                t_batch.insert(t_batch.end(), cmd.bytes.begin(), cmd.bytes.end());
                t_batchSources.push_back(std::move(cmd.bytes));
                return;
            }
        }
        else
            flushGifBatch();
    }
    else if (s_stagedPublish.load(std::memory_order_relaxed))
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

void GsWorker::enqueueView(uint8_t pathId, bool notePath, GsGifArenaRef arena, uint32_t off,
                            uint32_t len)
{
    // GSB2: the view half of enqueue()'s GSB1 block. Same publish behavior:
    // a batchable view appends (subs entry exactly as GSB1 builds it, plus
    // the subOff pointer); anything else publishes the batch first. A view
    // that cannot batch (wrong thread, no batch, oversize, or a batch that
    // would span a 256th arena) enqueues alone with its bytes copied, as
    // GSB1's oversize packets do.
    const size_t size = static_cast<size_t>(len);
    bool batched = false;
    if (s_gifBatch.load(std::memory_order_relaxed) && s_gifArena.load(std::memory_order_relaxed) &&
        arena && t_localBatchDepth != 0u && ps2_mtvu::onGifStage())
    {
        if (t_batchWorker != this)
            flushGifBatch();
        t_batchWorker = this;
        const size_t cap = s_gifBatchBytes.load(std::memory_order_relaxed);
        const bool newArena =
            t_batchArenas.empty() || t_batchArenas.back().get() != arena.get();
        if (!t_batchSubs.empty() &&
            (t_batchViewBytes + size > cap ||
             (newArena && t_batchArenas.size() > kGsGifBatchSubOffSlotMax)))
            publishBatch();
        if (size <= cap && size <= kGsGifBatchSubLenMask &&
            t_batchArenas.size() <= kGsGifBatchSubOffSlotMax &&
            static_cast<size_t>(off) + size <= GsGifArena::kBytes)
        {
            uint32_t slot = 0u;
            if (!t_batchArenas.empty() && t_batchArenas.back().get() == arena.get())
                slot = static_cast<uint32_t>(t_batchArenas.size() - 1u);
            else
            {
                slot = static_cast<uint32_t>(t_batchArenas.size());
                t_batchArenas.push_back(std::move(arena));
            }
            // Views of one arena arrive in fill order (the producer seals it
            // before opening the next), so consecutive views share the slot.
            const uint32_t entry = (notePath ? kGsGifBatchSubNote : 0u) |
                                   ((static_cast<uint32_t>(pathId) & kGsGifBatchSubPathMask)
                                    << kGsGifBatchSubPathShift) |
                                   static_cast<uint32_t>(size);
            t_batchSubs.push_back(entry);
            t_batchSubOff.push_back((slot << kGsGifBatchSubOffSlotShift) |
                                    (off & kGsGifBatchSubOffMask));
            t_batchViewBytes += size;
            s_arenaViews.fetch_add(1u, std::memory_order_relaxed);
            batched = true;
        }
        else
            flushGifBatch();
    }
    else
        flushGifBatch();
    if (batched)
        return;
    GsCommand cmd;
    cmd.kind = GsCmdKind::GifPacket;
    if (notePath)
    {
        cmd.u32a = kGsGifPacketHasPath;
        cmd.pathId = pathId;
    }
    if (arena && static_cast<size_t>(off) + size <= GsGifArena::kBytes)
        cmd.bytes.assign(arena.get()->bytes + off, arena.get()->bytes + off + size);
    enqueue(std::move(cmd));
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
    if (t_batchWorker == this)
        flushGifBatch(); // GSB1: same for a pending batch
    std::unique_lock<std::mutex> lock(m_mutex);
    // GSW2: with wake hysteresis on, a flush also releases space-blocked
    // producers (knob off adds one atomic load and one branch).
    const bool space =
        s_spaceLowWater.load(std::memory_order_relaxed) != 0u && m_spaceWaiters != 0u;
    if (m_batchDepth != 0 || !m_batchDirty)
    {
        lock.unlock();
        if (space)
            m_hasSpace.notify_all();
        return;
    }
    m_batchDirty = false;
    m_deferredSinceNs = 0;
    // MP1 L2 (a): notifies a sleeping worker only (a running one
    // drains to empty before it sleeps again).
    bool notify = m_workerIdle;
    m_workerIdle = false;
    lock.unlock();
    if (space)
        m_hasSpace.notify_all();
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
    // PIN1: PS2X_GS_WORKER_CPUS="6,7" pins this thread to itself (the N11
    // pattern); unset/empty = no change.
    if (const char *workerCpus = std::getenv("PS2X_GS_WORKER_CPUS"))
    {
        if (workerCpus[0] != '\0')
        {
            const int rc = ps2x::pinCurrentThreadToCpus(ps2x::parseCpuList(workerCpus));
            std::fprintf(stderr, "[affinity] gs worker thread cpus=%s rc=%d\n", workerCpus, rc);
        }
    }
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
            const size_t lowWater = s_spaceLowWater.load(std::memory_order_relaxed); // GSW2
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
                // GSW2: starvation guard — always wake space waiters before
                // sleeping (the queue is empty, so every waiter can proceed).
                if (lowWater != 0u && m_spaceWaiters != 0u)
                    m_hasSpace.notify_all();
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
            // GSW2: with a low-water mark, only once drained below it.
            notifySpace = m_spaceWaiters != 0u && (lowWater == 0u || m_queue.size() < lowWater);
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
            // GSW2: a Fence at stream position also releases space-blocked
            // producers (knob on only; fences are rare, so the lock round
            // is cheap; knob off adds one kind compare per command).
            if (batch[i].kind == GsCmdKind::Fence &&
                s_spaceLowWater.load(std::memory_order_relaxed) != 0u)
            {
                std::unique_lock<std::mutex> fenceLock(m_mutex);
                const bool fenceSpace = m_spaceWaiters != 0u;
                fenceLock.unlock();
                if (fenceSpace)
                    m_hasSpace.notify_all();
            }
        }
        const uint64_t executed = m_executedCount.fetch_add(batchSize, std::memory_order_relaxed) + batchSize;
        if (deferOn && (executed >> 18) != ((executed - batchSize) >> 18))
            std::fprintf(stderr, "[gs:handoff] executed=%llu wakes=%llu deferred=%llu watchdog=%llu staged_rounds=%llu gif_batches=%llu gif_batched=%llu gif_arena_views=%llu gif_arenas=%llu gif_arena_hits=%llu\n",
                         static_cast<unsigned long long>(executed),
                         static_cast<unsigned long long>(m_wakes.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(m_deferred.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(m_watchdog.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(s_stagedRounds.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(s_batchRounds.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(s_batchedCmds.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(s_arenaViews.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(GsGifArenaPool::acquireCount()),
                         static_cast<unsigned long long>(GsGifArenaPool::poolHitCount()));
    }
}
