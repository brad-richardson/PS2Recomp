#include "MiniTest.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"

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
        const bool queued = mode && std::strcmp(mode, "queue") == 0;
        const char *drop = std::getenv("PS2X_GS_REPLAY_DROP_PRIV");
        const bool dropPriv = drop && std::strcmp(drop, "1") == 0;
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

        uint64_t bisectTo = 0u;
        if (const char *value = std::getenv("PS2X_GS_REPLAY_BISECT_TO"))
            bisectTo = std::strtoull(value, nullptr, 10);
        std::ofstream packetTrace;
        if (const char *out = std::getenv("PS2X_GS_REPLAY_PACKET_TRACE"))
        {
            packetTrace.open(out, std::ios::binary);
            packetTrace << "index,tick,path,vram\n";
        }

        uint64_t packets = 0u, priv = 0u, transfers = 0u, markers = 0u;
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
                const uint8_t pathId = rec[9];
                uint32_t size = 0;
                std::memcpy(&size, rec.data() + 10, 4);
                if (pathId < 1u || pathId > 3u || size != length - 14u)
                {
                    parseOk = false;
                    break;
                }
                gs.noteGifPath(static_cast<GifPathId>(pathId));
                gs.processGIFPacket(rec.data() + 14, size);
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
                if (tick % stride != 0u)
                    continue;
                gs.refreshDisplaySnapshot();
                uint32_t vramSize = 0;
                const uint8_t *vramData = gs.lockDisplaySnapshot(vramSize);
                const uint32_t vramHash = vramData ? fnv(vramData, vramSize) : 0u;
                gs.unlockDisplaySnapshot();
                const PresentationFrame frame = gs.presentForDiagnostics();
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
                gs.uploadImageNative(regsIn[0], regsIn[1], regsIn[2], regsIn[3],
                                     rec.data() + 45, size);
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
                gs.clearFramebufferContext(context, rgba);
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

        if (std::getenv("PS2X_GS_REPLAY_PACKET_TRACE"))
            t.IsTrue(packetTrace.good(), "GB4 packet trace written");

        t.IsTrue(parseOk, "GB4 capture records parse cleanly");
        t.IsTrue(packets > 0u && markers > 0u, "capture has GIF packets and VBlank markers");
        t.IsTrue(!rows.empty(), "capture has sampled hashes");
        std::cout << "GB4_REPLAY_SUMMARY mode=" << (queued ? "queue" : "direct")
                  << " drop_priv=" << (dropPriv ? 1 : 0) << " packets=" << packets
                  << " priv=" << priv << " transfers=" << transfers
                  << " markers=" << markers << " readbacks=" << readbacks
                  << " clears=" << clears << " samples=" << rows.size() << '\n';
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
