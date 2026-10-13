// OFL1: minimal DirtySock responder (link never comes up). Shapes are
// transcribed from the ONL3-ONL5 host HLE (branch `onl5`); see
// `ps2_net_nolink.h` for why this exists. No sockets, no patches.
#include "ps2_net_nolink.h"

#include "ps2_e41_trace.h"
#include "runtime/ps2_memory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ps2_nolink
{
    namespace
    {
        // "RPC IOP Ver 1.0 " (DRTYSCKF.IRX .data+0x70, the RpcWakeup DMA
        // source; verified bytes, not a guess).
        constexpr char kIopVersion[16] = {
            'R', 'P', 'C', ' ', 'I', 'O', 'P', ' ',
            'V', 'e', 'r', ' ', '1', '.', '0', ' '};

        constexpr uint32_t kDirtySockCid = 1u;

        // DirtySock member table (from DRTYSCKF.IRX RpcRegisterMember call
        // sites; FOURCC (member, sub) -> handler). The EE looks handlers
        // up by FOURCC scan (sub_003F4C98); cookies (0x100+i) stand in for
        // IOP function addresses and are dispatched back in
        // scanAndDispatch.
        struct MemberEntry
        {
            uint32_t member;
            uint32_t sub;
        };
        constexpr MemberEntry kMembers[] = {
            {0x6e657477u, 0x63726561u}, // netw/crea
            {0x6e657477u, 0x64657374u}, // netw/dest
            {0x6e657477u, 0x73746172u}, // netw/star
            {0x6e657477u, 0x73687574u}, // netw/shut
            {0x6e657477u, 0x73746174u}, // netw/stat
            {0x6e657477u, 0x6c697374u}, // netw/list
            {0x68747470u, 0x63726561u}, // http/crea
            {0x68747470u, 0x64657374u}, // http/dest
            {0x68747470u, 0x67657420u}, // http/get
            {0x68747470u, 0x696e666fu}, // http/info
            {0x68747470u, 0x64617461u}, // http/data
            {0x73657373u, 0x63726561u}, // sess/crea
            {0x73657373u, 0x64657374u}, // sess/dest
            {0x73657373u, 0x73746174u}, // sess/stat
            {0x73657373u, 0x636f6e6eu}, // sess/conn
            {0x73657373u, 0x64697363u}, // sess/disc
            {0x73657373u, 0x73656e64u}, // sess/send
            {0x73657373u, 0x72656376u}, // sess/recv
            {0x70696e67u, 0x63726561u}, // ping/crea
            {0x70696e67u, 0x64657374u}, // ping/dest
            {0x70696e67u, 0x72657175u}, // ping/requ
            {0x70696e67u, 0x72657370u}, // ping/resp
            {0x6e616d65u, 0x6c6f6f6bu}, // name/look
            {0x6e616d65u, 0x646f6e65u}, // name/done
            {0x6e616d65u, 0x66726565u}, // name/free
            {0x6e616d65u, 0x61646472u}, // name/addr
            {0x6d6e676cu, 0x63726561u}, // mngl/crea
            {0x6d6e676cu, 0x64657374u}, // mngl/dest
            {0x6d6e676cu, 0x636f6e6eu}, // mngl/conn
            {0x6d6e676cu, 0x636d706cu}, // mngl/cmpl
            {0x676d6c6bu, 0x63726561u}, // gmlk/crea
            {0x676d6c6bu, 0x64657374u}, // gmlk/dest
            {0x676d6c6bu, 0x73746174u}, // gmlk/stat
            {0x676d6c6bu, 0x73656e64u}, // gmlk/send
            {0x676d6c6bu, 0x72656376u}, // gmlk/recv
            {0x676d6c6bu, 0x6374726cu}, // gmlk/ctrl
            {0x676d7574u, 0x63726561u}, // gmut/crea
            {0x676d7574u, 0x64657374u}, // gmut/dest
            {0x676d7574u, 0x72736574u}, // gmut/rset
            {0x676d7574u, 0x636f6e6eu}, // gmut/conn
            {0x676d7574u, 0x636f6d70u}, // gmut/comp
            {0x676d7574u, 0x73746174u}, // gmut/stat
            {0x676d7574u, 0x7461626cu}, // gmut/tabl
            {0x676d7574u, 0x61647674u}, // gmut/advt
            {0x676d7574u, 0x77697468u}, // gmut/with
            {0x676d7574u, 0x6c6f6361u}, // gmut/loca
            {0x676d7574u, 0x71756572u}, // gmut/quer
        };
        constexpr uint32_t kMemberCount = sizeof(kMembers) / sizeof(kMembers[0]);
        constexpr uint32_t kCookieBase = 0x100u;

        const char *memberName(uint32_t cookie)
        {
            const uint32_t i = (cookie >= kCookieBase) ? (cookie - kCookieBase) : 0xFFFFFFFFu;
            static const char *kNames[] = {
                "netw/crea", "netw/dest", "netw/star", "netw/shut", "netw/stat", "netw/list",
                "http/crea", "http/dest", "http/get ", "http/info", "http/data",
                "sess/crea", "sess/dest", "sess/stat", "sess/conn", "sess/disc", "sess/send", "sess/recv",
                "ping/crea", "ping/dest", "ping/requ", "ping/resp",
                "name/look", "name/done", "name/free", "name/addr",
                "mngl/crea", "mngl/dest", "mngl/conn", "mngl/cmpl",
                "gmlk/crea", "gmlk/dest", "gmlk/stat", "gmlk/send", "gmlk/recv", "gmlk/ctrl",
                "gmut/crea", "gmut/dest", "gmut/rset", "gmut/conn", "gmut/comp", "gmut/stat",
                "gmut/tabl", "gmut/advt", "gmut/with", "gmut/loca", "gmut/quer",
            };
            return (i < kMemberCount) ? kNames[i] : "?";
        }

        // EE DirtySock state struct base (fixed; both EE helpers use it).
        constexpr uint32_t kEeStateAddr = 0x51F0C0u;
        // EE struct field offsets (from sub_003F5000/sub_003F4528).
        constexpr uint32_t kFSlotBase = 0x4D94u;  // u32 EE slot-area base
        constexpr uint32_t kFReplyBase = 0x4DA0u; // u32 EE reply-area base
        constexpr uint32_t kFTableBase = 0x4DB8u; // u32 completion-flag array
        constexpr uint32_t kFIdxB = 0x4DD4u;      // ring consumer index
        constexpr uint32_t kFRingN = 0x4DD8u;     // ring size
        constexpr uint32_t kFSema = 0x4E08u;

        uint32_t readU32(const uint8_t *rdram, uint32_t addr, bool &ok)
        {
            ok = false;
            const uint8_t *ptr = getConstMemPtr(rdram, addr);
            if (!ptr)
            {
                return 0u;
            }
            uint32_t value = 0u;
            std::memcpy(&value, ptr, sizeof(value));
            ok = true;
            return value;
        }

        uint32_t readEeU32(const uint8_t *rdram, uint32_t addr)
        {
            bool ok = false;
            return readU32(rdram, addr, ok);
        }

        // Slot fingerprint dedup, shared by the ring path
        // (processDoorbell) and the DMA path (noteDma sweep). A slot is
        // dispatched exactly once per unique 8-word content, whichever
        // path sees it first; any byte change re-arms the slot.
        uint32_t g_slotLastW[64][8] = {};
        bool g_slotLastValid[64] = {};

        // EE->IOP payloads by IOP destination (sifbuf window only).
        std::mutex g_dmaMutex;
        std::map<uint32_t, std::vector<uint8_t>> g_dmaPayloads;

        std::string dmaString(uint32_t dst)
        {
            std::lock_guard<std::mutex> lock(g_dmaMutex);
            auto it = g_dmaPayloads.find(dst);
            if (it == g_dmaPayloads.end() || it->second.empty())
            {
                return std::string();
            }
            const uint8_t *data = it->second.data();
            size_t len = 0;
            while (len < it->second.size() && data[len] != 0u)
            {
                ++len;
            }
            return std::string(reinterpret_cast<const char *>(data), len);
        }

        void writeReply(uint8_t *rdram, uint32_t replyBase, uint32_t i,
                        uint32_t w2, uint32_t outlen, uint32_t status, uint32_t w7,
                        uint32_t r0, uint32_t r1)
        {
            if (replyBase == 0u)
            {
                return;
            }
            uint8_t *reply = getMemPtr(rdram, replyBase + i * 32u);
            if (!reply)
            {
                return;
            }
            std::memset(reply, 0, 32u);
            // REPLY[0]/REPLY[4] mimic the real IOP consume pointers
            // (guest-state-driven, advancing) instead of zero.
            std::memcpy(reply + 0u, &r0, 4u);
            std::memcpy(reply + 4u, &r1, 4u);
            std::memcpy(reply + 8u, &w2, 4u);
            std::memcpy(reply + 12u, &outlen, 4u);
            std::memcpy(reply + 20u, &status, 4u);
            std::memcpy(reply + 28u, &w7, 4u);
        }

        // One fake broadband config (256 B entry). Layout from
        // _BuildConfigList: +0 index (>0), +4 name, +66 name, +128 path.
        // All four must be non-empty/nonzero or the IOP drops the entry.
        uint32_t writeFakeConfig(uint8_t *rdram, uint32_t eeBuf, uint32_t maxBytes)
        {
            if (maxBytes < 256u)
            {
                return 0u;
            }
            uint8_t *dst = getMemPtr(rdram, eeBuf);
            if (!dst)
            {
                return 0u;
            }
            std::memset(dst, 0, 256u);
            const uint32_t index = 1u;
            std::memcpy(dst + 0u, &index, 4u);
            const char name[] = "Broadband";
            std::memcpy(dst + 4u, name, sizeof(name));
            std::memcpy(dst + 66u, name, sizeof(name));
            const char path[] = "mc0:Broadband#0";
            std::memcpy(dst + 128u, path, sizeof(path));
            return 1u;
        }

        // Unified session-handle namespace (all */crea methods; the EE
        // lib passes status back as the handle).
        uint32_t g_nextHandle = 0u;

        bool scanAndDispatch(uint8_t *rdram, uint64_t tick, bool fromRing)
        {
            const uint32_t slotBase = readEeU32(rdram, kEeStateAddr + kFSlotBase) & 0x1FFFFFFFu;
            const uint32_t replyBase = readEeU32(rdram, kEeStateAddr + kFReplyBase) & 0x1FFFFFFFu;
            const uint32_t tableBase = readEeU32(rdram, kEeStateAddr + kFTableBase) & 0x1FFFFFFFu;
            const uint32_t idxB = readEeU32(rdram, kEeStateAddr + kFIdxB);
            const uint32_t ringN = readEeU32(rdram, kEeStateAddr + kFRingN);
            (void)idxB;

            bool replied = false;
            const uint32_t n = (ringN > 0u && ringN <= 64u) ? ringN : 16u;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t slot = slotBase + i * 32u;
                const uint32_t w0 = readEeU32(rdram, slot + 0u);
                const uint32_t w1 = readEeU32(rdram, slot + 4u);
                const uint32_t w2 = readEeU32(rdram, slot + 8u);
                const uint32_t w3 = readEeU32(rdram, slot + 12u);
                const uint32_t w4 = readEeU32(rdram, slot + 16u);
                const uint32_t w5 = readEeU32(rdram, slot + 20u);
                const uint32_t w6 = readEeU32(rdram, slot + 24u);
                const uint32_t w7 = readEeU32(rdram, slot + 28u);
                if (w0 == 0u && w1 == 0u && w2 == 0u && w3 == 0u &&
                    w4 == 0u && w5 == 0u && w6 == 0u && w7 == 0u)
                {
                    continue;
                }
                const uint32_t cur[8] = {w0, w1, w2, w3, w4, w5, w6, w7};
                bool same = g_slotLastValid[i];
                for (int k = 0; same && k < 8; ++k)
                {
                    same = (g_slotLastW[i][k] == cur[k]);
                }
                if (same)
                {
                    continue;
                }
                for (int k = 0; k < 8; ++k)
                {
                    g_slotLastW[i][k] = cur[k];
                }
                g_slotLastValid[i] = true;
                // Handler 1 = RpcReturnTable: copy the member table to
                // w2 (EE addr, w3 max bytes). The alarm handler
                // (sub_003F4448) wakes when reply[0x1C] ECHOES
                // request[0x1C].
                if (w6 == 1u)
                {
                    const uint32_t eeBuf = w2 & 0x1FFFFFFFu;
                    const uint32_t maxBytes = w3;
                    uint8_t *dst = getMemPtr(rdram, eeBuf);
                    uint32_t outlen = 0u;
                    if (dst && maxBytes >= kMemberCount * 16u)
                    {
                        for (uint32_t m = 0; m < kMemberCount; ++m)
                        {
                            const uint32_t index = m;
                            const uint32_t cookie = kCookieBase + m;
                            std::memcpy(dst + m * 16u + 0u, &index, 4u);
                            std::memcpy(dst + m * 16u + 4u, &kMembers[m].member, 4u);
                            std::memcpy(dst + m * 16u + 8u, &kMembers[m].sub, 4u);
                            std::memcpy(dst + m * 16u + 12u, &cookie, 4u);
                        }
                        outlen = kMemberCount * 16u;
                    }
                    if (replyBase != 0u)
                    {
                        uint8_t *reply = getMemPtr(rdram, replyBase + i * 32u);
                        if (reply)
                        {
                            std::memset(reply, 0, 32u);
                            std::memcpy(reply + 8u, &w2, 4u);
                            std::memcpy(reply + 12u, &outlen, 4u);
                            const uint32_t status = static_cast<uint32_t>(kMemberCount);
                            std::memcpy(reply + 20u, &status, 4u);
                            std::memcpy(reply + 28u, &w7, 4u);
                        }
                    }
                    if (tableBase != 0u)
                    {
                        uint8_t *flag = getMemPtr(rdram, tableBase + i * 4u);
                        if (flag)
                        {
                            const uint32_t one = 1u;
                            std::memcpy(flag, &one, 4u);
                        }
                    }
                    std::cerr << "[nolink] table reply tick=" << tick << " members=" << kMemberCount
                              << std::endl;
                    replied = true;
                }
                else if (w6 >= kCookieBase && w6 < kCookieBase + kMemberCount)
                {
                    const uint32_t method = w6 - kCookieBase;
                    const uint32_t st_dc = readEeU32(rdram, kEeStateAddr + 0x4DDCu);
                    uint32_t status = 0u;
                    uint32_t outlen = 0u;
                    if (method == 5u)
                    {
                        // netw/list: a2 = IOP addr of the source string
                        // ("mc0:"/"mc1:"). One fake broadband config.
                        if (dmaString(w0) == "mc0:")
                        {
                            const uint32_t count = writeFakeConfig(rdram, w2 & 0x1FFFFFFFu, w3);
                            status = count;
                            outlen = count * 256u;
                        }
                    }
                    else if (method == 2u)
                    {
                        // netw/star: accept the config, but the link
                        // never comes up (netw/stat stays down).
                        status = 0u;
                    }
                    else if (method == 4u)
                    {
                        // netw/stat: a1 = sub-query FOURCC. Link down:
                        // every link query reads 0. mach is link-
                        // independent hardware identity (fixed
                        // locally-administered MAC).
                        if (w5 == 0x6d616378u)
                        {
                            const uint32_t eeBuf = w2 & 0x1FFFFFFFu;
                            uint8_t *dst = (w3 >= 6u) ? getMemPtr(rdram, eeBuf) : nullptr;
                            if (dst)
                            {
                                const uint8_t mac[6] = {0x02u, 0x0Cu, 0x29u, 0x4Fu, 0x4Eu, 0x4Cu};
                                std::memcpy(dst, mac, sizeof(mac));
                                outlen = 6u;
                            }
                            status = 0u;
                        }
                        else
                        {
                            status = 0u;
                        }
                    }
                    else if (method == 14u)
                    {
                        // sess/conn: no sockets here; fail the connect.
                        // The game waits out its own timer, then shows
                        // its connection error (same shape as a dead
                        // server under the full HLE).
                        status = 1u;
                    }
                    else if (method == 11u || method == 18u || method == 6u ||
                             method == 26u || method == 30u || method == 36u)
                    {
                        // */crea: hand out a host handle in status.
                        status = ++g_nextHandle;
                    }
                    else
                    {
                        // In-shape no-op stub (status 0/outlen 0): the
                        // link-down path never reaches these.
                        status = 0u;
                    }
                    std::cerr << "[nolink] call tick=" << tick << " " << memberName(w6)
                              << " -> status=" << status << " outlen=" << outlen
                              << " via=" << (fromRing ? "ring" : "dma") << std::endl;
                    writeReply(rdram, replyBase, i, w2, outlen, status, w7, st_dc, outlen);
                    replied = true;
                }
                else if (w6 != 0u)
                {
                    std::cerr << "[nolink] UNKNOWN handler tick=" << tick << " slot=" << i
                              << " h=0x" << std::hex << w6 << std::dec << std::endl;
                }
            }
            return replied;
        }

        bool processDoorbell(uint8_t *rdram, uint32_t flags, uint64_t tick)
        {
            (void)flags;
            return scanAndDispatch(rdram, tick, true);
        }
    } // namespace

    void noteDma(uint32_t src, uint32_t dst, uint32_t size, uint8_t *rdram)
    {
        if (!rdram)
        {
            return;
        }
        // Pre-net (slotBase 0) everything below is a no-op; only the
        // Online path arms the EE net state.
        const uint32_t slotBase = readEeU32(rdram, kEeStateAddr + kFSlotBase) & 0x1FFFFFFFu;
        if (slotBase == 0u)
        {
            return;
        }
        if (dst < 0xA4000000u || dst >= 0xA5000000u || size == 0u || size > 65536u)
        {
            return;
        }
        std::vector<uint8_t> bytes;
        bytes.reserve(size);
        for (uint32_t i = 0; i < size; ++i)
        {
            const uint8_t *ptr = getConstMemPtr(rdram, src + i);
            if (!ptr)
            {
                break;
            }
            bytes.push_back(*ptr);
        }
        {
            std::lock_guard<std::mutex> lock(g_dmaMutex);
            if (g_dmaPayloads.size() > 256u)
            {
                g_dmaPayloads.clear();
            }
            g_dmaPayloads[dst] = std::move(bytes);
        }
        // Dispatch newly queued slots at DMA time (the payload is
        // recorded first, so handlers see complete payloads); the
        // shared fingerprint dedup keeps the later ring from
        // re-running them.
        scanAndDispatch(rdram, ps2_e41_trace::lastVsyncTick(), false);
    }

    bool onSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                   uint32_t cid, uint32_t packetAddr, uint32_t packetSize,
                   uint32_t srcExtra, uint32_t dstExtra, uint32_t sizeExtra,
                   uint64_t tick)
    {
        (void)ctx;
        (void)runtime;
        (void)srcExtra;
        (void)dstExtra;
        (void)sizeExtra;
        if (cid != kDirtySockCid || !rdram)
        {
            return false;
        }
        if (packetSize < 16u || packetAddr == 0u)
        {
            std::cerr << "[nolink] cid=1 short packet size=" << packetSize << std::endl;
            return false;
        }
        bool ok = false;
        const uint32_t flags = readU32(rdram, packetAddr + 12u, ok);
        if (!ok)
        {
            std::cerr << "[nolink] cid=1 unreadable packet" << std::endl;
            return false;
        }

        // Doorbell: flags = sifbuf addr (process the queue) or
        // 0xFFFFFFFF (wake; same handling). RpcWakeup checks -1 FIRST,
        // so -1 is a doorbell even though bit0 is set.
        if (flags == 0xFFFFFFFFu || (flags & 1u) == 0u)
        {
            return processDoorbell(rdram, flags, tick);
        }

        // Version poll: IOP DMAs "RPC IOP Ver 1.0 " to flags&~1.
        {
            const uint32_t eeAddr = (flags & ~1u) & 0x1FFFFFFFu;
            if (eeAddr >= 32u * 1024u * 1024u)
            {
                std::cerr << "[nolink] poll reply addr out of range 0x" << std::hex << flags
                          << std::dec << std::endl;
                return false;
            }
            uint8_t *dst = getMemPtr(rdram, eeAddr);
            if (!dst)
            {
                std::cerr << "[nolink] poll reply addr unmapped 0x" << std::hex << eeAddr
                          << std::dec << std::endl;
                return false;
            }
            std::memcpy(dst, kIopVersion, sizeof(kIopVersion));
            std::cerr << "[nolink] poll reply tick=" << tick << " -> 0x" << std::hex << eeAddr
                      << std::dec << std::endl;
            return true;
        }
    }
} // namespace ps2_nolink
