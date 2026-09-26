#include "runtime/gs/gs_worker.h"
#include "ThreadNaming.h"
#include "ps2_thread_affinity.h"

#include <cstdio>
#include <cstdlib>

#include <chrono>
#include <cstdio>

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
    const size_t bytes = cmd.payloadBytes();
    const bool hasRpc = cmd.rpc != nullptr;
    std::unique_lock<std::mutex> lock(m_mutex);
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
        m_wakes.fetch_add(1u, std::memory_order_relaxed);
        m_hasWork.notify_one();
        m_hasSpace.wait(lock, hasSpace);
    }
    if (m_stopRequested)
    {
        // Shutdown path only (GS destructor / queue disable, after producer
        // threads are joined): run inline so the call still takes effect.
        lock.unlock();
        m_handler(cmd);
        if (cmd.rpc)
            cmd.rpc->signal();
        return;
    }
    m_queuedBytes += bytes;
    m_queue.push_back(std::move(cmd));
    m_enqueuedCount.fetch_add(1u, std::memory_order_relaxed);
    // GF1 H3: with deferred wakes on, an RPC never rides a batch silently:
    // its caller waits for it (e.g. the main thread's present latch must
    // not wait for the unit's next flush).
    const bool silent = m_batchDepth > 0 && !(m_wakeCommands != 0u && hasRpc);
    if (silent)
    {
        m_batchDirty = true;
    }
    else if (m_batchDepth == 0)
    {
        // This notify also delivers any pending deferred wake.
        m_batchDirty = false;
        m_deferredSinceNs = 0;
    }
    lock.unlock();
    if (!silent)
    {
        m_wakes.fetch_add(1u, std::memory_order_relaxed);
        m_hasWork.notify_one();
    }
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
    lock.unlock();
    if (notify)
    {
        m_wakes.fetch_add(1u, std::memory_order_relaxed);
        m_hasWork.notify_one();
    }
}

void GsWorker::flushWake()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_batchDepth != 0 || !m_batchDirty)
        return;
    m_batchDirty = false;
    m_deferredSinceNs = 0;
    lock.unlock();
    m_wakes.fetch_add(1u, std::memory_order_relaxed);
    m_hasWork.notify_one();
}

void GsWorker::setDeferredWakes(uint32_t wakeCommands, size_t wakeBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_wakeCommands = wakeCommands;
    m_wakeBytes = wakeBytes;
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
    // TN1: PS2X_GS_WORKER_CPUS="4,5" pins this thread to itself (mirrors the
    // N11 game-thread knob); unset/empty = no change. Unconditional stderr
    // line (RUNTIME_LOG compiles out of release builds).
    if (const char *workerCpus = std::getenv("PS2X_GS_WORKER_CPUS"))
    {
        if (workerCpus[0] != '\0')
        {
            const int rc = ps2x::pinCurrentThreadToCpus(ps2x::parseCpuList(workerCpus));
            std::fprintf(stderr, "[affinity] gs worker cpus=%s rc=%d\n", workerCpus, rc);
        }
    }
    for (;;)
    {
        GsCommand cmd;
        bool deferOn = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            deferOn = m_wakeCommands != 0u;
            const auto ready = [&] { return m_stopRequested || !m_queue.empty(); };
            if (m_wakeCommands == 0u)
            {
                m_hasWork.wait(lock, ready);
            }
            else
            {
                // GF1 H3 watchdog: a deferred wake that stays pending for
                // 50 ms means a missed flush point. Count it (the first few
                // print) and run anyway, so a miss costs time, never output.
                while (!ready())
                {
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
            }
            if (m_queue.empty())
            {
                if (m_stopRequested)
                    return;
                continue;
            }
            cmd = std::move(m_queue.front());
            m_queue.pop_front();
            m_queuedBytes -= cmd.payloadBytes();
            m_executing = true;
            if (m_batchDepth == 0 && m_batchDirty)
            {
                // GF1 H3: the worker is awake and drains to empty before it
                // sleeps again, so a pending deferred wake is moot.
                m_batchDirty = false;
                m_deferredSinceNs = 0;
            }
        }
        m_hasSpace.notify_all();
        m_handler(cmd);
        const uint64_t executed = m_executedCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (deferOn && (executed & 0x3FFFFu) == 0u)
            std::fprintf(stderr, "[gs:handoff] executed=%llu wakes=%llu deferred=%llu watchdog=%llu\n",
                         static_cast<unsigned long long>(executed),
                         static_cast<unsigned long long>(m_wakes.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(m_deferred.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(m_watchdog.load(std::memory_order_relaxed)));
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_executing = false;
        }
        if (cmd.rpc)
            cmd.rpc->signal();
    }
}
