#pragma once
// GSW1: a helper thread that runs one job at a time, in submit order.
// submit() first waits until the previous job has finished, then hands the
// new one over and returns; join() waits until the helper is idle. The thread
// starts on the first submit; stop() (and the destructor) joins it. Used by
// the GE1 AHB present to run the TRICKY meter composite + queue off the GS
// worker (PS2X_TRICKY_HUD_ASYNC); not included by generated code.
#include "ThreadNaming.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace ps2x_gs
{
class SerialJobThread
{
public:
    explicit SerialJobThread(std::string name) : m_name(std::move(name)) {}
    ~SerialJobThread() { stop(); }
    SerialJobThread(const SerialJobThread &) = delete;
    SerialJobThread &operator=(const SerialJobThread &) = delete;

    void submit(std::function<void()> job)
    {
        std::unique_lock<std::mutex> lock(m_m);
        m_idle.wait(lock, [&] { return !m_job; });
        if (!m_thread.joinable())
            m_thread = std::thread(&SerialJobThread::run, this);
        m_job = std::move(job);
        m_hasJob.notify_one();
    }

    // True when a job was still running (the caller waited for it).
    bool join()
    {
        std::unique_lock<std::mutex> lock(m_m);
        const bool busy = static_cast<bool>(m_job);
        m_idle.wait(lock, [&] { return !m_job; });
        return busy;
    }

    void stop()
    {
        {
            std::unique_lock<std::mutex> lock(m_m);
            m_idle.wait(lock, [&] { return !m_job; });
            m_stop = true;
            m_hasJob.notify_one();
        }
        if (m_thread.joinable())
            m_thread.join();
    }

private:
    void run()
    {
        ThreadNaming::SetCurrentThreadName(m_name.c_str());
        std::unique_lock<std::mutex> lock(m_m);
        for (;;)
        {
            m_hasJob.wait(lock, [&] { return m_stop || static_cast<bool>(m_job); });
            if (!m_job)
                return; // stop with nothing pending
            // submit() waits for an empty slot, so m_job is not touched
            // while it runs unlocked.
            lock.unlock();
            m_job();
            lock.lock();
            m_job = nullptr;
            m_idle.notify_all();
        }
    }

    std::string m_name;
    std::mutex m_m;
    std::condition_variable m_hasJob;
    std::condition_variable m_idle;
    std::function<void()> m_job;
    bool m_stop = false;
    std::thread m_thread;
};
} // namespace ps2x_gs
