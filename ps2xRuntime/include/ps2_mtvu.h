#ifndef PS2_MTVU_H
#define PS2_MTVU_H

// MT1: ownership hooks for the VIF1 -> VU1 -> GIF -> GS-frontend "unit"
// (local/research/MT1/REPORT.md). Unit jobs (the GIF + VIF1 part of a DMA
// kick, VIF1 FIFO writes, EE GS-privileged writes) own that state; the EE
// thread calls sync() before it touches unit-owned state, and the objects the
// unit owns call touch() so a missed sync shows up.
//
// PS2X_MTVU unset/0 (default): every hook is one branch on a cached flag.
// PS2X_MTVU=1 (threaded; resolved by configure() at runtime init, forced off
// while any dev trace that shares state with the unit is armed): jobs run in
// submit order on one worker thread; the guest-visible result is identical
// to the synchronous runtime (the det-hash is the proof). Rules R1/R2 of the
// report: a CSR load masked away from the unit's bits needs no sync; EE GS
// privileged writes are jobs (CSR stores apply on the EE). Knobs:
//   PS2X_MTVU_LAG=1     R3: VBlankStart waits only for the previous frame's
//                       jobs (full sync on det-hash ticks); up to one frame of
//                       extra display latency, no guest change.
//   PS2X_MTVU_CPUS=a,b  pin the worker (Linux/Android).
//   PS2X_MTVU_JITTER=N  test: sleep 0..N us before each job (host timing only).
//   PS2X_GAME_THREAD_STACK_KB also sizes the worker's stack.
// PS2X_MTVU=census (stage 2', still synchronous, no behaviour change):
//   - counts sync-point hits per reason, and the first sync after each job;
//   - reports a touch() of unit-owned state on the EE thread after a job
//     with no sync in between (VIOLATION lines: a threaded build would race);
//   - with PS2X_MTVU_CENSUS_OUT=<file>, writes one event per line for the
//     offline two-timeline model (tau = EE host ns with unit work removed):
//       J <tau> <unit_ns> <d|f>   job (d = DMA kick, f = VIF1 FIFO write)
//       S <tau> <reason> <detail> every sync hit (guest pc / address in hex;
//                                 repeats with no job in between collapse)
//       V <tau> <tick>            VBlankStart (always a sync)
//     plus a summary line on stderr every 300 vsyncs.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "ThreadNaming.h"
#include "ps2_fpmode.h"
#include "ps2_thread_affinity.h"
#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace ps2_mtvu
{
    enum class Mode : int
    {
        Off = 0,
        Threaded = 1,
        Census = 2
    };

    enum class Reason : uint8_t
    {
        VBlank,
        Vu1Mem,
        GsPrivRead,
        GsPrivReadMasked, // CSR load whose next insn masks away the unit's bits
        GsPrivWrite,
        GsPrivSync,
        Vif1Reg,
        Cmsar1,
        GsHle,
        NativeGif,
        SaveState,
        DtFallback,
        Count
    };

    enum class Site : uint8_t
    {
        GsProcess,
        GsNative,
        GsWriteReg,
        GsPriv,
        GsDrain,
        GsClear,
        GsReadback,
        GsReset,
        ArbSubmit,
        ArbDrain,
        Path3Fifo,
        Vu1Mem,
        Count
    };

    inline const char *reasonName(Reason r)
    {
        static const char *const names[] = {"vblank", "vu1mem", "gsprivread", "gsprivread-masked", "gsprivwrite", "gsprivsync",
                                            "vif1reg", "cmsar1", "gshle", "nativegif", "savestate", "dtfallback"};
        return names[static_cast<unsigned>(r)];
    }

    inline const char *siteName(Site s)
    {
        static const char *const names[] = {"gs-process", "gs-native", "gs-writereg", "gs-priv", "gs-drain",
                                            "gs-clear", "gs-readback", "gs-reset", "arb-submit", "arb-drain",
                                            "path3-fifo", "vu1-mem"};
        return names[static_cast<unsigned>(s)];
    }

    // GF1 H3: PS2Runtime installs (PS2X_GS_HANDOFF_DIET): runs on the unit
    // thread after every job, to deliver the GS worker's deferred wake.
    inline std::function<void()> &jobEndFn()
    {
        static std::function<void()> fn;
        return fn;
    }

    namespace detail
    {
        // -1 = not resolved yet: census comes from the environment on first
        // use; threaded only through configure() (runtime init) or tests.
        inline std::atomic<int> g_mode{-1};
        inline std::atomic<bool> g_lag{false};

        inline int mode()
        {
            int m = g_mode.load(std::memory_order_relaxed);
            if (m < 0)
            {
                const char *e = std::getenv("PS2X_MTVU");
                m = (e && std::strcmp(e, "census") == 0) ? static_cast<int>(Mode::Census) : 0;
                g_mode.store(m, std::memory_order_relaxed);
            }
            return m;
        }

        inline uint64_t nowNs()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
        }

        // Unit depth > 0: this thread is doing unit work (hooks are no-ops).
        inline thread_local int t_unitDepth = 0;
        // Set on the thread that runs VBlankStart / submits jobs (the EE
        // executor). touch() only checks there: the GS worker's own calls and
        // host presentation are downstream consumers, as today.
        inline thread_local bool t_isEe = false;
        // The worker thread itself, and the FBRST snapshot of the job it runs.
        inline thread_local bool t_onWorker = false;
        inline thread_local uint32_t t_jobFbrst = 0u;
        // A masked CSR read (R1) touches the priv block without a sync.
        inline thread_local int t_exempt = 0;

        struct Job
        {
            std::function<void()> fn;
            uint64_t fpControl = 0;
            uint32_t fbrst = 0;
            size_t bytes = 0;
        };

        struct Worker
        {
            static constexpr size_t kMaxJobs = 64u;
            static constexpr size_t kMaxBytes = 64u << 20;

            std::mutex m;
            std::condition_variable cvWork;
            std::condition_variable cvDone;
            std::condition_variable cvSpace;
            std::deque<Job> q;
            size_t qBytes = 0;
            bool stop = false;
            bool started = false;
            std::atomic<uint64_t> submitted{0};
            std::atomic<uint64_t> completed{0};
            uint64_t seqAtPrevVBlank = 0; // EE only
            uint32_t jitterUs = 0;
            std::thread th;
#if defined(__unix__) || defined(__APPLE__)
            pthread_t pth{};
            bool usePthread = false;
#endif
            // Threaded-mode receipts (EE only).
            std::array<uint64_t, static_cast<size_t>(Reason::Count)> waits{};
            std::array<uint64_t, static_cast<size_t>(Reason::Count)> waitNs{};
            std::array<uint64_t, static_cast<size_t>(Site::Count)> violations{};
            uint64_t violationsTotal = 0;
            uint64_t jobs = 0;

            ~Worker()
            {
                {
                    std::lock_guard<std::mutex> lock(m);
                    stop = true;
                }
                cvWork.notify_all();
#if defined(__unix__) || defined(__APPLE__)
                if (usePthread)
                    pthread_join(pth, nullptr);
#endif
                if (th.joinable())
                    th.join();
            }

            void loop()
            {
                t_onWorker = true;
                t_unitDepth = 1;
                ThreadNaming::SetCurrentThreadName("MTVU");
                if (const char *cpus = std::getenv("PS2X_MTVU_CPUS"))
                {
                    if (cpus[0] != '\0')
                    {
                        const int rc = ps2x::pinCurrentThreadToCpus(ps2x::parseCpuList(cpus));
                        std::fprintf(stderr, "[affinity] mtvu thread cpus=%s rc=%d\n", cpus, rc);
                    }
                }
                uint64_t rng = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1u;
                for (;;)
                {
                    Job *job = nullptr;
                    {
                        std::unique_lock<std::mutex> lock(m);
                        cvWork.wait(lock, [&] { return stop || !q.empty(); });
                        if (stop)
                            return;
                        job = &q.front(); // stays queued (deque front is stable) until done
                    }
                    if (jitterUs != 0u)
                    {
                        rng ^= rng << 13;
                        rng ^= rng >> 7;
                        rng ^= rng << 17;
                        std::this_thread::sleep_for(std::chrono::microseconds(rng % (jitterUs + 1u)));
                    }
                    ps2_fpmode::writeControl(job->fpControl);
                    t_jobFbrst = job->fbrst;
                    try
                    {
                        job->fn();
                    }
                    catch (const std::exception &e)
                    {
                        std::fprintf(stderr, "[mtvu] FATAL: unit job threw: %s\n", e.what());
                        std::abort();
                    }
                    catch (...)
                    {
                        std::fprintf(stderr, "[mtvu] FATAL: unit job threw\n");
                        std::abort();
                    }
                    if (const auto &end = jobEndFn())
                        end();
                    {
                        std::lock_guard<std::mutex> lock(m);
                        qBytes -= job->bytes;
                        q.pop_front();
                        completed.store(completed.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
                    }
                    cvDone.notify_all();
                    cvSpace.notify_all();
                }
            }

            void start()
            {
                started = true;
                long stackKb = 0;
                if (const char *env = std::getenv("PS2X_GAME_THREAD_STACK_KB"))
                    stackKb = std::strtol(env, nullptr, 10);
#if defined(__unix__) || defined(__APPLE__)
                if (stackKb > 0)
                {
                    pthread_attr_t attr;
                    pthread_attr_init(&attr);
                    if (pthread_attr_setstacksize(&attr, static_cast<size_t>(stackKb) * 1024u) == 0 &&
                        pthread_create(&pth, &attr, +[](void *p) -> void * {
                            static_cast<Worker *>(p)->loop();
                            return nullptr; }, this) == 0)
                        usePthread = true;
                    pthread_attr_destroy(&attr);
                    if (usePthread)
                        return;
                }
#endif
                th = std::thread([this] { loop(); });
            }

            void submit(Job &&job)
            {
                if (!started)
                    start();
                {
                    std::unique_lock<std::mutex> lock(m);
                    cvSpace.wait(lock, [&] {
                        return q.size() < kMaxJobs && (q.empty() || qBytes + job.bytes <= kMaxBytes);
                    });
                    qBytes += job.bytes;
                    q.push_back(std::move(job));
                    submitted.store(submitted.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
                }
                ++jobs;
                cvWork.notify_one();
            }

            bool pending() const
            {
                return completed.load(std::memory_order_acquire) != submitted.load(std::memory_order_relaxed);
            }

            // Wait until `target` jobs have completed; returns ns waited.
            uint64_t waitFor(uint64_t target)
            {
                if (completed.load(std::memory_order_acquire) >= target)
                    return 0u;
                const uint64_t t0 = nowNs();
                for (int spin = 0; spin < 256; ++spin)
                {
                    if (completed.load(std::memory_order_acquire) >= target)
                        return nowNs() - t0;
                    std::this_thread::yield();
                }
                std::unique_lock<std::mutex> lock(m);
                cvDone.wait(lock, [&] { return completed.load(std::memory_order_acquire) >= target; });
                return nowNs() - t0;
            }
        };

        inline Worker &worker()
        {
            static Worker w;
            return w;
        }

        inline void threadedSummary(uint64_t tick)
        {
            Worker &w = worker();
            std::fprintf(stderr, "[mtvu] threaded tick=%llu lag=%d jobs=%llu violations=%llu waits:",
                         static_cast<unsigned long long>(tick), g_lag.load(std::memory_order_relaxed) ? 1 : 0,
                         static_cast<unsigned long long>(w.jobs), static_cast<unsigned long long>(w.violationsTotal));
            for (size_t i = 0; i < w.waits.size(); ++i)
                if (w.waits[i] != 0u)
                    std::fprintf(stderr, " %s=%llu/%.1fms", reasonName(static_cast<Reason>(i)),
                                 static_cast<unsigned long long>(w.waits[i]), w.waitNs[i] / 1e6);
            for (size_t i = 0; i < w.violations.size(); ++i)
                if (w.violations[i] != 0u)
                    std::fprintf(stderr, " V:%s=%llu", siteName(static_cast<Site>(i)),
                                 static_cast<unsigned long long>(w.violations[i]));
            std::fprintf(stderr, "\n");
        }

        struct Census
        {
            bool dirty = false;       // a job ran and no sync followed yet
            uint64_t unitNs = 0;      // total unit work (removed from tau)
            uint64_t jobs = 0;
            uint64_t fifoJobs = 0;
            uint64_t snapshotBytes = 0;
            uint64_t snapshotNs = 0;
            uint64_t violationsTotal = 0;
            std::array<uint64_t, static_cast<size_t>(Reason::Count)> hits{};
            std::array<uint64_t, static_cast<size_t>(Reason::Count)> first{};
            std::array<uint64_t, static_cast<size_t>(Site::Count)> violations{};
            uint64_t tick = 0;
            Reason lastReason = Reason::Count;
            uint32_t lastDetail = 0;
            FILE *out = nullptr;
            bool outTried = false;
            std::function<bool()> dtFallback;
            std::vector<uint8_t> scratch;
        };

        inline Census &census()
        {
            static Census c;
            return c;
        }

        inline uint64_t tau()
        {
            return nowNs() - census().unitNs;
        }

        inline FILE *out()
        {
            Census &c = census();
            if (!c.outTried)
            {
                c.outTried = true;
                if (const char *path = std::getenv("PS2X_MTVU_CENSUS_OUT"))
                {
                    c.out = std::fopen(path, "w");
                    if (c.out)
                        std::setvbuf(c.out, nullptr, _IOFBF, 1u << 20);
                }
            }
            return c.out;
        }

        inline void syncSlow(Reason r, uint32_t detail)
        {
            Census &c = census();
            ++c.hits[static_cast<size_t>(r)];
            if (c.dirty)
            {
                c.dirty = false;
                ++c.first[static_cast<size_t>(r)];
            }
            // Spin loops re-hit the same site with no job between: log once.
            if (r == c.lastReason && detail == c.lastDetail)
                return;
            c.lastReason = r;
            c.lastDetail = detail;
            if (FILE *f = out())
                std::fprintf(f, "S %llu %s %x\n", static_cast<unsigned long long>(tau()), reasonName(r), detail);
        }

        inline void touchSlow(Site s)
        {
            Census &c = census();
            if (!c.dirty)
                return;
            ++c.violations[static_cast<size_t>(s)];
            if (c.violationsTotal++ < 16u)
                std::fprintf(stderr, "[mtvu] VIOLATION site=%s tick=%llu (unit state touched after a job, no sync)\n",
                             siteName(s), static_cast<unsigned long long>(c.tick));
        }

        inline void summary()
        {
            Census &c = census();
            std::fprintf(stderr, "[mtvu] census tick=%llu jobs=%llu fifo=%llu unit_ms=%.1f snap_bytes=%llu snap_ms=%.2f violations=%llu hits:",
                         static_cast<unsigned long long>(c.tick), static_cast<unsigned long long>(c.jobs),
                         static_cast<unsigned long long>(c.fifoJobs), c.unitNs / 1e6,
                         static_cast<unsigned long long>(c.snapshotBytes), c.snapshotNs / 1e6,
                         static_cast<unsigned long long>(c.violationsTotal));
            for (size_t i = 0; i < c.hits.size(); ++i)
                if (c.hits[i] != 0u)
                    std::fprintf(stderr, " %s=%llu/%llu", reasonName(static_cast<Reason>(i)),
                                 static_cast<unsigned long long>(c.hits[i]),
                                 static_cast<unsigned long long>(c.first[i]));
            for (size_t i = 0; i < c.violations.size(); ++i)
                if (c.violations[i] != 0u)
                    std::fprintf(stderr, " V:%s=%llu", siteName(static_cast<Site>(i)),
                                 static_cast<unsigned long long>(c.violations[i]));
            std::fprintf(stderr, "\n");
        }
    }

    inline bool active()
    {
        return detail::mode() != 0;
    }

    inline bool census()
    {
        return detail::mode() == static_cast<int>(Mode::Census);
    }

    inline bool threaded()
    {
        return detail::mode() == static_cast<int>(Mode::Threaded);
    }

    inline bool lag()
    {
        return detail::g_lag.load(std::memory_order_relaxed);
    }

    // Runtime init, before the game thread starts. diagArmed: a dev trace
    // that shares state with unit code is on, so PS2X_MTVU=1 stays off.
    inline void configure(bool diagArmed)
    {
        const char *e = std::getenv("PS2X_MTVU");
        int m = 0;
        if (e && std::strcmp(e, "census") == 0)
            m = static_cast<int>(Mode::Census);
        else if (e && std::strcmp(e, "1") == 0)
            m = diagArmed ? 0 : static_cast<int>(Mode::Threaded);
        const char *l = std::getenv("PS2X_MTVU_LAG");
        const bool lagOn = m == static_cast<int>(Mode::Threaded) && l && std::strcmp(l, "1") == 0;
        if (const char *j = std::getenv("PS2X_MTVU_JITTER"))
            detail::worker().jitterUs = static_cast<uint32_t>(std::strtoul(j, nullptr, 10));
        detail::g_lag.store(lagOn, std::memory_order_relaxed);
        detail::g_mode.store(m, std::memory_order_relaxed);
        if (e && std::strcmp(e, "1") == 0)
            std::fprintf(stderr, "[mtvu] mode=%s lag=%d jitter_us=%u%s\n", m ? "threaded" : "off",
                         lagOn ? 1 : 0, detail::worker().jitterUs,
                         diagArmed ? " (a dev trace is armed: threaded mode refused)" : "");
    }

    // Tests: force a mode (drains the worker first).
    inline void setModeForTest(Mode m, bool lagOn = false, uint32_t jitterUs = 0u);

    inline bool onWorker()
    {
        return detail::t_onWorker;
    }

    inline uint32_t jobFbrst()
    {
        return detail::t_jobFbrst;
    }

    // Threaded: wait for every submitted job.
    inline void syncAll(Reason r = Reason::Count)
    {
        detail::Worker &w = detail::worker();
        if (!w.pending())
            return;
        const uint64_t ns = w.waitFor(w.submitted.load(std::memory_order_relaxed));
        if (r != Reason::Count)
        {
            ++w.waits[static_cast<size_t>(r)];
            w.waitNs[static_cast<size_t>(r)] += ns;
        }
    }

    // EE side, before touching unit-owned state.
    inline void sync(Reason r, uint32_t detail = 0u)
    {
        if (!active() || detail::t_unitDepth != 0)
            return;
        if (threaded())
        {
            syncAll(r);
            return;
        }
        detail::syncSlow(r, detail);
    }

    // Threaded: queue unit work. fbrst = the kicking context's VU0 FBRST
    // (VU1 D/T enables) as the synchronous MSCAL callback would read it.
    inline void submit(std::function<void()> fn, size_t bytes, uint32_t fbrst)
    {
        detail::Job job;
        job.fn = std::move(fn);
        job.fpControl = ps2_fpmode::readControl();
        job.fbrst = fbrst;
        job.bytes = bytes;
        detail::t_isEe = true;
        detail::worker().submit(std::move(job));
    }

    // R1: a masked CSR read touches the priv block without a sync.
    struct ExemptScope
    {
        explicit ExemptScope(bool on) : m_on(on)
        {
            if (m_on)
                ++detail::t_exempt;
        }
        ~ExemptScope()
        {
            if (m_on)
                --detail::t_exempt;
        }
        bool m_on;
        ExemptScope(const ExemptScope &) = delete;
        ExemptScope &operator=(const ExemptScope &) = delete;
    };

    inline void setModeForTest(Mode m, bool lagOn, uint32_t jitterUs)
    {
        syncAll();
        detail::worker().jitterUs = jitterUs;
        detail::g_lag.store(lagOn && m == Mode::Threaded, std::memory_order_relaxed);
        detail::g_mode.store(static_cast<int>(m), std::memory_order_relaxed);
    }

    // Census classification of a guest load from the GS priv range at pc.
    // The unit writes only CSR bits 0-1 (SIGNAL/FINISH) and SIGLBLID
    // (gs_frontend.cpp); CSR FIFO is HLE'd constant and VSINT/FIELD come from
    // the EE's VBlank store. A CSR load whose next instruction is
    // `andi rt, rt, imm` with imm clear of the unit bits leaves architectural
    // state that cannot depend on the unit (-> GsPrivReadMasked).
    inline Reason privReadReason(const uint8_t *rdram, uint32_t pc, uint32_t vaddr, uint32_t bytes)
    {
        const uint32_t phys = vaddr & 0x1FFFFFFFu;
        if ((phys & ~7u) != 0x12001000u || !rdram)
            return Reason::GsPrivRead;
        const uint32_t pcPhys = pc & 0x01FFFFFCu; // 32 MB RDRAM
        if (pcPhys + 8u > 0x02000000u)
            return Reason::GsPrivRead;
        uint32_t prev = 0, load = 0, next = 0;
        if (pcPhys >= 4u)
            std::memcpy(&prev, rdram + pcPhys - 4u, 4);
        std::memcpy(&load, rdram + pcPhys, 4);
        std::memcpy(&next, rdram + pcPhys + 4u, 4);
        // In a branch delay slot the next instruction executed is not pc + 4:
        // no proof (conservative: every jump/branch/REGIMM/COP branch form).
        const uint32_t pop = prev >> 26;
        const bool prevBranch = (pop == 0u && ((prev & 0x3Fu) == 8u || (prev & 0x3Fu) == 9u)) ||
                                pop == 1u || pop == 2u || pop == 3u || (pop >= 4u && pop <= 7u) ||
                                (pop >= 0x14u && pop <= 0x17u) ||
                                ((pop >= 0x10u && pop <= 0x12u) && ((prev >> 21) & 31u) == 8u);
        if (prevBranch)
            return Reason::GsPrivRead;
        const uint32_t rt = (load >> 16) & 31u;
        uint64_t mask = bytes >= 8u ? ~0ull : ((1ull << (bytes * 8u)) - 1u);
        if ((next >> 26) == 0x0Cu && ((next >> 21) & 31u) == rt && ((next >> 16) & 31u) == rt && rt != 0u)
            mask &= static_cast<uint64_t>(next & 0xFFFFu);
        mask <<= (phys & 7u) * 8u;
        return (mask & 0x3ull) == 0u ? Reason::GsPrivReadMasked : Reason::GsPrivRead;
    }

    // Inside a unit-owned object: must be unit work or follow a sync.
    inline void touch(Site s)
    {
        if (!active() || detail::t_unitDepth != 0 || !detail::t_isEe || detail::t_exempt != 0)
            return;
        if (threaded())
        {
            detail::Worker &w = detail::worker();
            if (!w.pending())
                return;
            ++w.violations[static_cast<size_t>(s)];
            if (w.violationsTotal++ < 16u)
                std::fprintf(stderr, "[mtvu] VIOLATION site=%s (unit state touched while jobs are queued)\n",
                             siteName(s));
            return;
        }
        detail::touchSlow(s);
    }

    // PS2Runtime installs the D/T rule (REPORT.md map #10): true = the
    // current guest context has VU1 D/T stops enabled or pending, so VIF1
    // work runs inline on the EE after a sync instead of as a job.
    inline void setDtFallbackFn(std::function<bool()> fn)
    {
        detail::census().dtFallback = std::move(fn);
    }

    // PS2Runtime installs: the current guest context's VU0 FBRST.
    inline std::function<uint32_t()> &fbrstFn()
    {
        static std::function<uint32_t()> fn;
        return fn;
    }

    inline uint32_t currentFbrst()
    {
        const auto &fn = fbrstFn();
        return fn ? fn() : 0u;
    }

    inline bool dtFallback()
    {
        const auto &fn = detail::census().dtFallback;
        return fn && fn();
    }

    // Census only: time the copy a threaded job would take of `bytes` of
    // non-chain DMA source (EE-side cost, stays in tau).
    inline void noteSnapshot(const uint8_t *src, size_t bytes)
    {
        if (!active() || bytes == 0u || !src)
            return;
        detail::Census &c = detail::census();
        const uint64_t t0 = detail::nowNs();
        if (c.scratch.size() < bytes)
            c.scratch.resize(bytes);
        std::memcpy(c.scratch.data(), src, bytes);
        c.snapshotNs += detail::nowNs() - t0;
        c.snapshotBytes += bytes;
    }

    // One unit job (possibly in segments: pause() around EE-only work).
    class JobScope
    {
    public:
        JobScope(bool on, char kind)
            : m_on(on && census() && detail::t_unitDepth == 0), m_kind(kind)
        {
            if (!m_on)
                return;
            detail::t_isEe = true;
            m_tau = detail::tau();
            resume();
        }
        ~JobScope()
        {
            if (!m_on)
                return;
            pause();
            detail::Census &c = detail::census();
            c.unitNs += m_ns;
            ++c.jobs;
            if (m_kind == 'f')
                ++c.fifoJobs;
            c.dirty = true;
            if (FILE *f = detail::out())
                std::fprintf(f, "J %llu %llu %c\n", static_cast<unsigned long long>(m_tau),
                             static_cast<unsigned long long>(m_ns), m_kind);
        }
        void pause()
        {
            if (!m_on || !m_running)
                return;
            m_ns += detail::nowNs() - m_start;
            --detail::t_unitDepth;
            m_running = false;
        }
        void resume()
        {
            if (!m_on || m_running)
                return;
            ++detail::t_unitDepth;
            m_start = detail::nowNs();
            m_running = true;
        }
        JobScope(const JobScope &) = delete;
        JobScope &operator=(const JobScope &) = delete;

    private:
        bool m_on = false;
        bool m_running = false;
        char m_kind = 'd';
        uint64_t m_tau = 0;
        uint64_t m_start = 0;
        uint64_t m_ns = 0;
    };

    // EeScheduler VBlankStart (after the pacer sleep). hashTick: this tick
    // emits a det-hash (reads VU1 memory), so it always syncs fully.
    inline void vblank(uint64_t tick, bool hashTick = true)
    {
        if (!active())
            return;
        detail::t_isEe = true;
        if (threaded())
        {
            detail::Worker &w = detail::worker();
            const uint64_t now = w.submitted.load(std::memory_order_relaxed);
            if (lag() && !hashTick)
            {
                // R3: the previous frame's jobs must be done; this frame's may run on.
                if (w.completed.load(std::memory_order_acquire) < w.seqAtPrevVBlank)
                {
                    const uint64_t ns = w.waitFor(w.seqAtPrevVBlank);
                    ++w.waits[static_cast<size_t>(Reason::VBlank)];
                    w.waitNs[static_cast<size_t>(Reason::VBlank)] += ns;
                }
            }
            else
                syncAll(Reason::VBlank);
            w.seqAtPrevVBlank = now;
            if ((tick % 300u) == 0u)
                detail::threadedSummary(tick);
            return;
        }
        detail::Census &c = detail::census();
        c.tick = tick;
        sync(Reason::VBlank);
        if (FILE *f = detail::out())
        {
            std::fprintf(f, "V %llu %llu\n", static_cast<unsigned long long>(detail::tau()),
                         static_cast<unsigned long long>(tick));
            if ((tick % 60u) == 0u)
                std::fflush(f);
        }
        if ((tick % 300u) == 0u)
            detail::summary();
    }
}

#endif
