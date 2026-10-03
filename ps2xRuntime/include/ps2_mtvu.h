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
//   PS2X_MTVU_FINISH_EE=1  MQ2 (with PS2X_GS_FINISH_TIMING=pcsx2): a DMA kick
//                       whose PATH3 GIF pieces carry A+D FINISH sets CSR.FINISH
//                       on the EE at submit, in program order (PCSX2's EE-side
//                       GIF unit); the unit then consumes one credit per such
//                       write instead of setting FINISH again. PATH1/PATH2
//                       FINISH stays unit-side.
//   PS2X_MTVU_VIF1_STAT_FREE=1  MQ3: a guest VIF1_STAT (FDR) write skips the
//                       Vif1Reg unit sync (it touches no unit-owned state).
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
#include <type_traits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "ThreadNaming.h"
#include "ps2_adpf.h"
#include "ps2_fpmode.h"
#include "ps2_perf_log.h"
#include "ps2_thread_affinity.h"
#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace ps2_mtvu
{
    // VPL1: one GIF-stage op (PS2X_MTVU_GIF_STAGE=1). The MTVU thread turns
    // each unit action on the GIF arbiter / GS frontend into an op, in
    // program order; the MTVU-GIF thread runs them in the same order
    // (local/research/VPL1/REPORT.md §1).
    struct GifOp
    {
        enum class Kind : uint8_t
        {
            Submit, // GifArbiter::submitStaged(path, bytes, directHl)
            Drain,  // GifDrainBatch + GifArbiter::drain()
            Call,   // a GS-frontend call from a unit job (unitGsCall)
            JobEnd  // the unit job is complete once this op has run
        };
        Kind kind = Kind::Drain;
        uint8_t path = 0;
        bool directHl = false;
        uint32_t acct = 0;  // bytes counted against the in-flight cap
        std::vector<uint8_t> bytes;
        std::function<void()> fn;
    };

    // VPL2: one record of the VIF-stage log (PS2X_MTVU_VIF_STAGE=1). The
    // MTVU-VIF thread parses a unit job's VIF1 stream and turns every
    // VU-side action into a record, in program order; the MTVU thread runs
    // them in the same order (local/research/VPL2/REPORT.md §1.3). 16-byte
    // header, payload right after it, `size` covers both (16-aligned).
    enum class VifRecKind : uint8_t
    {
        Wrap,     // rest of the ring is unused; continue at offset 0
        JobBegin, // a = fbrst, payload u64 fpControl
        JobEnd,   // VPL1 GIF JobEnd
        Call,     // payload: std::function<void()>*, std::atomic<bool>* done (f = 1)
        Block,    // UNPACK bulk: qwords (a + i) & 0x3FF, i < n, = payload
        Masked,   // UNPACK generic: n x {u16 qword, u16 byte mask, 16 bytes}
        Mpg,      // VU1 code at byte a, b bytes = payload
        Mscal,    // f = 0 MSCAL / 1 MSCNT, a = startPC, b = top | itop << 16
        GifCopy,  // submitGifPacket(f & 3, bytes...), payload std::vector<uint8_t>*
        Msk3,     // MSKPATH3, a = imm
        P3Drain,  // drainPath3IfUnmasked()
        ArbDrain  // arbDrain()
    };
    struct VifRec
    {
        uint32_t size;
        VifRecKind kind;
        uint8_t f;
        uint16_t n;
        uint32_t a;
        uint32_t b;
    };
    static_assert(sizeof(VifRec) == 16u, "VifRec header is 16 bytes");
    using VifExecFn = void (*)(void *opaque, const VifRec &rec);

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
        FinishPoll, // GE3 Part 6: FINISH-only read found FINISH clear; unit queue only
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
                                            "vif1reg", "cmsar1", "gshle", "nativegif", "savestate", "dtfallback", "finishpoll"};
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
        // MQ2: EE-side PATH3 FINISH (PS2X_MTVU_FINISH_EE=1, threaded only).
        inline std::atomic<bool> g_finishEe{false};
        inline std::atomic<bool> g_vif1StatFree{false};         // MQ3
        inline std::atomic<uint64_t> g_vif1StatFreeN{0};        // MQ3: writes that skipped the sync
        inline std::atomic<uint64_t> g_vif1StatFreePending{0};  // MQ3: ... with unit jobs queued
        inline std::atomic<uint64_t> g_finishEeCredits{0}; // EE-set writes the unit has not reached yet
        inline std::atomic<uint64_t> g_finishEeSets{0};    // EE submits that set FINISH
        inline std::atomic<uint64_t> g_finishEeSkips{0};   // unit PATH3 FINISH writes covered by a credit
        inline std::atomic<uint64_t> g_finishEeUnmatched{0}; // unit PATH3 FINISH writes with no credit (set as before)
        // MQ2: the GIF path of the packet the arbiter is emitting on this
        // thread (0 = none, e.g. a direct XGKICK); set around the process call.
        inline thread_local uint8_t t_gifEmitPath = 0u;

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

        // TT1/GT1(a): owner ids + EE-owned plain state replace four
        // thread_locals (t_unitDepth/t_isEe/t_onWorker/t_exempt) that touch()
        // read on every GS-priv 64-bit load (~0.41 ms/f of TLSDESC on the
        // Odin, GT1 §6). Audit (TT1 report): the EE executor is the only
        // thread that submits/vblanks (noteEeThread, first-wins); unit
        // threads (the MTVU worker + the VU1W engine worker) note/forget
        // themselves; JobScope/ExemptScope scopes run EE-side only.
        // touch()/sync() do one get_id + relaxed loads, no TLS.
        // t_jobFbrst stays thread-local: it is read on both threads with
        // thread-specific values (EE reads 0, the worker its snapshot).
        static_assert(std::is_trivially_copyable_v<std::thread::id>);
        inline std::atomic<std::thread::id> g_eeTid{};
        inline std::atomic<std::thread::id> g_mtvuTid{};
        inline std::atomic<int> g_eeUnitDepth{0};
        inline std::atomic<int> g_eeExempt{0};
        // Live unit threads (MTVU worker + VU1W): at most 2, slots recycle
        // via forgetUnitThread on thread stop. touch()/sync() skip there.
        static constexpr int kMaxUnitThreads = 4;
        inline std::atomic<std::thread::id> g_unitTids[kMaxUnitThreads]{};
        inline thread_local uint32_t t_jobFbrst = 0u;
        inline void noteEeThread() noexcept
        {
            std::thread::id empty;
            g_eeTid.compare_exchange_strong(empty, std::this_thread::get_id(),
                                            std::memory_order_relaxed);
        }
        inline bool isEeThread() noexcept
        {
            return std::this_thread::get_id() == g_eeTid.load(std::memory_order_relaxed);
        }
        inline void noteUnitThread() noexcept
        {
            const std::thread::id me = std::this_thread::get_id();
            for (int i = 0; i < kMaxUnitThreads; ++i)
            {
                if (g_unitTids[i].load(std::memory_order_relaxed) == me)
                    return;
                std::thread::id empty;
                if (g_unitTids[i].compare_exchange_strong(empty, me, std::memory_order_relaxed))
                    return;
            }
        }
        inline void forgetUnitThread() noexcept
        {
            const std::thread::id me = std::this_thread::get_id();
            for (int i = 0; i < kMaxUnitThreads; ++i)
            {
                if (g_unitTids[i].load(std::memory_order_relaxed) == me)
                    g_unitTids[i].store(std::thread::id{}, std::memory_order_relaxed);
            }
        }
        inline bool isUnitThread() noexcept
        {
            const std::thread::id me = std::this_thread::get_id();
            for (int i = 0; i < kMaxUnitThreads; ++i)
            {
                if (g_unitTids[i].load(std::memory_order_relaxed) == me)
                    return true;
            }
            return false;
        }
        // RAII: the mark dies with the thread on every exit path, so a
        // recycled OS id never reads back as live.
        struct UnitThreadGuard
        {
            UnitThreadGuard() noexcept { noteUnitThread(); }
            ~UnitThreadGuard() noexcept { forgetUnitThread(); }
        };
        struct MtvuThreadGuard
        {
            MtvuThreadGuard() noexcept
            {
                g_mtvuTid.store(std::this_thread::get_id(), std::memory_order_relaxed);
                noteUnitThread();
            }
            ~MtvuThreadGuard() noexcept
            {
                g_mtvuTid.store(std::thread::id{}, std::memory_order_relaxed);
                forgetUnitThread();
            }
        };

        struct Job
        {
            std::function<void()> fn;
            uint64_t fpControl = 0;
            uint32_t fbrst = 0;
            size_t bytes = 0;
            bool vif = false; // VPL2: VIF1 work (runs on the VIF stage when it is on)
        };

        // VPL1 GIF stage. g_gifStage: routing on (set before the game thread
        // runs, cleared after a full sync). g_gifTid: the MTVU-GIF thread.
        inline std::atomic<bool> g_gifStage{false};
        inline std::atomic<std::thread::id> g_gifTid{};
        inline void workerJobDoneFromGif(); // after Worker

        // MW2: PS2X_MTVU_STAGE_WAIT. park: park the GIF stage only (the
        // IHP1 lever: its 50 us yield-spin is 77-84 % of the MTVU-GIF
        // thread). park_all: park the VIF log too. park2: GIF-only park with
        // a lock-free producer wake (no mutex on the hot publish path; the
        // waiter's predicate re-check makes the lock unnecessary — a notify
        // before the sleep finds its work via `tail != cHead`, one after
        // wakes it; wake points are unchanged, so no latency beyond one job).
        // park3: GIF-only park with a BATCHED wake — the producer notifies a
        // sleeping consumer only on job boundaries (JobEnd/Drain/Call ops,
        // every job ends with one published immediately) or past kWakeFill
        // pending ops, not on every publish. No work is lost: the waiter
        // re-checks `tail != cHead` under `m` before sleeping, so any publish
        // is seen; a skipped wake only delays intra-job GIF-side overlap
        // until the job's boundary publish. Worst case: one unit job of GIF
        // start delay (µs-scale MTVU work, sub-tick; the 100 ms cv-timeout is
        // the pathological backstop). JobEnd itself always wakes, so
        // EE-visible completion (completed++) latency is unchanged.
        // A parked stage spins kStageParkSpinNs (<= 4 us) with a CPU pause
        // (arm64 `yield`, x86 `pause`; the clock is read every 16 polls),
        // then the existing condvar sleep. spin (the default in this lane):
        // today's exact path, 50 us of std::this_thread::yield() + clock
        // reads, then the same sleep. Same wake protocol every way: every
        // publish is a `tail` release-store + seq_cst fence followed by a
        // notify iff `sleeping`, and the sleep sets `sleeping`, fences, and
        // re-checks `tail` before waiting, so a publish that lands between
        // the spin and the sleep is seen by the re-check and no wake is lost.
        static constexpr uint64_t kStageParkSpinNs = 4000u;
        inline int parseStagePark(const char *e)
        {
            if (e && std::strcmp(e, "park3") == 0)
                return 4;
            if (e && std::strcmp(e, "park2") == 0)
                return 3;
            if (e && std::strcmp(e, "park_all") == 0)
                return 2;
            if (e && std::strcmp(e, "park") == 0)
                return 1;
            return 0;
        }
        inline int &stageWaitMode()
        {
            static int v = -1; // -1 unresolved; 0 spin, 1 park, 2 park_all, 3 park2, 4 park3 (tests set it directly)
            return v;
        }
        inline const char *stageModeName(int v)
        {
            return v >= 4 ? "park3" : (v == 3 ? "park2" : (v == 2 ? "park_all" : (v == 1 ? "park" : "spin")));
        }
        inline int stageMode()
        {
            int v = stageWaitMode();
            if (v < 0)
            {
                v = parseStagePark(std::getenv("PS2X_MTVU_STAGE_WAIT"));
                stageWaitMode() = v;
                std::fprintf(stderr, "[mtvu] stage-wait=%s\n", stageModeName(v));
            }
            return v;
        }
        inline bool gifPark() { return stageMode() == 1 || stageMode() >= 3; }
        inline bool vifPark() { return stageMode() == 2; }
        inline void setStageWaitForTest(int v) { stageWaitMode() = v; }
        static inline void stageCpuPause()
        {
#if defined(__aarch64__) || defined(_M_ARM64)
            __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }

        // Ordered SPSC ring of GifOps: producer = the MTVU thread, consumer =
        // the MTVU-GIF thread. Batched publishes (every kPublishEvery ops and
        // at JobEnd), no shared atomic per op, and a wake only when the other
        // side has announced it sleeps (MP1: futex round trips were the cost).
        struct GifStage
        {
            static constexpr size_t kSlots = 4096u;
            static constexpr size_t kMask = kSlots - 1u;
            static constexpr uint64_t kMaxBytes = 16ull << 20;
            static constexpr uint64_t kPublishEvery = 32u;
            static constexpr uint64_t kSpinNs = 50000u;

            std::unique_ptr<GifOp[]> slots;
            std::function<void(GifOp &)> exec; // Submit/Drain (PS2Memory)
            // Producer-private (MTVU thread).
            alignas(64) uint64_t pTail = 0;
            uint64_t pPub = 0;
            uint64_t pBytes = 0;
            uint64_t pHeadCache = 0;
            uint64_t pDoneBytesCache = 0;
            // Published by the producer.
            alignas(64) std::atomic<uint64_t> tail{0};
            // Published by the consumer.
            alignas(64) std::atomic<uint64_t> head{0};
            std::atomic<uint64_t> doneBytes{0};
            alignas(64) std::atomic<bool> sleeping{false};
            std::atomic<bool> producerWaiting{false};
            std::atomic<bool> stop{false};
            // Consumer-private (MTVU-GIF thread).
            alignas(64) uint64_t cHead = 0;
            uint64_t cTailCache = 0;
            uint64_t cBytes = 0;
            uint64_t cPub = 0;
            bool cDirty = false; // GS work since the last wake flush
            std::mutex m;
            std::condition_variable cvConsumer;
            std::condition_variable cvProducer;
            std::thread th;
            bool running = false; // EE / init only
            std::function<void()> testBeforePark; // suite hook: runs after the spin, before the sleep
            // Receipts (logged only).
            std::atomic<uint64_t> nPub{0};
            std::atomic<uint64_t> nWakes{0}; // producer notifies sent while the consumer slept
            std::atomic<uint64_t> nSleeps{0};
            std::atomic<uint64_t> nFullWaits{0};
            std::atomic<uint64_t> nEscapes{0};
            std::atomic<uint64_t> busyNs{0};    // since the last vblank (perf stage)
            std::atomic<uint64_t> busyNsWin{0}; // since the last summary
            uint64_t summaryTail = 0;           // EE only

            // --- producer (MTVU thread) ---
            bool roomFor(uint32_t acct)
            {
                if (pTail - pHeadCache >= kSlots)
                    return false;
                // An empty ring always takes one op (an oversize packet must
                // still make progress, as the GS worker's byte cap does).
                return pTail == pHeadCache || pBytes - pDoneBytesCache + acct <= kMaxBytes;
            }
            void refreshCaches()
            {
                pHeadCache = head.load(std::memory_order_acquire);
                pDoneBytesCache = doneBytes.load(std::memory_order_acquire);
            }
            // Backstop so a huge boundary-free job still wakes the consumer:
            // at most 1/4 of the ring can sit un-noticed (pHeadCache may be
            // stale, which only over-wakes, never under).
            static constexpr uint64_t kWakeFill = 1024u;
            // boundary: this publish carries a job boundary (JobEnd/Drain/
            // Call) or comes from the blocked-producer path (producerWait/
            // fence: rare and latency-sensitive, always wakes).
            void publish(bool boundary)
            {
                if (pPub == pTail)
                    return;
                pPub = pTail;
                tail.store(pTail, std::memory_order_release);
                nPub.fetch_add(1u, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (sleeping.load(std::memory_order_relaxed))
                {
                    const int mode = stageMode();
                    // park3: batch the wake to a boundary/fill, not every publish.
                    // refreshCaches first: pHeadCache goes stale in steady
                    // streaming (refreshed only when the ring fills), and a
                    // stale cache makes the fill backstop fire on every
                    // publish. One acquire-load per publish while parked.
                    if (mode == 4)
                        refreshCaches();
                    if (mode == 4 && !boundary && pTail - pHeadCache < kWakeFill)
                        return;
                    nWakes.fetch_add(1u, std::memory_order_relaxed);
                    if (mode >= 3)
                    {
                        // park2/park3: no mutex on the hot publish path (see
                        // the MW2 header note for the no-lost-wake argument).
                        cvConsumer.notify_one();
                    }
                    else
                    {
                        {
                            std::lock_guard<std::mutex> lock(m);
                        }
                        cvConsumer.notify_one();
                    }
                }
            }
            // Wait until `done()` holds (the consumer made room / ran
            // everything). The consumer notifies only while producerWaiting.
            template <typename Pred>
            void producerWait(Pred done)
            {
                publish(true);
                for (int spin = 0; spin < 256; ++spin)
                {
                    refreshCaches();
                    if (done() || stop.load(std::memory_order_relaxed))
                        return;
                    std::this_thread::yield();
                }
                producerWaiting.store(true, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                {
                    std::unique_lock<std::mutex> lock(m);
                    for (;;)
                    {
                        refreshCaches();
                        if (done() || stop.load(std::memory_order_relaxed))
                            break;
                        cvProducer.wait_for(lock, std::chrono::milliseconds(1));
                    }
                }
                producerWaiting.store(false, std::memory_order_relaxed);
            }
            // A free slot (null once the stage is stopping: the op is dropped).
            GifOp *claim(uint32_t acct)
            {
                if (!roomFor(acct))
                {
                    refreshCaches();
                    if (!roomFor(acct))
                    {
                        nFullWaits.fetch_add(1u, std::memory_order_relaxed);
                        producerWait([&] { return roomFor(acct); });
                    }
                }
                if (stop.load(std::memory_order_relaxed))
                    return nullptr;
                return &slots[pTail & kMask];
            }
            void commit(const GifOp &op, bool publishNow)
            {
                ++pTail;
                pBytes += op.acct;
                if (publishNow || pTail - pPub >= kPublishEvery)
                    publish(op.kind != GifOp::Kind::Submit);
            }
            // Wait until the consumer has run every op pushed so far.
            void fence()
            {
                refreshCaches();
                if (pHeadCache == pTail)
                    return;
                producerWait([&] { return pHeadCache == pTail; });
            }

            // --- consumer (MTVU-GIF thread) ---
            void publishHead()
            {
                cPub = cHead;
                head.store(cHead, std::memory_order_release);
                doneBytes.store(cBytes, std::memory_order_release);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (producerWaiting.load(std::memory_order_relaxed))
                {
                    {
                        std::lock_guard<std::mutex> lock(m);
                    }
                    cvProducer.notify_one();
                }
            }
            void flushGsWake()
            {
                // GsWorker rule: deliver a deferred wake before waiting on
                // anything but an RPC; also per job end, as the unit did.
                cDirty = false;
                if (const auto &end = jobEndFn())
                    end();
            }
            // Spin briefly, then sleep until the producer publishes. False on stop.
            bool waitWork()
            {
                const uint64_t t0 = nowNs();
                if (gifPark())
                {
                    for (unsigned i = 0;; ++i)
                    {
                        if (stop.load(std::memory_order_relaxed))
                            return false;
                        cTailCache = tail.load(std::memory_order_acquire);
                        if (cTailCache != cHead)
                            return true;
                        if ((i & 15u) == 15u && nowNs() - t0 >= kStageParkSpinNs)
                            break;
                        stageCpuPause();
                    }
                }
                else
                {
                    for (;;)
                    {
                        if (stop.load(std::memory_order_relaxed))
                            return false;
                        cTailCache = tail.load(std::memory_order_acquire);
                        if (cTailCache != cHead)
                            return true;
                        if (nowNs() - t0 >= kSpinNs)
                            break;
                        std::this_thread::yield();
                    }
                }
                if (testBeforePark)
                    testBeforePark();
                sleeping.store(true, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (tail.load(std::memory_order_relaxed) == cHead && !stop.load(std::memory_order_relaxed))
                {
                    nSleeps.fetch_add(1u, std::memory_order_relaxed);
                    std::unique_lock<std::mutex> lock(m);
                    cvConsumer.wait_for(lock, std::chrono::milliseconds(100), [&] {
                        return tail.load(std::memory_order_acquire) != cHead || stop.load(std::memory_order_relaxed);
                    });
                }
                sleeping.store(false, std::memory_order_relaxed);
                cTailCache = tail.load(std::memory_order_acquire);
                return !stop.load(std::memory_order_relaxed);
            }
            void run(GifOp &op)
            {
                switch (op.kind)
                {
                case GifOp::Kind::Submit:
                case GifOp::Kind::Drain:
                    exec(op);
                    cDirty = true;
                    break;
                case GifOp::Kind::Call:
                    flushGsWake();
                    if (op.fn)
                        op.fn();
                    cDirty = true;
                    break;
                case GifOp::Kind::JobEnd:
                    flushGsWake();
                    workerJobDoneFromGif();
                    break;
                }
            }
            void loop()
            {
                const UnitThreadGuard unitGuard;
                g_gifTid.store(std::this_thread::get_id(), std::memory_order_relaxed);
                ThreadNaming::SetCurrentThreadName("MTVU-GIF");
                if (const char *cpus = std::getenv("PS2X_MTVU_GIF_CPUS"))
                {
                    if (cpus[0] != '\0')
                    {
                        const int rc = ps2x::pinCurrentThreadToCpus(ps2x::parseCpuList(cpus));
                        std::fprintf(stderr, "[affinity] mtvu-gif thread cpus=%s rc=%d\n", cpus, rc);
                    }
                }
                bool busy = false;
                uint64_t busyT0 = 0;
                for (;;)
                {
                    if (cHead == cTailCache)
                    {
                        cTailCache = tail.load(std::memory_order_acquire);
                        if (cHead == cTailCache)
                        {
                            if (busy)
                            {
                                const uint64_t ns = nowNs() - busyT0;
                                busyNs.fetch_add(ns, std::memory_order_relaxed);
                                busyNsWin.fetch_add(ns, std::memory_order_relaxed);
                                busy = false;
                            }
                            publishHead();
                            if (cDirty)
                                flushGsWake();
                            if (!waitWork())
                                break;
                            continue;
                        }
                    }
                    if (stop.load(std::memory_order_relaxed))
                        break;
                    if (!busy)
                    {
                        busy = true;
                        busyT0 = nowNs();
                    }
                    GifOp &op = slots[cHead & kMask];
                    const GifOp::Kind kind = op.kind;
                    run(op);
                    cBytes += op.acct;
                    std::vector<uint8_t>().swap(op.bytes); // no-op once moved from
                    op.fn = nullptr;
                    ++cHead;
                    if (kind == GifOp::Kind::JobEnd || cHead - cPub >= kPublishEvery)
                        publishHead();
                }
                g_gifTid.store(std::thread::id{}, std::memory_order_relaxed);
            }

            // --- lifecycle (EE / init; the ring must be empty) ---
            void start(std::function<void(GifOp &)> fn)
            {
                shutdown();
                exec = std::move(fn);
                slots.reset(new GifOp[kSlots]);
                pTail = pPub = pBytes = pHeadCache = pDoneBytesCache = 0u;
                cHead = cTailCache = cBytes = cPub = 0u;
                cDirty = false;
                summaryTail = 0u;
                tail.store(0u, std::memory_order_relaxed);
                head.store(0u, std::memory_order_relaxed);
                doneBytes.store(0u, std::memory_order_relaxed);
                stop.store(false, std::memory_order_relaxed);
                running = true;
                th = std::thread([this] { loop(); });
            }
            void shutdown()
            {
                if (!running)
                    return;
                stop.store(true, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(m);
                }
                cvConsumer.notify_all();
                cvProducer.notify_all();
                if (th.joinable())
                    th.join();
                running = false;
                exec = nullptr;
                slots.reset();
            }
        };

        // VPL2 VIF stage. g_vifStage: unit jobs run on the MTVU-VIF thread
        // and their VU-side actions on the MTVU thread from the record log
        // (set under the worker mutex, before the game thread runs; cleared
        // after a full sync). g_vifTid: the MTVU-VIF thread.
        inline std::atomic<bool> g_vifStage{false};
        inline std::atomic<std::thread::id> g_vifTid{};

        // Ordered SPSC byte log of VifRecs: producer = the MTVU-VIF thread,
        // consumer = the MTVU thread. Same publish/wake protocol as GifStage
        // (batched publishes, a wake only when the other side sleeps).
        struct VifLog
        {
            static constexpr uint64_t kBytes = 4ull << 20;
            static constexpr uint64_t kMask = kBytes - 1u;
            static constexpr uint32_t kPublishEvery = 32u;
            static constexpr uint64_t kSpinNs = 50000u;

            std::unique_ptr<uint8_t[]> storage;
            uint8_t *buf = nullptr; // 64-aligned inside storage
            VifExecFn exec = nullptr;
            void *opaque = nullptr;
            // Producer-private (MTVU-VIF thread).
            alignas(64) uint64_t pTail = 0;
            uint64_t pPub = 0;
            uint64_t pHeadCache = 0;
            uint32_t pRecs = 0;
            // Published by the producer.
            alignas(64) std::atomic<uint64_t> tail{0};
            // Published by the consumer.
            alignas(64) std::atomic<uint64_t> head{0};
            alignas(64) std::atomic<bool> sleeping{false};
            std::atomic<bool> producerWaiting{false};
            std::atomic<bool> stop{false};
            // Consumer-private (MTVU thread).
            alignas(64) uint64_t cHead = 0;
            uint64_t cTailCache = 0;
            uint32_t cRecs = 0;
            std::mutex m;
            std::condition_variable cvConsumer;
            std::condition_variable cvProducer;
            std::thread th;       // the MTVU-VIF thread
            bool running = false; // EE / init only
            std::function<void()> testBeforePark; // suite hook: runs after the spin, before the sleep
            std::atomic<bool> vuIn{false}; // the MTVU thread is inside vuLoop
            // Receipts (logged only).
            std::atomic<uint64_t> nPub{0};
            std::atomic<uint64_t> nRecs{0};
            std::atomic<uint64_t> nBytes{0};
            std::atomic<uint64_t> nJobs{0};
            std::atomic<uint64_t> nSleeps{0};
            std::atomic<uint64_t> nFullWaits{0};
            std::atomic<uint64_t> nEscapes{0};
            std::atomic<uint64_t> vifBusyNs{0};    // since the last vblank (perf stage)
            std::atomic<uint64_t> vifBusyNsWin{0}; // since the last summary

            // --- producer (MTVU-VIF thread) ---
            bool room(uint64_t need) const { return pTail + need - pHeadCache <= kBytes; }
            void refresh() { pHeadCache = head.load(std::memory_order_acquire); }
            void publish()
            {
                if (pPub == pTail)
                    return;
                pPub = pTail;
                pRecs = 0u;
                tail.store(pTail, std::memory_order_release);
                nPub.fetch_add(1u, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (sleeping.load(std::memory_order_relaxed))
                {
                    {
                        std::lock_guard<std::mutex> lock(m);
                    }
                    cvConsumer.notify_one();
                }
            }
            template <typename Pred>
            void producerWait(Pred done)
            {
                publish();
                for (int spin = 0; spin < 256; ++spin)
                {
                    refresh();
                    if (done() || stop.load(std::memory_order_relaxed))
                        return;
                    std::this_thread::yield();
                }
                producerWaiting.store(true, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                {
                    std::unique_lock<std::mutex> lock(m);
                    for (;;)
                    {
                        refresh();
                        if (done() || stop.load(std::memory_order_relaxed))
                            break;
                        cvProducer.wait_for(lock, std::chrono::milliseconds(1));
                    }
                }
                producerWaiting.store(false, std::memory_order_relaxed);
            }
            // `bytes` (16-aligned) contiguous bytes for one record; null once
            // the stage is stopping. A record never straddles the ring end:
            // the tail is covered by a Wrap record first.
            uint8_t *reserve(uint32_t bytes)
            {
                const uint64_t off = pTail & kMask;
                const uint64_t wrap = (off + bytes > kBytes) ? kBytes - off : 0u;
                const uint64_t need = wrap + bytes;
                if (!room(need))
                {
                    refresh();
                    if (!room(need))
                    {
                        nFullWaits.fetch_add(1u, std::memory_order_relaxed);
                        producerWait([&] { return room(need); });
                    }
                }
                if (stop.load(std::memory_order_relaxed))
                    return nullptr;
                if (wrap != 0u)
                {
                    VifRec w{};
                    w.size = static_cast<uint32_t>(wrap);
                    w.kind = VifRecKind::Wrap;
                    std::memcpy(buf + off, &w, sizeof(w));
                    pTail += wrap;
                }
                return buf + (pTail & kMask);
            }
            void commit(uint32_t bytes, bool publishNow)
            {
                pTail += bytes;
                nRecs.fetch_add(1u, std::memory_order_relaxed);
                nBytes.fetch_add(bytes, std::memory_order_relaxed);
                if (publishNow || ++pRecs >= kPublishEvery)
                    publish();
            }
            // A header-only record (+ an optional 16-byte payload).
            void push(VifRecKind kind, uint8_t f, uint16_t n, uint32_t a, uint32_t b, bool publishNow,
                      const void *payload16 = nullptr)
            {
                const uint32_t size = payload16 ? 32u : 16u;
                uint8_t *p = reserve(size);
                if (!p)
                    return;
                VifRec r{size, kind, f, n, a, b};
                std::memcpy(p, &r, sizeof(r));
                if (payload16)
                    std::memcpy(p + 16, payload16, 16u);
                commit(size, publishNow);
            }
            // Run fn on the MTVU thread after every earlier record, and wait.
            void runSync(std::function<void()> fn)
            {
                std::atomic<bool> done{false};
                auto *heap = new std::function<void()>(std::move(fn));
                std::atomic<bool> *donePtr = &done;
                uint8_t payload[16] = {};
                std::memcpy(payload, &heap, sizeof(heap));
                std::memcpy(payload + 8, &donePtr, sizeof(donePtr));
                push(VifRecKind::Call, 1u, 0u, 0u, 0u, true, payload);
                producerWait([&] { return done.load(std::memory_order_acquire); });
            }

            // --- consumer (MTVU thread) ---
            void publishHead()
            {
                cRecs = 0u;
                head.store(cHead, std::memory_order_release);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (producerWaiting.load(std::memory_order_relaxed))
                {
                    {
                        std::lock_guard<std::mutex> lock(m);
                    }
                    cvProducer.notify_one();
                }
            }
            // Spin briefly, then sleep until the producer publishes. False on stop.
            bool waitWork()
            {
                const uint64_t t0 = nowNs();
                if (vifPark())
                {
                    for (unsigned i = 0;; ++i)
                    {
                        if (stop.load(std::memory_order_relaxed))
                            return false;
                        cTailCache = tail.load(std::memory_order_acquire);
                        if (cTailCache != cHead)
                            return true;
                        if ((i & 15u) == 15u && nowNs() - t0 >= kStageParkSpinNs)
                            break;
                        stageCpuPause();
                    }
                }
                else
                {
                    for (;;)
                    {
                        if (stop.load(std::memory_order_relaxed))
                            return false;
                        cTailCache = tail.load(std::memory_order_acquire);
                        if (cTailCache != cHead)
                            return true;
                        if (nowNs() - t0 >= kSpinNs)
                            break;
                        std::this_thread::yield();
                    }
                }
                if (testBeforePark)
                    testBeforePark();
                sleeping.store(true, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (tail.load(std::memory_order_relaxed) == cHead && !stop.load(std::memory_order_relaxed))
                {
                    nSleeps.fetch_add(1u, std::memory_order_relaxed);
                    std::unique_lock<std::mutex> lock(m);
                    cvConsumer.wait_for(lock, std::chrono::milliseconds(100), [&] {
                        return tail.load(std::memory_order_acquire) != cHead || stop.load(std::memory_order_relaxed);
                    });
                }
                sleeping.store(false, std::memory_order_relaxed);
                cTailCache = tail.load(std::memory_order_acquire);
                return cTailCache != cHead || !stop.load(std::memory_order_relaxed);
            }

            // --- lifecycle (EE / init; the log must be empty) ---
            void reset(VifExecFn fn, void *op)
            {
                if (!storage)
                {
                    storage.reset(new uint8_t[kBytes + 64u]);
                    const uintptr_t raw = reinterpret_cast<uintptr_t>(storage.get());
                    buf = storage.get() + ((64u - (raw & 63u)) & 63u);
                }
                exec = fn;
                opaque = op;
                pTail = pPub = pHeadCache = 0u;
                pRecs = 0u;
                cHead = cTailCache = 0u;
                cRecs = 0u;
                tail.store(0u, std::memory_order_relaxed);
                head.store(0u, std::memory_order_relaxed);
                sleeping.store(false, std::memory_order_relaxed);
                producerWaiting.store(false, std::memory_order_relaxed);
                stop.store(false, std::memory_order_relaxed);
            }
            void wakeAll()
            {
                {
                    std::lock_guard<std::mutex> lock(m);
                }
                cvConsumer.notify_all();
                cvProducer.notify_all();
            }
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
            // IOSR1: syncAll() with no reason (Reason::Count: shutdown and
            // stage-transition paths, never steady state). Counted separately
            // so the per-window [perf-mtvu] reasons sum to ee.mtvu.
            uint64_t otherWaits = 0;
            uint64_t otherWaitNs = 0;
            std::array<uint64_t, static_cast<size_t>(Site::Count)> violations{};
            uint64_t violationsTotal = 0;
            uint64_t jobs = 0;
            // PT2: stage tail. tailOn caches PS2X_PERF_LOG (set in start());
            // tailBusyNs accumulates worker job ns since the last vblank
            // (worker fetch_adds, the EE exchanges at vblank).
            std::atomic<bool> tailOn{false};
            std::atomic<uint64_t> tailBusyNs{0};
            // VG2 lever 1 (PS2X_MTVU_BUSY_PER_VBLANK=1, needs tailOn): vuLoop
            // publishes the start of its open busy span here (0 = idle); the
            // EE's vblank moves it to its own timestamp and credits the part
            // before, so a span that crosses vblanks lands on each tick it
            // covers instead of all on the tick where it ends. Host timing only.
            bool busyPerVblank = false;
            std::atomic<uint64_t> busySinceNs{0};
            // VPL1: the GIF stage this unit feeds (idle unless started).
            GifStage gif;
            // VPL2: the VIF-stage log (idle unless started).
            VifLog vif;

            ~Worker()
            {
                {
                    std::lock_guard<std::mutex> lock(m);
                    stop = true;
                }
                cvWork.notify_all();
                // VPL2: stop the VIF thread and the MTVU thread's log loop.
                vif.stop.store(true, std::memory_order_relaxed);
                vif.wakeAll();
                if (vif.th.joinable())
                    vif.th.join();
                gif.shutdown(); // VPL1: unblocks a producer waiting for ring room
#if defined(__unix__) || defined(__APPLE__)
                if (usePthread)
                    pthread_join(pth, nullptr);
#endif
                if (th.joinable())
                    th.join();
            }

            void loop()
            {
                const MtvuThreadGuard mtvuGuard;
                ThreadNaming::SetCurrentThreadName("MTVU");
                // AD1: bind this thread's TID to its ADPF hint session.
                ps2x::adpf::noteThread(ps2x::adpf::Thread::Mtvu);
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
                        cvWork.wait(lock, [&] {
                            return stop || !q.empty() || g_vifStage.load(std::memory_order_relaxed);
                        });
                        if (stop)
                            return;
                        if (g_vifStage.load(std::memory_order_relaxed))
                            vif.vuIn.store(true, std::memory_order_relaxed); // VPL2: the VIF thread pops
                        else
                            job = &q.front(); // stays queued (deque front is stable) until done
                    }
                    if (!job)
                    {
                        vuLoop(rng);
                        vif.vuIn.store(false, std::memory_order_release);
                        continue;
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
                    const bool tail = tailOn.load(std::memory_order_relaxed);
                    const uint64_t jobT0 = tail ? nowNs() : 0u;
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
                    if (tail)
                        tailBusyNs.fetch_add(nowNs() - jobT0, std::memory_order_relaxed);
                    if (g_gifStage.load(std::memory_order_relaxed))
                    {
                        // VPL1: the job completes on the GIF thread, after its
                        // GIF ops (JobEnd bumps `completed`); only the queue
                        // slot is freed here.
                        if (GifOp *op = gif.claim(0u))
                        {
                            op->kind = GifOp::Kind::JobEnd;
                            op->acct = 0u;
                            gif.commit(*op, true);
                        }
                        {
                            std::lock_guard<std::mutex> lock(m);
                            qBytes -= job->bytes;
                            q.pop_front();
                        }
                        cvSpace.notify_all();
                        continue;
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

            // VPL2: the MTVU thread as the VU stage. Runs the VIF-stage log in
            // order until the stage stops (the log is empty by then).
            void vuLoop(uint64_t &rng)
            {
                VifLog &L = vif;
                const bool tail = tailOn.load(std::memory_order_relaxed);
                bool busy = false;
                uint64_t busyT0 = 0;
                for (;;)
                {
                    if (L.cHead == L.cTailCache)
                    {
                        L.cTailCache = L.tail.load(std::memory_order_acquire);
                        if (L.cHead == L.cTailCache)
                        {
                            if (busy)
                            {
                                if (tail && busyPerVblank)
                                {
                                    // VG2: the vblank may have moved the span start.
                                    const uint64_t since = busySinceNs.exchange(0u, std::memory_order_acq_rel);
                                    const uint64_t now = nowNs();
                                    if (since != 0u && now > since)
                                        tailBusyNs.fetch_add(now - since, std::memory_order_relaxed);
                                }
                                else if (tail)
                                    tailBusyNs.fetch_add(nowNs() - busyT0, std::memory_order_relaxed);
                                busy = false;
                            }
                            L.publishHead();
                            if (L.stop.load(std::memory_order_relaxed))
                                return;
                            if (!L.waitWork())
                                return;
                            continue;
                        }
                    }
                    if (!busy)
                    {
                        busy = true;
                        busyT0 = tail ? nowNs() : 0u;
                        if (tail && busyPerVblank)
                            busySinceNs.store(busyT0 | 1u, std::memory_order_release); // never 0 while busy
                    }
                    const uint8_t *p = L.buf + (L.cHead & VifLog::kMask);
                    VifRec r;
                    std::memcpy(&r, p, sizeof(r));
                    switch (r.kind)
                    {
                    case VifRecKind::Wrap:
                        break;
                    case VifRecKind::JobBegin:
                    {
                        if (jitterUs != 0u)
                        {
                            rng ^= rng << 13;
                            rng ^= rng >> 7;
                            rng ^= rng << 17;
                            std::this_thread::sleep_for(std::chrono::microseconds(rng % (jitterUs + 1u)));
                        }
                        uint64_t fp = 0;
                        std::memcpy(&fp, p + 16, sizeof(fp));
                        ps2_fpmode::writeControl(fp);
                        t_jobFbrst = r.a;
                        break;
                    }
                    case VifRecKind::JobEnd:
                        // The job completes on the GIF thread, after its GIF
                        // ops (VPL1 JobEnd bumps `completed`).
                        if (GifOp *op = gif.claim(0u))
                        {
                            op->kind = GifOp::Kind::JobEnd;
                            op->acct = 0u;
                            gif.commit(*op, true);
                        }
                        break;
                    case VifRecKind::Call:
                    {
                        std::function<void()> *fn = nullptr;
                        std::memcpy(&fn, p + 16, sizeof(fn));
                        try
                        {
                            (*fn)();
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
                        delete fn;
                        if (r.f & 1u)
                        {
                            std::atomic<bool> *done = nullptr;
                            std::memcpy(&done, p + 24, sizeof(done));
                            L.cHead += r.size;
                            done->store(true, std::memory_order_release);
                            L.publishHead(); // wakes the waiting VIF thread
                            continue;
                        }
                        break;
                    }
                    default:
                        L.exec(L.opaque, *reinterpret_cast<const VifRec *>(p));
                        break;
                    }
                    L.cHead += r.size;
                    if (r.kind == VifRecKind::JobEnd || ++L.cRecs >= VifLog::kPublishEvery)
                        L.publishHead();
                }
            }

            // VPL2: the MTVU-VIF thread. Pops unit jobs in submit order; VIF
            // work runs here (its VU-side actions become records), any other
            // job is forwarded whole as one Call record.
            void vifLoop()
            {
                const UnitThreadGuard unitGuard;
                g_vifTid.store(std::this_thread::get_id(), std::memory_order_relaxed);
                ThreadNaming::SetCurrentThreadName("MTVU-VIF");
                if (const char *cpus = std::getenv("PS2X_MTVU_VIF_CPUS"))
                {
                    if (cpus[0] != '\0')
                    {
                        const int rc = ps2x::pinCurrentThreadToCpus(ps2x::parseCpuList(cpus));
                        std::fprintf(stderr, "[affinity] mtvu-vif thread cpus=%s rc=%d\n", cpus, rc);
                    }
                }
                VifLog &L = vif;
                uint64_t rng = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 3u;
                for (;;)
                {
                    Job *job = nullptr;
                    {
                        std::unique_lock<std::mutex> lock(m);
                        cvWork.wait(lock, [&] {
                            return stop || L.stop.load(std::memory_order_relaxed) || !q.empty();
                        });
                        if (stop || L.stop.load(std::memory_order_relaxed))
                            break;
                        job = &q.front();
                    }
                    if (jitterUs != 0u)
                    {
                        rng ^= rng << 13;
                        rng ^= rng >> 7;
                        rng ^= rng << 17;
                        std::this_thread::sleep_for(std::chrono::microseconds(rng % (jitterUs + 1u)));
                    }
                    const uint64_t jobT0 = nowNs();
                    ps2_fpmode::writeControl(job->fpControl);
                    t_jobFbrst = job->fbrst;
                    L.push(VifRecKind::JobBegin, 0u, 0u, job->fbrst, 0u, false, &job->fpControl);
                    if (job->vif)
                    {
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
                    }
                    else
                    {
                        auto *heap = new std::function<void()>(std::move(job->fn));
                        uint8_t payload[16] = {};
                        std::memcpy(payload, &heap, sizeof(heap));
                        L.push(VifRecKind::Call, 0u, 0u, 0u, 0u, false, payload);
                    }
                    L.push(VifRecKind::JobEnd, 0u, 0u, 0u, 0u, true);
                    L.nJobs.fetch_add(1u, std::memory_order_relaxed);
                    const uint64_t ns = nowNs() - jobT0;
                    L.vifBusyNs.fetch_add(ns, std::memory_order_relaxed);
                    L.vifBusyNsWin.fetch_add(ns, std::memory_order_relaxed);
                    {
                        std::lock_guard<std::mutex> lock(m);
                        qBytes -= job->bytes;
                        q.pop_front();
                    }
                    cvSpace.notify_all();
                }
                g_vifTid.store(std::thread::id{}, std::memory_order_relaxed);
            }

            void start()
            {
                started = true;
                // AD1: ADPF reuses this accounting, so it turns it on too.
                tailOn.store(ps2x::perflog::enabled() || ps2x::adpf::enabled(), std::memory_order_relaxed);
                // VG2 lever 1: per-vblank split of vuLoop busy spans (default off).
                if (const char *env = std::getenv("PS2X_MTVU_BUSY_PER_VBLANK"))
                    busyPerVblank = env[0] != '\0' && env[0] != '0';
                if (busyPerVblank)
                    std::fprintf(stderr, "[mtvu] VG2 busy per vblank on (PS2X_MTVU_BUSY_PER_VBLANK=1, tail=%d)\n",
                                 tailOn.load(std::memory_order_relaxed) ? 1 : 0);
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

            // MW1: PS2X_MTVU_WAIT. park (default): spin kParkSpinNs with a CPU
            // pause, then sleep on cvDone. spin: the old path, 256 yields
            // (swtch_pri on Darwin, ~6 % of GameThread busy at the full-120
            // VBlank) and then cvDone. Same condition and ordering either way:
            // every `completed` bump happens under `m` and is followed by
            // cvDone.notify_all(), and the sleep re-checks under `m`, so a
            // completion that lands between the spin and the sleep is seen
            // by the predicate and no wake is lost.
            static constexpr uint64_t kParkSpinNs = 4000u;
            static bool parseWaitPark(const char *e)
            {
                return !(e && std::strcmp(e, "spin") == 0);
            }
            int waitPark = -1;                   // -1 unresolved; EE only
            uint64_t waitParks = 0;              // EE only: waits that slept
            std::function<void()> testBeforePark; // suite hook: runs before the sleep
            static inline void cpuPause()
            {
#if defined(__aarch64__) || defined(_M_ARM64)
                __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#endif
            }

            // Wait until `target` jobs have completed; returns ns waited.
            uint64_t waitFor(uint64_t target)
            {
                if (completed.load(std::memory_order_acquire) >= target)
                    return 0u;
                const uint64_t t0 = nowNs();
                if (waitPark < 0)
                {
                    waitPark = parseWaitPark(std::getenv("PS2X_MTVU_WAIT")) ? 1 : 0;
                    std::fprintf(stderr, "[mtvu] wait=%s\n", waitPark ? "park" : "spin");
                }
                if (waitPark)
                {
                    for (unsigned i = 0;; ++i)
                    {
                        if (completed.load(std::memory_order_acquire) >= target)
                            return nowNs() - t0;
                        if ((i & 15u) == 15u && nowNs() - t0 >= kParkSpinNs)
                            break;
                        cpuPause();
                    }
                }
                else
                {
                    for (int spin = 0; spin < 256; ++spin)
                    {
                        if (completed.load(std::memory_order_acquire) >= target)
                            return nowNs() - t0;
                        std::this_thread::yield();
                    }
                }
                if (testBeforePark)
                    testBeforePark();
                std::unique_lock<std::mutex> lock(m);
                if (completed.load(std::memory_order_acquire) < target)
                {
                    ++waitParks;
                    cvDone.wait(lock, [&] { return completed.load(std::memory_order_acquire) >= target; });
                }
                return nowNs() - t0;
            }
        };

        inline Worker &worker()
        {
            static Worker w;
            return w;
        }

        inline GifStage &gifStage()
        {
            return worker().gif;
        }

        // VPL1: JobEnd on the GIF thread completes the unit job (what every
        // sync waits for), as the MTVU loop does with the stage off.
        inline void workerJobDoneFromGif()
        {
            Worker &w = worker();
            {
                std::lock_guard<std::mutex> lock(w.m);
                w.completed.store(w.completed.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
            }
            w.cvDone.notify_all();
        }

        // MU2 counter: VIF1 input bytes + UNPACK commands per summary window
        // (logged only, never hashed: det-neutral). The unit thread (or the
        // EE, on the inline path) accumulates; the summary exchanges.
        inline std::atomic<uint64_t> &vif1Bytes()
        {
            static std::atomic<uint64_t> v{0};
            return v;
        }
        inline std::atomic<uint64_t> &vif1Unpacks()
        {
            static std::atomic<uint64_t> v{0};
            return v;
        }

        // MP2 census: UNPACK fast-path vs fallback per format + GIF bytes per
        // path (PS2X_MP2_CENSUS=1, default off). Logged only, never hashed:
        // det-neutral. The unit thread (or the EE, on the inline path)
        // accumulates; the summaries below exchange per window.
        inline std::atomic<bool> &mp2CensusFlag()
        {
            static std::atomic<bool> v{false};
            return v;
        }
        struct Mp2Unpack
        {
            // Format slot = vl | vn << 2 (vl minor, vn major; 16 combos).
            std::atomic<uint64_t> bulkN[16];
            std::atomic<uint64_t> bulkB[16];
            std::atomic<uint64_t> genN[16];
            std::atomic<uint64_t> genB[16];
            std::atomic<uint64_t> noopN[16];
            std::atomic<uint64_t> noopB[16];
            // Generic-path reject reason (first failing bulk clause): 0 knob,
            // 1 format (!V4_32), 2 mode, 3 cl!=wl, 4 mask.
            std::atomic<uint64_t> rejN[5];
            std::atomic<uint64_t> rejB[5];
        };
        inline Mp2Unpack &mp2Unpack()
        {
            static Mp2Unpack v{};
            return v;
        }
        struct Mp2Gif
        {
            std::atomic<uint64_t> n[4];     // by path id 1..3
            std::atomic<uint64_t> bytes[4]; // by path id 1..3
        };
        inline Mp2Gif &mp2Gif()
        {
            static Mp2Gif v{};
            return v;
        }
        inline void mp2Summary(uint64_t tick)
        {
            if (!mp2CensusFlag().load(std::memory_order_relaxed))
                return;
            // Slot = vl | vn << 2 (vl minor: 0=32, 1=16, 2=8, 3=5-or-16).
            static const char *fmtName[16] = {
                "S-32", "S-16", "S-8", "S-16v",      // vn=0 (S)
                "V2-32", "V2-16", "V2-8", "V2-16v",  // vn=1 (V2)
                "V3-32", "V3-16", "V3-8", "V3-16v",  // vn=2 (V3)
                "V4-32", "V4-16", "V4-8", "V4-5"};   // vn=3 (V4)
            Mp2Unpack &u = mp2Unpack();
            uint64_t bulkN = 0, bulkB = 0, genN = 0, genB = 0, noopN = 0, noopB = 0;
            std::fprintf(stderr, "[mtvu] mp2-unpack tick=%llu",
                         static_cast<unsigned long long>(tick));
            for (int f = 0; f < 16; ++f)
            {
                const uint64_t bn = u.bulkN[f].exchange(0u, std::memory_order_relaxed);
                const uint64_t bb = u.bulkB[f].exchange(0u, std::memory_order_relaxed);
                const uint64_t gn = u.genN[f].exchange(0u, std::memory_order_relaxed);
                const uint64_t gb = u.genB[f].exchange(0u, std::memory_order_relaxed);
                const uint64_t nn = u.noopN[f].exchange(0u, std::memory_order_relaxed);
                const uint64_t nb = u.noopB[f].exchange(0u, std::memory_order_relaxed);
                bulkN += bn; bulkB += bb; genN += gn; genB += gb; noopN += nn; noopB += nb;
                if (bn + gn + nn > 0u)
                    std::fprintf(stderr, " %s=b%llu/%llu,g%llu/%llu,n%llu/%llu", fmtName[f],
                                 static_cast<unsigned long long>(bn), static_cast<unsigned long long>(bb),
                                 static_cast<unsigned long long>(gn), static_cast<unsigned long long>(gb),
                                 static_cast<unsigned long long>(nn), static_cast<unsigned long long>(nb));
            }
            static const char *rejName[5] = {"knob", "format", "mode", "clwl", "mask"};
            std::fprintf(stderr, "\n[mtvu] mp2-unpack-sum tick=%llu bulk=%llu/%llu generic=%llu/%llu noop=%llu/%llu",
                         static_cast<unsigned long long>(tick),
                         static_cast<unsigned long long>(bulkN), static_cast<unsigned long long>(bulkB),
                         static_cast<unsigned long long>(genN), static_cast<unsigned long long>(genB),
                         static_cast<unsigned long long>(noopN), static_cast<unsigned long long>(noopB));
            for (int r = 0; r < 5; ++r)
            {
                const uint64_t rn = u.rejN[r].exchange(0u, std::memory_order_relaxed);
                const uint64_t rb = u.rejB[r].exchange(0u, std::memory_order_relaxed);
                if (rn > 0u)
                    std::fprintf(stderr, " rej-%s=%llu/%llu", rejName[r],
                                 static_cast<unsigned long long>(rn), static_cast<unsigned long long>(rb));
            }
            Mp2Gif &g = mp2Gif();
            std::fprintf(stderr, "\n[mtvu] mp2-gif tick=%llu", static_cast<unsigned long long>(tick));
            for (int p = 1; p <= 3; ++p)
            {
                const uint64_t pn = g.n[p].exchange(0u, std::memory_order_relaxed);
                const uint64_t pb = g.bytes[p].exchange(0u, std::memory_order_relaxed);
                std::fprintf(stderr, " p%d=%llu/%llu", p,
                             static_cast<unsigned long long>(pn), static_cast<unsigned long long>(pb));
            }
            std::fprintf(stderr, "\n");
        }

        inline void threadedSummary(uint64_t tick)
        {
            Worker &w = worker();
            const uint64_t vif1b = vif1Bytes().exchange(0u, std::memory_order_relaxed);
            const uint64_t unpacks = vif1Unpacks().exchange(0u, std::memory_order_relaxed);
            std::fprintf(stderr, "[mtvu] threaded tick=%llu lag=%d jobs=%llu vif1b=%llu unpacks=%llu violations=%llu waits:",
                         static_cast<unsigned long long>(tick), g_lag.load(std::memory_order_relaxed) ? 1 : 0,
                         static_cast<unsigned long long>(w.jobs),
                         static_cast<unsigned long long>(vif1b), static_cast<unsigned long long>(unpacks),
                         static_cast<unsigned long long>(w.violationsTotal));
            for (size_t i = 0; i < w.waits.size(); ++i)
                if (w.waits[i] != 0u)
                    std::fprintf(stderr, " %s=%llu/%.1fms", reasonName(static_cast<Reason>(i)),
                                 static_cast<unsigned long long>(w.waits[i]), w.waitNs[i] / 1e6);
            for (size_t i = 0; i < w.violations.size(); ++i)
                if (w.violations[i] != 0u)
                    std::fprintf(stderr, " V:%s=%llu", siteName(static_cast<Site>(i)),
                                 static_cast<unsigned long long>(w.violations[i]));
            if (g_finishEe.load(std::memory_order_relaxed))
                std::fprintf(stderr, " finishee=%llu/%llu/%llu credits=%llu",
                             static_cast<unsigned long long>(g_finishEeSets.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g_finishEeSkips.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g_finishEeUnmatched.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g_finishEeCredits.load(std::memory_order_relaxed)));
            if (g_vif1StatFree.load(std::memory_order_relaxed))
                std::fprintf(stderr, " vif1statfree=%llu/%llu",
                             static_cast<unsigned long long>(g_vif1StatFreeN.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g_vif1StatFreePending.load(std::memory_order_relaxed)));
            std::fprintf(stderr, "\n");
            if (g_gifStage.load(std::memory_order_relaxed))
            {
                GifStage &g = w.gif;
                const uint64_t tailNow = g.tail.load(std::memory_order_acquire);
                const uint64_t ops = tailNow - g.summaryTail;
                g.summaryTail = tailNow;
                std::fprintf(stderr, "[mtvu] gif-stage tick=%llu ops=%llu pub=%llu wakes=%llu sleeps=%llu fullwaits=%llu escapes=%llu busy_ms=%.1f\n",
                             static_cast<unsigned long long>(tick), static_cast<unsigned long long>(ops),
                             static_cast<unsigned long long>(g.nPub.exchange(0u, std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g.nWakes.exchange(0u, std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g.nSleeps.exchange(0u, std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g.nFullWaits.exchange(0u, std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g.nEscapes.load(std::memory_order_relaxed)),
                             g.busyNsWin.exchange(0u, std::memory_order_relaxed) / 1e6);
            }
            if (g_vifStage.load(std::memory_order_relaxed))
            {
                VifLog &L = w.vif;
                std::fprintf(stderr, "[mtvu] vif-stage tick=%llu jobs=%llu recs=%llu kb=%llu pub=%llu vusleeps=%llu fullwaits=%llu escapes=%llu vifbusy_ms=%.1f\n",
                             static_cast<unsigned long long>(tick),
                             static_cast<unsigned long long>(L.nJobs.exchange(0u, std::memory_order_relaxed)),
                             static_cast<unsigned long long>(L.nRecs.exchange(0u, std::memory_order_relaxed)),
                             static_cast<unsigned long long>(L.nBytes.exchange(0u, std::memory_order_relaxed) >> 10),
                             static_cast<unsigned long long>(L.nPub.exchange(0u, std::memory_order_relaxed)),
                             static_cast<unsigned long long>(L.nSleeps.exchange(0u, std::memory_order_relaxed)),
                             static_cast<unsigned long long>(L.nFullWaits.exchange(0u, std::memory_order_relaxed)),
                             static_cast<unsigned long long>(L.nEscapes.load(std::memory_order_relaxed)),
                             L.vifBusyNsWin.exchange(0u, std::memory_order_relaxed) / 1e6);
            }
            mp2Summary(tick); // MP2 census (no-op unless PS2X_MP2_CENSUS=1)
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
            const uint64_t vif1b = vif1Bytes().exchange(0u, std::memory_order_relaxed);
            const uint64_t unpacks = vif1Unpacks().exchange(0u, std::memory_order_relaxed);
            std::fprintf(stderr, "[mtvu] census tick=%llu jobs=%llu vif1b=%llu unpacks=%llu fifo=%llu unit_ms=%.1f snap_bytes=%llu snap_ms=%.2f violations=%llu hits:",
                         static_cast<unsigned long long>(c.tick), static_cast<unsigned long long>(c.jobs),
                         static_cast<unsigned long long>(vif1b), static_cast<unsigned long long>(unpacks),
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
            mp2Summary(c.tick); // MP2 census (no-op unless PS2X_MP2_CENSUS=1)
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
        const char *fe = std::getenv("PS2X_MTVU_FINISH_EE");
        detail::g_finishEe.store(m == static_cast<int>(Mode::Threaded) && fe && std::strcmp(fe, "1") == 0,
                                 std::memory_order_relaxed);
        const char *vs = std::getenv("PS2X_MTVU_VIF1_STAT_FREE");
        detail::g_vif1StatFree.store(m == static_cast<int>(Mode::Threaded) && vs && std::strcmp(vs, "1") == 0,
                                     std::memory_order_relaxed);
        detail::g_mode.store(m, std::memory_order_relaxed);
        if (e && std::strcmp(e, "1") == 0)
            std::fprintf(stderr, "[mtvu] mode=%s lag=%d finish_ee=%d vif1stat_free=%d jitter_us=%u%s\n",
                         m ? "threaded" : "off", lagOn ? 1 : 0,
                         detail::g_finishEe.load(std::memory_order_relaxed) ? 1 : 0,
                         detail::g_vif1StatFree.load(std::memory_order_relaxed) ? 1 : 0,
                         detail::worker().jitterUs,
                         diagArmed ? " (a dev trace is armed: threaded mode refused)" : "");
    }

    // Tests: force a mode (drains the worker first).
    inline void setModeForTest(Mode m, bool lagOn = false, uint32_t jitterUs = 0u);

    inline bool onWorker()
    {
        return std::this_thread::get_id() == detail::g_mtvuTid.load(std::memory_order_relaxed);
    }

    inline uint32_t jobFbrst()
    {
        return detail::t_jobFbrst;
    }

    // VPL1 GIF stage (PS2X_MTVU_GIF_STAGE=1; report local/research/VPL1).
    inline bool gifStageOn()
    {
        return detail::g_gifStage.load(std::memory_order_relaxed);
    }

    // The unit's GIF actions become ops only on the MTVU thread; any other
    // caller (the EE after a sync, the GIF thread itself) runs them inline.
    inline bool gifStageDefer()
    {
        return gifStageOn() && onWorker();
    }

    inline bool onGifStage()
    {
        return std::this_thread::get_id() == detail::g_gifTid.load(std::memory_order_relaxed);
    }

    // A unit thread that feeds the GS worker (lean local batches, deferred
    // wakes): the MTVU thread, or the GIF thread that took its submits over.
    inline bool onUnitGsProducer()
    {
        return onWorker() || onGifStage();
    }

    // MTVU thread only (gifStageDefer()). Dropped only while shutting down.
    inline void gifStageSubmit(uint8_t path, bool directHl, std::vector<uint8_t> &&bytes)
    {
        detail::GifStage &g = detail::gifStage();
        const uint32_t acct = static_cast<uint32_t>(bytes.size());
        if (GifOp *op = g.claim(acct))
        {
            op->kind = GifOp::Kind::Submit;
            op->path = path;
            op->directHl = directHl;
            op->acct = acct;
            op->bytes = std::move(bytes);
            g.commit(*op, false);
        }
    }

    inline void gifStageDrain()
    {
        detail::GifStage &g = detail::gifStage();
        if (GifOp *op = g.claim(0u))
        {
            op->kind = GifOp::Kind::Drain;
            op->acct = 0u;
            g.commit(*op, false);
        }
    }

    inline void gifStageCall(std::function<void()> fn)
    {
        detail::GifStage &g = detail::gifStage();
        if (GifOp *op = g.claim(0u))
        {
            op->kind = GifOp::Kind::Call;
            op->acct = 0u;
            op->fn = std::move(fn);
            g.commit(*op, true);
        }
    }

    // Safety net: a GS-worker enqueue on the MTVU thread with the stage on
    // bypassed the ops. Keep it ordered (run every earlier op first) and
    // count it; the [mtvu] gif-stage line must show escapes=0.
    inline void gifStageEscape()
    {
        detail::GifStage &g = detail::gifStage();
        const uint64_t n = g.nEscapes.fetch_add(1u, std::memory_order_relaxed);
        if (n < 8u)
            std::fprintf(stderr, "[mtvu] gif-stage ESCAPE n=%llu (GS enqueue on the MTVU thread; ring fenced)\n",
                         static_cast<unsigned long long>(n + 1u));
        g.fence();
    }

    // Runtime init, after configure() and before the game thread runs.
    // exec runs Submit/Drain ops on the GIF thread.
    inline void startGifStage(std::function<void(GifOp &)> exec)
    {
        detail::gifStage().start(std::move(exec));
        detail::g_gifStage.store(true, std::memory_order_release);
    }

    // After a full sync (the ring is empty): route inline again and join the
    // GIF thread, so no hook it reads is torn down under it.
    inline void stopGifStage()
    {
        detail::g_gifStage.store(false, std::memory_order_release);
        detail::gifStage().shutdown();
    }

    // VPL2 VIF stage (PS2X_MTVU_VIF_STAGE=1; report local/research/VPL2).
    inline bool vifStageOn()
    {
        return detail::g_vifStage.load(std::memory_order_relaxed);
    }

    inline bool onVifStage()
    {
        return std::this_thread::get_id() == detail::g_vifTid.load(std::memory_order_relaxed);
    }

    // True on the MTVU-VIF thread: a VU-side action becomes a record.
    inline bool vifStageDefer()
    {
        return vifStageOn() && onVifStage();
    }

    inline detail::VifLog &vifLog()
    {
        return detail::worker().vif;
    }

    // MTVU-VIF thread only: record builders (null / no-op once stopping).
    inline uint8_t *vifStageReserve(uint32_t bytes)
    {
        return vifLog().reserve(bytes);
    }
    inline void vifStageCommit(uint32_t bytes, bool publishNow)
    {
        vifLog().commit(bytes, publishNow);
    }
    inline void vifStagePush(VifRecKind kind, uint8_t f, uint32_t a, uint32_t b, bool publishNow,
                             const void *payload16 = nullptr)
    {
        vifLog().push(kind, f, 0u, a, b, publishNow, payload16);
    }

    // Safety net: VIF-thread code reached a VU-side entry point that is not
    // staged. Run it on the MTVU thread after every earlier record and wait
    // (ordered, just slower); the [mtvu] vif-stage line must show escapes=0.
    inline void vifStageEscape(std::function<void()> fn)
    {
        detail::VifLog &L = vifLog();
        const uint64_t n = L.nEscapes.fetch_add(1u, std::memory_order_relaxed);
        if (n < 8u)
            std::fprintf(stderr, "[mtvu] vif-stage ESCAPE n=%llu (VU-side call on the VIF thread; run on MTVU)\n",
                         static_cast<unsigned long long>(n + 1u));
        L.runSync(std::move(fn));
    }
    // A GS-worker enqueue on the VIF thread (can't be rerouted there): count.
    inline void vifStageNoteEscape()
    {
        const uint64_t n = vifLog().nEscapes.fetch_add(1u, std::memory_order_relaxed);
        if (n < 8u)
            std::fprintf(stderr, "[mtvu] vif-stage ESCAPE n=%llu (GS enqueue on the VIF thread)\n",
                         static_cast<unsigned long long>(n + 1u));
    }

    // Runtime init, after startGifStage() and before the game thread runs
    // (no jobs queued). exec runs the PS2Memory records on the MTVU thread.
    inline void startVifStage(VifExecFn exec, void *opaque)
    {
        detail::Worker &w = detail::worker();
        detail::VifLog &L = w.vif;
        L.reset(exec, opaque);
        if (!w.started)
            w.start();
        {
            std::lock_guard<std::mutex> lock(w.m);
            detail::g_vifStage.store(true, std::memory_order_release);
        }
        L.running = true;
        L.th = std::thread([&w] { w.vifLoop(); });
        w.cvWork.notify_all(); // the MTVU thread enters its log loop
    }

    // After a full sync (log and GIF ring empty): join the VIF thread and
    // take the MTVU thread back to popping jobs.
    inline void stopVifStage()
    {
        detail::Worker &w = detail::worker();
        detail::VifLog &L = w.vif;
        if (!L.running)
            return;
        {
            std::lock_guard<std::mutex> lock(w.m);
            detail::g_vifStage.store(false, std::memory_order_release);
        }
        L.stop.store(true, std::memory_order_relaxed);
        w.cvWork.notify_all();
        L.wakeAll();
        if (L.th.joinable())
            L.th.join();
        while (L.vuIn.load(std::memory_order_acquire))
        {
            L.wakeAll();
            std::this_thread::yield();
        }
        L.running = false;
    }

    // PT2: total threaded-mode sync-wait ns so far (EE side; the GameThread
    // wait stage deltas it per tick). 0 unless threaded.
    inline uint64_t threadedWaitNsTotal()
    {
        if (!threaded())
            return 0u;
        uint64_t sum = 0u;
        for (uint64_t v : detail::worker().waitNs)
            sum += v;
        return sum;
    }

    // IP7: the same total for one sync reason (e.g. VBlank).
    inline uint64_t threadedWaitNsFor(Reason r)
    {
        if (!threaded())
            return 0u;
        return detail::worker().waitNs[static_cast<size_t>(r)];
    }

    // IOSR1: per-window MTVU sync attribution for the iOS `[perf-mtvu]` log
    // line. The EE executor calls noteMtvuWindowTick() once per tick (gated by
    // the caller on the perf-log switch: one branch when the log is off); the
    // main thread's perflog::poll() takes the window with takeMtvuWindowSample().
    // All waits[]/waitNs[] reads happen on the EE thread that writes them;
    // the window accumulators are EE-fetch_add / main-thread-exchange. Output
    // only: no guest state, det-neutral.
    static constexpr size_t kMtvuReasonCount = static_cast<size_t>(Reason::Count);

    struct MtvuWindowSample
    {
        std::array<uint64_t, kMtvuReasonCount> n{};
        std::array<uint64_t, kMtvuReasonCount> ns{};
        uint64_t otherN = 0;
        uint64_t otherNs = 0;
        uint64_t finisheeSkips = 0;    // MQ2 unit PATH3 FINISH writes covered by a credit
        uint64_t vif1statfreeSkips = 0; // MQ3 VIF1_STAT writes that skipped the sync
    };

    namespace detail
    {
        struct MtvuWinAccum
        {
            std::array<std::atomic<uint64_t>, kMtvuReasonCount> n;
            std::array<std::atomic<uint64_t>, kMtvuReasonCount> ns;
            std::atomic<uint64_t> otherN;
            std::atomic<uint64_t> otherNs;
            std::atomic<uint64_t> finisheeSkips;
            std::atomic<uint64_t> vif1statfreeSkips;
            MtvuWinAccum()
            {
                for (auto &a : n)
                    a.store(0u, std::memory_order_relaxed);
                for (auto &a : ns)
                    a.store(0u, std::memory_order_relaxed);
                otherN.store(0u, std::memory_order_relaxed);
                otherNs.store(0u, std::memory_order_relaxed);
                finisheeSkips.store(0u, std::memory_order_relaxed);
                vif1statfreeSkips.store(0u, std::memory_order_relaxed);
            }
        };

        inline MtvuWinAccum &mtvuWin()
        {
            static MtvuWinAccum w;
            return w;
        }

        struct MtvuWinLast
        {
            std::array<uint64_t, kMtvuReasonCount> n{};
            std::array<uint64_t, kMtvuReasonCount> ns{};
            uint64_t otherN = 0;
            uint64_t otherNs = 0;
            uint64_t finisheeSkips = 0;
            uint64_t vif1statfreeSkips = 0;
        };

        inline MtvuWinLast &mtvuWinLast()
        {
            static MtvuWinLast w;
            return w;
        }
    } // namespace detail

    // EE side, once per tick. The caller gates on the perf-log switch.
    inline void noteMtvuWindowTick()
    {
        if (!threaded())
            return;
        detail::Worker &w = detail::worker();
        detail::MtvuWinLast &last = detail::mtvuWinLast();
        detail::MtvuWinAccum &win = detail::mtvuWin();
        for (size_t i = 0; i < kMtvuReasonCount; ++i)
        {
            const uint64_t curN = w.waits[i];
            const uint64_t dn = curN >= last.n[i] ? curN - last.n[i] : curN;
            if (dn != 0u)
                win.n[i].fetch_add(dn, std::memory_order_relaxed);
            last.n[i] = curN;
            const uint64_t curNs = w.waitNs[i];
            const uint64_t dNs = curNs >= last.ns[i] ? curNs - last.ns[i] : curNs;
            if (dNs != 0u)
                win.ns[i].fetch_add(dNs, std::memory_order_relaxed);
            last.ns[i] = curNs;
        }
        const uint64_t curON = w.otherWaits;
        const uint64_t dON = curON >= last.otherN ? curON - last.otherN : curON;
        if (dON != 0u)
            win.otherN.fetch_add(dON, std::memory_order_relaxed);
        last.otherN = curON;
        const uint64_t curONs = w.otherWaitNs;
        const uint64_t dONs = curONs >= last.otherNs ? curONs - last.otherNs : curONs;
        if (dONs != 0u)
            win.otherNs.fetch_add(dONs, std::memory_order_relaxed);
        last.otherNs = curONs;
        const uint64_t curFe = detail::g_finishEeSkips.load(std::memory_order_relaxed);
        const uint64_t dFe = curFe >= last.finisheeSkips ? curFe - last.finisheeSkips : curFe;
        if (dFe != 0u)
            win.finisheeSkips.fetch_add(dFe, std::memory_order_relaxed);
        last.finisheeSkips = curFe;
        const uint64_t curVsf = detail::g_vif1StatFreeN.load(std::memory_order_relaxed);
        const uint64_t dVsf = curVsf >= last.vif1statfreeSkips ? curVsf - last.vif1statfreeSkips : curVsf;
        if (dVsf != 0u)
            win.vif1statfreeSkips.fetch_add(dVsf, std::memory_order_relaxed);
        last.vif1statfreeSkips = curVsf;
    }

    // Main-thread side (perflog::poll): exchange the window accumulators.
    // False when MTVU is not threaded (no line is emitted).
    inline bool takeMtvuWindowSample(MtvuWindowSample &out)
    {
        if (!threaded())
            return false;
        detail::MtvuWinAccum &win = detail::mtvuWin();
        for (size_t i = 0; i < kMtvuReasonCount; ++i)
        {
            out.n[i] = win.n[i].exchange(0u, std::memory_order_relaxed);
            out.ns[i] = win.ns[i].exchange(0u, std::memory_order_relaxed);
        }
        out.otherN = win.otherN.exchange(0u, std::memory_order_relaxed);
        out.otherNs = win.otherNs.exchange(0u, std::memory_order_relaxed);
        out.finisheeSkips = win.finisheeSkips.exchange(0u, std::memory_order_relaxed);
        out.vif1statfreeSkips = win.vif1statfreeSkips.exchange(0u, std::memory_order_relaxed);
        return true;
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
        else
        {
            // IOSR1: unattributed waits (the [perf-mtvu] "other" bucket).
            ++w.otherWaits;
            w.otherWaitNs += ns;
        }
    }

    // EE side, before touching unit-owned state.
    inline void sync(Reason r, uint32_t detail = 0u)
    {
        // (No EE check here, as before: non-EE callers proceed to the slow
        // path. Only unit work skips.)
        if (!active() || detail::isUnitThread() ||
            detail::g_eeUnitDepth.load(std::memory_order_relaxed) != 0)
            return;
        if (threaded())
        {
            syncAll(r);
            return;
        }
        detail::syncSlow(r, detail);
    }

    // MQ2: EE-side PATH3 FINISH (PS2X_MTVU_FINISH_EE=1).
    inline bool finishEe()
    {
        return detail::g_finishEe.load(std::memory_order_relaxed);
    }

    // EE, at a DMA-kick submit: `writes` A+D FINISH writes in the kick's
    // PATH3 pieces were applied to CSR here; the unit must not set them again.
    // Called before submit(), whose queue mutex publishes the credit.
    inline void noteEeFinishSet(uint32_t writes)
    {
        detail::g_finishEeCredits.fetch_add(writes, std::memory_order_relaxed);
        detail::g_finishEeSets.fetch_add(1u, std::memory_order_relaxed);
    }

    // Unit thread, emitting a PATH3 packet with `writes` A+D FINISH writes:
    // true = all covered by EE credits (skip the CSR set). False = no credit
    // (counted; the caller sets FINISH as before).
    inline bool consumeEeFinishCredits(uint32_t writes)
    {
        uint64_t have = detail::g_finishEeCredits.load(std::memory_order_relaxed);
        while (have >= writes)
        {
            if (detail::g_finishEeCredits.compare_exchange_weak(have, have - writes, std::memory_order_relaxed))
            {
                detail::g_finishEeSkips.fetch_add(writes, std::memory_order_relaxed);
                return true;
            }
        }
        if (detail::g_finishEeUnmatched.fetch_add(1u, std::memory_order_relaxed) < 8u)
            std::fprintf(stderr, "[mtvu] finish-ee UNMATCHED unit PATH3 FINISH (writes=%u credits=%llu)\n", writes,
                         static_cast<unsigned long long>(have));
        return false;
    }

    // MQ3: PS2X_MTVU_VIF1_STAT_FREE=1 (threaded only).
    inline bool vif1StatFree()
    {
        return detail::g_vif1StatFree.load(std::memory_order_relaxed);
    }
    // EE: a VIF1_STAT write skipped its sync (counts those with jobs queued,
    // i.e. the ones that would have waited).
    inline void noteVif1StatFree()
    {
        detail::g_vif1StatFreeN.fetch_add(1u, std::memory_order_relaxed);
        if (detail::worker().pending())
            detail::g_vif1StatFreePending.fetch_add(1u, std::memory_order_relaxed);
    }

    // Arbiter: marks the path of the packet being emitted on this thread.
    struct GifEmitPathScope
    {
        explicit GifEmitPathScope(uint8_t path) { detail::t_gifEmitPath = path; }
        ~GifEmitPathScope() { detail::t_gifEmitPath = 0u; }
        GifEmitPathScope(const GifEmitPathScope &) = delete;
        GifEmitPathScope &operator=(const GifEmitPathScope &) = delete;
    };
    inline uint8_t gifEmitPath()
    {
        return detail::t_gifEmitPath;
    }

    // Threaded: queue unit work. fbrst = the kicking context's VU0 FBRST
    // (VU1 D/T enables) as the synchronous MSCAL callback would read it.
    // vif = VIF1 work (a DMA kick or a VIF1 FIFO write): with the VPL2 VIF
    // stage on it runs on the MTVU-VIF thread; any other job runs whole on
    // the MTVU thread, as before.
    inline void submit(std::function<void()> fn, size_t bytes, uint32_t fbrst, bool vif = false)
    {
        detail::Job job;
        job.fn = std::move(fn);
        job.fpControl = ps2_fpmode::readControl();
        job.fbrst = fbrst;
        job.bytes = bytes;
        job.vif = vif;
        detail::noteEeThread();
        detail::worker().submit(std::move(job));
    }

    // MU2 counter: VIF1 input bytes (per Impl call) + UNPACK commands. Two
    // relaxed adds; the [mtvu] summary exchanges per window. Logged, not hashed.
    inline void noteVif1Bytes(uint32_t n)
    {
        detail::vif1Bytes().fetch_add(n, std::memory_order_relaxed);
    }
    inline void noteVif1Unpack()
    {
        detail::vif1Unpacks().fetch_add(1u, std::memory_order_relaxed);
    }

    // MP2 census (PS2X_MP2_CENSUS=1): UNPACK outcome per format + GIF bytes
    // per path. Callers check mp2Census() first (one relaxed load); the note
    // calls are 2-3 relaxed adds. Logged, never hashed.
    inline bool mp2Census()
    {
        return detail::mp2CensusFlag().load(std::memory_order_relaxed);
    }
    inline void setMP2Census(bool on)
    {
        detail::mp2CensusFlag().store(on, std::memory_order_relaxed);
    }
    // outcome: 0 bulk, 1 generic, 2 noop; reject: first failing bulk clause
    // (0 knob, 1 format, 2 mode, 3 cl!=wl, 4 mask), -1 when bulk/noop.
    inline void noteMp2Unpack(int fmt, int outcome, int reject, uint32_t bytes)
    {
        detail::Mp2Unpack &u = detail::mp2Unpack();
        if (outcome == 0)
        {
            u.bulkN[fmt].fetch_add(1u, std::memory_order_relaxed);
            u.bulkB[fmt].fetch_add(bytes, std::memory_order_relaxed);
        }
        else if (outcome == 1)
        {
            u.genN[fmt].fetch_add(1u, std::memory_order_relaxed);
            u.genB[fmt].fetch_add(bytes, std::memory_order_relaxed);
            if (reject >= 0 && reject < 5)
            {
                u.rejN[reject].fetch_add(1u, std::memory_order_relaxed);
                u.rejB[reject].fetch_add(bytes, std::memory_order_relaxed);
            }
        }
        else
        {
            u.noopN[fmt].fetch_add(1u, std::memory_order_relaxed);
            u.noopB[fmt].fetch_add(bytes, std::memory_order_relaxed);
        }
    }
    inline void noteMp2GifSubmit(int path, uint32_t bytes)
    {
        if (path < 1 || path > 3)
            return;
        detail::Mp2Gif &g = detail::mp2Gif();
        g.n[path].fetch_add(1u, std::memory_order_relaxed);
        g.bytes[path].fetch_add(bytes, std::memory_order_relaxed);
    }

    // R1: a masked CSR read touches the priv block without a sync.
    struct ExemptScope
    {
        explicit ExemptScope(bool on) : m_on(on)
        {
            if (m_on)
                detail::g_eeExempt.fetch_add(1, std::memory_order_relaxed);
        }
        ~ExemptScope()
        {
            if (m_on)
                detail::g_eeExempt.fetch_sub(1, std::memory_order_relaxed);
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

    // GE3 Part 4: narrowed FINISH-only variant of the analysis above. Returns
    // true iff this CSR load's consuming mask provably observes no
    // worker-owned bit other than FINISH (bit 1): the guest-visible value
    // cannot depend on worker timing, so retirement can be skipped. Same
    // delay-slot conservatism. SIGNAL/VSINT-touching masks return false.
    inline bool privReadFinishOnly(const uint8_t *rdram, uint32_t pc, uint32_t vaddr, uint32_t bytes)
    {
        const uint32_t phys = vaddr & 0x1FFFFFFFu;
        if ((phys & ~7u) != 0x12001000u || !rdram)
            return false;
        const uint32_t pcPhys = pc & 0x01FFFFFCu; // 32 MB RDRAM
        if (pcPhys + 8u > 0x02000000u)
            return false;
        uint32_t prev = 0, load = 0, next = 0;
        if (pcPhys >= 4u)
            std::memcpy(&prev, rdram + pcPhys - 4u, 4);
        std::memcpy(&load, rdram + pcPhys, 4);
        std::memcpy(&next, rdram + pcPhys + 4u, 4);
        const uint32_t pop = prev >> 26;
        const bool prevBranch = (pop == 0u && ((prev & 0x3Fu) == 8u || (prev & 0x3Fu) == 9u)) ||
                                pop == 1u || pop == 2u || pop == 3u || (pop >= 4u && pop <= 7u) ||
                                (pop >= 0x14u && pop <= 0x17u) ||
                                ((pop >= 0x10u && pop <= 0x12u) && ((prev >> 21) & 31u) == 8u);
        if (prevBranch)
            return false;
        const uint32_t rt = (load >> 16) & 31u;
        uint64_t mask = bytes >= 8u ? ~0ull : ((1ull << (bytes * 8u)) - 1u);
        if ((next >> 26) == 0x0Cu && ((next >> 21) & 31u) == rt && ((next >> 16) & 31u) == rt && rt != 0u)
            mask &= static_cast<uint64_t>(next & 0xFFFFu);
        mask <<= (phys & 7u) * 8u;
        return (mask & ~0x2ull) == 0u;
    }

    // GE3 Part 6: per-probe-episode FINISH tracking (diag observers only, never
    // guest state). An episode opens on a FINISH-only W1C clear and closes on
    // the first exempt read that observes FINISH set. Setter kinds: 1 = EE
    // submit thread, 2 = MTVU unit thread, 3 = GS worker (decode redundant).
    // Defined once in ps2_runtime.cpp; callable from the GS/MTVU/memory TUs.
    void ge3EpOnClear(uint32_t clearPc);
    void ge3EpOnSet(uint32_t kind);
    void ge3EpOnRead(bool finishSet);

    // Inside a unit-owned object: must be unit work or follow a sync.
    inline void touch(Site s)
    {
        if (!active())
            return;
        // One get_id (thread-register read) + relaxed loads; the GS
        // worker's own calls and host presentation skip, as before.
        if (detail::isUnitThread())
            return;
        if (!detail::isEeThread())
            return;
        if (detail::g_eeUnitDepth.load(std::memory_order_relaxed) != 0 ||
            detail::g_eeExempt.load(std::memory_order_relaxed) != 0)
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
            : m_on(on && census() && !detail::isUnitThread() &&
                   detail::g_eeUnitDepth.load(std::memory_order_relaxed) == 0),
              m_kind(kind)
        {
            if (!m_on)
                return;
            detail::noteEeThread();
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
            detail::g_eeUnitDepth.fetch_sub(1, std::memory_order_relaxed);
            m_running = false;
        }
        void resume()
        {
            if (!m_on || m_running)
                return;
            detail::g_eeUnitDepth.fetch_add(1, std::memory_order_relaxed);
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
        detail::noteEeThread();
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
            if (w.tailOn.load(std::memory_order_relaxed))
            {
                uint64_t openNs = 0u;
                if (w.busyPerVblank)
                {
                    // VG2 lever 1: credit the open vuLoop span up to now and
                    // restart it here. If the worker closed it meanwhile (CAS
                    // fails), its own fetch_add carries the span.
                    uint64_t since = w.busySinceNs.load(std::memory_order_acquire);
                    const uint64_t t = detail::nowNs() | 1u;
                    if (since != 0u && t > since &&
                        w.busySinceNs.compare_exchange_strong(since, t, std::memory_order_acq_rel))
                        openNs = t - since;
                }
                const uint64_t busyNs = w.tailBusyNs.exchange(0u, std::memory_order_relaxed) + openNs;
                ps2x::perflog::stageRing(ps2x::perflog::Stage::MtvuBusy)
                    .push(static_cast<uint32_t>(tick), static_cast<float>(busyNs / 1e6));
                if (detail::g_gifStage.load(std::memory_order_relaxed))
                {
                    // VPL1: the GIF thread's busy time, next to mtvu.busy.
                    const uint64_t gifNs = w.gif.busyNs.exchange(0u, std::memory_order_relaxed);
                    ps2x::perflog::stageRing(ps2x::perflog::Stage::MtvuGifBusy)
                        .push(static_cast<uint32_t>(tick), static_cast<float>(gifNs / 1e6));
                }
                if (detail::g_vifStage.load(std::memory_order_relaxed))
                {
                    // VPL2: the VIF thread's job time, next to mtvu.busy.
                    const uint64_t vifNs = w.vif.vifBusyNs.exchange(0u, std::memory_order_relaxed);
                    ps2x::perflog::stageRing(ps2x::perflog::Stage::MtvuVifBusy)
                        .push(static_cast<uint32_t>(tick), static_cast<float>(vifNs / 1e6));
                }
                // AD1: the session is bound to the MTVU TID; the report itself
                // may come from any thread, so vblank (GameThread) sends it.
                ps2x::adpf::report(ps2x::adpf::Thread::Mtvu, busyNs);
            }
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
