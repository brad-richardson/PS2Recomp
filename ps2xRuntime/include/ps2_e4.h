// E4 first-missing-visible-result locator: one bounded steady-frame capture.
//
// Header-only diag module. Master gate: PS2X_E4_ARM_TICK=<vsync tick N> AND
// PS2X_E4_DIR=<output dir>. Unset/invalid = every tap compiles to cached-bool
// checks; zero guest-visible behavior change. Optional bind:
// PS2X_E4_FREEZE_TICK=<tick M> (default N+1).
//
// At VBlankStart tick==N (exact boundary, same site as E3b noteVBlank):
// clear + unpause the GS debug history. At tick==M: pause + dump the frozen
// history text, VRAM snapshots (arm + freeze), ReadVram grid samples over
// every distinct draw/display surface, and the full Present-request inputs.
// Emits [e4:armed] / [e4:frozen] / [e4:span-complete] on stderr.
// Constraint C1 (E3b shape): observation only -- no tap writes guest state,
// no draw/present/scheduler behavior touched.
#pragma once

#include "ps2_e7.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ps2_e4
{

inline constexpr uint64_t kNoTick = UINT64_MAX;
inline constexpr size_t kMaxSurfaces = 16u;
inline constexpr uint32_t kGridN = 32u;
// E4 emitted-byte cap (history text + samples + present; VRAM snaps capped
// separately at 2x4 MiB by construction).
inline constexpr uint64_t kDefaultByteCap = 2ull * 1024ull * 1024ull;

// ---- tiny pure parsers (unit-tested) ----

inline bool parseU64(const char *text, uint64_t &out)
{
    if (!text || !text[0])
    {
        return false;
    }
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (!end || end == text || *end != '\0')
    {
        return false;
    }
    out = static_cast<uint64_t>(parsed);
    return true;
}

// ---- cached env binds ----

inline uint64_t armTickRaw()
{
    if (ps2_e7::aligned()) return ps2_e7::alignedArm().load();
    static const uint64_t tick = [] {
        uint64_t parsed = kNoTick;
        if (!parseU64(std::getenv("PS2X_E4_ARM_TICK"), parsed))
        {
            return kNoTick;
        }
        return parsed;
    }();
    return tick;
}

inline const char *outDirRaw()
{
    static const char *dir = [] {
        const char *env = std::getenv("PS2X_E4_DIR");
        return (env && env[0] != '\0') ? env : nullptr;
    }();
    return dir;
}

inline uint64_t freezeTickRaw()
{
    if (ps2_e7::aligned()) { const auto a=armTickRaw(); return a==kNoTick ? kNoTick : a+1u; }
    static const uint64_t tick = [] {
        uint64_t parsed = kNoTick;
        if (parseU64(std::getenv("PS2X_E4_FREEZE_TICK"), parsed))
        {
            return parsed;
        }
        const uint64_t arm = armTickRaw();
        return (arm == kNoTick) ? kNoTick : arm + 1u;
    }();
    return tick;
}

// E50: PS2X_E4_HEAD=<N> keeps the FIRST N draws of the armed span (plus
// the GIF-tag/register events between them) in e4-head.txt. Unset/0 = off.
inline constexpr uint64_t kHeadByteCap = 32ull * 1024ull * 1024ull;
inline constexpr uint64_t kHeadMaxDraws = 65536ull;

inline uint64_t headDrawsRaw()
{
    static const uint64_t n = [] {
        uint64_t parsed = 0;
        if (!parseU64(std::getenv("PS2X_E4_HEAD"), parsed))
        {
            return uint64_t{0};
        }
        return parsed > kHeadMaxDraws ? kHeadMaxDraws : parsed;
    }();
    return n;
}

inline bool enabled()
{
    return armTickRaw() != kNoTick && outDirRaw() != nullptr &&
           freezeTickRaw() != kNoTick && freezeTickRaw() > armTickRaw();
}

// ---- T6 submitted-draw census (armed window only) ----

inline std::mutex &censusMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline std::map<uint64_t, std::map<uint32_t, uint64_t>> &censusTicks()
{
    static std::map<uint64_t, std::map<uint32_t, uint64_t>> census;
    return census;
}

inline std::atomic<bool> &armedFlag()
{
    static std::atomic<bool> armed{false};
    return armed;
}

inline std::atomic<bool> &frozenFlag()
{
    static std::atomic<bool> frozen{false};
    return frozen;
}

inline void noteSubmit(uint64_t tick, uint32_t fbp)
{
    if (!armedFlag().load(std::memory_order_acquire) || frozenFlag().load(std::memory_order_acquire))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(censusMutex());
    censusTicks()[tick][fbp]++;
}

// ---- pure formatters (unit-tested) ----

inline const char *eventKindName(GSDebugEventKind kind)
{
    switch (kind)
    {
    case GSDebugEventKind::GifTag:
        return "gif";
    case GSDebugEventKind::Register:
        return "reg";
    case GSDebugEventKind::Draw:
        return "draw";
    case GSDebugEventKind::Transfer:
        return "trx";
    case GSDebugEventKind::Present:
        return "present";
    }
    return "unknown";
}

inline std::string formatHistoryEntry(const GSDebugHistoryEntry &e)
{
    std::ostringstream os;
    os << "[e4:ev] seq=" << e.seq << " tick=" << e.vsyncTick << " frame=" << e.frameIndex
       << " kind=" << eventKindName(e.kind) << " prim=" << static_cast<uint32_t>(e.prim.type)
       << " tme=" << e.prim.tme << " abe=" << e.prim.abe << " fst=" << e.prim.fst
       << " ctxt=" << e.prim.ctxt << " fbp=" << e.frame.fbp << " fbw=" << e.frame.fbw
       << " psm=0x" << std::hex << static_cast<uint32_t>(e.frame.psm) << std::dec
       << " fbmsk=0x" << std::hex << e.frame.fbmsk << std::dec << " zbp=" << e.zbuf.zbp
       << " zpsm=0x" << std::hex << static_cast<uint32_t>(e.zbuf.psm) << std::dec
       << " zmask=" << e.zbuf.zmask << " tex0=" << e.tex0.tbp0 << "," << static_cast<uint32_t>(e.tex0.tbw)
       << ",0x" << std::hex << static_cast<uint32_t>(e.tex0.psm) << std::dec
       << "," << static_cast<uint32_t>(e.tex0.tw) << "," << static_cast<uint32_t>(e.tex0.th)
       << "," << static_cast<uint32_t>(e.tex0.tcc) << "," << static_cast<uint32_t>(e.tex0.tfx)
       << "," << e.tex0.cbp << ",0x" << std::hex << static_cast<uint32_t>(e.tex0.cpsm) << std::dec
       << " scissor=" << e.scissor.x0 << "," << e.scissor.y0 << "," << e.scissor.x1 << "," << e.scissor.y1
       << " test=0x" << std::hex << e.test << " alpha=0x" << e.alpha << std::dec
       << " verts=" << e.vertexCount << " x=" << e.xMin << ".." << e.xMax << " y=" << e.yMin << ".." << e.yMax
       << " z=" << e.zMin << ".." << e.zMax << " a=" << static_cast<uint32_t>(e.aMin) << ".."
       << static_cast<uint32_t>(e.aMax) << " gif=" << e.gifSizeBytes << "," << e.gifNloop << ","
       << static_cast<uint32_t>(e.gifFlg) << "," << static_cast<uint32_t>(e.gifNreg) << " reg=0x"
       << std::hex << static_cast<uint32_t>(e.reg) << ":0x" << e.regValue << std::dec
       << " trx=" << e.trxdir << "," << e.transferPixels << " present=" << e.displayFbp << ","
       << e.sourceFbp << "," << e.width << "x" << e.height << "," << e.usedPreferred;
    return os.str();
}

// E50: head row = the tail-ring row plus XYOFFSET and the first three
// vertices (pixels; screen = v - ofs/16).
inline std::string formatHeadEntry(const GSDebugHistoryEntry &e)
{
    std::ostringstream os;
    os << formatHistoryEntry(e);
    if (e.kind == GSDebugEventKind::Draw)
    {
        os << " ofs=" << (e.ofx / 16.0) << "," << (e.ofy / 16.0) << " v=";
        const uint32_t n = e.vertexCount < 3u ? e.vertexCount : 3u;
        for (uint32_t i = 0; i < n; ++i)
        {
            os << (i ? ";" : "") << e.vx[i] << "," << e.vy[i] << "," << e.vz[i];
        }
    }
    return os.str();
}

inline bool writeHeadText(const char *path, uint64_t arm, uint64_t freeze, uint64_t limit,
                          const std::vector<GSDebugHistoryEntry> &head, uint64_t &outBytes)
{
    std::ostringstream os;
    uint64_t draws = 0;
    for (const GSDebugHistoryEntry &e : head)
    {
        draws += (e.kind == GSDebugEventKind::Draw) ? 1u : 0u;
    }
    os << "# E4 head arm=" << arm << " freeze=" << freeze << " limit=" << limit << " entries=" << head.size()
       << " draws=" << draws << "\n";
    for (const GSDebugHistoryEntry &e : head)
    {
        os << formatHeadEntry(e) << "\n";
    }
    const std::string text = os.str();
    if (text.size() > kHeadByteCap)
    {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good())
    {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    outBytes = text.size();
    return out.good();
}

struct E4Surface
{
    std::string kind; // "draw-dst" | "draw-tex" | "disp1" | "disp2"
    uint32_t psm = 0;
    uint32_t base = 0;
    uint32_t bw = 0;
    uint32_t w = 0;
    uint32_t h = 0;
};

inline void decodeDispfb(uint64_t dispfb, uint32_t &fbp, uint32_t &fbw, uint32_t &psm)
{
    fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
    fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
    psm = static_cast<uint32_t>((dispfb >> 15) & 0x1Fu);
}

inline uint32_t decodeDisplayH(uint64_t display)
{
    const uint32_t dh = static_cast<uint32_t>((display >> 44) & 0x07FFu);
    return (dh == 0u) ? 448u : dh + 1u;
}

// Pure (unit-tested): collect distinct surfaces, capped at kMaxSurfaces.
// Returns the surfaces + whether the cap truncated the set.
inline std::pair<std::vector<E4Surface>, bool> collectSurfaces(const std::vector<GSDebugHistoryEntry> &history,
                                                               uint64_t dispfb1, uint64_t display1,
                                                               uint64_t dispfb2, uint64_t display2)
{
    std::vector<E4Surface> surfaces;
    auto addUnique = [&](const E4Surface &candidate) {
        for (const E4Surface &s : surfaces)
        {
            if (s.kind == candidate.kind && s.psm == candidate.psm && s.base == candidate.base &&
                s.bw == candidate.bw && s.w == candidate.w && s.h == candidate.h)
            {
                return;
            }
        }
        surfaces.push_back(candidate);
    };
    for (const GSDebugHistoryEntry &e : history)
    {
        if (e.kind != GSDebugEventKind::Draw)
        {
            continue;
        }
        E4Surface dst;
        dst.kind = "draw-dst";
        dst.psm = e.frame.psm;
        dst.base = e.frame.fbp;
        dst.bw = e.frame.fbw;
        dst.w = (e.frame.fbw == 0u) ? 64u : e.frame.fbw * 64u;
        dst.h = 512u; // frame height is not in the reg; sample the full VRAM row span
        addUnique(dst);
        if (e.prim.tme)
        {
            E4Surface tex;
            tex.kind = "draw-tex";
            tex.psm = e.tex0.psm;
            tex.base = e.tex0.tbp0;
            tex.bw = e.tex0.tbw;
            tex.w = 1u << std::min<uint32_t>(e.tex0.tw, 10u);
            tex.h = 1u << std::min<uint32_t>(e.tex0.th, 10u);
            addUnique(tex);
        }
    }
    uint32_t fbp = 0, fbw = 0, psm = 0;
    decodeDispfb(dispfb1, fbp, fbw, psm);
    addUnique(E4Surface{"disp1", psm, fbp, fbw, (fbw == 0u) ? 64u : fbw * 64u, decodeDisplayH(display1)});
    decodeDispfb(dispfb2, fbp, fbw, psm);
    addUnique(E4Surface{"disp2", psm, fbp, fbw, (fbw == 0u) ? 64u : fbw * 64u, decodeDisplayH(display2)});
    bool truncated = false;
    if (surfaces.size() > kMaxSurfaces)
    {
        surfaces.resize(kMaxSurfaces);
        truncated = true;
    }
    return {surfaces, truncated};
}

// ---- dump writers (called once at freeze; byte-capped by construction) ----

inline uint32_t fnv1a32(const uint8_t *data, size_t size)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

inline bool writeHistoryText(const char *path, uint64_t arm, uint64_t freeze,
                             const std::vector<GSDebugHistoryEntry> &history,
                             const std::map<uint64_t, std::map<uint32_t, uint64_t>> &census, uint64_t &outBytes)
{
    std::ostringstream os;
    os << "# E4 history arm=" << arm << " freeze=" << freeze << " entries=" << history.size() << "\n";
    for (const auto &tickEntry : census)
    {
        uint64_t total = 0;
        for (const auto &fbpEntry : tickEntry.second)
        {
            total += fbpEntry.second;
        }
        os << "# census tick=" << tickEntry.first << " draws=" << total;
        for (const auto &fbpEntry : tickEntry.second)
        {
            os << " fbp" << fbpEntry.first << "=" << fbpEntry.second;
        }
        os << "\n";
    }
    if (!history.empty())
    {
        os << "# coverage firstSeq=" << history.front().seq << " firstTick=" << history.front().vsyncTick
           << " lastSeq=" << history.back().seq << " lastTick=" << history.back().vsyncTick << "\n";
    }
    else
    {
        os << "# coverage EMPTY\n";
    }
    for (const GSDebugHistoryEntry &e : history)
    {
        os << formatHistoryEntry(e) << "\n";
    }
    const std::string text = os.str();
    if (text.size() > kDefaultByteCap)
    {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good())
    {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    outBytes = text.size();
    return out.good();
}

inline bool writePresentText(const char *path, uint64_t tick, const GSRegisters &regs, const GS &gs,
                             uint32_t vramArmBytes, uint32_t vramArmHash, uint32_t vramFreezeBytes,
                             uint32_t vramFreezeHash, uint64_t &outBytes)
{
    const GSDebugSnapshot snap = gs.getDebugSnapshot();
    GSFrameReg preferred{};
    uint32_t preferredDest = 0;
    const bool hasPreferred = gs.getPreferredDisplaySource(preferred, preferredDest);
    std::ostringstream os;
    os << "# E4 present-inputs tick=" << tick << "\n";
    os << "priv pmode=0x" << std::hex << regs.pmode << " smode2=0x" << regs.smode2 << " dispfb1=0x"
       << regs.dispfb1 << " display1=0x" << regs.display1 << " dispfb2=0x" << regs.dispfb2 << " display2=0x"
       << regs.display2 << " bgcolor=0x" << regs.bgcolor << std::dec << " csr=0x" << std::hex
       << regs.csr.load(std::memory_order_acquire) << std::dec << "\n";
    for (int i = 0; i < 2; ++i)
    {
        const GSContext &ctx = snap.ctx[i];
        os << "ctx" << i << " frame=" << ctx.frame.fbp << "," << ctx.frame.fbw << ",0x" << std::hex
           << static_cast<uint32_t>(ctx.frame.psm) << std::dec << " scissor=" << ctx.scissor.x0 << ","
           << ctx.scissor.y0 << "," << ctx.scissor.x1 << "," << ctx.scissor.y1 << " tex0=" << ctx.tex0.tbp0
           << "," << static_cast<uint32_t>(ctx.tex0.tbw) << ",0x" << std::hex
           << static_cast<uint32_t>(ctx.tex0.psm) << std::dec << " test=0x" << std::hex << ctx.test
           << " alpha=0x" << ctx.alpha << std::dec << " zbuf=" << ctx.zbuf.zbp << ",0x" << std::hex
           << static_cast<uint32_t>(ctx.zbuf.psm) << std::dec << "," << ctx.zbuf.zmask << "\n";
    }
    os << "preferred has=" << hasPreferred << " src=" << preferred.fbp << "," << preferred.fbw << ",0x"
       << std::hex << static_cast<uint32_t>(preferred.psm) << std::dec << " destFbp=" << preferredDest << "\n";
    os << "host has=" << snap.hasHostPresentationFrame << " size=" << snap.hostPresentationWidth << "x"
       << snap.hostPresentationHeight << " displayFbp=" << snap.hostPresentationDisplayFbp << " sourceFbp="
       << snap.hostPresentationSourceFbp << " preferred=" << snap.hostPresentationUsedPreferred
       << " lastDisplayBaseBytes=0x" << std::hex << snap.lastDisplayBaseBytes << std::dec << "\n";
    os << "transfer dir=" << snap.trxdir << " total=" << snap.transferTotalPixels << " copied="
       << snap.transferCopiedPixels << " at=" << snap.transferX << "," << snap.transferY << " pending="
       << snap.localToHostPendingBytes << "\n";
    os << "vram armBytes=" << vramArmBytes << " armFnv=0x" << std::hex << vramArmHash << std::dec
       << " freezeBytes=" << vramFreezeBytes << " freezeFnv=0x" << std::hex << vramFreezeHash << std::dec << "\n";
    const std::string text = os.str();
    if (text.size() > kDefaultByteCap)
    {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good())
    {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    outBytes = text.size();
    return out.good();
}

inline bool snapshotVramToFile(GS &gs, const char *path, uint32_t &outBytes, uint32_t &outHash)
{
    outBytes = 0;
    outHash = 0;
    gs.refreshDisplaySnapshot();
    uint32_t size = 0;
    const uint8_t *data = gs.lockDisplaySnapshot(size);
    if (!data || size == 0u)
    {
        gs.unlockDisplaySnapshot();
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(data), size);
    const bool ok = out.good();
    outBytes = size;
    outHash = fnv1a32(data, size);
    gs.unlockDisplaySnapshot();
    return ok;
}

inline bool writeSamplesText(const char *path, const GS &gs, const std::vector<E4Surface> &surfaces,
                             bool surfacesTruncated, uint64_t &outBytes)
{
    std::ostringstream os;
    os << "# E4 readback grid=" << kGridN << "x" << kGridN << " surfaces=" << surfaces.size()
       << " truncated=" << surfacesTruncated << "\n";
    for (const E4Surface &s : surfaces)
    {
        uint32_t rawNonzero = 0;
        uint32_t rgbNonzero = 0;
        int32_t firstX = -1, firstY = -1;
        uint32_t firstV = 0;
        std::vector<uint32_t> head;
        for (uint32_t gy = 0; gy < kGridN; ++gy)
        {
            for (uint32_t gx = 0; gx < kGridN; ++gx)
            {
                const uint32_t x = (s.w == 0u) ? 0u : (gx * s.w) / kGridN;
                const uint32_t y = (s.h == 0u) ? 0u : (gy * s.h) / kGridN;
                const uint32_t v = gs.ReadVram(s.psm, s.base, s.bw, x, y);
                if (head.size() < 8u)
                {
                    head.push_back(v);
                }
                if (v != 0u)
                {
                    rawNonzero++;
                    if (firstX < 0)
                    {
                        firstX = static_cast<int32_t>(x);
                        firstY = static_cast<int32_t>(y);
                        firstV = v;
                    }
                }
                if ((v & 0x00FFFFFFu) != 0u)
                {
                    rgbNonzero++;
                }
            }
        }
        os << "[e4:surface] kind=" << s.kind << " psm=0x" << std::hex << s.psm << std::dec << " base=" << s.base
           << " bw=" << s.bw << " W=" << s.w << " H=" << s.h << " rawNonzero=" << rawNonzero << " rgbNonzero="
           << rgbNonzero << " first=";
        if (firstX < 0)
        {
            os << "none";
        }
        else
        {
            os << firstX << "," << firstY << "=0x" << std::hex << firstV << std::dec;
        }
        os << " head=";
        for (size_t i = 0; i < head.size(); ++i)
        {
            if (i != 0u)
            {
                os << ",";
            }
            os << "0x" << std::hex << head[i] << std::dec;
        }
        os << "\n";
    }
    const std::string text = os.str();
    if (text.size() > kDefaultByteCap)
    {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good())
    {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    outBytes = text.size();
    return out.good();
}

// ---- VBlank boundary hook (called from EeScheduler::processEvent) ----

inline void noteVBlank(uint64_t tick, GS &gs, GSRegisters &regs)
{
    if (!enabled())
    {
        return;
    }
    const uint64_t arm = armTickRaw();
    const uint64_t freeze = freezeTickRaw();
    const char *dir = outDirRaw();
    if (tick == arm && !armedFlag().exchange(true, std::memory_order_acq_rel))
    {
        gs.clearDebugHistory();
        gs.setDebugHeadLimit(static_cast<size_t>(headDrawsRaw())); // E50 (0 = off)
        gs.setDebugHistoryPaused(false);
        uint32_t bytes = 0, hash = 0;
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/e4-vram-arm.bin", dir);
        const bool ok = snapshotVramToFile(gs, path, bytes, hash);
        std::cerr << "[e4:armed] tick=" << tick << " vram=" << (ok ? "ok" : "FAIL") << " bytes=" << bytes
                  << " fnv=0x" << std::hex << hash << std::dec << std::endl;
    }
    if (tick == freeze && armedFlag().load(std::memory_order_acquire) &&
        !frozenFlag().exchange(true, std::memory_order_acq_rel))
    {
        gs.setDebugHistoryPaused(true);
        const std::vector<GSDebugHistoryEntry> history = gs.getDebugHistory();
        std::map<uint64_t, std::map<uint32_t, uint64_t>> census;
        {
            std::lock_guard<std::mutex> lock(censusMutex());
            census = censusTicks();
        }
        char histPath[1024], vramPath[1024], presPath[1024], sampPath[1024];
        std::snprintf(histPath, sizeof(histPath), "%s/e4-history.txt", dir);
        std::snprintf(vramPath, sizeof(vramPath), "%s/e4-vram-freeze.bin", dir);
        std::snprintf(presPath, sizeof(presPath), "%s/e4-present.txt", dir);
        std::snprintf(sampPath, sizeof(sampPath), "%s/e4-samples.txt", dir);
        uint64_t histBytes = 0, presBytes = 0, sampBytes = 0;
        uint32_t vramBytes = 0, vramHash = 0;
        const bool histOk = writeHistoryText(histPath, arm, freeze, history, census, histBytes);
        if (headDrawsRaw() != 0u)
        {
            char headPath[1024];
            std::snprintf(headPath, sizeof(headPath), "%s/e4-head.txt", dir);
            uint64_t headBytes = 0;
            const std::vector<GSDebugHistoryEntry> head = gs.getDebugHead();
            const bool headOk = writeHeadText(headPath, arm, freeze, headDrawsRaw(), head, headBytes);
            gs.setDebugHeadLimit(0u);
            std::cerr << "[e4:head] tick=" << tick << " entries=" << head.size() << " bytes=" << headBytes
                      << " ok=" << (headOk ? 1 : 0) << std::endl;
        }
        const bool vramOk = snapshotVramToFile(gs, vramPath, vramBytes, vramHash);
        // Re-read the arm snapshot receipt from disk for the present file.
        uint32_t armBytes = 0, armHash = 0;
        {
            char armPath[1024];
            std::snprintf(armPath, sizeof(armPath), "%s/e4-vram-arm.bin", dir);
            std::ifstream in(armPath, std::ios::binary);
            if (in.good())
            {
                std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                armBytes = static_cast<uint32_t>(buf.size());
                armHash = buf.empty() ? 0u : fnv1a32(buf.data(), buf.size());
            }
        }
        const bool presOk =
            writePresentText(presPath, tick, regs, gs, armBytes, armHash, vramBytes, vramHash, presBytes);
        const auto collected = collectSurfaces(history, regs.dispfb1, regs.display1, regs.dispfb2, regs.display2);
        const bool sampOk = writeSamplesText(sampPath, gs, collected.first, collected.second, sampBytes);
        const uint64_t totalBytes = histBytes + presBytes + sampBytes + vramBytes + armBytes;
        std::cerr << "[e4:frozen] tick=" << tick << " entries=" << history.size() << " hist=" << (histOk ? "ok" : "FAIL")
                  << " vram=" << (vramOk ? "ok" : "FAIL") << " present=" << (presOk ? "ok" : "FAIL") << " samples="
                  << (sampOk ? "ok" : "FAIL") << " surfaces=" << collected.first.size()
                  << " trunc=" << collected.second << " bytes=" << totalBytes << std::endl;
        std::cerr << "[e4:span-complete] tick=" << tick << " bytes=" << totalBytes << std::endl;
    }
}

} // namespace ps2_e4
