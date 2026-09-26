#ifndef PS2_VU1_ENGINE_H
#define PS2_VU1_ENGINE_H

// VP2: in-order-commit VU1 engine (local/research/VP1/REPORT.md Q5,
// local/research/VP2/REPORT.md). PS2X_VU1_WORKERS=1 with PS2X_MTVU=1: the
// MTVU unit thread becomes the sequencer. It still parses VIF1 in order; at
// each MSCAL/MSCNT it commits the previous run, copies VU1 data memory into a
// private snapshot and hands the run to a worker thread, then keeps parsing.
// The run executes on the snapshot; its XGKICK packets are captured. GIF-bound
// sequencer work that follows a dispatched run (DIRECT, MSKPATH3, PATH3) goes
// into a reorder buffer behind it. Commit, on the sequencer, in stream order:
// the snapshot is written back except the words VIF wrote after the dispatch
// (the WAW mask), the run's packets are submitted as PATH1, then the deferred
// actions run. Every unit job ends with a full drain, so the EE-side MT1 sync
// points are unchanged. W=1: a run waits for the previous run to finish and be
// written back before its snapshot; the previous run's packet commit and the
// VIF parse up to the next MSCAL overlap with its compute.
//
// Unset/0 (default): off, every hook is one branch on a cached flag. Refused
// (off) unless MT1 threaded mode is on (which dev traces already refuse).
// Knobs:
//   PS2X_VU1_WORKERS=1     the engine (values > 1 are clamped to 1 for now).
//   PS2X_VU1_SPIN_US=N     spin N us before a waiting side sleeps (default 50;
//                          0 = condvar wake only).
//   PS2X_VU1_JITTER=N      test: sleep 0..N us before each run (host timing).
// Compile-time PS2X_VU1_ENGINE_STATS=1 (CMake option, default OFF): hand-off
// timing (enqueue -> worker start, finish -> commit), waits, unit time; a
// summary line every 300 vsyncs. Speed builds leave it off.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "ThreadNaming.h"
#include "ps2_fpmode.h"
#include "ps2_mtvu.h"
#include "ps2_thread_affinity.h"
#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif

#ifndef PS2X_VU1_ENGINE_STATS
#define PS2X_VU1_ENGINE_STATS 0
#endif

namespace ps2_vu1_engine
{
    constexpr uint32_t kDataBytes = 0x4000u;           // VU1 data memory
    constexpr uint32_t kWords = kDataBytes / 4u;       // 4096 32-bit words
    constexpr uint32_t kMaskWords = kWords / 64u;      // 64 x 64-bit mask words
    using WawMask = std::array<uint64_t, kMaskWords>;

    // Word-granular WAW mask: bit w set = the sequencer (VIF) wrote word w of
    // canonical VU1 data after the in-flight run's snapshot was taken.
    inline void markWord(uint64_t *mask, uint32_t word)
    {
        word &= kWords - 1u;
        mask[word >> 6] |= 1ull << (word & 63u);
    }

    // lanes: bit i = word (qwordIndex * 4 + i) was written.
    inline void markQwordLanes(uint64_t *mask, uint32_t qwordIndex, uint32_t lanes)
    {
        const uint32_t word = (qwordIndex * 4u) & (kWords - 1u);
        mask[word >> 6] |= static_cast<uint64_t>(lanes & 0xFu) << (word & 63u);
    }

    // Write-back: canonical <- result, except words set in waw (a later VIF
    // write, which in serial order came after the run, keeps its value). A
    // word the run did not write equals its snapshot, which equals canonical
    // unless VIF wrote it since, so copying every unmasked word is exact.
    inline void mergeWriteBack(uint8_t *canonical, const uint8_t *result, const uint64_t *waw)
    {
        for (uint32_t i = 0; i < kMaskWords; ++i)
        {
            const uint64_t m = waw[i];
            uint8_t *dst = canonical + i * 256u;
            const uint8_t *src = result + i * 256u;
            if (m == 0u)
            {
                std::memcpy(dst, src, 256u);
                continue;
            }
            if (m == ~0ull)
                continue;
            for (uint32_t b = 0; b < 64u; ++b)
                if ((m & (1ull << b)) == 0u)
                    std::memcpy(dst + b * 4u, src + b * 4u, 4u);
        }
    }

    // After a write-back, canonical equals the run's result except the WAW
    // words: copying just those into the result makes it the next snapshot
    // (instead of a full 16 KiB copy back).
    inline void refreshSnapshot(uint8_t *snapshot, const uint8_t *canonical, const uint64_t *waw)
    {
        for (uint32_t i = 0; i < kMaskWords; ++i)
        {
            const uint64_t m = waw[i];
            if (m == 0u)
                continue;
            uint8_t *dst = snapshot + i * 256u;
            const uint8_t *src = canonical + i * 256u;
            if (m == ~0ull)
            {
                std::memcpy(dst, src, 256u);
                continue;
            }
            for (uint32_t b = 0; b < 64u; ++b)
                if ((m & (1ull << b)) != 0u)
                    std::memcpy(dst + b * 4u, src + b * 4u, 4u);
        }
    }

    // Reorder buffer: runs and deferred sequencer actions in stream order.
    // commitReady() retires from the head: an action runs, a run commits only
    // once done(id) says it finished; it stops at the first unfinished run.
    class ReorderBuffer
    {
    public:
        struct Entry
        {
            uint64_t run = 0; // 0 = action
            std::function<void()> action;
        };

        void pushRun(uint64_t id) { m_q.push_back(Entry{id, {}}); }
        void pushAction(std::function<void()> fn) { m_q.push_back(Entry{0u, std::move(fn)}); }
        bool empty() const { return m_q.empty(); }
        size_t size() const { return m_q.size(); }

        template <class Done, class Commit>
        size_t commitReady(Done &&done, Commit &&commitRun)
        {
            size_t n = 0;
            while (!m_q.empty())
            {
                Entry &e = m_q.front();
                if (e.run != 0u)
                {
                    if (!done(e.run))
                        break;
                    const uint64_t id = e.run;
                    m_q.pop_front();
                    commitRun(id);
                }
                else
                {
                    std::function<void()> fn = std::move(e.action);
                    m_q.pop_front();
                    fn(); // may push more actions (they commit in this loop, in order)
                }
                ++n;
            }
            return n;
        }

    private:
        std::deque<Entry> m_q;
    };

    struct RunJob
    {
        uint64_t id = 0;
        bool resume = false; // MSCNT
        uint32_t startPC = 0;
        uint32_t top = 0;
        uint32_t itop = 0;
        uint32_t fbrst = 0;
        uint64_t fpControl = 0;
    };

    // Worker side: run one job on `data` (the private snapshot).
    using RunFn = std::function<void(const RunJob &, uint8_t *data)>;
    // Commit side: submit one captured PATH1 packet.
    using Path1Fn = std::function<void(const uint8_t *, uint32_t)>;

    namespace detail
    {
        // Set on the worker around a run: XGKICK packets append here
        // ([u32 size][bytes], in kick order) instead of going to the GIF.
        inline thread_local std::vector<uint8_t> *t_capture = nullptr;

        inline uint64_t nowNs()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
        }

        inline void cpuRelax()
        {
#if defined(__aarch64__) || defined(__arm64__)
            __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
            _mm_pause();
#endif
        }

#if PS2X_VU1_ENGINE_STATS
        // 10 ns buckets up to 200 us; everything above lands in the last one.
        struct Hist
        {
            static constexpr uint32_t kBuckets = 20000u;
            std::vector<uint32_t> b = std::vector<uint32_t>(kBuckets + 1u, 0u);
            uint64_t n = 0, sum = 0, max = 0;
            void add(uint64_t ns)
            {
                const uint64_t i = ns / 10u;
                ++b[i < kBuckets ? i : kBuckets];
                ++n;
                sum += ns;
                if (ns > max)
                    max = ns;
            }
            uint64_t pct(double p) const
            {
                if (n == 0u)
                    return 0u;
                const uint64_t want = static_cast<uint64_t>(p * static_cast<double>(n - 1u)) + 1u;
                uint64_t acc = 0;
                for (uint32_t i = 0; i <= kBuckets; ++i)
                {
                    acc += b[i];
                    if (acc >= want)
                        return static_cast<uint64_t>(i) * 10u + 5u;
                }
                return max;
            }
            void reset() { *this = Hist(); }
        };
#endif
    }

    class Engine
    {
    public:
        ~Engine() { stopWorker(); }

        // Runtime init (after ps2_mtvu::configure). mtvuThreaded: MT1 threaded
        // mode is on (the sequencer is its unit thread).
        void configure(bool mtvuThreaded)
        {
            const char *e = std::getenv("PS2X_VU1_WORKERS");
            const long want = e ? std::strtol(e, nullptr, 10) : 0;
            const uint32_t spin = envU32("PS2X_VU1_SPIN_US", 50u);
            const uint32_t jit = envU32("PS2X_VU1_JITTER", 0u);
            const int w = (want > 0 && mtvuThreaded) ? 1 : 0;
            setWorkersForTest(w, spin, jit);
            if (want != 0)
                std::fprintf(stderr, "[vu1eng] workers=%d (asked %ld) spin_us=%u jitter_us=%u stats=%d%s%s\n", w, want,
                             spin, jit, PS2X_VU1_ENGINE_STATS,
                             mtvuThreaded ? "" : " (needs PS2X_MTVU=1 threaded: off)",
                             want > 1 ? " (W>1 not built yet: clamped)" : "");
        }

        // Tests / configure: 0 = off (joins the worker), 1 = one worker.
        void setWorkersForTest(int workers, uint32_t spinUs = 50u, uint32_t jitterUs = 0u)
        {
            drainAll();
            stopWorker();
            m_spinNs = static_cast<uint64_t>(spinUs) * 1000u;
            m_jitterUs = jitterUs;
            m_on.store(workers > 0, std::memory_order_relaxed);
        }

        void setRunFn(RunFn fn) { m_runFn = std::move(fn); }
        void setPath1Fn(Path1Fn fn) { m_path1Fn = std::move(fn); }
        void setCanonical(uint8_t *vu1Data) { m_canonical = vu1Data; }
        void setTickFn(std::function<uint64_t()> fn) { m_tickFn = std::move(fn); }

        bool on() const { return m_on.load(std::memory_order_relaxed); }

        // The engine is used only by the MT1 unit thread (the sequencer).
        bool sequencing() const { return on() && ps2_mtvu::onWorker(); }

        // Sequencer: an MSCAL (resume=false) or MSCNT. W=1: the previous run
        // must finish and be written back (its result is this run's input),
        // then this run is posted, and only then does the previous run's
        // commit (PATH1 packets into the GIF/GS frontend, deferred actions)
        // run, overlapped with this run's compute.
        void dispatch(RunJob job)
        {
            ensureWorker();
            if (m_mergedId < m_posted)
            {
                waitFinished(m_posted, Wait::Dispatch);
                writeBack();
                refreshSnapshot(m_snapshot.data(), m_canonical, m_waw.data());
            }
            else
                std::memcpy(m_snapshot.data(), m_canonical, kDataBytes);
            m_waw.fill(0u);
            job.id = ++m_posted;
            // This slot last held run id-2, which the previous dispatch retired.
            m_packets[job.id & 1u].clear();
            job.fpControl = ps2_fpmode::readControl();
            m_job = job;
            m_tracking = true;
            m_rob.pushRun(job.id);
#if PS2X_VU1_ENGINE_STATS
            m_tEnq = detail::nowNs();
            ++m_runs;
#endif
            m_postedAtomic.store(job.id, std::memory_order_seq_cst);
            if (m_workerSleepers.load(std::memory_order_seq_cst) != 0u)
            {
                std::lock_guard<std::mutex> lock(m_m);
                m_cvWork.notify_one();
            }
            retire(); // the previous run's packets and the actions behind it
        }

        // Sequencer: true = a GIF-bound action must go behind the in-flight
        // run (defer() it). Commits whatever already finished first.
        bool deferring()
        {
            if (m_committing || m_rob.empty())
                return false;
            retire();
            return !m_rob.empty();
        }

        void defer(std::function<void()> fn)
        {
#if PS2X_VU1_ENGINE_STATS
            ++m_deferred;
#endif
            m_rob.pushAction(std::move(fn));
        }

        // Sequencer: the VIF WAW mask while a run is in flight, else null.
        uint64_t *wawMask() { return m_tracking ? m_waw.data() : nullptr; }

        // Sequencer, before VU1 code memory changes (MPG): the in-flight run
        // reads code memory, so wait for it to finish (no commit needed).
        void beforeCodeWrite()
        {
            if (m_postedAtomic.load(std::memory_order_relaxed) != m_finished.load(std::memory_order_acquire))
                waitFinished(m_posted, Wait::Code);
        }

        // Sequencer: wait for the in-flight run and retire the whole buffer
        // (end of every unit job; before an untracked VU1 data write).
        void drainAll() { commitAll(Wait::Drain); }

        // Stats builds: unit job boundaries (unit time per tick).
        void jobBegin()
        {
#if PS2X_VU1_ENGINE_STATS
            m_jobStart = detail::nowNs();
#endif
        }
        void jobEnd()
        {
#if PS2X_VU1_ENGINE_STATS
            m_unitNs += detail::nowNs() - m_jobStart;
            ++m_jobs;
            maybeSummary();
#endif
        }

        uint64_t committedForTest() const { return m_committed; }

    private:
        enum class Wait : uint8_t
        {
            Dispatch,
            Code,
            Drain,
            Count
        };

        static uint32_t envU32(const char *name, uint32_t def)
        {
            const char *v = std::getenv(name);
            return (v && v[0] != '\0') ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : def;
        }

        bool finishedAtLeast(uint64_t id) const { return m_finished.load(std::memory_order_acquire) >= id; }

        void commitAll(Wait why)
        {
            if (m_committing || m_rob.empty())
                return;
            waitFinished(m_posted, why);
            retire();
        }

        // Retire from the head. Committing covers deferred actions too: an
        // action that reaches a hook again (submitGifPacket inside a released
        // PATH3 flush, say) is already in stream order and runs at once.
        void retire()
        {
            m_committing = true;
            m_rob.commitReady([this](uint64_t id) { return finishedAtLeast(id); },
                              [this](uint64_t id) { commitRun(id); });
            m_committing = false;
        }

        void waitFinished(uint64_t id, Wait why)
        {
            if (finishedAtLeast(id))
                return;
#if PS2X_VU1_ENGINE_STATS
            const uint64_t t0 = detail::nowNs();
#else
            (void)why;
#endif
            const uint64_t spinUntil = m_spinNs != 0u ? detail::nowNs() + m_spinNs : 0u;
            uint32_t k = 0;
            while (!finishedAtLeast(id))
            {
                if (spinUntil == 0u || ((++k & 63u) == 0u && detail::nowNs() >= spinUntil))
                {
                    std::unique_lock<std::mutex> lock(m_m);
                    m_seqSleepers.fetch_add(1u, std::memory_order_seq_cst);
                    m_cvDone.wait(lock, [&] { return finishedAtLeast(id); });
                    m_seqSleepers.fetch_sub(1u, std::memory_order_relaxed);
                    break;
                }
                detail::cpuRelax();
            }
#if PS2X_VU1_ENGINE_STATS
            const uint64_t t1 = detail::nowNs();
            m_waitNs[static_cast<size_t>(why)] += t1 - t0;
            ++m_waits[static_cast<size_t>(why)];
            // h (finish -> commit side): only when the sequencer was already
            // waiting, so the lag is hand-off cost and not useful parse work.
            const uint64_t fin = m_tFinish.load(std::memory_order_relaxed);
            if (fin >= t0 && t1 >= fin)
                m_hFinish.add(t1 - fin);
#endif
        }

        // Write the in-flight (finished) run's result back to canonical memory.
        void writeBack()
        {
            m_tracking = false;
            mergeWriteBack(m_canonical, m_snapshot.data(), m_waw.data());
            m_mergedId = m_posted;
        }

        void commitRun(uint64_t id)
        {
            if (m_mergedId < id)
                writeBack();
            std::vector<uint8_t> &packets = m_packets[id & 1u];
            size_t pos = 0;
            while (pos + 4u <= packets.size())
            {
                uint32_t n = 0;
                std::memcpy(&n, packets.data() + pos, 4u);
                pos += 4u;
                if (m_path1Fn)
                    m_path1Fn(packets.data() + pos, n);
                pos += n;
            }
            packets.clear();
            ++m_committed;
        }

        void ensureWorker()
        {
            if (m_started)
                return;
            m_started = true;
            m_stop.store(false, std::memory_order_relaxed);
            long stackKb = 0;
            if (const char *env = std::getenv("PS2X_GAME_THREAD_STACK_KB"))
                stackKb = std::strtol(env, nullptr, 10);
#if defined(__unix__) || defined(__APPLE__)
            if (stackKb > 0)
            {
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                if (pthread_attr_setstacksize(&attr, static_cast<size_t>(stackKb) * 1024u) == 0 &&
                    pthread_create(&m_pth, &attr, +[](void *p) -> void * {
                        static_cast<Engine *>(p)->loop();
                        return nullptr; }, this) == 0)
                    m_usePthread = true;
                pthread_attr_destroy(&attr);
                if (m_usePthread)
                    return;
            }
#endif
            m_th = std::thread([this] { loop(); });
        }

        void stopWorker()
        {
            if (!m_started)
                return;
            {
                std::lock_guard<std::mutex> lock(m_m);
                m_stop.store(true, std::memory_order_seq_cst);
            }
            m_cvWork.notify_all();
#if defined(__unix__) || defined(__APPLE__)
            if (m_usePthread)
            {
                pthread_join(m_pth, nullptr);
                m_usePthread = false;
            }
#endif
            if (m_th.joinable())
                m_th.join();
            m_started = false;
        }

        void loop()
        {
            ps2_mtvu::detail::t_unitDepth = 1; // unit work: MT1 hooks are no-ops here
            ThreadNaming::SetCurrentThreadName("VU1W");
            if (const char *cpus = std::getenv("PS2X_VU1_WORKER_CPUS"))
            {
                if (cpus[0] != '\0')
                {
                    const int rc = ps2x::pinCurrentThreadToCpus(ps2x::parseCpuList(cpus));
                    std::fprintf(stderr, "[affinity] vu1 worker cpus=%s rc=%d\n", cpus, rc);
                }
            }
            uint64_t rng = detail::nowNs() | 1u;
            uint64_t done = m_finished.load(std::memory_order_relaxed);
            for (;;)
            {
                // Wait for a posted run (spin, then sleep).
                const uint64_t spinUntil = m_spinNs != 0u ? detail::nowNs() + m_spinNs : 0u;
                uint32_t k = 0;
                for (;;)
                {
                    if (m_stop.load(std::memory_order_acquire))
                        return;
                    if (m_postedAtomic.load(std::memory_order_acquire) > done)
                        break;
                    if (spinUntil == 0u || ((++k & 63u) == 0u && detail::nowNs() >= spinUntil))
                    {
                        std::unique_lock<std::mutex> lock(m_m);
                        m_workerSleepers.fetch_add(1u, std::memory_order_seq_cst);
                        m_cvWork.wait(lock, [&] {
                            return m_stop.load(std::memory_order_seq_cst) ||
                                   m_postedAtomic.load(std::memory_order_seq_cst) > done;
                        });
                        m_workerSleepers.fetch_sub(1u, std::memory_order_relaxed);
                        continue;
                    }
                    detail::cpuRelax();
                }
#if PS2X_VU1_ENGINE_STATS
                const uint64_t tStart = detail::nowNs();
                m_hStart.add(tStart - m_tEnq);
#endif
                const RunJob job = m_job;
                if (m_jitterUs != 0u)
                {
                    rng ^= rng << 13;
                    rng ^= rng >> 7;
                    rng ^= rng << 17;
                    std::this_thread::sleep_for(std::chrono::microseconds(rng % (m_jitterUs + 1u)));
                }
                ps2_fpmode::writeControl(job.fpControl);
                detail::t_capture = &m_packets[job.id & 1u];
                m_runFn(job, m_snapshot.data());
                detail::t_capture = nullptr;
                done = job.id;
#if PS2X_VU1_ENGINE_STATS
                const uint64_t tFin = detail::nowNs();
                m_runHist.add(tFin - tStart);
                m_tFinish.store(tFin, std::memory_order_relaxed);
#endif
                m_finished.store(done, std::memory_order_seq_cst);
                if (m_seqSleepers.load(std::memory_order_seq_cst) != 0u)
                {
                    std::lock_guard<std::mutex> lock(m_m);
                    m_cvDone.notify_all();
                }
            }
        }

#if PS2X_VU1_ENGINE_STATS
        void maybeSummary()
        {
            if (!m_tickFn)
                return;
            const uint64_t tick = m_tickFn();
            if (tick / 300u == m_lastSummaryTick / 300u)
                return;
            const uint64_t ticks = tick > m_lastSummaryTick ? tick - m_lastSummaryTick : 1u;
            std::fprintf(stderr,
                         "[vu1eng] tick=%llu ticks=%llu on=%d jobs=%llu runs=%llu deferred=%llu unit_ms_per_tick=%.3f "
                         "h_start_ns p50=%llu p90=%llu p99=%llu mean=%.0f n=%llu | h_finish_ns p50=%llu p90=%llu p99=%llu mean=%.0f n=%llu | "
                         "run_us p50=%.2f mean=%.2f | wait_ms dispatch=%.2f/%llu code=%.2f/%llu drain=%.2f/%llu\n",
                         static_cast<unsigned long long>(tick), static_cast<unsigned long long>(ticks), on() ? 1 : 0,
                         static_cast<unsigned long long>(m_jobs), static_cast<unsigned long long>(m_runs),
                         static_cast<unsigned long long>(m_deferred), m_unitNs / 1e6 / static_cast<double>(ticks),
                         static_cast<unsigned long long>(m_hStart.pct(0.5)), static_cast<unsigned long long>(m_hStart.pct(0.9)),
                         static_cast<unsigned long long>(m_hStart.pct(0.99)),
                         m_hStart.n ? static_cast<double>(m_hStart.sum) / static_cast<double>(m_hStart.n) : 0.0,
                         static_cast<unsigned long long>(m_hStart.n),
                         static_cast<unsigned long long>(m_hFinish.pct(0.5)), static_cast<unsigned long long>(m_hFinish.pct(0.9)),
                         static_cast<unsigned long long>(m_hFinish.pct(0.99)),
                         m_hFinish.n ? static_cast<double>(m_hFinish.sum) / static_cast<double>(m_hFinish.n) : 0.0,
                         static_cast<unsigned long long>(m_hFinish.n),
                         m_runHist.pct(0.5) / 1e3, m_runHist.n ? static_cast<double>(m_runHist.sum) / static_cast<double>(m_runHist.n) / 1e3 : 0.0,
                         m_waitNs[0] / 1e6, static_cast<unsigned long long>(m_waits[0]),
                         m_waitNs[1] / 1e6, static_cast<unsigned long long>(m_waits[1]),
                         m_waitNs[2] / 1e6, static_cast<unsigned long long>(m_waits[2]));
            m_lastSummaryTick = tick;
            m_jobs = m_runs = m_deferred = m_unitNs = 0;
            m_hStart.reset();
            m_hFinish.reset();
            m_runHist.reset();
            m_waitNs.fill(0u);
            m_waits.fill(0u);
        }
#endif

        std::atomic<bool> m_on{false};
        uint64_t m_spinNs = 50000u;
        uint32_t m_jitterUs = 0;
        RunFn m_runFn;
        Path1Fn m_path1Fn;
        std::function<uint64_t()> m_tickFn;
        uint8_t *m_canonical = nullptr;

        // Sequencer-owned.
        ReorderBuffer m_rob;
        WawMask m_waw{};
        uint64_t m_posted = 0;
        uint64_t m_mergedId = 0; // last run written back to canonical memory
        uint64_t m_committed = 0;
        bool m_tracking = false;
        bool m_committing = false;
        bool m_started = false;

        // Shared with the worker (W=1: one slot; the sequencer touches the
        // slot only while no run is in flight).
        alignas(64) std::array<uint8_t, kDataBytes> m_snapshot{};
        std::array<std::vector<uint8_t>, 2> m_packets; // by run id & 1: the worker fills one while the other commits
        RunJob m_job;
        alignas(64) std::atomic<uint64_t> m_postedAtomic{0};
        alignas(64) std::atomic<uint64_t> m_finished{0};
        std::atomic<uint32_t> m_workerSleepers{0};
        std::atomic<uint32_t> m_seqSleepers{0};
        std::atomic<bool> m_stop{false};
        std::mutex m_m;
        std::condition_variable m_cvWork;
        std::condition_variable m_cvDone;
        std::thread m_th;
#if defined(__unix__) || defined(__APPLE__)
        pthread_t m_pth{};
        bool m_usePthread = false;
#endif

#if PS2X_VU1_ENGINE_STATS
        uint64_t m_tEnq = 0;
        std::atomic<uint64_t> m_tFinish{0};
        uint64_t m_jobStart = 0;
        uint64_t m_unitNs = 0, m_jobs = 0, m_runs = 0, m_deferred = 0;
        uint64_t m_lastSummaryTick = 0;
        detail::Hist m_hStart, m_hFinish, m_runHist;
        std::array<uint64_t, static_cast<size_t>(Wait::Count)> m_waitNs{};
        std::array<uint64_t, static_cast<size_t>(Wait::Count)> m_waits{};
#endif
    };

    inline Engine &engine()
    {
        static Engine e;
        return e;
    }

    // Hooks for the unit code. Each is one branch when the engine is off.
    inline bool deferring()
    {
        Engine &e = engine();
        return e.sequencing() && e.deferring();
    }

    inline void defer(std::function<void()> fn)
    {
        engine().defer(std::move(fn));
    }

    inline uint64_t *wawMask()
    {
        Engine &e = engine();
        return e.sequencing() ? e.wawMask() : nullptr;
    }

    inline void beforeCodeWrite()
    {
        Engine &e = engine();
        if (e.sequencing())
            e.beforeCodeWrite();
    }

    inline void drainAll()
    {
        Engine &e = engine();
        if (e.sequencing())
            e.drainAll();
    }

    // VU1 XGKICK end on the worker: true = captured for the commit.
    inline bool captureXgkick(const uint8_t *data, uint32_t size)
    {
        std::vector<uint8_t> *cap = detail::t_capture;
        if (cap == nullptr)
            return false;
        const size_t at = cap->size();
        cap->resize(at + 4u + size);
        std::memcpy(cap->data() + at, &size, 4u);
        std::memcpy(cap->data() + at + 4u, data, size);
        return true;
    }

    // Stats builds: unit job boundaries.
    struct JobTimer
    {
        JobTimer()
        {
#if PS2X_VU1_ENGINE_STATS
            if (ps2_mtvu::onWorker())
                engine().jobBegin();
#endif
        }
        ~JobTimer()
        {
#if PS2X_VU1_ENGINE_STATS
            if (ps2_mtvu::onWorker())
                engine().jobEnd();
#endif
        }
    };
}

#endif
