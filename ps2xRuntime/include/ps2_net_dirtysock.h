#pragma once
// ONL3: host-socket network layer for SSX 3's EA DirtySock stack.
//
// The game talks to DRTYSCKF.IRX over SIFCMD cid=1 (never SIF RPC). With no
// IOP in HLE mode those sends are no-ops, so networking is dead. This module
// answers the SIFCMD protocol on host sockets. Gated by PS2X_NET=1 (default
// off); when off, onSendCmd returns immediately and guest behavior is
// identical to today.
#include <cstdint>
#include <string>
#include <vector>

struct R5900Context;
class PS2Runtime;

namespace ps2_net
{
    // True with PS2X_NET=1 and an explicit valid IPv4 PS2X_NET_SERVER.
    bool enabled();

    // True from gmlk/crea until the last gmlk/dest. Events mode holds
    // stock 60 while true unless the post-v0 LOCKSTEP120 knob is set.
    bool lockstepLinkActive();

    // Handle one EE->IOP SIFCMD send. Returns true when a reply was written
    // to guest RAM. No-op unless enabled() and cid == 1.
    bool onSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                   uint32_t cid, uint32_t packetAddr, uint32_t packetSize,
                   uint32_t srcExtra, uint32_t dstExtra, uint32_t sizeExtra,
                   uint64_t tick);

    // Append one payload record to the PS2X_NET_CAPTURE file (when set):
    // direction ("udp-tx"/"udp-rx"/"tcp-tx"/"tcp-rx"/"sif"), host tick,
    // guest tick, length, hex.
    void capture(const char *direction, uint64_t hostTick, uint64_t guestTick,
                 const uint8_t *data, uint32_t length);

    // Record an EE->IOP DMA payload addressed into the DirtySock sifbuf
    // window (0xA4000000+). Lets handlers read the strings/structs the EE
    // pushed (e.g. "mc0:" for netw/list). Also sweeps the EE slot ring
    // for newly queued requests (ONL4: some are queued without a ring)
    // and dispatches them, writing replies into rdram. No-op unless
    // enabled().
    void noteDma(uint32_t src, uint32_t dst, uint32_t size, uint8_t *rdram);

    // Read back a NUL-terminated string previously pushed to an IOP
    // address (empty when unknown).
    std::string dmaString(uint32_t dst);

    // Hex dump (up to maxBytes, space-separated) of a payload previously
    // pushed to an IOP address (empty when unknown).
    std::string dmaHex(uint32_t dst, uint32_t maxBytes);

    // Raw payload bytes previously pushed to an IOP address (empty when
    // unknown). Used for capture call sites.
    std::vector<uint8_t> dmaBytes(uint32_t dst);

    // ONL10: Quick Play navigator milestones (PS2X_NET_QUICKPLAY_NAV=1,
    // class net, owner ONL10). First-observed guest vsync tick per event,
    // 0 = not yet seen. Relaxed host-side atomics; recording them never
    // touches guest state. Read by the pad-layer navigator (Pad.cpp),
    // written at the HLE handler sites below.
    struct QpNavMilestones
    {
        uint64_t netwCrea = 0u;   // Online entered (RpcNetworkCreate)
        uint64_t creaCount = 0u;  // ONL11: netw/crea calls this boot
        uint64_t netwList = 0u;   // config list served (screen #8 open)
        uint64_t netwStar = 0u;   // saved config accepted (screen #8 OK)
        uint64_t sessConnOk = 0u; // lobby TCP connected (login submitted)
        uint32_t sessConnPort = 0u;
        uint64_t sessConnFail = 0u; // CONNECT-FAILED (no lobby -> abort nav)
        uint64_t gameAuth = 0u;     // game's own auth carried the login
        uint64_t pers = 0u;         // persona picked (screen #12 Cross)
        uint64_t onln = 0u;         // Online-main population started
        uint64_t news = 0u;
        uint64_t pset = 0u; // buddy PSET (Online-main populated)
        uint64_t rget = 0u;
        uint64_t quik = 0u; // filter confirm (must stay 0: user confirms)
        uint64_t quikCount = 0u;
    };
    QpNavMilestones qpNavMilestones();

    // ONL10: cached knob reads. navWanted() = NAV set && QUICKPLAY=1.
    // ONL11: qpNavMode() 0 = off, 1 = launch-flag (arm at boot), 2 =
    // online (dormant until Online entry, re-arms after a back-out).
    bool qpNavQuickplayOn();
    bool qpNavWanted();
    int qpNavMode();
} // namespace ps2_net
