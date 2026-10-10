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

#include <atomic>
#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_gif_arena.h"

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
    // 24 was OrderedCsrWrite (retired, CU3); values stay stable for pktseq.
    FlushCaches = 25, // BG1: persist host-side caches on the worker at stream position (RPC)
    GifBatch = 26,    // GSB1: concatenated GifPackets (bytes + subs table); the worker
                      // runs each sub-packet through the per-packet path in order
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
// GSB1: GifBatch subs entry layout (bit 31 = the HasPath note rode with it,
// bits 30..24 = pathId, bits 23..0 = length). GifPathId fits in 7 bits.
constexpr uint32_t kGsGifBatchSubNote = 1u << 31;
constexpr uint32_t kGsGifBatchSubPathShift = 24;
constexpr uint32_t kGsGifBatchSubPathMask = 0x7Fu;
constexpr uint32_t kGsGifBatchSubLenMask = 0x00FFFFFFu;
// GSB2: an arena batch keeps the same subs entries and adds one subOff entry
// per sub (bits 31..17 = arena slot in the command's arenas list, bits
// 16..0 = byte offset in that arena). A batch over a 16 MiB cap spans at
// most 256 arenas, which is exactly the slot range; the producer publishes
// before the 256th.
constexpr uint32_t kGsGifBatchSubOffSlotShift = 17;
constexpr uint32_t kGsGifBatchSubOffSlotMax = 255u;
constexpr uint32_t kGsGifBatchSubOffMask = 0x1FFFFu;

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
    // GSB1: GifBatch sub-packet table, one entry per concatenated packet:
    // bit 31 = the kGsGifPacketHasPath note rode with it, bits 30..24 =
    // pathId, bits 23..0 = length. Empty for every other kind.
    std::vector<uint32_t> subs;
    // GSB2: arena batch only (empty otherwise, and bytes is empty then): the
    // referenced arenas (one ref each, released when the command retires)
    // and one subOff entry per sub, pointing into them.
    std::vector<GsGifArenaRef> arenas;
    std::vector<uint32_t> subOff;
    std::shared_ptr<GsRpcBase> rpc;          // non-null for RPC kinds
    std::unique_ptr<GSRasterBackend> backend; // SetBackend only
    std::function<void()> apply;              // PrivWrite only

    size_t payloadBytes() const
    {
        // GSB2: an arena batch's bytes are its views' lengths (the same sum
        // GSB1's contiguous bytes hold), so queue backpressure is unchanged.
        if (kind == GsCmdKind::GifBatch && !arenas.empty())
        {
            size_t total = bytes.size();
            for (const uint32_t sub : subs)
                total += static_cast<size_t>(sub & kGsGifBatchSubLenMask);
            return total;
        }
        return bytes.size();
    }
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
    // MP1 L3: caps sized for the in-flight depth (deferred wakes queue 64
    // commands before a wake; the worker may lag further), and a full pool
    // keeps the larger buffer. (CU4 B1: the pre-MP1 caps are deleted; these
    // are the only caps.)
    static constexpr size_t kMaxBuffers = 128;
    static constexpr size_t kMaxBytes = 16u * 1024u * 1024u;
    // Larger single buffers bypass the pool (rare multi-hundred-KB image
    // transfers must not evict the working set).
    static constexpr size_t kMaxBufferBytes = 256u * 1024u;

    void setEnabled(bool on) { m_enabled.store(on, std::memory_order_relaxed); }
    bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }

    // Take a buffer with capacity >= size when one is pooled, else a fresh
    // empty vector. The caller sizes/copies into it. Empty when disabled.
    std::vector<uint8_t> acquire(size_t size)
    {
        if (!enabled() || size == 0u || size > kMaxBufferBytes)
            return {};
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
        // MP1 L3: swap out the smallest pooled buffer when this one is
        // larger and fits the byte cap in its place (a pool full of small
        // buffers would otherwise miss every larger packet: the unit
        // allocates and the GS worker frees it).
        Lock lock(m_lock);
        if (m_free.size() >= kMaxBuffers || m_bytes + cap > kMaxBytes)
        {
            size_t smallest = m_free.size();
            for (size_t i = 0; i < m_free.size(); ++i)
                if (smallest == m_free.size() || m_free[i].capacity() < m_free[smallest].capacity())
                    smallest = i;
            if (smallest == m_free.size() || m_free[smallest].capacity() >= cap ||
                m_bytes - m_free[smallest].capacity() + cap > kMaxBytes)
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

    // PKB1 item 3: return a batch of consumed buffers under a single lock
    // round instead of one round per packet. Same caps and swap-out rule as
    // release(), applied per buffer in order; buffers that don't fit are
    // freed (left for their owners to drop). The batch vector is consumed.
    void releaseBulk(std::vector<std::vector<uint8_t>> &batch)
    {
        if (batch.empty())
            return;
        if (!enabled())
        {
            batch.clear();
            return;
        }
        Lock lock(m_lock);
        for (auto &bytes : batch)
        {
            const size_t cap = bytes.capacity();
            if (cap == 0u || cap > kMaxBufferBytes)
                continue;
            bytes.clear();
            if (m_free.size() >= kMaxBuffers || m_bytes + cap > kMaxBytes)
            {
                size_t smallest = m_free.size();
                for (size_t i = 0; i < m_free.size(); ++i)
                    if (smallest == m_free.size() || m_free[i].capacity() < m_free[smallest].capacity())
                        smallest = i;
                if (smallest == m_free.size() || m_free[smallest].capacity() >= cap ||
                    m_bytes - m_free[smallest].capacity() + cap > kMaxBytes)
                    continue;
                m_bytes -= m_free[smallest].capacity();
                m_bytes += cap;
                // The smaller buffer leaves through `bytes`; freed below.
                std::swap(m_free[smallest], bytes);
                continue;
            }
            m_bytes += cap;
            m_free.push_back(std::move(bytes));
        }
        batch.clear();
    }

    size_t pooledCount() const
    {
        Lock lock(m_lock);
        return m_free.size();
    }
    size_t pooledBytes() const
    {
        Lock lock(m_lock);
        return m_bytes;
    }

private:
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
    mutable std::atomic_flag m_lock; // C++20: default-constructs clear
    std::vector<std::vector<uint8_t>> m_free; // guarded by m_lock
    size_t m_bytes = 0;                       // guarded by m_lock
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
    // MP1 L2: lean handoff is unconditional (CU4 B1): fewer mutex rounds and
    // futex calls per handoff. (a) Producers notify m_hasWork only while the
    // worker sleeps, and the worker notifies m_hasSpace only while a producer
    // waits for space; (b) the worker clears m_executing inside its next pop
    // lock instead of a second lock per pop; (c) local batches (below). Same
    // commands, same order, same bounds: wake timing only. Requires the
    // deferred wakes.
    // MP1 L2 (c): a thread-local batch for the MTVU unit thread (deferred
    // wakes on). No mutex round: enqueues inside it defer their wake like an
    // endBatch(mayDefer) would (wake now once the deferred-wake thresholds are
    // reached, else leave it pending for flushWake at job end). Nest-safe;
    // the thread must call flushWake() before it waits on anything but an RPC.
    static void beginLocalBatch();
    static void endLocalBatch();
    // GPK1 (PS2X_MTVU_GS_BATCH=1, default off): staged publish. On the
    // MTVU-GIF thread, a fire-and-forget enqueue inside a local batch is
    // staged thread-locally and published later under one queue-mutex round
    // (the GS worker queue lock was the contended per-command cost, GPK1
    // attribution). Each staged command is then admitted exactly as
    // enqueue() would: same order, bounds, backpressure and deferred-wake
    // rules. flushStaged() publishes; the GIF stage calls it before it
    // publishes consumed ops (so anything that waits on the GIF stage sees
    // the commands queued), flushWake() and any other enqueue from the
    // thread (RPCs included) publish first, and a stage reaching
    // kStageMaxCommands/kStageMaxBytes publishes itself. Host transport only.
    static constexpr size_t kStageMaxCommands = 64;
    static constexpr size_t kStageMaxBytes = 256u * 1024u;
    static void setStagedPublish(bool on);
    static void flushStaged();
    // GSB1 (PS2X_GS_GIF_BATCH=1, default off): GIF batch command. On the
    // MTVU-GIF thread, a GifPacket enqueued inside a local batch is appended
    // to a thread-local batch (concatenated bytes + the subs table) instead
    // of taking the queue; publish admits one GifBatch command under one
    // queue-mutex round. Publish points are exactly GPK1's: the GIF stage's
    // publishHead hook (gifPublishFn), flushWake (Call/JobEnd/idle), any
    // other enqueue from the thread (RPCs included, published first so the
    // RPC keeps its stream position), and a full batch (maxBytes, default
    // 64 KiB). A single packet larger than maxBytes (or than the 24-bit
    // length field) publishes the pending batch and enqueues alone, as today.
    // When both knobs are on, batching supersedes staged publish on the
    // batching thread (one pending structure, so FIFO order is trivial);
    // staged publish is untouched everywhere else. Host transport only: the
    // worker runs each sub-packet through today's per-packet code.
    static constexpr size_t kGifBatchDefaultBytes = 65536u;
    static void setGifBatch(bool on, size_t maxBytes);
    static void flushGifBatch();
    // Receipts (also on the [gs:handoff] line): batches published and
    // sub-packets batched, process-wide. Tests read deltas.
    static uint64_t gifBatchRounds();
    static uint64_t gifBatchedCmds();
    // Pool the published batch's source vectors return to (one bulk release
    // per batch; null = free them). Set once, before producers run.
    void setBatchPool(GsPacketPool *pool);
    // GSB2 (PS2X_GS_GIF_ARENA=1, default off; needs PS2X_GS_GIF_BATCH=1): the
    // batching thread appends arena views instead of copying bytes. Same
    // publish points, same sub-packet order and bytes, same worker-side
    // per-packet path; owned-byte packets enqueue alone, as oversize ones do.
    static void setGifArena(bool on);
    static bool gifArenaOn();
    // A view append: batch it under the batch rules (cap, publish points),
    // else publish first and enqueue it alone with its bytes copied. Only
    // the batching thread calls this, like the GifPacket path in enqueue().
    void enqueueView(uint8_t pathId, bool notePath, GsGifArenaRef arena, uint32_t off, uint32_t len);
    // Receipts: arena views batched (process-wide; tests read deltas).
    static uint64_t gifArenaViews();
    // TLS2 test hook: true iff the calling thread is served from the cached
    // TLS addresses (knob on + noted GIF thread). Production never calls this.
    static bool tlsCacheLiveForTest();

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
    // Admits one command with m_mutex held (enqueue's body). Returns whether
    // the caller must notify m_hasWork after unlocking. Shutdown runs the
    // command inline and returns with the lock released.
    bool admitLocked(std::unique_lock<std::mutex> &lock, GsCommand &cmd);
    void publishStaged();
    void publishBatch();

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

    // GSB1: pool for published batches' source vectors (GS::setQueueEnabled
    // points it at the GS packet pool; null in tests, where sources free).
    GsPacketPool *m_batchPool = nullptr;

    // Monotonic diagnostics, safe to read from any thread.
    std::atomic<uint64_t> m_enqueuedCount{0};
    std::atomic<uint64_t> m_executedCount{0};
    std::atomic<uint64_t> m_wakes{0};    // notifies of m_hasWork by producers
    std::atomic<uint64_t> m_deferred{0}; // endBatch wakes kept pending (H3)
    std::atomic<uint64_t> m_watchdog{0}; // H3: pending wake found stale by the worker
};
