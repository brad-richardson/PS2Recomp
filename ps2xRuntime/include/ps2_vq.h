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
#include <vector>

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
        words[19] = regs.siglblid.load(std::memory_order_relaxed);
        return fnv1a32(reinterpret_cast<const uint8_t *>(words), sizeof(words));
    }

    // GB3: window + present hash + optional frame dumps.
    //   PS2X_VQ_FROM / PS2X_VQ_TO / PS2X_VQ_STEP (defaults 100 / 1350 / 50,
    //   GB2's 26 ticks) pick the sample ticks.
    //   Each line also carries pres=<fnv32 of the presented RGBA rows>
    //   pw/ph (GS::presentForDiagnostics: the backend's Present at this
    //   stream position, no host-latch side effects).
    //   PS2X_VQ_DUMP_DIR=<dir> writes vq-<tick>.ppm (RGB) per sample.
    struct Window
    {
        uint64_t from = 100u;
        uint64_t to = 1350u;
        uint64_t step = 50u;
    };

    inline uint64_t envU64(const char *name, uint64_t def)
    {
        const char *env = std::getenv(name);
        if (!env || !*env)
            return def;
        return std::strtoull(env, nullptr, 0);
    }

    inline const Window &window()
    {
        static const Window w = [] {
            Window r;
            r.from = envU64("PS2X_VQ_FROM", 100u);
            r.to = envU64("PS2X_VQ_TO", 1350u);
            r.step = envU64("PS2X_VQ_STEP", 50u);
            if (r.step == 0u)
                r.step = 1u;
            return r;
        }();
        return w;
    }

    inline void dumpPpm(const char *dir, uint64_t tick, const PresentationFrame &frame)
    {
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/vq-%06llu.ppm", dir, static_cast<unsigned long long>(tick));
        FILE *f = std::fopen(path, "wb");
        if (!f)
            return;
        std::fprintf(f, "P6\n%u %u\n255\n", frame.width, frame.height);
        const size_t stride = static_cast<size_t>(640u) * 4u;
        std::vector<uint8_t> row(static_cast<size_t>(frame.width) * 3u);
        for (uint32_t y = 0; y < frame.height; ++y)
        {
            const size_t off = static_cast<size_t>(y) * stride;
            if (off + static_cast<size_t>(frame.width) * 4u > frame.pixels.size())
                break;
            for (uint32_t x = 0; x < frame.width; ++x)
            {
                row[x * 3u + 0u] = frame.pixels[off + x * 4u + 0u];
                row[x * 3u + 1u] = frame.pixels[off + x * 4u + 1u];
                row[x * 3u + 2u] = frame.pixels[off + x * 4u + 2u];
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
    }

    inline void noteVBlank(uint64_t tick, GS &gs, GSRegisters &regs)
    {
        if (!enabled())
        {
            return;
        }
        const Window &w = window();
        static bool announced = false;
        if (!announced)
        {
            announced = true;
            std::cerr << "[vq] armed ticks " << w.from << ".." << w.to << " step " << w.step << std::endl;
        }
        if (tick < w.from || tick > w.to || ((tick - w.from) % w.step) != 0u)
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

        const PresentationFrame frame = gs.presentForDiagnostics();
        uint32_t presFnv = 2166136261u;
        if (frame)
        {
            const size_t stride = static_cast<size_t>(640u) * 4u;
            const size_t rowBytes = static_cast<size_t>(frame.width) * 4u;
            for (uint32_t y = 0; y < frame.height; ++y)
            {
                const size_t off = static_cast<size_t>(y) * stride;
                if (off + rowBytes > frame.pixels.size())
                    break;
                for (size_t i = 0; i < rowBytes; ++i)
                {
                    presFnv ^= frame.pixels[off + i];
                    presFnv *= 16777619u;
                }
            }
            if (const char *dir = std::getenv("PS2X_VQ_DUMP_DIR"))
            {
                if (*dir)
                    dumpPpm(dir, tick, frame);
            }
        }
        else
        {
            presFnv = 0u;
        }

        std::cerr << "[vq] tick=" << tick << " vram=" << std::hex << vramFnv << " regs=" << regsFnv
                  << std::dec << " size=" << size << " sub=" << gs.submitCount()
                  << " reg=" << gs.regWriteCount() << " priv=" << gs.privWriteCount()
                  << " pres=" << std::hex << presFnv << std::dec << " pw=" << frame.width
                  << " ph=" << frame.height << std::endl;
    }
} // namespace ps2_vq
