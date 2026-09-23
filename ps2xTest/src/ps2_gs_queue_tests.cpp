#include "MiniTest.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_worker.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
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

        tc.Run("queue backpressure: a full ring blocks producers until drained", [](TestCase &t)
        {
            std::atomic<bool> gate{false};
            std::atomic<int> executed{0};
            GsWorker worker(2u, 64u,
                            [&](GsCommand &)
                            {
                                while (!gate.load(std::memory_order_acquire))
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                executed.fetch_add(1, std::memory_order_relaxed);
                            });
            worker.start();
            // The worker immediately pops one command and parks in the
            // gated handler, so three enqueues fill a 2-deep ring (one
            // executing + two queued) and the fourth must block.
            for (uint8_t i = 1; i <= 3; ++i)
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
    });
}
