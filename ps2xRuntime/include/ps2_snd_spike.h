// AU2 spike (dev-only, default off): stand in for the IOP side of EA's SND
// driver (SNDDRV.IRX) just enough to wake the EE sound thread, and record
// what the EE sends back. Env:
//   PS2X_SND_TICK=<n>      deliver n cid-1 type-0 "tick" packets per vblank
//                          to the handler the game registered for cid 1.
//   PS2X_SND_LOG=<file>    event log (bounded, kMaxLines).
//   PS2X_SND_DUMP_DIR=<d>  payload dumps (bounded, kMaxDumpBytes).
//
// SNDDRV protocol (AU2 Part A, local/research/AU2/REPORT.md):
//   IOP->EE cid 1, +0x10 type: 0 = tick (opt = IOP address the EE DMAs its
//     tag buffer to), 2 = SPU-upload done (opt = the EE's callback id),
//     3 = data only. A tick also writes a 0x240-byte status block to the EE
//     address given in the init RPC (+4); SNDDRV stamps its serial at +0 and
//     +0x23C.
//   EE->IOP cid 0, +0x10 type 1 = SNDIOP_dmqueue(+0x14 id, +0x18 IOP src,
//     +0x1C SPU dst, +0x20 u16 size, +0x22 s16 prio) -> sceSdVoiceTrans.
// While the spike is on, sceSifSetDma calls made from the SND library are
// captured host-side (keyed by IOP address) instead of being copied into
// low EE RDRAM.

#pragma once

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cstdarg>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ps2_snd_spike
{

inline constexpr uint64_t kMaxLines = 60000ull;
inline constexpr uint64_t kMaxDumpBytes = 256ull << 20;
inline constexpr uint32_t kPacketAddr = 0x01F31000u;   // after the runtime's HLE pools
inline constexpr uint32_t kDonePacketBase = 0x01F31100u; // ring of 16 x 0x20
inline constexpr uint32_t kTagbufIopAddr = 0x0000B3C4u; // SNDDRV's own value (state+0xE0)
inline constexpr uint32_t kStatusBytes = 0x240u;
inline constexpr uint32_t kSndCodeLo = 0x003B0000u;     // EE SND library range (AU1)
inline constexpr uint32_t kSndCodeHi = 0x003D0000u;
inline constexpr uint32_t kIsceSendCmdRa = 0x00426220u; // isceSifSendCmd -> _sceSifSendCmd
inline constexpr uint32_t kTickCounterAddr = 0x0050A8E8u + 0x180u;
inline constexpr uint32_t kRpcSendBuf = 0x0050AD00u;

struct State
{
    std::mutex mutex;
    bool init = false;
    bool enabled = false;
    uint32_t ticksPerVblank = 0;
    FILE *log = nullptr;
    std::string dumpDir;
    uint64_t lines = 0;
    uint64_t dumpBytes = 0;
    uint32_t handler = 0, handlerData = 0, handlerGp = 0;
    uint32_t statusAddr = 0;
    uint32_t serial = 0;
    uint64_t ticks = 0, cid0 = 0, dmq = 0, done = 0, setdma = 0, tagbufs = 0;
    uint32_t doneRing = 0;
    std::map<uint32_t, std::vector<uint8_t>> iopMem; // IOP dst -> last payload
};

inline State &state()
{
    static State s;
    return s;
}

inline void initLocked(State &s)
{
    s.init = true;
    const char *tick = std::getenv("PS2X_SND_TICK");
    if (!tick || !*tick)
        return;
    s.ticksPerVblank = static_cast<uint32_t>(std::strtoul(tick, nullptr, 0));
    if (s.ticksPerVblank == 0)
        return;
    if (const char *p = std::getenv("PS2X_SND_LOG"); p && *p)
        s.log = std::fopen(p, "w");
    if (const char *d = std::getenv("PS2X_SND_DUMP_DIR"); d && *d)
        s.dumpDir = d;
    s.enabled = true;
}

inline bool enabled()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.init)
        initLocked(s);
    return s.enabled;
}

inline void logLocked(State &s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
inline void logLocked(State &s, const char *fmt, ...)
{
    if (!s.log || s.lines >= kMaxLines)
        return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(s.log, fmt, ap);
    va_end(ap);
    std::fputc('\n', s.log);
    if ((++s.lines & 63u) == 0u)
        std::fflush(s.log);
}

inline uint32_t rd32(const uint8_t *rdram, uint32_t addr)
{
    uint32_t v = 0;
    const uint32_t p = addr & PS2_RAM_MASK;
    if (p + 4u <= PS2_RAM_SIZE)
        std::memcpy(&v, rdram + p, 4);
    return v;
}

inline void wr32(uint8_t *rdram, uint32_t addr, uint32_t v)
{
    const uint32_t p = addr & PS2_RAM_MASK;
    if (p + 4u <= PS2_RAM_SIZE)
        std::memcpy(rdram + p, &v, 4);
}

inline void dumpLocked(State &s, const std::string &name, const uint8_t *data, size_t size)
{
    if (s.dumpDir.empty() || size == 0 || s.dumpBytes + size > kMaxDumpBytes)
        return;
    const std::string path = s.dumpDir + "/" + name;
    if (FILE *f = std::fopen(path.c_str(), "wb"))
    {
        std::fwrite(data, 1, size, f);
        std::fclose(f);
        s.dumpBytes += size;
    }
}

inline GuestInvocation makeHandlerCall(const State &s, uint32_t packet)
{
    GuestInvocation inv{};
    inv.kind = GuestInvocationKind::Interrupt;
    inv.context.pc = s.handler;
    SET_GPR_U32(&inv.context, 4, packet);
    SET_GPR_U32(&inv.context, 5, s.handlerData);
    SET_GPR_U32(&inv.context, 28, s.handlerGp);
    SET_GPR_U32(&inv.context, 29, 0u); // run() assigns the invocation stack
    SET_GPR_U32(&inv.context, 31, 0u);
    return inv;
}

inline void writePacket(uint8_t *rdram, uint32_t at, uint32_t type, uint32_t opt)
{
    wr32(rdram, at + 0x00u, 0x20u); // psize 0x20, no extra data
    wr32(rdram, at + 0x04u, 0u);
    wr32(rdram, at + 0x08u, 1u);    // cid 1
    wr32(rdram, at + 0x0Cu, opt);
    wr32(rdram, at + 0x10u, type);
    for (uint32_t o = 0x14u; o < 0x20u; o += 4u)
        wr32(rdram, at + o, 0u);
}

// sceSifAddCmdHandler (Stubs/SIF.cpp): remember the cid-1 handler.
inline void noteAddCmdHandler(uint32_t cid, uint32_t handler, uint32_t data, uint32_t gp)
{
    if (!enabled() || cid != 1u)
        return;
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.handler = handler;
    s.handlerData = data;
    s.handlerGp = gp;
    logLocked(s, "addcmdhandler cid=1 handler=0x%x data=0x%x gp=0x%x", handler, data, gp);
}

// SifCallRpc (Syscalls/RPC.cpp): the SND init call carries the status address.
inline void noteRpc(const uint8_t *rdram, uint32_t sid, uint32_t fno, uint32_t send, uint32_t size)
{
    if (!enabled() || sid != 0x534E44u)
        return;
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    char hex[3 * 64 + 1] = {0};
    for (uint32_t i = 0; i < size && i < 64u; ++i)
        std::snprintf(hex + 3 * i, 4, "%02x ", (rd32(rdram, send + (i & ~3u)) >> (8 * (i & 3u))) & 0xFFu);
    if (fno == 0u && size >= 8u)
        s.statusAddr = rd32(rdram, send + 4u);
    logLocked(s, "snd-rpc fno=%u send=0x%x size=%u status=0x%x bytes=[%s]", fno, send, size, s.statusAddr, hex);
}

// EeScheduler::processEvent(VBlankStart): deliver the IOP tick(s).
template <typename Queue>
inline void onVBlank(uint8_t *rdram, uint64_t vsync, Queue &&queue)
{
    if (!rdram || !enabled())
        return;
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.handler == 0u)
        return;
    for (uint32_t i = 0; i < s.ticksPerVblank; ++i)
    {
        ++s.serial;
        if (s.statusAddr != 0u)
        {
            wr32(rdram, s.statusAddr + 0x000u, s.serial);
            wr32(rdram, s.statusAddr + 0x23Cu, s.serial);
        }
        writePacket(rdram, kPacketAddr, 0u, kTagbufIopAddr);
        queue(makeHandlerCall(s, kPacketAddr));
        ++s.ticks;
    }
    if (s.ticks <= 8u || (vsync % 60u) == 0u)
        logLocked(s, "tick vsync=%llu ticks=%llu counter=0x%x cid0=%llu dmq=%llu done=%llu setdma=%llu tagbufs=%llu",
                  (unsigned long long)vsync, (unsigned long long)s.ticks, rd32(rdram, kTickCounterAddr),
                  (unsigned long long)s.cid0, (unsigned long long)s.dmq, (unsigned long long)s.done,
                  (unsigned long long)s.setdma, (unsigned long long)s.tagbufs);
}

// sceSifSetDma (Stubs/SIF.cpp), one descriptor. Returns true when the spike
// consumed it (SND-originated: captured, not copied into EE RDRAM).
inline bool onSetDma(const uint8_t *rdram, uint64_t vsync, uint32_t ra, uint32_t src, uint32_t dst,
                     uint32_t size, uint32_t attr)
{
    if (!rdram || ra < kSndCodeLo || ra >= kSndCodeHi || !enabled())
        return false;
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.setdma;
    const uint32_t p = src & PS2_RAM_MASK;
    if (size > (16u << 20) || p + size > PS2_RAM_SIZE)
        return true;
    std::vector<uint8_t> bytes(rdram + p, rdram + p + size);
    const bool tagbuf = dst == kTagbufIopAddr;
    if (tagbuf)
        ++s.tagbufs;
    if (!tagbuf || s.tagbufs <= 40u || (s.tagbufs % 300u) == 0u)
    {
        char name[96];
        std::snprintf(name, sizeof(name), "setdma-%06llu-v%llu-%s-%08x-%u.bin",
                      (unsigned long long)s.setdma, (unsigned long long)vsync, tagbuf ? "tag" : "iop", dst, size);
        dumpLocked(s, name, bytes.data(), bytes.size());
    }
    logLocked(s, "setdma vsync=%llu ra=0x%x src=0x%x dst=0x%x size=%u attr=0x%x%s", (unsigned long long)vsync, ra,
              src, dst, size, attr, tagbuf ? " tagbuf" : "");
    s.iopMem[dst] = std::move(bytes);
    return true;
}

// Syscalls/RPC.cpp sceSifSendCmd (both entry points). cid 0 = SND command.
template <typename Queue>
inline void onSendCmd(uint8_t *rdram, uint64_t vsync, uint32_t ra, uint32_t cid, uint32_t packet,
                      uint32_t psize, Queue &&queue)
{
    if (!rdram || cid != 0u || !enabled())
        return;
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.cid0;
    uint32_t w[12] = {0};
    for (uint32_t i = 0; i < 12u && i * 4u < psize; ++i)
        w[i] = rd32(rdram, packet + 4u * i);
    const uint32_t type = w[4], id = w[5], iopSrc = w[6], spuDst = w[7];
    const uint32_t size = w[8] & 0xFFFFu;
    const int32_t prio = static_cast<int16_t>(w[8] >> 16);
    logLocked(s, "cid0 vsync=%llu ra=0x%x psize=%u type=%u id=0x%x iop=0x%x spu=0x%x size=%u prio=%d w9=0x%x w10=0x%x w11=0x%x",
              (unsigned long long)vsync, ra, psize, type, id, iopSrc, spuDst, size, prio, w[9], w[10], w[11]);
    if (type != 1u)
        return;
    ++s.dmq;
    auto it = s.iopMem.upper_bound(iopSrc);
    if (it != s.iopMem.begin())
    {
        --it;
        const uint32_t off = iopSrc - it->first;
        if (off < it->second.size())
        {
            const size_t n = std::min<size_t>(size, it->second.size() - off);
            char name[96];
            std::snprintf(name, sizeof(name), "dmq-%06llu-v%llu-spu%06x-%u.bin", (unsigned long long)s.dmq,
                          (unsigned long long)vsync, spuDst, size);
            dumpLocked(s, name, it->second.data() + off, n);
        }
    }
    if (id != 0u && s.handler != 0u)
    {
        const uint32_t at = kDonePacketBase + 0x20u * (s.doneRing++ & 15u);
        writePacket(rdram, at, 2u, id);
        queue(makeHandlerCall(s, at));
        ++s.done;
    }
}

} // namespace ps2_snd_spike
