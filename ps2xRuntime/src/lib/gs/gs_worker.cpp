#include "runtime/gs/gs_worker.h"
#include "ThreadNaming.h"

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
    std::unique_lock<std::mutex> lock(m_mutex);
    // Backpressure: a full ring blocks the producer (GIF FIFO-full stall).
    // An oversize single command bypasses the byte cap so it can always
    // make progress once the queue drains below it.
    m_hasSpace.wait(lock,
                    [&]
                    {
                        if (m_stopRequested)
                            return true;
                        if (m_queue.size() >= m_maxDescriptors)
                            return false;
                        if (bytes > m_maxPayloadBytes)
                            return true;
                        return m_queuedBytes + bytes <= m_maxPayloadBytes;
                    });
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
    lock.unlock();
    m_hasWork.notify_one();
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
    for (;;)
    {
        GsCommand cmd;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_hasWork.wait(lock, [&] { return m_stopRequested || !m_queue.empty(); });
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
        }
        m_hasSpace.notify_all();
        m_handler(cmd);
        m_executedCount.fetch_add(1u, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_executing = false;
        }
        if (cmd.rpc)
            cmd.rpc->signal();
    }
}
