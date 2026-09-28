#include "MiniTest.h"
#include "ps2_build_id.h"
#include "ps2_record_env.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "runtime/ps2_pad.h"
#include "Stubs/Pad.h"

#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>


namespace
{
    constexpr uint32_t kPadDataAddr = 0x1000;

    constexpr uint16_t kPadBtnSelect = 1u << 0;
    constexpr uint16_t kPadBtnL3 = 1u << 1;
    constexpr uint16_t kPadBtnR3 = 1u << 2;
    constexpr uint16_t kPadBtnStart = 1u << 3;
    constexpr uint16_t kPadBtnUp = 1u << 4;
    constexpr uint16_t kPadBtnRight = 1u << 5;
    constexpr uint16_t kPadBtnDown = 1u << 6;
    constexpr uint16_t kPadBtnLeft = 1u << 7;
    constexpr uint16_t kPadBtnL2 = 1u << 8;
    constexpr uint16_t kPadBtnR2 = 1u << 9;
    constexpr uint16_t kPadBtnL1 = 1u << 10;
    constexpr uint16_t kPadBtnR1 = 1u << 11;
    constexpr uint16_t kPadBtnTriangle = 1u << 12;
    constexpr uint16_t kPadBtnCircle = 1u << 13;
    constexpr uint16_t kPadBtnCross = 1u << 14;
    constexpr uint16_t kPadBtnSquare = 1u << 15;

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    void openPadPort(R5900Context &ctx, std::vector<uint8_t> &rdram, uint32_t port = 0, uint32_t slot = 0)
    {
        setRegU32(ctx, 4, port);
        setRegU32(ctx, 5, slot);
        setRegU32(ctx, 6, kPadDataAddr + 0x200u);
        ps2_stubs::scePadPortOpen(rdram.data(), &ctx, nullptr);
    }

    void closePadPort(R5900Context &ctx, std::vector<uint8_t> &rdram, uint32_t port = 0, uint32_t slot = 0)
    {
        setRegU32(ctx, 4, port);
        setRegU32(ctx, 5, slot);
        ps2_stubs::scePadPortClose(rdram.data(), &ctx, nullptr);
    }

    void runPadRead(R5900Context &ctx, std::vector<uint8_t> &rdram)
    {
        setRegU32(ctx, 4, 0u);
        setRegU32(ctx, 5, 0u);
        setRegU32(ctx, 6, kPadDataAddr); // a2
        ps2_stubs::scePadRead(rdram.data(), &ctx, nullptr);
    }

    uint16_t readButtons(const std::vector<uint8_t> &rdram)
    {
        const uint8_t *data = rdram.data() + kPadDataAddr;
        return static_cast<uint16_t>(data[2] | (data[3] << 8));
    }

    // PL1: save/restore one env var across a snapshot test (mirrors the
    // EnvGuard in ps2_gs_external_tests.cpp).
    struct Pl1EnvGuard
    {
        explicit Pl1EnvGuard(const char *name) : m_name(name)
        {
            if (const char *v = std::getenv(name))
            {
                m_old = v;
                m_had = true;
            }
        }
        ~Pl1EnvGuard()
        {
            if (m_had)
            {
                ::setenv(m_name, m_old.c_str(), 1);
            }
            else
            {
                ::unsetenv(m_name);
            }
        }
        const char *m_name;
        std::string m_old;
        bool m_had = false;
    };

    std::string readFileBytes(const std::filesystem::path &path)
    {
        std::string content;
        if (std::FILE *f = std::fopen(path.string().c_str(), "rb"))
        {
            char buf[4096];
            size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            {
                content.append(buf, n);
            }
            std::fclose(f);
        }
        return content;
    }

    uint64_t pl1Fnv1a(const void *data, size_t size, uint64_t hash)
    {
        const uint8_t *bytes = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    // The header's snapshot-hash scheme, recomputed independently: FNV-1a 64
    // over sorted relpaths (+ NUL) and file bytes.
    uint64_t pl1SaveSetHash(const std::filesystem::path &root)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::is_directory(root, ec) || ec)
        {
            return 0u; // missing input reads 0; the existence assert fires
        }
        std::vector<std::string> rels;
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code ec2;
            if (it->is_regular_file(ec2) && !ec2)
            {
                std::error_code ec3;
                std::string rel = fs::relative(it->path(), root, ec3).string();
                if (!ec3)
                {
                    rels.push_back(rel);
                }
            }
        }
        std::sort(rels.begin(), rels.end());
        uint64_t hash = 14695981039346656037ull;
        for (const std::string &rel : rels)
        {
            hash = pl1Fnv1a(rel.data(), rel.size(), hash);
            hash = pl1Fnv1a("\0", 1, hash);
            const std::string bytes = readFileBytes(root / rel);
            hash = pl1Fnv1a(bytes.data(), bytes.size(), hash);
        }
        return hash;
    }
}

void register_pad_input_tests()
{
    MiniTest::Case("PadInput", [](TestCase &tc)
                   {
        tc.Run("scePadRead uses override state", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            const uint16_t buttons = static_cast<uint16_t>(0xFFFFu & ~kPadBtnCross & ~kPadBtnStart);
            ps2_stubs::setPadOverrideState(buttons, 0x00, 0xFF, 0x10, 0xEE);

            runPadRead(ctx, rdram);

            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadRead should return 1");
            t.Equals(readButtons(rdram), buttons, "button bitmask should match override state");
            const uint8_t *data = rdram.data() + kPadDataAddr;
            t.Equals(data[4], static_cast<uint8_t>(0x10), "rx should match override");
            t.Equals(data[5], static_cast<uint8_t>(0xEE), "ry should match override");
            t.Equals(data[6], static_cast<uint8_t>(0x00), "lx should match override");
            t.Equals(data[7], static_cast<uint8_t>(0xFF), "ly should match override");

            ps2_stubs::clearPadOverrideState();
            closePadPort(ctx, rdram);
        });

        tc.Run("scePadRead button bits are active-low", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            struct ButtonCase
            {
                uint16_t mask;
                const char *name;
            };

            const ButtonCase cases[] = {
                {kPadBtnSelect, "select"},
                {kPadBtnL3, "l3"},
                {kPadBtnR3, "r3"},
                {kPadBtnStart, "start"},
                {kPadBtnUp, "up"},
                {kPadBtnRight, "right"},
                {kPadBtnDown, "down"},
                {kPadBtnLeft, "left"},
                {kPadBtnL2, "l2"},
                {kPadBtnR2, "r2"},
                {kPadBtnL1, "l1"},
                {kPadBtnR1, "r1"},
                {kPadBtnTriangle, "triangle"},
                {kPadBtnCircle, "circle"},
                {kPadBtnCross, "cross"},
                {kPadBtnSquare, "square"}};

            for (const auto &entry : cases)
            {
                const uint16_t buttons = static_cast<uint16_t>(0xFFFFu & ~entry.mask);
                ps2_stubs::setPadOverrideState(buttons, 0x80, 0x80, 0x80, 0x80);
                runPadRead(ctx, rdram);

                t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadRead should succeed for opened ports");
                const uint16_t mask = readButtons(rdram);
                t.IsTrue((mask & entry.mask) == 0, std::string("button should be active-low: ").append(entry.name));
            }

            ps2_stubs::clearPadOverrideState();
            closePadPort(ctx, rdram);
        });

        tc.Run("scePadGetButtonMask returns all buttons", [](TestCase &t)
               {
            R5900Context ctx;
            ps2_stubs::scePadGetButtonMask(nullptr, &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0xFFFF), "button mask should be 0xFFFF");
        });

        tc.Run("basic pad init/port/state functions return expected values", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadInit should succeed");

            ps2_stubs::scePadInit2(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadInit2 should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "closed port should report DISCONNECTED");

            openPadPort(ctx, rdram);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadPortOpen should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(6), "scePadGetState should return STABLE");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetReqState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "scePadGetReqState should return completed");

            ps2_stubs::scePadGetPortMax(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(2), "scePadGetPortMax should be 2");

            setRegU32(ctx, 4, 0u);
            ps2_stubs::scePadGetSlotMax(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadGetSlotMax should be 1");

            ps2_stubs::scePadGetModVersion(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0x0200), "scePadGetModVersion should be 0x0200");

            closePadPort(ctx, rdram);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadPortClose should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "closed port should return DISCONNECTED after close");
        });

        tc.Run("pad command state reports EXECCMD once before returning STABLE", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1u);
            setRegU32(ctx, 7, 3u);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(5), "first state after mode command should be EXECCMD");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(6), "second state after mode command should return STABLE");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadEnterPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadEnterPressMode should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(5), "first state after press-mode command should be EXECCMD");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(6), "second state after press-mode command should return STABLE");

            closePadPort(ctx, rdram);
        });

        tc.Run("pad info and mode helpers return consistent values", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 6, static_cast<uint32_t>(-1));
            ps2_stubs::scePadInfoAct(rdram.data(), &ctx, nullptr);
            t.IsTrue(static_cast<uint32_t>(getRegU32(&ctx, 2)) >= 1u, "scePadInfoAct should report at least one actuator descriptor");

            ps2_stubs::scePadInfoComb(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "scePadInfoComb should return 0");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1);
            setRegU32(ctx, 7, 0);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(4), "scePadInfoMode CURID should return digital at open");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 4);
            setRegU32(ctx, 7, static_cast<uint32_t>(-1));
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.IsTrue(static_cast<uint32_t>(getRegU32(&ctx, 2)) >= 1u, "scePadInfoMode table count should be non-zero");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadInfoPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadInfoPressMode should report pressure support");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 0u);
            setRegU32(ctx, 7, 3u);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should accept digital mode");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1u);
            setRegU32(ctx, 7, 0u);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(4), "CURID should switch to digital mode");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1u);
            setRegU32(ctx, 7, 3u);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should accept analog mode");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 4);
            setRegU32(ctx, 7, 0u);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(7), "mode table entry should return DualShock in analog mode");

            closePadPort(ctx, rdram);
        });

        tc.Run("pads open in digital mode and switch to analog on scePadSetMainMode", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(6), "freshly opened port should report STABLE");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1);
            setRegU32(ctx, 7, 0);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(4), "scePadInfoMode CURID should return digital at open");

            runPadRead(ctx, rdram);
            const uint8_t *data = rdram.data() + kPadDataAddr;
            t.Equals(data[1], static_cast<uint8_t>(0x41), "mode byte should be 0x41 (digital) at open");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1);
            setRegU32(ctx, 7, 3);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should succeed switching to analog");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1);
            setRegU32(ctx, 7, 0);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(7), "scePadInfoMode CURID should return analog after SetMainMode");

            // scePadSetMainMode queues a one-shot EXECCMD transient state; pump scePadGetState
            // once so the port settles back to STABLE before reading, mirroring the existing
            // "pad command state reports EXECCMD once before returning STABLE" test.
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);

            runPadRead(ctx, rdram);
            t.Equals(data[1], static_cast<uint8_t>(0x73), "mode byte should be 0x73 (analog) after SetMainMode");

            closePadPort(ctx, rdram);
        });

        tc.Run("pad setters return success", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetActAlign(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetActAlign should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetActDirect(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetActDirect should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 0xFFFFu);
            ps2_stubs::scePadSetButtonInfo(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetButtonInfo should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1u);
            setRegU32(ctx, 7, 3u);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetReqState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetReqState should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetVrefParam(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetVrefParam should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetWarningLevel(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "scePadSetWarningLevel should return 0");

            ps2_stubs::scePadEnd(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadEnd should succeed");

            openPadPort(ctx, rdram);
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadEnterPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadEnterPressMode should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadExitPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadExitPressMode should succeed");

            closePadPort(ctx, rdram);
        });

        tc.Run("scePadRead fills pressure bytes and honors button info mask", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 0xFFFFu);
            ps2_stubs::scePadSetButtonInfo(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetButtonInfo should accept all buttons");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadEnterPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadEnterPressMode should enable pressure data");

            const uint16_t pressedButtons = static_cast<uint16_t>(0xFFFFu &
                                                                   ~kPadBtnLeft &
                                                                   ~kPadBtnUp &
                                                                   ~kPadBtnTriangle &
                                                                   ~kPadBtnCross &
                                                                   ~kPadBtnL1 &
                                                                   ~kPadBtnR2);
            ps2_stubs::setPadOverrideState(pressedButtons, 0x80, 0x80, 0x80, 0x80);
            runPadRead(ctx, rdram);

            const uint8_t *data = rdram.data() + kPadDataAddr;
            t.Equals(data[8], static_cast<uint8_t>(0x00), "right pressure should be clear when not pressed");
            t.Equals(data[9], static_cast<uint8_t>(0xFF), "left pressure should be populated when pressed");
            t.Equals(data[10], static_cast<uint8_t>(0xFF), "up pressure should be populated when pressed");
            t.Equals(data[11], static_cast<uint8_t>(0x00), "down pressure should be clear when not pressed");
            t.Equals(data[12], static_cast<uint8_t>(0xFF), "triangle pressure should be populated when pressed");
            t.Equals(data[13], static_cast<uint8_t>(0x00), "circle pressure should be clear when not pressed");
            t.Equals(data[14], static_cast<uint8_t>(0xFF), "cross pressure should be populated when pressed");
            t.Equals(data[15], static_cast<uint8_t>(0x00), "square pressure should be clear when not pressed");
            t.Equals(data[16], static_cast<uint8_t>(0xFF), "L1 pressure should be populated when pressed");
            t.Equals(data[17], static_cast<uint8_t>(0x00), "L2 pressure should be clear when not pressed");
            t.Equals(data[18], static_cast<uint8_t>(0x00), "R1 pressure should be clear when not pressed");
            t.Equals(data[19], static_cast<uint8_t>(0xFF), "R2 pressure should be populated when pressed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, static_cast<uint32_t>(kPadBtnL1 | kPadBtnR2));
            ps2_stubs::scePadSetButtonInfo(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetButtonInfo should narrow the enabled pressure mask");

            runPadRead(ctx, rdram);

            t.Equals(data[9], static_cast<uint8_t>(0x00), "masked-out direction pressure should clear");
            t.Equals(data[10], static_cast<uint8_t>(0x00), "masked-out direction pressure should clear");
            t.Equals(data[12], static_cast<uint8_t>(0x00), "masked-out face-button pressure should clear");
            t.Equals(data[14], static_cast<uint8_t>(0x00), "masked-out face-button pressure should clear");
            t.Equals(data[16], static_cast<uint8_t>(0xFF), "enabled L1 pressure should remain populated");
            t.Equals(data[19], static_cast<uint8_t>(0xFF), "enabled R2 pressure should remain populated");

            ps2_stubs::clearPadOverrideState();
            closePadPort(ctx, rdram);
        });

        tc.Run("pad string helpers map state codes", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            setRegU32(ctx, 4, 1);
            setRegU32(ctx, 5, kPadDataAddr);
            ps2_stubs::scePadStateIntToStr(rdram.data(), &ctx, nullptr);
            t.IsTrue(std::string(reinterpret_cast<const char *>(rdram.data() + kPadDataAddr)).find("FINDPAD") != std::string::npos,
                     "state 1 should map to FINDPAD");

            setRegU32(ctx, 4, 0);
            setRegU32(ctx, 5, kPadDataAddr + 64);
            ps2_stubs::scePadStateIntToStr(rdram.data(), &ctx, nullptr);
            t.IsTrue(std::string(reinterpret_cast<const char *>(rdram.data() + kPadDataAddr + 64)).find("DISCONNECTED") != std::string::npos,
                     "state 0 should map to DISCONNECTED");

            setRegU32(ctx, 4, 1);
            setRegU32(ctx, 5, kPadDataAddr + 128);
            ps2_stubs::scePadReqIntToStr(rdram.data(), &ctx, nullptr);
            t.IsTrue(std::string(reinterpret_cast<const char *>(rdram.data() + kPadDataAddr + 128)).find("BUSY") != std::string::npos,
                     "req state 1 should map to BUSY");
        });
        tc.Run("scePadGetFrameCount increments", [](TestCase &t)
               {
            R5900Context ctx;
            ps2_stubs::scePadGetFrameCount(nullptr, &ctx, nullptr);
            const uint32_t first = getRegU32(&ctx, 2);
            ps2_stubs::scePadGetFrameCount(nullptr, &ctx, nullptr);
            const uint32_t second = getRegU32(&ctx, 2);
            t.Equals(second, first + 1, "frame count should increment");
        });

        tc.Run("scePadStateIntToStr and scePadReqIntToStr write strings", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            setRegU32(ctx, 4, 6);
            setRegU32(ctx, 5, kPadDataAddr);
            ps2_stubs::scePadStateIntToStr(rdram.data(), &ctx, nullptr);
            const char *stateStr = reinterpret_cast<const char *>(rdram.data() + kPadDataAddr);
            t.IsTrue(std::string(stateStr).find("STABLE") != std::string::npos, "state string should include STABLE");

            setRegU32(ctx, 4, 0);
            setRegU32(ctx, 5, kPadDataAddr + 64);
            ps2_stubs::scePadReqIntToStr(rdram.data(), &ctx, nullptr);
            const char *reqStr = reinterpret_cast<const char *>(rdram.data() + kPadDataAddr + 64);
            t.IsTrue(std::string(reqStr).find("COMPLETE") != std::string::npos, "req string should include COMPLETE");
        });

        tc.Run("pad script parser accepts buttons, combos and analog", [](TestCase &t)
               {
            std::vector<ps2_stubs::PadScriptEntry> entries;
            t.IsTrue(ps2_stubs::parsePadScript("65000:start:500,70000:up+cross:300,80000:lx=0+ly=255:1000", entries),
                     "well-formed script should parse");
            t.Equals(static_cast<uint32_t>(entries.size()), static_cast<uint32_t>(3), "script should yield three entries");
            t.Equals(static_cast<uint32_t>(entries[0].atMs), static_cast<uint32_t>(65000), "entry 0 atMs");
            t.Equals(static_cast<uint32_t>(entries[0].holdMs), static_cast<uint32_t>(500), "entry 0 holdMs");
            t.Equals(static_cast<uint32_t>(entries[0].pressMask), static_cast<uint32_t>(kPadBtnStart), "entry 0 presses start");
            t.Equals(static_cast<uint32_t>(entries[1].pressMask),
                     static_cast<uint32_t>(static_cast<uint16_t>(kPadBtnUp | kPadBtnCross)),
                     "entry 1 presses up+cross");
            t.IsTrue(entries[2].hasLx && entries[2].hasLy, "entry 2 drives lx and ly");
            t.Equals(entries[2].lx, static_cast<uint8_t>(0), "entry 2 lx");
            t.Equals(entries[2].ly, static_cast<uint8_t>(255), "entry 2 ly");
            t.IsTrue(!entries[2].hasRx && !entries[2].hasRy, "entry 2 leaves rx/ry alone");

            const char *bad[] = {
                nullptr, "", "65000:start", "65000:start:0", "abc:start:500",
                "65000:nosuchbutton:500", "65000::500",
                "65000:start::500", "65000:lx=256:500", "65000:lz=1:500",
                "65000:start+:500", "65000:start:500x",
                // RP2/GQ1: a trailing comma is tolerated (tested below), but
                // a non-trailing empty item is still malformed.
                "65000:start:500,,70000:cross:100", ",",
            };
            for (const char *spec : bad)
            {
                std::vector<ps2_stubs::PadScriptEntry> rejected;
                t.IsTrue(!ps2_stubs::parsePadScript(spec, rejected), "malformed script should be rejected");
            }

            // RP2/GQ1: an empty trailing item (one trailing comma) parses.
            std::vector<ps2_stubs::PadScriptEntry> trailed;
            t.IsTrue(ps2_stubs::parsePadScript("65000:start:500,", trailed),
                     "trailing comma should be tolerated");
            t.Equals(static_cast<uint32_t>(trailed.size()), static_cast<uint32_t>(1),
                     "trailed script yields one entry");
        });

        tc.Run("pad script presses and releases on schedule", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            t.IsTrue(ps2_stubs::setPadScriptForTest("1000:start+cross:500,1200:lx=0:800"), "test script should install");

            ps2_stubs::setPadScriptNowMsForTest(999);
            runPadRead(ctx, rdram);
            t.Equals(readButtons(rdram), static_cast<uint16_t>(0xFFFFu), "no buttons pressed before the window");

            ps2_stubs::setPadScriptNowMsForTest(1000);
            runPadRead(ctx, rdram);
            t.Equals(readButtons(rdram),
                     static_cast<uint16_t>(0xFFFFu & ~kPadBtnStart & ~kPadBtnCross),
                     "start+cross pressed inside the window");

            ps2_stubs::setPadScriptNowMsForTest(1300);
            runPadRead(ctx, rdram);
            const uint8_t *data = rdram.data() + kPadDataAddr;
            t.Equals(readButtons(rdram),
                     static_cast<uint16_t>(0xFFFFu & ~kPadBtnStart & ~kPadBtnCross),
                     "buttons still pressed in the overlap");
            t.Equals(data[6], static_cast<uint8_t>(0), "lx driven in the overlap");
            t.Equals(data[7], static_cast<uint8_t>(0x80), "ly untouched in the overlap");

            ps2_stubs::setPadScriptNowMsForTest(1500);
            runPadRead(ctx, rdram);
            t.Equals(readButtons(rdram), static_cast<uint16_t>(0xFFFFu), "buttons released at the window end");
            t.Equals(data[6], static_cast<uint8_t>(0), "lx still driven after the button window");

            ps2_stubs::setPadScriptNowMsForTest(2000);
            runPadRead(ctx, rdram);
            t.Equals(readButtons(rdram), static_cast<uint16_t>(0xFFFFu), "buttons released after the script");
            t.Equals(data[6], static_cast<uint8_t>(0x80), "lx centered after the script");

            ps2_stubs::clearPadScriptForTest();
            closePadPort(ctx, rdram);
        });

        tc.Run("pad script vsync clock counts guest vsyncs not wall ms", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            // Tick 10 = 166 guest ms, tick 11 = 183 guest ms
            // (tick * 100000 / 5994). The window [167, 184) covers tick 11 only.
            t.IsTrue(ps2_stubs::setPadScriptForTest("167:start:17"), "test script should install");
            ps2_stubs::setPadScriptVsyncClockForTest(true);

            ps2_stubs::setPadScriptVsyncTickForTest(10);
            runPadRead(ctx, rdram);
            t.Equals(readButtons(rdram), static_cast<uint16_t>(0xFFFFu), "tick 10 (166ms) is before the window");

            ps2_stubs::setPadScriptVsyncTickForTest(11);
            runPadRead(ctx, rdram);
            t.Equals(readButtons(rdram),
                     static_cast<uint16_t>(0xFFFFu & ~kPadBtnStart),
                     "tick 11 (183ms) is inside the window");

            ps2_stubs::setPadScriptVsyncTickForTest(12);
            runPadRead(ctx, rdram);
            t.Equals(readButtons(rdram), static_cast<uint16_t>(0xFFFFu), "tick 12 (200ms) is past the window");

            ps2_stubs::clearPadScriptForTest();
            closePadPort(ctx, rdram);
        });

        tc.Run("pad script is off by default", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            runPadRead(ctx, rdram);
            t.Equals(readButtons(rdram), static_cast<uint16_t>(0xFFFFu), "reads are unmodified with no script");
            const uint8_t *data = rdram.data() + kPadDataAddr;
            t.Equals(data[6], static_cast<uint8_t>(0x80), "lx centered with no script");

            closePadPort(ctx, rdram);
        });

        tc.Run("pad recorder emits a replayable script", [](TestCase &t)
               {
            const std::string recPath =
                (std::filesystem::temp_directory_path() / "ir1_padrec_test.txt").string();
            std::remove(recPath.c_str());

            // RB2: the header must record the reverse-DMA knob so lagV
            // recordings replay with lagV. Save/restore the ambient value.
            const char *savedRevdma = std::getenv("PS2X_VIF1_REVERSE_DMA");
            const std::string savedRevdmaCopy = savedRevdma ? savedRevdma : "";
            const bool hadRevdma = savedRevdma != nullptr;
            ::setenv("PS2X_VIF1_REVERSE_DMA", "lagV", 1);

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            t.IsTrue(ps2_stubs::setPadRecordForTest(recPath.c_str()), "recorder should arm on a temp path");

            struct Step
            {
                uint64_t tick;
                uint16_t buttons;
                uint8_t lx, ly, rx, ry;
            };
            const uint16_t relaxed = 0xFFFFu;
            const uint16_t cross = static_cast<uint16_t>(0xFFFFu & ~kPadBtnCross);
            const uint16_t crossDown =
                static_cast<uint16_t>(0xFFFFu & ~kPadBtnCross & ~kPadBtnDown);
            // One read per tick; the recorder keys by tick.
            const Step steps[] = {
                {100, relaxed, 0x80, 0x80, 0x80, 0x80},
                {110, cross, 0x00, 0x80, 0x80, 0x80},
                {125, crossDown, 0x00, 0xC8, 0x80, 0x80},
                {140, relaxed, 0x80, 0x80, 0x80, 0x80},
            };
            for (const Step &s : steps)
            {
                ps2_stubs::setPadOverrideState(s.buttons, s.lx, s.ly, s.rx, s.ry);
                ps2_stubs::setPadRecordTickForTest(s.tick);
                runPadRead(ctx, rdram);
            }
            ps2_stubs::clearPadOverrideState();
            ps2_stubs::closePadRecordForTest();

            // The recording must parse as a pad script.
            std::FILE *f = std::fopen(recPath.c_str(), "rb");
            t.IsTrue(f != nullptr, "recording file should exist");
            std::string content;
            if (f)
            {
                char buf[256];
                size_t n = 0;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                {
                    content.append(buf, n);
                }
                std::fclose(f);
            }
            t.IsTrue(content.rfind("# padrec v1\n", 0) == 0,
                     "recording starts with a header block");
            t.IsTrue(content.find("\n# knobs ") != std::string::npos,
                     "recording header carries knobs");
            t.IsTrue(content.find("VIF1_REVERSE_DMA=lagV") != std::string::npos,
                     "recording header carries the reverse-DMA knob");
            t.IsTrue(content.find("\n# mcroot=") != std::string::npos,
                     "recording header carries save hash");
            std::vector<ps2_stubs::PadScriptEntry> entries;
            t.IsTrue(ps2_stubs::parsePadScript(content.c_str(), entries),
                     "recording should parse as a pad script");
            t.Equals(static_cast<uint32_t>(entries.size()), static_cast<uint32_t>(4),
                     "one entry per state span (incl. tail)");

            // Replay the recording on the vsync clock: every recorded tick
            // must reproduce the recorded buttons and exact analog bytes.
            t.IsTrue(ps2_stubs::setPadScriptForTest(content.c_str()),
                     "recording should install as a script");
            ps2_stubs::setPadScriptVsyncClockForTest(true);
            for (uint64_t tick = 100; tick <= 140; ++tick)
            {
                const Step *want = &steps[0];
                for (const Step &s : steps)
                {
                    if (s.tick <= tick)
                    {
                        want = &s;
                    }
                }
                ps2_stubs::setPadScriptVsyncTickForTest(tick);
                runPadRead(ctx, rdram);
                const uint8_t *data = rdram.data() + kPadDataAddr;
                t.Equals(readButtons(rdram), want->buttons,
                         "replay buttons at tick " + std::to_string(tick));
                t.Equals(data[6], want->lx, "replay lx at tick " + std::to_string(tick));
                t.Equals(data[7], want->ly, "replay ly at tick " + std::to_string(tick));
                t.Equals(data[4], want->rx, "replay rx at tick " + std::to_string(tick));
                t.Equals(data[5], want->ry, "replay ry at tick " + std::to_string(tick));
            }

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            closePadPort(ctx, rdram);
            std::remove(recPath.c_str());
            if (hadRevdma)
            {
                ::setenv("PS2X_VIF1_REVERSE_DMA", savedRevdmaCopy.c_str(), 1);
            }
            else
            {
                ::unsetenv("PS2X_VIF1_REVERSE_DMA");
            }
        });

        tc.Run("pad script parser ignores comment and blank lines", [](TestCase &t)
               {
            std::vector<ps2_stubs::PadScriptEntry> entries;
            t.IsTrue(ps2_stubs::parsePadScript(
                         "# padrec v1\n# start_utc=2026-09-28T01:00:00Z\n\n1000:start:500\n",
                         entries),
                     "header + entries should parse");
            t.Equals(static_cast<uint32_t>(entries.size()), static_cast<uint32_t>(1),
                     "one entry under the header");
            t.Equals(static_cast<uint32_t>(entries[0].pressMask),
                     static_cast<uint32_t>(kPadBtnStart), "entry presses start");
            t.IsTrue(ps2_stubs::parsePadScript("# h\r\n1000:start:500\r\n", entries),
                     "CRLF line endings should parse");

            std::vector<ps2_stubs::PadScriptEntry> rejected;
            t.IsTrue(!ps2_stubs::parsePadScript("1000:st#art:500", rejected),
                     "mid-entry # is still malformed");
            t.IsTrue(!ps2_stubs::parsePadScript("# only a comment\n", rejected),
                     "header-only spec has no entries");
        });

        tc.Run("pad script file loads the same entries as inline", [](TestCase &t)
               {
            const std::string filePath =
                (std::filesystem::temp_directory_path() / "rp2_padscript_test.txt").string();
            std::remove(filePath.c_str());
            {
                std::FILE *f = std::fopen(filePath.c_str(), "w");
                t.IsTrue(f != nullptr, "script file should be writable");
                if (f)
                {
                    std::fputs("1000:start+cross:500,1200:lx=0:800\n", f);
                    std::fclose(f);
                }
            }

            std::vector<ps2_stubs::PadScriptEntry> fromFile;
            t.IsTrue(ps2_stubs::parsePadScriptFile(filePath.c_str(), fromFile),
                     "script file should parse");
            std::vector<ps2_stubs::PadScriptEntry> fromInline;
            t.IsTrue(ps2_stubs::parsePadScript("1000:start+cross:500,1200:lx=0:800", fromInline),
                     "inline spec should parse");
            t.Equals(static_cast<uint32_t>(fromFile.size()), static_cast<uint32_t>(2),
                     "file holds two entries");
            t.Equals(static_cast<uint32_t>(fromFile.size()),
                     static_cast<uint32_t>(fromInline.size()),
                     "file and inline parse to the same count");
            if (fromFile.size() == 2 && fromInline.size() == 2)
            {
                t.Equals(static_cast<uint32_t>(fromFile[0].pressMask),
                         static_cast<uint32_t>(fromInline[0].pressMask),
                         "entry 0 press mask matches inline");
                t.Equals(static_cast<uint32_t>(fromFile[1].lx),
                         static_cast<uint32_t>(fromInline[1].lx),
                         "entry 1 lx matches inline");
            }

            // The file form also arms pad reads.
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);
            t.IsTrue(ps2_stubs::setPadScriptFromFileForTest(filePath.c_str()),
                     "script file should install");
            ps2_stubs::setPadScriptNowMsForTest(1000);
            runPadRead(ctx, rdram);
            t.Equals(readButtons(rdram),
                     static_cast<uint16_t>(0xFFFFu & ~kPadBtnStart & ~kPadBtnCross),
                     "file script presses inside the window");
            ps2_stubs::clearPadScriptForTest();
            closePadPort(ctx, rdram);
            std::remove(filePath.c_str());
        });

        tc.Run("pad script file skips header and blank lines", [](TestCase &t)
               {
            const std::string filePath =
                (std::filesystem::temp_directory_path() / "rp2_padscript_hdr_test.txt").string();
            std::remove(filePath.c_str());
            {
                std::FILE *f = std::fopen(filePath.c_str(), "w");
                t.IsTrue(f != nullptr, "script file should be writable");
                if (f)
                {
                    std::fputs("# padrec v1\n# start_utc=2026-09-28T01:00:00Z\n"
                               "# knobs SIM_MODE=split120_render60_v1\n\n"
                               "1000:start:500\n",
                               f);
                    std::fclose(f);
                }
            }
            std::vector<ps2_stubs::PadScriptEntry> entries;
            t.IsTrue(ps2_stubs::parsePadScriptFile(filePath.c_str(), entries),
                     "header + entries should parse from a file");
            t.Equals(static_cast<uint32_t>(entries.size()), static_cast<uint32_t>(1),
                     "one entry under the header");
            if (!entries.empty())
            {
                t.Equals(static_cast<uint32_t>(entries[0].pressMask),
                         static_cast<uint32_t>(kPadBtnStart), "entry presses start");
            }
            std::remove(filePath.c_str());
        });

        tc.Run("pad script file missing is a clean failure", [](TestCase &t)
               {
            const std::string missing =
                (std::filesystem::temp_directory_path() / "rp2_padscript_nope_test.txt").string();
            std::remove(missing.c_str());
            std::vector<ps2_stubs::PadScriptEntry> entries;
            t.IsTrue(!ps2_stubs::parsePadScriptFile(missing.c_str(), entries),
                     "missing file should not parse");
            t.IsTrue(!ps2_stubs::parsePadScriptFile("", entries),
                     "empty path should not parse");
            t.IsTrue(!ps2_stubs::parsePadScriptFile(nullptr, entries),
                     "null path should not parse");
            t.IsTrue(!ps2_stubs::setPadScriptFromFileForTest(missing.c_str()),
                     "missing file should not install");

            // The failed install arms nothing.
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);
            ps2_stubs::setPadScriptFromFileForTest(missing.c_str());
            runPadRead(ctx, rdram);
            t.Equals(readButtons(rdram), static_cast<uint16_t>(0xFFFFu),
                     "reads are unmodified after a failed file install");
            ps2_stubs::clearPadScriptForTest();
            closePadPort(ctx, rdram);
        });

        tc.Run("pad recorder dir mode writes one session file and prunes", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            // PL1: hermetic card env (an ambient PS2X_MC_ROOT would snapshot
            // into this dir; the snapshot path has its own tests below).
            Pl1EnvGuard mcRootGuard("PS2X_MC_ROOT");
            ::unsetenv("PS2X_MC_ROOT");
            const fs::path dir = fs::temp_directory_path() / "ir1_padrec_dir_test";
            fs::remove_all(dir);
            fs::create_directories(dir);
            // Five stale session files + one non-matching file (must survive).
            for (int i = 0; i < 5; ++i)
            {
                char name[64];
                std::snprintf(name, sizeof(name), "padrec-20200101-00000%d.txt", i);
                std::FILE *f = std::fopen((dir / name).string().c_str(), "w");
                std::fputs("stale", f);
                std::fclose(f);
            }
            {
                std::FILE *f = std::fopen((dir / "keep.txt").string().c_str(), "w");
                std::fputs("x", f);
                std::fclose(f);
            }

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            t.IsTrue(ps2_stubs::setPadRecordDirForTest(dir.string().c_str(), 3),
                     "dir mode should arm");
            ps2_stubs::setPadOverrideState(0xFFFFu, 0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(100);
            runPadRead(ctx, rdram);
            ps2_stubs::setPadOverrideState(static_cast<uint16_t>(0xFFFFu & ~kPadBtnCross),
                                           0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(110);
            runPadRead(ctx, rdram);
            ps2_stubs::clearPadOverrideState();
            ps2_stubs::closePadRecordForTest();

            // Prune ran at arm (6 files, keep 3): 3 oldest stale gone.
            t.IsTrue(!fs::exists(dir / "padrec-20200101-000000.txt"), "oldest pruned");
            t.IsTrue(!fs::exists(dir / "padrec-20200101-000001.txt"), "second-oldest pruned");
            t.IsTrue(!fs::exists(dir / "padrec-20200101-000002.txt"), "third-oldest pruned");
            t.IsTrue(fs::exists(dir / "padrec-20200101-000003.txt"), "newer stale kept");
            t.IsTrue(fs::exists(dir / "padrec-20200101-000004.txt"), "newest stale kept");
            t.IsTrue(fs::exists(dir / "keep.txt"), "non-matching file kept");
            std::vector<fs::path> fresh;
            for (const auto &de : fs::directory_iterator(dir))
            {
                const std::string n = de.path().filename().string();
                // PL1: the session file is the .txt; a .mc sibling (when a
                // card root resolves) is not a session.
                if (n.size() > 11 && n.compare(0, 7, "padrec-") == 0 &&
                    n.compare(n.size() - 4, 4, ".txt") == 0 &&
                    n.find("20200101") == std::string::npos)
                {
                    fresh.push_back(de.path());
                }
            }
            t.Equals(static_cast<uint32_t>(fresh.size()), static_cast<uint32_t>(1),
                     "one session file");
            if (!fresh.empty())
            {
                std::FILE *f = std::fopen(fresh[0].string().c_str(), "rb");
                std::string content;
                if (f)
                {
                    char buf[256];
                    size_t n = 0;
                    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                    {
                        content.append(buf, n);
                    }
                    std::fclose(f);
                }
                t.IsTrue(content.rfind("# padrec v1\n", 0) == 0, "session file has header");
                std::vector<ps2_stubs::PadScriptEntry> entries;
                t.IsTrue(ps2_stubs::parsePadScript(content.c_str(), entries),
                         "session file parses as a script");
                t.Equals(static_cast<uint32_t>(entries.size()), static_cast<uint32_t>(2),
                         "one change + tail");
            }

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            closePadPort(ctx, rdram);
            fs::remove_all(dir);
        });

        tc.Run("pad recorder header carries build and env_sha", [](TestCase &t)
               {
            const std::string recPath =
                (std::filesystem::temp_directory_path() / "pl1_padrec_hdr_test.txt").string();
            std::remove(recPath.c_str());

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            ps2x::setRecordedEnvFileHash("0123456789abcdef");
            t.IsTrue(ps2_stubs::setPadRecordForTest(recPath.c_str()), "file mode should arm");
            ps2_stubs::setPadOverrideState(0xFFFFu, 0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(100);
            runPadRead(ctx, rdram);
            ps2_stubs::clearPadOverrideState();
            ps2_stubs::closePadRecordForTest();
            ps2x::setRecordedEnvFileHash("");

            const std::string content = readFileBytes(recPath);
            t.IsTrue(content.find("# build=" + std::string(ps2x::buildId()) + "\n") != std::string::npos,
                     "header carries the build id");
            t.IsTrue(content.find("# env_sha=0123456789abcdef\n") != std::string::npos,
                     "header carries the stashed env hash");
            t.IsTrue(content.find("# mcsnap=none ") != std::string::npos,
                     "file mode takes no snapshot");
            std::vector<ps2_stubs::PadScriptEntry> entries;
            t.IsTrue(ps2_stubs::parsePadScript(content.c_str(), entries),
                     "recording still parses as a script");
            t.Equals(static_cast<uint32_t>(entries.size()), static_cast<uint32_t>(1),
                     "single span + tail");

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            closePadPort(ctx, rdram);
            std::remove(recPath.c_str());
        });

        tc.Run("pad recorder dir mode snapshots the memory card", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            Pl1EnvGuard mcRootGuard("PS2X_MC_ROOT");
            const fs::path card = fs::temp_directory_path() / "pl1_padrec_card_test";
            fs::remove_all(card);
            fs::create_directories(card / "BASLUS-20772");
            {
                std::FILE *f = std::fopen((card / "BASLUS-20772" / "save.bin").string().c_str(), "wb");
                std::fputs("card-bytes-1", f);
                std::fclose(f);
                f = std::fopen((card / "top.dat").string().c_str(), "wb");
                std::fputs("top-bytes", f);
                std::fclose(f);
            }
            ::setenv("PS2X_MC_ROOT", card.string().c_str(), 1);
            const fs::path dir = fs::temp_directory_path() / "pl1_padrec_snap_test";
            fs::remove_all(dir);
            fs::create_directories(dir);

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            t.IsTrue(ps2_stubs::setPadRecordDirForTest(dir.string().c_str(), 30),
                     "dir mode should arm");
            ps2_stubs::setPadOverrideState(0xFFFFu, 0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(100);
            runPadRead(ctx, rdram);
            ps2_stubs::clearPadOverrideState();
            ps2_stubs::closePadRecordForTest();

            std::vector<fs::path> snaps, sessions;
            for (const auto &de : fs::directory_iterator(dir))
            {
                const std::string n = de.path().filename().string();
                if (n.size() > 7 && n.compare(0, 7, "padrec-") == 0)
                {
                    if (n.compare(n.size() - 4, 4, ".txt") == 0)
                        sessions.push_back(de.path());
                    else if (n.compare(n.size() - 3, 3, ".mc") == 0 && de.is_directory())
                        snaps.push_back(de.path());
                }
            }
            t.Equals(static_cast<uint32_t>(sessions.size()), static_cast<uint32_t>(1),
                     "one session file");
            t.Equals(static_cast<uint32_t>(snaps.size()), static_cast<uint32_t>(1),
                     "one snapshot sibling");
            if (!snaps.empty())
            {
                t.IsTrue(readFileBytes(snaps[0] / "BASLUS-20772" / "save.bin") == "card-bytes-1",
                         "snapshot copies nested saves");
                t.IsTrue(readFileBytes(snaps[0] / "top.dat") == "top-bytes",
                         "snapshot copies top-level files");
            }
            if (!sessions.empty() && !snaps.empty())
            {
                const std::string content = readFileBytes(sessions[0]);
                const std::string leaf = snaps[0].filename().string();
                t.IsTrue(content.find("# mcsnap=" + leaf + " ") != std::string::npos,
                         "header names the snapshot dir");
                t.IsTrue(content.find(" mcsrc=" + card.string() + " ") != std::string::npos,
                         "header names the card root");
                char wantSha[32];
                std::snprintf(wantSha, sizeof(wantSha), "%016llx",
                              static_cast<unsigned long long>(pl1SaveSetHash(card)));
                t.IsTrue(content.find(" mcsnap_sha=" + std::string(wantSha) + " ") != std::string::npos,
                         "header hash matches the card bytes");
                t.IsTrue(content.find(" mcsnap_files=2\n") != std::string::npos,
                         "header counts the snapshot files");
                t.Equals(pl1SaveSetHash(snaps[0]), pl1SaveSetHash(card),
                         "snapshot re-hashes to the same value (standalone)");
            }

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            closePadPort(ctx, rdram);
            fs::remove_all(dir);
            fs::remove_all(card);
        });

        tc.Run("pad recorder dir mode prunes snapshots with recordings", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            Pl1EnvGuard mcRootGuard("PS2X_MC_ROOT");
            const fs::path card = fs::temp_directory_path() / "pl1_padrec_emptycard_test";
            fs::remove_all(card);
            fs::create_directories(card); // empty: the live session snapshots 0 files
            ::setenv("PS2X_MC_ROOT", card.string().c_str(), 1);
            const fs::path dir = fs::temp_directory_path() / "pl1_padrec_prune_test";
            fs::remove_all(dir);
            fs::create_directories(dir);
            const char *stale[] = {
                "padrec-20200101-000000.txt", "padrec-20200101-000001.txt", "padrec-20200101-000002.txt"};
            for (const char *name : stale)
            {
                std::FILE *f = std::fopen((dir / name).string().c_str(), "w");
                std::fputs("stale", f);
                std::fclose(f);
            }
            // Sibling snapshots for the two oldest; the newest stale has
            // none (missing siblings prune cleanly).
            for (int i = 0; i < 2; ++i)
            {
                const fs::path snap =
                    dir / (std::string(stale[i]).substr(0, std::string(stale[i]).size() - 4) + ".mc");
                fs::create_directories(snap);
                std::FILE *f = std::fopen((snap / "save.bin").string().c_str(), "w");
                std::fputs("snap", f);
                std::fclose(f);
            }
            // An orphan snapshot (no recording) and a non-matching file.
            fs::create_directories(dir / "padrec-19990101-000000.mc");
            {
                std::FILE *f = std::fopen((dir / "keep.txt").string().c_str(), "w");
                std::fputs("x", f);
                std::fclose(f);
            }

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            // 3 stale + 1 live, keep 2: the two oldest go with their snaps.
            t.IsTrue(ps2_stubs::setPadRecordDirForTest(dir.string().c_str(), 2),
                     "dir mode should arm");
            ps2_stubs::setPadOverrideState(0xFFFFu, 0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(100);
            runPadRead(ctx, rdram);
            ps2_stubs::clearPadOverrideState();
            ps2_stubs::closePadRecordForTest();

            t.IsTrue(!fs::exists(dir / "padrec-20200101-000000.txt"), "oldest recording pruned");
            t.IsTrue(!fs::exists(dir / "padrec-20200101-000000.mc"), "oldest snapshot pruned");
            t.IsTrue(!fs::exists(dir / "padrec-20200101-000001.txt"), "second recording pruned");
            t.IsTrue(!fs::exists(dir / "padrec-20200101-000001.mc"), "second snapshot pruned");
            t.IsTrue(fs::exists(dir / "padrec-20200101-000002.txt"), "newest stale kept");
            t.IsTrue(fs::exists(dir / "padrec-19990101-000000.mc"), "orphan snapshot kept");
            t.IsTrue(fs::exists(dir / "keep.txt"), "non-matching file kept");
            uint32_t liveTxt = 0u, liveMc = 0u;
            for (const auto &de : fs::directory_iterator(dir))
            {
                const std::string n = de.path().filename().string();
                if (n.size() > 11 && n.compare(0, 7, "padrec-") == 0 &&
                    n.find("20200101") == std::string::npos && n.find("19990101") == std::string::npos)
                {
                    if (n.compare(n.size() - 4, 4, ".txt") == 0)
                        ++liveTxt;
                    else if (n.compare(n.size() - 3, 3, ".mc") == 0)
                        ++liveMc;
                }
            }
            t.Equals(liveTxt, 1u, "live session file kept");
            t.Equals(liveMc, 1u, "live snapshot kept");

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            closePadPort(ctx, rdram);
            fs::remove_all(dir);
            fs::remove_all(card);
        });

        tc.Run("pad recorder skips oversize snapshots with a header note", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            Pl1EnvGuard mcRootGuard("PS2X_MC_ROOT");
            const fs::path card = fs::temp_directory_path() / "pl1_padrec_bigcard_test";
            fs::remove_all(card);
            fs::create_directories(card);
            {
                // 17 MiB over the 16 MiB bound.
                std::FILE *f = std::fopen((card / "big.bin").string().c_str(), "wb");
                const std::string zeros(1u << 20, '\0');
                for (int i = 0; i < 17; ++i)
                {
                    std::fwrite(zeros.data(), 1, zeros.size(), f);
                }
                std::fclose(f);
            }
            ::setenv("PS2X_MC_ROOT", card.string().c_str(), 1);
            const fs::path dir = fs::temp_directory_path() / "pl1_padrec_big_test";
            fs::remove_all(dir);
            fs::create_directories(dir);

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            t.IsTrue(ps2_stubs::setPadRecordDirForTest(dir.string().c_str(), 30),
                     "dir mode should arm");
            ps2_stubs::setPadOverrideState(0xFFFFu, 0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(100);
            runPadRead(ctx, rdram);
            ps2_stubs::clearPadOverrideState();
            ps2_stubs::closePadRecordForTest();

            std::string content;
            uint32_t mcDirs = 0u;
            for (const auto &de : fs::directory_iterator(dir))
            {
                const std::string n = de.path().filename().string();
                if (n.size() > 4 && n.compare(n.size() - 4, 4, ".txt") == 0)
                {
                    content = readFileBytes(de.path());
                }
                else if (n.size() > 3 && n.compare(n.size() - 3, 3, ".mc") == 0)
                {
                    ++mcDirs;
                }
            }
            t.IsTrue(content.find("# mcsnap=too-large ") != std::string::npos,
                     "header notes the skipped snapshot");
            t.Equals(mcDirs, 0u, "no partial snapshot dir kept");

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            closePadPort(ctx, rdram);
            fs::remove_all(dir);
            fs::remove_all(card);
        });

        tc.Run("ds1: a mid-session load starts a new recording segment (file mode)", [](TestCase &t)
               {
            const std::string recPath =
                (std::filesystem::temp_directory_path() / "ds1_padrec_load_test.txt").string();
            std::remove(recPath.c_str());
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);
            t.IsTrue(ps2_stubs::setPadRecordForTest(recPath.c_str()), "recorder arms");

            const uint16_t relaxed = 0xFFFFu;
            const uint16_t cross = static_cast<uint16_t>(0xFFFFu & ~kPadBtnCross);
            ps2_stubs::setPadOverrideState(relaxed, 0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(100);
            runPadRead(ctx, rdram);
            ps2_stubs::setPadOverrideState(cross, 0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(110);
            runPadRead(ctx, rdram);
            // Rewind to tick 105, then keep playing.
            ps2_stubs::padRecordNoteLoad(105);
            ps2_stubs::setPadOverrideState(relaxed, 0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(106);
            runPadRead(ctx, rdram);
            ps2_stubs::setPadOverrideState(cross, 0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(115);
            runPadRead(ctx, rdram);
            ps2_stubs::clearPadOverrideState();
            ps2_stubs::closePadRecordForTest();

            std::FILE *f = std::fopen(recPath.c_str(), "rb");
            std::string content;
            if (f)
            {
                char buf[256];
                size_t n = 0;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                    content.append(buf, n);
                std::fclose(f);
            }
            t.IsTrue(content.find("# --- new segment: state loaded (tick 105)") != std::string::npos,
                     "marker notes the load tick");
            std::vector<ps2_stubs::PadScriptEntry> entries;
            t.IsTrue(ps2_stubs::parsePadScript(content.c_str(), entries), "file still parses");
            // Two pre-load spans + two post-load spans. Without the segment
            // split the rewound reads would be dropped and the tail would
            // carry the stale pre-load state ([110,116) cross: 2 entries).
            t.Equals(static_cast<uint32_t>(entries.size()), static_cast<uint32_t>(4), "two segments, 4 spans");
            // Entries store ms (tick * 100000 / 5994); the rewound span is at tick 106.
            const uint64_t rewoundMs = (106u * 100000ull) / 5994ull;
            bool rewound = false;
            for (const auto &e : entries)
            {
                if (e.atMs == rewoundMs)
                    rewound = true;
            }
            t.IsTrue(rewound, "post-load span starts at the rewound tick");

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            closePadPort(ctx, rdram);
            std::remove(recPath.c_str());
        });

        tc.Run("ds1: a mid-session load opens a new session file (dir mode)", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            const fs::path dir = fs::temp_directory_path() / "ds1_padrec_load_dir_test";
            fs::remove_all(dir);
            fs::create_directories(dir);
            // PL1: hermetic card env + count .txt sessions only (a .mc
            // snapshot sibling is not a session).
            Pl1EnvGuard mcRootGuard("PS2X_MC_ROOT");
            ::unsetenv("PS2X_MC_ROOT");
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);
            t.IsTrue(ps2_stubs::setPadRecordDirForTest(dir.string().c_str(), 30), "dir mode arms");

            const uint16_t relaxed = 0xFFFFu;
            const uint16_t cross = static_cast<uint16_t>(0xFFFFu & ~kPadBtnCross);
            ps2_stubs::setPadOverrideState(relaxed, 0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(100);
            runPadRead(ctx, rdram);
            ps2_stubs::setPadOverrideState(cross, 0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(110);
            runPadRead(ctx, rdram);
            ps2_stubs::padRecordNoteLoad(105);
            ps2_stubs::setPadOverrideState(relaxed, 0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(106);
            runPadRead(ctx, rdram);
            ps2_stubs::setPadOverrideState(cross, 0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(115);
            runPadRead(ctx, rdram);
            ps2_stubs::clearPadOverrideState();
            ps2_stubs::closePadRecordForTest();

            std::vector<fs::path> sessions;
            for (const auto &de : fs::directory_iterator(dir))
            {
                const std::string n = de.path().filename().string();
                if (n.compare(0, 7, "padrec-") == 0 && n.size() > 4 &&
                    n.compare(n.size() - 4, 4, ".txt") == 0)
                    sessions.push_back(de.path());
            }
            t.Equals(static_cast<uint32_t>(sessions.size()), static_cast<uint32_t>(2), "two session files");
            uint32_t noted = 0u;
            for (const fs::path &p : sessions)
            {
                std::FILE *f = std::fopen(p.string().c_str(), "rb");
                std::string content;
                if (f)
                {
                    char buf[256];
                    size_t n = 0;
                    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                        content.append(buf, n);
                    std::fclose(f);
                }
                std::vector<ps2_stubs::PadScriptEntry> entries;
                t.IsTrue(ps2_stubs::parsePadScript(content.c_str(), entries), "session parses");
                t.Equals(static_cast<uint32_t>(entries.size()), static_cast<uint32_t>(2), "one change + tail");
                if (content.find("# load_tick=105") != std::string::npos)
                    ++noted;
            }
            t.Equals(noted, 1u, "exactly the new session header notes the load");

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            closePadPort(ctx, rdram);
            fs::remove_all(dir);
        });

        tc.Run("ds1: quick-save/load chord edges on the thumb while SELECT is held", [](TestCase &t)
        {
            PSChordState st;
            bool save = false, load = false;
            psChordStep(st, false, false, false, save, load);
            t.IsTrue(!save && !load, "idle: no edges");
            psChordStep(st, true, false, false, save, load);
            t.IsTrue(!save && !load, "SELECT alone: no edges");
            psChordStep(st, true, true, false, save, load);
            t.IsTrue(save && !load, "SELECT+L3: save edge");
            psChordStep(st, true, true, false, save, load);
            t.IsTrue(!save && !load, "held chord: no repeat");
            psChordStep(st, true, false, true, save, load);
            t.IsTrue(!save && load, "SELECT+R3: load edge");
            psChordStep(st, false, true, true, save, load);
            t.IsTrue(!save && !load, "SELECT released: no edges");
            psChordStep(st, false, false, false, save, load);
            psChordStep(st, true, true, true, save, load);
            t.IsTrue(save && load, "fresh SELECT+L3+R3: both edges");
        });
    });
}
