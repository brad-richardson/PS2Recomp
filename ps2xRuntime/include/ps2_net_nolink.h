// SPDX-License-Identifier: GPL-3.0-or-later OR MIT
#pragma once
// OFL1: minimal DirtySock responder — the game never leaves the menu
// without it.
//
// SSX 3 talks to DRTYSCKF.IRX over SIFCMD cid=1 (never SIF RPC). With no
// IOP in HLE mode those sends are no-ops: the EE ring size at
// [0x51F0C0+0x4DD8] stays 0 and the ring-index modulo in sub_003F4528
// (0x3F4618-0x3F4628) trips its `break 0,7` divisor guard, which lands on
// the unmapped COP0 vector 0x80000080 (missing-target crash, ra=0x3F4610).
//
// This module answers the SIFCMD protocol with correct shapes but the
// link never comes up: version poll, member table, echo completions,
// one fake broadband config, netw/star ok, every netw/stat link query
// down. The game then shows its own network error and returns to the
// menu. No sockets, no patches, no knob: it only acts on SIFCMD cid=1
// traffic, which the game sends only on the Online path, so every other
// path (including all det keys) is byte-identical.
//
// Merge contract: when the full PS2X_NET HLE lands, it must be tried
// first at both hook sites and this responder skipped when it replies.
#include <cstdint>

struct R5900Context;
class PS2Runtime;

namespace ps2_nolink
{
    // Handle one EE->IOP SIFCMD send. Returns true when a reply was
    // written to guest RAM. No-op unless cid == 1.
    bool onSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                   uint32_t cid, uint32_t packetAddr, uint32_t packetSize,
                   uint32_t srcExtra, uint32_t dstExtra, uint32_t sizeExtra,
                   uint64_t tick);

    // Record an EE->IOP DMA payload addressed into the DirtySock sifbuf
    // window (0xA4000000+) and sweep the EE slot ring for newly queued
    // requests. No-op until the EE net state is live (slotBase != 0),
    // which only the Online path sets.
    void noteDma(uint32_t src, uint32_t dst, uint32_t size, uint8_t *rdram);
} // namespace ps2_nolink
