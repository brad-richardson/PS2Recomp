// N8D7M12 Part 1: shared GS-stream replay core (N8D7M11 §3).
//
// Factored verbatim out of the desktop replay test (asserts replaced by
// Ps2xGsReplayResult; console formats unchanged). See the header for the
// no-test-harness / desktop-parity contract.

#include "runtime/gs/gs_replay_core.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_parallel_backend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vq.h"

#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    uint32_t fnv(const uint8_t *data, size_t size)
    {
        uint32_t hash = 2166136261u;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 16777619u;
        }
        return hash;
    }

    struct ScopedReplayRtz
    {
        int previous = -1;
        bool ok = true;

        explicit ScopedReplayRtz(bool enabled)
        {
            if (!enabled)
                return;
            previous = std::fegetround();
            ok = previous >= 0 && std::fesetround(FE_TOWARDZERO) == 0;
        }

        ~ScopedReplayRtz()
        {
            if (previous >= 0)
                std::fesetround(previous);
        }
    };

    bool setPriv(GSRegisters &r, uint32_t off, uint64_t value)
    {
        switch (off)
        {
        case 0x0000: r.pmode = value; break;
        case 0x0010: r.smode1 = value; break;
        case 0x0020: r.smode2 = value; break;
        case 0x0030: r.srfsh = value; break;
        case 0x0040: r.synch1 = value; break;
        case 0x0050: r.synch2 = value; break;
        case 0x0060: r.syncv = value; break;
        case 0x0070: r.dispfb1 = value; break;
        case 0x0080: r.display1 = value; break;
        case 0x0090: r.dispfb2 = value; break;
        case 0x00A0: r.display2 = value; break;
        case 0x00B0: r.extbuf = value; break;
        case 0x00C0: r.extdata = value; break;
        case 0x00D0: r.extwrite = value; break;
        case 0x00E0: r.bgcolor = value; break;
        case 0x1000: r.csr.store(value, std::memory_order_release); break;
        case 0x1010: r.imr = value; break;
        case 0x1040: r.busdir = value; break;
        case 0x1080: r.siglblid.store(value, std::memory_order_release); break;
        default: return false;
        }
        return true;
    }

    uint32_t privHash(const GSRegisters &r)
    {
        const uint64_t values[] = {r.pmode, r.smode1, r.smode2, r.srfsh, r.synch1,
            r.synch2, r.syncv, r.dispfb1, r.display1, r.dispfb2, r.display2,
            r.extbuf, r.extdata, r.extwrite, r.bgcolor,
            r.csr.load(std::memory_order_acquire), r.vsyncTick.load(std::memory_order_acquire),
            r.imr, r.busdir, r.siglblid.load(std::memory_order_acquire)};
        return fnv(reinterpret_cast<const uint8_t *>(values), sizeof(values));
    }

    uint32_t presentHash(const PresentationFrame &frame)
    {
        if (!frame)
            return 0u;
        uint32_t hash = 2166136261u;
        const size_t stride = 640u * 4u;
        const size_t rowBytes = static_cast<size_t>(frame.width) * 4u;
        for (uint32_t y = 0; y < frame.height; ++y)
        {
            const size_t offset = static_cast<size_t>(y) * stride;
            if (offset + rowBytes > frame.pixels.size())
                break;
            for (size_t i = 0; i < rowBytes; ++i)
            {
                hash ^= frame.pixels[offset + i];
                hash *= 16777619u;
            }
        }
        return hash;
    }

    bool dumpTick(uint64_t tick)
    {
        const char *list = std::getenv("PS2X_GS_REPLAY_PPM_TICKS");
        if (!list)
            return false;
        while (*list)
        {
            char *end = nullptr;
            const unsigned long long value = std::strtoull(list, &end, 10);
            if (end == list)
                return false;
            if (value == tick)
                return true;
            if (*end != ',')
                return false;
            list = end + 1;
        }
        return false;
    }
}

Ps2xGsReplayReadResult ps2x_gs_replay_read_event(FILE *f, uint32_t &length,
                                                 std::vector<uint8_t> &record)
{
    const size_t prefixBytes = std::fread(&length, 1, sizeof(length), f);
    if (prefixBytes == 0u && std::feof(f))
        return Ps2xGsReplayReadResult::End;
    if (prefixBytes != sizeof(length))
        return Ps2xGsReplayReadResult::Invalid;
    if (length < 9u || length > 64u * 1024u * 1024u)
        return Ps2xGsReplayReadResult::Invalid;
    record.resize(length);
    return std::fread(record.data(), 1, length, f) == length
        ? Ps2xGsReplayReadResult::Record : Ps2xGsReplayReadResult::Invalid;
}

Ps2xGsReplayResult ps2x_gs_replay_run()
{
    Ps2xGsReplayResult result;
    const char *path = std::getenv("PS2X_GS_REPLAY_CAPTURE");
    if (!path || !path[0])
    {
        result.skipped = true;
        return result;
    }
    FILE *f = std::fopen(path, "rb");
    if (!f)
    {
        result.openOk = false;
        return result;
    }
    char magic[8]{};
    if (std::fread(magic, 1, sizeof(magic), f) != sizeof(magic) ||
        std::memcmp(magic, "PS2XGSC1", sizeof(magic)) != 0)
    {
        std::fclose(f);
        result.headerOk = false;
        return result;
    }

    const char *mode = std::getenv("PS2X_GS_REPLAY_MODE");
    const char *backend = std::getenv("PS2X_GS_REPLAY_BACKEND");
    const bool parallelBackend = backend && std::strcmp(backend, "parallel") == 0;
    const bool queued = parallelBackend || (mode && std::strcmp(mode, "queue") == 0);
    const char *drop = std::getenv("PS2X_GS_REPLAY_DROP_PRIV");
    const bool dropPriv = drop && std::strcmp(drop, "1") == 0;
    const char *rounding = std::getenv("PS2X_GS_REPLAY_RTZ");
    const bool rtzPath1 = rounding && std::strcmp(rounding, "path1") == 0;
    const bool rtzAll = rounding && std::strcmp(rounding, "all") == 0;
    if (rounding && !rtzPath1 && !rtzAll)
    {
        std::fclose(f);
        result.rtzOk = false;
        return result;
    }
    std::vector<uint8_t> packetPaths;
    if (const char *pathFile = std::getenv("PS2X_GS_REPLAY_PATH_FILE"))
    {
        std::ifstream in(pathFile);
        uint64_t index = 0u;
        unsigned pathId = 0u;
        while (in >> index >> pathId)
        {
            if (index != packetPaths.size() || pathId < 1u || pathId > 3u)
                break;
            packetPaths.push_back(static_cast<uint8_t>(pathId));
        }
        if (packetPaths.empty() || (!in.eof() && !in.good()))
        {
            std::fclose(f);
            result.pathFileOk = false;
            return result;
        }
    }
    uint32_t stride = 50u;
    if (const char *step = std::getenv("PS2X_GS_REPLAY_STEP"))
    {
        const unsigned long value = std::strtoul(step, nullptr, 10);
        if (value > 0ul && value < 1000000ul)
            stride = static_cast<uint32_t>(value);
    }

    std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
    GSRegisters regs{};
    regs.csr.store(0x4000u, std::memory_order_relaxed);
    GS gs;
    gs.init(vram.data(), static_cast<uint32_t>(vram.size()), &regs);
    // N8D7M12 Part 5F4P2: dev-only default-off worker-consumption
    // fingerprint. Must precede queue enable (worker-start happens-before).
    const char *pktSeqEnv = std::getenv("PS2X_GS_REPLAY_PKTSEQ");
    const bool pktSeq = pktSeqEnv && std::strcmp(pktSeqEnv, "1") == 0;
    gs.setPktSeqEnabled(pktSeq);
    if (queued)
        gs.setQueueEnabled(true);
    if (parallelBackend)
    {
        if (!ps2x_gs_parallel::available())
        {
            std::fclose(f);
            result.backendOk = false;
            return result;
        }
        gs.setRasterBackend(ps2x_gs_parallel::create(&regs));
    }

    uint64_t bisectTo = 0u;
    if (const char *value = std::getenv("PS2X_GS_REPLAY_BISECT_TO"))
        bisectTo = std::strtoull(value, nullptr, 10);
    std::ofstream packetTrace;
    if (const char *out = std::getenv("PS2X_GS_REPLAY_PACKET_TRACE"))
    {
        packetTrace.open(out, std::ios::binary);
        packetTrace << "index,tick,path,vram\n";
    }

    // N8D7M5 executed-word provenance (default OFF; CPU-direct replay
    // only). Desktop-only: compiled only when PS2X_GS_REPLAY_WORD_WATCH is
    // defined (test builds; see ps2xRuntime/CMakeLists.txt). Android-target
    // builds leave this whole region out, so the shipped core has no
    // word-watch code, env reader, or counter dependency. With no worker
    // thread the kind-1/5/7 calls below execute synchronously, so a
    // before/after word pair plus the advanced submitCount proves the logged
    // packet executed the write (see REPORT §1). Queued (parallel) replays
    // must not use this: processGIFPacket would only enqueue.
#ifdef PS2X_GS_REPLAY_WORD_WATCH
    std::vector<uint32_t> watchAddrs;
    bool watchValid = true;
    if (const char *words = std::getenv("PS2X_GS_REPLAY_WORDS"))
    {
        if (*words)
        {
            const char *cursor = words;
            while (*cursor && watchValid)
            {
                char *end = nullptr;
                const unsigned long value = std::strtoul(cursor, &end, 0);
                if (end == cursor || value + 4u > vram.size() || (value & 3u) != 0u ||
                    watchAddrs.size() >= 8u)
                {
                    watchValid = false;
                    break;
                }
                watchAddrs.push_back(static_cast<uint32_t>(value));
                if (*end && *end != ',')
                {
                    watchValid = false;
                    break;
                }
                cursor = (*end == ',') ? end + 1 : end;
            }
        }
        if (watchValid && !watchAddrs.empty())
            ps2xN8D7M5SetCounting(true);
    }
    if (!watchValid)
    {
        std::fclose(f);
        result.wordsOk = false;
        return result;
    }
    size_t n8d7m5Emitted = 0u, n8d7m5Dropped = 0u;
    constexpr size_t kN8D7M5LineCap = 4096u;
    auto snapshotWords = [&](std::vector<uint32_t> &out)
    {
        out.clear();
        for (const uint32_t a : watchAddrs)
        {
            uint32_t w = 0u;
            std::memcpy(&w, vram.data() + a, sizeof(w));
            out.push_back(w);
        }
    };
    auto emitWordLine = [&](uint64_t idx, uint64_t submit, uint64_t tick,
                            const char *path, const Ps2xN8D7M5Counts &before,
                            const Ps2xN8D7M5Counts &after, uint32_t addr,
                            uint32_t oldW, uint32_t newW)
    {
        if (n8d7m5Emitted >= kN8D7M5LineCap)
        {
            ++n8d7m5Dropped;
            return;
        }
        ++n8d7m5Emitted;
        const bool dD = after.draw > before.draw;
        const bool dT = after.transfer > before.transfer;
        const bool dC = after.clear > before.clear;
        char kind[32];
        std::snprintf(kind, sizeof(kind), "%s%s%s%s%s",
                      dD ? "draw" : "", (dD && (dT || dC)) ? "+" : "",
                      dT ? "transfer" : "", (dT && dC) ? "+" : "",
                      dC ? "clear" : ((!dD && !dT) ? "none" : ""));
        std::cout << "[n8d7m5] word idx=" << idx << " submit=" << submit
                  << " tick=" << tick << " path=" << path << " kind=" << kind
                  << " addr=0x" << std::hex << addr << " old=0x" << oldW
                  << " new=0x" << newW << std::dec << '\n';
    };
#endif // PS2X_GS_REPLAY_WORD_WATCH

    uint64_t packets = 0u, priv = 0u, transfers = 0u, markers = 0u, roundedPackets = 0u;
    uint64_t readbacks = 0u, clears = 0u;
    bool parseOk = true;
    std::vector<std::string> rows;
    while (true)
    {
        uint32_t length = 0;
        std::vector<uint8_t> rec;
        const long recordOffset = std::ftell(f);
        const Ps2xGsReplayReadResult readResult = ps2x_gs_replay_read_event(f, length, rec);
        if (readResult != Ps2xGsReplayReadResult::Record)
        {
            if (readResult == Ps2xGsReplayReadResult::Invalid)
            {
                std::cerr << "GB4_REPLAY_PARSE_ERROR offset=" << recordOffset << '\n';
                parseOk = false;
            }
            break;
        }
        const uint8_t kind = rec[0];
        uint64_t tick = 0;
        std::memcpy(&tick, rec.data() + 1, 8);
        if (kind == 1u)
        {
            if (length < 14u)
            {
                parseOk = false;
                break;
            }
            const uint8_t pathId = packetPaths.empty()
                ? rec[9] : (packets < packetPaths.size() ? packetPaths[packets] : 0u);
            uint32_t size = 0;
            std::memcpy(&size, rec.data() + 10, 4);
            if (pathId < 1u || pathId > 3u || size != length - 14u)
            {
                parseOk = false;
                break;
            }
            gs.noteGifPath(static_cast<GifPathId>(pathId));
            const bool forceRtz = rtzAll || (rtzPath1 && pathId == 1u);
#ifdef PS2X_GS_REPLAY_WORD_WATCH
            std::vector<uint32_t> n8d7m5Before;
            Ps2xN8D7M5Counts n8d7m5CountsBefore{};
            if (!watchAddrs.empty())
            {
                snapshotWords(n8d7m5Before);
                n8d7m5CountsBefore = ps2xN8D7M5Counts();
            }
#endif
            {
                ScopedReplayRtz scope(forceRtz);
                if (!scope.ok)
                {
                    parseOk = false;
                    break;
                }
                gs.processGIFPacket(rec.data() + 14, size);
            }
#ifdef PS2X_GS_REPLAY_WORD_WATCH
            if (!watchAddrs.empty())
            {
                const uint64_t submitAfter = gs.submitCount();
                const Ps2xN8D7M5Counts countsAfter = ps2xN8D7M5Counts();
                const std::string pathStr = std::to_string(static_cast<unsigned>(pathId));
                for (size_t wi = 0u; wi < watchAddrs.size(); ++wi)
                {
                    uint32_t afterW = 0u;
                    std::memcpy(&afterW, vram.data() + watchAddrs[wi], sizeof(afterW));
                    if (afterW != n8d7m5Before[wi])
                        emitWordLine(packets, submitAfter, tick, pathStr.c_str(),
                                     n8d7m5CountsBefore, countsAfter,
                                     watchAddrs[wi], n8d7m5Before[wi], afterW);
                }
            }
#endif
            if (forceRtz)
                ++roundedPackets;
            if (packetTrace && tick < bisectTo)
                packetTrace << packets << ',' << tick << ',' << static_cast<unsigned>(pathId)
                            << ',' << std::hex << fnv(vram.data(), vram.size()) << std::dec << '\n';
            ++packets;
        }
        else if (kind == 2u)
        {
            if (length != 21u)
            {
                parseOk = false;
                break;
            }
            uint32_t offset = 0;
            uint64_t value = 0;
            std::memcpy(&offset, rec.data() + 9, 4);
            std::memcpy(&value, rec.data() + 13, 8);
            if (!dropPriv)
                gs.privWrite([&regs, offset, value]() { setPriv(regs, offset, value); });
            ++priv;
        }
        else if (kind == 3u)
        {
            if (length != 37u)
            {
                parseOk = false;
                break;
            }
            // Transfer metadata is recorded for audit. Its operation is
            // reproduced by the source packet immediately preceding it.
            ++transfers;
        }
        else if (kind == 4u)
        {
            if (length != 9u)
            {
                parseOk = false;
                break;
            }
            gs.drainQueue();
            regs.vsyncTick.store(tick, std::memory_order_release);
            ++markers;
            const bool sampled = tick % stride == 0u;
            const bool named = dumpTick(tick);
            if (!sampled && !named)
                continue;
            // N8D7M12 Part 5F4P2: sampled worker-consumption digest.
            // Stdout only (never into rows/OUT, so parallel.hashes is
            // untouched). drainQueue above already snapshotted at stream
            // position, including the quiescent path.
            if (pktSeq && sampled)
            {
                char seqLine[128];
                std::snprintf(seqLine, sizeof(seqLine), "GB4_PKTSEQ tick=%llu seq=%016llx commands=%llu",
                              static_cast<unsigned long long>(tick),
                              static_cast<unsigned long long>(gs.pktSeqSnapshot()),
                              static_cast<unsigned long long>(gs.pktSeqSnapshotCommands()));
                std::cout << seqLine << '\n';
            }
            uint32_t vramHash = 0u;
            if (sampled)
            {
                gs.refreshDisplaySnapshot();
                uint32_t vramSize = 0;
                const uint8_t *vramData = gs.lockDisplaySnapshot(vramSize);
                vramHash = vramData ? fnv(vramData, vramSize) : 0u;
                gs.unlockDisplaySnapshot();
            }
            const PresentationFrame frame = gs.presentForDiagnostics();
            if (const char *dir = std::getenv("PS2X_GS_REPLAY_PPM_DIR"))
                if (named && *dir && frame)
                    ps2_vq::dumpPpm(dir, tick, frame);
            if (named)
            {
                std::cout << "GB4_FRAME tick=" << tick
                          << " backend=" << (parallelBackend ? "parallel" : "cpu")
                          << " pmode=" << std::hex << regs.pmode
                          << " dispfb1=" << regs.dispfb1 << " dispfb2=" << regs.dispfb2
                          << std::dec << " display_fbp=" << frame.displayFbp
                          << " source_fbp=" << frame.sourceFbp
                          << " preferred=" << frame.usedPreferred
                          << " present=" << std::hex << presentHash(frame) << std::dec << '\n';
                if (!parallelBackend)
                    if (const char *rawDir = std::getenv("PS2X_GS_REPLAY_RAW_PPM_DIR"))
                        if (*rawDir)
                        {
                            GSCpuBackend raw;
                            raw.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
                            GSPresentationRequest request{};
                            request.pmode = regs.pmode;
                            request.smode2 = regs.smode2;
                            request.dispfb1 = regs.dispfb1;
                            request.display1 = regs.display1;
                            request.dispfb2 = regs.dispfb2;
                            request.display2 = regs.display2;
                            request.bgcolor = regs.bgcolor;
                            request.vsyncTick = tick;
                            const auto displayFrame = [](uint64_t word) {
                                return GSFrameReg{static_cast<uint32_t>(word & 0x1ffu),
                                                  static_cast<uint32_t>((word >> 9) & 0x3fu),
                                                  static_cast<uint8_t>((word >> 15) & 0x1fu), 0u};
                            };
                            request.contextFrames[0] = displayFrame(regs.dispfb1);
                            request.contextFrames[1] = displayFrame(regs.dispfb2);
                            const PresentationFrame rawFrame = raw.Present(request);
                            if (rawFrame)
                                ps2_vq::dumpPpm(rawDir, tick, rawFrame);
                            std::cout << "GB4_RAW_FRAME tick=" << tick
                                      << " dispfb1_fbp=" << (regs.dispfb1 & 0x1ffu)
                                      << " display_fbp=" << rawFrame.displayFbp
                                      << " source_fbp=" << rawFrame.sourceFbp
                                      << " present=" << std::hex << presentHash(rawFrame)
                                      << std::dec << '\n';
                        }
            }
            if (!sampled)
                continue;
            char row[160];
            std::snprintf(row, sizeof(row),
                          "GB4_REPLAY tick=%llu vram=%08x priv=%08x present=%08x",
                          static_cast<unsigned long long>(tick), vramHash, privHash(regs),
                          presentHash(frame));
            rows.emplace_back(row);
        }
        else if (kind == 5u)
        {
            if (length < 45u)
            {
                parseOk = false;
                break;
            }
            uint64_t regsIn[4]{};
            uint32_t size = 0;
            std::memcpy(regsIn, rec.data() + 9, sizeof(regsIn));
            std::memcpy(&size, rec.data() + 41, sizeof(size));
            if (size != length - 45u)
            {
                parseOk = false;
                break;
            }
            {
                ScopedReplayRtz scope(rtzAll);
                if (!scope.ok)
                {
                    parseOk = false;
                    break;
                }
#ifdef PS2X_GS_REPLAY_WORD_WATCH
                std::vector<uint32_t> n8d7m5Before;
                Ps2xN8D7M5Counts n8d7m5CountsBefore{};
                if (!watchAddrs.empty())
                {
                    snapshotWords(n8d7m5Before);
                    n8d7m5CountsBefore = ps2xN8D7M5Counts();
                }
#endif
                gs.uploadImageNative(regsIn[0], regsIn[1], regsIn[2], regsIn[3],
                                     rec.data() + 45, size);
#ifdef PS2X_GS_REPLAY_WORD_WATCH
                if (!watchAddrs.empty())
                {
                    const uint64_t submitAfter = gs.submitCount();
                    const Ps2xN8D7M5Counts countsAfter = ps2xN8D7M5Counts();
                    for (size_t wi = 0u; wi < watchAddrs.size(); ++wi)
                    {
                        uint32_t afterW = 0u;
                        std::memcpy(&afterW, vram.data() + watchAddrs[wi], sizeof(afterW));
                        if (afterW != n8d7m5Before[wi])
                            emitWordLine(packets, submitAfter, tick, "native",
                                         n8d7m5CountsBefore, countsAfter,
                                         watchAddrs[wi], n8d7m5Before[wi], afterW);
                    }
                }
#endif
            }
            if (rtzAll)
                ++roundedPackets;
            if (packetTrace && tick < bisectTo)
                packetTrace << packets << ',' << tick << ",native,"
                            << std::hex << fnv(vram.data(), vram.size()) << std::dec << '\n';
            ++packets;
        }
        else if (kind == 6u)
        {
            if (length < 17u)
            {
                parseOk = false;
                break;
            }
            uint32_t maxBytes = 0, size = 0;
            std::memcpy(&maxBytes, rec.data() + 9, 4);
            std::memcpy(&size, rec.data() + 13, 4);
            if (size > maxBytes || size != length - 17u)
            {
                parseOk = false;
                break;
            }
            std::vector<uint8_t> actual(maxBytes);
            const uint32_t count = gs.consumeLocalToHostBytes(actual.data(), maxBytes);
            if (count != size || std::memcmp(actual.data(), rec.data() + 17, size) != 0)
            {
                parseOk = false;
                break;
            }
            ++readbacks;
        }
        else if (kind == 7u)
        {
            if (length != 17u)
            {
                parseOk = false;
                break;
            }
            uint32_t context = 0, rgba = 0;
            std::memcpy(&context, rec.data() + 9, 4);
            std::memcpy(&rgba, rec.data() + 13, 4);
#ifdef PS2X_GS_REPLAY_WORD_WATCH
            std::vector<uint32_t> n8d7m5Before;
            Ps2xN8D7M5Counts n8d7m5CountsBefore{};
            if (!watchAddrs.empty())
            {
                snapshotWords(n8d7m5Before);
                n8d7m5CountsBefore = ps2xN8D7M5Counts();
            }
#endif
            gs.clearFramebufferContext(context, rgba);
#ifdef PS2X_GS_REPLAY_WORD_WATCH
            if (!watchAddrs.empty())
            {
                const uint64_t submitAfter = gs.submitCount();
                const Ps2xN8D7M5Counts countsAfter = ps2xN8D7M5Counts();
                for (size_t wi = 0u; wi < watchAddrs.size(); ++wi)
                {
                    uint32_t afterW = 0u;
                    std::memcpy(&afterW, vram.data() + watchAddrs[wi], sizeof(afterW));
                    if (afterW != n8d7m5Before[wi])
                        emitWordLine(clears, submitAfter, tick, "clear",
                                     n8d7m5CountsBefore, countsAfter,
                                     watchAddrs[wi], n8d7m5Before[wi], afterW);
                }
            }
#endif
            ++clears;
        }
        else
        {
            parseOk = false;
            break;
        }
    }
    std::fclose(f);
    gs.drainQueue();
#ifdef PS2X_GS_REPLAY_WORD_WATCH
    if (!watchAddrs.empty())
    {
        ps2xN8D7M5SetCounting(false);
        for (const uint32_t a : watchAddrs)
        {
            uint32_t w = 0u;
            std::memcpy(&w, vram.data() + a, sizeof(w));
            std::cout << "[n8d7m5] final addr=0x" << std::hex << a
                      << " word=0x" << w << std::dec << '\n';
        }
        if (n8d7m5Dropped > 0u)
            std::cout << "[n8d7m5] truncated dropped=" << n8d7m5Dropped << '\n';
    }
#endif // PS2X_GS_REPLAY_WORD_WATCH

    result.parseOk = parseOk;
    result.hasStream = packets > 0u && markers > 0u;
    result.hasSamples = !rows.empty();
    result.packets = packets;
    result.priv = priv;
    result.transfers = transfers;
    result.markers = markers;
    result.readbacks = readbacks;
    result.clears = clears;
    result.roundedPackets = roundedPackets;
    result.queued = queued;
    result.parallelBackend = parallelBackend;
    result.dropPriv = dropPriv;
    result.rtz = rtzAll ? "all" : (rtzPath1 ? "path1" : "off");
    result.rows = rows;

    if (std::getenv("PS2X_GS_REPLAY_PACKET_TRACE"))
        result.packetTraceOk = packetTrace.good();

    std::cout << "GB4_REPLAY_SUMMARY mode=" << (queued ? "queue" : "direct")
              << " backend=" << (parallelBackend ? "parallel" : "cpu")
              << " drop_priv=" << (dropPriv ? 1 : 0) << " packets=" << packets
              << " priv=" << priv << " transfers=" << transfers
              << " markers=" << markers << " readbacks=" << readbacks
              << " clears=" << clears << " samples=" << rows.size()
              << " rtz=" << (rtzAll ? "all" : (rtzPath1 ? "path1" : "off"))
              << " rounded_packets=" << roundedPackets << '\n';
    if (parallelBackend)
    {
        const auto stats = ps2x_gs_parallel::stats();
        std::cout << "GB4_PARALLEL_STATS packets=" << stats.gifPackets
                  << " presents=" << stats.presents
                  << " null_scanouts=" << stats.nullScanouts
                  << " unsupported_clears=" << stats.unsupportedClears
                  << " unsupported_vram_io=" << stats.unsupportedVramIo
                  << " init_ok=" << stats.initOk << " init_failed=" << stats.initFailed << '\n';
    }
    for (const auto &row : rows)
        std::cout << row << '\n';

    if (const char *outPath = std::getenv("PS2X_GS_REPLAY_OUT"))
    {
        std::ofstream out(outPath, std::ios::binary);
        for (const auto &row : rows)
            out << row << '\n';
        result.outOk = out.good();
    }
    if (const char *expectedPath = std::getenv("PS2X_GS_REPLAY_EXPECT"))
    {
        std::ifstream in(expectedPath, std::ios::binary);
        std::vector<std::string> expected;
        std::string line;
        while (std::getline(in, line))
            expected.push_back(line);
        size_t first = 0;
        while (first < rows.size() && first < expected.size() && rows[first] == expected[first])
            ++first;
        if (first == rows.size() && first == expected.size())
        {
            result.expectOk = true;
        }
        else
        {
            char message[320];
            std::snprintf(message, sizeof(message),
                          "GB4 replay mismatch row=%zu got=%s expected=%s", first,
                          first < rows.size() ? rows[first].c_str() : "<EOF>",
                          first < expected.size() ? expected[first].c_str() : "<EOF>");
            result.expectOk = false;
            result.expectMessage = message;
        }
    }
    return result;
}
