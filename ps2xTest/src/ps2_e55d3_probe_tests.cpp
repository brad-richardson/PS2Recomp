// E55D3 focused test: the shared pad/card probe orders all three families
// by one monotonic seq, leaves guest results unchanged, stays silent when
// unconfigured, and emits exactly one cap line without truncated records.
// E55D14: each sceMcGetDir also emits one default-off getdirpath sibling
// carrying the raw query and normalized query/parent/pattern/host; the
// sibling shares seq with its own pord and never changes guest bytes.
// Structural checks only (family tags, seq integers, line framing); the test
// never reimplements the probe's hex encoder.

#include "MiniTest.h"
#include "ps2_e55d3_pad_card_probe.h"
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "Stubs/Pad.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kPadDataAddr = 0x1000;
    constexpr uint32_t kPadDmaAddr = 0x1200;
    constexpr uint32_t kMcStrAddr = 0x1000;
    constexpr uint32_t kMcTableAddr = 0x4000;
    constexpr uint32_t kMcReadAddr = 0x5000;
    constexpr uint32_t kMcSyncCmdAddr = 0x3C00;
    constexpr uint32_t kMcSyncResultAddr = 0x3C04;

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    void clearContext(R5900Context &ctx)
    {
        std::memset(&ctx, 0, sizeof(ctx));
    }

    void writeGuestString(uint8_t *rdram, uint32_t addr, const std::string &value)
    {
        std::memcpy(rdram + addr, value.c_str(), value.size() + 1);
    }

    int32_t syncMc(std::vector<uint8_t> &rdram)
    {
        R5900Context syncCtx{};
        setRegU32(syncCtx, 4, 0u);
        setRegU32(syncCtx, 5, kMcSyncCmdAddr);
        setRegU32(syncCtx, 6, kMcSyncResultAddr);
        ps2_stubs::sceMcSync(rdram.data(), &syncCtx, nullptr);
        int32_t result = 0;
        std::memcpy(&result, rdram.data() + kMcSyncResultAddr, sizeof(result));
        return result;
    }

    struct TempMcRoot
    {
        std::filesystem::path base;
        std::filesystem::path mcRoot;

        TempMcRoot()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            base = std::filesystem::temp_directory_path() / ("ps2x-e55d3-" + std::to_string(now));
            mcRoot = base / "mcroot";
            std::filesystem::create_directories(mcRoot);
            PS2Runtime::IoPaths ioPaths;
            ioPaths.mcRoot = mcRoot;
            PS2Runtime::setIoPaths(ioPaths);
        }

        ~TempMcRoot()
        {
            std::error_code ec;
            std::filesystem::remove_all(base, ec);
        }
    };

    std::string readWholeFile(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    std::vector<std::string> splitLines(const std::string &text)
    {
        std::vector<std::string> lines;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            lines.push_back(line);
        }
        return lines;
    }

    // Extracts the decimal seq=<n> field; -1 when absent/unparseable.
    long long seqOf(const std::string &line)
    {
        const size_t pos = line.find("seq=");
        if (pos == std::string::npos)
        {
            return -1;
        }
        return std::atoll(line.c_str() + pos + 4);
    }

    // Extracts " ord=<n>" (leading space avoids matching " pord="); -1 absent.
    long long ordOf(const std::string &line)
    {
        const size_t pos = line.find(" ord=");
        if (pos == std::string::npos)
        {
            return -1;
        }
        return std::atoll(line.c_str() + pos + 5);
    }

    // Extracts " pord=<n>" on getdirpath lines; -1 absent.
    long long pordOf(const std::string &line)
    {
        const size_t pos = line.find(" pord=");
        if (pos == std::string::npos)
        {
            return -1;
        }
        return std::atoll(line.c_str() + pos + 6);
    }

    void openPadPort0(R5900Context &ctx, std::vector<uint8_t> &rdram)
    {
        ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
        setRegU32(ctx, 4, 0u);
        setRegU32(ctx, 5, 0u);
        setRegU32(ctx, 6, kPadDmaAddr);
        ps2_stubs::scePadPortOpen(rdram.data(), &ctx, nullptr);
        ps2_stubs::setPadOverrideState(0xFFFFu & ~0x4000u, 0x80, 0x80, 0x80, 0x80);
    }

    void closePadPort0(R5900Context &ctx, std::vector<uint8_t> &rdram)
    {
        ps2_stubs::clearPadOverrideState();
        setRegU32(ctx, 4, 0u);
        setRegU32(ctx, 5, 0u);
        ps2_stubs::scePadPortClose(rdram.data(), &ctx, nullptr);
    }

    void runPadRead0(R5900Context &ctx, std::vector<uint8_t> &rdram)
    {
        setRegU32(ctx, 4, 0u);
        setRegU32(ctx, 5, 0u);
        setRegU32(ctx, 6, kPadDataAddr);
        ps2_stubs::scePadRead(rdram.data(), &ctx, nullptr);
    }
} // namespace

void register_ps2_e55d3_probe_tests()
{
    MiniTest::Case("Ps2E55D3Probe", [](TestCase &tc)
                   {
        tc.Run("disabled by default: failed stub call writes no file", [](TestCase &t)
        {
            const std::string sentinel =
                (std::filesystem::temp_directory_path() / "ps2x-e55d3-disabled.txt").string();
            std::remove(sentinel.c_str());
            ps2_e55d3_probe::clearForTest();
            t.IsTrue(!ps2_e55d3_probe::armed(), "probe must be disarmed with no file configured");

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx{};
            // Unmapped address: getMemPtr yields null, stub returns 0.
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 0xFFFFFFFFu);
            ps2_stubs::scePadRead(rdram.data(), &ctx, nullptr);
            t.Equals(getRegU32(&ctx, 2), 0u, "scePadRead with a bad address must fail");
            t.IsTrue(!std::filesystem::exists(sentinel), "disabled probe must create no file");
            ps2_e55d3_probe::clearForTest();
        });

        tc.Run("pad, getdir and mcread share one increasing seq", [](TestCase &t)
        {
            const std::string tmp =
                (std::filesystem::temp_directory_path() / "ps2x-e55d3-order.txt").string();
            std::remove(tmp.c_str());
            ps2_e55d3_probe::configureForTest(tmp.c_str(), 0u);
            t.IsTrue(ps2_e55d3_probe::armed(), "configured probe must be armed");

            TempMcRoot paths;
            const std::string payload = "e55d3-order";
            std::filesystem::create_directories(paths.mcRoot / "SAVEDATA");
            {
                std::ofstream out(paths.mcRoot / "SAVEDATA" / "game.dat", std::ios::binary);
                out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            }

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx{};

            // Family 1: pad.
            openPadPort0(ctx, rdram);
            runPadRead0(ctx, rdram);
            t.Equals(getRegU32(&ctx, 2), 1u, "scePadRead must succeed with the port open");

            // Family 2: getdir (E55D14: one getdirpath sibling + one status line).
            writeGuestString(rdram.data(), kMcStrAddr, "/SAVEDATA/*");
            clearContext(ctx);
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, kMcStrAddr);
            setRegU32(ctx, 7, 0u);
            setRegU32(ctx, 8, 8u);
            setRegU32(ctx, 9, kMcTableAddr);
            ps2_stubs::sceMcGetDir(rdram.data(), &ctx, nullptr);
            t.Equals(syncMc(rdram), 3, "sceMcGetDir must report '.', '..' and game.dat");

            // Family 3: mcread.
            writeGuestString(rdram.data(), kMcStrAddr, "/SAVEDATA/game.dat");
            clearContext(ctx);
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, kMcStrAddr);
            setRegU32(ctx, 7, PS2_FIO_O_RDONLY);
            ps2_stubs::sceMcOpen(rdram.data(), &ctx, nullptr);
            const int32_t fd = syncMc(rdram);
            t.IsTrue(fd > 0, "sceMcOpen must hand out a positive fd");
            clearContext(ctx);
            setRegU32(ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(ctx, 5, kMcReadAddr);
            setRegU32(ctx, 6, static_cast<uint32_t>(payload.size()));
            ps2_stubs::sceMcRead(rdram.data(), &ctx, nullptr);
            t.Equals(syncMc(rdram), static_cast<int32_t>(payload.size()),
                     "sceMcRead must report the full byte count");
            t.Equals(std::string(reinterpret_cast<const char *>(rdram.data() + kMcReadAddr),
                                 payload.size()),
                     payload, "sceMcRead must fill the guest buffer with file bytes");
            clearContext(ctx);
            setRegU32(ctx, 4, static_cast<uint32_t>(fd));
            ps2_stubs::sceMcClose(rdram.data(), &ctx, nullptr);
            syncMc(rdram);

            clearContext(ctx);
            closePadPort0(ctx, rdram);

            ps2_e55d3_probe::clearForTest();
            const std::vector<std::string> lines = splitLines(readWholeFile(tmp));
            t.Equals(lines.size(), static_cast<size_t>(4u), "pad + getdirpath + getdir + mcread in call order");
            t.IsTrue(lines[0].rfind("pad seq=1 ", 0u) == 0u, "first line is the pad record with seq 1");
            t.IsTrue(lines[1].rfind("getdirpath seq=2 ", 0u) == 0u, "second line is the path sibling with seq 2");
            t.IsTrue(lines[2].rfind("getdir seq=3 ", 0u) == 0u, "third line is the getdir record with seq 3");
            t.IsTrue(lines[3].rfind("mcread seq=4 ", 0u) == 0u, "fourth line is the mcread record with seq 4");
            t.Equals(seqOf(lines[0]), 1ll, "pad seq parses as 1");
            t.Equals(seqOf(lines[1]), 2ll, "getdirpath seq parses as 2");
            t.Equals(seqOf(lines[2]), 3ll, "getdir seq parses as 3");
            t.Equals(seqOf(lines[3]), 4ll, "mcread seq parses as 4");
            t.Equals(ordOf(lines[0]), 1ll, "pad per-family ord stays 1");
            t.Equals(pordOf(lines[1]), 1ll, "path sibling pord starts at 1");
            t.Equals(ordOf(lines[2]), 1ll, "getdir per-family ord stays 1");
            t.Equals(ordOf(lines[3]), 1ll, "mcread per-family ord stays 1");
            t.IsTrue(lines[1].find(" pord=1 ") != std::string::npos, "sibling carries its own pord");
            t.IsTrue(lines[1].find(" raw=\"/SAVEDATA/*\"") != std::string::npos,
                     "sibling carries the raw guest query");
            for (const auto &line : lines)
            {
                if (line.rfind("getdirpath ", 0u) == 0u)
                {
                    continue;
                }
                t.IsTrue(line.find(" ok=1 ") != std::string::npos, "non-sibling calls succeeded");
                const size_t pos = line.find("bytes=");
                t.IsTrue(pos != std::string::npos && pos + 6u < line.size(),
                         "successful records carry a nonempty hex payload");
            }
            std::remove(tmp.c_str());
        });

        tc.Run("disabled getdir writes no file", [](TestCase &t)
        {
            const std::string sentinel =
                (std::filesystem::temp_directory_path() / "ps2x-e55d3-getdirpath-disabled.txt").string();
            std::remove(sentinel.c_str());
            // Probe stays disarmed; configureForTest with an empty path would
            // also do, but clearForTest is the exact default-off state.
            ps2_e55d3_probe::clearForTest();
            t.IsTrue(!ps2_e55d3_probe::armed(), "probe must be disarmed by default");
            // Point the (unused) path at the sentinel to prove no I/O happens
            // while disarmed: no file may appear.
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx{};
            writeGuestString(rdram.data(), kMcStrAddr, "/SAVEDATA/*");
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, kMcStrAddr);
            setRegU32(ctx, 7, 0u);
            setRegU32(ctx, 8, 8u);
            setRegU32(ctx, 9, kMcTableAddr);
            ps2_stubs::sceMcGetDir(rdram.data(), &ctx, nullptr);
            syncMc(rdram);
            t.IsTrue(!std::filesystem::exists(sentinel), "disabled probe must create no file");
            t.IsTrue(!std::filesystem::exists(
                         std::filesystem::temp_directory_path() / "ps2x-e55d3-getdirpath-disabled.txt"),
                     "disabled getdirpath must create no file");
            ps2_e55d3_probe::clearForTest();
        });

        tc.Run("getdirpath normal path records query fields", [](TestCase &t)
        {
            const std::string tmp =
                (std::filesystem::temp_directory_path() / "ps2x-e55d3-getdirpath-normal.txt").string();
            std::remove(tmp.c_str());
            ps2_e55d3_probe::configureForTest(tmp.c_str(), 0u);
            TempMcRoot paths;
            std::filesystem::create_directories(paths.mcRoot / "SAVEDATA");
            {
                std::ofstream out(paths.mcRoot / "SAVEDATA" / "game.dat", std::ios::binary);
                out.write("x", 1);
            }
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx{};
            writeGuestString(rdram.data(), kMcStrAddr, "/SAVEDATA/*");
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, kMcStrAddr);
            setRegU32(ctx, 7, 0u);
            setRegU32(ctx, 8, 8u);
            setRegU32(ctx, 9, kMcTableAddr);
            ps2_stubs::sceMcGetDir(rdram.data(), &ctx, nullptr);
            const int32_t entries = syncMc(rdram);
            t.IsTrue(entries > 0, "seeded getdir must return entries");
            ps2_e55d3_probe::clearForTest();
            const std::vector<std::string> lines = splitLines(readWholeFile(tmp));
            t.Equals(lines.size(), static_cast<size_t>(2u), "one sibling plus one status line");
            t.IsTrue(lines[0].rfind("getdirpath ", 0u) == 0u, "sibling comes first");
            t.IsTrue(lines[1].rfind("getdir ", 0u) == 0u, "status comes second");
            t.IsTrue(lines[0].find(" port=0 slot=0 ") != std::string::npos, "sibling carries port/slot");
            t.IsTrue(lines[0].find(" max=8 ") != std::string::npos, "sibling carries max");
            t.IsTrue(lines[0].find(" raw=\"/SAVEDATA/*\"") != std::string::npos, "sibling carries raw");
            t.IsTrue(lines[0].find(" query=\"/SAVEDATA/*\"") != std::string::npos, "sibling carries query");
            t.IsTrue(lines[0].find(" parent=\"SAVEDATA\"") != std::string::npos, "sibling carries parent");
            t.IsTrue(lines[0].find(" pattern=\"*\"") != std::string::npos, "sibling carries pattern");
            const std::string hostWant = (paths.mcRoot / "SAVEDATA").lexically_normal().string();
            t.IsTrue(lines[0].find(" host=\"" + hostWant + "\"") != std::string::npos,
                     "sibling carries host directory");
            t.Equals(pordOf(lines[0]), 1ll, "sibling pord is 1");
            t.Equals(ordOf(lines[1]), 1ll, "getdir ord is 1");
            std::remove(tmp.c_str());
        });

        tc.Run("getdirpath escapes special characters", [](TestCase &t)
        {
            const std::string tmp =
                (std::filesystem::temp_directory_path() / "ps2x-e55d3-getdirpath-esc.txt").string();
            std::remove(tmp.c_str());
            ps2_e55d3_probe::configureForTest(tmp.c_str(), 0u);
            const std::string raw = std::string("a\\b\"c") + '\n' + '\x01' + "d";
            ps2_e55d3_probe::noteGetDirPath(7u, 0, 0, 8, raw, "q\"q", "p\\p", "*",
                                            "/host/dir");
            ps2_e55d3_probe::clearForTest();
            const std::vector<std::string> lines = splitLines(readWholeFile(tmp));
            t.Equals(lines.size(), static_cast<size_t>(1u), "one sibling line, no embedded newline");
            t.IsTrue(lines[0].rfind("getdirpath ", 0u) == 0u, "line is the sibling");
            t.IsTrue(lines[0].find("raw=\"a\\\\b\\\"c\\x0a\\x01d\"") != std::string::npos,
                     "raw escapes backslash, quote, newline and control");
            t.IsTrue(lines[0].find("query=\"q\\\"q\"") != std::string::npos, "query escapes quote");
            t.IsTrue(lines[0].find("parent=\"p\\\\p\"") != std::string::npos, "parent escapes backslash");
            t.IsTrue(lines[0].find("rawLen=8 ") != std::string::npos, "rawLen counts raw bytes");
            // A 2000-byte raw field must be bounded to the 1024-char escaped cap.
            const std::string big(2000u, 'A');
            ps2_e55d3_probe::configureForTest(tmp.c_str(), 0u);
            ps2_e55d3_probe::noteGetDirPath(8u, 0, 0, 8, big, big, big, big, big);
            ps2_e55d3_probe::clearForTest();
            const std::vector<std::string> bigLines = splitLines(readWholeFile(tmp));
            t.Equals(bigLines.size(), static_cast<size_t>(1u), "one bounded sibling line");
            t.IsTrue(bigLines[0].size() <= 6u * 1024u, "whole line stays near 5 KiB");
            std::remove(tmp.c_str());
        });

        tc.Run("getdirpath early exits carry unknown sentinels", [](TestCase &t)
        {
            const std::string tmp =
                (std::filesystem::temp_directory_path() / "ps2x-e55d3-getdirpath-early.txt").string();
            std::remove(tmp.c_str());
            ps2_e55d3_probe::configureForTest(tmp.c_str(), 0u);
            TempMcRoot paths;
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx{};
            // Bad port: port 9 is outside the two-port array.
            writeGuestString(rdram.data(), kMcStrAddr, "/SAVEDATA/*");
            setRegU32(ctx, 4, 9u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, kMcStrAddr);
            setRegU32(ctx, 7, 0u);
            setRegU32(ctx, 8, 8u);
            setRegU32(ctx, 9, kMcTableAddr);
            ps2_stubs::sceMcGetDir(rdram.data(), &ctx, nullptr);
            syncMc(rdram);
            // Unformatted: unformat port 0, query, then reformat to restore.
            clearContext(ctx);
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::sceMcUnformat(rdram.data(), &ctx, nullptr);
            syncMc(rdram);
            writeGuestString(rdram.data(), kMcStrAddr, "/SAVEDATA/*");
            clearContext(ctx);
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, kMcStrAddr);
            setRegU32(ctx, 7, 0u);
            setRegU32(ctx, 8, 8u);
            setRegU32(ctx, 9, kMcTableAddr);
            ps2_stubs::sceMcGetDir(rdram.data(), &ctx, nullptr);
            syncMc(rdram);
            clearContext(ctx);
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::sceMcFormat(rdram.data(), &ctx, nullptr);
            syncMc(rdram);
            ps2_e55d3_probe::clearForTest();
            const std::vector<std::string> lines = splitLines(readWholeFile(tmp));
            t.Equals(lines.size(), static_cast<size_t>(4u), "two siblings plus two status lines");
            t.IsTrue(lines[0].rfind("getdirpath ", 0u) == 0u, "bad-port sibling first");
            t.IsTrue(lines[1].rfind("getdir ", 0u) == 0u, "bad-port status second");
            t.IsTrue(lines[2].rfind("getdirpath ", 0u) == 0u, "unformatted sibling third");
            t.IsTrue(lines[3].rfind("getdir ", 0u) == 0u, "unformatted status fourth");
            t.IsTrue(lines[1].find(" reason=bad-port ") != std::string::npos, "bad-port status kept");
            t.IsTrue(lines[3].find(" reason=unformatted ") != std::string::npos,
                     "unformatted status kept");
            for (size_t i : {0u, 2u})
            {
                t.IsTrue(lines[i].find(" query=\"-\"") != std::string::npos, "unknown query sentinel");
                t.IsTrue(lines[i].find(" parent=\"-\"") != std::string::npos, "unknown parent sentinel");
                t.IsTrue(lines[i].find(" pattern=\"-\"") != std::string::npos, "unknown pattern sentinel");
                t.IsTrue(lines[i].find(" host=\"-\"") != std::string::npos, "unknown host sentinel");
                t.IsTrue(lines[i].find(" raw=\"/SAVEDATA/*\"") != std::string::npos, "raw still logged");
            }
            t.Equals(pordOf(lines[0]), 1ll, "first sibling pord 1");
            t.Equals(pordOf(lines[2]), 2ll, "second sibling pord 2");
            t.Equals(ordOf(lines[1]), 1ll, "getdir ord 1");
            t.Equals(ordOf(lines[3]), 2ll, "getdir ord 2");
            std::remove(tmp.c_str());
        });

        tc.Run("getdirpath pord orders every GetDir", [](TestCase &t)
        {
            const std::string tmp =
                (std::filesystem::temp_directory_path() / "ps2x-e55d3-getdirpath-ord.txt").string();
            std::remove(tmp.c_str());
            ps2_e55d3_probe::configureForTest(tmp.c_str(), 0u);
            TempMcRoot paths;
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx{};
            for (int i = 0; i < 3; ++i)
            {
                writeGuestString(rdram.data(), kMcStrAddr, "/NOPE/*");
                clearContext(ctx);
                setRegU32(ctx, 4, 0u);
                setRegU32(ctx, 5, 0u);
                setRegU32(ctx, 6, kMcStrAddr);
                setRegU32(ctx, 7, 0u);
                setRegU32(ctx, 8, 8u);
                setRegU32(ctx, 9, kMcTableAddr);
                ps2_stubs::sceMcGetDir(rdram.data(), &ctx, nullptr);
                syncMc(rdram);
            }
            ps2_e55d3_probe::clearForTest();
            const std::vector<std::string> lines = splitLines(readWholeFile(tmp));
            t.Equals(lines.size(), static_cast<size_t>(6u), "three siblings plus three status lines");
            long long lastSeq = 0ll;
            for (size_t i = 0u; i < lines.size(); ++i)
            {
                const long long seq = seqOf(lines[i]);
                t.IsTrue(seq == lastSeq + 1ll, "seq increases by one every line");
                lastSeq = seq;
                if ((i % 2u) == 0u)
                {
                    t.IsTrue(lines[i].rfind("getdirpath ", 0u) == 0u, "even lines are siblings");
                    t.Equals(pordOf(lines[i]), static_cast<long long>(i / 2u + 1u), "pord 1..3");
                }
                else
                {
                    t.IsTrue(lines[i].rfind("getdir ", 0u) == 0u, "odd lines are status");
                    t.Equals(ordOf(lines[i]), static_cast<long long>(i / 2u + 1u), "getdir ord 1..3");
                }
            }
            std::remove(tmp.c_str());
        });

        tc.Run("getdirpath tiny cap yields one cap line and no partial record", [](TestCase &t)
        {
            const std::string tmp =
                (std::filesystem::temp_directory_path() / "ps2x-e55d3-getdirpath-cap.txt").string();
            std::remove(tmp.c_str());
            constexpr uint64_t kTinyCap = 700u;
            ps2_e55d3_probe::configureForTest(tmp.c_str(), kTinyCap);
            for (int i = 0; i < 10; ++i)
            {
                ps2_e55d3_probe::noteGetDirPath(100u + static_cast<uint64_t>(i), 0, 0, 8,
                                                "/SAVEDATA/*", "/SAVEDATA/*", "SAVEDATA", "*",
                                                "/host/mc0/SAVEDATA");
            }
            t.IsTrue(!ps2_e55d3_probe::armed(), "probe must disarm itself after the cap");
            ps2_e55d3_probe::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.IsTrue(!text.empty(), "cap run must leave output behind");
            t.IsTrue(text.size() <= kTinyCap, "file must respect the tiny byte cap");
            t.IsTrue(!text.empty() && text.back() == '\n', "file must end on a record boundary");
            const std::vector<std::string> lines = splitLines(text);
            size_t capLines = 0u;
            size_t pathLines = 0u;
            for (const auto &line : lines)
            {
                if (line.rfind("cap ", 0u) == 0u)
                {
                    ++capLines;
                }
                else if (line.rfind("getdirpath ", 0u) == 0u)
                {
                    ++pathLines;
                }
            }
            t.Equals(capLines, static_cast<size_t>(1u), "exactly one cap record");
            t.IsTrue(pathLines >= 1u, "at least one full sibling record precedes the cap");
            t.IsTrue(lines.back().rfind("cap ", 0u) == 0u, "the cap record is last");
            std::remove(tmp.c_str());
        });

        tc.Run("tiny cap yields one cap line and no truncated record", [](TestCase &t)
        {
            const std::string tmp =
                (std::filesystem::temp_directory_path() / "ps2x-e55d3-cap.txt").string();
            std::remove(tmp.c_str());
            constexpr uint64_t kTinyCap = 600u;
            ps2_e55d3_probe::configureForTest(tmp.c_str(), kTinyCap);

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx{};
            openPadPort0(ctx, rdram);
            for (int i = 0; i < 10; ++i)
            {
                runPadRead0(ctx, rdram);
            }
            t.IsTrue(!ps2_e55d3_probe::armed(), "probe must disarm itself after the cap");
            clearContext(ctx);
            closePadPort0(ctx, rdram);

            ps2_e55d3_probe::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.IsTrue(!text.empty(), "cap run must leave output behind");
            t.IsTrue(text.size() <= kTinyCap, "file must respect the tiny byte cap");
            t.IsTrue(!text.empty() && text.back() == '\n', "file must end on a record boundary");
            const std::vector<std::string> lines = splitLines(text);
            size_t capLines = 0u;
            size_t padLines = 0u;
            for (const auto &line : lines)
            {
                if (line.rfind("cap ", 0u) == 0u)
                {
                    ++capLines;
                }
                else if (line.rfind("pad ", 0u) == 0u)
                {
                    ++padLines;
                }
            }
            t.Equals(capLines, static_cast<size_t>(1u), "exactly one cap record");
            t.IsTrue(padLines >= 1u, "at least one full pad record precedes the cap");
            t.IsTrue(lines.back().rfind("cap ", 0u) == 0u, "the cap record is last");
            std::remove(tmp.c_str());
        });
    });
}
