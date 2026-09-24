#include "MiniTest.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_parallel_backend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vq.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cfenv>
#include <fstream>
#include <iomanip>
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

    enum class ReadEventResult { Record, End, Invalid };

    ReadEventResult readEvent(FILE *f, uint32_t &length, std::vector<uint8_t> &record)
    {
        const size_t prefixBytes = std::fread(&length, 1, sizeof(length), f);
        if (prefixBytes == 0u && std::feof(f))
            return ReadEventResult::End;
        if (prefixBytes != sizeof(length))
            return ReadEventResult::Invalid;
        if (length < 9u || length > 64u * 1024u * 1024u)
            return ReadEventResult::Invalid;
        record.resize(length);
        return std::fread(record.data(), 1, length, f) == length
            ? ReadEventResult::Record : ReadEventResult::Invalid;
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

    struct Gb5IndexRow
    {
        uint64_t index = 0, tick = 0, offset = 0;
        unsigned path = 0, embeddedPath = 0, bytes = 0, gifFnv = 0;
    };

    bool loadGb5Index(const char *path, std::vector<Gb5IndexRow> &rows)
    {
        std::ifstream in(path);
        std::string line;
        if (!std::getline(in, line))
            return false;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line != "packet_index,tick,record_offset,corrected_path,embedded_path,length,fnv32")
            return false;
        while (std::getline(in, line))
        {
            Gb5IndexRow row{};
            unsigned long long index = 0, tick = 0, offset = 0;
            if (std::sscanf(line.c_str(), "%llu,%llu,%llu,%u,%u,%u,%x",
                            &index, &tick, &offset, &row.path,
                            &row.embeddedPath, &row.bytes, &row.gifFnv) != 7)
                return false;
            row.index = index;
            row.tick = tick;
            row.offset = offset;
            if (row.index != 143805u + rows.size() ||
                row.tick != (rows.size() < 461u ? 949u : 950u))
                return false;
            rows.push_back(row);
        }
        return in.eof() && rows.size() == 922u;
    }

    struct Gb5Crop
    {
        std::vector<uint32_t> pixels;
        uint32_t hash = 2166136261u;
    };

    struct Gb5Crops { Gb5Crop upper, lower; };

    Gb5Crop cropRgb(const PresentationFrame &frame, uint32_t x0, uint32_t y0,
                    uint32_t x1, uint32_t y1)
    {
        Gb5Crop crop;
        crop.pixels.reserve(static_cast<size_t>(x1 - x0) * (y1 - y0));
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = x0; x < x1; ++x)
            {
                const size_t pos = (static_cast<size_t>(y) * 640u + x) * 4u;
                const uint32_t rgb = (static_cast<uint32_t>(frame.pixels[pos]) << 16u) |
                    (static_cast<uint32_t>(frame.pixels[pos + 1u]) << 8u) |
                    frame.pixels[pos + 2u];
                crop.pixels.push_back(rgb);
                for (unsigned shift : {16u, 8u, 0u})
                {
                    crop.hash ^= (rgb >> shift) & 0xffu;
                    crop.hash *= 16777619u;
                }
            }
        return crop;
    }

    bool readGb5Crops(GSCpuBackend &raw, const GSRegisters &regs,
                      uint64_t tick, Gb5Crops &out)
    {
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
        const PresentationFrame frame = raw.Present(request);
        if (!frame || frame.width != 512u || frame.height != 448u ||
            frame.displayFbp != 112u || frame.sourceFbp != 112u ||
            frame.pixels.size() < 640u * 448u * 4u)
            return false;
        out.upper = cropRgb(frame, 320u, 120u, 420u, 205u);
        out.lower = cropRgb(frame, 340u, 360u, 430u, 420u);
        return true;
    }

    uint32_t changedPixels(const Gb5Crop &before, const Gb5Crop &after)
    {
        uint32_t count = 0;
        for (size_t i = 0; i < before.pixels.size(); ++i)
            count += before.pixels[i] != after.pixels[i];
        return count;
    }

    bool parseGb5bTickRange(const char *text, uint64_t &lo, uint64_t &hi)
    {
        if (!text || !*text)
            return false;
        char *end = nullptr;
        const unsigned long long first = std::strtoull(text, &end, 10);
        if (end == text || *end != ',')
            return false;
        const char *second = end + 1;
        const unsigned long long last = std::strtoull(second, &end, 10);
        if (end == second || *end != '\0' || first == 0u || last < first)
            return false;
        lo = first;
        hi = last;
        return true;
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

    void replay(TestCase &t)
    {
        const char *path = std::getenv("PS2X_GS_REPLAY_CAPTURE");
        if (!path || !path[0])
        {
            t.IsTrue(true, "PS2X_GS_REPLAY_CAPTURE unset; GB4 replay skipped");
            return;
        }
        FILE *f = std::fopen(path, "rb");
        if (!f)
        {
            t.IsTrue(false, "cannot open PS2X_GS_REPLAY_CAPTURE");
            return;
        }
        char magic[8]{};
        if (std::fread(magic, 1, sizeof(magic), f) != sizeof(magic) ||
            std::memcmp(magic, "PS2XGSC1", sizeof(magic)) != 0)
        {
            std::fclose(f);
            t.IsTrue(false, "invalid GB4 capture header");
            return;
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
            t.IsTrue(false, "PS2X_GS_REPLAY_RTZ must be path1 or all");
            return;
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
                t.IsTrue(false, "invalid PS2X_GS_REPLAY_PATH_FILE");
                return;
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
        if (queued)
            gs.setQueueEnabled(true);
        if (parallelBackend)
        {
            if (!ps2x_gs_parallel::available())
            {
                std::fclose(f);
                t.IsTrue(false, "parallel replay requested without compiled backend");
                return;
            }
            gs.setRasterBackend(ps2x_gs_parallel::create(&regs));
        }

        std::vector<Gb5IndexRow> gb5Index;
        std::ofstream gb5Trace;
        GSCpuBackend gb5Raw;
        const char *gb5Out = std::getenv("PS2X_GS_REPLAY_GB5_TRACE");
        const bool gb5Probe = gb5Out && *gb5Out;
        if (gb5Probe)
        {
            const char *gb5IndexPath = std::getenv("PS2X_GS_REPLAY_GB5_INDEX");
            if (parallelBackend || queued || !gb5IndexPath ||
                !loadGb5Index(gb5IndexPath, gb5Index))
            {
                std::fclose(f);
                t.IsTrue(false, "GB5 requires direct CPU replay and valid X5 index");
                return;
            }
            gb5Raw.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
            gb5Trace.open(gb5Out, std::ios::binary);
            if (!gb5Trace)
            {
                std::fclose(f);
                t.IsTrue(false, "GB5 crop trace could not be opened");
                return;
            }
            gb5Trace << "tick,packet_index,path,gif_bytes,gif_fnv32,upper_before,upper_after,upper_changed,lower_before,lower_after,lower_changed\n";
        }

        std::ofstream gb5bTrace, gb5bTimeline;
        GSCpuBackend gb5bRaw;
        uint64_t gb5bLo = 0u, gb5bHi = 0u, gb5bDrop = UINT64_MAX;
        const char *gb5bOut = std::getenv("PS2X_GS_REPLAY_GB5B_TRACE");
        const bool gb5bProbe = gb5bOut && *gb5bOut;
        if (gb5bProbe)
        {
            const char *range = std::getenv("PS2X_GS_REPLAY_GB5B_TICKS");
            if (parallelBackend || queued || gb5Probe ||
                !parseGb5bTickRange(range, gb5bLo, gb5bHi))
            {
                std::fclose(f);
                t.IsTrue(false, "GB5B requires direct CPU replay, no GB5 probe, and valid GB5B tick range");
                return;
            }
            if (const char *drop = std::getenv("PS2X_GS_REPLAY_GB5B_DROP"))
            {
                char *end = nullptr;
                gb5bDrop = std::strtoull(drop, &end, 10);
                if (end == drop || *end != '\0')
                {
                    std::fclose(f);
                    t.IsTrue(false, "GB5B drop index must be numeric");
                    return;
                }
            }
            gb5bRaw.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
            gb5bTrace.open(gb5bOut, std::ios::binary);
            gb5bTimeline.open(std::string(gb5bOut) + ".timeline", std::ios::binary);
            if (!gb5bTrace || !gb5bTimeline)
            {
                std::fclose(f);
                t.IsTrue(false, "GB5B trace or timeline could not be opened");
                return;
            }
            gb5bTrace << "tick,packet_index,path,gif_bytes,gif_fnv32,upper_before,upper_after,upper_changed,lower_before,lower_after,lower_changed\n";
            gb5bTimeline << "tick,record,offset,info\n";
        }
        else if (const char *drop = std::getenv("PS2X_GS_REPLAY_GB5B_DROP"))
        {
            if (*drop)
            {
                std::fclose(f);
                t.IsTrue(false, "GB5B drop requires the GB5B probe");
                return;
            }
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

        uint64_t packets = 0u, priv = 0u, transfers = 0u, markers = 0u, roundedPackets = 0u;
        uint32_t gb5Rows = 0u;
        uint32_t gb5bRows = 0u, gb5bTimelineRows = 0u;
        uint64_t gb5bPriv = 0u, gb5bTransfers = 0u, gb5bMarkers = 0u;
        uint64_t gb5bReadbacks = 0u, gb5bClears = 0u, gb5bNative = 0u;
        Gb5Crops gb5bPrevAfter;
        uint64_t gb5bPrevTick = 0u;
        bool gb5bPrevValid = false, gb5bSawHi = false;
        uint64_t readbacks = 0u, clears = 0u;
        bool parseOk = true;
        std::vector<std::string> rows;
        while (true)
        {
            uint32_t length = 0;
            std::vector<uint8_t> rec;
            const long recordOffset = std::ftell(f);
            const ReadEventResult readResult = readEvent(f, length, rec);
            if (readResult != ReadEventResult::Record)
            {
                if (readResult == ReadEventResult::Invalid)
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
                const bool gb5Packet = gb5Probe && packets >= 143805u && packets <= 144726u;
                Gb5Crops gb5Before, gb5After;
                const Gb5IndexRow *gb5Row = nullptr;
                if (gb5Packet)
                {
                    gb5Row = &gb5Index[packets - 143805u];
                    if (gb5Row->index != packets || gb5Row->tick != tick ||
                        gb5Row->offset != static_cast<uint64_t>(recordOffset) ||
                        gb5Row->path != pathId || gb5Row->embeddedPath != rec[9] ||
                        gb5Row->bytes != size || gb5Row->gifFnv != fnv(rec.data() + 14, size) ||
                        !readGb5Crops(gb5Raw, regs, tick, gb5Before))
                    {
                        parseOk = false;
                        std::cerr << "GB5_PROBE_ERROR before packet=" << packets << '\n';
                        break;
                    }
                }
                const bool gb5bPacket = gb5bProbe && tick >= gb5bLo && tick <= gb5bHi;
                Gb5Crops gb5bBefore, gb5bAfter;
                if (gb5bPacket)
                {
                    if (!readGb5Crops(gb5bRaw, regs, tick, gb5bBefore))
                    {
                        parseOk = false;
                        std::cerr << "GB5B_PROBE_ERROR before packet=" << packets << " tick=" << tick << '\n';
                        break;
                    }
                }
                gs.noteGifPath(static_cast<GifPathId>(pathId));
                const bool forceRtz = rtzAll || (rtzPath1 && pathId == 1u);
                {
                    ScopedReplayRtz scope(forceRtz);
                    if (!scope.ok)
                    {
                        parseOk = false;
                        break;
                    }
                    if (packets != gb5bDrop)
                        gs.processGIFPacket(rec.data() + 14, size);
                }
                if (gb5Packet)
                {
                    if (!readGb5Crops(gb5Raw, regs, tick, gb5After))
                    {
                        parseOk = false;
                        std::cerr << "GB5_PROBE_ERROR after packet=" << packets << '\n';
                        break;
                    }
                    const uint32_t upperChanged = changedPixels(gb5Before.upper, gb5After.upper);
                    const uint32_t lowerChanged = changedPixels(gb5Before.lower, gb5After.lower);
                    if (upperChanged || lowerChanged)
                    {
                        if (++gb5Rows > 1000u)
                        {
                            parseOk = false;
                            std::cerr << "GB5_PROBE_ERROR row cap exceeded\n";
                            break;
                        }
                        gb5Trace << tick << ',' << packets << ',' << static_cast<unsigned>(pathId) << ',' << size << ','
                                 << std::hex << std::setw(8) << std::setfill('0') << gb5Row->gifFnv << ','
                                 << std::setw(8) << gb5Before.upper.hash << ','
                                 << std::setw(8) << gb5After.upper.hash << std::dec << ',' << upperChanged << ','
                                 << std::hex << std::setw(8) << gb5Before.lower.hash << ','
                                 << std::setw(8) << gb5After.lower.hash << std::dec << ',' << lowerChanged << '\n';
                    }
                }
                if (gb5bPacket)
                {
                    if (!readGb5Crops(gb5bRaw, regs, tick, gb5bAfter))
                    {
                        parseOk = false;
                        std::cerr << "GB5B_PROBE_ERROR after packet=" << packets << " tick=" << tick << '\n';
                        break;
                    }
                    if (gb5bPrevValid && tick == gb5bPrevTick &&
                        (gb5bBefore.upper.hash != gb5bPrevAfter.upper.hash ||
                         gb5bBefore.lower.hash != gb5bPrevAfter.lower.hash))
                    {
                        if (++gb5bTimelineRows > 5000u)
                        {
                            parseOk = false;
                            std::cerr << "GB5B_PROBE_ERROR timeline cap exceeded\n";
                            break;
                        }
                        gb5bTimeline << tick << ",gap," << packets << ",upper_prev="
                                     << std::hex << std::setw(8) << std::setfill('0') << gb5bPrevAfter.upper.hash
                                     << "_before=" << std::setw(8) << gb5bBefore.upper.hash
                                     << "_lower_prev=" << std::setw(8) << gb5bPrevAfter.lower.hash
                                     << "_before=" << std::setw(8) << gb5bBefore.lower.hash
                                     << std::dec << '\n';
                    }
                    const uint32_t upperChanged = changedPixels(gb5bBefore.upper, gb5bAfter.upper);
                    const uint32_t lowerChanged = changedPixels(gb5bBefore.lower, gb5bAfter.lower);
                    if (upperChanged || lowerChanged)
                    {
                        if (++gb5bRows > 1000u)
                        {
                            parseOk = false;
                            std::cerr << "GB5B_PROBE_ERROR row cap exceeded\n";
                            break;
                        }
                        gb5bTrace << tick << ',' << packets << ',' << static_cast<unsigned>(pathId) << ',' << size << ','
                                 << std::hex << std::setw(8) << std::setfill('0') << fnv(rec.data() + 14, size) << ','
                                 << std::setw(8) << gb5bBefore.upper.hash << ','
                                 << std::setw(8) << gb5bAfter.upper.hash << std::dec << ',' << upperChanged << ','
                                 << std::hex << std::setw(8) << gb5bBefore.lower.hash << ','
                                 << std::setw(8) << gb5bAfter.lower.hash << std::dec << ',' << lowerChanged << '\n';
                    }
                    gb5bPrevAfter = gb5bAfter;
                    gb5bPrevTick = tick;
                    gb5bPrevValid = true;
                }
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
                if (gb5bProbe && tick >= gb5bLo && tick <= gb5bHi)
                {
                    if (++gb5bTimelineRows > 5000u)
                    {
                        parseOk = false;
                        break;
                    }
                    gb5bTimeline << tick << ",priv," << recordOffset << ",off="
                                 << std::hex << std::setw(4) << std::setfill('0') << offset
                                 << "_value=" << std::setw(16) << value << std::dec << '\n';
                    ++gb5bPriv;
                }
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
                if (gb5bProbe && tick >= gb5bLo && tick <= gb5bHi)
                    ++gb5bTransfers;
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
                if (gb5bProbe && tick >= gb5bLo && tick <= gb5bHi)
                {
                    if (++gb5bTimelineRows > 5000u)
                    {
                        parseOk = false;
                        break;
                    }
                    gb5bTimeline << tick << ",marker," << recordOffset << ",\n";
                    ++gb5bMarkers;
                }
                if (gb5bProbe && tick == gb5bHi)
                {
                    Gb5Crops finalCrops;
                    if (!readGb5Crops(gb5bRaw, regs, tick, finalCrops))
                    {
                        parseOk = false;
                        std::cerr << "GB5B_PROBE_ERROR final tick=" << tick << '\n';
                        break;
                    }
                    std::cout << "GB5B_FINAL tick=" << tick << " upper=" << std::hex
                              << std::setw(8) << std::setfill('0') << finalCrops.upper.hash
                              << " lower=" << std::setw(8) << finalCrops.lower.hash
                              << std::dec << " rows=" << gb5bRows << '\n';
                    gb5bSawHi = true;
                }
                if (gb5Probe && tick == 950u)
                {
                    Gb5Crops finalCrops;
                    if (!readGb5Crops(gb5Raw, regs, tick, finalCrops))
                    {
                        parseOk = false;
                        std::cerr << "GB5_PROBE_ERROR final tick=950\n";
                        break;
                    }
                    std::cout << "GB5_FINAL tick=950 upper=" << std::hex
                              << std::setw(8) << std::setfill('0') << finalCrops.upper.hash
                              << " lower=" << std::setw(8) << finalCrops.lower.hash
                              << std::dec << " rows=" << gb5Rows << '\n';
                }
                const bool sampled = tick % stride == 0u;
                const bool named = dumpTick(tick);
                if (!sampled && !named)
                    continue;
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
                    gs.uploadImageNative(regsIn[0], regsIn[1], regsIn[2], regsIn[3],
                                         rec.data() + 45, size);
                }
                if (rtzAll)
                    ++roundedPackets;
                if (packetTrace && tick < bisectTo)
                    packetTrace << packets << ',' << tick << ",native,"
                                << std::hex << fnv(vram.data(), vram.size()) << std::dec << '\n';
                if (gb5bProbe && tick >= gb5bLo && tick <= gb5bHi)
                {
                    if (++gb5bTimelineRows > 5000u)
                    {
                        parseOk = false;
                        break;
                    }
                    gb5bTimeline << tick << ",native," << recordOffset << ",bytes=" << size << '\n';
                    ++gb5bNative;
                }
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
                if (gb5bProbe && tick >= gb5bLo && tick <= gb5bHi)
                {
                    if (++gb5bTimelineRows > 5000u)
                    {
                        parseOk = false;
                        break;
                    }
                    gb5bTimeline << tick << ",readback," << recordOffset << ",bytes=" << size << '\n';
                    ++gb5bReadbacks;
                }
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
                gs.clearFramebufferContext(context, rgba);
                ++clears;
                if (gb5bProbe && tick >= gb5bLo && tick <= gb5bHi)
                {
                    if (++gb5bTimelineRows > 5000u)
                    {
                        parseOk = false;
                        break;
                    }
                    gb5bTimeline << tick << ",clear," << recordOffset << ",context=" << context << '\n';
                    ++gb5bClears;
                }
            }
            else
            {
                parseOk = false;
                break;
            }
        }
        std::fclose(f);
        gs.drainQueue();

        if (std::getenv("PS2X_GS_REPLAY_PACKET_TRACE"))
            t.IsTrue(packetTrace.good(), "GB4 packet trace written");
        if (gb5Probe)
            t.IsTrue(gb5Trace.good() && packets > 144726u, "GB5 crop trace and X5 window complete");
        if (gb5bProbe)
            t.IsTrue(gb5bTrace.good() && gb5bTimeline.good() && gb5bSawHi, "GB5B crop trace, timeline and final tick complete");

        t.IsTrue(parseOk, "GB4 capture records parse cleanly");
        t.IsTrue(packets > 0u && markers > 0u, "capture has GIF packets and VBlank markers");
        t.IsTrue(!rows.empty(), "capture has sampled hashes");
        std::cout << "GB4_REPLAY_SUMMARY mode=" << (queued ? "queue" : "direct")
                  << " backend=" << (parallelBackend ? "parallel" : "cpu")
                  << " drop_priv=" << (dropPriv ? 1 : 0) << " packets=" << packets
                  << " priv=" << priv << " transfers=" << transfers
                  << " markers=" << markers << " readbacks=" << readbacks
                  << " clears=" << clears << " samples=" << rows.size()
                  << " rtz=" << (rtzAll ? "all" : (rtzPath1 ? "path1" : "off"))
                  << " rounded_packets=" << roundedPackets << '\n';
        if (gb5bProbe)
        {
            std::cout << "GB5B_NONPACKET ticks=" << gb5bLo << '-' << gb5bHi
                      << " priv=" << gb5bPriv << " transfers=" << gb5bTransfers
                      << " markers=" << gb5bMarkers << " native=" << gb5bNative
                      << " readbacks=" << gb5bReadbacks << " clears=" << gb5bClears
                      << " rows=" << gb5bRows << " timeline=" << gb5bTimelineRows
                      << " drop=";
            if (gb5bDrop == UINT64_MAX)
                std::cout << "none\n";
            else
                std::cout << gb5bDrop << '\n';
        }
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
            t.IsTrue(out.good(), "replay hashes written");
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
                t.IsTrue(true, "GB4 replay hashes match expected stream");
            else
            {
                char message[320];
                std::snprintf(message, sizeof(message),
                              "GB4 replay mismatch row=%zu got=%s expected=%s", first,
                              first < rows.size() ? rows[first].c_str() : "<EOF>",
                              first < expected.size() ? expected[first].c_str() : "<EOF>");
                t.IsTrue(false, message);
            }
        }
    }
}

void register_ps2_gs_replay_tests()
{
    MiniTest::Case("PS2GSReplay", [](TestCase &tc)
    {
        tc.Run("GB4 rejects a partial capture record at EOF", [](TestCase &t)
        {
            FILE *f = std::tmpfile();
            if (!f)
            {
                t.IsTrue(false, "tmpfile opened");
                return;
            }
            uint32_t length = 9u;
            std::fwrite(&length, sizeof(length), 1, f);
            const uint8_t body[3] = {4u, 0u, 0u};
            std::fwrite(body, 1, sizeof(body), f);
            std::rewind(f);
            std::vector<uint8_t> record;
            t.IsTrue(readEvent(f, length, record) == ReadEventResult::Invalid,
                     "truncated body is invalid even when fread sets EOF");
            std::rewind(f);
            t.IsTrue(readEvent(f, length, record) == ReadEventResult::Invalid,
                     "partial body stays invalid on a second read");
            std::fclose(f);
        });
        tc.Run("GB4 replays the captured GS command stream and hashes VBlank checkpoints",
               [](TestCase &t) { replay(t); });
    });
}
