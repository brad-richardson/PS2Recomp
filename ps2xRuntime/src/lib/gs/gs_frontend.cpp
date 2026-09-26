#include "runtime/gs/gs_frontend.h"
#include "ps2_fpmode.h"
#include "runtime/gs/ps2_gs_shadow.h"
#include "ps2_e7.h"
#include "ps2_gfx_stats.h"
#include "ps2_mtvu.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_stream_capture.h"
#include "ps2_e4.h"
#include "ps2_log.h"
#include "ps2_park_snapshot.h"
#include "runtime/ps2_memory.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

namespace
{
    static constexpr uint32_t kHostFrameWidth = 640u;

    GSPrimReg decodePrimRegister(uint64_t value)
    {
        GSPrimReg prim{};
        prim.type = static_cast<GSPrimType>(value & 0x7u);
        prim.iip = ((value >> 3) & 1u) != 0u;
        prim.tme = ((value >> 4) & 1u) != 0u;
        prim.fge = ((value >> 5) & 1u) != 0u;
        prim.abe = ((value >> 6) & 1u) != 0u;
        prim.aa1 = ((value >> 7) & 1u) != 0u;
        prim.fst = ((value >> 8) & 1u) != 0u;
        prim.ctxt = ((value >> 9) & 1u) != 0u;
        prim.fix = ((value >> 10) & 1u) != 0u;
        return prim;
    }

    static inline uint64_t loadLE64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }

    // N8D7M12 Part 5F4P2: 64-bit FNV-1a helpers with explicit LE field
    // encoding. Variable-length payloads are length-prefixed (u32 LE)
    // so concatenations cannot alias.
    static constexpr uint64_t kPktSeqOffset = 14695981039346656037ull;
    static constexpr uint64_t kPktSeqPrime = 1099511628211ull;

    static inline void pktSeqMixByte(uint64_t &digest, uint8_t b)
    {
        digest ^= static_cast<uint64_t>(b);
        digest *= kPktSeqPrime;
    }

    static inline void pktSeqMixU32(uint64_t &digest, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            pktSeqMixByte(digest, static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
    }

    static inline void pktSeqMixU64(uint64_t &digest, uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            pktSeqMixByte(digest, static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
    }

    static inline void pktSeqMixBytes(uint64_t &digest, const uint8_t *data, size_t size)
    {
        pktSeqMixU32(digest, static_cast<uint32_t>(size));
        for (size_t i = 0; i < size; ++i)
            pktSeqMixByte(digest, data[i]);
    }

    struct PackedGifPacketTag
    {
        uint64_t lo = 0u;
        uint64_t hi = 0u;
        uint32_t payloadOffset = 0u;
        uint32_t nloop = 0u;
        uint32_t nreg = 0u;
        uint8_t regs[16]{};
    };

    template <typename Visitor>
    bool visitPackedGifPacket(const uint8_t *data, uint32_t sizeBytes, Visitor &&visitor)
    {
        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            PackedGifPacketTag tag{};
            tag.lo = loadLE64(data + offset);
            tag.hi = loadLE64(data + offset + 8u);

            const uint8_t flg = static_cast<uint8_t>((tag.lo >> 58u) & 0x3u);
            if (flg != GIF_FMT_PACKED)
                return false;

            tag.nloop = static_cast<uint32_t>(tag.lo & 0x7FFFu);
            tag.nreg = static_cast<uint32_t>((tag.lo >> 60u) & 0xFu);
            if (tag.nreg == 0u)
                tag.nreg = 16u;

            const uint64_t payloadBytes64 =
                static_cast<uint64_t>(tag.nloop) * static_cast<uint64_t>(tag.nreg) * 16ull;
            if (payloadBytes64 > 0xFFFFFFFFull)
                return false;

            offset += 16u;
            const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);
            if (payloadBytes > sizeBytes - offset)
                return false;

            tag.payloadOffset = offset;
            for (uint32_t i = 0u; i < tag.nreg; ++i)
                tag.regs[i] = static_cast<uint8_t>((tag.hi >> (i * 4u)) & 0xFu);

            if (!visitor(tag))
                return false;

            offset += payloadBytes;
        }

        return offset == sizeBytes;
    }

    bool validatePackedGifPacket(const uint8_t *data, uint32_t sizeBytes)
    {
        return visitPackedGifPacket(data, sizeBytes, [](const PackedGifPacketTag &)
                                    { return true; });
    }

    std::atomic<uint32_t> s_debugGifPacketCount{0};
    std::atomic<uint32_t> s_debugGsRegisterCount{0};
    std::atomic<uint32_t> s_debugGsPackedVertexCount{0};
    std::atomic<uint32_t> s_debugGsVertexKickCount{0};
    std::atomic<uint32_t> s_debugCopyRegCount{0};
    std::atomic<uint32_t> s_debugTexaWriteCount{0};
    std::atomic<uint32_t> s_debugCvFontUploadCount{0};
    std::atomic<uint32_t> s_debugLocalCopyCount{0};

    struct GsPacketVramTrace
    {
        uint64_t tick;
        uint64_t index;
        uint8_t path;
        const uint8_t *vram;
        uint32_t vramSize;

        ~GsPacketVramTrace()
        {
            ps2x_gs_capture::packetDone(tick, index, path, vram, vramSize);
        }
    };
}

// GB2: true while the calling thread is this GS's worker executing a
// queued command. Public methods route to the queue only when a worker
// exists AND the caller is not the worker itself, so the worker runs the
// exact direct-call bodies without re-enqueueing.
namespace
{
    thread_local bool t_inGsWorker = false;

    struct GsWorkerScope
    {
        GsWorkerScope() { t_inGsWorker = true; }
        ~GsWorkerScope() { t_inGsWorker = false; }
    };

    // GF1: dev-only live comparator of what the GS worker consumes
    // (PS2X_GS_CONSUMED_LOG=1): FNV-1a over (kind, path, bytes) of every GIF
    // packet in execution order, printed every 65536 packets. Unlike the [pk]
    // log (unit-side submits) it sees the queue's output, so a transport
    // change must leave it identical. Worker thread only; off = one branch.
    struct ConsumedLog
    {
        bool on = false;
        uint64_t n = 0;
        uint64_t h = 1469598103934665603ull;
    };

    ConsumedLog &consumedLog()
    {
        static ConsumedLog s{[]
                             {
                                 const char *e = std::getenv("PS2X_GS_CONSUMED_LOG");
                                 return e != nullptr && e[0] == '1';
                             }()};
        return s;
    }

    void noteConsumedGif(uint8_t kind, uint8_t path, const uint8_t *data, size_t size)
    {
        ConsumedLog &s = consumedLog();
        auto mix = [&](uint8_t b)
        {
            s.h ^= b;
            s.h *= 1099511628211ull;
        };
        mix(kind);
        mix(path);
        for (size_t i = 0; i < size; ++i)
            mix(data[i]);
        if ((++s.n & 0xFFFFu) == 0u)
            std::fprintf(stderr, "[gs:consumed] n=%llu h=%016llx\n", static_cast<unsigned long long>(s.n),
                         static_cast<unsigned long long>(s.h));
    }

    struct QueuedPreferredSource
    {
        bool has = false;
        GSFrameReg source{};
        uint32_t destFbp = 0;
    };
}


GS::GS()
    : m_backend(std::make_unique<GSCpuBackend>())
{
    reset();
}

GS::~GS() = default;

bool GS::setQueueEnabled(bool enabled)
{
    if (enabled && !m_worker)
    {
        auto worker = std::make_unique<GsWorker>(GsWorker::kDefaultMaxDescriptors,
                                                 GsWorker::kDefaultMaxPayloadBytes,
                                                 [this](GsCommand &cmd)
                                                 { executeQueuedCommand(cmd); });
        worker->start();
        m_worker = std::move(worker);
    }
    else if (!enabled && m_worker)
    {
        m_worker->stop();
        m_worker.reset();
    }
    return true;
}

void GS::drainQueue()
{
    ps2_mtvu::touch(ps2_mtvu::Site::GsDrain); // MT1: unit-owned
    if (!m_worker || t_inGsWorker)
        return;
    if (m_worker->isQuiescent())
    {
        // N8D7M12 Part 5F4P2: no Fence is enqueued here, so copy the
        // running digest into the snapshot directly. Queue empty AND
        // nothing executing (single replay producer) makes this stable.
        // N8D7M12 Part 5F4P3: relaxed fast check first so default-off
        // drains never acquire the mutex; re-check under lock for races.
        if (!m_pktSeqEnabled.load(std::memory_order_relaxed))
            return; // provable no-op: nothing queued, nothing executing
        std::lock_guard<std::mutex> lock(m_pktSeqMutex);
        if (!m_pktSeqEnabled.load(std::memory_order_relaxed))
            return;
        m_pktSeqSnapshot = m_pktSeqDigest;
        m_pktSeqSnapshotCommands = m_pktSeqCommands;
        return; // provable no-op: nothing queued, nothing executing
    }
    GsCommand cmd;
    cmd.kind = GsCmdKind::Fence;
    cmd.rpc = std::make_shared<GsRpcBase>();
    std::shared_ptr<GsRpcBase> rpc = cmd.rpc;
    m_worker->enqueue(std::move(cmd));
    rpc->wait();
}

void GS::setPktSeqEnabled(bool enabled)
{
    // N8D7M12 Part 5F4P3: locked for race-safety; the relaxed store
    // publishes the flag for the per-command fast check.
    std::lock_guard<std::mutex> lock(m_pktSeqMutex);
    m_pktSeqEnabled.store(enabled, std::memory_order_relaxed);
    if (enabled)
    {
        m_pktSeqDigest = kPktSeqOffset;
        m_pktSeqCommands = 0u;
        m_pktSeqSnapshot = kPktSeqOffset;
        m_pktSeqSnapshotCommands = 0u;
    }
    else
    {
        m_pktSeqSnapshot = 0u;
        m_pktSeqSnapshotCommands = 0u;
    }
}

bool GS::pktSeqEnabled() const
{
    // N8D7M12 Part 5F4P3: race-safe relaxed load, no mutex needed.
    return m_pktSeqEnabled.load(std::memory_order_relaxed);
}

uint64_t GS::pktSeqSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_pktSeqMutex);
    return m_pktSeqSnapshot;
}

uint64_t GS::pktSeqSnapshotCommands() const
{
    std::lock_guard<std::mutex> lock(m_pktSeqMutex);
    return m_pktSeqSnapshotCommands;
}

void GS::noteConsumedCommand(const GsCommand &cmd)
{
    // N8D7M12 Part 5F4P3: default-off fast path. Relaxed load before
    // the mutex, so OFF never acquires m_pktSeqMutex per command.
    // Guarded re-check under lock covers a concurrent toggle; ON then
    // updates digest/count exactly as before.
    if (!m_pktSeqEnabled.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(m_pktSeqMutex);
    if (!m_pktSeqEnabled.load(std::memory_order_relaxed))
        return;
    if (cmd.kind == GsCmdKind::Fence)
    {
        // Timing-dependent presence: snapshot only, never hashed/counted.
        m_pktSeqSnapshot = m_pktSeqDigest;
        m_pktSeqSnapshotCommands = m_pktSeqCommands;
        return;
    }
    uint64_t &d = m_pktSeqDigest;
    pktSeqMixByte(d, static_cast<uint8_t>(cmd.kind));
    switch (cmd.kind)
    {
    case GsCmdKind::GifPacket:
        // Consumed path comes from prior NoteGifPath consumption (FIFO),
        // so read the live field, not the packet itself.
        pktSeqMixByte(d, static_cast<uint8_t>(m_curGifPath));
        pktSeqMixBytes(d, cmd.bytes.data(), cmd.bytes.size());
        break;
    case GsCmdKind::NoteGifPath:
        pktSeqMixByte(d, cmd.pathId);
        break;
    case GsCmdKind::RegWrite:
        pktSeqMixByte(d, cmd.regAddr);
        pktSeqMixU64(d, cmd.regValue);
        break;
    case GsCmdKind::UploadImageNative:
        pktSeqMixU64(d, cmd.setupRegs[0]);
        pktSeqMixU64(d, cmd.setupRegs[1]);
        pktSeqMixU64(d, cmd.setupRegs[2]);
        pktSeqMixU64(d, cmd.setupRegs[3]);
        pktSeqMixBytes(d, cmd.bytes.data(), cmd.bytes.size());
        break;
    case GsCmdKind::NativePacked:
        pktSeqMixBytes(d, cmd.bytes.data(), cmd.bytes.size());
        break;
    case GsCmdKind::ClearCtx:
        pktSeqMixU32(d, cmd.u32a);
        pktSeqMixU32(d, cmd.u32b);
        break;
    case GsCmdKind::ClearActive:
        pktSeqMixU32(d, cmd.u32a);
        break;
    case GsCmdKind::WriteVram:
        pktSeqMixU32(d, cmd.u32a);
        pktSeqMixU32(d, cmd.u32b);
        pktSeqMixU32(d, cmd.u32c);
        pktSeqMixU32(d, cmd.u32d);
        pktSeqMixU32(d, cmd.u32e);
        pktSeqMixU32(d, static_cast<uint32_t>(cmd.regValue));
        break;
    case GsCmdKind::PrivWrite:
        // Opaque callable: kind tag only (content gap, declared).
        break;
    default:
        // Side RPCs (Consume/ReadVram/RefreshSnapshot/DiagPresent/…)
        // carry no packet bytes: kind tag only, still counted so the
        // count stays run-deterministic under the fixed replay script.
        break;
    }
    ++m_pktSeqCommands;
}

void GS::executeQueuedCommand(GsCommand &cmd)
{
    // N8D7M12 Part 5F4P2: hash before the handler runs so a GifPacket
    // sees the prior-consumed m_curGifPath value. N8D7M12 Part 5F4P3:
    // noteConsumedCommand fast-returns while disabled without locking.
    if (cmd.kind == GsCmdKind::GifPacket && (cmd.u32a & kGsGifPacketHasPath) != 0u)
    {
        // GF1 H1: the folded NoteGifPath runs first, digest included, so the
        // consumed sequence is the one the two separate commands produce.
        if (m_pktSeqEnabled.load(std::memory_order_relaxed))
        {
            GsCommand note;
            note.kind = GsCmdKind::NoteGifPath;
            note.pathId = cmd.pathId;
            noteConsumedCommand(note);
        }
        m_curGifPath = static_cast<GifPathId>(cmd.pathId);
    }
    noteConsumedCommand(cmd);
    const GsWorkerScope scope;
    switch (cmd.kind)
    {
    case GsCmdKind::GifPacket:
        if (consumedLog().on)
            noteConsumedGif(static_cast<uint8_t>(cmd.kind), static_cast<uint8_t>(m_curGifPath), cmd.bytes.data(),
                            cmd.bytes.size());
        processGIFPacket(cmd.bytes.data(), static_cast<uint32_t>(cmd.bytes.size()));
        break;
    case GsCmdKind::NoteGifPath:
        m_curGifPath = static_cast<GifPathId>(cmd.pathId);
        break;
    case GsCmdKind::RegWrite:
        writeRegister(cmd.regAddr, cmd.regValue);
        break;
    case GsCmdKind::UploadImageNative:
        uploadImageNative(cmd.setupRegs[0], cmd.setupRegs[1], cmd.setupRegs[2], cmd.setupRegs[3],
                          cmd.bytes.data(), static_cast<uint32_t>(cmd.bytes.size()));
        break;
    case GsCmdKind::NativePacked:
        if (consumedLog().on)
            noteConsumedGif(static_cast<uint8_t>(cmd.kind), static_cast<uint8_t>(m_curGifPath), cmd.bytes.data(),
                            cmd.bytes.size());
        processNativePackedGIFPacket(cmd.bytes.data(), static_cast<uint32_t>(cmd.bytes.size()));
        break;
    case GsCmdKind::ClearCtx:
        std::static_pointer_cast<GsRpc<bool>>(cmd.rpc)->result =
            clearFramebufferContext(cmd.u32a, cmd.u32b);
        break;
    case GsCmdKind::ClearActive:
        std::static_pointer_cast<GsRpc<bool>>(cmd.rpc)->result = clearActiveFramebuffer(cmd.u32a);
        break;
    case GsCmdKind::WriteVram:
        WriteVram(cmd.u32a, cmd.u32b, cmd.u32c, cmd.u32d, cmd.u32e,
                  static_cast<uint32_t>(cmd.regValue));
        break;
    case GsCmdKind::ClearDebugHistory:
        clearDebugHistory();
        break;
    case GsCmdKind::SetDebugPaused:
        setDebugHistoryPaused(cmd.u32a != 0u);
        break;
    case GsCmdKind::PrivWrite:
        privWrite(std::move(cmd.apply));
        break;
    case GsCmdKind::Consume:
    {
        auto rpc = std::static_pointer_cast<GsRpc<std::vector<uint8_t>>>(cmd.rpc);
        rpc->result.resize(cmd.u32a);
        const uint32_t n = consumeLocalToHostBytes(rpc->result.data(), cmd.u32a);
        rpc->result.resize(n);
        break;
    }
    case GsCmdKind::ReadVram:
        std::static_pointer_cast<GsRpc<uint32_t>>(cmd.rpc)->result =
            ReadVram(cmd.u32a, cmd.u32b, cmd.u32c, cmd.u32d, cmd.u32e);
        break;
    case GsCmdKind::RefreshSnapshot:
        refreshDisplaySnapshot();
        break;
    case GsCmdKind::LatchPresent:
        latchHostPresentationFrame();
        break;
    case GsCmdKind::Reset:
        reset();
        break;
    case GsCmdKind::GetDebugSnapshot:
        std::static_pointer_cast<GsRpc<GSDebugSnapshot>>(cmd.rpc)->result = getDebugSnapshot();
        break;
    case GsCmdKind::GetDebugHistory:
        std::static_pointer_cast<GsRpc<std::vector<GSDebugHistoryEntry>>>(cmd.rpc)->result =
            getDebugHistory();
        break;
    case GsCmdKind::IsDebugPaused:
        std::static_pointer_cast<GsRpc<bool>>(cmd.rpc)->result = isDebugHistoryPaused();
        break;
    case GsCmdKind::GetPreferredSource:
    {
        auto rpc = std::static_pointer_cast<GsRpc<QueuedPreferredSource>>(cmd.rpc);
        rpc->result.has = getPreferredDisplaySource(rpc->result.source, rpc->result.destFbp);
        break;
    }
    case GsCmdKind::SetBackend:
        setRasterBackend(std::move(cmd.backend));
        break;
    case GsCmdKind::Fence:
        break;
    case GsCmdKind::DiagPresent:
        std::static_pointer_cast<GsRpc<PresentationFrame>>(cmd.rpc)->result = presentForDiagnostics();
        break;
    }
}

void GS::init(uint8_t *vram, uint32_t vramSize, GSRegisters *privRegs)
{
    m_localMemoryStorage = vram;
    m_localMemorySize = vramSize;
    m_privRegs = privRegs;
    if (!m_backend)
        m_backend = std::make_unique<GSCpuBackend>();
    m_backend->Initialize(vram, vramSize);
    reset();
}

void GS::reset()
{
    ps2_mtvu::touch(ps2_mtvu::Site::GsReset); // MT1: unit-owned
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::Reset;
        cmd.rpc = std::make_shared<GsRpcBase>();
        std::shared_ptr<GsRpcBase> rpc = cmd.rpc;
        m_worker->enqueue(std::move(cmd));
        rpc->wait();
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    ps2x_gs_shadow::onReset(); // G44: backend re-inits on next feed
    std::memset(m_ctx, 0, sizeof(m_ctx));
    m_prim = {};
    m_primRegister = {};
    m_prmodeRegister = {};
    m_curR = 0x80;
    m_curG = 0x80;
    m_curB = 0x80;
    m_curA = 0x80;
    m_curQ = 1.0f;
    m_curS = 0.0f;
    m_curT = 0.0f;
    m_curU = 0;
    m_curV = 0;
    m_curFog = 0;
    m_fogR = 0;
    m_fogG = 0;
    m_fogB = 0;
    m_prmodecont = true;
    m_pabe = false;
    m_scanmsk = 0u;
    m_dimx = 0u;
    m_dthe = 0u;
    m_colclamp = 0u;
    m_texa = {0u, false, 0u};
    m_texclut = {0u, 0u, 0u};
    m_bitbltbuf = {};
    m_trxpos = {};
    m_trxreg = {};
    m_trxdir = 3;
    m_vtxCount = 0;
    m_vtxIndex = 0;
    m_curGifPath = GifPathId::Path1;
    m_preferredDisplaySourceFrame = {};
    m_preferredDisplayDestFbp = 0;
    m_hasPreferredDisplaySource = false;
    if (m_backend)
    {
        m_backend->Flush();
        m_backend->Sync(GSSyncReason::Reset);
        m_backend->Reset();
    }
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
    }

    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;

    for (int i = 0; i < 2; ++i)
    {
        m_ctx[i].frame.fbw = 10;
        m_ctx[i].scissor = {0, 639, 0, 447};
        m_ctx[i].xyoffset = {0, 0};
    }
}

GSContext &GS::activeContext()
{
    return m_ctx[m_prim.ctxt ? 1 : 0];
}

void GS::snapshotVRAM()
{
    // Presentation/debug snapshots run outside m_stateMutex so the EE can keep
    // feeding the GS while a backend performs host-side conversion. Keep the
    // selected backend alive and unswappable for the duration of the call.
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (!m_backend)
        return;
    std::vector<uint8_t> snapshot;
    m_backend->Sync(GSSyncReason::DebugReadback);
    m_backend->SnapshotVram(snapshot);
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_displaySnapshot.swap(snapshot);
}

const uint8_t *GS::lockDisplaySnapshot(uint32_t &outSize)
{
    m_snapshotMutex.lock();
    if (m_displaySnapshot.empty())
    {
        outSize = 0;
        return nullptr;
    }

    outSize = static_cast<uint32_t>(m_displaySnapshot.size());
    return m_displaySnapshot.data();
}

GSDebugSnapshot GS::getDebugSnapshot() const
{
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::GetDebugSnapshot;
        auto rpc = std::make_shared<GsRpc<GSDebugSnapshot>>();
        cmd.rpc = rpc;
        m_worker->enqueue(std::move(cmd));
        rpc->wait();
        return rpc->result;
    }
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    GSDebugSnapshot snapshot{};
    snapshot.ctx[0] = m_ctx[0];
    snapshot.ctx[1] = m_ctx[1];
    snapshot.prim = m_prim;
    snapshot.texa = m_texa;
    snapshot.texclut = m_texclut;
    snapshot.scanmsk = m_scanmsk;
    snapshot.dimx = m_dimx;
    snapshot.dthe = m_dthe;
    snapshot.colclamp = m_colclamp;
    snapshot.bitbltbuf = m_bitbltbuf;
    snapshot.trxpos = m_trxpos;
    snapshot.trxreg = m_trxreg;
    const GSTransferSnapshot transfer = m_backend ? m_backend->GetTransferSnapshot() : GSTransferSnapshot{};
    snapshot.trxdir = transfer.direction;
    snapshot.transferX = transfer.x;
    snapshot.transferY = transfer.y;
    snapshot.transferTotalPixels = transfer.totalPixels;
    snapshot.transferCopiedPixels = transfer.copiedPixels;
    snapshot.lastDisplayBaseBytes = m_lastDisplayBaseBytes;
    snapshot.preferredDisplaySourceFrame = m_preferredDisplaySourceFrame;
    snapshot.preferredDisplayDestFbp = m_preferredDisplayDestFbp;
    snapshot.hasPreferredDisplaySource = m_hasPreferredDisplaySource;
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        snapshot.hostPresentationWidth = m_hostPresentationWidth;
        snapshot.hostPresentationHeight = m_hostPresentationHeight;
        snapshot.hostPresentationDisplayFbp = m_hostPresentationDisplayFbp;
        snapshot.hostPresentationSourceFbp = m_hostPresentationSourceFbp;
        snapshot.hostPresentationUsedPreferred = m_hostPresentationUsedPreferred;
        snapshot.hasHostPresentationFrame = m_hasHostPresentationFrame;
    }
    snapshot.localToHostPendingBytes = transfer.localToHostPendingBytes;
    return snapshot;
}

std::vector<GSDebugHistoryEntry> GS::getDebugHistory() const
{
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::GetDebugHistory;
        auto rpc = std::make_shared<GsRpc<std::vector<GSDebugHistoryEntry>>>();
        cmd.rpc = rpc;
        m_worker->enqueue(std::move(cmd));
        rpc->wait();
        return rpc->result;
    }
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    std::vector<GSDebugHistoryEntry> out;
    out.reserve(m_debugHistoryCount);
    const size_t first = (m_debugHistoryWrite + kDebugHistoryCapacity - m_debugHistoryCount) % kDebugHistoryCapacity;
    for (size_t i = 0; i < m_debugHistoryCount; ++i)
    {
        out.push_back(m_debugHistory[(first + i) % kDebugHistoryCapacity]);
    }
    return out;
}

void GS::clearDebugHistory()
{
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::ClearDebugHistory;
        m_worker->enqueue(std::move(cmd));
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;
}

bool GS::isDebugHistoryPaused() const
{
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::IsDebugPaused;
        auto rpc = std::make_shared<GsRpc<bool>>();
        cmd.rpc = rpc;
        m_worker->enqueue(std::move(cmd));
        rpc->wait();
        return rpc->result;
    }
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_debugHistoryPaused;
}

void GS::setDebugHistoryPaused(bool paused)
{
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::SetDebugPaused;
        cmd.u32a = paused ? 1u : 0u;
        m_worker->enqueue(std::move(cmd));
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryPaused = paused;
}

GSDebugHistoryEntry GS::makeDebugEventUnlocked(GSDebugEventKind kind) const
{
    GSDebugHistoryEntry entry{};
    entry.kind = kind;
    entry.prim = m_prim;
    const uint32_t ci = m_prim.ctxt ? 1u : 0u;
    entry.frame = m_ctx[ci].frame;
    entry.zbuf = m_ctx[ci].zbuf;
    entry.tex0 = m_ctx[ci].tex0;
    entry.scissor = m_ctx[ci].scissor;
    entry.test = m_ctx[ci].test;
    entry.alpha = m_ctx[ci].alpha;
    entry.bitbltbuf = m_bitbltbuf;
    entry.trxpos = m_trxpos;
    entry.trxreg = m_trxreg;
    const GSTransferSnapshot transfer = m_backend ? m_backend->GetTransferSnapshot() : GSTransferSnapshot{};
    entry.trxdir = transfer.direction;
    entry.transferPixels = transfer.totalPixels;
    return entry;
}

void GS::recordDebugEventUnlocked(GSDebugHistoryEntry entry)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    const uint64_t tick = m_privRegs ? m_privRegs->vsyncTick.load(std::memory_order_acquire) : 0u;
    if (m_debugLastVsyncTick == UINT64_MAX)
    {
        m_debugLastVsyncTick = tick;
    }
    else if (tick != m_debugLastVsyncTick)
    {
        ++m_debugFrameIndex;
        m_debugLastVsyncTick = tick;
    }

    entry.seq = m_debugNextSeq++;
    entry.vsyncTick = tick;
    entry.frameIndex = m_debugFrameIndex;

    if (entry.kind == GSDebugEventKind::Draw)
    {
        ps2_e4::noteSubmit(tick, entry.frame.fbp); // E4 T6 census (armed window only)
    }

    m_debugHistory[m_debugHistoryWrite] = entry;
    m_debugHistoryWrite = (m_debugHistoryWrite + 1u) % kDebugHistoryCapacity;
    if (m_debugHistoryCount < kDebugHistoryCapacity)
    {
        ++m_debugHistoryCount;
    }
}

void GS::recordGifTagDebugEventUnlocked(uint32_t sizeBytes, uint32_t nloop, uint8_t flg, uint32_t nreg)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::GifTag);
    entry.gifSizeBytes = sizeBytes;
    entry.gifNloop = nloop;
    entry.gifFlg = flg;
    entry.gifNreg = static_cast<uint8_t>(std::min<uint32_t>(nreg, 16u));
    recordDebugEventUnlocked(entry);
}

void GS::recordRegisterDebugEventUnlocked(uint8_t regAddr, uint64_t value)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    switch (regAddr)
    {
    case GS_REG_PRIM:
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    case GS_REG_TEXA:
    case GS_REG_TEXCLUT:
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    case GS_REG_BITBLTBUF:
    case GS_REG_TRXPOS:
    case GS_REG_TRXREG:
    case GS_REG_TRXDIR:
        break;
    default:
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Register);
    entry.reg = regAddr;
    entry.regValue = value;
    recordDebugEventUnlocked(entry);
}

void GS::recordDrawDebugEventUnlocked(int vertexCount)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    if (vertexCount <= 0)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Draw);
    entry.vertexCount = static_cast<uint32_t>(vertexCount);

    const int count = std::min(vertexCount, kMaxVerts);
    entry.xMin = entry.xMax = m_vtxQueue[0].x;
    entry.yMin = entry.yMax = m_vtxQueue[0].y;
    entry.zMin = entry.zMax = m_vtxQueue[0].z;
    entry.aMin = entry.aMax = m_vtxQueue[0].a;

    for (int i = 1; i < count; ++i)
    {
        const GSVertex &v = m_vtxQueue[i];
        entry.xMin = std::min(entry.xMin, v.x);
        entry.xMax = std::max(entry.xMax, v.x);
        entry.yMin = std::min(entry.yMin, v.y);
        entry.yMax = std::max(entry.yMax, v.y);
        entry.zMin = std::min(entry.zMin, v.z);
        entry.zMax = std::max(entry.zMax, v.z);
        entry.aMin = std::min(entry.aMin, v.a);
        entry.aMax = std::max(entry.aMax, v.a);
    }

    recordDebugEventUnlocked(entry);
}

void GS::recordTransferDebugEventUnlocked()
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Transfer);
    entry.transferPixels = m_backend ? m_backend->GetTransferSnapshot().totalPixels : 0u;
    recordDebugEventUnlocked(entry);
}

void GS::recordPresentDebugEventUnlocked(uint32_t displayFbp, uint32_t sourceFbp, uint32_t width, uint32_t height, bool usedPreferred)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Present);
    entry.displayFbp = displayFbp;
    entry.sourceFbp = sourceFbp;
    entry.width = width;
    entry.height = height;
    entry.usedPreferred = usedPreferred;
    recordDebugEventUnlocked(entry);
}

bool GS::getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const
{
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::GetPreferredSource;
        auto rpc = std::make_shared<GsRpc<QueuedPreferredSource>>();
        cmd.rpc = rpc;
        m_worker->enqueue(std::move(cmd));
        rpc->wait();
        outSource = rpc->result.source;
        outDestFbp = rpc->result.destFbp;
        return rpc->result.has;
    }
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasPreferredDisplaySource)
    {
        outSource = {};
        outDestFbp = 0u;
        return false;
    }

    outSource = m_preferredDisplaySourceFrame;
    outDestFbp = m_preferredDisplayDestFbp;
    return true;
}

void GS::unlockDisplaySnapshot()
{
    m_snapshotMutex.unlock();
}

uint32_t GS::getLastDisplayBaseBytes() const
{
    return m_lastDisplayBaseBytes;
}

void GS::refreshDisplaySnapshot()
{
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::RefreshSnapshot;
        cmd.rpc = std::make_shared<GsRpcBase>();
        std::shared_ptr<GsRpcBase> rpc = cmd.rpc;
        m_worker->enqueue(std::move(cmd));
        rpc->wait();
        return;
    }
    snapshotVRAM();
}

GSPresentationRequest GS::buildPresentationRequestUnlocked() const
{
    GSPresentationRequest request{};
    if (!m_privRegs)
        return request;
    request.pmode = m_privRegs->pmode;
    request.smode2 = m_privRegs->smode2;
    request.dispfb1 = m_privRegs->dispfb1;
    request.display1 = m_privRegs->display1;
    request.dispfb2 = m_privRegs->dispfb2;
    request.display2 = m_privRegs->display2;
    request.bgcolor = m_privRegs->bgcolor;
    request.vsyncTick = m_privRegs->vsyncTick.load(std::memory_order_acquire);
    request.contextFrames[0] = m_ctx[0].frame;
    request.contextFrames[1] = m_ctx[1].frame;
    request.preferredSource = m_preferredDisplaySourceFrame;
    request.preferredDestFbp = m_preferredDisplayDestFbp;
    request.hasPreferredSource = m_hasPreferredDisplaySource;
    return request;
}

void GS::latchHostPresentationFrame()
{
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::LatchPresent;
        cmd.rpc = std::make_shared<GsRpcBase>();
        std::shared_ptr<GsRpcBase> rpc = cmd.rpc;
        m_worker->enqueue(std::move(cmd));
        rpc->wait();
        return;
    }
    GSPresentationRequest request{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        if (!m_backend || !m_privRegs)
        {
            std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
            m_hostPresentationFrame.clear();
            m_hasHostPresentationFrame = false;
            m_hostPresentationWidth = m_hostPresentationHeight = 0u;
            return;
        }
        request = buildPresentationRequestUnlocked();
    }

    PresentationFrame frame{};
    {
        std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Presentation);
            frame = m_backend->Present(request);
        }
    }

    const bool hasFrame = static_cast<bool>(frame);
    const uint32_t displayFbp = frame.displayFbp;
    const uint32_t sourceFbp = frame.sourceFbp;
    const uint32_t width = frame.width;
    const uint32_t height = frame.height;
    const bool usedPreferred = frame.usedPreferred;
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationFrame = std::move(frame.pixels);
        m_hostPresentationWidth = width;
        m_hostPresentationHeight = height;
        m_hostPresentationDisplayFbp = displayFbp;
        m_hostPresentationSourceFbp = sourceFbp;
        m_hostPresentationUsedPreferred = usedPreferred;
        m_hasHostPresentationFrame = hasFrame;
    }

    if (hasFrame)
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        recordPresentDebugEventUnlocked(displayFbp, sourceFbp, width, height, usedPreferred);
    }
}

PresentationFrame GS::presentForDiagnostics()
{
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::DiagPresent;
        auto rpc = std::make_shared<GsRpc<PresentationFrame>>();
        cmd.rpc = rpc;
        m_worker->enqueue(std::move(cmd));
        rpc->wait();
        return std::move(rpc->result);
    }
    GSPresentationRequest request{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        if (!m_backend || !m_privRegs)
            return {};
        request = buildPresentationRequestUnlocked();
    }
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (!m_backend)
        return {};
    m_backend->Flush();
    m_backend->Sync(GSSyncReason::Presentation);
    return m_backend->Present(request);
}

bool GS::copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp,
                                          uint32_t *outSourceFbp,
                                          bool *outUsedPreferred) const
{
    std::lock_guard<std::mutex> lock(m_presentationMutex);
    if (!m_hasHostPresentationFrame || m_hostPresentationFrame.empty())
    {
        outPixels.clear();
        outWidth = 0u;
        outHeight = 0u;
        if (outDisplayFbp)
            *outDisplayFbp = 0u;
        if (outSourceFbp)
            *outSourceFbp = 0u;
        if (outUsedPreferred)
            *outUsedPreferred = false;
        return false;
    }

    outWidth = m_hostPresentationWidth;
    outHeight = m_hostPresentationHeight;
    if (outDisplayFbp)
        *outDisplayFbp = m_hostPresentationDisplayFbp;
    if (outSourceFbp)
        *outSourceFbp = m_hostPresentationSourceFbp;
    if (outUsedPreferred)
        *outUsedPreferred = m_hostPresentationUsedPreferred;

    const size_t packedRowBytes = static_cast<size_t>(outWidth) * 4u;
    outPixels.resize(packedRowBytes * static_cast<size_t>(outHeight));
    if (outWidth != 0u && outHeight != 0u)
    {
        // HR1: frames that don't fit the legacy 640x512 host frame
        // (paraLLEl high-resolution scanout) arrive packed at stride = width.
        const bool packed = outWidth > kHostFrameWidth || outHeight > 512u;
        const size_t sourceRowBytes = static_cast<size_t>(packed ? outWidth : kHostFrameWidth) * 4u;
        for (uint32_t y = 0; y < outHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * sourceRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * packedRowBytes;
            if (srcOffset + packedRowBytes > m_hostPresentationFrame.size() ||
                dstOffset + packedRowBytes > outPixels.size())
            {
                outPixels.clear();
                outWidth = 0u;
                outHeight = 0u;
                if (outDisplayFbp)
                    *outDisplayFbp = 0u;
                if (outSourceFbp)
                    *outSourceFbp = 0u;
                if (outUsedPreferred)
                    *outUsedPreferred = false;
                return false;
            }

            std::memcpy(outPixels.data() + dstOffset,
                        m_hostPresentationFrame.data() + srcOffset,
                        packedRowBytes);
        }
    }
    return true;
}

void GS::processGIFPacketWithPath(GifPathId path, bool notePath, std::vector<uint8_t> &bytes)
{
    const uint32_t sizeBytes = static_cast<uint32_t>(bytes.size());
    if (!m_worker || t_inGsWorker || sizeBytes < 16u)
    {
        if (notePath)
            noteGifPath(path);
        processGIFPacket(bytes.data(), sizeBytes);
        return;
    }
    ps2_mtvu::touch(ps2_mtvu::Site::GsProcess); // MT1: unit-owned
    GsCommand cmd;
    cmd.kind = GsCmdKind::GifPacket;
    if (notePath)
    {
        cmd.u32a = kGsGifPacketHasPath;
        cmd.pathId = static_cast<uint8_t>(path);
    }
    cmd.bytes.assign(bytes.begin(), bytes.end());
    m_worker->enqueue(std::move(cmd));
}

void GS::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    ps2_mtvu::touch(ps2_mtvu::Site::GsProcess); // MT1: unit-owned
    if (m_worker && !t_inGsWorker)
    {
        if (!data || sizeBytes < 16)
            return;
        GsCommand cmd;
        cmd.kind = GsCmdKind::GifPacket;
        cmd.bytes.assign(data, data + sizeBytes);
        m_worker->enqueue(std::move(cmd));
        return;
    }
    // E53: GS math runs in host IEEE mode even when called from the EE thread (PS2 FP mode).
    ps2_fpmode::ScopedHostMode hostFpMode;
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes < 16 || !m_backend)
        return;

    const uint64_t index = m_submitCount.fetch_add(1u, std::memory_order_relaxed);
    const uint64_t tick = m_privRegs ? m_privRegs->vsyncTick.load() : 0u;
    const uint8_t path = static_cast<uint8_t>(m_curGifPath);
    GsPacketVramTrace trace{tick, index, path, m_localMemoryStorage, m_localMemorySize};
    ps2x_gs_capture::packet(tick, path, data, sizeBytes);
    ps2_e7::packet(m_privRegs ? m_privRegs->vsyncTick.load() : 0u, "gs-enter", data, sizeBytes);
    // GB3 Part 2: a raw-GIF backend (paraLLEl) renders the packet itself; the
    // decode below still runs for CSR/transfer/preferred-source state.
    if (m_rawGifBackend.load(std::memory_order_relaxed))
        m_backend->RawGifPacket(static_cast<uint32_t>(m_curGifPath), data, sizeBytes);
    if (tryProcessNativeImageUploadPacket(data, sizeBytes))
        return;

    // T1: true GIF-packet count (the [gs:gif] line below caps at 48).
    ps2_park::tallyGsGif();
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t packetIndex = s_debugGifPacketCount.fetch_add(1, std::memory_order_relaxed);
        if (packetIndex < 48u)
        {
            const uint64_t tagLo = loadLE64(data);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            RUNTIME_LOG("[gs:gif] idx=" << packetIndex
                                        << " size=" << sizeBytes
                                        << " nloop=" << nloop
                                        << " flg=" << static_cast<uint32_t>(flg)
                                        << " nreg=" << nreg
                                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                                        << std::endl);
        }
    });

    uint32_t offset = 0;
    while (offset + 16 <= sizeBytes)
    {
        uint64_t tagLo = loadLE64(data + offset);
        uint64_t tagHi = loadLE64(data + offset + 8);
        offset += 16;

        m_curQ = 1.0f;

        uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFF);
        uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;

        recordGifTagDebugEventUnlocked(sizeBytes, nloop, flg, nreg);

        bool pre = ((tagLo >> 46) & 1) != 0;
        if (pre)
        {
            writeRegisterUnlocked(GS_REG_PRIM, (tagLo >> 47) & 0x7FF);
        }

        uint8_t regs[16];
        for (uint32_t i = 0; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4)) & 0xF);

        if (flg == GIF_FMT_PACKED)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 16 > sizeBytes)
                        return;
                    uint64_t lo = loadLE64(data + offset);
                    uint64_t hi = loadLE64(data + offset + 8);
                    offset += 16;
                    writeRegisterPacked(regs[r], lo, hi);
                }
            }
        }
        else if (flg == GIF_FMT_REGLIST)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 8 > sizeBytes)
                        return;
                    writeRegisterUnlocked(regs[r], loadLE64(data + offset));
                    offset += 8;
                }
            }
            if ((nloop * nreg) & 1)
                offset += 8;
        }
        else if (flg == GIF_FMT_IMAGE)
        {
            uint32_t imageBytes = nloop * 16;
            if (offset + imageBytes > sizeBytes)
                imageBytes = sizeBytes - offset;
            processImageData(data + offset, imageBytes);
            offset += imageBytes;
        }
    }
}

bool GS::processNativePackedGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    ps2_mtvu::touch(ps2_mtvu::Site::GsNative); // MT1: unit-owned
    if (m_worker && !t_inGsWorker)
    {
        // The DMA-chain caller needs the verdict synchronously, but the
        // verdict is a pure function of the bytes: validate here, decode on
        // the worker. The worker re-validates the same bytes identically.
        if (!data || sizeBytes < 16u || !validatePackedGifPacket(data, sizeBytes))
            return false;
        GsCommand cmd;
        cmd.kind = GsCmdKind::NativePacked;
        cmd.bytes.assign(data, data + sizeBytes);
        m_worker->enqueue(std::move(cmd));
        return true;
    }
    // E53: GS math runs in host IEEE mode even when called from the EE thread (PS2 FP mode).
    ps2_fpmode::ScopedHostMode hostFpMode;
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes < 16u || !m_backend)
        return false;

    if (!validatePackedGifPacket(data, sizeBytes))
        return false;

    const uint64_t index = m_submitCount.fetch_add(1u, std::memory_order_relaxed);
    const uint64_t tick = m_privRegs ? m_privRegs->vsyncTick.load() : 0u;
    const uint8_t path = static_cast<uint8_t>(m_curGifPath);
    GsPacketVramTrace trace{tick, index, path, m_localMemoryStorage, m_localMemorySize};
    ps2x_gs_capture::packet(tick, path, data, sizeBytes);
    const bool processed = visitPackedGifPacket(data, sizeBytes, [&](const PackedGifPacketTag &tag)
                                                {
        m_curQ = 1.0f;

        recordGifTagDebugEventUnlocked(sizeBytes, tag.nloop, GIF_FMT_PACKED, tag.nreg);

        const bool pre = ((tag.lo >> 46u) & 1u) != 0u;
        if (pre)
            writeRegisterUnlocked(GS_REG_PRIM, (tag.lo >> 47u) & 0x7FFu);

        uint32_t offset = tag.payloadOffset;
        for (uint32_t loop = 0u; loop < tag.nloop; ++loop)
        {
            for (uint32_t r = 0u; r < tag.nreg; ++r)
            {
                const uint64_t lo = loadLE64(data + offset);
                const uint64_t hi = loadLE64(data + offset + 8u);
                offset += 16u;
                writeRegisterPacked(tag.regs[r], lo, hi);
            }
        }

        return true; });

    if (!processed)
        return false;

    ++m_nativePackedGIFPacketCount;
    return true;
}

void GS::uploadImageNative(uint64_t bitbltbuf,
                           uint64_t trxpos,
                           uint64_t trxreg,
                           uint64_t trxdir,
                           const uint8_t *data,
                           uint32_t sizeBytes)
{
    ps2_mtvu::touch(ps2_mtvu::Site::GsNative); // MT1: unit-owned
    if (m_worker && !t_inGsWorker)
    {
        if (!data || sizeBytes == 0)
            return;
        GsCommand cmd;
        cmd.kind = GsCmdKind::UploadImageNative;
        cmd.setupRegs[0] = bitbltbuf;
        cmd.setupRegs[1] = trxpos;
        cmd.setupRegs[2] = trxreg;
        cmd.setupRegs[3] = trxdir;
        cmd.bytes.assign(data, data + sizeBytes);
        m_worker->enqueue(std::move(cmd));
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    const uint64_t index = m_submitCount.fetch_add(1u, std::memory_order_relaxed);
    const uint64_t tick = m_privRegs ? m_privRegs->vsyncTick.load() : 0u;
    GsPacketVramTrace trace{tick, index, static_cast<uint8_t>(m_curGifPath),
                            m_localMemoryStorage, m_localMemorySize};
    ps2x_gs_capture::nativeUpload(tick,
                                  bitbltbuf, trxpos, trxreg, trxdir, data, sizeBytes);
    uploadImageNativeUnlocked(bitbltbuf, trxpos, trxreg, trxdir, data, sizeBytes);
}

void GS::uploadImageNativeUnlocked(uint64_t bitbltbuf,
                                   uint64_t trxpos,
                                   uint64_t trxreg,
                                   uint64_t trxdir,
                                   const uint8_t *data,
                                   uint32_t sizeBytes)
{
    if (!data || sizeBytes == 0 || !m_backend)
        return;

    writeRegisterUnlocked(GS_REG_BITBLTBUF, bitbltbuf);
    writeRegisterUnlocked(GS_REG_TRXPOS, trxpos);
    writeRegisterUnlocked(GS_REG_TRXREG, trxreg);
    writeRegisterUnlocked(GS_REG_TRXDIR, trxdir);
    processImageData(data, sizeBytes);
    ++m_nativeImageUploadCount;
}

bool GS::tryProcessNativeImageUploadPacket(const uint8_t *data, uint32_t sizeBytes)
{
    constexpr uint32_t kSetupRegisters = 4u;
    constexpr uint32_t kPackedAdPayloadBytes = kSetupRegisters * 16u;
    constexpr uint64_t kPackedAdDescriptor = 0x0Eull;

    if (!data || sizeBytes < 16u + kPackedAdPayloadBytes + 16u)
        return false;

    const uint64_t setupTagLo = loadLE64(data);
    const uint64_t setupTagHi = loadLE64(data + 8u);
    const uint32_t setupNloop = static_cast<uint32_t>(setupTagLo & 0x7FFFu);
    const uint8_t setupFlg = static_cast<uint8_t>((setupTagLo >> 58u) & 0x3u);
    uint32_t setupNreg = static_cast<uint32_t>((setupTagLo >> 60u) & 0xFu);
    if (setupNreg == 0u)
        setupNreg = 16u;

    if (setupNloop != kSetupRegisters ||
        setupFlg != GIF_FMT_PACKED ||
        setupNreg != 1u ||
        (setupTagHi & 0xFull) != kPackedAdDescriptor)
    {
        return false;
    }

    uint64_t regs[kSetupRegisters] = {};
    uint32_t offset = 16u;
    constexpr uint8_t expectedRegs[kSetupRegisters] = {
        GS_REG_BITBLTBUF,
        GS_REG_TRXPOS,
        GS_REG_TRXREG,
        GS_REG_TRXDIR,
    };

    for (uint32_t i = 0; i < kSetupRegisters; ++i)
    {
        regs[i] = loadLE64(data + offset);
        const uint64_t reg = loadLE64(data + offset + 8u);
        if ((reg & 0xFFu) != expectedRegs[i])
            return false;
        offset += 16u;
    }

    const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
    const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
    const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
    if (trxdirMode != 0u || rrw == 0u || rrh == 0u)
        return false;

    if (offset + 16u > sizeBytes)
        return false;

    const uint64_t imageTagLo = loadLE64(data + offset);
    const uint8_t imageFlg = static_cast<uint8_t>((imageTagLo >> 58u) & 0x3u);
    const uint32_t imageNloop = static_cast<uint32_t>(imageTagLo & 0x7FFFu);
    if (imageFlg != GIF_FMT_IMAGE || imageNloop == 0u)
        return false;

    offset += 16u;
    const uint64_t imageBytes64 = static_cast<uint64_t>(imageNloop) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);
    if (offset + imageBytes != sizeBytes)
        return false;

    uploadImageNativeUnlocked(regs[0], regs[1], regs[2], regs[3], data + offset, imageBytes);
    return true;
}

void GS::writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi)
{
    switch (regDesc)
    {
    case 0x00:
        writeRegisterUnlocked(GS_REG_PRIM, lo & 0x7FF);
        break;
    case 0x01:
        m_curR = static_cast<uint8_t>(lo & 0xFF);
        m_curG = static_cast<uint8_t>((lo >> 32) & 0xFF);
        m_curB = static_cast<uint8_t>(hi & 0xFF);
        m_curA = static_cast<uint8_t>((hi >> 32) & 0xFF);
        break;
    case 0x02:
    {
        uint32_t sBits = static_cast<uint32_t>(lo & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((lo >> 32) & 0xFFFFFFFF);
        uint32_t qBits = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case 0x03:
        m_curU = static_cast<uint16_t>(lo & 0x3FFFu);
        m_curV = static_cast<uint16_t>((lo >> 32) & 0x3FFFu);
        break;
    case 0x04:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>((hi >> 4) & 0xFFFFFF);
        uint8_t f = static_cast<uint8_t>((hi >> 36) & 0xFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf] idx=" << debugIndex
                                                    << " x=" << x
                                                    << " y=" << y
                                                    << " z=0x" << std::hex << z
                                                    << std::dec
                                                    << " fog=" << static_cast<uint32_t>(f)
                                                    << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = f;
        vertexKick(!adk);
        break;
    }
    case 0x05:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz] idx=" << debugIndex
                                                   << " x=" << x
                                                   << " y=" << y
                                                   << " z=0x" << std::hex << z
                                                   << std::dec
                                                   << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                   << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                   << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(!adk);
        break;
    }
    case 0x0A:
        m_curFog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        break;
    case 0x0C:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf3] idx=" << debugIndex
                                                     << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                     << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                     << " kick=0"
                                                     << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                     << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((hi >> 4) & 0xFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        vertexKick(false);
        break;
    }
    case 0x0D:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz3] idx=" << debugIndex
                                                    << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                    << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                    << " kick=0"
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>(hi & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(false);
        break;
    }
    case 0x0E:
    {
        uint8_t addr = static_cast<uint8_t>(hi & 0xFF);
        writeRegisterUnlocked(addr, lo);
        break;
    }
    case 0x0F:
        break;
    default:
        writeRegisterUnlocked(regDesc, lo);
        break;
    }
}

void GS::writeRegister(uint8_t regAddr, uint64_t value)
{
    ps2_mtvu::touch(ps2_mtvu::Site::GsWriteReg); // MT1: unit-owned
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::RegWrite;
        cmd.regAddr = regAddr;
        cmd.regValue = value;
        m_worker->enqueue(std::move(cmd));
        return;
    }
    // E53: GS math runs in host IEEE mode even when called from the EE thread (PS2 FP mode).
    ps2_fpmode::ScopedHostMode hostFpMode;
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_regWriteCount.fetch_add(1u, std::memory_order_relaxed);
    writeRegisterUnlocked(regAddr, value);
    if (m_rawGifBackend.load(std::memory_order_relaxed))
        m_backend->RawWriteRegister(regAddr, value); // GB3: HLE W1/W2 bypass the GIF stream
    // G44: HLE direct writes (W1/W2) only; GIF-decode-internal writes use
    // writeRegisterUnlocked and must NOT double-feed (paraLLEl decodes the
    // same GIF stream itself).
    ps2x_gs_shadow::onWriteRegister(regAddr, value);
}

void GS::privWrite(std::function<void()> apply)
{
    ps2_mtvu::touch(ps2_mtvu::Site::GsPriv); // MT1: unit-owned
    if (!apply)
        return;
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::PrivWrite;
        cmd.apply = std::move(apply);
        m_worker->enqueue(std::move(cmd));
        return;
    }
    m_privWriteCount.fetch_add(1u, std::memory_order_relaxed);
    apply();
}

void GS::writeRegisterUnlocked(uint8_t regAddr, uint64_t value)
{
    const bool interestingReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_RGBAQ ||
        regAddr == GS_REG_ST ||
        regAddr == GS_REG_UV ||
        regAddr == GS_REG_XYZ2 ||
        regAddr == GS_REG_XYZ3 ||
        regAddr == GS_REG_XYZF2 ||
        regAddr == GS_REG_XYZF3 ||
        regAddr == GS_REG_TEX0_1 ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX2_1 ||
        regAddr == GS_REG_TEX2_2 ||
        regAddr == GS_REG_TEXCLUT ||
        regAddr == GS_REG_TEXA ||
        regAddr == GS_REG_XYOFFSET_1 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_1 ||
        regAddr == GS_REG_SCISSOR_2 ||
        regAddr == GS_REG_FRAME_1 ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_ALPHA_1 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_1 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_BITBLTBUF ||
        regAddr == GS_REG_TRXPOS ||
        regAddr == GS_REG_TRXREG ||
        regAddr == GS_REG_TRXDIR;

    PS2_IF_AGRESSIVE_LOGS({
        if (interestingReg)
        {
            const uint32_t debugIndex = s_debugGsRegisterCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 128u)
            {
                RUNTIME_LOG("[gs:reg] idx=" << debugIndex
                                            << " reg=0x" << std::hex << static_cast<uint32_t>(regAddr)
                                            << " value=0x" << value
                                            << std::dec
                                            << std::endl);
            }
        }
    });

    const bool isCopyRelevantReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX1_2 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_PABE ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_2;
    // T1: true copy-reg count (the [gs:copy-reg] line below caps at 64).
    if (isCopyRelevantReg)
    {
        ps2_park::tallyGsCopyReg();
    }
    PS2_IF_AGRESSIVE_LOGS({
        if (isCopyRelevantReg &&
            s_debugCopyRegCount.fetch_add(1u, std::memory_order_relaxed) < 64u)
        {
            RUNTIME_LOG("[gs:copy-reg] reg=0x"
                        << std::hex << static_cast<uint32_t>(regAddr)
                        << " value=0x" << value
                        << std::dec
                        << " primCtxt=" << static_cast<uint32_t>(m_prim.ctxt)
                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                        << std::endl);
        }
    });

    switch (regAddr)
    {
    case GS_REG_PRIM:
    {
        m_primRegister = decodePrimRegister(value);
        if (m_prmodecont)
        {
            m_prim = m_primRegister;
        }
        else
        {
            // PRIM always selects the primitive topology. With AC=0, all
            // rendering attributes remain sourced from PRMODE.
            m_prim.type = m_primRegister.type;
        }
        m_vtxCount = 0;
        m_vtxIndex = 0;
        break;
    }
    case GS_REG_RGBAQ:
    {
        m_curR = static_cast<uint8_t>(value & 0xFF);
        m_curG = static_cast<uint8_t>((value >> 8) & 0xFF);
        m_curB = static_cast<uint8_t>((value >> 16) & 0xFF);
        m_curA = static_cast<uint8_t>((value >> 24) & 0xFF);
        uint32_t qBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case GS_REG_ST:
    {
        uint32_t sBits = static_cast<uint32_t>(value & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        break;
    }
    case GS_REG_UV:
    {
        m_curU = static_cast<uint16_t>(value & 0x3FFFu);
        m_curV = static_cast<uint16_t>((value >> 16) & 0x3FFFu);
        break;
    }
    case GS_REG_XYZF2:
    case GS_REG_XYZF3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFF);
        vtx.fog = static_cast<uint8_t>((value >> 56) & 0xFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vertexKick(regAddr == GS_REG_XYZF2);
        break;
    }
    case GS_REG_XYZ2:
    case GS_REG_XYZ3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(regAddr == GS_REG_XYZ2);
        break;
    }
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    {
        int ci = (regAddr == GS_REG_TEX0_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.tbp0 = static_cast<uint32_t>(value & 0x3FFF);
        t.tbw = static_cast<uint8_t>((value >> 14) & 0x3F);
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.tw = static_cast<uint8_t>((value >> 26) & 0xF);
        t.th = static_cast<uint8_t>((value >> 30) & 0xF);
        t.tcc = static_cast<uint8_t>((value >> 34) & 0x1);
        t.tfx = static_cast<uint8_t>((value >> 35) & 0x3);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        break;
    }
    case GS_REG_CLAMP_1:
    case GS_REG_CLAMP_2:
    {
        int ci = (regAddr == GS_REG_CLAMP_2) ? 1 : 0;
        m_ctx[ci].clamp = value;
        break;
    }
    case GS_REG_FOG:
        m_curFog = static_cast<uint8_t>((value >> 56) & 0xFF);
        break;
    case GS_REG_TEX1_1:
    case GS_REG_TEX1_2:
    {
        int ci = (regAddr == GS_REG_TEX1_2) ? 1 : 0;
        m_ctx[ci].tex1 = value;
        break;
    }
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    {
        int ci = (regAddr == GS_REG_TEX2_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        break;
    }
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    {
        int ci = (regAddr == GS_REG_XYOFFSET_2) ? 1 : 0;
        m_ctx[ci].xyoffset.ofx = static_cast<uint16_t>(value & 0xFFFF);
        m_ctx[ci].xyoffset.ofy = static_cast<uint16_t>((value >> 32) & 0xFFFF);
        break;
    }
    case GS_REG_PRMODECONT:
    {
        m_prmodecont = (value & 1) != 0;
        const GSPrimType type = m_primRegister.type;
        m_prim = m_prmodecont ? m_primRegister : m_prmodeRegister;
        m_prim.type = type;
        break;
    }
    case GS_REG_PRMODE:
    {
        m_prmodeRegister = decodePrimRegister(value);
        if (!m_prmodecont)
        {
            const GSPrimType type = m_primRegister.type;
            m_prim = m_prmodeRegister;
            m_prim.type = type;
        }
        break;
    }
    case GS_REG_TEXCLUT:
        m_texclut.cbw = static_cast<uint8_t>(value & 0x3Fu);
        m_texclut.cou = static_cast<uint8_t>((value >> 6) & 0x3Fu);
        m_texclut.cov = static_cast<uint16_t>((value >> 12) & 0x3FFu);
        break;
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    {
        int ci = (regAddr == GS_REG_SCISSOR_2) ? 1 : 0;
        m_ctx[ci].scissor.x0 = static_cast<uint16_t>(value & 0x7FF);
        m_ctx[ci].scissor.x1 = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_ctx[ci].scissor.y0 = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_ctx[ci].scissor.y1 = static_cast<uint16_t>((value >> 48) & 0x7FF);
        break;
    }
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    {
        int ci = (regAddr == GS_REG_ALPHA_2) ? 1 : 0;
        m_ctx[ci].alpha = value;
        break;
    }
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    {
        int ci = (regAddr == GS_REG_TEST_2) ? 1 : 0;
        m_ctx[ci].test = value;
        break;
    }
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    {
        int ci = (regAddr == GS_REG_FRAME_2) ? 1 : 0;
        m_ctx[ci].frame.fbp = static_cast<uint32_t>(value & 0x1FF);
        m_ctx[ci].frame.fbw = static_cast<uint32_t>((value >> 16) & 0x3F);
        m_ctx[ci].frame.psm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_ctx[ci].frame.fbmsk = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        break;
    }
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    {
        int ci = (regAddr == GS_REG_ZBUF_2) ? 1 : 0;
        m_ctx[ci].zbuf.zbp = value & 0x1FF;
        m_ctx[ci].zbuf.psm = ((value >> 24) & 0xF) | 0x30;
        m_ctx[ci].zbuf.zmask = (value >> 32) & 1;
        break;
    }
    case GS_REG_FBA_1:
    case GS_REG_FBA_2:
    {
        int ci = (regAddr == GS_REG_FBA_2) ? 1 : 0;
        m_ctx[ci].fba = value;
        break;
    }
    case GS_REG_BITBLTBUF:
    {
        m_bitbltbuf.sbp = static_cast<uint32_t>(value & 0x3FFF);
        m_bitbltbuf.sbw = static_cast<uint8_t>((value >> 16) & 0x3F);
        m_bitbltbuf.spsm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_bitbltbuf.dbp = static_cast<uint32_t>((value >> 32) & 0x3FFF);
        m_bitbltbuf.dbw = static_cast<uint8_t>((value >> 48) & 0x3F);
        m_bitbltbuf.dpsm = static_cast<uint8_t>((value >> 56) & 0x3F);
        break;
    }
    case GS_REG_TRXPOS:
    {
        m_trxpos.ssax = static_cast<uint16_t>(value & 0x7FF);
        m_trxpos.ssay = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_trxpos.dsax = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_trxpos.dsay = static_cast<uint16_t>((value >> 48) & 0x7FF);
        m_trxpos.dir = static_cast<uint8_t>((value >> 59) & 0x3);
        break;
    }
    case GS_REG_TRXREG:
    {
        m_trxreg.rrw = static_cast<uint16_t>(value & 0xFFF);
        m_trxreg.rrh = static_cast<uint16_t>((value >> 32) & 0xFFF);
        break;
    }
    case GS_REG_TRXDIR:
    {
        m_trxdir = static_cast<uint32_t>(value & 0x3);

        if (m_backend)
        {
            GSTransferCommand command{};
            command.bitbltbuf = m_bitbltbuf;
            command.trxpos = m_trxpos;
            command.trxreg = m_trxreg;
            command.direction = m_trxdir;
            m_backend->BeginTransfer(command);
        }
        recordTransferDebugEventUnlocked();
        ps2x_gs_capture::transfer(m_privRegs ? m_privRegs->vsyncTick.load() : 0u,
                                  (static_cast<uint64_t>(m_bitbltbuf.sbp) |
                                   (static_cast<uint64_t>(m_bitbltbuf.sbw) << 16) |
                                   (static_cast<uint64_t>(m_bitbltbuf.spsm) << 24) |
                                   (static_cast<uint64_t>(m_bitbltbuf.dbp) << 32) |
                                   (static_cast<uint64_t>(m_bitbltbuf.dbw) << 48) |
                                   (static_cast<uint64_t>(m_bitbltbuf.dpsm) << 56)),
                                  (static_cast<uint64_t>(m_trxpos.ssax) |
                                   (static_cast<uint64_t>(m_trxpos.ssay) << 16) |
                                   (static_cast<uint64_t>(m_trxpos.dsax) << 32) |
                                   (static_cast<uint64_t>(m_trxpos.dsay) << 48) |
                                   (static_cast<uint64_t>(m_trxpos.dir) << 59)),
                                  (static_cast<uint64_t>(m_trxreg.rrw) |
                                   (static_cast<uint64_t>(m_trxreg.rrh) << 32)), m_trxdir);
        break;
    }
    case GS_REG_HWREG:
    {
        uint8_t buf[8];
        std::memcpy(buf, &value, 8);
        processImageData(buf, 8);
        break;
    }
    case GS_REG_PABE:
        m_pabe = (value & 1u) != 0u;
        break;
    case GS_REG_FOGCOL:
        m_fogR = static_cast<uint8_t>(value & 0xFFu);
        m_fogG = static_cast<uint8_t>((value >> 8) & 0xFFu);
        m_fogB = static_cast<uint8_t>((value >> 16) & 0xFFu);
        break;
    case GS_REG_TEXFLUSH:
        if (m_backend)
            m_backend->TextureFlush();
        break;
    case GS_REG_SCANMSK:
        m_scanmsk = value;
        break;
    case GS_REG_DIMX:
        m_dimx = value;
        break;
    case GS_REG_DTHE:
        m_dthe = value;
        break;
    case GS_REG_COLCLAMP:
        m_colclamp = value;
        break;
    case GS_REG_MIPTBP1_1:
    case GS_REG_MIPTBP1_2:
    {
        const int ci = (regAddr == GS_REG_MIPTBP1_2) ? 1 : 0;
        m_ctx[ci].miptbp1 = value;
        break;
    }
    case GS_REG_MIPTBP2_1:
    case GS_REG_MIPTBP2_2:
    {
        const int ci = (regAddr == GS_REG_MIPTBP2_2) ? 1 : 0;
        m_ctx[ci].miptbp2 = value;
        break;
    }
    case GS_REG_TEXA:
    {
        m_texa.ta0 = static_cast<uint8_t>(value & 0xFFu);
        m_texa.aem = ((value >> 15) & 0x1u) != 0u;
        m_texa.ta1 = static_cast<uint8_t>((value >> 32) & 0xFFu);
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t texaIndex = s_debugTexaWriteCount.fetch_add(1u, std::memory_order_relaxed);
            if (texaIndex < 24u)
            {
                RUNTIME_LOG("[gs:texa] idx=" << texaIndex
                                             << " value=0x" << std::hex << value
                                             << " ta0=0x" << ((value >> 0) & 0xFFu)
                                             << " aem=" << ((value >> 15) & 0x1u)
                                             << " ta1=0x" << ((value >> 32) & 0xFFu)
                                             << std::dec
                                             << std::endl);
            }
        });
        break;
    }
    case GS_REG_SIGNAL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            // GB2 Part 7: CAS loop — siglblid is atomic (game-thread
            // guest stores race this worker masked-RMW).
            uint64_t expected = m_privRegs->siglblid.load();
            uint64_t desired;
            do
            {
                uint32_t lo = static_cast<uint32_t>(expected & 0xFFFFFFFF);
                lo = (lo & ~mask) | (id & mask);
                desired = (expected & 0xFFFFFFFF00000000ULL) | lo;
            } while (!m_privRegs->siglblid.compare_exchange_weak(expected, desired));
            m_privRegs->csr.fetch_or(0x1);
        }
        break;
    }
    case GS_REG_FINISH:
    {
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Finish);
        }
        if (m_privRegs)
            m_privRegs->csr.fetch_or(0x2);
        break;
    }
    case GS_REG_LABEL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            // GB2 Part 7: CAS loop — see SIGNAL above.
            uint64_t expected = m_privRegs->siglblid.load();
            uint64_t desired;
            do
            {
                uint32_t hi = static_cast<uint32_t>(expected >> 32);
                hi = (hi & ~mask) | (id & mask);
                desired = (static_cast<uint64_t>(hi) << 32) | (expected & 0xFFFFFFFF);
            } while (!m_privRegs->siglblid.compare_exchange_weak(expected, desired));
        }
        break;
    }
    case 0x59:
        if (m_privRegs)
            m_privRegs->dispfb1 = value;
        break;
    case 0x5a:
        if (m_privRegs)
            m_privRegs->display1 = value;
        break;
    case 0x5b:
        if (m_privRegs)
            m_privRegs->dispfb2 = value;
        break;
    case 0x5c:
        if (m_privRegs)
            m_privRegs->display2 = value;
        break;
    case 0x5f:
        if (m_privRegs)
            m_privRegs->bgcolor = value;
        break;
    default:
        break;
    }

    recordRegisterDebugEventUnlocked(regAddr, value);
}

void GS::vertexKick(bool drawing)
{
    ++m_vtxCount;
    ++m_vtxIndex;

    // T1: true kick count (the [gs:kick] line below caps at 96).
    ps2_park::tallyGsKick(drawing);

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t debugIndex = s_debugGsVertexKickCount.fetch_add(1, std::memory_order_relaxed);
        if (debugIndex < 96u)
        {
            RUNTIME_LOG("[gs:kick] idx=" << debugIndex
                                         << " drawing=" << static_cast<uint32_t>(drawing ? 1u : 0u)
                                         << " prim=" << static_cast<uint32_t>(m_prim.type)
                                         << " vtxCount=" << m_vtxCount
                                         << std::endl);
        }
    });

    int needed = 0;
    switch (m_prim.type)
    {
    case GS_PRIM_POINT:
        needed = 1;
        break;
    case GS_PRIM_LINE:
        needed = 2;
        break;
    case GS_PRIM_LINESTRIP:
        needed = 2;
        break;
    case GS_PRIM_TRIANGLE:
        needed = 3;
        break;
    case GS_PRIM_TRISTRIP:
        needed = 3;
        break;
    case GS_PRIM_TRIFAN:
        needed = 3;
        break;
    case GS_PRIM_SPRITE:
        needed = 2;
        break;
    default:
        return;
    }

    if (m_vtxCount < needed)
        return;

    if (drawing && m_backend)
    {
        GSPrimitiveBatch batch = buildDrawBatch(needed);
        updatePreferredDisplaySourceForDraw(batch);
        m_backend->Submit(batch);
        recordDrawDebugEventUnlocked(needed);
        // E33: per-path draw census. One relaxed check when stats are off.
        if (ps2_gfx_stats::enabled())
        {
            float xMin = m_vtxQueue[0].x;
            float xMax = m_vtxQueue[0].x;
            float yMin = m_vtxQueue[0].y;
            float yMax = m_vtxQueue[0].y;
            double zMin = m_vtxQueue[0].z;
            double zMax = m_vtxQueue[0].z;
            const int count = std::min(needed, kMaxVerts);
            for (int i = 1; i < count; ++i)
            {
                const GSVertex &v = m_vtxQueue[i];
                xMin = std::min(xMin, v.x);
                xMax = std::max(xMax, v.x);
                yMin = std::min(yMin, v.y);
                yMax = std::max(yMax, v.y);
                zMin = std::min(zMin, v.z);
                zMax = std::max(zMax, v.z);
            }
            ps2_gfx_stats::noteDraw(m_curGifPath, static_cast<uint32_t>(needed),
                                    xMin, xMax, yMin, yMax, zMin, zMax,
                                    batch.state.context.frame.fbp,
                                    static_cast<uint32_t>(batch.state.prim.type));
        }
    }

    switch (m_prim.type)
    {
    case GS_PRIM_LINE:
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_SPRITE:
    case GS_PRIM_POINT:
        m_vtxCount = 0;
        break;
    case GS_PRIM_LINESTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxCount = 1;
        break;
    case GS_PRIM_TRISTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    case GS_PRIM_TRIFAN:
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    default:
        m_vtxCount = 0;
        break;
    }
}

void GS::processImageData(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_backend)
        m_backend->UploadImage(data, sizeBytes);
}


bool GS::clearFramebufferContext(uint32_t contextIndex, uint32_t rgba)
{
    ps2_mtvu::touch(ps2_mtvu::Site::GsClear); // MT1: unit-owned
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::ClearCtx;
        cmd.u32a = contextIndex;
        cmd.u32b = rgba;
        auto rpc = std::make_shared<GsRpc<bool>>();
        cmd.rpc = rpc;
        m_worker->enqueue(std::move(cmd));
        rpc->wait();
        return rpc->result;
    }
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    ps2x_gs_capture::clearContext(m_privRegs ? m_privRegs->vsyncTick.load() : 0u,
                                  contextIndex, rgba);
    return m_backend && m_backend->ClearFramebuffer(m_ctx[(contextIndex != 0u) ? 1 : 0], rgba);
}

bool GS::clearActiveFramebuffer(uint32_t rgba)
{
    ps2_mtvu::touch(ps2_mtvu::Site::GsClear); // MT1: unit-owned
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::ClearActive;
        cmd.u32a = rgba;
        auto rpc = std::make_shared<GsRpc<bool>>();
        cmd.rpc = rpc;
        m_worker->enqueue(std::move(cmd));
        rpc->wait();
        return rpc->result;
    }
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend && m_backend->ClearFramebuffer(activeContext(), rgba);
}

uint32_t GS::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    ps2_mtvu::touch(ps2_mtvu::Site::GsReadback); // MT1: unit-owned
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::Consume;
        cmd.u32a = maxBytes;
        auto rpc = std::make_shared<GsRpc<std::vector<uint8_t>>>();
        cmd.rpc = rpc;
        m_worker->enqueue(std::move(cmd));
        rpc->wait();
        const size_t n = std::min<size_t>(rpc->result.size(), maxBytes);
        if (dst && n != 0u)
            std::memcpy(dst, rpc->result.data(), n);
        return static_cast<uint32_t>(n);
    }
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    const uint32_t n = m_backend ? m_backend->ConsumeLocalToHostBytes(dst, maxBytes) : 0u;
    ps2x_gs_capture::localToHost(m_privRegs ? m_privRegs->vsyncTick.load() : 0u,
                                 maxBytes, dst, n);
    return n;
}

void GS::setRasterBackend(std::unique_ptr<GSRasterBackend> backend)
{
    if (!backend)
        backend = std::make_unique<GSCpuBackend>();

    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::SetBackend;
        cmd.backend = std::move(backend);
        cmd.rpc = std::make_shared<GsRpcBase>();
        std::shared_ptr<GsRpcBase> rpc = cmd.rpc;
        m_worker->enqueue(std::move(cmd));
        rpc->wait();
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (m_backend)
    {
        m_backend->Flush();
        m_backend->Sync(GSSyncReason::Reset);

        // The external 4 MiB GS allocation is the backend hand-off format.
        // This keeps hot backend replacement deterministic even when a future
        // GPU backend keeps a private/mirrored local-memory representation.
        if (m_localMemoryStorage && m_localMemorySize != 0u)
        {
            std::vector<uint8_t> localMemory;
            m_backend->SnapshotVram(localMemory);
            const size_t bytes = std::min<size_t>(localMemory.size(), m_localMemorySize);
            if (bytes != 0u)
                std::memcpy(m_localMemoryStorage, localMemory.data(), bytes);
        }
    }

    m_backend = std::move(backend);
    m_backend->Initialize(m_localMemoryStorage, m_localMemorySize);
    m_rawGifBackend.store(m_backend->WantsRawGif(), std::memory_order_release);
}

uint32_t GS::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (m_worker && !t_inGsWorker)
    {
        GsCommand cmd;
        cmd.kind = GsCmdKind::ReadVram;
        cmd.u32a = psm;
        cmd.u32b = base;
        cmd.u32c = bw;
        cmd.u32d = x;
        cmd.u32e = y;
        auto rpc = std::make_shared<GsRpc<uint32_t>>();
        cmd.rpc = rpc;
        m_worker->enqueue(std::move(cmd));
        rpc->wait();
        return rpc->result;
    }
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend ? m_backend->ReadVram(psm, base, bw, x, y) : 0u;
}

void GS::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (m_worker && !t_inGsWorker)
    {
        // Fire-and-forget keeps stream order; any later RPC (ReadVram,
        // present, consume) fences it. Direct readers of VRAM bytes all go
        // through RPCs, so no caller can observe the write early or late.
        GsCommand cmd;
        cmd.kind = GsCmdKind::WriteVram;
        cmd.u32a = psm;
        cmd.u32b = base;
        cmd.u32c = bw;
        cmd.u32d = x;
        cmd.u32e = y;
        cmd.regValue = value;
        m_worker->enqueue(std::move(cmd));
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (m_backend)
        m_backend->WriteVram(psm, base, bw, x, y, value);
}

GSPrimitiveBatch GS::buildDrawBatch(int vertexCount) const
{
    GSPrimitiveBatch batch{};
    batch.vertexCount = static_cast<uint8_t>(std::min(vertexCount, 3));
    for (int i = 0; i < batch.vertexCount; ++i)
        batch.vertices[static_cast<size_t>(i)] = m_vtxQueue[i];
    batch.state.context = m_ctx[m_prim.ctxt ? 1 : 0];
    batch.state.prim = m_prim;
    batch.state.texa = m_texa;
    batch.state.texclut = m_texclut;
    batch.state.pabe = m_pabe;
    batch.state.scanmsk = m_scanmsk;
    batch.state.dimx = m_dimx;
    batch.state.dthe = m_dthe;
    batch.state.colclamp = m_colclamp;
    batch.state.fogR = m_fogR;
    batch.state.fogG = m_fogG;
    batch.state.fogB = m_fogB;
    batch.state.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(batch.state.context.tex0.tw, 10u));
    batch.state.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(batch.state.context.tex0.th, 10u));
    const uint64_t tex1 = batch.state.context.tex1;
    const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5u) & 0x1u);
    const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6u) & 0x7u);
    batch.state.linearFilter = mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    return batch;
}

void GS::updatePreferredDisplaySourceForDraw(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSContext &ctx = state.context;
    if (m_hasPreferredDisplaySource && ctx.frame.fbp == m_preferredDisplayDestFbp)
        m_hasPreferredDisplaySource = false;
    if (state.prim.type != GS_PRIM_SPRITE || batch.vertexCount < 2u)
        return;

    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    int x0 = static_cast<int>(v0.x) - (ctx.xyoffset.ofx >> 4);
    int y0 = static_cast<int>(v0.y) - (ctx.xyoffset.ofy >> 4);
    int x1 = static_cast<int>(v1.x) - (ctx.xyoffset.ofx >> 4);
    int y1 = static_cast<int>(v1.y) - (ctx.xyoffset.ofy >> 4);
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const int xEnd = x0 + std::max(1, x1 - x0) - 1;
    const int yEnd = y0 + std::max(1, y1 - y0) - 1;
    const uint8_t alphaMode = static_cast<uint8_t>(ctx.alpha & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((ctx.alpha >> 32u) & 0xFFu);
    const bool displayCopy = state.prim.tme && state.prim.abe && state.prim.fst && state.prim.ctxt &&
                             ctx.frame.fbp != ctx.tex0.tbp0 && alphaMode == 0x64u &&
                             (alphaFix == 0x60u || alphaFix == 0x80u) &&
                             x0 <= 0 && y0 <= 0 && xEnd >= 639 && yEnd >= 447;
    if (displayCopy)
    {
        m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
        m_preferredDisplayDestFbp = ctx.frame.fbp;
        m_hasPreferredDisplaySource = true;
    }
}
