#ifndef PS2_GS_FRONTEND_H
#define PS2_GS_FRONTEND_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_worker.h"
#include "runtime/gs/ps2_gif_arbiter.h"

struct GSDebugSnapshot
{
    GSContext ctx[2]{};
    GSPrimReg prim{};
    GSTexaReg texa{};
    GSTexClutReg texclut{};
    uint64_t scanmsk = 0;
    uint64_t dimx = 0;
    uint64_t dthe = 0;
    uint64_t colclamp = 0;
    GSBitBltBuf bitbltbuf{};
    GSTrxPos trxpos{};
    GSTrxReg trxreg{};
    uint32_t trxdir = 0;
    uint32_t transferX = 0;
    uint32_t transferY = 0;
    uint32_t transferTotalPixels = 0;
    uint32_t transferCopiedPixels = 0;
    uint32_t lastDisplayBaseBytes = 0;
    GSFrameReg preferredDisplaySourceFrame{};
    uint32_t preferredDisplayDestFbp = 0;
    bool hasPreferredDisplaySource = false;
    uint32_t hostPresentationWidth = 0;
    uint32_t hostPresentationHeight = 0;
    uint32_t hostPresentationDisplayFbp = 0;
    uint32_t hostPresentationSourceFbp = 0;
    bool hostPresentationUsedPreferred = false;
    bool hasHostPresentationFrame = false;
    size_t localToHostPendingBytes = 0;
};

// MQ2: number of A+D FINISH writes in a GIF packet (0 if malformed first).
uint32_t ps2xGifFinishWrites(const uint8_t *data, uint32_t sizeBytes);

enum class GSDebugEventKind : uint8_t
{
    GifTag = 0,
    Register = 1,
    Draw = 2,
    Transfer = 3,
    Present = 4,
};

struct GSDebugHistoryEntry
{
    uint64_t seq = 0;
    uint64_t vsyncTick = 0;
    uint32_t frameIndex = 0;
    GSDebugEventKind kind = GSDebugEventKind::Register;

    uint8_t reg = 0;
    uint64_t regValue = 0;

    uint32_t gifSizeBytes = 0;
    uint32_t gifNloop = 0;
    uint8_t gifFlg = 0;
    uint8_t gifNreg = 0;

    GSPrimReg prim{};
    GSFrameReg frame{};
    GSZbufReg zbuf{};
    GSTex0Reg tex0{};
    GSScissorReg scissor{};
    uint64_t test = 0;
    uint64_t alpha = 0;

    uint32_t vertexCount = 0;
    float xMin = 0.0f;
    float xMax = 0.0f;
    float yMin = 0.0f;
    float yMax = 0.0f;
    double zMin = 0.0;
    double zMax = 0.0;
    uint8_t aMin = 0;
    uint8_t aMax = 0;

    GSBitBltBuf bitbltbuf{};
    GSTrxPos trxpos{};
    GSTrxReg trxreg{};
    uint32_t trxdir = 0;
    uint32_t transferPixels = 0;

    uint32_t displayFbp = 0;
    uint32_t sourceFbp = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool usedPreferred = false;
};

class GS
{
public:
    GS();
    ~GS();

    void init(uint8_t *vram, uint32_t vramSize, struct GSRegisters *privRegs = nullptr);
    // GB2 step (a): when enabled, mutation submits enqueue onto the GS
    // worker thread and synchronous operations become stream-ordered RPCs.
    // Disabled (default) the code path is identical to the direct calls.
    // Enable once, while no other thread uses this GS (the runtime enables
    // from syncCoreSubsystems during init, before the game thread spawns).
    // GF1 H4: maxDescriptors 0 = GsWorker::kDefaultMaxDescriptors.
    bool setQueueEnabled(bool enabled, size_t maxDescriptors = 0);
    bool queueEnabled() const { return m_worker != nullptr; }
    // NP1: coalesce worker wakeups to one per batch (direct mode: no-op).
    void beginWorkerBatch() { if (m_worker) m_worker->beginBatch(); }
    // GF1 H3: mayDefer = the caller is the MTVU unit thread (see GsWorker).
    void endWorkerBatch(bool mayDefer = false) { if (m_worker) m_worker->endBatch(mayDefer); }
    void flushWorkerWake() { if (m_worker) m_worker->flushWake(); }
    // MP1 L2: lean handoff is unconditional (CU4 B1); local batches need the
    // worker only.
    bool workerLocalBatchesOk() const { return m_worker != nullptr; }
    void setWorkerDeferredWakes(uint32_t wakeCommands, size_t wakeBytes)
    {
        if (m_worker)
            m_worker->setDeferredWakes(wakeCommands, wakeBytes);
    }
    // GP4 H5: pool of reusable packet buffers (the diet block enables it;
    // the arbiter borrows it). Disabled = direct alloc/free.
    void setPacketPoolEnabled(bool on) { m_packetPool.setEnabled(on); }
    bool packetPoolEnabled() const { return m_packetPool.enabled(); }
    GsPacketPool &packetPool() { return m_packetPool; }
    // GP4 H6: worker pop batch size (the diet block sets kPopBatch).
    void setWorkerPopBatch(size_t n)
    {
        if (m_worker)
            m_worker->setPopBatch(n);
    }
    // PKB1 item 3: batch pool releases per worker wake. While on, the worker
    // stashes consumed payload buffers and returns up to kPopBatch of them
    // under one pool lock round (releaseBulk) instead of one round per
    // packet. Off = release each command's bytes immediately (today's path).
    void setReleaseBatching(bool on);
    bool releaseBatching() const { return m_releaseBatching; }
    // Blocks until all previously enqueued commands have executed.
    void drainQueue();
    // BG1: persist the backend's host-side caches (external GS only; a no-op
    // elsewhere). Queued mode runs it on the worker at stream position (an
    // RPC this call waits on); direct mode runs it now.
    void flushExternalCaches();
    // N8D7M12 Part 5F4P2: dev-only default-off fingerprint of commands in
    // the order the GS worker actually consumes them. Fence is excluded
    // from the digest/count (its presence is timing-dependent: drainQueue
    // skips it when quiescent); a Fence only snapshots running->snapshot.
    // PrivWrite contributes its kind tag only (apply is opaque).
    void setPktSeqEnabled(bool enabled);
    bool pktSeqEnabled() const;
    uint64_t pktSeqSnapshot() const;
    uint64_t pktSeqSnapshotCommands() const;
    // GB2 Part 2: monotonic counters. submitCount covers executed
    // packets (processGIFPacket incl. the native-image shape, direct
    // uploadImageNative, direct processNativePackedGIFPacket — each counted
    // once at execution, so drain-then-read is exact in both modes);
    // regWriteCount covers public writeRegister calls (HLE W1/W2 shape).
    uint64_t submitCount() const { return m_submitCount.load(std::memory_order_relaxed); }
    uint64_t regWriteCount() const { return m_regWriteCount.load(std::memory_order_relaxed); }
    void reset();
    void setRasterBackend(std::unique_ptr<GSRasterBackend> backend);
    // GB3 Part 2: true while the backend takes the raw GIF stream
    // (paraLLEl). The EE side then routes P6/P7 native fast paths through
    // the arbiter so the backend sees every packet with its path.
    bool rawGifBackendActive() const { return m_rawGifBackend.load(std::memory_order_acquire); }
    // GE2: guest-VBlank delivery (GSvsync) and priv-register mirroring, both
    // live only while an opted-in backend (external) is installed.
    bool wantsGuestVsync() const { return m_wantsGuestVsync.load(std::memory_order_acquire); }
    bool wantsPrivMirror() const { return m_wantsPrivMirror.load(std::memory_order_acquire); }
    // GE2: one guest-VBlank boundary. Queued mode enqueues a GuestVsync
    // command (stream-ordered after the CSR FIELD PrivWrite); direct mode
    // calls the backend now. No-op unless the backend opted in.
    void noteGuestVsync(uint64_t tick);

    void processGIFPacket(const uint8_t *data, uint32_t sizeBytes);
    // GF1 H1 (PS2X_GS_HANDOFF_DIET): noteGifPath(path) when notePath, then
    // processGIFPacket, as one queued command (one enqueue instead of two).
    // Same effects and order as the two calls; direct mode makes the calls.
    // H2: queued mode takes `bytes` (moved into the command; left empty).
    void processGIFPacketWithPath(GifPathId path, bool notePath, std::vector<uint8_t> &bytes);
    // GE3 Part 2: PCSX2-timed FINISH. With m_finishTimingPcsx2, sets CSR
    // FINISH now (submitting thread, packet already stream-ordered) when the
    // packet carries an A+D FINISH write. No-op unless the knob is on.
    void noteFinishTimingPcsx2(const uint8_t *data, uint32_t sizeBytes);
    // E33: records which GIF path the packet currently being processed came
    // from, so draws kicked during processing attribute to that path. Only
    // called with stats armed; defaults to Path1 (the XGKICK-direct route).
    // GB2: queued mode enqueues the note so it keeps its stream position
    // ahead of its packet (the arbiter listener runs before the process
    // call); the worker assigns the field directly.
    void noteGifPath(GifPathId path)
    {
        if (m_worker)
        {
            GsCommand cmd;
            cmd.kind = GsCmdKind::NoteGifPath;
            cmd.pathId = static_cast<uint8_t>(path);
            m_worker->enqueue(std::move(cmd));
            return;
        }
        m_curGifPath = path;
    }
    bool processNativePackedGIFPacket(const uint8_t *data, uint32_t sizeBytes);
    void uploadImageNative(uint64_t bitbltbuf,
                           uint64_t trxpos,
                           uint64_t trxreg,
                           uint64_t trxdir,
                           const uint8_t *data,
                           uint32_t sizeBytes);
    void writeRegister(uint8_t regAddr, uint64_t value);
    // GB3: a store to the GS privileged registers (guest write32/64 to
    // 0x12000000+, HLE IMR/DispEnv/SetGsCrt writes, the VBlank FIELD
    // toggle). Queued mode enqueues `apply` so it runs on the worker in
    // program order relative to the packets before and after it (packet
    // SIGNAL/FINISH/LABEL and the HLE display regs also land there);
    // direct mode, or a call from the worker itself, runs it now.
    void privWrite(std::function<void()> apply);
    uint64_t privWriteCount() const { return m_privWriteCount.load(std::memory_order_relaxed); }

    const uint8_t *lockDisplaySnapshot(uint32_t &outSize);
    void unlockDisplaySnapshot();
    uint32_t getLastDisplayBaseBytes() const;
    const GSFrameReg &getContextFrame(int index) const
    {
        return m_ctx[(index != 0) ? 1 : 0].frame;
    }
    GSDebugSnapshot getDebugSnapshot() const;
    std::vector<GSDebugHistoryEntry> getDebugHistory() const;
    void clearDebugHistory();
    bool isDebugHistoryPaused() const;
    void setDebugHistoryPaused(bool paused);
    bool getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const;
    void latchHostPresentationFrame();
    // GB3 diagnostics (VQ gate): run the backend's Present for the current
    // state into a returned frame without touching the host latch. Queued
    // mode runs it on the worker at stream position (an RPC). Pixels use
    // the backend's row stride (640 * 4 bytes for both backends).
    PresentationFrame presentForDiagnostics();
    bool copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp = nullptr,
                                          uint32_t *outSourceFbp = nullptr,
                                          bool *outUsedPreferred = nullptr) const;
    bool clearFramebufferContext(uint32_t contextIndex, uint32_t rgba);
    bool clearActiveFramebuffer(uint32_t rgba);
    uint64_t nativeImageUploadCount() const { return m_nativeImageUploadCount; }
    uint64_t nativePackedGIFPacketCount() const { return m_nativePackedGIFPacketCount; }

    uint32_t consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes);

    // RB2: lagged local->host serving (`PS2X_VIF1_REVERSE_DMA=lag1` or `lagV`).
    // The sync path (RB1) drains the unit + worker at every probe readback;
    // the lag paths instead snapshot each TRXDIR=1 transfer worker-ordered at
    // setup time (snapshotLaggedReadback, called from the TRXDIR=1 case) and
    // serve each reverse DMA from an OLDER snapshot, with no worker or unit
    // sync on the EE. Deterministic given deterministic rendering: the k-th
    // serve always yields snapshot k-D's bytes (D = lag depth; it spins for an
    // already-complete snapshot, and only the spin count is timing-dependent).
    // The first D serves yield 0 bytes (EE untouched, QWC remainder, like a
    // short sync transfer). Snapshots ride the GIF stream order, so the capture
    // stays ordered after the probe's own packet on MTVU builds too (a
    // worker-direct capture command would race it). Slot cap: transfers over
    // kRb2LagSlotBytes are drained and served as 0 bytes (truncated; logged).
    // Depth: lag1 serves k-1 (D=1); lagV serves k-kRb2LagVDepth. Part 2 showed
    // D=1 leaves only ~200 us of worker slack under the ~6-probe/frame bursts
    // (max 17 probes/vsync observed on Odin SP1), so the EE spin-waits on the
    // Adreno worker; lagV's depth (24 > 17) restores >1 frame of slack.
    static constexpr uint32_t kRb2LagSlotBytes = 4096u;
    static constexpr uint64_t kRb2LagVDepth = 24u;
    static constexpr uint64_t kRb2LagRing = kRb2LagVDepth + 1u;
    // Serves the snapshot D back into dst (<= maxBytes). Returns bytes
    // available (0 = none yet / truncated / no snapshot); spinUs is the
    // snapshot wait, timedOut sets on the hang-guard timeout (serve zeros).
    uint32_t serveLaggedReadback(uint8_t *dst, uint32_t maxBytes, uint64_t &spinUs, bool &timedOut);

    // LT1b: lagF (`PS2X_VIF1_REVERSE_DMA=lagF`, mode 4), the frame-keyed lag.
    // Each TRXDIR=1 set gets a ticket j (its GS-stream sequence number) and an
    // asynchronous backend request (GE1: GPU copy at the set's stream point, no
    // fence wait); the GS worker resolves tickets at a fixed guest VSync (sets
    // made before VSync g-1 resolve at VSync g: one frame of GPU slack,
    // independent of L) into the slot table. The EE
    // serves probe (frame f, ordinal i) with the bytes of probe (f-L, i): the EE
    // records, per guest frame, the serve index of its first probe, and serve k
    // pairs with set k (one TRXDIR=1 per reverse DMA, the same pairing RB2
    // uses), so ticket = start(f-L) + i. Frames are EE vsync ticks; the key never
    // depends on where the worker saw a VSync. No probe (f-L, i): serve 0 bytes
    // (today's stale behaviour, the light stays visible), counted. L =
    // PS2X_RB_LAGF_FRAMES (2..4, default 4: about 3 in 4 probe packets reach
    // the worker after the next VSync, so L=3 left the EE spinning on the
    // resolve; LT1b Part 2). Deterministic per platform: only
    // the spin is timing-dependent. Backends without the async path (CPU) read
    // synchronously at the set instead. PS2X_RB_LAGF_VERIFY=1 (diag) also runs
    // the sync consume at each set and compares it with the async bytes at
    // resolve. Savestates: tickets in flight are not serialized; after a load,
    // probes serve 0 bytes until L frames have passed (the EE frame table is
    // keyed by tick and cleared when the tick moves backwards).
    struct LagfServeInfo
    {
        uint64_t ticket = 0;
        uint64_t srcTick = 0;
        uint32_t ordinal = 0;
        bool miss = false;
        bool timedOut = false;
        uint64_t spinUs = 0;
    };
    static constexpr uint64_t kLagfRing = 512u;
    uint32_t serveLagFrameReadback(uint8_t *dst, uint32_t maxBytes, uint64_t eeTick, LagfServeInfo &info);

    void refreshDisplaySnapshot();

    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;

private:
    void executeQueuedCommand(GsCommand &cmd);
    // PKB1 item 3: bulk-return the stashed buffers (single pool lock round).
    void flushReleaseStash();
    void noteConsumedCommand(const GsCommand &cmd);
    void snapshotVRAM();
    void writeRegisterUnlocked(uint8_t regAddr, uint64_t value);
    void writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi);
    void uploadImageNativeUnlocked(uint64_t bitbltbuf,
                                   uint64_t trxpos,
                                   uint64_t trxreg,
                                   uint64_t trxdir,
                                   const uint8_t *data,
                                   uint32_t sizeBytes);
    void vertexKick(bool drawing);

    void recordDebugEventUnlocked(GSDebugHistoryEntry entry);
    GSDebugHistoryEntry makeDebugEventUnlocked(GSDebugEventKind kind) const;
    void recordGifTagDebugEventUnlocked(uint32_t sizeBytes, uint32_t nloop, uint8_t flg, uint32_t nreg);
    void recordRegisterDebugEventUnlocked(uint8_t regAddr, uint64_t value);
    void recordDrawDebugEventUnlocked(int vertexCount);
    void recordTransferDebugEventUnlocked();
    void recordPresentDebugEventUnlocked(uint32_t displayFbp, uint32_t sourceFbp, uint32_t width, uint32_t height, bool usedPreferred);

    void processImageData(const uint8_t *data, uint32_t sizeBytes);
    bool tryProcessNativeImageUploadPacket(const uint8_t *data, uint32_t sizeBytes);
    GSPrimitiveBatch buildDrawBatch(int vertexCount) const;
    void updatePreferredDisplaySourceForDraw(const GSPrimitiveBatch &batch);
    GSPresentationRequest buildPresentationRequestUnlocked() const;


    GSContext &activeContext();
    friend struct GSSavestate;
    friend struct GsLagfTestAccess; // QSR1 SRB4 unit test only

    uint8_t *m_localMemoryStorage = nullptr;
    uint32_t m_localMemorySize = 0u;
    struct GSRegisters *m_privRegs = nullptr;
    mutable std::recursive_mutex m_stateMutex;
    mutable std::mutex m_backendLifetimeMutex;
    mutable std::mutex m_presentationMutex;

    GSContext m_ctx[2];
    GSPrimReg m_prim{};
    GifPathId m_curGifPath = GifPathId::Path1;
    GSPrimReg m_primRegister{};
    GSPrimReg m_prmodeRegister{};

    uint8_t m_curR = 0x80, m_curG = 0x80, m_curB = 0x80, m_curA = 0x80;
    float m_curQ = 1.0f;
    float m_curS = 0.0f, m_curT = 0.0f;
    uint16_t m_curU = 0, m_curV = 0;
    uint8_t m_curFog = 0;
    uint8_t m_fogR = 0, m_fogG = 0, m_fogB = 0;

    bool m_prmodecont = true;
    bool m_pabe = false;
    uint64_t m_scanmsk = 0;
    uint64_t m_dimx = 0;
    uint64_t m_dthe = 0;
    uint64_t m_colclamp = 0;
    GSTexaReg m_texa{0u, false, 0u};
    GSTexClutReg m_texclut{0u, 0u, 0u};

    GSBitBltBuf m_bitbltbuf{};
    GSTrxPos m_trxpos{};
    GSTrxReg m_trxreg{};
    uint32_t m_trxdir = 3;


    static constexpr int kMaxVerts = 6;
    GSVertex m_vtxQueue[kMaxVerts];
    int m_vtxCount = 0;
    int m_vtxIndex = 0;

    std::vector<uint8_t> m_displaySnapshot;
    std::mutex m_snapshotMutex;
    uint32_t m_lastDisplayBaseBytes = 0;
    GSFrameReg m_preferredDisplaySourceFrame{};
    uint32_t m_preferredDisplayDestFbp = 0;
    bool m_hasPreferredDisplaySource = false;
    std::vector<uint8_t> m_hostPresentationFrame;
    uint32_t m_hostPresentationWidth = 0;
    uint32_t m_hostPresentationHeight = 0;
    uint32_t m_hostPresentationDisplayFbp = 0;
    uint32_t m_hostPresentationSourceFbp = 0;
    bool m_hostPresentationUsedPreferred = false;
    bool m_hasHostPresentationFrame = false;
    uint64_t m_nativeImageUploadCount = 0;
    uint64_t m_nativePackedGIFPacketCount = 0;

    static constexpr size_t kDebugHistoryCapacity = 512;
    std::array<GSDebugHistoryEntry, kDebugHistoryCapacity> m_debugHistory{};
    size_t m_debugHistoryWrite = 0;
    size_t m_debugHistoryCount = 0;
    uint64_t m_debugNextSeq = 1;
    uint32_t m_debugFrameIndex = 0;
    uint64_t m_debugLastVsyncTick = UINT64_MAX;
    bool m_debugHistoryPaused = true;

    std::unique_ptr<GSRasterBackend> m_backend;
    // GB2: null unless setQueueEnabled(true). Published before any other
    // thread touches this GS; cleared only by setQueueEnabled(false) or
    // the destructor, after producer threads are joined.
    std::unique_ptr<GsWorker> m_worker;
    // GP4 H5: reusable packet buffers (producers acquire, the worker
    // releases after execute). Thread-safe; inert unless enabled.
    GsPacketPool m_packetPool;
    // PKB1 item 3: stash of consumed payload buffers awaiting a bulk
    // release. Touched on the GS worker thread only (executeQueuedCommand
    // runs there in queued mode); flushed under one lock round every
    // kPopBatch buffers and whenever the queue stops.
    std::vector<std::vector<uint8_t>> m_releaseStash;
    bool m_releaseBatching = false;
    // GB2 Part 2: monotonic submit counters (atomic: incremented on the
    // worker when queued, read on the game thread after a drain).
    std::atomic<uint64_t> m_submitCount{0};
    std::atomic<uint64_t> m_regWriteCount{0};
    std::atomic<uint64_t> m_privWriteCount{0};
    std::atomic<bool> m_rawGifBackend{false};
    std::atomic<bool> m_minimalGifDecode{false};
    // GE3 Part 2: PS2X_GS_FINISH_TIMING=pcsx2 sets CSR FINISH on the
    // submitting thread at GIF arbitration (PCSX2 Gif_Unit parity) instead of
    // after GsWorker decode. Default off. Set once in the constructor before
    // the game thread spawns; read on EE/MTVU submit threads.
    bool m_finishTimingPcsx2 = false;
    std::atomic<bool> m_wantsGuestVsync{false}; // GE2: cached WantsGuestVsync()
    std::atomic<bool> m_wantsPrivMirror{false}; // GE2: cached WantsPrivMirror()
    // N8D7M12 Part 5F4P2: worker-consumption packet-sequence fingerprint.
    // Running digest starts at the FNV-64 offset; snapshot is 0
    // while disabled. N8D7M12 Part 5F4P3: m_pktSeqEnabled is atomic so
    // the default-off per-command path takes only a relaxed fast check
    // and never acquires m_pktSeqMutex while disabled. When the flag
    // reads true the worker locks and re-checks under the mutex (toggle
    // race), then updates digest/count exactly as before. The setter
    // locks for race-safety; the replay sets the flag before worker
    // start (happens-before). Digest/snapshot/commands stay guarded
    // by m_pktSeqMutex.
    mutable std::mutex m_pktSeqMutex;
    std::atomic<bool> m_pktSeqEnabled{false};
    uint64_t m_pktSeqDigest = 14695981039346656037ull;
    uint64_t m_pktSeqCommands = 0u;
    uint64_t m_pktSeqSnapshot = 0u;
    uint64_t m_pktSeqSnapshotCommands = 0u;

    // RB2 (appended last): lagged readback cache (ring of kRb2LagRing slots;
    // snapshot j writes slot j%R). Snapshots publish under m_rb2Mutex with a
    // release store of m_rb2Snaps; serves spin on an acquire load, then copy
    // under the mutex. No aliasing: serve k reads snapshot k-D's slot, and the
    // next writer of that slot (snapshot k-D+R) cannot execute before probe
    // k-D+R enqueues its TRXDIR, which the guest orders after serve k (R>D).
    // m_rb2Serves is EE-thread-only but stays mutex-guarded. Hang-guard, not
    // behavior: the serve spin times out (serve zeros) if a snapshot never
    // completes. Depth is read live from the knob; don't flip modes mid-run.
    void snapshotLaggedReadback();
    std::mutex m_rb2Mutex;
    std::atomic<uint64_t> m_rb2Snaps{0};
    uint8_t m_rb2Slot[kRb2LagRing][kRb2LagSlotBytes]{};
    uint32_t m_rb2SlotBytes[kRb2LagRing] = {0u};
    bool m_rb2SlotTruncated[kRb2LagRing] = {false};
    uint64_t m_rb2Serves = 0u;
    std::atomic<uint64_t> m_rb2Timeouts{0};

    // LT1b (appended last): lagF state. Slots are published under m_lagfMutex
    // with a release store of `ready` (= the ticket they hold); the EE spins on
    // an acquire load, then copies under the mutex. Worker-only: m_lagfSets,
    // the GS frame/ordinal and resolve cursor, the VSync set marks, verify
    // bytes. EE-only: the frame table and serve counter.
    struct LagfSlot
    {
        std::atomic<uint64_t> ready{~0ull};
        bool async = false;
        uint32_t bytes = 0u;
        uint64_t gsTick = 0u;
        uint32_t gsOrdinal = 0u;
        uint8_t data[kRb2LagSlotBytes];
    };
    struct LagfEeFrame
    {
        uint64_t tick = ~0ull;
        uint64_t start = 0u;
        uint32_t count = 0u;
    };
    void lagfRequest(const GSTransferCommand &command);
    void lagfOnGuestVsync(uint64_t tick);
    LagfSlot *lagfSlots();
    std::mutex m_lagfMutex;
    std::unique_ptr<LagfSlot[]> m_lagfSlots;
    std::vector<std::vector<uint8_t>> m_lagfVerifyBytes;
    uint64_t m_lagfSets = 0u;
    uint64_t m_lagfGsTick = 0u;
    uint32_t m_lagfGsOrdinal = 0u;
    uint64_t m_lagfResolved = 0u;
    std::array<std::pair<uint64_t, uint64_t>, 8> m_lagfVsyncSets{};
    bool m_lagfVsyncSetsInit = false;
    std::array<LagfEeFrame, 8> m_lagfEe{};
    uint64_t m_lagfServes = 0u;
    uint64_t m_lagfLastEeTick = 0u;
    // Counters ([lagF] sum lines): async requests, sync fallbacks, served,
    // misses, timeouts, ordinal mismatches, frame-delta histogram (-2..+2, index
    // 0..4; outside counted at 5), spin log2 histogram (us), verify compared /
    // mismatched / unresolved.
    std::atomic<uint64_t> m_lagfAsync{0}, m_lagfFallback{0}, m_lagfServed{0}, m_lagfMisses{0},
        m_lagfTimeouts{0}, m_lagfOrdMismatch{0}, m_lagfVerifyCompared{0}, m_lagfVerifyMismatch{0},
        m_lagfUnresolved{0}, m_lagfSpinMaxUs{0};
    std::array<std::atomic<uint64_t>, 6> m_lagfFrameDelta{};
    std::array<std::atomic<uint64_t>, 24> m_lagfSpinHist{};
};

#endif
