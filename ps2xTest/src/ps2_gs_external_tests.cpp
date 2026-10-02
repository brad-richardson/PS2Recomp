// GE2: external-GS shell tests. Env gating (default off), lazy local->host
// FIFO (download trigger, cursor, partial reads, replacement), delegation
// identity vs the bare CPU backend, the versioned save blob, and the
// recording counters. No game boot, no Vulkan.
#include "MiniTest.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_external_backend.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

constexpr uint32_t kVramSize = 4u * 1024u * 1024u;

struct EnvGuard
{
    explicit EnvGuard(const char *name) : m_name(name)
    {
        const char *v = std::getenv(name);
        if (v)
        {
            m_old = v;
            m_had = true;
        }
    }
    ~EnvGuard()
    {
        if (m_had)
        {
            ::setenv(m_name, m_old.c_str(), 1);
        }
        else
        {
            ::unsetenv(m_name);
        }
    }
    const char *m_name;
    std::string m_old;
    bool m_had = false;
};

// Local->host PSMCT32 rect read helper: dir=1, rrw x rrh from (ssax,ssay).
GSTransferCommand localToHostCmd(uint32_t rrw, uint32_t rrh)
{
    GSTransferCommand cmd{};
    cmd.bitbltbuf.sbp = 0u;
    cmd.bitbltbuf.sbw = 64u;
    cmd.bitbltbuf.spsm = 0u; // PSMCT32
    cmd.trxpos.ssax = 0u;
    cmd.trxpos.ssay = 0u;
    cmd.trxreg.rrw = static_cast<uint16_t>(rrw);
    cmd.trxreg.rrh = static_cast<uint16_t>(rrh);
    cmd.direction = 1u;
    return cmd;
}

std::vector<uint8_t> drain(GSRasterBackend &be, uint32_t chunk = 65536u)
{
    std::vector<uint8_t> out;
    std::vector<uint8_t> tmp(chunk);
    for (;;)
    {
        const uint32_t n = be.ConsumeLocalToHostBytes(tmp.data(), chunk);
        if (n == 0u)
            break;
        out.insert(out.end(), tmp.begin(), tmp.begin() + n);
    }
    return out;
}

// IOSL1: probe-bind-table stubs (LT1b signatures, no GE1 linkage: the suite
// never links the adapter, so it pins the table policy with its own symbols).
int probeRequestStub(uint64_t, uint64_t, uint64_t, uint64_t) { return 0; }
int probeResolveStub(uint64_t, uint64_t) { return 0; }
int probeTakeStub(uint64_t, uint8_t *, uint32_t) { return 0; }
int probeStatsStub(uint64_t[8]) { return 0; }

} // namespace

void register_ps2_gs_external_tests()
{
    MiniTest::Case("Ps2GsExternal", [](TestCase &tc)
    {
        tc.Run("env gating: default off, exact match only", [](TestCase &t)
        {
            EnvGuard g("PS2X_GS_BACKEND");
            ::unsetenv("PS2X_GS_BACKEND");
            t.IsTrue(ps2x_gs_external::available(), "external backend is always built");
            t.IsFalse(ps2x_gs_external::requested(), "unset => not requested");
            ::setenv("PS2X_GS_BACKEND", "parallel", 1);
            t.IsFalse(ps2x_gs_external::requested(), "parallel => not requested");
            ::setenv("PS2X_GS_BACKEND", "external ", 1);
            t.IsFalse(ps2x_gs_external::requested(), "trailing space => not requested");
            ::setenv("PS2X_GS_BACKEND", "external", 1);
            t.IsTrue(ps2x_gs_external::requested(), "exact match => requested");
        });

        tc.Run("IOSL1 static probe bind table: request+resolve+take arm lagF, stats optional",
               [](TestCase &t)
               {
                   using ps2x_gs_external::Ge1ProbeBindTable;
                   const Ge1ProbeBindTable full{&probeRequestStub, &probeResolveStub,
                                                &probeTakeStub, &probeStatsStub};
                   t.IsTrue(full.bound(), "full LT1b quartet (the iOS static bind) arms async");
                   const Ge1ProbeBindTable noStats{&probeRequestStub, &probeResolveStub,
                                                   &probeTakeStub, nullptr};
                   t.IsTrue(noStats.bound(), "stats is diagnostics-only; null stats still arms");
                   const Ge1ProbeBindTable noRequest{nullptr, &probeResolveStub,
                                                     &probeTakeStub, &probeStatsStub};
                   t.IsFalse(noRequest.bound(), "missing request falls back to sync");
                   const Ge1ProbeBindTable noResolve{&probeRequestStub, nullptr,
                                                     &probeTakeStub, &probeStatsStub};
                   t.IsFalse(noResolve.bound(), "missing resolve falls back to sync");
                   const Ge1ProbeBindTable noTake{&probeRequestStub, &probeResolveStub,
                                                  nullptr, &probeStatsStub};
                   t.IsFalse(noTake.bound(), "missing take falls back to sync");
                   const Ge1ProbeBindTable empty{};
                   t.IsFalse(empty.bound(), "pre-LT1b table (all null) falls back to sync");
               });

        tc.Run("recording counts: gif paths, vsync gaps, priv, regs", [](TestCase &t)
        {
            EnvGuard g1("PS2X_GS_BACKEND");
            EnvGuard g2("PS2X_GS_EXTERNAL_LOG");
            ::unsetenv("PS2X_GS_EXTERNAL_LOG");
            std::vector<uint8_t> vram(kVramSize, 0);
            auto be = ps2x_gs_external::create(nullptr);
            t.IsTrue(!!be, "create returns a backend");
            t.IsTrue(be->WantsRawGif(), "stub takes the raw stream");
            t.IsTrue(be->WantsGuestVsync(), "stub wants guest vsync");
            t.IsTrue(be->WantsPrivMirror(), "stub wants priv mirror");
            be->Initialize(vram.data(), kVramSize);
            const uint8_t pkt[32] = {0x11};
            be->RawGifPacket(1u, pkt, sizeof(pkt));
            be->RawGifPacket(3u, pkt, sizeof(pkt));
            be->RawGifPacket(3u, pkt, sizeof(pkt));
            be->RawNativePackedPacket(pkt, sizeof(pkt));
            be->RawWriteRegister(0x4Cu, 0x1234u);
            be->PrivMirrored(0x1000u, 0x8u);
            be->GuestVsync(1u, 1u);
            be->GuestVsync(2u, 0u);
            be->GuestVsync(4u, 0u); // skipped 3 => one gap
            const auto st = ps2x_gs_external::stats();
            t.Equals(st.gifPackets, 3ull, "3 raw packets");
            t.Equals(st.gifPacketsByPath[1], 1ull, "path1 count");
            t.Equals(st.gifPacketsByPath[3], 2ull, "path3 count");
            t.Equals(st.gifQwords[1], 2ull, "path1 qwords");
            t.Equals(st.gifQwords[3], 4ull, "path3 qwords");
            t.Equals(st.nativePacked, 1ull, "native packed counted");
            t.Equals(st.regWrites, 1ull, "reg writes counted");
            t.Equals(st.privMirrored, 1ull, "priv mirrored counted");
            t.Equals(st.vsyncs, 3ull, "3 vsyncs");
            t.Equals(st.vsyncGaps, 1ull, "one skipped tick detected");
            t.IsFalse(st.logOpen, "no log without the env knob");
        });

        tc.Run("lazy fifo: one download, cursor, partial reads, byte-identical", [](TestCase &t)
        {
            EnvGuard g("PS2X_GS_EXTERNAL_LOG");
            ::unsetenv("PS2X_GS_EXTERNAL_LOG");
            std::vector<uint8_t> vram(kVramSize);
            for (size_t i = 0; i < vram.size(); ++i)
                vram[i] = static_cast<uint8_t>((i * 2654435761u) >> 16);
            // Reference: bare CPU backend, same setup, one full drain.
            GSCpuBackend ref;
            ref.Initialize(vram.data(), kVramSize);
            ref.BeginTransfer(localToHostCmd(16u, 4u));
            const std::vector<uint8_t> want = drain(ref);
            t.IsTrue(!want.empty(), "reference produces bytes");

            auto be = ps2x_gs_external::create(nullptr);
            be->Initialize(vram.data(), kVramSize);
            be->BeginTransfer(localToHostCmd(16u, 4u));
            t.Equals(ps2x_gs_external::stats().fifoDownloads, 0ull, "no download at setup (lazy)");
            // Two partial reads must concatenate to the reference bytes.
            std::vector<uint8_t> head(10u), tail(want.size());
            const uint32_t n0 = be->ConsumeLocalToHostBytes(head.data(), 10u);
            t.Equals(n0, 10u, "first partial read serves 10");
            t.Equals(ps2x_gs_external::stats().fifoDownloads, 1ull, "downloaded at first consume");
            const uint32_t n1 = be->ConsumeLocalToHostBytes(
                tail.data(), static_cast<uint32_t>(tail.size()));
            t.Equals(static_cast<size_t>(n0 + n1), want.size(), "partial reads cover all bytes");
            t.IsTrue(std::memcmp(head.data(), want.data(), n0) == 0, "head bytes match");
            t.IsTrue(std::memcmp(tail.data(), want.data() + n0, n1) == 0, "tail bytes match");
            const uint32_t n2 = be->ConsumeLocalToHostBytes(tail.data(), 64u);
            t.Equals(n2, 0u, "consume past the end returns 0");
            t.Equals(ps2x_gs_external::stats().fifoDownloads, 1ull, "no re-download");
        });

        tc.Run("fifo snapshot + idle follow the adapter cursor", [](TestCase &t)
        {
            EnvGuard g("PS2X_GS_EXTERNAL_LOG");
            ::unsetenv("PS2X_GS_EXTERNAL_LOG");
            std::vector<uint8_t> vram(kVramSize, 0xA5);
            auto be = ps2x_gs_external::create(nullptr);
            be->Initialize(vram.data(), kVramSize);
            be->BeginTransfer(localToHostCmd(8u, 2u));
            const size_t full = be->GetTransferSnapshot().localToHostPendingBytes;
            t.IsTrue(full > 0u, "pending bytes before any consume");
            t.IsFalse(be->SavestateIdle(), "busy while bytes are pending");
            t.Equals(be->SavestateBusyReason(), std::string("gs-transfer"), "busy reason names gs-transfer");
            std::vector<uint8_t> tmp(full);
            const uint32_t n = be->ConsumeLocalToHostBytes(tmp.data(), 7u);
            t.Equals(n, 7u, "partial read");
            t.Equals(be->GetTransferSnapshot().localToHostPendingBytes, full - 7u,
                     "snapshot pending follows the cursor");
            t.IsFalse(be->SavestateIdle(), "still busy mid-fifo");
            drain(*be);
            t.Equals(be->GetTransferSnapshot().localToHostPendingBytes, size_t(0),
                     "no pending bytes after the drain");
            t.IsTrue(be->SavestateIdle(), "idle once the fifo is drained");
        });

        tc.Run("new local->host setup replaces the fifo", [](TestCase &t)
        {
            EnvGuard g("PS2X_GS_EXTERNAL_LOG");
            ::unsetenv("PS2X_GS_EXTERNAL_LOG");
            std::vector<uint8_t> vram(kVramSize, 0x3C);
            auto be = ps2x_gs_external::create(nullptr);
            be->Initialize(vram.data(), kVramSize);
            be->BeginTransfer(localToHostCmd(32u, 8u));
            std::vector<uint8_t> tmp(16u);
            be->ConsumeLocalToHostBytes(tmp.data(), 16u); // download + partial read
            be->BeginTransfer(localToHostCmd(4u, 1u));    // replacement
            const auto st = ps2x_gs_external::stats();
            t.IsTrue(st.fifoReplacedUnread > 0u, "unread leftovers counted as replaced");
            t.Equals(st.fifoDownloads, 1ull, "replacement does not download (still lazy)");
            const std::vector<uint8_t> got = drain(*be);
            GSCpuBackend ref;
            ref.Initialize(vram.data(), kVramSize);
            ref.BeginTransfer(localToHostCmd(4u, 1u));
            const std::vector<uint8_t> want = drain(ref);
            t.Equals(got.size(), want.size(), "replacement fifo size matches");
            t.IsTrue(got == want, "replacement fifo bytes match");
        });

        tc.Run("save blob: roundtrip continues the fifo; rejects bad blobs", [](TestCase &t)
        {
            EnvGuard g("PS2X_GS_EXTERNAL_LOG");
            ::unsetenv("PS2X_GS_EXTERNAL_LOG");
            std::vector<uint8_t> vram(kVramSize, 0x71);
            auto be = ps2x_gs_external::create(nullptr);
            be->Initialize(vram.data(), kVramSize);
            be->BeginTransfer(localToHostCmd(16u, 4u));
            be->GuestVsync(7u, 1u);
            be->PrivMirrored(0x0000u, 0x5u);
            std::vector<uint8_t> head(20u);
            be->ConsumeLocalToHostBytes(head.data(), 20u);
            std::vector<uint8_t> blob;
            be->SavestateSave(blob);
            t.IsTrue(blob.size() > 8u + 4u, "blob carries state past the header");
            const std::vector<uint8_t> rest = drain(*be);

            auto be2 = ps2x_gs_external::create(nullptr);
            be2->Initialize(vram.data(), kVramSize);
            t.IsTrue(be2->SavestateLoad(blob.data(), blob.size()), "blob loads");
            const std::vector<uint8_t> rest2 = drain(*be2);
            t.IsTrue(rest == rest2, "loaded fifo continues the same bytes");
            const auto st = ps2x_gs_external::stats();
            t.Equals(st.loads, 1ull, "load counted");

            std::vector<uint8_t> bad = blob;
            bad[0] ^= 0xFFu;
            t.IsFalse(be2->SavestateLoad(bad.data(), bad.size()), "bad magic rejected");
            bad = blob;
            bad[10] ^= 0xFFu; // inside the version word
            t.IsFalse(be2->SavestateLoad(bad.data(), bad.size()), "bad version rejected");
            t.IsFalse(be2->SavestateLoad(blob.data(), blob.size() - 1u), "truncation rejected");
            t.IsFalse(be2->SavestateLoad(nullptr, 0u), "empty rejected");

            // CN2b: a v2 blob (no GE1 l2h arm after the adapter FIFO) still loads.
            const size_t fifoSizeAt = 8u + 4u + 6u * 8u + 19u * 8u + 1u + 8u;
            uint64_t fifoSize = 0u;
            std::memcpy(&fifoSize, blob.data() + fifoSizeAt, 8u);
            const size_t armAt = fifoSizeAt + 8u + static_cast<size_t>(fifoSize);
            std::vector<uint8_t> v2 = blob;
            v2.erase(v2.begin() + static_cast<std::ptrdiff_t>(armAt),
                     v2.begin() + static_cast<std::ptrdiff_t>(armAt + 5u));
            v2[8] = 2u;
            v2[9] = v2[10] = v2[11] = 0u;
            auto be3 = ps2x_gs_external::create(nullptr);
            be3->Initialize(vram.data(), kVramSize);
            t.IsTrue(be3->SavestateLoad(v2.data(), v2.size()), "v2 blob loads");
            t.IsTrue(drain(*be3) == rest, "v2 blob continues the same bytes");
        });

        tc.Run("delegation: host->local upload matches the cpu backend", [](TestCase &t)
        {
            EnvGuard g("PS2X_GS_EXTERNAL_LOG");
            ::unsetenv("PS2X_GS_EXTERNAL_LOG");
            std::vector<uint8_t> vramA(kVramSize, 0), vramB(kVramSize, 0);
            GSTransferCommand cmd{};
            cmd.bitbltbuf.dbp = 0u;
            cmd.bitbltbuf.dbw = 64u;
            cmd.bitbltbuf.dpsm = 0u; // PSMCT32
            cmd.trxpos.dsax = 0u;
            cmd.trxpos.dsay = 0u;
            cmd.trxreg.rrw = 16u;
            cmd.trxreg.rrh = 4u;
            cmd.direction = 0u;
            std::vector<uint8_t> img(16u * 4u * 4u);
            for (size_t i = 0; i < img.size(); ++i)
                img[i] = static_cast<uint8_t>(i * 31u + 7u);
            auto be = ps2x_gs_external::create(nullptr);
            be->Initialize(vramA.data(), kVramSize);
            be->BeginTransfer(cmd);
            be->UploadImage(img.data(), static_cast<uint32_t>(img.size()));
            GSCpuBackend ref;
            ref.Initialize(vramB.data(), kVramSize);
            ref.BeginTransfer(cmd);
            ref.UploadImage(img.data(), static_cast<uint32_t>(img.size()));
            std::vector<uint8_t> snapA, snapB;
            be->SnapshotVram(snapA);
            ref.SnapshotVram(snapB);
            t.Equals(snapA.size(), snapB.size(), "snapshot sizes match");
            t.IsTrue(snapA == snapB, "stub delegation is vram-identical to cpu");
        });
    });
}
