#include "MiniTest.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_serial_job_thread.h"
#include "runtime/gs/gs_worker.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/ps2_gs_external_backend.h"
#include "runtime/ps2_memory.h"
#include "ps2_mtvu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// GB2 step (a) determinism: the CPU backend behind the GS command queue is
// byte-exact against direct calls. Every op below runs on a direct GS and
// a queued GS; RPC results compare op-by-op (first divergence reports its
// op index, kind, and both hashes), then SnapshotVram bytes, Present
// pixels, and consume bytes compare in full.

namespace
{
    constexpr uint8_t kFlgPacked = 0u;
    constexpr uint8_t kFlgReglist = 1u;
    constexpr uint8_t kFlgImage = 2u;

    void appendU64(std::vector<uint8_t> &dst, uint64_t value)
    {
        const size_t pos = dst.size();
        dst.resize(pos + sizeof(uint64_t));
        std::memcpy(dst.data() + pos, &value, sizeof(uint64_t));
    }

    void appendGifTag(std::vector<uint8_t> &dst, uint16_t nloop, uint8_t flg, uint8_t nreg, uint64_t regs,
                      bool pre = false, uint64_t prim = 0u)
    {
        uint64_t tag = static_cast<uint64_t>(nloop & 0x7FFFu) | (1ull << 15);
        tag |= (static_cast<uint64_t>(flg & 0x3u) << 58);
        tag |= (static_cast<uint64_t>(nreg & 0xFu) << 60);
        if (pre)
            tag |= (1ull << 46) | ((prim & 0x7FFull) << 47);
        appendU64(dst, tag);
        appendU64(dst, regs);
    }

    void appendGifAd(std::vector<uint8_t> &dst, uint64_t value, uint64_t reg)
    {
        appendU64(dst, value);
        appendU64(dst, reg);
    }

    // Packed RGBA (0x01): R0,G0,B0,A0 32-bit fixed each. Packed XYZ2v (0x05):
    // X[15:0], Y at lo>>32, Z full hi, ADC at hi>>47.
    void appendPackedRgba(std::vector<uint8_t> &dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        appendU64(dst, static_cast<uint64_t>(r) | (static_cast<uint64_t>(g) << 32));
        appendU64(dst, static_cast<uint64_t>(b) | (static_cast<uint64_t>(a) << 32));
    }

    void appendPackedXyz2v(std::vector<uint8_t> &dst, uint16_t x16, uint16_t y16, uint32_t z)
    {
        appendU64(dst, static_cast<uint64_t>(x16) | (static_cast<uint64_t>(y16) << 32));
        appendU64(dst, static_cast<uint64_t>(z));
    }

    uint32_t fnv1a32(const uint8_t *data, size_t size)
    {
        uint32_t hash = 2166136261u;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 16777619u;
        }
        return hash;
    }

    uint32_t fnv1a32(const std::vector<uint8_t> &v) { return fnv1a32(v.data(), v.size()); }

    std::vector<uint8_t> snapshotVramBytes(GS &gs)
    {
        gs.refreshDisplaySnapshot();
        uint32_t size = 0;
        const uint8_t *data = gs.lockDisplaySnapshot(size);
        std::vector<uint8_t> out;
        if (data && size != 0u)
            out.assign(data, data + size);
        gs.unlockDisplaySnapshot();
        return out;
    }

    struct PresentPixels
    {
        std::vector<uint8_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t displayFbp = 0;
        uint32_t sourceFbp = 0;
        bool usedPreferred = false;
        bool ok = false;
    };

    PresentPixels presentPixels(GS &gs)
    {
        PresentPixels out;
        gs.latchHostPresentationFrame();
        out.ok = gs.copyLatchedHostPresentationFrame(out.pixels, out.width, out.height,
                                                     &out.displayFbp, &out.sourceFbp,
                                                     &out.usedPreferred);
        return out;
    }

    void initQueueTestRegs(GSRegisters &regs)
    {
        regs.pmode = 1ull;
        regs.smode2 = 0ull;
        regs.dispfb1 = 150ull | (10ull << 9) | (static_cast<uint64_t>(GS_PSM_CT32) << 15);
        regs.display1 = (639ull << 32) | (447ull << 44);
        regs.dispfb2 = regs.dispfb1;
        regs.display2 = regs.display1;
        regs.bgcolor = 0ull;
        regs.csr.store(0x4000ull, std::memory_order_relaxed);
        regs.vsyncTick.store(0ull, std::memory_order_relaxed);
        regs.imr = 0ull;
        regs.busdir = 0ull;
        regs.siglblid = 0ull;
    }

    // --- synthetic streams (cover every command kind) ---

    // Packed triangle: PRIM + RGBA + XYZ2v x3.
    std::vector<uint8_t> makePackedTriangle(uint8_t r, uint8_t g, uint8_t b)
    {
        std::vector<uint8_t> pkt;
        const uint64_t prim = static_cast<uint64_t>(GS_PRIM_TRIANGLE); // ctxt 0
        appendGifTag(pkt, 3u, kFlgPacked, 3u, 0x510ull);
        appendU64(pkt, prim);
        appendU64(pkt, 0ull);
        appendPackedRgba(pkt, r, g, b, 0x80);
        appendPackedXyz2v(pkt, 16u << 4, 16u << 4, 0x1000u);
        appendU64(pkt, prim);
        appendU64(pkt, 0ull);
        appendPackedRgba(pkt, r, g, b, 0x80);
        appendPackedXyz2v(pkt, 64u << 4, 16u << 4, 0x1000u);
        appendU64(pkt, prim);
        appendU64(pkt, 0ull);
        appendPackedRgba(pkt, r, g, b, 0x80);
        appendPackedXyz2v(pkt, 16u << 4, 64u << 4, 0x1000u);
        return pkt;
    }

    // REGLIST points: PRIM preset to POINT by the caller; two RGBAQ/XYZ2 pairs.
    std::vector<uint8_t> makeReglistPoints()
    {
        std::vector<uint8_t> pkt;
        appendGifTag(pkt, 2u, kFlgReglist, 2u, 0x52ull); // RGBAQ, XYZ2
        for (int i = 0; i < 2; ++i)
        {
            const uint64_t rgbaq = 0x10ull | (0x20ull << 8) | (0x30ull << 16) |
                                   (0x40ull << 24) | (0x3F800000ull << 32);
            const uint64_t xyz = (static_cast<uint64_t>(32u + i * 8u) << 0) |
                                 (static_cast<uint64_t>(48u) << 16) |
                                 (static_cast<uint64_t>(0x2000u) << 32);
            appendU64(pkt, rgbaq);
            appendU64(pkt, xyz);
        }
        return pkt;
    }

    // Host->local IMAGE upload in the native-upload shape: 4x A+D setup
    // (BITBLTBUF/TRXPOS/TRXREG/TRXDIR=0) then an IMAGE tag + payload.
    std::vector<uint8_t> makeImageUpload(uint32_t dbp, uint16_t dsax, uint16_t dsay, uint16_t rrw,
                                         uint16_t rrh, uint8_t seed)
    {
        std::vector<uint8_t> pkt;
        const uint64_t bitbltbuf = (static_cast<uint64_t>(dbp) << 32) |
                                   (static_cast<uint64_t>(1u) << 48) |
                                   (static_cast<uint64_t>(GS_PSM_CT32) << 56);
        const uint64_t trxpos = (static_cast<uint64_t>(dsax) << 32) | (static_cast<uint64_t>(dsay) << 48);
        const uint64_t trxreg = static_cast<uint64_t>(rrw) | (static_cast<uint64_t>(rrh) << 32);
        appendGifTag(pkt, 4u, kFlgPacked, 1u, 0xEull);
        appendGifAd(pkt, bitbltbuf, GS_REG_BITBLTBUF);
        appendGifAd(pkt, trxpos, GS_REG_TRXPOS);
        appendGifAd(pkt, trxreg, GS_REG_TRXREG);
        appendGifAd(pkt, 0ull, GS_REG_TRXDIR);
        const uint32_t payloadBytes = static_cast<uint32_t>(rrw) * static_cast<uint32_t>(rrh) * 4u;
        appendGifTag(pkt, static_cast<uint16_t>(payloadBytes / 16u), kFlgImage, 0u, 0ull);
        for (uint32_t i = 0; i < payloadBytes; ++i)
            pkt.push_back(static_cast<uint8_t>(seed + i * 7u));
        return pkt;
    }

    // SIGNAL + FINISH + LABEL + A+D priv writes (DISPFB1/BGCOLOR).
    std::vector<uint8_t> makeSignalPacket()
    {
        std::vector<uint8_t> pkt;
        appendGifTag(pkt, 5u, kFlgPacked, 1u, 0xEull);
        appendGifAd(pkt, 0x11223344ull | (0xFFull << 32), GS_REG_SIGNAL);
        appendGifAd(pkt, 0ull, GS_REG_FINISH);
        appendGifAd(pkt, 0x55667788ull | (0xFF00ull << 32), GS_REG_LABEL);
        appendGifAd(pkt, 0x0123456789ABCDEFull, 0x59u); // DISPFB1
        appendGifAd(pkt, 0x00112233ull, 0x5Fu);         // BGCOLOR
        return pkt;
    }

    // Minimal valid packed packet for the native packed path: RGBA + XYZ2v.
    std::vector<uint8_t> makeNativePackedPoint()
    {
        std::vector<uint8_t> pkt;
        appendGifTag(pkt, 1u, kFlgPacked, 2u, 0x51ull);
        appendPackedRgba(pkt, 0xAA, 0xBB, 0xCC, 0x80);
        appendPackedXyz2v(pkt, 96u << 4, 96u << 4, 0x3000u);
        return pkt;
    }

    struct ScriptResult
    {
        const char *op = nullptr;
        uint64_t scalar = 0; // u32/bool/counter returns
        std::vector<uint8_t> bytes;
    };

    // Applies the full synthetic script to one GS, recording every RPC
    // result. Both instances run the identical call sequence.
    void runScript(GS &gs, std::vector<ScriptResult> &results)
    {
        auto note = [&](const char *op, uint64_t scalar = 0) {
            results.push_back(ScriptResult{op, scalar, {}});
        };
        auto noteBytes = [&](const char *op, const uint8_t *data, uint32_t n) {
            ScriptResult r{op, n, {}};
            if (data && n != 0u)
                r.bytes.assign(data, data + n);
            results.push_back(std::move(r));
        };

        // Context setup via direct register writes (HLE W1/W2 shape).
        gs.writeRegister(GS_REG_FRAME_1, (0ull << 0) | (10ull << 16) |
                                            (static_cast<uint64_t>(GS_PSM_CT32) << 24));
        gs.writeRegister(GS_REG_ZBUF_1, (32ull << 0) | (1ull << 32));
        gs.writeRegister(GS_REG_SCISSOR_1, (639ull << 16) | (447ull << 48));
        gs.writeRegister(GS_REG_TEST_1, 0x30000ull);
        gs.writeRegister(GS_REG_ALPHA_1, 0x44ull);
        gs.writeRegister(GS_REG_XYOFFSET_1, 0ull);
        gs.writeRegister(GS_REG_TEX0_1, (64ull << 0) | (4ull << 14) |
                                            (static_cast<uint64_t>(GS_PSM_CT32) << 20) |
                                            (6ull << 26) | (6ull << 30) | (1ull << 34));
        gs.writeRegister(GS_REG_TEXA, 0x8000ull);
        gs.writeRegister(GS_REG_TEXCLUT, (16ull << 0) | (8ull << 6));
        gs.writeRegister(GS_REG_PRMODECONT, 1ull);
        gs.writeRegister(GS_REG_PABE, 0ull);
        gs.writeRegister(GS_REG_FOGCOL, 0x102030ull);
        gs.writeRegister(GS_REG_TEXFLUSH, 0ull);
        gs.writeRegister(GS_REG_SCANMSK, 0ull);
        gs.writeRegister(GS_REG_DIMX, 0ull);
        gs.writeRegister(GS_REG_DTHE, 0ull);
        gs.writeRegister(GS_REG_COLCLAMP, 1ull);
        note("setup-regs");

        note("clear-ctx", gs.clearFramebufferContext(0u, 0xFF010203u) ? 1u : 0u);
        note("clear-active", gs.clearActiveFramebuffer(0xFF040506u) ? 1u : 0u);

        const std::vector<uint8_t> tri = makePackedTriangle(0xC0, 0x20, 0x40);
        gs.noteGifPath(GifPathId::Path3);
        gs.processGIFPacket(tri.data(), static_cast<uint32_t>(tri.size()));
        note("gif-triangle");

        // Reset RPC mid-stream, then verify both sides agree post-reset.
        gs.reset();
        const std::vector<uint8_t> postReset = snapshotVramBytes(gs);
        noteBytes("post-reset-vram", postReset.data(), static_cast<uint32_t>(postReset.size()));

        // Re-establish context after reset (reset clears it on both sides).
        gs.writeRegister(GS_REG_FRAME_1, (0ull << 0) | (10ull << 16) |
                                            (static_cast<uint64_t>(GS_PSM_CT32) << 24));
        gs.writeRegister(GS_REG_SCISSOR_1, (639ull << 16) | (447ull << 48));
        gs.writeRegister(GS_REG_TEST_1, 0x30000ull);
        gs.writeRegister(GS_REG_PRIM, static_cast<uint64_t>(GS_PRIM_POINT));
        note("setup-regs-2");

        const std::vector<uint8_t> pts = makeReglistPoints();
        gs.noteGifPath(GifPathId::Path1);
        gs.processGIFPacket(pts.data(), static_cast<uint32_t>(pts.size()));
        note("gif-reglist");

        const std::vector<uint8_t> img = makeImageUpload(0u, 8u, 8u, 8u, 4u, 0x51u);
        gs.noteGifPath(GifPathId::Path3);
        gs.processGIFPacket(img.data(), static_cast<uint32_t>(img.size()));
        gs.drainQueue(); // counters increment on the worker; fence first (no-op when direct)
        note("gif-image", gs.nativeImageUploadCount());

        const std::vector<uint8_t> sig = makeSignalPacket();
        gs.noteGifPath(GifPathId::Path2);
        gs.processGIFPacket(sig.data(), static_cast<uint32_t>(sig.size()));
        note("gif-signal");

        const uint64_t nativeBitblt = (static_cast<uint64_t>(0u) << 32) |
                                      (static_cast<uint64_t>(1u) << 48) |
                                      (static_cast<uint64_t>(GS_PSM_CT32) << 56);
        const uint64_t nativeTrxpos = (static_cast<uint64_t>(32u) << 32) | (static_cast<uint64_t>(32u) << 48);
        const uint64_t nativeTrxreg = static_cast<uint64_t>(4u) | (static_cast<uint64_t>(4u) << 32);
        uint8_t nativePayload[64];
        for (uint32_t i = 0; i < sizeof(nativePayload); ++i)
            nativePayload[i] = static_cast<uint8_t>(0xA0u + i);
        gs.uploadImageNative(nativeBitblt, nativeTrxpos, nativeTrxreg, 0ull, nativePayload,
                             sizeof(nativePayload));
        gs.drainQueue();
        note("upload-native", gs.nativeImageUploadCount());

        gs.writeRegister(GS_REG_PRIM, static_cast<uint64_t>(GS_PRIM_POINT));
        const std::vector<uint8_t> packed = makeNativePackedPoint();
        note("native-packed", gs.processNativePackedGIFPacket(packed.data(), static_cast<uint32_t>(packed.size())) ? 1u : 0u);
        gs.drainQueue();
        note("native-packed-count", gs.nativePackedGIFPacketCount());
        const std::vector<uint8_t> notPacked = makeReglistPoints();
        note("native-packed-reject",
             gs.processNativePackedGIFPacket(notPacked.data(), static_cast<uint32_t>(notPacked.size())) ? 1u : 0u);

        gs.WriteVram(GS_PSM_CT32, 0u, 1u, 200u, 100u, 0xDEADBEEFu);
        gs.WriteVram(GS_PSM_CT32, 0u, 1u, 201u, 100u, 0x12345678u);
        note("read-vram-0", gs.ReadVram(GS_PSM_CT32, 0u, 1u, 200u, 100u));
        note("read-vram-1", gs.ReadVram(GS_PSM_CT32, 0u, 1u, 201u, 100u));
        note("read-vram-draw", gs.ReadVram(GS_PSM_CT32, 0u, 10u, 20u, 20u));

        // Local->host consume at stream position.
        const uint64_t storeBitblt = (static_cast<uint64_t>(0u) << 0) |
                                     (static_cast<uint64_t>(1u) << 16) |
                                     (static_cast<uint64_t>(GS_PSM_CT32) << 24);
        gs.writeRegister(GS_REG_BITBLTBUF, storeBitblt);
        gs.writeRegister(GS_REG_TRXPOS, 0ull);
        gs.writeRegister(GS_REG_TRXREG, (8ull << 0) | (2ull << 32));
        gs.writeRegister(GS_REG_TRXDIR, 1ull);
        uint8_t consumeBuf[128] = {};
        const uint32_t n0 = gs.consumeLocalToHostBytes(consumeBuf, 40u);
        noteBytes("consume-0", consumeBuf, n0);
        const uint32_t n1 = gs.consumeLocalToHostBytes(consumeBuf, sizeof(consumeBuf));
        noteBytes("consume-1", consumeBuf, n1);
        const uint32_t n2 = gs.consumeLocalToHostBytes(consumeBuf, sizeof(consumeBuf));
        note("consume-2", n2);

        const GSDebugSnapshot snap = gs.getDebugSnapshot();
        note("snap-trxdir", snap.trxdir);
        note("snap-copied", snap.transferCopiedPixels);
        note("snap-frame0", snap.ctx[0].frame.fbp | (snap.ctx[0].frame.fbw << 16));
        GSFrameReg preferred{};
        uint32_t preferredDest = 0;
        const bool hasPreferred = gs.getPreferredDisplaySource(preferred, preferredDest);
        note("preferred", (hasPreferred ? 1u : 0u) | (preferred.fbp << 8) | (preferredDest << 24));
        note("paused", gs.isDebugHistoryPaused() ? 1u : 0u);
    }

    bool compareResults(TestCase &t, const std::vector<ScriptResult> &direct,
                        const std::vector<ScriptResult> &queued)
    {
        if (direct.size() != queued.size())
        {
            t.IsTrue(false, "direct vs queued op count differs");
            return false;
        }
        for (size_t i = 0; i < direct.size(); ++i)
        {
            const ScriptResult &a = direct[i];
            const ScriptResult &b = queued[i];
            const std::string where = std::string(a.op ? a.op : "?") + " op#" + std::to_string(i);
            if (a.scalar != b.scalar)
            {
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                              "DIVERGE %s: scalar direct=0x%llx queued=0x%llx", where.c_str(),
                              static_cast<unsigned long long>(a.scalar),
                              static_cast<unsigned long long>(b.scalar));
                t.IsTrue(false, msg);
                return false;
            }
            if (a.bytes.size() != b.bytes.size() ||
                (a.bytes.empty() ? false : std::memcmp(a.bytes.data(), b.bytes.data(), a.bytes.size()) != 0))
            {
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                              "DIVERGE %s: bytes direct=%zu/fnv=%08x queued=%zu/fnv=%08x",
                              where.c_str(), a.bytes.size(), fnv1a32(a.bytes), b.bytes.size(),
                              fnv1a32(b.bytes));
                t.IsTrue(false, msg);
                return false;
            }
        }
        return true;
    }

    bool compareVram(TestCase &t, const char *label, const std::vector<uint8_t> &a,
                     const std::vector<uint8_t> &b)
    {
        if (a.size() != b.size() || std::memcmp(a.data(), b.data(), a.size()) != 0)
        {
            char msg[256];
            std::snprintf(msg, sizeof(msg), "%s: VRAM differs direct=%zu/fnv=%08x queued=%zu/fnv=%08x",
                          label, a.size(), fnv1a32(a), b.size(), fnv1a32(b));
            t.IsTrue(false, msg);
            return false;
        }
        return true;
    }

    bool comparePresent(TestCase &t, const char *label, const PresentPixels &a, const PresentPixels &b)
    {
        if (!a.ok && !b.ok)
            return true; // both sides agree there is no frame to latch
        if (!a.ok || !b.ok || a.width != b.width || a.height != b.height ||
            a.displayFbp != b.displayFbp || a.sourceFbp != b.sourceFbp ||
            a.usedPreferred != b.usedPreferred || a.pixels.size() != b.pixels.size() ||
            std::memcmp(a.pixels.data(), b.pixels.data(), a.pixels.size()) != 0)
        {
            char msg[256];
            std::snprintf(
                msg, sizeof(msg),
                "%s: Present differs direct=%ux%u/fnv=%08x queued=%ux%u/fnv=%08x", label, a.width,
                a.height, fnv1a32(a.pixels), b.width, b.height, fnv1a32(b.pixels));
            t.IsTrue(false, msg);
            return false;
        }
        return true;
    }

    bool compareRegs(TestCase &t, const char *label, const GSRegisters &a, const GSRegisters &b)
    {
        const uint64_t csrA = a.csr.load(std::memory_order_relaxed);
        const uint64_t csrB = b.csr.load(std::memory_order_relaxed);
        if (csrA != csrB || a.siglblid != b.siglblid || a.dispfb1 != b.dispfb1 ||
            a.bgcolor != b.bgcolor)
        {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "%s: priv regs differ csr=%llx/%llx sig=%llx/%llx", label,
                          static_cast<unsigned long long>(csrA), static_cast<unsigned long long>(csrB),
                          static_cast<unsigned long long>(a.siglblid),
                          static_cast<unsigned long long>(b.siglblid));
            t.IsTrue(false, msg);
            return false;
        }
        return true;
    }
}

void register_ps2_gs_queue_tests()
{
    MiniTest::Case("PS2GSQueue", [](TestCase &tc)
    {
        tc.Run("queued CPU backend is byte-exact vs direct on synthetic streams", [](TestCase &t)
        {
            std::vector<uint8_t> vramDirect(PS2_GS_VRAM_SIZE, 0u);
            std::vector<uint8_t> vramQueued(PS2_GS_VRAM_SIZE, 0u);
            GSRegisters regsDirect{};
            GSRegisters regsQueued{};
            initQueueTestRegs(regsDirect);
            initQueueTestRegs(regsQueued);

            GS direct;
            direct.init(vramDirect.data(), static_cast<uint32_t>(vramDirect.size()), &regsDirect);
            GS queued;
            queued.init(vramQueued.data(), static_cast<uint32_t>(vramQueued.size()), &regsQueued);
            t.IsFalse(queued.queueEnabled(), "queue should be off by default");
            queued.setQueueEnabled(true);
            t.IsTrue(queued.queueEnabled(), "queue should report enabled");

            std::vector<ScriptResult> directResults;
            std::vector<ScriptResult> queuedResults;
            runScript(direct, directResults);
            runScript(queued, queuedResults);
            queued.drainQueue();
            if (!compareResults(t, directResults, queuedResults))
                return;

            if (!compareVram(t, "synthetic", snapshotVramBytes(direct), snapshotVramBytes(queued)))
                return;
            if (!compareRegs(t, "synthetic", regsDirect, regsQueued))
                return;
            // The script's SIGNAL packet overwrote DISPFB1 with garbage (by
            // design, to test A+D priv writes); point both displays back at
            // the drawn frame so Present compares real pixels.
            regsDirect.dispfb1 = 0ull | (10ull << 9) | (static_cast<uint64_t>(GS_PSM_CT32) << 15);
            regsQueued.dispfb1 = regsDirect.dispfb1;
            if (!comparePresent(t, "synthetic", presentPixels(direct), presentPixels(queued)))
                return;
            t.IsTrue(true, "synthetic streams: RPC results, VRAM, Present, priv regs all byte-exact");
        });

        tc.Run("queued CPU backend is byte-exact vs direct on a captured packet stream",
               [](TestCase &t)
               {
                   const char *dir = std::getenv("PS2X_GS_QUEUE_CAPTURE");
                   if (!dir || !dir[0])
                   {
                       t.IsTrue(true, "no PS2X_GS_QUEUE_CAPTURE dir: captured replay not run");
                       return;
                   }
                   struct Captured
                   {
                       std::string path;
                       GifPathId pathId = GifPathId::Path3;
                   };
                   std::vector<Captured> packets;
                   char indexPath[1024];
                   std::snprintf(indexPath, sizeof(indexPath), "%s/cap-index.txt", dir);
                   FILE *index = std::fopen(indexPath, "r");
                   if (!index)
                   {
                       t.IsTrue(false, "PS2X_GS_QUEUE_CAPTURE dir has no cap-index.txt");
                       return;
                   }
                   char line[256];
                   while (std::fgets(line, sizeof(line), index))
                   {
                       unsigned seq = 0, path = 0, bytes = 0;
                       if (std::sscanf(line, "%u path=%u bytes=%u", &seq, &path, &bytes) != 3 ||
                           bytes == 0u)
                           continue;
                       char name[256];
                       std::snprintf(name, sizeof(name), "%s/cap-%06u-p%u-%u.bin", dir, seq, path,
                                     bytes);
                       FILE *f = std::fopen(name, "rb");
                       if (!f)
                           continue;
                       std::fclose(f);
                       Captured c;
                       c.path = name;
                       c.pathId = (path == 1u) ? GifPathId::Path1
                                               : ((path == 2u) ? GifPathId::Path2 : GifPathId::Path3);
                       packets.push_back(std::move(c));
                   }
                   std::fclose(index);
                   if (packets.empty())
                   {
                       t.IsTrue(false, "captured dir holds no replayable packets");
                       return;
                   }

                   std::vector<uint8_t> vramDirect(PS2_GS_VRAM_SIZE, 0u);
                   std::vector<uint8_t> vramQueued(PS2_GS_VRAM_SIZE, 0u);
                   GSRegisters regsDirect{};
                   GSRegisters regsQueued{};
                   initQueueTestRegs(regsDirect);
                   initQueueTestRegs(regsQueued);
                   GS direct;
                   direct.init(vramDirect.data(), static_cast<uint32_t>(vramDirect.size()), &regsDirect);
                   GS queued;
                   queued.init(vramQueued.data(), static_cast<uint32_t>(vramQueued.size()), &regsQueued);
                   queued.setQueueEnabled(true);

                   for (size_t i = 0; i < packets.size(); ++i)
                   {
                       FILE *f = std::fopen(packets[i].path.c_str(), "rb");
                       if (!f)
                       {
                           t.IsTrue(false, "captured packet vanished mid-replay");
                           return;
                       }
                       std::fseek(f, 0, SEEK_END);
                       const long size = std::ftell(f);
                       std::fseek(f, 0, SEEK_SET);
                       std::vector<uint8_t> bytes(static_cast<size_t>(size));
                       const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
                       std::fclose(f);
                       if (got != bytes.size() || bytes.size() < 16u)
                           continue;
                       direct.noteGifPath(packets[i].pathId);
                       direct.processGIFPacket(bytes.data(), static_cast<uint32_t>(bytes.size()));
                       queued.noteGifPath(packets[i].pathId);
                       queued.processGIFPacket(bytes.data(), static_cast<uint32_t>(bytes.size()));
                   }
                   queued.drainQueue();

                   const std::vector<uint8_t> vramD = snapshotVramBytes(direct);
                   const std::vector<uint8_t> vramQ = snapshotVramBytes(queued);
                   if (!compareVram(t, "captured", vramD, vramQ))
                       return;
                   if (!compareRegs(t, "captured", regsDirect, regsQueued))
                       return;
                   regsDirect.dispfb1 =
                       0ull | (10ull << 9) | (static_cast<uint64_t>(GS_PSM_CT32) << 15);
                   regsQueued.dispfb1 = regsDirect.dispfb1;
                   if (!comparePresent(t, "captured", presentPixels(direct), presentPixels(queued)))
                       return;
                   uint8_t bufD[4096] = {};
                   uint8_t bufQ[4096] = {};
                   const uint32_t nD = direct.consumeLocalToHostBytes(bufD, sizeof(bufD));
                   const uint32_t nQ = queued.consumeLocalToHostBytes(bufQ, sizeof(bufQ));
                   if (nD != nQ || (nD != 0u && std::memcmp(bufD, bufQ, nD) != 0))
                   {
                       char msg[256];
                       std::snprintf(msg, sizeof(msg),
                                     "captured: consume differs n=%u/%u fnv=%08x/%08x", nD, nQ,
                                     fnv1a32(bufD, nD), fnv1a32(bufQ, nQ));
                       t.IsTrue(false, msg);
                       return;
                   }
                   char msg[128];
                   std::snprintf(msg, sizeof(msg), "captured replay: %zu packets byte-exact",
                                 packets.size());
                   t.IsTrue(true, msg);
               });

        tc.Run("queue RPCs execute at stream position after fire-and-forget writes", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GSRegisters regs{};
            initQueueTestRegs(regs);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), &regs);
            gs.setQueueEnabled(true);

            // WriteVram is fire-and-forget; the ReadVram RPC must observe it.
            gs.WriteVram(GS_PSM_CT32, 0u, 1u, 7u, 9u, 0xA5A5A5A5u);
            t.Equals(gs.ReadVram(GS_PSM_CT32, 0u, 1u, 7u, 9u), 0xA5A5A5A5u,
                     "ReadVram RPC should observe a prior queued WriteVram");

            // A packet followed by a present latch must present the packet.
            gs.writeRegister(GS_REG_FRAME_1, (0ull << 0) | (10ull << 16) |
                                                (static_cast<uint64_t>(GS_PSM_CT32) << 24));
            gs.writeRegister(GS_REG_SCISSOR_1, (639ull << 16) | (447ull << 48));
            gs.writeRegister(GS_REG_TEST_1, 0x30000ull);
            const std::vector<uint8_t> tri = makePackedTriangle(0x11, 0x22, 0x33);
            gs.processGIFPacket(tri.data(), static_cast<uint32_t>(tri.size()));
            const PresentPixels after = presentPixels(gs);
            t.IsTrue(after.ok, "present RPC after a queued packet should produce a frame");
            t.Equals(gs.ReadVram(GS_PSM_CT32, 0u, 10u, 20u, 20u), gs.ReadVram(GS_PSM_CT32, 0u, 10u, 20u, 20u),
                     "back-to-back ReadVram RPCs should agree");
            gs.drainQueue();
            t.IsTrue(true, "stream-order checks done");
        });

        tc.Run("queue handles concurrent producers and disjoint updates deterministically",
               [](TestCase &t)
               {
                   std::vector<uint8_t> vramQ(PS2_GS_VRAM_SIZE, 0u);
                   std::vector<uint8_t> vramD(PS2_GS_VRAM_SIZE, 0u);
                   GSRegisters regsQ{};
                   GSRegisters regsD{};
                   initQueueTestRegs(regsQ);
                   initQueueTestRegs(regsD);
                   GS queued;
                   queued.init(vramQ.data(), static_cast<uint32_t>(vramQ.size()), &regsQ);
                   queued.setQueueEnabled(true);

                   // Four producers, disjoint VRAM rows each: final bytes are
                   // independent of interleave, so any MPSC schedule must
                   // match the sequential direct run.
                   constexpr int kProducers = 4;
                   constexpr int kUploadsEach = 8;
                   std::vector<std::thread> producers;
                   for (int p = 0; p < kProducers; ++p)
                   {
                       producers.emplace_back([&, p]
                                              {
                                                  for (int u = 0; u < kUploadsEach; ++u)
                                                  {
                                                      const uint16_t row =
                                                          static_cast<uint16_t>(p * 32 + u * 2);
                                                      const std::vector<uint8_t> img = makeImageUpload(
                                                          0u, 0u, row, 16u, 2u,
                                                          static_cast<uint8_t>(p * 16 + u));
                                                      queued.processGIFPacket(img.data(),
                                                                              static_cast<uint32_t>(img.size()));
                                                  }
                                              });
                   }
                   for (auto &th : producers)
                       th.join();
                   queued.drainQueue();

                   GS direct;
                   direct.init(vramD.data(), static_cast<uint32_t>(vramD.size()), &regsD);
                   for (int p = 0; p < kProducers; ++p)
                   {
                       for (int u = 0; u < kUploadsEach; ++u)
                       {
                           const uint16_t row = static_cast<uint16_t>(p * 32 + u * 2);
                           const std::vector<uint8_t> img = makeImageUpload(
                               0u, 0u, row, 16u, 2u, static_cast<uint8_t>(p * 16 + u));
                           direct.processGIFPacket(img.data(), static_cast<uint32_t>(img.size()));
                       }
                   }
                   if (!compareVram(t, "concurrent", snapshotVramBytes(direct),
                                    snapshotVramBytes(queued)))
                       return;
                   t.IsTrue(true, "concurrent producers match sequential direct");
               });

        tc.Run("queue lifecycle: default off, idempotent enable, disable resumes direct",
               [](TestCase &t)
               {
                   std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
                   GS gs;
                   gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
                   t.IsFalse(gs.queueEnabled(), "fresh GS should have the queue off");
                   gs.drainQueue(); // no-op while disabled
                   t.IsTrue(gs.setQueueEnabled(true), "enable should succeed");
                   t.IsTrue(gs.queueEnabled(), "queue should be on after enable");
                   t.IsTrue(gs.setQueueEnabled(true), "re-enable should be a no-op success");
                   gs.WriteVram(GS_PSM_CT32, 0u, 1u, 1u, 1u, 0x11223344u);
                   t.Equals(gs.ReadVram(GS_PSM_CT32, 0u, 1u, 1u, 1u), 0x11223344u,
                            "queued write then read should roundtrip");
                   t.IsTrue(gs.setQueueEnabled(false), "disable should succeed");
                   t.IsFalse(gs.queueEnabled(), "queue should be off after disable");
                   gs.WriteVram(GS_PSM_CT32, 0u, 1u, 2u, 2u, 0x55667788u);
                   t.Equals(gs.ReadVram(GS_PSM_CT32, 0u, 1u, 2u, 2u), 0x55667788u,
                            "direct path should work after disable");
                   t.Equals(gs.ReadVram(GS_PSM_CT32, 0u, 1u, 1u, 1u), 0x11223344u,
                            "queued state should survive the disable");
               });

        // GB3 Part 1: guest priv stores ride the GS stream. The worker is
        // held behind a gate so a FINISH/SIGNAL packet is still queued when
        // the guest's CSR/SIGLBLID stores and the VBlank FIELD flip arrive;
        // program order (direct semantics) must still decide the final
        // regs. Before GB3 the stores applied at once on the game thread,
        // so the late packet re-set FINISH and overwrote SIGLBLID.
        tc.Run("GB3: priv stores keep program order vs queued packets (matches direct)", [](TestCase &t)
        {
            struct Final
            {
                uint64_t csr = 0, siglblid = 0, dispfb1 = 0, imr = 0;
                uint64_t privWrites = 0;
            };
            auto runSequence = [](bool queued) -> Final
            {
                PS2Memory mem;
                if (!mem.initialize())
                    return {};
                GS gs;
                gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
                mem.setGsFrontend(&gs);
                std::atomic<bool> gate{!queued};
                if (queued)
                {
                    gs.setQueueEnabled(true);
                    gs.privWrite([&gate]()
                                 {
                        while (!gate.load(std::memory_order_acquire))
                            std::this_thread::yield(); });
                }
                // Packet: A+D FINISH, then A+D SIGNAL(id=0x1234, mask=0xFFFF).
                std::vector<uint8_t> pkt;
                appendGifTag(pkt, 2u, kFlgPacked, 1u, 0xEull);
                appendGifAd(pkt, 0u, GS_REG_FINISH);
                appendGifAd(pkt, 0x0000FFFF00001234ull, GS_REG_SIGNAL);
                gs.processGIFPacket(pkt.data(), static_cast<uint32_t>(pkt.size()));
                // Guest: clear SIGNAL|FINISH (W1C), write SIGLBLID, DISPFB1, IMR.
                mem.write64(0x12001000u, 0x3ull);
                mem.write64(0x12001080u, 0xABCD00005678ull);
                mem.write32(0x12000070u, 0x1234u);
                mem.write64(0x12001010u, 0x7F00ull);
                // VBlank FIELD flip (odd tick), the EeScheduler path.
                GSRegisters &regs = mem.gs();
                mem.gsPrivStore([&regs]()
                                { regs.csr.fetch_or(0x2000ull, std::memory_order_acq_rel); });
                gate.store(true, std::memory_order_release);
                gs.drainQueue();
                mem.gsPrivSync();
                Final f;
                f.csr = regs.csr.load();
                f.siglblid = regs.siglblid.load();
                f.dispfb1 = regs.dispfb1;
                f.imr = regs.imr;
                f.privWrites = gs.privWriteCount();
                gs.setQueueEnabled(false);
                mem.setGsFrontend(nullptr);
                return f;
            };
            const Final d = runSequence(false);
            const Final q = runSequence(true);
            t.Equals(d.csr & 0x3ull, 0ull, "direct: guest W1C clears SIGNAL|FINISH set by the earlier packet");
            t.Equals(d.csr & 0x2000ull, 0x2000ull, "direct: FIELD set by the VBlank flip");
            t.Equals(q.csr, d.csr, "queued CSR == direct CSR (in-stream W1C + FIELD)");
            t.Equals(q.siglblid, d.siglblid, "queued SIGLBLID == direct (guest store after SIGNAL)");
            t.Equals(d.siglblid, 0xABCD00005678ull, "direct SIGLBLID = the guest store");
            t.Equals(q.dispfb1, d.dispfb1, "queued DISPFB1 == direct");
            t.Equals(q.imr, d.imr, "queued IMR == direct");
            t.Equals(d.privWrites, 5ull, "direct counts 5 priv stores");
            t.Equals(q.privWrites, 6ull, "queued counts 5 priv stores + the gate");
        });

        tc.Run("VPL1 GIF stage keeps the unit's GS stream order", [](TestCase &t)
        {
            // The unit's arbiter work (PATH1/2/3 submits, per-XGKICK drains,
            // the PATH1-before-PATH3 sort) and R2 priv stores must reach the
            // GS worker in the same order with the GIF stage on as with the
            // serial unit (pktSeq digest), with and without host jitter, and
            // nothing may bypass the stage.
            auto run = [](bool gifStage, uint32_t jitterUs)
            {
                ps2_mtvu::setModeForTest(ps2_mtvu::Mode::Threaded, true, jitterUs);
                PS2Memory mem;
                mem.initialize();
                GS gs;
                gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
                gs.setRasterBackend(ps2x_gs_external::create(&mem.gs()));
                gs.setQueueEnabled(true);
                gs.setPktSeqEnabled(true);
                mem.setGsFrontend(&gs);
                GifArbiter arbiter([&gs](const uint8_t *data, uint32_t size) { gs.processGIFPacket(data, size); });
                mem.setGifArbiter(&arbiter);
                if (gifStage)
                    ps2_mtvu::startGifStage([&mem](ps2_mtvu::GifOp &op) { mem.execGifStageOp(op); });
                auto label = [](uint64_t v)
                {
                    std::vector<uint8_t> b;
                    appendGifTag(b, 1u, kFlgPacked, 1u, 0xEull);
                    appendGifAd(b, v, GS_REG_LABEL);
                    return b;
                };
                for (uint32_t i = 0; i < 200u; ++i)
                {
                    ps2_mtvu::submit([&mem, i, label]()
                    {
                        const std::vector<uint8_t> p1 = label(0x100000ull + i);
                        mem.submitGifPacket(GifPathId::Path1, p1.data(), static_cast<uint32_t>(p1.size()));
                        if (i % 3u == 0u)
                        {
                            const std::vector<uint8_t> p2 = label(0x200000ull + i);
                            mem.submitGifPacket(GifPathId::Path2, p2.data(), static_cast<uint32_t>(p2.size()), true,
                                                (i & 1u) != 0u);
                        }
                        if (i % 5u == 0u)
                        {
                            // Undrained PATH3 then PATH1: the drain sorts PATH1 first.
                            const std::vector<uint8_t> p3 = label(0x300000ull + i);
                            mem.submitGifPacket(GifPathId::Path3, p3.data(), static_cast<uint32_t>(p3.size()), false);
                            const std::vector<uint8_t> p1b = label(0x400000ull + i);
                            mem.submitGifPacket(GifPathId::Path1, p1b.data(), static_cast<uint32_t>(p1b.size()), true);
                        }
                    }, 64u, 0u);
                    if (i % 7u == 0u)
                        mem.write64(0x12000070u, 0x1000ull + i); // DISPFB1: an R2 priv-store job
                }
                ps2_mtvu::syncAll();
                gs.drainQueue();
                const std::array<uint64_t, 3> out{gs.pktSeqSnapshot(), gs.pktSeqSnapshotCommands(),
                                                  mem.gs().dispfb1};
                if (gifStage)
                    ps2_mtvu::stopGifStage();
                mem.setGifArbiter(nullptr);
                gs.setQueueEnabled(false);
                mem.setGsFrontend(nullptr);
                ps2_mtvu::setModeForTest(ps2_mtvu::Mode::Off);
                return out;
            };
            const uint64_t escapes0 = ps2_mtvu::detail::gifStage().nEscapes.load();
            const auto serial = run(false, 0u);
            t.IsTrue(serial[1] >= 370u, "digest covers the packet stream and priv stores");
            for (uint32_t jitterUs : {0u, 200u})
            {
                const auto staged = run(true, jitterUs);
                t.Equals(staged[0], serial[0], "GIF-stage GS stream digest == serial unit");
                t.Equals(staged[1], serial[1], "GIF-stage GS command count == serial unit");
                t.Equals(staged[2], serial[2], "GIF-stage final DISPFB1 == serial unit");
            }
            t.Equals(ps2_mtvu::detail::gifStage().nEscapes.load(), escapes0, "no GS enqueue bypassed the stage");
        });

        tc.Run("queue backpressure: a full ring blocks producers until drained", [](TestCase &t)
        {
            std::atomic<bool> gate{false};
            std::atomic<bool> entered{false};
            std::atomic<int> executed{0};
            GsWorker worker(2u, 64u,
                            [&](GsCommand &)
                            {
                                entered.store(true, std::memory_order_release);
                                while (!gate.load(std::memory_order_acquire))
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                executed.fetch_add(1, std::memory_order_relaxed);
                            });
            worker.start();
            // The worker pops the first command and parks in the gated
            // handler; two more enqueues then fill the 2-deep ring and the
            // fourth must block. (GP4 H6: pops are batched, so the first
            // enqueue is fenced on handler entry — otherwise the maiden
            // batch could take several and the ring would not be full.)
            {
                GsCommand c;
                c.kind = GsCmdKind::GifPacket;
                c.bytes.resize(16u, 1u);
                worker.enqueue(std::move(c));
            }
            for (int i = 0; i < 200 && !entered.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            t.IsTrue(entered.load(std::memory_order_acquire), "worker should park in the gated handler");
            for (uint8_t i = 2; i <= 3; ++i)
            {
                GsCommand c;
                c.kind = GsCmdKind::GifPacket;
                c.bytes.resize(16u, i);
                worker.enqueue(std::move(c));
            }
            std::atomic<bool> fourthDone{false};
            std::thread fourth(
                [&]
                {
                    GsCommand c;
                    c.kind = GsCmdKind::GifPacket;
                    c.bytes.resize(16u, 4u);
                    worker.enqueue(std::move(c));
                    fourthDone.store(true, std::memory_order_release);
                });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            t.IsFalse(fourthDone.load(std::memory_order_acquire),
                      "fourth enqueue should block while the 2-deep ring is full");
            t.Equals(worker.pendingCount(), static_cast<size_t>(2),
                     "full ring should hold 2 descriptors");
            gate.store(true, std::memory_order_release);
            fourth.join();
            worker.stop();
            t.IsTrue(fourthDone.load(std::memory_order_acquire),
                     "blocked producer should proceed once the worker drains");
            t.Equals(executed.load(std::memory_order_relaxed), 4,
                     "all four commands should execute");
        });

        tc.Run("PT2 enqueue backpressure wait accumulates into the caller sink", [](TestCase &t)
        {
            // Same 2-deep gated ring as above; the blocked producer sets its
            // own thread-local sink (as the EE does around run()).
            std::atomic<bool> gate{false};
            std::atomic<bool> entered{false};
            GsWorker worker(2u, 64u,
                            [&](GsCommand &)
                            {
                                entered.store(true, std::memory_order_release);
                                while (!gate.load(std::memory_order_acquire))
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            });
            worker.start();
            {
                GsCommand c;
                c.kind = GsCmdKind::GifPacket;
                c.bytes.resize(16u, 1u);
                worker.enqueue(std::move(c));
            }
            for (int i = 0; i < 200 && !entered.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            t.IsTrue(entered.load(std::memory_order_acquire), "worker should park in the gated handler");
            uint64_t sink = 0;
            GsWorker::setEnqueueWaitSink(&sink);
            for (uint8_t i = 2; i <= 3; ++i)
            {
                GsCommand c;
                c.kind = GsCmdKind::GifPacket;
                c.bytes.resize(16u, i);
                worker.enqueue(std::move(c));
            }
            t.Equals(sink, static_cast<uint64_t>(0), "unblocked enqueues should not touch the sink");
            GsWorker::setEnqueueWaitSink(nullptr);
            std::atomic<bool> fourthDone{false};
            uint64_t blockedSink = 0;
            std::thread fourth(
                [&]
                {
                    GsWorker::setEnqueueWaitSink(&blockedSink);
                    GsCommand c;
                    c.kind = GsCmdKind::GifPacket;
                    c.bytes.resize(16u, 4u);
                    worker.enqueue(std::move(c));
                    GsWorker::setEnqueueWaitSink(nullptr);
                    fourthDone.store(true, std::memory_order_release);
                });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            t.IsFalse(fourthDone.load(std::memory_order_acquire), "fourth enqueue should still block");
            t.Equals(blockedSink, static_cast<uint64_t>(0),
                     "sink should stay zero until the wait completes");
            gate.store(true, std::memory_order_release);
            fourth.join();
            worker.stop();
            t.IsTrue(fourthDone.load(std::memory_order_acquire), "blocked producer should proceed");
            t.IsTrue(blockedSink > 0u, "queue-full wait should accumulate into the sink");
        });

        // N8D7M12 Part 5F4P2: worker-consumption fingerprint properties.
        // Property-only: compares digests across instances, never
        // reimplements the hash.
        tc.Run("pktseq fingerprint is stable, order- and payload-sensitive, default-off silent",
               [](TestCase &t)
        {
            auto runFixed = [](bool swapOrder, bool flipPayload, bool enable) {
                struct Out
                {
                    uint64_t seq = 0;
                    uint64_t commands = 0;
                    bool enabled = false;
                };
                std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
                GSRegisters regs{};
                initQueueTestRegs(regs);
                GS gs;
                gs.init(vram.data(), static_cast<uint32_t>(vram.size()), &regs);
                if (!enable)
                {
                    if (gs.pktSeqEnabled())
                        return Out{1u, 1u, true}; // default must be off
                }
                else
                {
                    gs.setPktSeqEnabled(true);
                }
                gs.setQueueEnabled(true);
                std::vector<uint8_t> tri = makePackedTriangle(0xC0, 0x20, 0x40);
                if (flipPayload)
                    tri[tri.size() - 1] ^= 0xFFu;
                gs.noteGifPath(GifPathId::Path3);
                gs.processGIFPacket(tri.data(), static_cast<uint32_t>(tri.size()));
                if (!swapOrder)
                {
                    gs.WriteVram(GS_PSM_CT32, 0u, 1u, 10u, 10u, 0x11111111u);
                    gs.WriteVram(GS_PSM_CT32, 0u, 1u, 11u, 10u, 0x22222222u);
                }
                else
                {
                    gs.WriteVram(GS_PSM_CT32, 0u, 1u, 11u, 10u, 0x22222222u);
                    gs.WriteVram(GS_PSM_CT32, 0u, 1u, 10u, 10u, 0x11111111u);
                }
                gs.writeRegister(GS_REG_TEST_1, 0x30000ull);
                gs.privWrite([] {});
                gs.drainQueue();
                Out o;
                o.seq = gs.pktSeqSnapshot();
                o.commands = gs.pktSeqSnapshotCommands();
                o.enabled = gs.pktSeqEnabled();
                if (enable)
                {
                    // Quiescent drain must leave the snapshot stable.
                    gs.drainQueue();
                    if (gs.pktSeqSnapshot() != o.seq ||
                        gs.pktSeqSnapshotCommands() != o.commands)
                    {
                        o.seq ^= 0x8000000000000000ull; // mark instability
                    }
                }
                return o;
            };
            const auto a = runFixed(false, false, true);
            const auto b = runFixed(false, false, true);
            t.IsTrue(a.enabled && b.enabled, "pktseq should report enabled after opt-in");
            t.IsTrue(a.commands != 0u, "enabled fingerprint should count commands");
            t.Equals(a.seq, b.seq, "same consumed sequence should give the same digest");
            t.Equals(a.commands, b.commands, "same consumed sequence should give the same count");
            const auto swapped = runFixed(true, false, true);
            t.Equals(swapped.commands, a.commands, "reordered pair should keep the same count");
            t.IsTrue(swapped.seq != a.seq, "reordering two distinguishable writes should change the digest");
            const auto flipped = runFixed(false, true, true);
            t.IsTrue(flipped.seq != a.seq, "one payload byte change should change the digest");
            const auto off = runFixed(false, false, false);
            t.IsTrue(!off.enabled, "pktseq should be off by default");
            t.Equals(off.seq, 0ull, "default-off snapshot should stay silent");
            t.Equals(off.commands, 0ull, "default-off count should stay silent");
        });

        // GF1 H3: deferred wakes deliver every command, in order, with far
        // fewer wakes than batches; job-end flushes leave nothing stale.
        tc.Run("GF1 deferred wakes keep FIFO order and cut wakes", [](TestCase &t)
        {
            std::vector<uint32_t> seen;
            GsWorker worker(0u, 0u, [&](GsCommand &cmd) { seen.push_back(cmd.u32b); });
            worker.setDeferredWakes(64u, 256u * 1024u);
            worker.start();
            uint32_t next = 0;
            uint64_t rng = 0x9E3779B97F4A7C15ull;
            for (int job = 0; job < 200; ++job)
            {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                const int drains = 1 + static_cast<int>(rng % 40u);
                for (int d = 0; d < drains; ++d)
                {
                    worker.beginBatch();
                    GsCommand c;
                    c.kind = GsCmdKind::GifPacket;
                    c.u32b = next++;
                    c.bytes.resize(16u + (rng % 64u) * 16u, 0x5Au);
                    worker.enqueue(std::move(c));
                    worker.endBatch(true);
                    if ((rng >> 20) % 7u == 0u)
                        std::this_thread::sleep_for(std::chrono::microseconds(rng % 300u));
                }
                worker.flushWake(); // unit job end
            }
            worker.stop();
            t.Equals(static_cast<uint64_t>(seen.size()), static_cast<uint64_t>(next), "every command should execute");
            bool ordered = true;
            for (size_t i = 0; i < seen.size(); ++i)
                ordered = ordered && seen[i] == static_cast<uint32_t>(i);
            t.IsTrue(ordered, "commands should execute in FIFO order");
            t.Equals(worker.watchdogCount(), 0ull, "no deferred wake should go stale with job-end flushes");
            t.IsTrue(worker.wakeCount() < static_cast<uint64_t>(next), "deferred wakes should notify less than once per drain");
        });

        // GF1 H3: a batch that fills the queue while the worker sleeps must
        // not hang (the producer wakes the worker before it blocks).
        tc.Run("GF1 a batch that fills the queue wakes the worker before blocking", [](TestCase &t)
        {
            std::atomic<int> executed{0};
            GsWorker worker(8u, 0u, [&](GsCommand &) { executed.fetch_add(1, std::memory_order_relaxed); });
            worker.setDeferredWakes(1024u, 64u * 1024u * 1024u);
            worker.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); // worker asleep on an empty queue
            worker.beginBatch();
            for (int i = 0; i < 100; ++i)
            {
                GsCommand c;
                c.kind = GsCmdKind::GifPacket;
                c.bytes.resize(16u, 1u);
                worker.enqueue(std::move(c));
            }
            worker.endBatch(true);
            worker.flushWake();
            worker.stop();
            t.Equals(executed.load(std::memory_order_relaxed), 100, "all 100 commands should execute");
        });

        // GF1 H3: an RPC from another thread is never held back by an open
        // unit batch (its caller waits on it).
        tc.Run("GF1 an RPC enqueued during another thread's batch is delivered", [](TestCase &t)
        {
            GsWorker worker(0u, 0u, [](GsCommand &) {});
            worker.setDeferredWakes(64u, 256u * 1024u);
            worker.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            worker.beginBatch(); // the unit's batch stays open
            std::atomic<bool> done{false};
            std::thread other(
                [&]
                {
                    GsCommand c;
                    c.kind = GsCmdKind::Fence;
                    c.rpc = std::make_shared<GsRpcBase>();
                    std::shared_ptr<GsRpcBase> rpc = c.rpc;
                    worker.enqueue(std::move(c));
                    rpc->wait();
                    done.store(true, std::memory_order_release);
                });
            for (int i = 0; i < 200 && !done.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            t.IsTrue(done.load(std::memory_order_acquire), "RPC should complete while the batch is open");
            worker.endBatch(true);
            other.join();
            worker.stop();
        });

        // MP1 L2: lean handoff + thread-local unit batches deliver every
        // command in FIFO order with fewer wakes, and leave nothing stale
        // at job-end flushes. (CU4 B1: lean is unconditional; the GF1
        // non-lean leg is deleted.)
        tc.Run("MP1 lean local batches keep FIFO order and cut wakes", [](TestCase &t)
        {
            auto run = [](std::vector<uint32_t> &seen, uint64_t &wakes, uint64_t &watchdog, bool &quiet)
            {
                GsWorker worker(0u, 0u, [&](GsCommand &cmd) { seen.push_back(cmd.u32b); });
                worker.setDeferredWakes(64u, 256u * 1024u);
                worker.start();
                uint32_t next = 0;
                uint64_t rng = 0x9E3779B97F4A7C15ull;
                std::thread unit(
                    [&]
                    {
                        for (int job = 0; job < 200; ++job)
                        {
                            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                            const int drains = 1 + static_cast<int>(rng % 40u);
                            for (int d = 0; d < drains; ++d)
                            {
                                GsWorker::beginLocalBatch();
                                GsCommand c;
                                c.kind = GsCmdKind::GifPacket;
                                c.u32b = next++;
                                c.bytes.resize(16u + (rng % 64u) * 16u, 0x5Au);
                                worker.enqueue(std::move(c));
                                GsWorker::endLocalBatch();
                                if ((rng >> 20) % 7u == 0u)
                                    std::this_thread::sleep_for(std::chrono::microseconds(rng % 300u));
                            }
                            worker.flushWake(); // unit job end
                        }
                    });
                unit.join();
                for (int i = 0; i < 400 && !worker.isQuiescent(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                quiet = worker.isQuiescent();
                worker.stop();
                wakes = worker.wakeCount();
                watchdog = worker.watchdogCount();
                return next;
            };
            std::vector<uint32_t> seenLean;
            uint64_t wakesLean = 0, wdLean = 0;
            bool quietLean = false;
            const uint32_t nLean = run(seenLean, wakesLean, wdLean, quietLean);
            t.Equals(static_cast<uint64_t>(seenLean.size()), static_cast<uint64_t>(nLean), "every command should execute");
            bool ordered = true;
            for (size_t i = 0; i < seenLean.size(); ++i)
                ordered = ordered && seenLean[i] == static_cast<uint32_t>(i);
            t.IsTrue(ordered, "lean commands should execute in FIFO order");
            t.Equals(wdLean, 0ull, "no deferred wake should go stale with job-end flushes");
            t.IsTrue(quietLean, "the worker should reach quiescence after the last flush");
        });

        // MP1 L2: a local batch that fills the queue while the worker sleeps
        // wakes it before blocking (lean notifies a sleeping worker only).
        tc.Run("MP1 lean local batch that fills the queue wakes the worker", [](TestCase &t)
        {
            std::atomic<int> executed{0};
            GsWorker worker(8u, 0u, [&](GsCommand &) { executed.fetch_add(1, std::memory_order_relaxed); });
            worker.setDeferredWakes(1024u, 64u * 1024u * 1024u);
            worker.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); // worker asleep on an empty queue
            GsWorker::beginLocalBatch();
            for (int i = 0; i < 100; ++i)
            {
                GsCommand c;
                c.kind = GsCmdKind::GifPacket;
                c.bytes.resize(16u, 1u);
                worker.enqueue(std::move(c));
            }
            GsWorker::endLocalBatch();
            worker.flushWake();
            worker.stop();
            t.Equals(executed.load(std::memory_order_relaxed), 100, "all 100 commands should execute");
        });

        // MP1 L2: an RPC enqueued inside a local batch (same thread) and one
        // from another thread both complete while the batch is open.
        tc.Run("MP1 lean RPCs complete during a local batch", [](TestCase &t)
        {
            GsWorker worker(0u, 0u, [](GsCommand &) {});
            worker.setDeferredWakes(64u, 256u * 1024u);
            worker.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            GsWorker::beginLocalBatch();
            {
                GsCommand pkt;
                pkt.kind = GsCmdKind::GifPacket;
                pkt.bytes.resize(16u, 1u);
                worker.enqueue(std::move(pkt)); // stays deferred
                GsCommand c;
                c.kind = GsCmdKind::Fence;
                c.rpc = std::make_shared<GsRpcBase>();
                std::shared_ptr<GsRpcBase> rpc = c.rpc;
                worker.enqueue(std::move(c));
                rpc->wait();
            }
            std::atomic<bool> done{false};
            std::thread other(
                [&]
                {
                    GsCommand c;
                    c.kind = GsCmdKind::Fence;
                    c.rpc = std::make_shared<GsRpcBase>();
                    std::shared_ptr<GsRpcBase> rpc = c.rpc;
                    worker.enqueue(std::move(c));
                    rpc->wait();
                    done.store(true, std::memory_order_release);
                });
            for (int i = 0; i < 200 && !done.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            t.IsTrue(done.load(std::memory_order_acquire), "another thread's RPC should complete during the local batch");
            GsWorker::endLocalBatch();
            other.join();
            worker.flushWake();
            worker.stop();
            t.Equals(worker.enqueuedCount(), worker.executedCount(), "every command should execute");
        });

        // GPK1: staged publish on the GIF stage thread keeps FIFO order,
        // holds commands until a publish point, and publishes before an RPC,
        // flushWake and at the stage cap.
        tc.Run("GPK1 staged publish keeps FIFO order and publishes at flush points", [](TestCase &t)
        {
            std::vector<uint32_t> seen;
            std::mutex seenMutex;
            GsWorker worker(0u, 0u, [&](GsCommand &cmd)
                            {
                                std::lock_guard<std::mutex> lock(seenMutex);
                                seen.push_back(cmd.u32b);
                            });
            worker.setDeferredWakes(64u, 256u * 1024u);
            worker.start();
            GsWorker::setStagedPublish(true);
            uint32_t next = 0;
            bool heldUntilFlush = false, rpcAfterStaged = false, capPublished = false;
            std::thread gif(
                [&]
                {
                    ps2_mtvu::detail::g_gifTid.store(std::this_thread::get_id(), std::memory_order_relaxed);
                    auto packet = [&]
                    {
                        GsWorker::beginLocalBatch();
                        GsCommand c;
                        c.kind = GsCmdKind::GifPacket;
                        c.u32b = next++;
                        c.bytes.resize(16u, 0x5Au);
                        worker.enqueue(std::move(c));
                        GsWorker::endLocalBatch();
                    };
                    // Held: nothing reaches the queue before a publish point.
                    for (int i = 0; i < 10; ++i)
                        packet();
                    heldUntilFlush = worker.enqueuedCount() == 0u;
                    GsWorker::flushStaged();
                    // An RPC publishes the stage first, then runs after it.
                    for (int i = 0; i < 5; ++i)
                        packet();
                    GsCommand fence;
                    fence.kind = GsCmdKind::Fence;
                    fence.u32b = 0xFFFFFFFFu;
                    fence.rpc = std::make_shared<GsRpcBase>();
                    std::shared_ptr<GsRpcBase> rpc = fence.rpc;
                    worker.enqueue(std::move(fence));
                    rpc->wait();
                    {
                        std::lock_guard<std::mutex> lock(seenMutex);
                        rpcAfterStaged = seen.size() == 16u && seen.back() == 0xFFFFFFFFu;
                    }
                    // The cap publishes without a flush call.
                    const uint64_t before = worker.enqueuedCount();
                    for (size_t i = 0; i < GsWorker::kStageMaxCommands; ++i)
                        packet();
                    capPublished = worker.enqueuedCount() == before + GsWorker::kStageMaxCommands;
                    // Random runs, published by flushWake (job end).
                    uint64_t rng = 0x9E3779B97F4A7C15ull;
                    for (int job = 0; job < 200; ++job)
                    {
                        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                        const int drains = 1 + static_cast<int>(rng % 90u);
                        for (int d = 0; d < drains; ++d)
                            packet();
                        worker.flushWake();
                    }
                    ps2_mtvu::detail::g_gifTid.store(std::thread::id{}, std::memory_order_relaxed);
                });
            gif.join();
            GsWorker::setStagedPublish(false);
            for (int i = 0; i < 400 && !worker.isQuiescent(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            worker.stop();
            t.IsTrue(heldUntilFlush, "staged commands should stay off the queue until a publish point");
            t.IsTrue(rpcAfterStaged, "an RPC should publish the stage first and run after it");
            t.IsTrue(capPublished, "a full stage should publish itself");
            t.Equals(worker.enqueuedCount(), worker.executedCount(), "every command should execute");
            bool ordered = true;
            uint32_t expect = 0;
            for (const uint32_t v : seen)
            {
                if (v == 0xFFFFFFFFu)
                    continue;
                ordered = ordered && v == expect++;
            }
            t.IsTrue(ordered && expect == next, "staged commands should execute in FIFO order");
            t.Equals(worker.watchdogCount(), 0ull, "no deferred wake should go stale");
        });

        // GF1 H1/H2: one command carrying the path gives the same consumed
        // sequence and VRAM as NoteGifPath + GifPacket.
        tc.Run("GF1 processGIFPacketWithPath matches noteGifPath + processGIFPacket", [](TestCase &t)
        {
            auto run = [](bool folded, std::vector<uint8_t> &vramOut, uint64_t &seq, uint64_t &cmds)
            {
                std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
                GSRegisters regs{};
                initQueueTestRegs(regs);
                GS gs;
                gs.init(vram.data(), static_cast<uint32_t>(vram.size()), &regs);
                gs.setQueueEnabled(true);
                gs.setPktSeqEnabled(true);
                gs.writeRegister(GS_REG_TEST_1, 0x30000ull);
                const std::vector<std::vector<uint8_t>> pkts = {
                    makePackedTriangle(200u, 10u, 30u), makeReglistPoints(),
                    makeImageUpload(0x100u, 0u, 0u, 8u, 8u, 3u), makePackedTriangle(5u, 250u, 60u)};
                const GifPathId paths[] = {GifPathId::Path1, GifPathId::Path2, GifPathId::Path3, GifPathId::Path1};
                for (size_t i = 0; i < pkts.size(); ++i)
                {
                    const bool note = i != 1u; // one packet without a note
                    std::vector<uint8_t> bytes = pkts[i];
                    if (folded)
                    {
                        gs.processGIFPacketWithPath(paths[i], note, bytes);
                    }
                    else
                    {
                        if (note)
                            gs.noteGifPath(paths[i]);
                        gs.processGIFPacket(bytes.data(), static_cast<uint32_t>(bytes.size()));
                    }
                }
                gs.drainQueue();
                seq = gs.pktSeqSnapshot();
                cmds = gs.pktSeqSnapshotCommands();
                vramOut = snapshotVramBytes(gs);
            };
            std::vector<uint8_t> vA, vB;
            uint64_t sA = 0, sB = 0, cA = 0, cB = 0;
            run(false, vA, sA, cA);
            run(true, vB, sB, cB);
            t.IsTrue(cA != 0u, "digest should count commands");
            t.Equals(sB, sA, "folded path command should give the same consumed digest");
            t.Equals(cB, cA, "folded path command should give the same consumed count");
            t.IsTrue(vA == vB, "folded path command should give the same VRAM");
        });

        // GP4 H5: the packet pool reuses buffers, honors its caps, and stays
        // out of the way when disabled.
        tc.Run("GP4 H5 pool acquire/release roundtrip and caps", [](TestCase &t)
        {
            GsPacketPool pool;
            t.IsTrue(pool.acquire(64u).empty(), "disabled acquire should return empty");
            std::vector<uint8_t> fresh(64u, 0xABu);
            pool.release(std::move(fresh));
            t.Equals(pool.pooledCount(), 0u, "disabled release should not pool");
            pool.setEnabled(true);
            std::vector<uint8_t> buf(128u);
            for (size_t i = 0; i < buf.size(); ++i)
                buf[i] = static_cast<uint8_t>(i);
            pool.release(std::move(buf));
            t.Equals(pool.pooledCount(), 1u, "enabled release should pool");
            std::vector<uint8_t> got = pool.acquire(64u);
            t.IsTrue(got.capacity() >= 128u, "acquire should reuse a pooled buffer with room");
            t.Equals(pool.pooledCount(), 0u, "acquire should pop the buffer");
            got.resize(64u); // acquired buffers are empty; callers size them
            for (size_t i = 0; i < 64u; ++i)
                got[i] = static_cast<uint8_t>(0xFFu - i);
            pool.release(std::move(got));
            std::vector<uint8_t> got2 = pool.acquire(64u);
            got2.resize(64u);
            for (size_t i = 0; i < 64u; ++i)
                got2[i] = static_cast<uint8_t>(i + 1u);
            bool exact = got2.size() == 64u;
            for (size_t i = 0; i < 64u && exact; ++i)
                exact = got2[i] == static_cast<uint8_t>(i + 1u);
            t.IsTrue(exact, "reused buffer should carry exactly the overwritten bytes");
            // Caps: over-count and oversize releases are dropped (freed).
            for (size_t i = 0; i < GsPacketPool::kMaxBuffers + 8u; ++i)
            {
                std::vector<uint8_t> b(1024u, 0x5Au);
                pool.release(std::move(b));
            }
            t.IsTrue(pool.pooledCount() <= GsPacketPool::kMaxBuffers, "pool should honor the buffer cap");
            t.IsTrue(pool.acquire(GsPacketPool::kMaxBufferBytes + 1u).empty(),
                     "oversize acquire should bypass the pool");
            std::vector<uint8_t> big(GsPacketPool::kMaxBufferBytes + 1u, 0x11u);
            const size_t before = pool.pooledCount();
            pool.release(std::move(big));
            t.Equals(pool.pooledCount(), before, "oversize release should bypass the pool");
        });

        // MP1 L3: the pool keeps the larger buffer when full, honors its
        // caps, and then serves every size from the pool. (CU4 B1: the only
        // caps; the pre-MP1 leg is deleted.)
        tc.Run("MP1 L3 pool keeps larger buffers under its caps", [](TestCase &t)
        {
            GsPacketPool pool;
            pool.setEnabled(true);
            for (size_t i = 0; i < GsPacketPool::kMaxBuffers; ++i)
            {
                std::vector<uint8_t> b(64u, 0x5Au);
                pool.release(std::move(b));
            }
            t.Equals(pool.pooledCount(), GsPacketPool::kMaxBuffers, "pool should fill to its buffer cap");
            for (size_t i = 0; i < GsPacketPool::kMaxBuffers; ++i)
            {
                std::vector<uint8_t> b(64u * 1024u, 0x33u);
                pool.release(std::move(b));
                t.IsTrue(b.capacity() == 0u || b.capacity() < 64u * 1024u,
                         "a swapped release should hand back the smaller buffer");
            }
            t.IsTrue(pool.pooledCount() <= GsPacketPool::kMaxBuffers, "pool should honor the buffer cap");
            t.IsTrue(pool.pooledBytes() <= GsPacketPool::kMaxBytes, "pool should honor the byte cap");
            std::vector<uint8_t> got = pool.acquire(48u * 1024u);
            t.IsTrue(got.capacity() >= 48u * 1024u, "a large request should now hit the pool");
        });

        // MP1 L3: drain orders queues of up to two packets with the one
        // compare stable_sort would make (CU4 B1: unconditional; the
        // stable_sort-every-drain path is deleted). Three or more packets
        // go through stable_sort itself.
        tc.Run("MP1 L3 arbiter drain order", [](TestCase &t)
        {
            auto drainIds = [](const std::vector<std::pair<GifPathId, bool>> &paths)
            {
                std::vector<uint32_t> order;
                GifArbiter arb([&](const uint8_t *data, uint32_t)
                               {
                                   uint32_t id = 0;
                                   std::memcpy(&id, data + 16, sizeof(id));
                                   order.push_back(id);
                               });
                uint32_t id = 0;
                for (const auto &[path, directHl] : paths)
                {
                    std::vector<uint8_t> pkt(32u, 0u);
                    std::memcpy(pkt.data() + 16, &id, sizeof(id));
                    ++id;
                    arb.submit(path, pkt.data(), static_cast<uint32_t>(pkt.size()), directHl);
                }
                arb.drain();
                return order;
            };
            // Path priority: Path1 drains before Path3 either way round.
            t.IsTrue((drainIds({{GifPathId::Path3, false}, {GifPathId::Path1, false}}) ==
                       std::vector<uint32_t>{1u, 0u}),
                      "Path1 should drain before Path3");
            t.IsTrue((drainIds({{GifPathId::Path1, false}, {GifPathId::Path3, false}}) ==
                       std::vector<uint32_t>{0u, 1u}),
                      "already-ordered pair should keep its order");
            // DIRECTHL cannot preempt a PATH3 IMAGE transfer.
            auto imagePkt = [](GifPathId path, bool image, uint32_t id)
            {
                std::vector<uint8_t> pkt(32u, 0u);
                if (image)
                    pkt[7] = 0x08u; // tag FLG=2 (IMAGE)
                std::memcpy(pkt.data() + 16, &id, sizeof(id));
                return pkt;
            };
            {
                std::vector<uint32_t> order;
                GifArbiter arb([&](const uint8_t *data, uint32_t)
                               {
                                   uint32_t id = 0;
                                   std::memcpy(&id, data + 16, sizeof(id));
                                   order.push_back(id);
                               });
                std::vector<uint8_t> image = imagePkt(GifPathId::Path3, true, 0u);
                std::vector<uint8_t> hl = imagePkt(GifPathId::Path2, false, 1u);
                arb.submit(GifPathId::Path2, hl.data(), static_cast<uint32_t>(hl.size()), true);
                arb.submit(GifPathId::Path3, image.data(), static_cast<uint32_t>(image.size()), false);
                arb.drain();
                t.IsTrue(order == std::vector<uint32_t>{0u, 1u},
                         "PATH3 IMAGE should drain before DIRECTHL");
            }
            // Random mixes drain deterministically and preserve every packet.
            for (uint64_t seed : {0x9E3779B97F4A7C15ull, 0x1234567ull, 0xDEADBEEFCAFEull})
            {
                std::vector<uint32_t> a, b;
                uint32_t submitted = 0;
                auto run = [](uint64_t s, std::vector<uint32_t> &order, uint32_t &out)
                {
                    GifArbiter arb([&](const uint8_t *data, uint32_t)
                                   {
                                       uint32_t id = 0;
                                       std::memcpy(&id, data + 16, sizeof(id));
                                       order.push_back(id);
                                   });
                    uint64_t rng = s;
                    uint32_t id = 0;
                    for (int round = 0; round < 300; ++round)
                    {
                        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                        const int n = 1 + static_cast<int>(rng % 5u);
                        for (int k = 0; k < n; ++k)
                        {
                            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                            std::vector<uint8_t> pkt(32u, 0u);
                            const GifPathId path = static_cast<GifPathId>(1u + rng % 3u);
                            if (path == GifPathId::Path3 && (rng >> 8) % 2u == 0u)
                                pkt[7] = 0x08u; // tag FLG=2 (IMAGE)
                            std::memcpy(pkt.data() + 16, &id, sizeof(id));
                            ++id;
                            arb.submit(path, pkt.data(), static_cast<uint32_t>(pkt.size()), (rng >> 12) % 2u == 0u);
                        }
                        arb.drain();
                    }
                    out = id;
                };
                uint32_t nA = 0, nB = 0;
                run(seed, a, nA);
                run(seed, b, nB);
                t.IsTrue(!a.empty() && a == b, "same seed should drain in the same order");
                t.Equals(static_cast<uint64_t>(a.size()), static_cast<uint64_t>(nA),
                         "every submitted packet should drain exactly once");
                std::vector<uint32_t> sorted = a;
                std::sort(sorted.begin(), sorted.end());
                bool ids = sorted.size() == static_cast<size_t>(nA);
                for (size_t i = 0; i < sorted.size() && ids; ++i)
                    ids = sorted[i] == static_cast<uint32_t>(i);
                t.IsTrue(ids, "drained ids should be exactly the submitted set");
            }
        });

        // GP4 H5: pooled handoff gives the same consumed stream and VRAM as
        // the direct handoff (pool on vs off, identical submits).
        tc.Run("GP4 H5 pooled queue matches direct queue", [](TestCase &t)
        {
            auto run = [](bool pooled, std::vector<uint8_t> &vramOut, uint64_t &seq, uint64_t &cmds,
                          size_t &pooledOut)
            {
                std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
                GSRegisters regs{};
                initQueueTestRegs(regs);
                GS gs;
                gs.init(vram.data(), static_cast<uint32_t>(vram.size()), &regs);
                gs.setQueueEnabled(true);
                gs.setPktSeqEnabled(true);
                gs.setPacketPoolEnabled(pooled);
                gs.writeRegister(GS_REG_TEST_1, 0x30000ull);
                const std::vector<std::vector<uint8_t>> pkts = {
                    makePackedTriangle(200u, 10u, 30u), makeReglistPoints(),
                    makeImageUpload(0x100u, 0u, 0u, 8u, 8u, 3u), makePackedTriangle(5u, 250u, 60u)};
                for (size_t r = 0; r < 3u; ++r)
                {
                    for (const auto &pkt : pkts)
                        gs.processGIFPacket(pkt.data(), static_cast<uint32_t>(pkt.size()));
                }
                gs.drainQueue();
                seq = gs.pktSeqSnapshot();
                cmds = gs.pktSeqSnapshotCommands();
                pooledOut = gs.packetPool().pooledCount();
                vramOut = snapshotVramBytes(gs);
            };
            std::vector<uint8_t> vA, vB;
            uint64_t sA = 0, sB = 0, cA = 0, cB = 0;
            size_t pA = 0, pB = 0;
            run(false, vA, sA, cA, pA);
            run(true, vB, sB, cB, pB);
            t.IsTrue(cA != 0u, "digest should count commands");
            t.Equals(sB, sA, "pooled queue should give the same consumed digest");
            t.Equals(cB, cA, "pooled queue should give the same consumed count");
            t.IsTrue(vA == vB, "pooled queue should give the same VRAM");
            t.Equals(pA, 0u, "pool should stay empty when disabled");
            t.IsTrue(pB != 0u, "pool should retain buffers when enabled");
        });

        // GP4 H6: batched pops keep FIFO order, byte counts and per-command
        // RPC signaling across batch boundaries.
        tc.Run("GP4 H6 batched pops keep order and RPC signaling", [](TestCase &t)
        {
            std::vector<uint32_t> seen;
            std::mutex seenMutex;
            GsWorker worker(0u, 0u, [&](GsCommand &cmd)
                            {
                                std::lock_guard<std::mutex> lock(seenMutex);
                                seen.push_back(cmd.u32b);
                            });
            worker.setPopBatch(GsWorker::kPopBatch);
            worker.start();
            const size_t n = 3u * GsWorker::kPopBatch + 3u;
            std::vector<std::shared_ptr<GsRpc<uint32_t>>> rpcs;
            for (size_t i = 0; i < n; ++i)
            {
                GsCommand c;
                c.kind = GsCmdKind::GifPacket;
                c.u32b = static_cast<uint32_t>(i);
                c.bytes.resize(64u, static_cast<uint8_t>(i));
                if (i % 5u == 4u)
                {
                    auto rpc = std::make_shared<GsRpc<uint32_t>>();
                    c.rpc = rpc;
                    rpcs.push_back(rpc);
                }
                worker.enqueue(std::move(c));
            }
            for (auto &rpc : rpcs)
                rpc->wait();
            worker.stop();
            t.Equals(static_cast<uint64_t>(seen.size()), static_cast<uint64_t>(n),
                     "every command should execute");
            bool ordered = seen.size() == n;
            for (size_t i = 0; i < seen.size() && ordered; ++i)
                ordered = seen[i] == static_cast<uint32_t>(i);
            t.IsTrue(ordered, "commands should execute in FIFO order across batches");
            t.Equals(worker.enqueuedCount(), static_cast<uint64_t>(n), "enqueued count should match");
            t.Equals(worker.executedCount(), static_cast<uint64_t>(n), "executed count should match");
            t.Equals(worker.pendingCount(), 0u, "queue should drain");
            t.Equals(worker.pendingBytes(), 0u, "byte accounting should drain");
        });

        // PKB1 item 1+2: no-zero-fill + pooled arbiter copies carry exactly
        // the source bytes, in the same drain order as the exact path, across
        // both submit shapes (direct submit and copyForSubmit+submitStaged).
        tc.Run("PKB1 arbiter copies are byte-exact with no zero-fill", [](TestCase &t)
        {
            auto run = [&](bool noZeroFill, bool pooled, std::vector<std::vector<uint8_t>> &out)
            {
                GsPacketPool pool;
                pool.setEnabled(pooled);
                std::vector<std::vector<uint8_t>> seen;
                std::vector<GifPathId> paths;
                GifArbiter arb(
                    [&](const uint8_t *data, uint32_t size)
                    { seen.emplace_back(data, data + size); });
                arb.setPacketPool(pooled ? &pool : nullptr);
                arb.setNoZeroFill(noZeroFill);
                // Listener records the true path per drained packet.
                arb.setPacketListener([&](GifPathId path, uint32_t) { paths.push_back(path); });
                const GifPathId allPaths[] = {GifPathId::Path1, GifPathId::Path2, GifPathId::Path3};
                for (uint32_t i = 0; i < 120u; ++i)
                {
                    // Vary the size so pooled buffers get reused at different
                    // lengths (stale-byte exposure if the copy were short).
                    // Min 32: bytes 16..19 hold the embedded sequence id.
                    const size_t size = 32u + ((i * 37u) % 5u) * 16u;
                    std::vector<uint8_t> pkt(size);
                    for (size_t b = 0; b < size; ++b)
                        pkt[b] = static_cast<uint8_t>((i * 131u + b * 17u + 0x5Au) & 0xFFu);
                    // Embed the sequence id after the tag word.
                    uint32_t id = i;
                    std::memcpy(pkt.data() + 16, &id, sizeof(id));
                    const GifPathId path = allPaths[i % 3u];
                    if ((i & 1u) == 0u)
                        arb.submit(path, pkt.data(), static_cast<uint32_t>(pkt.size()));
                    else
                        arb.submitStaged(path, arb.copyForSubmit(pkt.data(), static_cast<uint32_t>(pkt.size())),
                                         false);
                }
                arb.drain();
                t.Equals(paths.size(), static_cast<size_t>(120), "every packet should drain");
                t.Equals(seen.size(), static_cast<size_t>(120), "every packet should reach the process fn");
                out = std::move(seen);
                // Every drained packet must carry exactly its source bytes
                // (bytes 16..19 hold the embedded sequence id).
                for (size_t k = 0; k < out.size(); ++k)
                {
                    uint32_t id = 0;
                    std::memcpy(&id, out[k].data() + 16, sizeof(id));
                    const size_t wantSize = 32u + ((id * 37u) % 5u) * 16u;
                    bool exact = out[k].size() == wantSize;
                    for (size_t b = 0; b < out[k].size() && exact; ++b)
                    {
                        if (b >= 16u && b < 20u)
                            continue; // the embedded id, checked below
                        exact = out[k][b] == static_cast<uint8_t>((id * 131u + b * 17u + 0x5Au) & 0xFFu);
                    }
                    if (!exact)
                    {
                        t.IsTrue(false, "drained packet should carry exactly its source bytes");
                        break;
                    }
                }
                return paths;
            };
            std::vector<std::vector<uint8_t>> exactOut, pkbOut;
            const std::vector<GifPathId> exactPaths = run(false, false, exactOut);
            const std::vector<GifPathId> pkbPaths = run(true, true, pkbOut);
            t.Equals(pkbOut.size(), exactOut.size(), "no packet lost with PKB1 copies");
            bool same = pkbOut.size() == exactOut.size() && pkbPaths == exactPaths;
            for (size_t k = 0; k < pkbOut.size() && same; ++k)
                same = pkbOut[k] == exactOut[k];
            t.IsTrue(same, "no-zero-fill pooled copies drain the same ordered bytes as the exact path");
        });

        // PKB1 item 3: tiny-queue stress. Pooled arbiter (no-zero-fill) into
        // a GS on a 4-deep queue with batched pops + batched releases: no
        // lost packets, FIFO kept, same digest/count/VRAM as the direct path.
        tc.Run("PKB1 tiny-queue stress keeps every packet in order", [](TestCase &t)
        {
            const std::vector<std::vector<uint8_t>> pkts = {
                makePackedTriangle(200u, 10u, 30u), makeReglistPoints(),
                makeImageUpload(0x100u, 0u, 0u, 8u, 8u, 3u), makePackedTriangle(5u, 250u, 60u)};
            auto run = [&](bool pkb, std::vector<uint8_t> &vramOut, uint64_t &seq, uint64_t &cmds)
            {
                std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
                GSRegisters regs{};
                initQueueTestRegs(regs);
                GS gs;
                gs.init(vram.data(), static_cast<uint32_t>(vram.size()), &regs);
                gs.setPktSeqEnabled(true);
                gs.writeRegister(GS_REG_TEST_1, 0x30000ull);
                // Both runs go through the 4-deep queue (the digest counts
                // queued commands); the PKB1 run adds the pooled arbiter,
                // batched pops and batched releases.
                gs.setQueueEnabled(true, 4u); // tiny: 4 descriptors
                GsPacketPool arbPool;
                if (pkb)
                {
                    gs.setPacketPoolEnabled(true);
                    gs.setWorkerPopBatch(GsWorker::kPopBatch);
                    gs.setReleaseBatching(true);
                    arbPool.setEnabled(true);
                }
                GifArbiter arb([&](const uint8_t *data, uint32_t size) { gs.processGIFPacket(data, size); });
                if (pkb)
                {
                    arb.setPacketPool(&arbPool);
                    arb.setNoZeroFill(true);
                }
                const GifPathId allPaths[] = {GifPathId::Path1, GifPathId::Path2, GifPathId::Path3};
                for (size_t r = 0; r < 25u; ++r)
                {
                    for (size_t k = 0; k < pkts.size(); ++k)
                    {
                        const auto &pkt = pkts[k];
                        const GifPathId path = allPaths[(r * pkts.size() + k) % 3u];
                        if (((r + k) & 1u) == 0u)
                            arb.submit(path, pkt.data(), static_cast<uint32_t>(pkt.size()));
                        else
                            arb.submitStaged(path,
                                             arb.copyForSubmit(pkt.data(), static_cast<uint32_t>(pkt.size())),
                                             false);
                    }
                    arb.drain();
                }
                gs.drainQueue();
                if (pkb)
                {
                    t.IsTrue(gs.packetPool().pooledCount() != 0u,
                             "pool should retain buffers after the stress");
                }
                seq = gs.pktSeqSnapshot();
                cmds = gs.pktSeqSnapshotCommands();
                vramOut = snapshotVramBytes(gs);
            };
            std::vector<uint8_t> vExact, vPkb;
            uint64_t sExact = 0, sPkb = 0, cExact = 0, cPkb = 0;
            run(false, vExact, sExact, cExact);
            run(true, vPkb, sPkb, cPkb);
            t.IsTrue(cExact != 0u, "digest should count commands");
            t.Equals(cPkb, cExact, "tiny-queue PKB1 run should execute every command");
            t.Equals(sPkb, sExact, "tiny-queue PKB1 run should give the same consumed digest");
            t.IsTrue(vPkb == vExact, "tiny-queue PKB1 run should give the same VRAM");
        });

        // PKB1 item 3: releaseBulk returns a batch under one lock round with
        // the same caps as release().
        tc.Run("PKB1 releaseBulk batches under the pool caps", [](TestCase &t)
        {
            GsPacketPool pool;
            std::vector<std::vector<uint8_t>> batch;
            for (size_t i = 0; i < 10u; ++i)
                batch.emplace_back(256u, static_cast<uint8_t>(i));
            pool.releaseBulk(batch);
            t.IsTrue(batch.empty(), "disabled releaseBulk should drop the batch");
            t.Equals(pool.pooledCount(), 0u, "disabled releaseBulk should not pool");
            pool.setEnabled(true);
            for (size_t i = 0; i < 10u; ++i)
                batch.emplace_back(256u, static_cast<uint8_t>(i));
            pool.releaseBulk(batch);
            t.IsTrue(batch.empty(), "releaseBulk should consume the batch");
            t.Equals(pool.pooledCount(), 10u, "releaseBulk should pool every fitting buffer");
            std::vector<uint8_t> got = pool.acquire(200u);
            t.IsTrue(got.capacity() >= 256u, "bulk-pooled buffers should be reusable");
            t.IsTrue(pool.pooledBytes() <= GsPacketPool::kMaxBytes, "pool should honor the byte cap");
        });

        tc.Run("GSW1 serial job thread runs jobs off-thread, in order, one at a time", [](TestCase &t)
        {
            const std::thread::id caller = std::this_thread::get_id();
            std::vector<int> order;
            std::atomic<int> running{0};
            bool overlapped = false, onCaller = false;
            std::mutex m;
            {
                ps2x_gs::SerialJobThread helper("GsHudTest");
                t.IsFalse(helper.join(), "join on an idle, unstarted helper should not wait");
                for (int i = 0; i < 200; ++i)
                {
                    helper.submit([&, i]
                                  {
                                      if (running.fetch_add(1) != 0)
                                          overlapped = true;
                                      if (std::this_thread::get_id() == caller)
                                          onCaller = true;
                                      if (i % 37 == 0)
                                          std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                      {
                                          std::lock_guard<std::mutex> lock(m);
                                          order.push_back(i);
                                      }
                                      running.fetch_sub(1);
                                  });
                    if (i % 50 == 49)
                    {
                        helper.join();
                        std::lock_guard<std::mutex> lock(m);
                        t.Equals(static_cast<int>(order.size()), i + 1, "join should wait for every submitted job");
                    }
                }
                // The destructor stops the helper after the last job.
                helper.submit([&]
                              {
                                  std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                  std::lock_guard<std::mutex> lock(m);
                                  order.push_back(1000);
                              });
            }
            t.IsFalse(overlapped, "jobs must never overlap");
            t.IsFalse(onCaller, "jobs must run on the helper thread");
            t.Equals(static_cast<int>(order.size()), 201, "every job should run, including the one pending at stop");
            bool inOrder = true;
            for (int i = 0; i < 200; ++i)
                inOrder = inOrder && order[static_cast<size_t>(i)] == i;
            t.IsTrue(inOrder && order.back() == 1000, "jobs should run in submit order");
        });
    });
}
