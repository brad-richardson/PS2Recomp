#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_log.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace GSInternal;

// GB7B bounded spatial title-glyph candidate probe (default OFF).
// Logs CPU draw batches whose clipped screen rect overlaps the title
// crops upper (320,120)-(420,205) or lower (340,360)-(430,420).
// Caps: 20,000 rows, 8 MiB total; rendering is untouched.
namespace
{
struct Gb7bProbeState
{
    bool enabled = false;
    std::ofstream out;
    Gb7bPacketContext ctx{};
    uint64_t rows = 0;
    uint64_t bytes = 0;
    bool capped = false;
};

Gb7bProbeState &gb7bState()
{
    static Gb7bProbeState s;
    return s;
}

constexpr uint64_t kGb7bMaxRows = 20000u;
constexpr uint64_t kGb7bMaxBytes = 8u * 1024u * 1024u;
constexpr int kGb7bUpperX0 = 320;
constexpr int kGb7bUpperY0 = 120;
constexpr int kGb7bUpperX1 = 420;
constexpr int kGb7bUpperY1 = 205;
constexpr int kGb7bLowerX0 = 340;
constexpr int kGb7bLowerY0 = 360;
constexpr int kGb7bLowerX1 = 430;
constexpr int kGb7bLowerY1 = 420;

uint64_t gb7bOverlap(int x0, int y0, int x1, int y1,
                     int cx0, int cy0, int cx1, int cy1)
{
    const int ox0 = std::max(x0, cx0);
    const int oy0 = std::max(y0, cy0);
    const int ox1 = std::min(x1, cx1);
    const int oy1 = std::min(y1, cy1);
    if (ox1 < ox0 || oy1 < oy0)
        return 0u;
    return static_cast<uint64_t>(ox1 - ox0 + 1) *
           static_cast<uint64_t>(oy1 - oy0 + 1);
}

uint32_t gb7bFnv(const uint8_t *data, size_t size)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}
} // namespace

void ps2xGb7bProbeOpen(const char *path)
{
    Gb7bProbeState &s = gb7bState();
    if (s.enabled || !path || !*path)
        return;
    s.out.open(path, std::ios::binary | std::ios::trunc);
    if (!s.out)
        return;
    s.out << "tick\tpacket_index\tpath\tprim\ttme\tfst\trect\trect_kind\tcrop\t"
             "frame\ttex0\tclamp\ttest\talpha\tuv\tsource_fingerprint\t"
             "classification\treason\n";
    s.bytes += 118u;
    s.ctx = Gb7bPacketContext{};
    s.rows = 0;
    s.capped = false;
    s.enabled = s.out.good();
}

void ps2xGb7bProbeClose()
{
    Gb7bProbeState &s = gb7bState();
    s.enabled = false;
    if (s.out.is_open())
        s.out.close();
}

void ps2xGb7bSetPacketContext(uint64_t tick, uint64_t packetIndex, unsigned path)
{
    Gb7bProbeState &s = gb7bState();
    s.ctx.tick = tick;
    s.ctx.packetIndex = packetIndex;
    s.ctx.path = path;
}

bool ps2xGb7bProbeEnabled()
{
    return gb7bState().enabled;
}

// GB7C2 bounded CPU pixel/ROI title-text chain probe (default OFF).
// Predeclared single chain only: tick600 path2 packet47176 (off-screen
// fbp=0 T4 sprites, STQ) -> tick601 path3 packet47240 (same-column
// fbp=112/tbp0=0 display blit, FST). Per candidate sprite the probe logs
// one batch row, up to 4 interior pixels with an independent old read
// taken immediately before WritePixel plus the new value after, and one
// off-screen ROI before/after diff. The WritePixel-internal conditional
// old read is never used as proof. Rendering is untouched: the probe
// only adds ReadVramUnlocked calls (side-effect-free) and log writes.
namespace
{
struct Gb7c2ProbeState
{
    bool enabled = false;
    std::ofstream out;
    Gb7c2PacketContext ctx{};
    uint64_t nextBatch = 0;
    uint64_t rows = 0;
    uint64_t bytes = 0;
    bool capped = false;
    // Current batch (valid while DrawPrimitive dispatches it).
    int curKind = 0; // 0 = untraced, 1 = C1 glyph batch, 2 = carrier batch
    uint64_t curBatch = 0;
    // C1 interior-pixel picks (framebuffer coords).
    int pickX[4] = {0, 0, 0, 0};
    int pickY[4] = {0, 0, 0, 0};
    int curPicks = 0;
    int curLogged = 0;
    // Off-screen ROI (340,375)-(410,415) snapshots, CT32 words.
    uint32_t roiFbp = 0;
    uint32_t roiFbw = 8;
    uint32_t roiPsm = 0;
    std::vector<uint32_t> roiBefore;
    std::vector<uint32_t> lastC1After;
    uint32_t lastC1Hash = 0;
    bool lastC1Valid = false;
};

Gb7c2ProbeState &gb7c2State()
{
    static Gb7c2ProbeState s;
    return s;
}

constexpr uint64_t kGb7c2MaxRows = 20000u;
constexpr uint64_t kGb7c2MaxBytes = 8u * 1024u * 1024u;
constexpr int kGb7c2RoiX0 = 340;
constexpr int kGb7c2RoiY0 = 375;
constexpr int kGb7c2RoiX1 = 410;
constexpr int kGb7c2RoiY1 = 415;
constexpr int kGb7c2CropX0 = 340;
constexpr int kGb7c2CropY0 = 360;
constexpr int kGb7c2CropX1 = 430;
constexpr int kGb7c2CropY1 = 420;
constexpr int kGb7c2MaxPicks = 4;
constexpr int kGb7c2MaxCarrierPixels = 8;

void gb7c2Emit(const char *srcXy, const char *dstXy,
               const char *oldV, const char *newV,
               const char *texel, const char *roiDiff, const char *cropDiff,
               const char *classification)
{
    Gb7c2ProbeState &s = gb7c2State();
    if (!s.enabled || s.capped)
        return;
    char line[2048];
    const int count = std::snprintf(
        line, sizeof(line), "%llu\t%llu\t%u\t%llu\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
        static_cast<unsigned long long>(s.ctx.tick),
        static_cast<unsigned long long>(s.ctx.packetIndex), s.ctx.path,
        static_cast<unsigned long long>(s.curBatch),
        srcXy, dstXy, oldV, newV, texel, roiDiff, cropDiff, classification);
    if (count <= 0 || static_cast<size_t>(count) >= sizeof(line))
        return;
    if (s.rows >= kGb7c2MaxRows || s.bytes + static_cast<uint64_t>(count) > kGb7c2MaxBytes)
    {
        s.capped = true;
        return;
    }
    s.out << line;
    if (!s.out.good())
    {
        s.capped = true;
        return;
    }
    ++s.rows;
    s.bytes += static_cast<uint64_t>(count);
}
} // namespace

void ps2xGb7c2ProbeOpen(const char *path)
{
    Gb7c2ProbeState &s = gb7c2State();
    if (s.enabled || !path || !*path)
        return;
    s.out.open(path, std::ios::binary | std::ios::trunc);
    if (!s.out)
        return;
    s.out << "tick\tpacket\tpath\tbatch\tsrc_xy\tdst_xy\told\tnew\ttexel\troi_diff\tcrop_diff\tclassification\n";
    s.bytes += 84u;
    s.ctx = Gb7c2PacketContext{};
    s.nextBatch = 0;
    s.rows = 0;
    s.capped = false;
    s.curKind = 0;
    s.curBatch = 0;
    s.curPicks = 0;
    s.curLogged = 0;
    s.roiBefore.clear();
    s.lastC1After.clear();
    s.lastC1Hash = 0;
    s.lastC1Valid = false;
    s.enabled = s.out.good();
}

void ps2xGb7c2ProbeClose()
{
    Gb7c2ProbeState &s = gb7c2State();
    if (s.enabled)
    {
        std::cout << "GB7C2_SUMMARY rows=" << s.rows << " bytes=" << s.bytes
                  << " capped=" << (s.capped ? 1 : 0) << '\n';
    }
    s.enabled = false;
    if (s.out.is_open())
        s.out.close();
}

void ps2xGb7c2SetPacketContext(uint64_t tick, uint64_t packetIndex, unsigned path)
{
    Gb7c2ProbeState &s = gb7c2State();
    s.ctx.tick = tick;
    s.ctx.packetIndex = packetIndex;
    s.ctx.path = path;
    s.nextBatch = 0;
}

bool ps2xGb7c2ProbeEnabled()
{
    return gb7c2State().enabled;
}

// GB7C4 actual carrier glyph-row sample probe (default OFF).
// Predeclared candidate pairs only (GB7C3 candidate-pairs.tsv):
//   A: C1 src (343,378) -> carrier dst (342,377), packet47240 batch 10
//   B: C1 src (369,381) -> carrier dst (368,380), batch 11
//   C: C1 src (382,381) -> carrier dst (381,380), batch 11
//   D: C1 src (407,378) -> carrier dst (406,377), batch 12
// C1 frame block (fbp0<<5 = 0) and carrier tex block (tbp0 = 0) are both
// block 0 at tbw/fbw 8, so a carrier tap swizzled address is directly
// comparable to the C1 source word address. Destination words live in the
// fbp112 block (CT24, which shares the C32 page tables with CT32, so the
// same address helper applies). Rendering is untouched: the probe only
// adds ReadVramUnlocked calls (side-effect-free) and log writes.
namespace
{
struct Gb7c4ProbeState
{
    bool enabled = false;
    std::ofstream out;
    Gb7c4PacketContext ctx{};
    uint64_t nextBatch = 0;
    uint64_t rows = 0;
    uint64_t bytes = 0;
    bool capped = false;
    // Current batch (valid while DrawPrimitive dispatches it).
    int curKind = 0; // 0 = untraced, 1 = C1 batch, 2 = carrier batch
    uint64_t curBatch = 0;
    // C1 source words observed at the candidate coords (frame block 0).
    bool c1Seen[4] = {false, false, false, false};
    uint32_t c1Old[4] = {0, 0, 0, 0};
    uint32_t c1New[4] = {0, 0, 0, 0};
    uint32_t c1Addr[4] = {0, 0, 0, 0};
    bool carrierSeen[4] = {false, false, false, false};
    // Candidate A yielded outcome A: suppress B/C/D carrier rows.
    bool aComplete = false;
};

Gb7c4ProbeState &gb7c4State()
{
    static Gb7c4ProbeState s;
    return s;
}

constexpr uint64_t kGb7c4MaxRows = 20000u;
constexpr uint64_t kGb7c4MaxBytes = 8u * 1024u * 1024u;
// Candidate C1 sources (framebuffer pixels in the fbp0 block).
constexpr int kGb7c4SrcX[4] = {343, 369, 382, 407};
constexpr int kGb7c4SrcY[4] = {378, 381, 381, 378};
// Candidate carrier destinations (framebuffer pixels in fbp112).
constexpr int kGb7c4DstX[4] = {342, 368, 381, 406};
constexpr int kGb7c4DstY[4] = {377, 380, 380, 377};
constexpr char kGb7c4CandName[4] = {'A', 'B', 'C', 'D'};

void gb7c4Emit(const char *cand, const char *kind,
               const char *dstXy, const char *dstAddr,
               const char *oldV, const char *newV,
               const char *srcUv, const char *taps, const char *tapState,
               const char *blend, const char *state, const char *test,
               const char *classification)
{
    Gb7c4ProbeState &s = gb7c4State();
    if (!s.enabled || s.capped)
        return;
    char line[4096];
    const int count = std::snprintf(
        line, sizeof(line), "%llu\t%llu\t%u\t%llu\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
        static_cast<unsigned long long>(s.ctx.tick),
        static_cast<unsigned long long>(s.ctx.packetIndex), s.ctx.path,
        static_cast<unsigned long long>(s.curBatch),
        cand, kind, dstXy, dstAddr, oldV, newV, srcUv, taps, tapState,
        blend, state, test, classification);
    if (count <= 0 || static_cast<size_t>(count) >= sizeof(line))
        return;
    if (s.rows >= kGb7c4MaxRows || s.bytes + static_cast<uint64_t>(count) > kGb7c4MaxBytes)
    {
        s.capped = true;
        return;
    }
    s.out << line;
    if (!s.out.good())
    {
        s.capped = true;
        return;
    }
    ++s.rows;
    s.bytes += static_cast<uint64_t>(count);
}
} // namespace

void ps2xGb7c4ProbeOpen(const char *path)
{
    Gb7c4ProbeState &s = gb7c4State();
    if (s.enabled || !path || !*path)
        return;
    s.out.open(path, std::ios::binary | std::ios::trunc);
    if (!s.out)
        return;
    s.out << "tick\tpacket\tpath\tbatch\tcand\tkind\tdst_xy\tdst_addr\told\tnew\tsrc_uv\ttaps\ttap_state\tblend\tstate\ttest\tclassification\n";
    s.bytes += 95u;
    s.ctx = Gb7c4PacketContext{};
    s.nextBatch = 0;
    s.rows = 0;
    s.capped = false;
    s.curKind = 0;
    s.curBatch = 0;
    for (int i = 0; i < 4; ++i)
    {
        s.c1Seen[i] = false;
        s.c1Old[i] = s.c1New[i] = s.c1Addr[i] = 0u;
        s.carrierSeen[i] = false;
    }
    s.aComplete = false;
    s.enabled = s.out.good();
}

void ps2xGb7c4ProbeClose()
{
    Gb7c4ProbeState &s = gb7c4State();
    if (s.enabled)
    {
        std::cout << "GB7C4_SUMMARY rows=" << s.rows << " bytes=" << s.bytes
                  << " capped=" << (s.capped ? 1 : 0) << '\n';
    }
    s.enabled = false;
    if (s.out.is_open())
        s.out.close();
}

void ps2xGb7c4SetPacketContext(uint64_t tick, uint64_t packetIndex, unsigned path)
{
    Gb7c4ProbeState &s = gb7c4State();
    s.ctx.tick = tick;
    s.ctx.packetIndex = packetIndex;
    s.ctx.path = path;
    s.nextBatch = 0;
}

bool ps2xGb7c4ProbeEnabled()
{
    return gb7c4State().enabled;
}

// GB7C5 first-displayed-glyph-pixel writer watch (default OFF).
// Watched pixel A from GB7C4: displayed (342,377) in FBP112/FBW8/PSMCT24,
// swizzled storage word byte address 0x0019ae38 (GSPSMCT32::addrPSMCT32
// with block = 112<<5 = 3584, width 8; CT24 shares the C32 page tables).
// Expected displayed word 0x00353341. The watch sits in
// WriteVramUnlocked, the single funnel for draw pixels, host-to-local
// uploads, local-to-local transfers, clears and direct VRAM writes, so a
// change by any path is attributed with the current op tag; a word that
// changes with no logged row is impossible while the probe is open and
// uncapped (cap status is reported, and the checker returns OTHER when
// capped). Rendering is untouched: one enabled branch plus two 4-byte
// loads per write, and log writes only on an actual watched-word change.
namespace
{
struct Gb7c5ProbeState
{
    bool enabled = false;
    std::ofstream out;
    Gb7c5PacketContext ctx{};
    uint64_t nextBatch = 0;
    uint64_t curBatch = 0;
    uint64_t seq = 0;
    uint64_t rows = 0;
    uint64_t bytes = 0;
    bool capped = false;
    bool startLogged = false;
    bool preLogged = false;
    bool postLogged = false;
    // Live VRAM base registered at Initialize (replay is single-threaded;
    // snapshots read the watched word with memcpy, no lock needed).
    const uint8_t *vram = nullptr;
    uint32_t vramSize = 0;
    char op[16] = "-";
    char detail[256] = "-";
};

Gb7c5ProbeState &gb7c5State()
{
    static Gb7c5ProbeState s;
    return s;
}

constexpr uint32_t kGb7c5WatchAddr = 0x0019ae38u;
constexpr uint64_t kGb7c5MaxRows = 20000u;
constexpr uint64_t kGb7c5MaxBytes = 8u * 1024u * 1024u;

uint32_t gb7c5WatchWord()
{
    Gb7c5ProbeState &s = gb7c5State();
    if (s.vram == nullptr || s.vramSize < kGb7c5WatchAddr + 4u)
        return 0u;
    uint32_t word = 0u;
    std::memcpy(&word, s.vram + kGb7c5WatchAddr, sizeof(word));
    return word;
}

void gb7c5Emit(uint32_t x, uint32_t y, uint32_t psm, uint32_t base, uint32_t bw,
               uint32_t oldV, uint32_t newV, const char *kind)
{
    Gb7c5ProbeState &s = gb7c5State();
    if (!s.enabled || s.capped)
        return;
    char line[1024];
    const int count = std::snprintf(
        line, sizeof(line), "%llu\t%llu\t%llu\t%u\t%llu\t%s\t%08x\t%u\t%u\t%u\t%u\t%u\t%08x\t%08x\t%s\t%s\n",
        static_cast<unsigned long long>(s.seq),
        static_cast<unsigned long long>(s.ctx.tick),
        static_cast<unsigned long long>(s.ctx.packetIndex), s.ctx.path,
        static_cast<unsigned long long>(s.curBatch),
        s.op, kGb7c5WatchAddr, x, y, psm, base, bw, oldV, newV, kind, s.detail);
    if (count <= 0 || static_cast<size_t>(count) >= sizeof(line))
        return;
    if (s.rows >= kGb7c5MaxRows || s.bytes + static_cast<uint64_t>(count) > kGb7c5MaxBytes)
    {
        s.capped = true;
        return;
    }
    s.out << line;
    if (!s.out.good())
    {
        s.capped = true;
        return;
    }
    ++s.seq;
    ++s.rows;
    s.bytes += static_cast<uint64_t>(count);
}
} // namespace

void ps2xGb7c5ProbeOpen(const char *path)
{
    Gb7c5ProbeState &s = gb7c5State();
    if (s.enabled || !path || !*path)
        return;
    s.out.open(path, std::ios::binary | std::ios::trunc);
    if (!s.out)
        return;
    s.out << "seq\ttick\tpacket\tpath\tbatch\top\taddr\tx\ty\tpsm\tbase\tbw\told\tnew\tkind\tdetail\n";
    s.bytes += 68u;
    s.ctx = Gb7c5PacketContext{};
    s.nextBatch = 0;
    s.curBatch = 0;
    s.seq = 0;
    s.rows = 0;
    s.capped = false;
    s.startLogged = false;
    s.preLogged = false;
    s.postLogged = false;
    std::snprintf(s.op, sizeof(s.op), "-");
    std::snprintf(s.detail, sizeof(s.detail), "-");
    s.enabled = s.out.good();
}

void ps2xGb7c5ProbeClose()
{
    Gb7c5ProbeState &s = gb7c5State();
    if (s.enabled)
    {
        std::cout << "GB7C5_SUMMARY rows=" << s.rows << " bytes=" << s.bytes
                  << " capped=" << (s.capped ? 1 : 0) << '\n';
    }
    s.enabled = false;
    if (s.out.is_open())
        s.out.close();
}

void ps2xGb7c5SetPacketContext(uint64_t tick, uint64_t packetIndex, unsigned path)
{
    Gb7c5ProbeState &s = gb7c5State();
    s.ctx.tick = tick;
    s.ctx.packetIndex = packetIndex;
    s.ctx.path = path;
    s.nextBatch = 0;
    if (!s.enabled || s.capped)
        return;
    if (packetIndex == 0u && !s.startLogged)
    {
        s.startLogged = true;
        const uint32_t word = gb7c5WatchWord();
        gb7c5Emit(342u, 377u, 1u, 3584u, 8u, word, word, "snapshot-start");
    }
    if (packetIndex == 47240u && !s.preLogged)
    {
        s.preLogged = true;
        const uint32_t word = gb7c5WatchWord();
        gb7c5Emit(342u, 377u, 1u, 3584u, 8u, word, word, "pre-47240");
    }
}

void ps2xGb7c5NotePacket47240Done()
{
    Gb7c5ProbeState &s = gb7c5State();
    if (!s.enabled || s.capped || s.postLogged)
        return;
    s.postLogged = true;
    const uint32_t word = gb7c5WatchWord();
    gb7c5Emit(342u, 377u, 1u, 3584u, 8u, word, word, "post-47240");
}

bool ps2xGb7c5ProbeEnabled()
{
    return gb7c5State().enabled;
}

void ps2xGb7c5RegisterVram(const uint8_t *vram, uint32_t vramSize)
{
    Gb7c5ProbeState &s = gb7c5State();
    s.vram = vram;
    s.vramSize = vramSize;
}

uint64_t GSCpuBackend::NoteGb7c5BatchBegin(const GSPrimitiveBatch &batch)
{
    Gb7c5ProbeState &s = gb7c5State();
    const uint64_t batchId = s.nextBatch++;
    s.curBatch = batchId;
    if (!s.enabled || s.capped)
        return batchId;
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    const char *primName = "other";
    switch (state.prim.type)
    {
    case GS_PRIM_SPRITE:
        primName = "sprite";
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        primName = "triangle";
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        primName = "line";
        break;
    case GS_PRIM_POINT:
        primName = "point";
        break;
    default:
        break;
    }
    std::snprintf(s.op, sizeof(s.op), "draw");
    std::snprintf(s.detail, sizeof(s.detail),
                  "prim=%s frame=fbp%u,fbw%u,psm%u,fbmsk=0x%08x",
                  primName, ctx.frame.fbp, static_cast<unsigned>(ctx.frame.fbw),
                  static_cast<unsigned>(ctx.frame.psm), ctx.frame.fbmsk);
    return batchId;
}

void GSCpuBackend::NoteGb7c5UploadOp()
{
    Gb7c5ProbeState &s = gb7c5State();
    if (!s.enabled || s.capped)
        return;
    std::snprintf(s.op, sizeof(s.op), "upload");
    std::snprintf(s.detail, sizeof(s.detail),
                  "dst=dpsm%u,dbp%u,dbw%u rrw%u,rrh%u dsax%u,dsay%u",
                  static_cast<unsigned>(m_transfer.bitbltbuf.dpsm),
                  m_transfer.bitbltbuf.dbp,
                  static_cast<unsigned>(m_transfer.bitbltbuf.dbw),
                  m_transfer.trxreg.rrw, m_transfer.trxreg.rrh,
                  m_transfer.trxpos.dsax, m_transfer.trxpos.dsay);
}

void GSCpuBackend::NoteGb7c5LocalToLocalOp()
{
    Gb7c5ProbeState &s = gb7c5State();
    if (!s.enabled || s.capped)
        return;
    std::snprintf(s.op, sizeof(s.op), "ll");
    std::snprintf(s.detail, sizeof(s.detail),
                  "src=spsm%u,sbp%u,sbw%u dst=dpsm%u,dbp%u,dbw%u rrw%u,rrh%u ssax%u,ssay%u dsax%u,dsay%u",
                  static_cast<unsigned>(m_transfer.bitbltbuf.spsm),
                  m_transfer.bitbltbuf.sbp,
                  static_cast<unsigned>(m_transfer.bitbltbuf.sbw),
                  static_cast<unsigned>(m_transfer.bitbltbuf.dpsm),
                  m_transfer.bitbltbuf.dbp,
                  static_cast<unsigned>(m_transfer.bitbltbuf.dbw),
                  m_transfer.trxreg.rrw, m_transfer.trxreg.rrh,
                  m_transfer.trxpos.ssax, m_transfer.trxpos.ssay,
                  m_transfer.trxpos.dsax, m_transfer.trxpos.dsay);
}

void GSCpuBackend::NoteGb7c5ClearOp(const GSContext &context, uint32_t rgba)
{
    Gb7c5ProbeState &s = gb7c5State();
    if (!s.enabled || s.capped)
        return;
    std::snprintf(s.op, sizeof(s.op), "clear");
    std::snprintf(s.detail, sizeof(s.detail),
                  "frame=fbp%u,fbw%u,psm%u rgba=0x%08x",
                  context.frame.fbp, static_cast<unsigned>(context.frame.fbw),
                  static_cast<unsigned>(context.frame.psm), rgba);
}

void GSCpuBackend::NoteGb7c5DirectOp(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
{
    Gb7c5ProbeState &s = gb7c5State();
    if (!s.enabled || s.capped)
        return;
    std::snprintf(s.op, sizeof(s.op), "direct");
    std::snprintf(s.detail, sizeof(s.detail),
                  "psm%u,base%u,bw%u xy=(%u,%u)",
                  psm, base, bw, x, y);
}

bool ps2xDeinterlaceBobValue(const char *value)
{
    if (value == nullptr || value[0] == '\0')
        return false;
    return std::strcmp(value, "bob") == 0;
}

uint32_t ps2xDeinterlaceSourceLine(uint32_t y, uint32_t height, bool oddField, bool bob)
{
    if (!bob)
        return y;
    uint32_t sourceY = ((y >> 1u) << 1u) + (oddField ? 1u : 0u);
    if (sourceY >= height)
        sourceY = height - 1u;
    return sourceY;
}

namespace
{
    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0) & 0xFF) >> 3;
        uint32_t g = ((c >> 8) & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    u32 Rgba5551ToRgba8888(u16 c)
    {
        u32 r = ((c >> 0) & 0x1F) << 3;
        u32 g = ((c >> 5) & 0x1F) << 3;
        u32 b = ((c >> 10) & 0x1F) << 3;
        u32 a = ((c >> 15) & 0x01) << 7;

        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};

    int wrapTextureCoordinate(int coordinate,
                              int textureSize,
                              uint8_t mode,
                              uint16_t regionMin,
                              uint16_t regionMax)
    {
        switch (mode & 0x3u)
        {
        case 0: // REPEAT
            return static_cast<int>(static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(textureSize - 1));
        case 1: // CLAMP
            return clampInt(coordinate, 0, textureSize - 1);
        case 2: // REGION_CLAMP
            return std::min(std::max(coordinate, static_cast<int>(regionMin)), static_cast<int>(regionMax));
        case 3: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(regionMin)) | static_cast<uint32_t>(regionMax));
        default:
            return coordinate;
        }
    }

    bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct PixelWriteMask
    {
        bool writeRgb = true;
        bool writeAlpha = true;
        bool writeDepth = true;

        bool writesFramebuffer() const
        {
            return writeRgb || writeAlpha;
        }

        bool writesAnything() const
        {
            return writesFramebuffer() || writeDepth;
        }
    };

    PixelWriteMask classifyAlphaTest(uint64_t testReg, uint8_t alpha, uint8_t framePsm)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, true, false};
        case 2: // ZB_ONLY
            return {false, false, true};
        case 3: // RGB_ONLY
            // RGB_ONLY is only distinct for RGBA32. The GS treats it as
            // FB_ONLY for RGB24 and RGBA16 framebuffers.
            if (framePsm == GS_PSM_CT32)
                return {true, false, false};
            return {true, true, false};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    bool passesDestinationAlphaTest(uint64_t testReg, uint8_t framePsm, uint32_t rawFramebufferPixel)
    {
        const bool date = ((testReg >> 14) & 0x1u) != 0u;
        if (!date)
            return true;

        const bool datm = ((testReg >> 15) & 0x1u) != 0u;
        switch (framePsm)
        {
        case GS_PSM_CT32:
            return (((rawFramebufferPixel >> 31) & 0x1u) != 0u) == datm;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return (((rawFramebufferPixel >> 15) & 0x1u) != 0u) == datm;
        case GS_PSM_CT24:
            // RGB24 has no destination alpha, so DATE always passes.
            return true;
        default:
            return true;
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        // CSM1 swaps address bits 3 and 4. Preserve the remaining bits:
        // 16-bit CLUTs expose a ninth address bit through CSA[4].
        return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    // TODO: clut cache
    uint32_t resolveClutIndex(uint8_t index, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm)
    {
        uint32_t clutIndex = static_cast<uint32_t>(index);

        // CSM2 addresses the source directly through TEXCLUT. CSA is required
        // to be zero there, so it must not offset the source coordinates.
        if (csm != 0u)
            return (sourcePsm == GS_PSM_T4 ||
                    sourcePsm == GS_PSM_T4HH ||
                    sourcePsm == GS_PSM_T4HL)
                       ? (clutIndex & 0x0Fu)
                       : clutIndex;

        const bool is16BitClut = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
        const uint32_t csaMask = is16BitClut ? 0x1Fu : 0x0Fu;
        const uint32_t clutIndexMask = is16BitClut ? 0x1FFu : 0x0FFu;
        const uint32_t clutBase = (static_cast<uint32_t>(csa) & csaMask) << 4u;

        switch (sourcePsm)
        {
        case GS_PSM_T4:
        case GS_PSM_T4HH:
        case GS_PSM_T4HL:
            clutIndex = clutBase + (clutIndex & 0x0Fu);
            break;
        case GS_PSM_T8:
        case GS_PSM_T8H:
            clutIndex = clutBase + clutIndex;
            break;
        default:
            return clutIndex;
        }

        return swizzleClutIndexCSM1(clutIndex & clutIndexMask);
    }

    uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        return clampU8(static_cast<int>(std::lround(top + (bottom - top) * fy)));
    }
}

namespace
{
    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;
        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }
        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        return {
            static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu),
            static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu)};
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        return {
            (pmode64 & 0x1ull) != 0ull,
            (pmode64 & 0x2ull) != 0ull,
            ((pmode64 >> 5) & 0x1ull) != 0ull,
            ((pmode64 >> 6) & 0x1ull) != 0ull,
            ((pmode64 >> 7) & 0x1ull) != 0ull,
            static_cast<uint8_t>((pmode64 >> 8) & 0xFFu)};
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode2)
    {
        return {(smode2 & 0x1ull) != 0ull, ((smode2 >> 1) & 0x1ull) != 0ull};
    }

    // PS2X_DEINTERLACE reader. Default "weave": the value must be exactly
    // "bob" to select the historical field-doubling presentation. Test hooks
    // below override the cached env read; production never calls them.
    std::optional<bool> &deinterlaceBobTestOverride()
    {
        static std::optional<bool> override;
        return override;
    }

    bool deinterlaceBobEnabled()
    {
        if (deinterlaceBobTestOverride().has_value())
            return *deinterlaceBobTestOverride();
        static const bool bob = [] {
            if (const char *env = std::getenv("PS2X_DEINTERLACE"))
            {
                return ps2xDeinterlaceBobValue(env);
            }
            return false;
        }();
        return bob;
    }

    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        if (pixels.empty() || width == 0u || height < 2u)
            return;
        const std::vector<uint8_t> source = pixels;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t sourceY = ps2xDeinterlaceSourceLine(y, height, oddField, true);
            std::memcpy(pixels.data() + y * kHostFrameWidth * 4u,
                        source.data() + sourceY * kHostFrameWidth * 4u,
                        width * 4u);
        }
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
                row[x * 4u + 3u] = 255u;
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
            {
                if (row[x * 4u] != 0u || row[x * 4u + 1u] != 0u || row[x * 4u + 2u] != 0u)
                    ++count;
            }
        }
        return count;
    }
}

void ps2xSetDeinterlaceBobForTest(bool bob)
{
    deinterlaceBobTestOverride() = bob;
}

void ps2xClearDeinterlaceBobForTest()
{
    deinterlaceBobTestOverride().reset();
}

GSCpuBackend::GSCpuBackend()
{
    using namespace GSMem;
    static std::once_flag lookupTablesOnce;
    std::call_once(lookupTablesOnce, []()
                   { InitLookupTables(); });
    for (size_t i = 0; i < kPsmHandlerCount; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_readVramFuncs[i] = ReadCT32;
            m_writeVramFuncs[i] = WriteCT32;
            break;
        case GS_PSM_CT24:
            m_readVramFuncs[i] = ReadCT24;
            m_writeVramFuncs[i] = WriteCT24;
            break;
        case GS_PSM_CT16:
            m_readVramFuncs[i] = ReadCT16;
            m_writeVramFuncs[i] = WriteCT16;
            break;
        case GS_PSM_CT16S:
            m_readVramFuncs[i] = ReadCT16S;
            m_writeVramFuncs[i] = WriteCT16S;
            break;
        case GS_PSM_T8:
            m_readVramFuncs[i] = ReadP8;
            m_writeVramFuncs[i] = WriteP8;
            break;
        case GS_PSM_T8H:
            m_readVramFuncs[i] = ReadP8H;
            m_writeVramFuncs[i] = WriteP8H;
            break;
        case GS_PSM_T4:
            m_readVramFuncs[i] = ReadP4;
            m_writeVramFuncs[i] = WriteP4;
            break;
        case GS_PSM_T4HH:
            m_readVramFuncs[i] = ReadP4HH;
            m_writeVramFuncs[i] = WriteP4HH;
            break;
        case GS_PSM_T4HL:
            m_readVramFuncs[i] = ReadP4HL;
            m_writeVramFuncs[i] = WriteP4HL;
            break;
        case GS_PSM_Z32:
            m_readVramFuncs[i] = ReadZ32;
            m_writeVramFuncs[i] = WriteZ32;
            break;
        case GS_PSM_Z24:
            m_readVramFuncs[i] = ReadZ24;
            m_writeVramFuncs[i] = WriteZ24;
            break;
        case GS_PSM_Z16:
            m_readVramFuncs[i] = ReadZ16;
            m_writeVramFuncs[i] = WriteZ16;
            break;
        case GS_PSM_Z16S:
            m_readVramFuncs[i] = ReadZ16S;
            m_writeVramFuncs[i] = WriteZ16S;
            break;
        default:
            m_readVramFuncs[i] = ReadNull;
            m_writeVramFuncs[i] = WriteNull;
            break;
        }
    }
    Reset();
}

void GSCpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vram = vram;
    m_vramSize = vramSize;
    ps2xGb7c5RegisterVram(vram, vramSize);
    ResetUnlocked();
}

void GSCpuBackend::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ResetUnlocked();
}

void GSCpuBackend::ResetUnlocked()
{
    m_transfer = {};
    m_transfer.direction = 3u;
    m_transferState = {};
    m_transferState.direction = 3u;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
}

void GSCpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || batch.vertexCount == 0u)
        return;
    DrawPrimitive(batch);
}

void GSCpuBackend::Flush()
{
    // CPU backend is immediate. GPU backends may submit command buffers here.
}

void GSCpuBackend::TextureFlush()
{
    // CPU texture reads are coherent with local memory. Future cached/GPU
    // backends use this boundary to invalidate texture views.
}

void GSCpuBackend::Sync(GSSyncReason)
{
    // CPU backend is immediate. GPU backends may wait on fences/readbacks here.
}

uint32_t GSCpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return ReadVramUnlocked(psm, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (!m_vram)
        return 0u;
    return m_readVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y);
}

void GSCpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (gb7c5State().enabled)
        NoteGb7c5DirectOp(psm, base, bw, x, y);
    WriteVramUnlocked(psm, base, bw, x, y, value);
}

void GSCpuBackend::WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (!m_vram)
        return;
    // GB7C5 writer watch: compare the actual storage word before/after.
    // Single funnel for draw, upload, local-to-local, clear and direct
    // writes. OFF path is one enabled branch; no render effect.
    Gb7c5ProbeState &watch = gb7c5State();
    if (watch.enabled && !watch.capped && m_vramSize > kGb7c5WatchAddr + 4u)
    {
        uint32_t before = 0u;
        std::memcpy(&before, m_vram + kGb7c5WatchAddr, sizeof(before));
        m_writeVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y, value);
        uint32_t after = 0u;
        std::memcpy(&after, m_vram + kGb7c5WatchAddr, sizeof(after));
        if (before != after)
            gb7c5Emit(x, y, psm, base, bw, before, after, "change");
        return;
    }
    m_writeVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y, value);
}

void GSCpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || m_vramSize == 0u)
    {
        out.clear();
        return;
    }
    out.resize(m_vramSize);
    std::memcpy(out.data(), m_vram, m_vramSize);
}

GSTransferSnapshot GSCpuBackend::GetTransferSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    GSTransferSnapshot result = m_transferState;
    result.localToHostPendingBytes = m_localToHostReadPos < m_localToHostBuffer.size()
                                         ? m_localToHostBuffer.size() - m_localToHostReadPos
                                         : 0u;
    return result;
}

void GSCpuBackend::NoteGb7bCandidate(const GSPrimitiveBatch &batch)
{
    Gb7bProbeState &s = gb7bState();
    if (!s.enabled || s.capped)
        return;
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    if (batch.vertexCount == 0u)
        return;

    // Clipped screen rect: exact for sprites (mirrors DrawSprite), clamped
    // vertex bbox otherwise (conservative for triangles/lines/points).
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    const char *rectKind = "bbox";
    const char *primName = "other";
    switch (state.prim.type)
    {
    case GS_PRIM_SPRITE:
        primName = "sprite";
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        primName = "triangle";
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        primName = "line";
        break;
    default:
        primName = "point";
        break;
    }
    if (state.prim.type == GS_PRIM_SPRITE && batch.vertexCount >= 2u)
    {
        const int ofx = ctx.xyoffset.ofx >> 4;
        const int ofy = ctx.xyoffset.ofy >> 4;
        int ax0 = static_cast<int>(batch.vertices[0].x) - ofx;
        int ay0 = static_cast<int>(batch.vertices[0].y) - ofy;
        int ax1 = static_cast<int>(batch.vertices[1].x) - ofx;
        int ay1 = static_cast<int>(batch.vertices[1].y) - ofy;
        if (ax0 > ax1)
            std::swap(ax0, ax1);
        if (ay0 > ay1)
            std::swap(ay0, ay1);
        const int spanX = std::max(1, ax1 - ax0);
        const int spanY = std::max(1, ay1 - ay0);
        ax1 = ax0 + spanX - 1;
        ay1 = ay0 + spanY - 1;
        if (ax1 < ctx.scissor.x0 || ax0 > ctx.scissor.x1 ||
            ay1 < ctx.scissor.y0 || ay0 > ctx.scissor.y1)
            return;
        x0 = clampInt(ax0, ctx.scissor.x0, ctx.scissor.x1);
        y0 = clampInt(ay0, ctx.scissor.y0, ctx.scissor.y1);
        x1 = clampInt(ax1, ctx.scissor.x0, ctx.scissor.x1);
        y1 = clampInt(ay1, ctx.scissor.y0, ctx.scissor.y1);
        rectKind = "exact";
    }
    else
    {
        const int ofx = ctx.xyoffset.ofx >> 4;
        const int ofy = ctx.xyoffset.ofy >> 4;
        float fx0 = batch.vertices[0].x - static_cast<float>(ofx);
        float fy0 = batch.vertices[0].y - static_cast<float>(ofy);
        float fx1 = fx0, fy1 = fy0;
        for (uint8_t i = 1; i < batch.vertexCount && i < 3u; ++i)
        {
            const float fx = batch.vertices[i].x - static_cast<float>(ofx);
            const float fy = batch.vertices[i].y - static_cast<float>(ofy);
            fx0 = std::min(fx0, fx);
            fy0 = std::min(fy0, fy);
            fx1 = std::max(fx1, fx);
            fy1 = std::max(fy1, fy);
        }
        const int bx0 = static_cast<int>(std::floor(fx0));
        const int by0 = static_cast<int>(std::floor(fy0));
        const int bx1 = static_cast<int>(std::ceil(fx1));
        const int by1 = static_cast<int>(std::ceil(fy1));
        if (bx1 < ctx.scissor.x0 || bx0 > ctx.scissor.x1 ||
            by1 < ctx.scissor.y0 || by0 > ctx.scissor.y1)
            return;
        x0 = clampInt(bx0, ctx.scissor.x0, ctx.scissor.x1);
        y0 = clampInt(by0, ctx.scissor.y0, ctx.scissor.y1);
        x1 = clampInt(bx1, ctx.scissor.x0, ctx.scissor.x1);
        y1 = clampInt(by1, ctx.scissor.y0, ctx.scissor.y1);
    }

    const uint64_t upperHit = gb7bOverlap(x0, y0, x1, y1,
                                          kGb7bUpperX0, kGb7bUpperY0,
                                          kGb7bUpperX1 - 1, kGb7bUpperY1 - 1);
    const uint64_t lowerHit = gb7bOverlap(x0, y0, x1, y1,
                                          kGb7bLowerX0, kGb7bLowerY0,
                                          kGb7bLowerX1 - 1, kGb7bLowerY1 - 1);
    if (upperHit == 0u && lowerHit == 0u)
        return;

    // Bounded source-texel fingerprint at the CPU sample site: up to 4
    // vertex/centroid samples through SampleTexture (read-only).
    char fingerprint[64];
    if (!state.prim.tme)
    {
        std::snprintf(fingerprint, sizeof(fingerprint), "none(untextured)");
    }
    else
    {
        uint32_t samples[4] = {0u, 0u, 0u, 0u};
        const uint8_t n = batch.vertexCount > 3u ? 3u : batch.vertexCount;
        for (uint8_t i = 0; i < n; ++i)
        {
            const GSVertex &v = batch.vertices[i];
            if (state.prim.fst)
                samples[i] = SampleTexture(state, 0.0f, 0.0f, 1.0f, v.u, v.v);
            else
                samples[i] = SampleTexture(state, v.s, v.t, v.q, 0u, 0u);
        }
        if (n >= 2u)
        {
            const GSVertex &a = batch.vertices[0];
            const GSVertex &b = batch.vertices[1];
            if (state.prim.fst)
            {
                const uint16_t mu = static_cast<uint16_t>((static_cast<uint32_t>(a.u) + b.u) / 2u);
                const uint16_t mv = static_cast<uint16_t>((static_cast<uint32_t>(a.v) + b.v) / 2u);
                samples[3] = SampleTexture(state, 0.0f, 0.0f, 1.0f, mu, mv);
            }
            else
            {
                samples[3] = SampleTexture(state, (a.s + b.s) * 0.5f,
                                           (a.t + b.t) * 0.5f,
                                           (a.q + b.q) * 0.5f, 0u, 0u);
            }
        }
        else
        {
            samples[3] = samples[0];
        }
        const uint32_t fp = gb7bFnv(reinterpret_cast<const uint8_t *>(samples), sizeof(samples));
        std::snprintf(fingerprint, sizeof(fingerprint), "texel4=%08x", fp);
    }

    const uint64_t rectArea = static_cast<uint64_t>(x1 - x0 + 1) *
                              static_cast<uint64_t>(y1 - y0 + 1);
    const char *classification = "overlap-candidate";
    const char *reason = "textured batch overlaps a title crop; needs pixel-trace "
                         "confirmation, overlap alone is not glyph proof";
    if (!state.prim.tme)
    {
        classification = "untextured-overwrite";
        reason = "untextured batch overlaps a title crop; fade/composite "
                 "overwrite candidate, not a glyph producer";
    }
    else if ((upperHit >= 7650u && rectArea >= 4u * 8500u) ||
             (lowerHit >= 4860u && rectArea >= 4u * 5400u))
    {
        classification = "full-crop-overwrite";
        reason = "textured batch covers >=90% of an overlapped crop with rect "
                 "area >=4x the crop; composite/fade candidate, not a proved "
                 "glyph producer";
    }

    char line[1024];
    const int count = std::snprintf(
        line, sizeof(line),
        "%llu\t%llu\t%u\t%s\t%u\t%u\t"
        "(%d,%d)-(%d,%d)\t%s\tupper=%llu;lower=%llu\t"
        "fbp=%u,fbw=%u,psm=%u\t"
        "tbp0=%u,tbw=%u,psm=%u,tw=%u,th=%u,tcc=%u,tfx=%u,cbp=%u,cpsm=%u,csm=%u,csa=%u\t"
        "clamp=0x%llx\ttest=0x%llx\talpha=0x%llx\t"
        "uv0=(%u,%u);uv1=(%u,%u);stq0=(%g,%g,%g);stq1=(%g,%g,%g)\t%s\t%s\t%s\n",
        static_cast<unsigned long long>(s.ctx.tick),
        static_cast<unsigned long long>(s.ctx.packetIndex), s.ctx.path,
        primName, state.prim.tme ? 1u : 0u, state.prim.fst ? 1u : 0u,
        x0, y0, x1, y1, rectKind, static_cast<unsigned long long>(upperHit),
        static_cast<unsigned long long>(lowerHit),
        ctx.frame.fbp, ctx.frame.fbw, static_cast<unsigned>(ctx.frame.psm),
        ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw),
        static_cast<unsigned>(ctx.tex0.psm), static_cast<unsigned>(ctx.tex0.tw),
        static_cast<unsigned>(ctx.tex0.th), static_cast<unsigned>(ctx.tex0.tcc),
        static_cast<unsigned>(ctx.tex0.tfx), ctx.tex0.cbp,
        static_cast<unsigned>(ctx.tex0.cpsm), static_cast<unsigned>(ctx.tex0.csm),
        static_cast<unsigned>(ctx.tex0.csa),
        static_cast<unsigned long long>(ctx.clamp),
        static_cast<unsigned long long>(ctx.test),
        static_cast<unsigned long long>(ctx.alpha),
        static_cast<unsigned>(batch.vertices[0].u >> 4),
        static_cast<unsigned>(batch.vertices[0].v >> 4),
        batch.vertexCount >= 2u ? static_cast<unsigned>(batch.vertices[1].u >> 4) : 0u,
        batch.vertexCount >= 2u ? static_cast<unsigned>(batch.vertices[1].v >> 4) : 0u,
        batch.vertices[0].s, batch.vertices[0].t, batch.vertices[0].q,
        batch.vertexCount >= 2u ? batch.vertices[1].s : 0.0f,
        batch.vertexCount >= 2u ? batch.vertices[1].t : 0.0f,
        batch.vertexCount >= 2u ? batch.vertices[1].q : 1.0f,
        fingerprint, classification, reason);
    if (count <= 0 || static_cast<size_t>(count) >= sizeof(line))
        return;
    if (s.rows >= kGb7bMaxRows || s.bytes + static_cast<uint64_t>(count) > kGb7bMaxBytes)
    {
        s.capped = true;
        return;
    }
    s.out << line;
    if (!s.out.good())
    {
        s.capped = true;
        return;
    }
    ++s.rows;
    s.bytes += static_cast<uint64_t>(count);
}

uint64_t GSCpuBackend::NoteGb7c2BatchBegin(const GSPrimitiveBatch &batch)
{
    Gb7c2ProbeState &s = gb7c2State();
    const uint64_t batchId = s.nextBatch++;
    s.curBatch = batchId;
    s.curKind = 0;
    s.curPicks = 0;
    s.curLogged = 0;
    if (!s.enabled || s.capped)
        return batchId;
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    const bool isSprite = state.prim.type == GS_PRIM_SPRITE && batch.vertexCount >= 2u;
    const bool isC1 = isSprite && !state.prim.fst &&
                      s.ctx.tick == 600u && s.ctx.packetIndex == 47176u && s.ctx.path == 2u &&
                      ctx.frame.fbp == 0u && ctx.tex0.tbp0 == 11017u;
    const bool isCarrier = isSprite && state.prim.fst && state.prim.tme &&
                           s.ctx.tick == 601u && s.ctx.packetIndex == 47240u && s.ctx.path == 3u &&
                           ctx.frame.fbp == 112u && ctx.tex0.tbp0 == 0u;
    if (!isC1 && !isCarrier)
        return batchId;
    s.curKind = isC1 ? 1 : 2;

    // Clipped sprite rect (mirrors DrawSprite).
    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    int ax0 = static_cast<int>(batch.vertices[0].x) - ofx;
    int ay0 = static_cast<int>(batch.vertices[0].y) - ofy;
    int ax1 = static_cast<int>(batch.vertices[1].x) - ofx;
    int ay1 = static_cast<int>(batch.vertices[1].y) - ofy;
    if (ax0 > ax1)
        std::swap(ax0, ax1);
    if (ay0 > ay1)
        std::swap(ay0, ay1);
    const int spanX = std::max(1, ax1 - ax0);
    const int spanY = std::max(1, ay1 - ay0);
    ax1 = ax0 + spanX - 1;
    ay1 = ay0 + spanY - 1;
    const int x0 = clampInt(ax0, ctx.scissor.x0, ctx.scissor.x1);
    const int y0 = clampInt(ay0, ctx.scissor.y0, ctx.scissor.y1);
    const int x1 = clampInt(ax1, ctx.scissor.x0, ctx.scissor.x1);
    const int y1 = clampInt(ay1, ctx.scissor.y0, ctx.scissor.y1);

    char detail[512];
    std::snprintf(detail, sizeof(detail),
                  "rect=(%d,%d)-(%d,%d) fst=%u tme=%u frame=fbp%u,fbw%u,psm%u "
                  "tex=tbp0-%u,tbw%u,psm%u,tw%u,th%u,tcc%u,tfx%u,cbp%u,cpsm%u,csm%u,csa%u "
                  "test=0x%llx alpha=0x%llx",
                  x0, y0, x1, y1, state.prim.fst ? 1u : 0u, state.prim.tme ? 1u : 0u,
                  ctx.frame.fbp, static_cast<unsigned>(ctx.frame.fbw),
                  static_cast<unsigned>(ctx.frame.psm),
                  ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw),
                  static_cast<unsigned>(ctx.tex0.psm), static_cast<unsigned>(ctx.tex0.tw),
                  static_cast<unsigned>(ctx.tex0.th), static_cast<unsigned>(ctx.tex0.tcc),
                  static_cast<unsigned>(ctx.tex0.tfx), ctx.tex0.cbp,
                  static_cast<unsigned>(ctx.tex0.cpsm), static_cast<unsigned>(ctx.tex0.csm),
                  static_cast<unsigned>(ctx.tex0.csa),
                  static_cast<unsigned long long>(ctx.test),
                  static_cast<unsigned long long>(ctx.alpha));
    gb7c2Emit("-", "-", "-", "-", detail, "-", "-",
              isC1 ? "batch-c1" : "batch-carrier");

    if (isC1)
    {
        // Fixed interior-pixel set: four corners, then center; deduped.
        const int cx = (x0 + x1) / 2;
        const int cy = (y0 + y1) / 2;
        const int candX[5] = {x0, x1, x0, x1, cx};
        const int candY[5] = {y0, y0, y1, y1, cy};
        for (int i = 0; i < 5 && s.curPicks < kGb7c2MaxPicks; ++i)
        {
            bool dup = false;
            for (int j = 0; j < s.curPicks; ++j)
            {
                if (s.pickX[j] == candX[i] && s.pickY[j] == candY[i])
                {
                    dup = true;
                    break;
                }
            }
            if (!dup)
            {
                s.pickX[s.curPicks] = candX[i];
                s.pickY[s.curPicks] = candY[i];
                ++s.curPicks;
            }
        }
        // Before snapshot of the off-screen ROI at this batch's frame format.
        s.roiFbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
        s.roiFbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
        s.roiPsm = ctx.frame.psm;
        const int w = kGb7c2RoiX1 - kGb7c2RoiX0 + 1;
        const int h = kGb7c2RoiY1 - kGb7c2RoiY0 + 1;
        s.roiBefore.assign(static_cast<size_t>(w * h), 0u);
        for (int yy = 0; yy < h; ++yy)
        {
            for (int xx = 0; xx < w; ++xx)
            {
                s.roiBefore[static_cast<size_t>(yy * w + xx)] =
                    ReadVramUnlocked(s.roiPsm, s.roiFbp, s.roiFbw,
                                     kGb7c2RoiX0 + xx, kGb7c2RoiY0 + yy);
            }
        }
        return batchId;
    }
    // Carrier batch: re-read the ROI and compare against the last C1
    // after-image. Any change means an intervening writer (unknown).
    const int w = kGb7c2RoiX1 - kGb7c2RoiX0 + 1;
    const int h = kGb7c2RoiY1 - kGb7c2RoiY0 + 1;
    char link[512];
    if (!s.lastC1Valid || s.lastC1After.size() != static_cast<size_t>(w * h))
    {
        std::snprintf(link, sizeof(link), "c1=none");
        gb7c2Emit("-", "-", "-", "-", "-", link, "-", "roi-link-no-c1-unknown");
        return batchId;
    }
    uint64_t changed = 0;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    bool first = true;
    for (int yy = 0; yy < h; ++yy)
    {
        for (int xx = 0; xx < w; ++xx)
        {
            const uint32_t now = ReadVramUnlocked(s.roiPsm, s.roiFbp, s.roiFbw,
                                                  kGb7c2RoiX0 + xx, kGb7c2RoiY0 + yy);
            if (now != s.lastC1After[static_cast<size_t>(yy * w + xx)])
            {
                ++changed;
                if (first)
                {
                    bx0 = bx1 = kGb7c2RoiX0 + xx;
                    by0 = by1 = kGb7c2RoiY0 + yy;
                    first = false;
                }
                else
                {
                    bx0 = std::min(bx0, kGb7c2RoiX0 + xx);
                    by0 = std::min(by0, kGb7c2RoiY0 + yy);
                    bx1 = std::max(bx1, kGb7c2RoiX0 + xx);
                    by1 = std::max(by1, kGb7c2RoiY0 + yy);
                }
            }
        }
    }
    if (changed == 0u)
    {
        std::snprintf(link, sizeof(link), "c1hash=%08x equal=1 changed=0",
                      s.lastC1Hash);
        gb7c2Emit("-", "-", "-", "-", "-", link, "-", "roi-link-equal");
    }
    else
    {
        const uint64_t area = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
        const bool fullCover = changed * 10u >= area * 9u;
        std::snprintf(link, sizeof(link),
                      "c1hash=%08x equal=0 changed=%llu bbox=(%d,%d)-(%d,%d)",
                      s.lastC1Hash, static_cast<unsigned long long>(changed),
                      bx0, by0, bx1, by1);
        gb7c2Emit("-", "-", "-", "-", "-", link, "-",
                  fullCover ? "roi-link-changed-full-cover-intervened-unknown"
                            : "roi-link-changed-unknown-writer");
    }
    return batchId;
}

void GSCpuBackend::NoteGb7c2BatchEnd(const GSPrimitiveBatch &batch, uint64_t batchId)
{
    (void)batch;
    (void)batchId;
    Gb7c2ProbeState &s = gb7c2State();
    if (!s.enabled || s.capped || s.curKind != 1)
        return;
    // After snapshot + diff of the off-screen ROI for this C1 batch.
    const int w = kGb7c2RoiX1 - kGb7c2RoiX0 + 1;
    const int h = kGb7c2RoiY1 - kGb7c2RoiY0 + 1;
    if (s.roiBefore.size() != static_cast<size_t>(w * h))
        return;
    std::vector<uint32_t> after(static_cast<size_t>(w * h), 0u);
    for (int yy = 0; yy < h; ++yy)
    {
        for (int xx = 0; xx < w; ++xx)
        {
            after[static_cast<size_t>(yy * w + xx)] =
                ReadVramUnlocked(s.roiPsm, s.roiFbp, s.roiFbw,
                                 kGb7c2RoiX0 + xx, kGb7c2RoiY0 + yy);
        }
    }
    uint64_t changed = 0;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    bool first = true;
    char coords[256] = "";
    size_t coordLen = 0;
    int coordCount = 0;
    for (int yy = 0; yy < h; ++yy)
    {
        for (int xx = 0; xx < w; ++xx)
        {
            const size_t i = static_cast<size_t>(yy * w + xx);
            if (after[i] != s.roiBefore[i])
            {
                ++changed;
                const int px = kGb7c2RoiX0 + xx;
                const int py = kGb7c2RoiY0 + yy;
                if (first)
                {
                    bx0 = bx1 = px;
                    by0 = by1 = py;
                    first = false;
                }
                else
                {
                    bx0 = std::min(bx0, px);
                    by0 = std::min(by0, py);
                    bx1 = std::max(bx1, px);
                    by1 = std::max(by1, py);
                }
                if (coordCount < 8)
                {
                    char cell[32];
                    std::snprintf(cell, sizeof(cell), "%s(%d,%d)",
                                  coordCount == 0 ? "" : ";", px, py);
                    const size_t n = std::strlen(cell);
                    if (coordLen + n < sizeof(coords))
                    {
                        std::memcpy(coords + coordLen, cell, n + 1);
                        coordLen += n;
                        ++coordCount;
                    }
                }
            }
        }
    }
    s.lastC1After = after;
    s.lastC1Hash = gb7bFnv(reinterpret_cast<const uint8_t *>(after.data()),
                           after.size() * sizeof(uint32_t));
    s.lastC1Valid = true;
    char diff[512];
    const uint64_t area = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
    if (changed == 0u)
    {
        std::snprintf(diff, sizeof(diff), "changed=0");
        gb7c2Emit("-", "-", "-", "-", "-", diff, "-", "roi-clean");
    }
    else
    {
        const bool fullCover = changed * 10u >= area * 9u;
        std::snprintf(diff, sizeof(diff),
                      "changed=%llu bbox=(%d,%d)-(%d,%d) coords=%s",
                      static_cast<unsigned long long>(changed), bx0, by0, bx1, by1, coords);
        gb7c2Emit("-", "-", "-", "-", "-", diff, "-",
                  fullCover ? "roi-full-cover-intervened-unknown" : "roi-changed");
    }
}

void GSCpuBackend::TraceGb7c2Pixel(const GSDrawState &state, int x, int y, uint32_t z,
                                   int su, int sv, int wu, int wv,
                                   uint32_t texel, uint32_t rawTex, int clutIndex,
                                   uint8_t combR, uint8_t combG, uint8_t combB, uint8_t combA,
                                   uint32_t oldVal)
{
    Gb7c2ProbeState &s = gb7c2State();
    if (!s.enabled || s.capped || s.curKind == 0)
        return;
    const auto &ctx = state.context;
    const uint32_t fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
    const uint32_t fpsm = ctx.frame.psm;
    // New value after the (already executed) WritePixel. The probe's own
    // old read is independent and unconditional; the WritePixel internal
    // read is conditional on frmw and is not used here.
    const uint32_t newVal = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
    // TEST/ALPHA outcome evaluated on the independent old value.
    const PixelWriteMask wm = classifyAlphaTest(ctx.test, combA, static_cast<uint8_t>(fpsm));
    char outcome[64];
    if (!wm.writesFramebuffer())
    {
        std::snprintf(outcome, sizeof(outcome), "%s",
                      wm.writeDepth ? "depth-only-no-fb-write" : "test-rejected:no-write");
    }
    else if (!passesDestinationAlphaTest(ctx.test, static_cast<uint8_t>(fpsm), oldVal))
    {
        std::snprintf(outcome, sizeof(outcome), "test-rejected:dest-alpha");
    }
    else
    {
        const uint32_t zMethod = static_cast<uint32_t>((ctx.test >> 17) & 3u);
        bool zpass = false;
        if (zMethod == 0u)
            zpass = false;
        else if (zMethod == 1u)
            zpass = true;
        else
        {
            const uint32_t zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
            const uint32_t storedZ = ReadVramUnlocked(ctx.zbuf.psm, zbp, fbw, x, y);
            zpass = (zMethod == 2u) ? (z >= storedZ) : (z > storedZ);
        }
        if (!zpass)
            std::snprintf(outcome, sizeof(outcome), "test-rejected:ztest");
        else if (newVal != oldVal)
            std::snprintf(outcome, sizeof(outcome), "traced-write");
        else
            std::snprintf(outcome, sizeof(outcome), "write-same-value-unknown");
    }
    char srcXy[64], dstXy[64], oldS[16], newS[16], texS[320], cls[160];
    std::snprintf(srcXy, sizeof(srcXy), "(%d,%d)", su, sv);
    std::snprintf(dstXy, sizeof(dstXy), "(%d,%d)", x, y);
    std::snprintf(oldS, sizeof(oldS), "%08x", oldVal);
    std::snprintf(newS, sizeof(newS), "%08x", newVal);
    if (clutIndex >= 0)
    {
        std::snprintf(texS, sizeof(texS),
                      "fst=%u lin=%u wrap=(%d,%d) raw=%08x nibble=%x clut=%d rgba=%08x "
                      "test=0x%llx alpha=0x%llx",
                      state.prim.fst ? 1u : 0u, state.linearFilter ? 1u : 0u,
                      wu, wv, rawTex, rawTex & 0xFu, clutIndex, texel,
                      static_cast<unsigned long long>(ctx.test),
                      static_cast<unsigned long long>(ctx.alpha));
    }
    else
    {
        std::snprintf(texS, sizeof(texS),
                      "fst=%u lin=%u wrap=(%d,%d) raw=%08x rgba=%08x "
                      "test=0x%llx alpha=0x%llx",
                      state.prim.fst ? 1u : 0u, state.linearFilter ? 1u : 0u,
                      wu, wv, rawTex, texel,
                      static_cast<unsigned long long>(ctx.test),
                      static_cast<unsigned long long>(ctx.alpha));
    }
    std::snprintf(cls, sizeof(cls), "%s;rgba-in=(%u,%u,%u,%u)", outcome,
                  combR, combG, combB, combA);
    gb7c2Emit(srcXy, dstXy, oldS, newS, texS, "-", "-", cls);
}

uint64_t GSCpuBackend::NoteGb7c4BatchBegin(const GSPrimitiveBatch &batch)
{
    Gb7c4ProbeState &s = gb7c4State();
    const uint64_t batchId = s.nextBatch++;
    s.curBatch = batchId;
    s.curKind = 0;
    if (!s.enabled || s.capped)
        return batchId;
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    const bool isSprite = state.prim.type == GS_PRIM_SPRITE && batch.vertexCount >= 2u;
    const bool isC1 = isSprite && !state.prim.fst &&
                      s.ctx.tick == 600u && s.ctx.packetIndex == 47176u && s.ctx.path == 2u &&
                      ctx.frame.fbp == 0u && ctx.tex0.tbp0 == 11017u;
    const bool isCarrier = isSprite && state.prim.fst && state.prim.tme &&
                           s.ctx.tick == 601u && s.ctx.packetIndex == 47240u && s.ctx.path == 3u &&
                           ctx.frame.fbp == 112u && ctx.tex0.tbp0 == 0u;
    if (!isC1 && !isCarrier)
        return batchId;
    s.curKind = isC1 ? 1 : 2;

    // Clipped sprite rect (mirrors DrawSprite).
    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    int ax0 = static_cast<int>(batch.vertices[0].x) - ofx;
    int ay0 = static_cast<int>(batch.vertices[0].y) - ofy;
    int ax1 = static_cast<int>(batch.vertices[1].x) - ofx;
    int ay1 = static_cast<int>(batch.vertices[1].y) - ofy;
    if (ax0 > ax1)
        std::swap(ax0, ax1);
    if (ay0 > ay1)
        std::swap(ay0, ay1);
    const int spanX = std::max(1, ax1 - ax0);
    const int spanY = std::max(1, ay1 - ay0);
    ax1 = ax0 + spanX - 1;
    ay1 = ay0 + spanY - 1;
    const int x0 = clampInt(ax0, ctx.scissor.x0, ctx.scissor.x1);
    const int y0 = clampInt(ay0, ctx.scissor.y0, ctx.scissor.y1);
    const int x1 = clampInt(ax1, ctx.scissor.x0, ctx.scissor.x1);
    const int y1 = clampInt(ay1, ctx.scissor.y0, ctx.scissor.y1);

    const GSVertex &v1 = batch.vertices[1];
    char detail[1024];
    std::snprintf(detail, sizeof(detail),
                  "rect=(%d,%d)-(%d,%d) fst=%u tme=%u abe=%u frame=fbp%u,fbw%u,psm%u,fbmsk=0x%08x "
                  "tex=tbp0-%u,tbw%u,psm%u,tw%u,th%u,tcc%u,tfx%u,cbp%u,cpsm%u,csm%u,csa%u,cld%u "
                  "clamp=0x%llx xyoff=(%u,%u) tex1=0x%llx texa=(ta0=%u,aem=%u,ta1=%u) "
                  "test=0x%llx alpha=0x%llx pabe=%u vrt=(%u,%u,%u,%u)",
                  x0, y0, x1, y1, state.prim.fst ? 1u : 0u, state.prim.tme ? 1u : 0u,
                  state.prim.abe ? 1u : 0u,
                  ctx.frame.fbp, static_cast<unsigned>(ctx.frame.fbw),
                  static_cast<unsigned>(ctx.frame.psm), ctx.frame.fbmsk,
                  ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw),
                  static_cast<unsigned>(ctx.tex0.psm), static_cast<unsigned>(ctx.tex0.tw),
                  static_cast<unsigned>(ctx.tex0.th), static_cast<unsigned>(ctx.tex0.tcc),
                  static_cast<unsigned>(ctx.tex0.tfx), ctx.tex0.cbp,
                  static_cast<unsigned>(ctx.tex0.cpsm), static_cast<unsigned>(ctx.tex0.csm),
                  static_cast<unsigned>(ctx.tex0.csa), static_cast<unsigned>(ctx.tex0.cld),
                  static_cast<unsigned long long>(ctx.clamp),
                  static_cast<unsigned>(ctx.xyoffset.ofx),
                  static_cast<unsigned>(ctx.xyoffset.ofy),
                  static_cast<unsigned long long>(ctx.tex1),
                  static_cast<unsigned>(state.texa.ta0), state.texa.aem ? 1u : 0u,
                  static_cast<unsigned>(state.texa.ta1),
                  static_cast<unsigned long long>(ctx.test),
                  static_cast<unsigned long long>(ctx.alpha),
                  state.pabe ? 1u : 0u,
                  static_cast<unsigned>(v1.r), static_cast<unsigned>(v1.g),
                  static_cast<unsigned>(v1.b), static_cast<unsigned>(v1.a));
    gb7c4Emit("-", isC1 ? "batch-c1" : "batch-carrier",
              "-", "-", "-", "-", "-", "-", "-", "-", detail, "-",
              isC1 ? "batch-c1" : "batch-carrier");
    return batchId;
}

void GSCpuBackend::TraceGb7c4C1Pixel(const GSDrawState &state, int x, int y, uint32_t z,
                                     float texUf, float texVf,
                                     uint32_t texel,
                                     uint8_t combR, uint8_t combG, uint8_t combB, uint8_t combA,
                                     uint32_t oldVal, int candIdx)
{
    Gb7c4ProbeState &s = gb7c4State();
    if (!s.enabled || s.capped)
        return;
    const auto &ctx = state.context;
    const uint32_t fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
    const uint32_t fpsm = ctx.frame.psm;
    // New value after the (already executed) WritePixel, plus the swizzled
    // VRAM word address from the runtime helper (not a linear offset).
    const uint32_t newVal = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
    const uint32_t dstAddr = GSPSMCT32::addrPSMCT32(fbp, fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    // STQ pre/post-CLAMP UV plus the raw T4 nibble/CLUT at the wrapped texel.
    const int su = static_cast<int>(texUf);
    const int sv = static_cast<int>(texVf);
    const uint64_t clamp = ctx.clamp;
    const int wu = wrapTextureCoordinate(su, state.textureWidth,
                                         static_cast<uint8_t>(clamp & 0x3u),
                                         static_cast<uint16_t>((clamp >> 4) & 0x3FFu),
                                         static_cast<uint16_t>((clamp >> 14) & 0x3FFu));
    const int wv = wrapTextureCoordinate(sv, state.textureHeight,
                                         static_cast<uint8_t>((clamp >> 2) & 0x3u),
                                         static_cast<uint16_t>((clamp >> 24) & 0x3FFu),
                                         static_cast<uint16_t>((clamp >> 34) & 0x3FFu));
    const uint32_t raw = ReadVramUnlocked(ctx.tex0.psm, ctx.tex0.tbp0, ctx.tex0.tbw,
                                          static_cast<uint32_t>(wu), static_cast<uint32_t>(wv));
    const int clut = static_cast<int>(resolveClutIndex(static_cast<uint8_t>(raw),
                                                       ctx.tex0.cpsm, ctx.tex0.csm,
                                                       ctx.tex0.csa, ctx.tex0.psm));
    // TEST/ALPHA/z outcome evaluated on the independent old value.
    const PixelWriteMask wm = classifyAlphaTest(ctx.test, combA, static_cast<uint8_t>(fpsm));
    char outcome[64];
    if (!wm.writesFramebuffer())
    {
        std::snprintf(outcome, sizeof(outcome), "%s",
                      wm.writeDepth ? "depth-only-no-fb-write" : "test-rejected:no-write");
    }
    else if (!passesDestinationAlphaTest(ctx.test, static_cast<uint8_t>(fpsm), oldVal))
    {
        std::snprintf(outcome, sizeof(outcome), "test-rejected:dest-alpha");
    }
    else
    {
        const uint32_t zMethod = static_cast<uint32_t>((ctx.test >> 17) & 3u);
        bool zpass = false;
        if (zMethod == 0u)
            zpass = false;
        else if (zMethod == 1u)
            zpass = true;
        else
        {
            const uint32_t zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
            const uint32_t storedZ = ReadVramUnlocked(ctx.zbuf.psm, zbp, fbw, x, y);
            zpass = (zMethod == 2u) ? (z >= storedZ) : (z > storedZ);
        }
        if (!zpass)
            std::snprintf(outcome, sizeof(outcome), "test-rejected:ztest");
        else if (newVal != oldVal)
            std::snprintf(outcome, sizeof(outcome), "traced-write");
        else
            std::snprintf(outcome, sizeof(outcome), "write-same-value-unknown");
    }
    s.c1Seen[candIdx] = true;
    s.c1Old[candIdx] = oldVal;
    s.c1New[candIdx] = newVal;
    s.c1Addr[candIdx] = dstAddr;
    const char cand[2] = {kGb7c4CandName[candIdx], '\0'};
    char dstXy[32], dstAddrS[16], oldS[16], newS[16], srcUv[128], tapState[160], blend[64], stateS[256], testS[128];
    std::snprintf(dstXy, sizeof(dstXy), "(%d,%d)", x, y);
    std::snprintf(dstAddrS, sizeof(dstAddrS), "%08x", dstAddr);
    std::snprintf(oldS, sizeof(oldS), "%08x", oldVal);
    std::snprintf(newS, sizeof(newS), "%08x", newVal);
    std::snprintf(srcUv, sizeof(srcUv), "preF=(%.3f,%.3f) preI=(%d,%d) post=(%d,%d)",
                  texUf, texVf, su, sv, wu, wv);
    std::snprintf(tapState, sizeof(tapState), "raw=%08x nibble=%x clut=%d texel=%08x",
                  raw, raw & 0xFu, clut, texel);
    std::snprintf(blend, sizeof(blend), "rgba-in=(%u,%u,%u,%u)", combR, combG, combB, combA);
    std::snprintf(stateS, sizeof(stateS),
                  "frame=fbp%u,fbw%u,psm%u tex=tbp0-%u,tbw%u,psm%u test=0x%llx alpha=0x%llx",
                  ctx.frame.fbp, static_cast<unsigned>(ctx.frame.fbw),
                  static_cast<unsigned>(ctx.frame.psm),
                  ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw),
                  static_cast<unsigned>(ctx.tex0.psm),
                  static_cast<unsigned long long>(ctx.test),
                  static_cast<unsigned long long>(ctx.alpha));
    std::snprintf(testS, sizeof(testS), "outcome=%s", outcome);
    gb7c4Emit(cand, "c1-pixel", dstXy, dstAddrS, oldS, newS, srcUv, "-",
              tapState, blend, stateS, testS, outcome);
}

void GSCpuBackend::TraceGb7c4CarrierPixel(const GSDrawState &state, int x, int y, uint32_t z,
                                          float texUf, float texVf,
                                          uint16_t sampleU, uint16_t sampleV,
                                          uint32_t texel,
                                          uint8_t combR, uint8_t combG, uint8_t combB, uint8_t combA,
                                          uint8_t vrtR, uint8_t vrtG, uint8_t vrtB, uint8_t vrtA,
                                          uint32_t oldVal, int candIdx)
{
    Gb7c4ProbeState &s = gb7c4State();
    if (!s.enabled || s.capped)
        return;
    const auto &ctx = state.context;
    const auto &tex = ctx.tex0;
    const uint32_t fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
    const uint32_t fpsm = ctx.frame.psm;
    const uint32_t newVal = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
    // Destination swizzled word address (CT24 shares the C32 page tables).
    const uint32_t dstAddr = GSPSMCT32::addrPSMCT32(fbp, fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    // Recompute the four bilinear taps exactly as SampleTexture does for
    // the FST path: float UV from the quantized fixed-point sample, minus
    // half texel, floored, with per-tap CLAMP wrap.
    const uint64_t clamp = ctx.clamp;
    const uint8_t wrapU = static_cast<uint8_t>(clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
    const uint16_t minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
    const uint16_t maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
    const uint16_t minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
    const uint16_t maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);
    const int texW = state.textureWidth;
    const int texH = state.textureHeight;
    const float tU = static_cast<float>(sampleU) / 16.0f;
    const float tV = static_cast<float>(sampleV) / 16.0f;
    const float sU = tU - 0.5f;
    const float sV = tV - 0.5f;
    const int u0 = static_cast<int>(std::floor(sU));
    const int v0 = static_cast<int>(std::floor(sV));
    const int u1 = u0 + 1;
    const int v1 = v0 + 1;
    const float fx = sU - static_cast<float>(u0);
    const float fy = sV - static_cast<float>(v0);
    const int preX[4] = {u0, u1, u0, u1};
    const int preY[4] = {v0, v0, v1, v1};
    int postX[4], postY[4];
    uint32_t tapAddr[4], tapWord[4], tapRgba[4];
    for (int i = 0; i < 4; ++i)
    {
        postX[i] = wrapTextureCoordinate(preX[i], texW, wrapU, minU, maxU);
        postY[i] = wrapTextureCoordinate(preY[i], texH, wrapV, minV, maxV);
        tapWord[i] = ReadVramUnlocked(tex.psm, tex.tbp0, tex.tbw,
                                      static_cast<uint32_t>(postX[i]),
                                      static_cast<uint32_t>(postY[i]));
        tapRgba[i] = applyTexa(state.texa, tex.psm, tapWord[i]);
        tapAddr[i] = GSPSMCT32::addrPSMCT32(tex.tbp0, static_cast<uint32_t>(tex.tbw),
                                            static_cast<uint32_t>(postX[i]),
                                            static_cast<uint32_t>(postY[i]));
    }
    // Single-point post-CLAMP reference for the integer UV.
    const int suPre = static_cast<int>(sampleU >> 4);
    const int svPre = static_cast<int>(sampleV >> 4);
    const int suPost = wrapTextureCoordinate(suPre, texW, wrapU, minU, maxU);
    const int svPost = wrapTextureCoordinate(svPre, texH, wrapV, minV, maxV);
    // TEST/ALPHA/z outcome evaluated on the independent old value.
    const PixelWriteMask wm = classifyAlphaTest(ctx.test, combA, static_cast<uint8_t>(fpsm));
    char writeOutcome[64];
    if (!wm.writesFramebuffer())
    {
        std::snprintf(writeOutcome, sizeof(writeOutcome), "%s",
                      wm.writeDepth ? "depth-only-no-fb-write" : "test-rejected:no-write");
    }
    else if (!passesDestinationAlphaTest(ctx.test, static_cast<uint8_t>(fpsm), oldVal))
    {
        std::snprintf(writeOutcome, sizeof(writeOutcome), "test-rejected:dest-alpha");
    }
    else
    {
        const uint32_t zMethod = static_cast<uint32_t>((ctx.test >> 17) & 3u);
        bool zpass = false;
        if (zMethod == 0u)
            zpass = false;
        else if (zMethod == 1u)
            zpass = true;
        else
        {
            const uint32_t zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
            const uint32_t storedZ = ReadVramUnlocked(ctx.zbuf.psm, zbp, fbw, x, y);
            zpass = (zMethod == 2u) ? (z >= storedZ) : (z > storedZ);
        }
        if (!zpass)
            std::snprintf(writeOutcome, sizeof(writeOutcome), "test-rejected:ztest");
        else if (newVal != oldVal)
            std::snprintf(writeOutcome, sizeof(writeOutcome), "accepted-write-changed");
        else
            std::snprintf(writeOutcome, sizeof(writeOutcome), "accepted-write-same-unknown");
    }
    // Predeclared outcome ladder for this candidate.
    const int wantX = kGb7c4SrcX[candIdx];
    const int wantY = kGb7c4SrcY[candIdx];
    int hitTap = -1;
    for (int i = 0; i < 4; ++i)
    {
        if (postX[i] == wantX && postY[i] == wantY)
        {
            hitTap = i;
            break;
        }
    }
    char outcome[64];
    if (hitTap < 0)
    {
        std::snprintf(outcome, sizeof(outcome), "C-not-sampled");
    }
    else if (!s.c1Seen[candIdx])
    {
        std::snprintf(outcome, sizeof(outcome), "OTHER-c1-unseen");
    }
    else if (tapWord[hitTap] != s.c1New[candIdx])
    {
        std::snprintf(outcome, sizeof(outcome), "OTHER-c1-word-mismatch");
    }
    else if (tapAddr[hitTap] != s.c1Addr[candIdx])
    {
        std::snprintf(outcome, sizeof(outcome), "OTHER-addr-mismatch");
    }
    else if (std::strcmp(writeOutcome, "accepted-write-changed") == 0)
    {
        std::snprintf(outcome, sizeof(outcome), "A-glyph-row-transported");
        if (candIdx == 0)
            s.aComplete = true;
    }
    else
    {
        std::snprintf(outcome, sizeof(outcome), "B-blocked-or-same");
    }
    s.carrierSeen[candIdx] = true;
    const char cand[2] = {kGb7c4CandName[candIdx], '\0'};
    char dstXy[32], dstAddrS[16], oldS[16], newS[16], srcUv[160], taps[256], tapState[320];
    char blend[96], stateS[1024], testS[160];
    std::snprintf(dstXy, sizeof(dstXy), "(%d,%d)", x, y);
    std::snprintf(dstAddrS, sizeof(dstAddrS), "%08x", dstAddr);
    std::snprintf(oldS, sizeof(oldS), "%08x", oldVal);
    std::snprintf(newS, sizeof(newS), "%08x", newVal);
    std::snprintf(srcUv, sizeof(srcUv),
                  "interpF=(%.3f,%.3f) quantF=(%.4f,%.4f) preI=(%d,%d) postSingle=(%d,%d)",
                  texUf, texVf, tU, tV, suPre, svPre, suPost, svPost);
    std::snprintf(taps, sizeof(taps),
                  "pre=(%d,%d);(%d,%d);(%d,%d);(%d,%d) "
                  "post=(%d,%d);(%d,%d);(%d,%d);(%d,%d)",
                  preX[0], preY[0], preX[1], preY[1], preX[2], preY[2], preX[3], preY[3],
                  postX[0], postY[0], postX[1], postY[1], postX[2], postY[2], postX[3], postY[3]);
    std::snprintf(tapState, sizeof(tapState),
                  "addrs=(%08x,%08x,%08x,%08x) words=(%08x,%08x,%08x,%08x) "
                  "rgba=(%08x,%08x,%08x,%08x) hitTap=%d want=(%d,%d)",
                  tapAddr[0], tapAddr[1], tapAddr[2], tapAddr[3],
                  tapWord[0], tapWord[1], tapWord[2], tapWord[3],
                  tapRgba[0], tapRgba[1], tapRgba[2], tapRgba[3],
                  hitTap, wantX, wantY);
    std::snprintf(blend, sizeof(blend), "texel=%08x fx=%.4f fy=%.4f", texel, fx, fy);
    std::snprintf(stateS, sizeof(stateS),
                  "clamp=0x%llx wrapUV=(%u,%u) regionU=(%u,%u) regionV=(%u,%u) "
                  "xyoff=(%u,%u) tex0=(tbp0=%u,tbw=%u,psm=%u,tw=%u,th=%u,tcc=%u,tfx=%u,cbp=%u,cpsm=%u,csm=%u,csa=%u,cld=%u) "
                  "texWH=(%d,%d) lin=%u tex1=0x%llx texa=(ta0=%u,aem=%u,ta1=%u) "
                  "frame=(fbp=%u,fbw=%u,psm=%u,fbmsk=0x%08x) alpha=0x%llx abe=%u pabe=%u "
                  "vrt=(%u,%u,%u,%u) rgba-in=(%u,%u,%u,%u) z=%u",
                  static_cast<unsigned long long>(clamp), wrapU, wrapV, minU, maxU, minV, maxV,
                  static_cast<unsigned>(ctx.xyoffset.ofx),
                  static_cast<unsigned>(ctx.xyoffset.ofy),
                  tex.tbp0, static_cast<unsigned>(tex.tbw),
                  static_cast<unsigned>(tex.psm), static_cast<unsigned>(tex.tw),
                  static_cast<unsigned>(tex.th), static_cast<unsigned>(tex.tcc),
                  static_cast<unsigned>(tex.tfx), tex.cbp,
                  static_cast<unsigned>(tex.cpsm), static_cast<unsigned>(tex.csm),
                  static_cast<unsigned>(tex.csa), static_cast<unsigned>(tex.cld),
                  texW, texH, state.linearFilter ? 1u : 0u,
                  static_cast<unsigned long long>(ctx.tex1),
                  static_cast<unsigned>(state.texa.ta0), state.texa.aem ? 1u : 0u,
                  static_cast<unsigned>(state.texa.ta1),
                  ctx.frame.fbp, static_cast<unsigned>(ctx.frame.fbw),
                  static_cast<unsigned>(ctx.frame.psm), ctx.frame.fbmsk,
                  static_cast<unsigned long long>(ctx.alpha),
                  state.prim.abe ? 1u : 0u, state.pabe ? 1u : 0u,
                  vrtR, vrtG, vrtB, vrtA,
                  combR, combG, combB, combA, z);
    std::snprintf(testS, sizeof(testS), "test=0x%llx write=%s",
                  static_cast<unsigned long long>(ctx.test), writeOutcome);
    gb7c4Emit(cand, "carrier-pixel", dstXy, dstAddrS, oldS, newS, srcUv, taps,
              tapState, blend, stateS, testS, outcome);
}

void GSCpuBackend::DrawPrimitive(const GSPrimitiveBatch &batch)
{
    if (gb7bState().enabled)
        NoteGb7bCandidate(batch);
    // GB7C2: assign the intra-packet batch id (no render effect when OFF).
    uint64_t gb7c2Batch = 0;
    const bool gb7c2Active = gb7c2State().enabled;
    if (gb7c2Active)
        gb7c2Batch = NoteGb7c2BatchBegin(batch);
    // GB7C4: assign the intra-packet batch id (no render effect when OFF).
    // Only called when open; the hook itself assigns ids and flags batches.
    if (gb7c4State().enabled)
        NoteGb7c4BatchBegin(batch);
    // GB7C5: assign the intra-packet batch id with the same counting and
    // tag the current op as a draw (no render effect when OFF).
    if (gb7c5State().enabled)
        NoteGb7c5BatchBegin(batch);
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex < 64u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << ")"
                      << " uv0=(" << (batch.vertices[0].u >> 4) << "," << (batch.vertices[0].v >> 4) << ")"
                      << " stq0=(" << batch.vertices[0].s << "," << batch.vertices[0].t << "," << batch.vertices[0].q << ")"
                      << " v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << ")"
                      << " uv1=(" << (batch.vertices[1].u >> 4) << "," << (batch.vertices[1].v >> 4) << ")"
                      << " stq1=(" << batch.vertices[1].s << "," << batch.vertices[1].t << "," << batch.vertices[1].q << ")"
                      << " v2=(" << batch.vertices[2].x << "," << batch.vertices[2].y << ")"
                      << " uv2=(" << (batch.vertices[2].u >> 4) << "," << (batch.vertices[2].v >> 4) << ")"
                      << " stq2=(" << batch.vertices[2].s << "," << batch.vertices[2].t << "," << batch.vertices[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(batch.vertices[0].r) << ","
                      << static_cast<uint32_t>(batch.vertices[0].g) << ","
                      << static_cast<uint32_t>(batch.vertices[0].b) << ","
                      << static_cast<uint32_t>(batch.vertices[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(batch.vertices[1].r) << ","
                      << static_cast<uint32_t>(batch.vertices[1].g) << ","
                      << static_cast<uint32_t>(batch.vertices[1].b) << ","
                      << static_cast<uint32_t>(batch.vertices[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(batch.vertices[2].r) << ","
                      << static_cast<uint32_t>(batch.vertices[2].g) << ","
                      << static_cast<uint32_t>(batch.vertices[2].b) << ","
                      << static_cast<uint32_t>(batch.vertices[2].a) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((state.prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });

    switch (state.prim.type)
    {
    case GS_PRIM_SPRITE:
        DrawSprite(batch);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        DrawTriangle(batch);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        DrawLine(batch);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = batch.vertices[0];
        const auto &ctx = state.context;
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        WritePixel(state, px, py, static_cast<u32>(v.z), v.r, v.g, v.b, v.a, v.fog);
        break;
    }
    default:
        break;
    }
    if (gb7c2Active)
        NoteGb7c2BatchEnd(batch, gb7c2Batch);
}

void GSCpuBackend::WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog)
{
    const auto &ctx = state.context;
    if (x < ctx.scissor.x0 || x > ctx.scissor.x1 || y < ctx.scissor.y0 || y > ctx.scissor.y1)
        return;

    if (state.prim.fge)
    {
        const uint32_t inverseFog = 255u - fog;
        auto applyFog = [&](uint8_t input, uint8_t fogColor) -> uint8_t
        {
            return static_cast<uint8_t>(((static_cast<uint32_t>(fog) * input) >> 8) + ((inverseFog * fogColor) >> 8));
        };

        r = applyFog(r, state.fogR);
        g = applyFog(g, state.fogG);
        b = applyFog(b, state.fogB);
    }

    const u32 fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const u32 fbw = std::max<u32>(ctx.frame.fbw, 1u);
    const u32 fpsm = ctx.frame.psm;
    const u32 zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const u32 zpsm = ctx.zbuf.psm;

    const PixelWriteMask writeMask = classifyAlphaTest(ctx.test, a, static_cast<uint8_t>(fpsm));
    if (!writeMask.writesAnything())
    {
        return;
    }

    const uint32_t ztestMethod = static_cast<uint32_t>((ctx.test >> 17) & 3u);
    const bool alphaBlendEnabled = state.prim.abe;
    const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && fpsm == GS_PSM_CT32;
    const bool destinationAlphaTestNeedsRead = ((ctx.test >> 14) & 0x1u) != 0u && (fpsm == GS_PSM_CT32 || fpsm == GS_PSM_CT16 || fpsm == GS_PSM_CT16S);

    // small optimization, avoid reading the framebuffer for simple draws
    // TODO: only one address lookup for rmw
    const bool frmw = destinationAlphaTestNeedsRead || (writeMask.writesFramebuffer() && ((ctx.frame.fbmsk != 0) || alphaBlendEnabled || preserveDestinationAlpha));

    u32 rawFramebufferPixel = 0;
    u32 fbrgba = 0;
    if (frmw)
    {
        rawFramebufferPixel = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
        fbrgba = rawFramebufferPixel;

        if (bitsPerPixel(fpsm) == 16)
        {
            fbrgba = Rgba5551ToRgba8888(fbrgba);
        }
        else if (fpsm == GS_PSM_CT24)
        {
            // The GS supplies 0x80 as destination alpha for RGB24 blending.
            fbrgba |= 0x80000000u;
        }
    }

    if (!passesDestinationAlphaTest(ctx.test, static_cast<uint8_t>(fpsm), rawFramebufferPixel))
    {
        return;
    }

    bool zpass = false;
    uint32_t storedZ = 0u;
    switch (ztestMethod)
    {
    case 0:
        zpass = false;
        break;
    case 1:
        zpass = true;
        break;
    case 2:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) >= storedZ;
        break;
    case 3:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) > storedZ;
        break;
    }

    if (!zpass)
    {
        return;
    }

    if (writeMask.writesFramebuffer())
    {
        const u8 srcR = r;
        const u8 srcG = g;
        const u8 srcB = b;

        if (state.prim.abe)
        {
            uint8_t dr = fbrgba & 0xFF;
            uint8_t dg = (fbrgba >> 8) & 0xFF;
            uint8_t db = (fbrgba >> 16) & 0xFF;
            uint8_t da = (fbrgba >> 24) & 0xFF;

            // PABE disables alpha blending when the source alpha MSB is clear.
            if (!(state.pabe && (a & 0x80u) == 0u))
            {
                uint64_t alphaReg = ctx.alpha;
                uint8_t asel = alphaReg & 3;
                uint8_t bsel = (alphaReg >> 2) & 3;
                uint8_t csel = (alphaReg >> 4) & 3;
                uint8_t dsel = (alphaReg >> 6) & 3;
                uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

                auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
                {
                    if (sel == 0)
                        return cs;
                    if (sel == 1)
                        return cd;
                    return 0;
                };
                int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                           : fix;

                r = clampU8(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr));
                g = clampU8(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg));
                b = clampU8(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db));
            }
            else
            {
                r = srcR;
                g = srcG;
                b = srcB;
            }
        }

        if (writeMask.writeAlpha && (ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24)
        {
            a = static_cast<uint8_t>(a | 0x80u);
        }

        u32 pixel = pack32(r, g, b, a);

        if (ctx.frame.fbmsk != 0)
        {
            pixel = (pixel & ~ctx.frame.fbmsk) | (fbrgba & ctx.frame.fbmsk);
        }

        if (preserveDestinationAlpha)
        {
            pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
        }

        // format conversion
        if (bitsPerPixel(fpsm) == 16)
        {
            pixel = Rgba8888ToRgba5551(pixel);
        }

        WriteVramUnlocked(fpsm, fbp, fbw, x, y, pixel);
    }

    if (writeMask.writeDepth && !ctx.zbuf.zmask)
    {
        WriteVramUnlocked(zpsm, zbp, fbw, x, y, z);
    }
}

uint32_t GSCpuBackend::LookupCLUT(const GSDrawState &state,
                                  uint8_t index,
                                  uint32_t cbp,
                                  uint8_t cpsm,
                                  uint8_t csm,
                                  uint8_t csa,
                                  uint8_t sourcePsm)
{
    const uint32_t clutIndex = resolveClutIndex(index, cpsm, csm, csa, sourcePsm);
    const uint32_t clutWidth = (state.texclut.cbw != 0u) ? static_cast<uint32_t>(state.texclut.cbw) : 1u;
    const uint32_t clutX = static_cast<uint32_t>(state.texclut.cou) + (clutIndex & 0x0Fu);
    const uint32_t clutY = static_cast<uint32_t>(state.texclut.cov) + (clutIndex >> 4);

    switch (cpsm)
    {
    case GS_PSM_CT32:
        return applyTexa(state.texa, cpsm, GSMem::ReadCT32(m_vram, cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT24:
        return applyTexa(state.texa, cpsm, GSMem::ReadCT24(m_vram, cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT16:
        return applyTexa(state.texa, cpsm, Rgba5551ToRgba8888(GSMem::ReadCT16(m_vram, cbp, clutWidth, clutX, clutY)));
    case GS_PSM_CT16S:
        return applyTexa(state.texa, cpsm, Rgba5551ToRgba8888(GSMem::ReadCT16S(m_vram, cbp, clutWidth, clutX, clutY)));
    default:
        break;
    }

    return 0xFFFF00FFu;
}

uint32_t GSCpuBackend::SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v)
{
    const auto &ctx = state.context;
    const auto &tex = ctx.tex0;

    const int texW = state.textureWidth;
    const int texH = state.textureHeight;
    const uint64_t clamp = ctx.clamp;
    const uint8_t wrapU = static_cast<uint8_t>(clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
    const uint16_t minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
    const uint16_t maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
    const uint16_t minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
    const uint16_t maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);

    float texUf, texVf;
    if (state.prim.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(texW);
        texVf = t * invQ * static_cast<float>(texH);
    }

    auto samplePoint = [&](int sampleU, int sampleV) -> uint32_t
    {
        sampleU = wrapTextureCoordinate(sampleU, texW, wrapU, minU, maxU);
        sampleV = wrapTextureCoordinate(sampleV, texH, wrapV, minV, maxV);

        u32 out = ReadVramUnlocked(tex.psm, tex.tbp0, tex.tbw, sampleU, sampleV);

        switch (tex.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return applyTexa(state.texa, tex.psm, out);
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return applyTexa(state.texa, tex.psm, Rgba5551ToRgba8888(out));
        case GS_PSM_T8:
        case GS_PSM_T8H:
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return LookupCLUT(state, static_cast<u8>(out), tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
        }

        return 0xFFFF00FFu;
    };

    if (!state.linearFilter)
    {
        return samplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
    }

    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = static_cast<int>(std::floor(sampleU));
    const int v0 = static_cast<int>(std::floor(sampleV));
    const int u1 = u0 + 1;
    const int v1 = v0 + 1;
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);

    const uint32_t c00 = samplePoint(u0, v0);
    const uint32_t c10 = samplePoint(u1, v0);
    const uint32_t c01 = samplePoint(u0, v1);
    const uint32_t c11 = samplePoint(u1, v1);

    const uint8_t r = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu),
                                  static_cast<uint8_t>(c10 & 0xFFu),
                                  static_cast<uint8_t>(c01 & 0xFFu),
                                  static_cast<uint8_t>(c11 & 0xFFu),
                                  fx, fy);
    const uint8_t g = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 8) & 0xFFu),
                                  fx, fy);
    const uint8_t b = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 16) & 0xFFu),
                                  fx, fy);
    const uint8_t a = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 24) & 0xFFu),
                                  fx, fy);

    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

void GSCpuBackend::DrawSprite(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;
    u32 z1 = static_cast<u32>(v1.z);

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
        return;

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

    const uint64_t alphaReg = ctx.alpha;
    const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);

    uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    if (state.prim.tme)
    {
        const auto &tex = ctx.tex0;
        const int texW = state.textureWidth;
        const int texH = state.textureHeight;

        float u0f, v0f, u1f, v1f;
        if (state.prim.fst)
        {
            u0f = static_cast<float>(v0.u >> 4);
            v0f = static_cast<float>(v0.v >> 4);
            u1f = static_cast<float>(v1.u >> 4);
            v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }

        float spriteW = static_cast<float>(spanX);
        float spriteH = static_cast<float>(spanY);
        if (spriteW < 1.0f)
            spriteW = 1.0f;
        if (spriteH < 1.0f)
            spriteH = 1.0f;

        // GB7C2: frame address + wrap modes for the independent old/new
        // reads. Pure locals; no render effect. Only live when the probe
        // is open (one branch per batch otherwise).
        Gb7c2ProbeState &gb7c2 = gb7c2State();
        const bool gb7c2Sprite = gb7c2.enabled && !gb7c2.capped && gb7c2.curKind != 0;
        const uint32_t gb7c2Fbp = gb7c2Sprite ? GSInternal::framePageBaseToBlock(ctx.frame.fbp) : 0u;
        const uint32_t gb7c2Fbw = gb7c2Sprite ? std::max<uint32_t>(ctx.frame.fbw, 1u) : 1u;
        const uint32_t gb7c2Fpsm = gb7c2Sprite ? ctx.frame.psm : 0u;
        const uint64_t gb7c2Clamp = ctx.clamp;
        const uint8_t gb7c2WrapU = static_cast<uint8_t>(gb7c2Clamp & 0x3u);
        const uint8_t gb7c2WrapV = static_cast<uint8_t>((gb7c2Clamp >> 2) & 0x3u);
        const uint16_t gb7c2MinU = static_cast<uint16_t>((gb7c2Clamp >> 4) & 0x3FFu);
        const uint16_t gb7c2MaxU = static_cast<uint16_t>((gb7c2Clamp >> 14) & 0x3FFu);
        const uint16_t gb7c2MinV = static_cast<uint16_t>((gb7c2Clamp >> 24) & 0x3FFu);
        const uint16_t gb7c2MaxV = static_cast<uint16_t>((gb7c2Clamp >> 34) & 0x3FFu);
        // GB7C4: candidate glyph-row capture. Pure locals; no render effect.
        // Only live when the probe is open (one branch per batch otherwise).
        Gb7c4ProbeState &gb7c4 = gb7c4State();
        const bool gb7c4Sprite = gb7c4.enabled && !gb7c4.capped && gb7c4.curKind != 0;
        const uint32_t gb7c4Fbp = gb7c4Sprite ? GSInternal::framePageBaseToBlock(ctx.frame.fbp) : 0u;
        const uint32_t gb7c4Fbw = gb7c4Sprite ? std::max<uint32_t>(ctx.frame.fbw, 1u) : 1u;
        const uint32_t gb7c4Fpsm = gb7c4Sprite ? ctx.frame.psm : 0u;

        for (int y = drawY0; y <= drawY1; ++y)
        {
            float ty = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
            float texVf = v0f + (v1f - v0f) * ty;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                float tx = (static_cast<float>(x - unclippedX0) + 0.5f) / spriteW;
                float texUf = u0f + (u1f - u0f) * tx;
                uint32_t texel = 0xFFFF00FFu;
                int gb7c2Su = 0, gb7c2Sv = 0;
                // GB7C4 candidate capture (set in whichever UV branch runs).
                bool gb7c4Trace = false;
                int gb7c4Cand = -1;
                float gb7c4TexUf = 0.0f, gb7c4TexVf = 0.0f;
                uint16_t gb7c4SampleU = 0, gb7c4SampleV = 0;
                uint32_t gb7c4Old = 0;
                if (state.prim.fst)
                {
                    const int fixedU = static_cast<int>((texUf * 16.0f) + 0.5f);
                    const int fixedV = static_cast<int>((texVf * 16.0f) + 0.5f);
                    const uint16_t sampleU = static_cast<uint16_t>(clampInt(fixedU, 0, 0xFFFF));
                    const uint16_t sampleV = static_cast<uint16_t>(clampInt(fixedV, 0, 0xFFFF));
                    gb7c2Su = static_cast<int>(sampleU >> 4);
                    gb7c2Sv = static_cast<int>(sampleV >> 4);
                    gb7c4TexUf = texUf;
                    gb7c4TexVf = texVf;
                    gb7c4SampleU = sampleU;
                    gb7c4SampleV = sampleV;
                    texel = SampleTexture(state, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
                }
                else
                {
                    gb7c2Su = static_cast<int>(texUf);
                    gb7c2Sv = static_cast<int>(texVf);
                    gb7c4TexUf = texUf;
                    gb7c4TexVf = texVf;
                    texel = SampleTexture(state, texUf / static_cast<float>(texW), texVf / static_cast<float>(texH), 1.0f, 0u, 0u);
                }

                // GB7C2 pixel trace: decide membership, take the independent
                // old read, then let WritePixel run unchanged below.
                bool gb7c2Trace = false;
                int gb7c2Wu = 0, gb7c2Wv = 0;
                uint32_t gb7c2Raw = 0;
                int gb7c2Clut = -1;
                uint32_t gb7c2Old = 0;
                if (gb7c2Sprite)
                {
                    if (gb7c2.curKind == 1)
                    {
                        for (int pi = 0; pi < gb7c2.curPicks; ++pi)
                        {
                            if (gb7c2.pickX[pi] == x && gb7c2.pickY[pi] == y)
                            {
                                gb7c2Trace = true;
                                break;
                            }
                        }
                    }
                    else if (gb7c2.curKind == 2 && gb7c2.curLogged < kGb7c2MaxCarrierPixels &&
                             x >= kGb7c2CropX0 && x <= kGb7c2CropX1 &&
                             y >= kGb7c2CropY0 && y <= kGb7c2CropY1 &&
                             gb7c2Su >= kGb7c2RoiX0 && gb7c2Su <= kGb7c2RoiX1 &&
                             gb7c2Sv >= kGb7c2RoiY0 && gb7c2Sv <= kGb7c2RoiY1)
                    {
                        gb7c2Trace = true;
                        ++gb7c2.curLogged;
                    }
                    if (gb7c2Trace)
                    {
                        gb7c2Wu = wrapTextureCoordinate(gb7c2Su, texW, gb7c2WrapU, gb7c2MinU, gb7c2MaxU);
                        gb7c2Wv = wrapTextureCoordinate(gb7c2Sv, texH, gb7c2WrapV, gb7c2MinV, gb7c2MaxV);
                        gb7c2Raw = ReadVramUnlocked(tex.psm, tex.tbp0, tex.tbw, gb7c2Wu, gb7c2Wv);
                        if (tex.psm == GS_PSM_T8 || tex.psm == GS_PSM_T8H ||
                            tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HL || tex.psm == GS_PSM_T4HH)
                        {
                            gb7c2Clut = static_cast<int>(resolveClutIndex(
                                static_cast<uint8_t>(gb7c2Raw),
                                tex.cpsm, tex.csm, tex.csa, tex.psm));
                        }
                        gb7c2Old = ReadVramUnlocked(gb7c2Fpsm, gb7c2Fbp, gb7c2Fbw, x, y);
                    }
                }

                // GB7C4 candidate glyph-row trace: exact C1 source pixels and
                // exact carrier destination pixels; independent old read
                // before WritePixel runs unchanged below.
                if (gb7c4Sprite)
                {
                    if (gb7c4.curKind == 1)
                    {
                        for (int ci = 0; ci < 4; ++ci)
                        {
                            if (x == kGb7c4SrcX[ci] && y == kGb7c4SrcY[ci])
                            {
                                gb7c4Trace = true;
                                gb7c4Cand = ci;
                                break;
                            }
                        }
                    }
                    else if (gb7c4.curKind == 2)
                    {
                        for (int ci = 0; ci < 4; ++ci)
                        {
                            if (x == kGb7c4DstX[ci] && y == kGb7c4DstY[ci])
                            {
                                // Candidate A already yielded: stop tracing B/C/D.
                                if (ci > 0 && gb7c4.aComplete)
                                    break;
                                gb7c4Trace = true;
                                gb7c4Cand = ci;
                                break;
                            }
                        }
                    }
                    if (gb7c4Trace)
                        gb7c4Old = ReadVramUnlocked(gb7c4Fpsm, gb7c4Fbp, gb7c4Fbw, x, y);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                WritePixel(state, x, y, z1, color.r, color.g, color.b, color.a, v1.fog);
                if (gb7c2Trace)
                {
                    TraceGb7c2Pixel(state, x, y, z1, gb7c2Su, gb7c2Sv, gb7c2Wu, gb7c2Wv,
                                    texel, gb7c2Raw, gb7c2Clut,
                                    color.r, color.g, color.b, color.a, gb7c2Old);
                }
                if (gb7c4Trace)
                {
                    if (gb7c4.curKind == 1)
                    {
                        TraceGb7c4C1Pixel(state, x, y, z1, gb7c4TexUf, gb7c4TexVf,
                                          texel, color.r, color.g, color.b, color.a,
                                          gb7c4Old, gb7c4Cand);
                    }
                    else
                    {
                        TraceGb7c4CarrierPixel(state, x, y, z1, gb7c4TexUf, gb7c4TexVf,
                                               gb7c4SampleU, gb7c4SampleV,
                                               texel, color.r, color.g, color.b, color.a,
                                               r, g, b, a, gb7c4Old, gb7c4Cand);
                    }
                }
            }
        }
    }
    else
    {
        for (int y = drawY0; y <= drawY1; ++y)
            for (int x = drawX0; x <= drawX1; ++x)
                WritePixel(state, x, y, z1, r, g, b, a, v1.fog);
    }
}

void GSCpuBackend::DrawTriangle(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const GSVertex &v2 = batch.vertices[2];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    float fx0 = v0.x - static_cast<float>(ofx);
    float fy0 = v0.y - static_cast<float>(ofy);
    float fx1 = v1.x - static_cast<float>(ofx);
    float fy1 = v1.y - static_cast<float>(ofy);
    float fx2 = v2.x - static_cast<float>(ofx);
    float fy2 = v2.y - static_cast<float>(ofy);

    int minX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
    int maxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
    int minY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    int maxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));

    minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
    maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
    minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
    maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
    if (std::fabs(denom) < 0.001f)
        return;

    const float winding = (denom < 0.0f) ? -1.0f : 1.0f;
    const float invAbsDenom = 1.0f / std::fabs(denom);

    // G46: coverage uses exact edge functions in 1/16-pixel units (vertex XY
    // is 12.4 fixed point, so this is exact) with a top-left fill rule. A
    // pixel on an edge shared by two triangles is drawn by exactly one of
    // them; the old inclusive epsilon test drew it twice, which showed as a
    // bright seam along the diagonal of blended quads. Sample points and
    // attribute interpolation are unchanged.
    struct CoverageEdge
    {
        int64_t ax, ay, dx, dy;
        bool inclusive;
    };
    const int64_t ex[3] = {std::llround(fx0 * 16.0f), std::llround(fx1 * 16.0f), std::llround(fx2 * 16.0f)};
    const int64_t ey[3] = {std::llround(fy0 * 16.0f), std::llround(fy1 * 16.0f), std::llround(fy2 * 16.0f)};
    const int64_t area16 = (ex[1] - ex[0]) * (ey[2] - ey[0]) - (ey[1] - ey[0]) * (ex[2] - ex[0]);
    const int order[3] = {0, (area16 < 0) ? 2 : 1, (area16 < 0) ? 1 : 2};
    CoverageEdge edges[3];
    for (int e = 0; e < 3; ++e)
    {
        const int a = order[e];
        const int b = order[(e + 1) % 3];
        CoverageEdge &edge = edges[e];
        edge.ax = ex[a];
        edge.ay = ey[a];
        edge.dx = ex[b] - ex[a];
        edge.dy = ey[b] - ey[a];
        // Interior is on the positive side; left edges run upward, top
        // edges run rightward along a horizontal line.
        edge.inclusive = edge.dy < 0 || (edge.dy == 0 && edge.dx > 0);
    }

    for (int y = minY; y <= maxY; ++y)
    {
        float py = static_cast<float>(y) + 0.5f;
        const int64_t py16 = static_cast<int64_t>(y) * 16 + 8;
        for (int x = minX; x <= maxX; ++x)
        {
            float px = static_cast<float>(x) + 0.5f;
            const int64_t px16 = static_cast<int64_t>(x) * 16 + 8;

            bool covered = true;
            for (const CoverageEdge &edge : edges)
            {
                const int64_t side = edge.dx * (py16 - edge.ay) - edge.dy * (px16 - edge.ax);
                if (side < 0 || (side == 0 && !edge.inclusive))
                {
                    covered = false;
                    break;
                }
            }
            if (!covered)
                continue;

            float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
            float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
            float w2 = 1.0f - w0 - w1;

            double z = v0.z * w0 + v1.z * w1 + v2.z * w2;

            uint8_t r, g, b, a;
            if (state.prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
            }
            else
            {
                r = v2.r;
                g = v2.g;
                b = v2.b;
                a = v2.a;
            }

            if (state.prim.tme)
            {
                float is, it, iq;
                uint16_t iu, iv;
                if (state.prim.fst)
                {
                    iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                    iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                    is = 0.0f;
                    it = 0.0f;
                    iq = 1.0f;
                }
                else
                {
                    // The GS DDA interpolates the homogeneous S, T and Q
                    // values. Texel coordinates are calculated from S/Q and
                    // T/Q only after interpolation.
                    is = v0.s * w0 + v1.s * w1 + v2.s * w2;
                    it = v0.t * w0 + v1.t * w1 + v2.t * w2;
                    iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
                    iu = 0;
                    iv = 0;
                }

                uint32_t texel = SampleTexture(state, is, it, iq, iu, iv);

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const auto &tex = ctx.tex0;
                const uint8_t shadeR = r;
                const uint8_t shadeG = g;
                const uint8_t shadeB = b;
                const uint8_t shadeA = a;
                const TextureCombineResult color = combineTexture(tex, shadeR, shadeG, shadeB, shadeA, tr, tg, tb, ta);

                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
            }

            const uint8_t fog = clampU8(static_cast<int>(v0.fog * w0 + v1.fog * w1 + v2.fog * w2));
            WritePixel(state, x, y, static_cast<u32>(z + 0.5), r, g, b, a, fog);
        }
    }
}

void GSCpuBackend::DrawLine(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (state.prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        double z = (v0.z + (v1.z - v0.z) * t);
        const uint8_t fog = clampU8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * t));
        WritePixel(state, x0, y0, static_cast<u32>(z), r, g, b, a, fog);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}

void GSCpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transfer = command;
    m_transferState.x = command.trxpos.dsax;
    m_transferState.y = command.trxpos.dsay;
    m_transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * static_cast<uint32_t>(command.trxreg.rrh);
    m_transferState.copiedPixels = 0u;
    m_transferState.direction = command.direction;
    m_transferState.localToHostPendingBytes = 0u;

    if (command.direction == 2u)
    {
        if (gb7c5State().enabled)
            NoteGb7c5LocalToLocalOp();
        PerformLocalToLocalTransfer();
    }
    else if (command.direction == 1u)
        PerformLocalToHostTransfer();
}

void GSCpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!data || sizeBytes == 0u || !m_vram || m_transferState.direction != 0u)
        return;
    if (m_transfer.trxreg.rrw == 0u || m_transfer.trxreg.rrh == 0u || m_transferState.totalPixels == 0u)
        return;

    const uint32_t dbp = m_transfer.bitbltbuf.dbp;
    const uint32_t dbw = std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u);
    const uint8_t dpsm = m_transfer.bitbltbuf.dpsm;
    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t dsax = m_transfer.trxpos.dsax;
    uint32_t offset = 0u;
    if (gb7c5State().enabled)
        NoteGb7c5UploadOp();

    auto advancePixel = [&](uint32_t count)
    {
        const uint32_t totalPixels = m_transferState.totalPixels;
        m_transferState.copiedPixels =
            std::min<uint32_t>(totalPixels, m_transferState.copiedPixels + count);

        if (m_transferState.copiedPixels >= totalPixels)
        {
            m_transferState.direction = 3u;
            m_transferState.totalPixels = 0u;
            return;
        }

        m_transferState.x = dsax + (m_transferState.copiedPixels % rrw);
        m_transferState.y = m_transfer.trxpos.dsay + (m_transferState.copiedPixels / rrw);
    };

    while (offset < sizeBytes && m_transferState.direction == 0u)
    {
        switch (dpsm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        {
            if (sizeBytes - offset < 4u)
                return;
            uint32_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 4u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            if (sizeBytes - offset < 3u)
                return;
            const uint32_t value = static_cast<uint32_t>(data[offset]) |
                                   (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
                                   (static_cast<uint32_t>(data[offset + 2u]) << 16u);
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 3u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        {
            if (sizeBytes - offset < 2u)
                return;
            uint16_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 2u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_T8:
        case GS_PSM_T8H:
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, data[offset++]);
            advancePixel(1u);
            break;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            const uint8_t packed = data[offset++];
            const uint32_t firstPixel = m_transferState.copiedPixels;
            WriteVramUnlocked(dpsm, dbp, dbw,
                              dsax + (firstPixel % rrw),
                              m_transfer.trxpos.dsay + (firstPixel / rrw),
                              packed & 0x0Fu);
            if (firstPixel + 1u < m_transferState.totalPixels)
            {
                const uint32_t secondPixel = firstPixel + 1u;
                WriteVramUnlocked(dpsm, dbp, dbw,
                                  dsax + (secondPixel % rrw),
                                  m_transfer.trxpos.dsay + (secondPixel / rrw),
                                  (packed >> 4u) & 0x0Fu);
            }
            advancePixel(std::min<uint32_t>(2u, m_transferState.totalPixels - firstPixel));
            break;
        }
        default:
            return;
        }
    }
}

void GSCpuBackend::PerformLocalToLocalTransfer()
{
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t total = rrw * rrh;
    if (total == 0u)
    {
        m_transferState.direction = 3u;
        return;
    }

    for (uint32_t pixel = 0; pixel < total; ++pixel)
    {
        uint32_t x = pixel % rrw;
        uint32_t y = pixel / rrw;
        if ((m_transfer.trxpos.dir & 0x2u) != 0u)
            x = rrw - x - 1u;
        if ((m_transfer.trxpos.dir & 0x1u) != 0u)
            y = rrh - y - 1u;

        const uint32_t value = ReadVramUnlocked(m_transfer.bitbltbuf.spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u),
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        WriteVramUnlocked(m_transfer.bitbltbuf.dpsm,
                          m_transfer.bitbltbuf.dbp,
                          std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u),
                          x + m_transfer.trxpos.dsax,
                          y + m_transfer.trxpos.dsay,
                          value);
    }

    m_transferState.copiedPixels = total;
    m_transferState.direction = 3u;
}

void GSCpuBackend::PerformLocalToHostTransfer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t sbw = std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u);
    const uint8_t spsm = m_transfer.bitbltbuf.spsm;
    const uint32_t bpp = static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm)));
    const uint32_t total = rrw * rrh;
    m_localToHostBuffer.reserve((static_cast<size_t>(total) * bpp + 7u) / 8u);

    for (uint32_t pixel = 0u; pixel < total; ++pixel)
    {
        const uint32_t x = pixel % rrw;
        const uint32_t y = pixel / rrw;
        const uint32_t value = ReadVramUnlocked(spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                sbw,
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 24u));
            break;
        case 24:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            break;
        case 16:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            break;
        case 8:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            break;
        case 4:
        {
            if ((pixel & 1u) != 0u)
                break;
            uint32_t next = 0u;
            if (pixel + 1u < total)
            {
                const uint32_t nextPixel = pixel + 1u;
                const uint32_t nextX = nextPixel % rrw;
                const uint32_t nextY = nextPixel / rrw;
                next = ReadVramUnlocked(spsm, m_transfer.bitbltbuf.sbp, sbw,
                                        nextX + m_transfer.trxpos.ssax,
                                        nextY + m_transfer.trxpos.ssay);
            }
            m_localToHostBuffer.push_back(static_cast<uint8_t>((value & 0x0Fu) | ((next & 0x0Fu) << 4u)));
            break;
        }
        default:
            break;
        }
    }

    m_transferState.copiedPixels = total;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size();
}

uint32_t GSCpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!dst || maxBytes == 0u || m_localToHostReadPos >= m_localToHostBuffer.size())
        return 0u;
    const size_t count = std::min<size_t>(maxBytes, m_localToHostBuffer.size() - m_localToHostReadPos);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, count);
    m_localToHostReadPos += count;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size() - m_localToHostReadPos;
    return static_cast<uint32_t>(count);
}

bool GSCpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || context.frame.fbw == 0u)
        return false;
    if (gb7c5State().enabled)
        NoteGb7c5ClearOp(context, rgba);

    const uint32_t x0 = context.scissor.x0;
    const uint32_t x1 = std::max<uint32_t>(x0, context.scissor.x1);
    const uint32_t y0 = context.scissor.y0;
    const uint32_t y1 = std::max<uint32_t>(y0, context.scissor.y1);
    uint8_t r = static_cast<uint8_t>(rgba);
    uint8_t g = static_cast<uint8_t>(rgba >> 8u);
    uint8_t b = static_cast<uint8_t>(rgba >> 16u);
    uint8_t a = static_cast<uint8_t>(rgba >> 24u);
    if ((context.fba & 1ull) != 0ull && context.frame.psm != GS_PSM_CT24)
        a |= 0x80u;

    const uint32_t fbp = GSInternal::framePageBaseToBlock(context.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(context.frame.fbw, 1u);
    if (context.frame.psm == GS_PSM_CT32 || context.frame.psm == GS_PSM_CT24)
    {
        const uint32_t source = static_cast<uint32_t>(r) |
                                (static_cast<uint32_t>(g) << 8u) |
                                (static_cast<uint32_t>(b) << 16u) |
                                (static_cast<uint32_t>(a) << 24u);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint32_t pixel = source;
                if (context.frame.fbmsk != 0u)
                {
                    const uint32_t old = ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y);
                    pixel = (pixel & ~context.frame.fbmsk) | (old & context.frame.fbmsk);
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }

    if (context.frame.psm == GS_PSM_CT16 || context.frame.psm == GS_PSM_CT16S)
    {
        const uint16_t source = encodeFramePixelPSMCT16(r, g, b, a);
        const uint16_t mask = static_cast<uint16_t>(context.frame.fbmsk);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint16_t pixel = source;
                if (mask != 0u)
                {
                    const uint16_t old = static_cast<uint16_t>(ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y));
                    pixel = static_cast<uint16_t>((pixel & ~mask) | (old & mask));
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }
    return false;
}

bool GSCpuBackend::CopyFrameToHostRgba(const GSFrameReg &frame,
                                       uint32_t width,
                                       uint32_t height,
                                       std::vector<uint8_t> &outPixels,
                                       bool preserveAlpha,
                                       bool useLocalMemoryLayout,
                                       bool frameBaseIsPages,
                                       uint32_t sourceOriginX,
                                       uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
        return false;

    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
    const uint32_t baseBytes = frameBaseIsPages ? frame.fbp * 8192u : frame.fbp * 256u;
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbw = frame.fbw ? frame.fbw : kHostFrameWidth / 64u;
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t stride = fbw * 64u * bytesPerPixel;

    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *dst = outPixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t sx = sourceOriginX + x;
            const uint32_t sy = sourceOriginY + y;
            if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
            {
                uint32_t color = 0u;
                if (useLocalMemoryLayout)
                    color = ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy);
                else
                {
                    const uint32_t pixelBytes = frame.psm == GS_PSM_CT24 ? 3u : 4u;
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * pixelBytes;
                    if (offset + pixelBytes > m_vramSize)
                        return false;
                    color = m_vram[offset] | (static_cast<uint32_t>(m_vram[offset + 1u]) << 8u) |
                            (static_cast<uint32_t>(m_vram[offset + 2u]) << 16u);
                    if (pixelBytes == 4u)
                        color |= static_cast<uint32_t>(m_vram[offset + 3u]) << 24u;
                }
                dst[x * 4u] = static_cast<uint8_t>(color);
                dst[x * 4u + 1u] = static_cast<uint8_t>(color >> 8u);
                dst[x * 4u + 2u] = static_cast<uint8_t>(color >> 16u);
                dst[x * 4u + 3u] = preserveAlpha && frame.psm != GS_PSM_CT24 ? static_cast<uint8_t>(color >> 24u) : 255u;
            }
            else if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
            {
                uint16_t color = 0u;
                if (useLocalMemoryLayout)
                    color = static_cast<uint16_t>(ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy));
                else
                {
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * 2u;
                    if (offset + 2u > m_vramSize)
                        return false;
                    std::memcpy(&color, m_vram + offset, sizeof(color));
                }
                const uint32_t r = color & 31u;
                const uint32_t g = (color >> 5u) & 31u;
                const uint32_t b = (color >> 10u) & 31u;
                dst[x * 4u] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
                dst[x * 4u + 3u] = preserveAlpha ? ((color & 0x8000u) ? 0x80u : 0u) : 255u;
            }
            else
            {
                outPixels.clear();
                return false;
            }
        }
    }
    return true;
}

PresentationFrame GSCpuBackend::Present(const GSPresentationRequest &request)
{
    // Snapshot local memory under the backend lock, then perform the expensive
    // display conversion without holding the producer-side raster lock.
    thread_local std::vector<uint8_t> snapshot;
    SnapshotVram(snapshot);
    if (snapshot.empty())
        return {};

    thread_local GSCpuBackend snapshotBackend;
    snapshotBackend.Initialize(snapshot.data(), static_cast<uint32_t>(snapshot.size()));
    return snapshotBackend.PresentFromLocalMemory(request);
}

PresentationFrame GSCpuBackend::PresentFromLocalMemory(const GSPresentationRequest &request)
{
    PresentationFrame result{};
    const GSPmodeState pmode = decodePmode(request.pmode);
    const GSSmode2State smode2 = decodeSMode2(request.smode2);
    const bool fieldMode = smode2.interlaced && !smode2.frameMode;
    const bool oddField = (request.vsyncTick & 1ull) != 0ull;
    const GSFrameReg displayFrame1 = decodeDisplayFrame(request.dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(request.dispfb2);
    const GSDisplayReadOrigin origin1 = decodeDisplayReadOrigin(request.dispfb1);
    const GSDisplayReadOrigin origin2 = decodeDisplayReadOrigin(request.dispfb2);
    uint32_t width1 = 0u, height1 = 0u, width2 = 0u, height2 = 0u;
    decodeDisplaySize(request.display1, width1, height1);
    decodeDisplaySize(request.display2, width2, height2);
    const bool valid1 = pmode.enableCrt1 && hasDisplaySetup(request.display1, displayFrame1);
    const bool valid2 = pmode.enableCrt2 && hasDisplaySetup(request.display2, displayFrame2);
    if (!valid1 && !valid2)
        return result;

    auto copySource = [&](const GSFrameReg &displayFrame,
                          const GSDisplayReadOrigin &origin,
                          uint32_t width,
                          uint32_t height,
                          bool allowPreferred,
                          bool preserveAlpha,
                          GSFrameReg &selected,
                          std::vector<uint8_t> &pixels,
                          bool &usedPreferred) -> bool
    {
        selected = displayFrame;
        pixels.clear();
        usedPreferred = false;
        if (allowPreferred && request.hasPreferredSource && request.preferredDestFbp == displayFrame.fbp &&
            (request.preferredSource.fbw != 0u || request.preferredSource.fbp != displayFrame.fbp) &&
            CopyFrameToHostRgba(request.preferredSource, width, height, pixels, preserveAlpha, true, false, 0u, 0u))
        {
            selected = request.preferredSource;
            usedPreferred = true;
        }
        if (pixels.empty() && !CopyFrameToHostRgba(displayFrame, width, height, pixels, preserveAlpha, true, true, origin.x, origin.y))
            return false;

        if (!usedPreferred && displayFrame.fbp == 0u && countNonBlackPixels(pixels, width, height) == 0u)
        {
            for (const GSFrameReg &candidate : request.contextFrames)
            {
                if (candidate.fbp == selected.fbp && candidate.fbw == selected.fbw && candidate.psm == selected.psm)
                    continue;
                std::vector<uint8_t> candidatePixels;
                if (!CopyFrameToHostRgba(candidate, width, height, candidatePixels, preserveAlpha, true, true, 0u, 0u))
                    continue;
                if (countNonBlackPixels(candidatePixels, width, height) == 0u)
                    continue;
                selected = candidate;
                pixels.swap(candidatePixels);
                break;
            }
        }
        return true;
    };

    if (valid1 && valid2)
    {
        GSFrameReg selected1{}, selected2{};
        std::vector<uint8_t> crt1, crt2;
        bool preferred1 = false, preferred2 = false;
        if (copySource(displayFrame1, origin1, width1, height1, false, true, selected1, crt1, preferred1) &&
            copySource(displayFrame2, origin2, width2, height2, false, true, selected2, crt2, preferred2))
        {
            result.width = std::max(width1, width2);
            result.height = std::max(height1, height2);
            result.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            const uint8_t bgR = static_cast<uint8_t>(request.bgcolor);
            const uint8_t bgG = static_cast<uint8_t>(request.bgcolor >> 8u);
            const uint8_t bgB = static_cast<uint8_t>(request.bgcolor >> 16u);
            for (uint32_t y = 0; y < result.height; ++y)
                for (uint32_t x = 0; x < result.width; ++x)
                {
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    dst[0] = bgR;
                    dst[1] = bgG;
                    dst[2] = bgB;
                    dst[3] = pmode.alp;
                }
            if (!pmode.slbg)
                for (uint32_t y = 0; y < height2; ++y)
                    std::memcpy(result.pixels.data() + y * kHostFrameWidth * 4u, crt2.data() + y * kHostFrameWidth * 4u, width2 * 4u);
            for (uint32_t y = 0; y < height1; ++y)
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t *src = crt1.data() + (y * kHostFrameWidth + x) * 4u;
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    const uint32_t factor = pmode.mmod ? pmode.alp : std::min<uint32_t>(255u, static_cast<uint32_t>(src[3]) * 2u);
                    dst[0] = blendPresentationChannel(src[0], dst[0], factor);
                    dst[1] = blendPresentationChannel(src[1], dst[1], factor);
                    dst[2] = blendPresentationChannel(src[2], dst[2], factor);
                    dst[3] = pmode.amod ? dst[3] : src[3];
                }
            normalizePresentationAlpha(result.pixels, result.width, result.height);
            if (fieldMode && deinterlaceBobEnabled())
                applyFieldPresentation(result.pixels, result.width, result.height, oddField);
            result.displayFbp = displayFrame1.fbp;
            result.sourceFbp = selected1.fbp;
            return result;
        }
    }

    const GSFrameReg &displayFrame = valid1 ? displayFrame1 : displayFrame2;
    const GSDisplayReadOrigin &origin = valid1 ? origin1 : origin2;
    result.width = valid1 ? width1 : width2;
    result.height = valid1 ? height1 : height2;
    GSFrameReg selected = displayFrame;
    if (!copySource(displayFrame, origin, result.width, result.height, true, false, selected, result.pixels, result.usedPreferred))
        return {};
    if (fieldMode && deinterlaceBobEnabled())
        applyFieldPresentation(result.pixels, result.width, result.height, oddField);
    normalizePresentationAlpha(result.pixels, result.width, result.height);
    result.displayFbp = displayFrame.fbp;
    result.sourceFbp = selected.fbp;
    return result;
}
