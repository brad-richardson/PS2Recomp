#pragma once

// GB2 Part 2: quiescent boot gate (header-only, ps2_e4.h pattern).
//
// On the game thread, at fixed guest vsync ticks (every 50 from 100 to
// 1350), stop-the-world drain the GS queue (Fence RPC when queued, no-op
// when direct), then hash SnapshotVram + the priv regs and log one line:
//
//   [vq] tick=100 vram=<fnv32> regs=<fnv32> size=<bytes> sub=<n> reg=<n>
//
// `sub`/`reg` are the GS packet-submit and HLE-reg-write counters: on the
// first hash mismatch, (prev_sub, cur_sub] is the packet index range to
// bisect. Armed by PS2X_VQ=1 only; unset = one relaxed check per VBlank.
//
// Quiescence argument: the game thread is the only GS producer, so after
// the drain every submitted command has executed. Concurrent main-thread
// presents don't mutate VRAM or priv regs (Present is read-only; the
// present debug event fires only when history is unpaused).

#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace ps2_vq
{
    inline bool enabled()
    {
        static const bool on = [] {
            const char *env = std::getenv("PS2X_VQ");
            return env && std::strcmp(env, "1") == 0;
        }();
        return on;
    }

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

    inline uint32_t hashRegs(const GSRegisters &regs)
    {
        uint64_t words[20];
        words[0] = regs.pmode;
        words[1] = regs.smode1;
        words[2] = regs.smode2;
        words[3] = regs.srfsh;
        words[4] = regs.synch1;
        words[5] = regs.synch2;
        words[6] = regs.syncv;
        words[7] = regs.dispfb1;
        words[8] = regs.display1;
        words[9] = regs.dispfb2;
        words[10] = regs.display2;
        words[11] = regs.extbuf;
        words[12] = regs.extdata;
        words[13] = regs.extwrite;
        words[14] = regs.bgcolor;
        words[15] = regs.csr.load(std::memory_order_relaxed);
        words[16] = regs.vsyncTick.load(std::memory_order_relaxed);
        words[17] = regs.imr;
        words[18] = regs.busdir;
        words[19] = regs.siglblid;
        return fnv1a32(reinterpret_cast<const uint8_t *>(words), sizeof(words));
    }

    inline void noteVBlank(uint64_t tick, GS &gs, GSRegisters &regs)
    {
        if (!enabled())
        {
            return;
        }
        static bool announced = false;
        if (!announced)
        {
            announced = true;
            std::cerr << "[vq] armed ticks 100..1350 step 50" << std::endl;
        }
        if (tick < 100u || tick > 1350u || (tick % 50u) != 0u)
        {
            return;
        }
        gs.drainQueue();
        gs.refreshDisplaySnapshot();
        uint32_t size = 0;
        const uint8_t *data = gs.lockDisplaySnapshot(size);
        uint32_t vramFnv = 0;
        if (data && size != 0u)
        {
            vramFnv = fnv1a32(data, size);
        }
        gs.unlockDisplaySnapshot();
        const uint32_t regsFnv = hashRegs(regs);
        std::cerr << "[vq] tick=" << tick << " vram=" << std::hex << vramFnv << " regs=" << regsFnv
                  << std::dec << " size=" << size << " sub=" << gs.submitCount()
                  << " reg=" << gs.regWriteCount() << std::endl;
    }
} // namespace ps2_vq
