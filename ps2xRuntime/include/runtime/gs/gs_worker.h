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
    std::function<void()> apply;              // PrivWrite only

    size_t payloadBytes() const { return bytes.size(); }
};

class GsWorker
{
public:
    static constexpr size_t kDefaultMaxDescriptors = 1024;
    static constexpr size_t kDefaultMaxPayloadBytes = 16u * 1024u * 1024u;

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

    // NP1: coalesce handoff wakeups across a drain. Between beginBatch and
    // endBatch, enqueue() queues without notifying; endBatch notifies once
    // if anything was queued. Nest-safe. Queue order, bounds and
    // backpressure are unchanged; the worker's wait predicate re-check makes
    // the deferred wakeup race-free. No batch may span a synchronous RPC wait.
    void beginBatch();
    void endBatch();

    size_t pendingCount() const;
    size_t pendingBytes() const;
    // True when the queue is empty AND no command is executing (checked
    // under one mutex). A Fence issued while quiescent is a proven no-op,
    // so drainers may skip it; all prior effects are visible via the mutex.
    bool isQuiescent() const;
    uint64_t enqueuedCount() const { return m_enqueuedCount.load(std::memory_order_relaxed); }
    uint64_t executedCount() const { return m_executedCount.load(std::memory_order_relaxed); }

private:
    void threadMain();

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
    bool m_stopRequested = false;
    bool m_running = false;
    std::thread m_thread;

    // Monotonic diagnostics, safe to read from any thread.
    std::atomic<uint64_t> m_enqueuedCount{0};
    std::atomic<uint64_t> m_executedCount{0};
};
