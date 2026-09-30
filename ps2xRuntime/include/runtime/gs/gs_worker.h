#pragma once

// GB2 step (a): the CPU GS backend on its own thread behind a queue.
//
// This header is the transport only: a bounded multi-producer /
// single-consumer command queue plus the GS worker thread that drains it.
// The consumer is the *unmodified* GS decode + CPU backend (see
// GS::executeQueuedCommand in gs_frontend.cpp): "CPU mode" is the existing
// frontend decode moved to run on the GS thread, fed by this ring instead
// of direct calls (GB1 DESIGN.md section 1b/2a).
//
// Ordering: commands execute strictly FIFO on the worker thread. Fire-and-
// forget submits (gif packets, register writes, native uploads) preserve
// EE submission order; synchronous operations (readbacks, present latch,
// reset) are RPCs that carry a fence the caller waits on, so they execute
// at stream position after all prior commands (GB1 section 2c).
//
// Bounds (GB1 section 2d provisional): 1024 packet descriptors / 16 MiB of
// payload bytes. A producer that finds the queue full blocks until the
// worker drains space (the hardware analogue is a GIF FIFO-full stall).
// A single command larger than the byte cap is still enqueued (descriptor
// cap only) so oversize input cannot deadlock the producer; the largest
// real input is a 1 MiB DMA transfer (16-bit QWC).

#include <array>
#include <atomic>
#include <bit>
#include "runtime/gs/gs_backend.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

enum class GsCmdKind : uint8_t
{
    // Fire-and-forget (ordered, no caller wait).
    GifPacket = 0,
    NoteGifPath,
    RegWrite,
    UploadImageNative,
    NativePacked,
    ClearCtx,
    ClearActive,
    WriteVram,
    ClearDebugHistory,
    SetDebugPaused,
    PrivWrite, // GB3: guest/HLE priv-register store, applied in stream order
    GuestVsync,  // GE2: guest VBlank boundary (tick in regValue, CSR FIELD in u32a)
    // RPCs (carry a fence; the caller waits for stream position).
    Consume,
    ReadVram,
    RefreshSnapshot,
    LatchPresent,
    Reset,
    GetDebugSnapshot,
    GetDebugHistory,
    IsDebugPaused,
    GetPreferredSource,
    SetBackend,
    Fence,
    DiagPresent, // GB3: present into a caller-owned frame (no latch side effects)
    OrderedCsrWrite, // O: typed SIGNAL/FINISH W1C after prior GIF packets
    FlushCaches, // BG1: persist host-side caches on the worker at stream position (RPC)
};

// Base fence for RPC commands. The worker signals it after executing the
// command; results ride on the GsRpc<T> subclass the caller holds.
struct GsRpcBase
{
    virtual ~GsRpcBase() = default;
    void wait()
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return done; });
    }
    void signal()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        cv.notify_all();
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
};

template <typename T>
struct GsRpc : public GsRpcBase
{
    T result{};
};

// GF1 H1 (PS2X_GS_HANDOFF_DIET): GifPacket.u32a flag. The command also
// carries its GIF path in pathId; the worker applies it just before the
// packet, exactly as a NoteGifPath command queued right ahead of it would.
constexpr uint32_t kGsGifPacketHasPath = 1u;

struct GsCommand
{
    GsCmdKind kind = GsCmdKind::Fence;
    uint8_t pathId = 0;      // GifPacket / NoteGifPath (GifPathId value)
    uint8_t regAddr = 0;     // RegWrite
    uint64_t regValue = 0;   // RegWrite
    uint64_t setupRegs[4] = {}; // UploadImageNative (BITBLTBUF/TRXPOS/TRXREG/TRXDIR)
    // ReadVram/WriteVram: psm, base, bw, x, y (+ value lo/hi via regValue).
    // ClearCtx: context index in u32a, rgba in u32b.
    // Consume: maxBytes in u32a. SetDebugPaused: flag in u32a.
    uint32_t u32a = 0;
    uint32_t u32b = 0;
    uint32_t u32c = 0;
    uint32_t u32d = 0;
    uint32_t u32e = 0;
    std::vector<uint8_t> bytes;              // GifPacket / UploadImageNative / NativePacked payload
    std::shared_ptr<GsRpcBase> rpc;          // non-null for RPC kinds
    std::unique_ptr<GSRasterBackend> backend; // SetBackend only
    std::function<void()> apply;              // PrivWrite / OrderedCsrWrite

    size_t payloadBytes() const { return bytes.size(); }
};

// GP4 H5 (PS2X_GS_HANDOFF_DIET): pool of reusable packet byte-buffers.
//
// Every GIF packet mallocs on the producer (arbiter submit, or the direct
// assign path) and frees on the GS worker when its command retires. That
// per-packet cycle is scudo + 16-byte-CAS + allocator-mutex time on the
// worker. The pool turns the steady state into pointer moves under a brief
// spinlock: producers acquire a buffer with room, the worker releases the
// consumed one. Bytes are copied identically either way, so the GS stream
// (and det) is unchanged; queue order, bounds and backpressure are
// untouched. Null/disabled = today's direct alloc/free (the knob-off path).
class GsPacketPool
{
public:
    static constexpr size_t kMaxBuffers = 32;
    static constexpr size_t kMaxBytes = 8u * 1024u * 1024u;
    // MP1 L3 (setLean): caps sized for the in-flight depth (deferred wakes
    // queue 64 commands before a wake; the worker may lag further), and a
    // full pool keeps the larger buffer. Without it, a pool full of small
    // buffers misses every larger packet: the unit allocates and the GS
    // worker frees it (~0.5 ms/f of scudo on the Odin worker, MP1 pre-check).
    static constexpr size_t kLeanMaxBuffers = 128;
    static constexpr size_t kLeanMaxBytes = 16u * 1024u * 1024u;
    // Larger single buffers bypass the pool (rare multi-hundred-KB image
    // transfers must not evict the working set).
    static constexpr size_t kMaxBufferBytes = 256u * 1024u;

    void setEnabled(bool on) { m_enabled.store(on, std::memory_order_relaxed); }
    bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }
    // MP1 L3: set once, before producers run.
    void setLean(bool on) { m_lean.store(on, std::memory_order_relaxed); }
    bool lean() const { return m_lean.load(std::memory_order_relaxed); }
    // VG2 lever 2 (PS2X_GS_POOL_O1=1): size-class free lists, so a release
    // at the cap no longer scans every pooled buffer for the smallest (VG1:
    // ~0.36 ms/f of the Odin GS worker). Set once, before producers run.
    // Same bytes, same order; only which pooled buffer is reused changes.
    void setO1(bool on)
    {
        if (on)
            for (auto &c : m_cls)
                c.reserve(kLeanMaxBuffers);
        m_o1.store(on, std::memory_order_relaxed);
    }
    bool o1() const { return m_o1.load(std::memory_order_relaxed); }

    // Take a buffer with capacity >= size when one is pooled, else a fresh
    // empty vector. The caller sizes/copies into it. Empty when disabled.
    std::vector<uint8_t> acquire(size_t size)
    {
        if (!enabled() || size == 0u || size > kMaxBufferBytes)
            return {};
        if (o1())
            return acquireO1(size);
        Lock lock(m_lock);
        for (size_t i = m_free.size(); i-- > 0u;)
        {
            if (m_free[i].capacity() >= size)
            {
                std::vector<uint8_t> out = std::move(m_free[i]);
                m_free[i] = std::move(m_free.back());
                m_free.pop_back();
                m_bytes -= out.capacity();
                return out;
            }
        }
        return {};
    }

    // Return a consumed buffer (moved-from after). Dropped (freed) when
    // disabled, empty, oversize, or over the caps.
    void release(std::vector<uint8_t> &&bytes)
    {
        const size_t cap = bytes.capacity();
        if (!enabled() || cap == 0u || cap > kMaxBufferBytes)
            return;
        bytes.clear();
        const bool lean = m_lean.load(std::memory_order_relaxed);
        const size_t maxBuffers = lean ? kLeanMaxBuffers : kMaxBuffers;
        const size_t maxBytes = lean ? kLeanMaxBytes : kMaxBytes;
        if (o1())
        {
            releaseO1(std::move(bytes), cap, lean, maxBuffers, maxBytes);
            return;
        }
        Lock lock(m_lock);
        if (m_free.size() >= maxBuffers || m_bytes + cap > maxBytes)
        {
            if (!lean)
                return;
            // MP1 L3: swap out the smallest pooled buffer when this one is
            // larger and fits the byte cap in its place.
            size_t smallest = m_free.size();
            for (size_t i = 0; i < m_free.size(); ++i)
                if (smallest == m_free.size() || m_free[i].capacity() < m_free[smallest].capacity())
                    smallest = i;
            if (smallest == m_free.size() || m_free[smallest].capacity() >= cap ||
                m_bytes - m_free[smallest].capacity() + cap > maxBytes)
                return;
            m_bytes -= m_free[smallest].capacity();
            m_bytes += cap;
            // The smaller buffer leaves through `bytes`; its owner frees it.
            std::swap(m_free[smallest], bytes);
            return;
        }
        m_bytes += cap;
        m_free.push_back(std::move(bytes));
    }

    size_t pooledCount() const
    {
        Lock lock(m_lock);
        return o1() ? m_count : m_free.size();
    }
    size_t pooledBytes() const
    {
        Lock lock(m_lock);
        return m_bytes;
    }

private:
    // VG2: class c holds capacities in [2^c, 2^(c+1)); kMaxBufferBytes = 2^18.
    static constexpr unsigned kClasses = 19;
    static unsigned sizeClass(size_t n) // n >= 1
    {
        const unsigned c = static_cast<unsigned>(std::bit_width(n)) - 1u;
        return c < kClasses ? c : kClasses - 1u;
    }
    static unsigned lowestBit(uint32_t m) { return static_cast<unsigned>(std::countr_zero(m)); } // m != 0
    std::vector<uint8_t> popClass(unsigned c) // m_lock held, class non-empty
    {
        auto &v = m_cls[c];
        std::vector<uint8_t> out = std::move(v.back());
        v.pop_back();
        if (v.empty())
            m_mask &= ~(1u << c);
        --m_count;
        m_bytes -= out.capacity();
        return out;
    }
    void pushClass(std::vector<uint8_t> &&b, size_t cap) // m_lock held
    {
        const unsigned c = sizeClass(cap);
        m_cls[c].push_back(std::move(b));
        m_mask |= (1u << c);
        ++m_count;
        m_bytes += cap;
    }
    std::vector<uint8_t> acquireO1(size_t size)
    {
        const unsigned k = sizeClass(size);
        Lock lock(m_lock);
        auto &own = m_cls[k];
        // Own class: its last buffer when it fits (a best fit).
        if (!own.empty() && own.back().capacity() >= size)
            return popClass(k);
        // Any larger class fits by construction: take the smallest one.
        const uint32_t higher = (k + 1u < kClasses) ? (m_mask & ~((2u << k) - 1u)) : 0u;
        if (higher != 0u)
            return popClass(lowestBit(higher));
        // Last resort: the rest of the own class (bounded by its size).
        for (size_t i = own.size(); i-- > 0u;)
        {
            if (own[i].capacity() >= size)
            {
                std::swap(own[i], own.back());
                return popClass(k);
            }
        }
        return {};
    }
    void releaseO1(std::vector<uint8_t> &&bytes, size_t cap, bool lean, size_t maxBuffers, size_t maxBytes)
    {
        Lock lock(m_lock);
        if (m_count >= maxBuffers || m_bytes + cap > maxBytes)
        {
            if (!lean || m_mask == 0u)
                return;
            // MP1 L3 in O(1): swap out a buffer of the smallest class when
            // this one is larger and fits the byte cap in its place.
            auto &v = m_cls[lowestBit(m_mask)];
            const size_t victimCap = v.back().capacity();
            if (victimCap >= cap || m_bytes - victimCap + cap > maxBytes)
                return;
            std::vector<uint8_t> victim = popClass(lowestBit(m_mask));
            pushClass(std::move(bytes), cap);
            // The smaller buffer leaves through `bytes`; its owner frees it.
            bytes = std::move(victim);
            return;
        }
        pushClass(std::move(bytes), cap);
    }

    struct Lock
    {
        explicit Lock(std::atomic_flag &flag) : m_flag(flag)
        {
            while (m_flag.test_and_set(std::memory_order_acquire))
            {
                // Brief critical sections, 2-3 threads; spin, don't sleep.
            }
        }
        ~Lock() { m_flag.clear(std::memory_order_release); }
        Lock(const Lock &) = delete;
        Lock &operator=(const Lock &) = delete;
        std::atomic_flag &m_flag;
    };

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_lean{false}; // MP1 L3
    mutable std::atomic_flag m_lock; // C++20: default-constructs clear
    std::vector<std::vector<uint8_t>> m_free; // guarded by m_lock
    size_t m_bytes = 0;                       // guarded by m_lock
    // VG2 lever 2 (o1 mode only; guarded by m_lock).
    std::atomic<bool> m_o1{false};
    std::array<std::vector<std::vector<uint8_t>>, kClasses> m_cls;
    uint32_t m_mask = 0; // bit c set iff m_cls[c] is non-empty
    size_t m_count = 0;
};

class GsWorker
{
public:
    static constexpr size_t kDefaultMaxDescriptors = 1024;
    static constexpr size_t kDefaultMaxPayloadBytes = 16u * 1024u * 1024u;
    // GP4 H6: the worker pops up to this many commands per mutex acquisition
    // (FIFO order preserved). The queue mutex + its futex wakes cost ~0.5 ms
    // per frame on the GS worker at one lock round per packet.
    static constexpr size_t kPopBatch = 8;
    // GW4 (setWorkerLean): the lean worker pops up to this many commands (or
    // kWorkerLeanBatchBytes of payload, whichever first) per mutex round.
    static constexpr size_t kWorkerLeanBatch = 64;
    static constexpr size_t kWorkerLeanBatchBytes = 1u * 1024u * 1024u;

    using Handler = std::function<void(GsCommand &)>;

    GsWorker(size_t maxDescriptors, size_t maxPayloadBytes, Handler handler);
    ~GsWorker();

    GsWorker(const GsWorker &) = delete;
    GsWorker &operator=(const GsWorker &) = delete;

    void start(); // Idempotent. Spawns the GS thread ("GsWorker").
    void stop();  // Idempotent. Drains queued commands, joins the thread.

    // Enqueue a command; blocks while the queue is full. Safe from any
    // producer thread (game thread submits, main thread presents).
    void enqueue(GsCommand cmd);
    // PT2 Part 2: caller-side backpressure accounting. The calling thread's
    // enqueue waits (queue-full blocks) accumulate ns into *sink while set;
    // null (default) measures nothing. Thread-local: the EE sets it to its
    // wait accumulator around run() so backpressure files under ee.wait.
    // Knob-off cost is one thread-local read per enqueue.
    static void setEnqueueWaitSink(uint64_t *sink);

    // NP1: coalesce handoff wakeups across a drain. Between beginBatch and
    // endBatch, enqueue() queues without notifying; endBatch notifies once
    // if anything was queued. Nest-safe. Queue order, bounds and
    // backpressure are unchanged; the worker's wait predicate re-check makes
    // the deferred wakeup race-free. No batch may span a synchronous RPC wait.
    void beginBatch();
    // GF1 H3: `mayDefer` (the MTVU unit thread, with deferred wakes set)
    // lets the outermost endBatch keep its wake pending while fewer than
    // the set commands/bytes are queued; flushWake(), a later endBatch, any
    // non-batched or RPC enqueue, or a producer about to block delivers it.
    void endBatch(bool mayDefer = false);
    // GF1 H3: deliver a pending deferred wake (the unit calls it at job end).
    void flushWake();
    // GF1 H3: 0 = off (NP1 behaviour). Set once, before producers run.
    void setDeferredWakes(uint32_t wakeCommands, size_t wakeBytes);
    // GP4 H6: worker pop batch size, 1 (default, one-by-one) to kPopBatch.
    // Set once, before producers run.
    void setPopBatch(size_t n);
    // MP1 L2 (PS2X_GS_LEAN_HANDOFF, on with the diet): fewer mutex rounds and
    // futex calls per handoff. (a) Producers notify m_hasWork only while the
    // worker sleeps, and the worker notifies m_hasSpace only while a producer
    // waits for space; (b) the worker clears m_executing inside its next pop
    // lock instead of a second lock per pop; (c) local batches (below). Same
    // commands, same order, same bounds: wake timing only. Set once, before
    // producers run, and only together with deferred wakes.
    void setLeanHandoff(bool on);
    bool leanHandoff() const { return m_lean.load(std::memory_order_relaxed); }
    // MP1 L2 (c): a thread-local batch for the MTVU unit thread (lean + deferred
    // wakes on). No mutex round: enqueues inside it defer their wake like an
    // endBatch(mayDefer) would (wake now once the deferred-wake thresholds are
    // reached, else leave it pending for flushWake at job end). Nest-safe;
    // the thread must call flushWake() before it waits on anything but an RPC.
    static void beginLocalBatch();
    static void endLocalBatch();
    // GW4 (PS2X_GS_WORKER_LEAN=1, default off; needs setLeanHandoff): the
    // worker drains up to kWorkerLeanBatch commands per lock round into a
    // reused batch (no per-pop command array construction), and wakes space
    // waiters only once the queue has drained below 3/4 of its caps (or is
    // empty) instead of after every pop, which turned a full queue into a
    // producer wake/sleep ping-pong. Same commands, same order, same
    // fences/RPCs; lock and wake timing only. Set once, before producers run
    // (the thread picks the loop at its next round boundary).
    void setWorkerLean(bool on) { m_workerLean.store(on, std::memory_order_relaxed); }
    bool workerLean() const { return m_workerLean.load(std::memory_order_relaxed); }

    size_t pendingCount() const;
    size_t pendingBytes() const;
    // True when the queue is empty AND no command is executing (checked
    // under one mutex). A Fence issued while quiescent is a proven no-op,
    // so drainers may skip it; all prior effects are visible via the mutex.
    bool isQuiescent() const;
    uint64_t enqueuedCount() const { return m_enqueuedCount.load(std::memory_order_relaxed); }
    uint64_t executedCount() const { return m_executedCount.load(std::memory_order_relaxed); }
    uint64_t wakeCount() const { return m_wakes.load(std::memory_order_relaxed); }
    uint64_t deferredCount() const { return m_deferred.load(std::memory_order_relaxed); }
    uint64_t watchdogCount() const { return m_watchdog.load(std::memory_order_relaxed); }

private:
    void threadMain();
    void threadMainLean(); // GW4

    Handler m_handler;
    const size_t m_maxDescriptors;
    const size_t m_maxPayloadBytes;

    mutable std::mutex m_mutex;
    std::condition_variable m_hasWork;
    std::condition_variable m_hasSpace;
    std::deque<GsCommand> m_queue;
    bool m_executing = false; // set under m_mutex around the handler call
    size_t m_queuedBytes = 0;
    uint32_t m_batchDepth = 0; // guarded by m_mutex
    bool m_batchDirty = false; // guarded by m_mutex
    // GF1 H3 (guarded by m_mutex; wake thresholds fixed before producers run).
    uint32_t m_wakeCommands = 0;
    size_t m_wakeBytes = 0;
    // GP4 H6: pop batch size (fixed before producers run; read on the worker
    // without the mutex, like m_maxDescriptors).
    size_t m_popBatch = 1;
    uint64_t m_deferredSinceNs = 0; // when the pending deferred wake began
    bool m_stopRequested = false;
    bool m_running = false;
    std::thread m_thread;
    // MP1 L2 (guarded by m_mutex): the worker is (about to be) blocked on
    // m_hasWork / producers blocked on m_hasSpace. Lean mode notifies only then.
    bool m_workerIdle = false;
    uint32_t m_spaceWaiters = 0;
    std::atomic<bool> m_lean{false}; // fixed before producers run
    std::atomic<bool> m_workerLean{false}; // GW4: set before producers run

    // Monotonic diagnostics, safe to read from any thread.
    std::atomic<uint64_t> m_enqueuedCount{0};
    std::atomic<uint64_t> m_executedCount{0};
    std::atomic<uint64_t> m_wakes{0};    // notifies of m_hasWork by producers
    std::atomic<uint64_t> m_deferred{0}; // endBatch wakes kept pending (H3)
    std::atomic<uint64_t> m_watchdog{0}; // H3: pending wake found stale by the worker
};
