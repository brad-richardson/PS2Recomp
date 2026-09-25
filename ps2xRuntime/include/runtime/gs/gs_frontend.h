#ifndef PS2_GS_FRONTEND_H
#define PS2_GS_FRONTEND_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
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
    bool setQueueEnabled(bool enabled);
    bool queueEnabled() const { return m_worker != nullptr; }
    // NP1: coalesce worker wakeups to one per batch (direct mode: no-op).
    void beginWorkerBatch() { if (m_worker) m_worker->beginBatch(); }
    void endWorkerBatch() { if (m_worker) m_worker->endBatch(); }
    // Blocks until all previously enqueued commands have executed.
    void drainQueue();
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

    void processGIFPacket(const uint8_t *data, uint32_t sizeBytes);
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

    void refreshDisplaySnapshot();

    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;

private:
    void executeQueuedCommand(GsCommand &cmd);
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
    // GB2 Part 2: monotonic submit counters (atomic: incremented on the
    // worker when queued, read on the game thread after a drain).
    std::atomic<uint64_t> m_submitCount{0};
    std::atomic<uint64_t> m_regWriteCount{0};
    std::atomic<uint64_t> m_privWriteCount{0};
    std::atomic<bool> m_rawGifBackend{false};
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
};

#endif
