#include "MiniTest.h"
#include "ps2_build_id.h"
#include "ps2_record_env.h"
#include "ps2_rpl3_nv.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "runtime/ps2_pad.h"
#include "Stubs/Pad.h"

#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

        tc.Run("tk12 autopilot is inert when unset (APH1)", [](TestCase &t)
               {
            // Guard every knob the harness reads so ambient env cannot arm it.
            Pl1EnvGuard routeGuard("PS2X_TK12_AP_ROUTE");
            Pl1EnvGuard ptrGuard("PS2X_TK12_AP_PTR");
            Pl1EnvGuard fromGuard("PS2X_TK12_AP_FROM");
            Pl1EnvGuard toGuard("PS2X_TK12_AP_TO");
            Pl1EnvGuard logGuard("PS2X_TK12_AP_LOG_EVERY");
            Pl1EnvGuard lookGuard("PS2X_TK12_AP_LOOK");
            Pl1EnvGuard fullGuard("PS2X_TK12_AP_FULL_DEG");
            Pl1EnvGuard dpadGuard("PS2X_TK12_AP_DPAD_DEG");
            ::unsetenv("PS2X_TK12_AP_ROUTE");
            ::unsetenv("PS2X_TK12_AP_PTR");
            ::unsetenv("PS2X_TK12_AP_FROM");
            ::unsetenv("PS2X_TK12_AP_TO");
            ::unsetenv("PS2X_TK12_AP_LOG_EVERY");
            ::unsetenv("PS2X_TK12_AP_LOOK");
            ::unsetenv("PS2X_TK12_AP_FULL_DEG");
            ::unsetenv("PS2X_TK12_AP_DPAD_DEG");
            ps2_stubs::clearTk12ForTest();
            t.IsTrue(!ps2_stubs::tk12ArmedForTest(), "no route env means the autopilot stays off");

            // Pad reads are unmodified with the harness unset.
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);
            runPadRead(ctx, rdram);
            t.Equals(readButtons(rdram), static_cast<uint16_t>(0xFFFFu), "reads are unmodified with no route");
            const uint8_t *data = rdram.data() + kPadDataAddr;
            t.Equals(data[6], static_cast<uint8_t>(0x80), "lx centered with no route");

            // Positive control: a two-point route file arms the harness.
            const std::string routePath =
                (std::filesystem::temp_directory_path() / "aph1_tk12_route_test.txt").string();
            std::remove(routePath.c_str());
            {
                std::FILE *f = std::fopen(routePath.c_str(), "w");
                t.IsTrue(f != nullptr, "route file should be writable");
                if (f)
                {
                    std::fputs("0 0\n1000 0\n", f);
                    std::fclose(f);
                }
            }
            ::setenv("PS2X_TK12_AP_ROUTE", routePath.c_str(), 1);
            ps2_stubs::clearTk12ForTest();
            t.IsTrue(ps2_stubs::tk12ArmedForTest(), "a route file arms the autopilot");
            ps2_stubs::clearTk12ForTest();
            std::remove(routePath.c_str());
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
            // RPL3: new recordings are v3 (v2 stamps plus NV lines); v2 files
            // still parse (covered below).
            t.IsTrue(content.rfind("# padrec v3\n", 0) == 0,
                     "recording starts with a header block (v3: exact stock-time stamps)");
            t.IsTrue(content.find("\n# full120 FULL120=") != std::string::npos &&
                         content.find("events_clock=exact") != std::string::npos,
                     "recording header carries the full120 knobs and its stamp clock");
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
                               "# knobs VU1_ENGINE=microvu\n\n"
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

        tc.Run("pad script events clock follows the recording header (FH17)", [](TestCase &t)
               {
            const std::string filePath =
                (std::filesystem::temp_directory_path() / "fh17_padscript_clock_test.txt").string();
            struct Case
            {
                const char *text;
                bool legacy;
                const char *what;
            };
            // Stamps at odd stock half-ticks (h*100000/11988 ms, h odd) come
            // only from 120 Hz reads: 8 = h 1, 25 = h 3, 41 = h 5, ...
            const std::string oddRows =
                "8:cross:8,25:cross:8,41:cross:8,58:cross:8,75:cross:8,91:cross:8,108:cross:8,125:cross:8";
            const std::string evenRows = "150:cross:16,166:cross:16,183:cross:16,200:cross:16,216:cross:16";
            const std::string v1 = "# padrec v1\n# start_utc=2026-09-30T18:29:55Z\n";
            const std::string c1 = v1 + oddRows + "," + evenRows + "\n";
            const std::string c2 = "# padrec v1\r\n" + oddRows + "\r\n";
            const std::string c3 = v1 + evenRows + "\n";
            const std::string c4 = "# padrec v2\n" + oddRows + "\n";
            const std::string c5 = oddRows + "\n";
            const std::string c6 = "# note\n# padrec v1\n" + oddRows + "\n";
            const std::string c7 = v1 + "8:cross:8,25:cross:8,41:cross:8," + evenRows + "\n";
            const Case cases[] = {
                {c1.c_str(), true, "v1 events recording (odd half-tick rows) keeps the FH5 clock"},
                {c2.c_str(), true, "v1 events recording with CRLF keeps the FH5 clock"},
                {c3.c_str(), false, "v1 stock recording (even half-ticks only) reads the exact clock"},
                {c4.c_str(), false, "v2 recording reads the exact clock"},
                {c5.c_str(), false, "a hand script reads the exact clock"},
                {c6.c_str(), false, "only a first-line v1 header selects the FH5 clock"},
                {c7.c_str(), false, "fewer than 8 odd rows reads the exact clock"},
            };
            for (const Case &c : cases)
            {
                std::remove(filePath.c_str());
                std::FILE *f = std::fopen(filePath.c_str(), "wb");
                t.IsTrue(f != nullptr, "script file should be writable");
                if (!f)
                    continue;
                std::fputs(c.text, f);
                std::fclose(f);
                t.IsTrue(ps2_stubs::setPadScriptFromFileForTest(filePath.c_str()), "script installs");
                t.Equals(ps2_stubs::padScriptLegacyEventsClockForTest(), c.legacy, c.what);
                ps2_stubs::clearPadScriptForTest();
            }
            t.IsTrue(!ps2_stubs::padScriptLegacyEventsClockForTest(), "clear resets the clock choice");
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
                // RPL3: new recordings are v3.
                t.IsTrue(content.rfind("# padrec v3\n", 0) == 0, "session file has header");
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

        tc.Run("lim1: the L3+R3 chord writes a TSV window and stamps the padrec", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            const fs::path dir = fs::temp_directory_path() / "lim1_marker_chord_test";
            fs::remove_all(dir);
            const std::string recPath = (fs::temp_directory_path() / "lim1_padrec_test.txt").string();
            std::remove(recPath.c_str());
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_stubs::clearPadMarkerForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);
            t.IsTrue(ps2_stubs::setPadMarkerDirForTest(dir.string().c_str()), "marker should arm");
            t.IsTrue(ps2_stubs::setPadRecordForTest(recPath.c_str()), "recorder should arm");
            ps2_stubs::setPadMarkerInput2ForTest(0x00200000u, 2u);
            ps2_stubs::setPadMarkerRdramForTest(rdram.data());
            for (uint32_t i = 0u; i < 2u; ++i)
            {
                // Input2Record words: raw, press, release, level, repeat, countdown, guard.
                // (Bank at 2 MB: the pad buffer lives at 0x100000 and would clobber it.)
                const uint32_t words[7] = {0x3F800000u, i, 0u, 1u, 0u, 48u, 0x40000000u};
                std::memcpy(&rdram[0x00200000u + 4u + i * 28u], words, sizeof(words));
            }
            ps2_stubs::setPadMarkerTrackerForTest(0x00c00100u, 0xdeadbeefu, 2u, 1u, 0x40c90fdcu);
            ps2_stubs::setPadMarkerGateForTest(0x0badf00du, 0x42u);

            const uint16_t relaxed = 0xFFFFu;
            const uint16_t chord =
                static_cast<uint16_t>(0xFFFFu & ~kPadBtnL3 & ~kPadBtnR3);
            const uint64_t trigTick = 1000u;
            const uint64_t trigMs = (trigTick * 100000ull) / 5994ull;
            // One read per tick across the whole 10 s window plus the flush read.
            for (uint64_t tick = 400u; tick <= 1130u; ++tick)
            {
                const uint16_t buttons = (tick == trigTick) ? chord : relaxed;
                ps2_stubs::setPadOverrideState(buttons, 0x80, 0x80, 0x80, 0x80);
                ps2_stubs::setPadRecordTickForTest(tick);
                ps2_stubs::setPadMarkerTickForTest(tick);
                runPadRead(ctx, rdram);
                if (tick == trigTick)
                {
                    t.Equals(readButtons(rdram), chord, "the chord passes through to the guest");
                }
            }
            ps2_stubs::clearPadOverrideState();
            ps2_stubs::closePadRecordForTest();
            ps2_stubs::closePadMarkerForTest();

            std::vector<fs::path> files;
            for (const auto &de : fs::directory_iterator(dir))
            {
                const std::string n = de.path().filename().string();
                if (n.compare(0, 7, "marker-") == 0 && n.size() > 4 &&
                    n.compare(n.size() - 4, 4, ".tsv") == 0)
                    files.push_back(de.path());
            }
            t.Equals(static_cast<uint32_t>(files.size()), static_cast<uint32_t>(1), "one marker file");
            std::string content;
            if (!files.empty())
            {
                std::FILE *f = std::fopen(files[0].string().c_str(), "rb");
                if (f)
                {
                    char buf[4096];
                    size_t n = 0;
                    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                        content.append(buf, n);
                    std::fclose(f);
                }
            }
            t.IsTrue(content.find("trigger_ms=" + std::to_string(trigMs)) != std::string::npos,
                     "header carries the trigger ms");
            t.IsTrue(content.find("FULL120_FIX=") != std::string::npos, "header carries the FIX echo");
            // Data rows: the driven ticks whose ms falls in [trigMs-8000, trigMs+2000].
            uint32_t wantRows = 0u;
            for (uint64_t tick = 400u; tick <= 1130u; ++tick)
            {
                const uint64_t ms = (tick * 100000ull) / 5994ull;
                if (ms + 8000u >= trigMs && ms <= trigMs + 2000u)
                    ++wantRows;
            }
            uint32_t dataRows = 0u;
            size_t pos = 0u;
            bool sawHeader = false;
            while (pos < content.size())
            {
                const size_t end = content.find('\n', pos);
                const std::string line = content.substr(pos, end == std::string::npos ? end : end - pos);
                if (!line.empty() && line[0] != '#')
                {
                    if (!sawHeader)
                        sawHeader = line.compare(0, 4, "tick") == 0;
                    else
                        ++dataRows;
                }
                if (end == std::string::npos)
                    break;
                pos = end + 1u;
            }
            t.IsTrue(sawHeader, "file has a column header row");
            t.Equals(dataRows, wantRows, "window rows only");
            t.IsTrue(wantRows > 100u, "the 10 s window holds >100 rows at 1 read/tick");
            t.IsTrue(content.find("\tfff9\t") != std::string::npos, "a row shows the chord word");
            t.IsTrue(content.find("00c00100") != std::string::npos, "tracker S column");
            t.IsTrue(content.find("deadbeef") != std::string::npos, "tracker in0 column");
            t.IsTrue(content.find("\t1\t2\t3\t4\t4.5\t5\t") != std::string::npos,
                     "position/velocity float columns");
            t.IsTrue(content.find("0:3f800000,0,0,1,0,30,40000000;1:3f800000,1,0,1,0,30,40000000") !=
                         std::string::npos,
                     "input2 bank column");
            t.IsTrue(content.find("0badf00d") != std::string::npos, "gate a0 column");
            t.IsTrue(content.find("00000042\t00000099\t0\n") != std::string::npos,
                     "gate v0/v1/others columns");
            {
                float tf = 0.0f;
                const uint32_t tb = 0x40c90fdcu;
                std::memcpy(&tf, &tb, 4u);
                char want[32];
                std::snprintf(want, sizeof(want), "%.9g", static_cast<double>(tf));
                t.IsTrue(content.find(want) != std::string::npos, "target float column");
            }
            std::string rec;
            {
                std::FILE *f = std::fopen(recPath.c_str(), "rb");
                if (f)
                {
                    char buf[256];
                    size_t n = 0;
                    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                        rec.append(buf, n);
                    std::fclose(f);
                }
            }
            t.IsTrue(rec.find("# MARK " + std::to_string(trigMs)) != std::string::npos,
                     "the padrec carries the MARK stamp");
            std::vector<ps2_stubs::PadScriptEntry> entries;
            t.IsTrue(ps2_stubs::parsePadScript(rec.c_str(), entries), "stamped padrec still parses");

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_stubs::clearPadMarkerForTest();
            closePadPort(ctx, rdram);
            fs::remove_all(dir);
            std::remove(recPath.c_str());
        });

        tc.Run("lim1: single sticks and wide chords do not trigger", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            const fs::path dir = fs::temp_directory_path() / "lim1_marker_chord_neg_test";
            fs::remove_all(dir);
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_stubs::clearPadMarkerForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);
            // Unarmed (default): chord reads are no-ops and pass through.
            ps2_stubs::setPadOverrideState(
                static_cast<uint16_t>(0xFFFFu & ~kPadBtnL3 & ~kPadBtnR3), 0x80, 0x80, 0x80, 0x80);
            runPadRead(ctx, rdram);
            t.Equals(readButtons(rdram),
                     static_cast<uint16_t>(0xFFFFu & ~kPadBtnL3 & ~kPadBtnR3),
                     "unarmed reads pass through");
            t.IsTrue(!fs::exists(dir), "unarmed reads create no dir");
            t.IsTrue(ps2_stubs::setPadMarkerDirForTest(dir.string().c_str()), "marker should arm");

            const uint16_t relaxed = 0xFFFFu;
            const uint16_t l3 = static_cast<uint16_t>(0xFFFFu & ~kPadBtnL3);
            const uint16_t r3 = static_cast<uint16_t>(0xFFFFu & ~kPadBtnR3);
            const uint16_t chord = static_cast<uint16_t>(l3 & r3);
            const auto read = [&](uint64_t tick, uint16_t buttons) {
                ps2_stubs::setPadOverrideState(buttons, 0x80, 0x80, 0x80, 0x80);
                ps2_stubs::setPadMarkerTickForTest(tick);
                runPadRead(ctx, rdram);
            };
            const auto fileCount = [&]() {
                uint32_t n = 0u;
                for (const auto &de : fs::directory_iterator(dir))
                {
                    const std::string name = de.path().filename().string();
                    if (name.compare(0, 7, "marker-") == 0)
                        ++n;
                }
                return n;
            };
            // L3 alone, held and released: nothing.
            read(2000u, l3);
            read(2001u, l3);
            read(2002u, l3);
            read(2003u, relaxed);
            read(2300u, relaxed);
            t.Equals(fileCount(), 0u, "L3 alone writes nothing");
            // L3 then R3 200 ms later: nothing (12 ticks ~ 200 ms).
            read(3000u, l3);
            read(3012u, chord);
            read(3015u, chord);
            read(3016u, relaxed);
            read(3300u, relaxed);
            t.Equals(fileCount(), 0u, "a 200 ms chord writes nothing");
            // L3 then R3 50 ms later: one file (3 ticks ~ 50 ms).
            read(4000u, l3);
            read(4003u, chord);
            read(4005u, chord);
            read(4006u, relaxed);
            read(4300u, relaxed);
            t.Equals(fileCount(), 1u, "a 50 ms chord writes one file");
            // Holding the chord across reads fires once (no repeat).
            read(5000u, chord);
            read(5001u, chord);
            read(5002u, chord);
            read(5003u, relaxed);
            read(5300u, relaxed);
            t.Equals(fileCount(), 2u, "a held chord fires exactly once");

            ps2_stubs::clearPadOverrideState();
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_stubs::clearPadMarkerForTest();
            closePadPort(ctx, rdram);
            fs::remove_all(dir);
        });

        tc.Run("lim1: the session cap is 50 markers", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            const fs::path dir = fs::temp_directory_path() / "lim1_marker_cap_test";
            fs::remove_all(dir);
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_stubs::clearPadMarkerForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);
            t.IsTrue(ps2_stubs::setPadMarkerDirForTest(dir.string().c_str()), "marker should arm");
            const uint16_t relaxed = 0xFFFFu;
            const uint16_t chord =
                static_cast<uint16_t>(0xFFFFu & ~kPadBtnL3 & ~kPadBtnR3);
            for (uint32_t k = 0u; k < 55u; ++k)
            {
                const uint64_t tick = 1000u + 20u * k;
                ps2_stubs::setPadOverrideState(chord, 0x80, 0x80, 0x80, 0x80);
                ps2_stubs::setPadMarkerTickForTest(tick);
                runPadRead(ctx, rdram);
                ps2_stubs::setPadOverrideState(relaxed, 0x80, 0x80, 0x80, 0x80);
                ps2_stubs::setPadMarkerTickForTest(tick + 1u);
                runPadRead(ctx, rdram);
            }
            ps2_stubs::setPadOverrideState(relaxed, 0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadMarkerTickForTest(1000u + 20u * 55u + 300u);
            runPadRead(ctx, rdram);
            ps2_stubs::clearPadOverrideState();
            ps2_stubs::closePadMarkerForTest();
            uint32_t n = 0u;
            for (const auto &de : fs::directory_iterator(dir))
            {
                const std::string name = de.path().filename().string();
                if (name.compare(0, 7, "marker-") == 0 && name.size() > 4 &&
                    name.compare(name.size() - 4, 4, ".tsv") == 0)
                    ++n;
            }
            t.Equals(n, 50u, "exactly 50 files despite 55 chords");

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_stubs::clearPadMarkerForTest();
            closePadPort(ctx, rdram);
            fs::remove_all(dir);
        });

        tc.Run("rpl3: noted host values record as NV lines and serve back in order", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            const std::string recPath =
                (fs::temp_directory_path() / "rpl3_nv_test.txt").string();
            std::remove(recPath.c_str());
            Pl1EnvGuard scriptGuard("PS2X_PAD_SCRIPT");
            ::unsetenv("PS2X_PAD_SCRIPT");
            Pl1EnvGuard recordGuard("PS2X_PAD_RECORD");
            ::setenv("PS2X_PAD_RECORD", recPath.c_str(), 1);

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_rpl3::resetForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            const uint8_t rtc0[8] = {0x00, 0x56, 0x34, 0x12, 0x00, 0x16, 0x07, 0x04};
            const uint8_t rtc1[8] = {0x00, 0x57, 0x34, 0x12, 0x00, 0x16, 0x07, 0x04};
            // Pre-arm (production: boot RTC reads precede the first pad read).
            ps2_rpl3::noteRtc(12u, 0x31ae84u, rtc0);
            t.IsTrue(ps2_stubs::setPadRecordForTest(recPath.c_str()), "recorder should arm");
            const uint16_t mcSets[24] = {
                1, 2, 3, 4, 5, 2026, 1, 2, 3, 4, 5, 2026,
                6, 7, 8, 9, 10, 2025, 6, 7, 8, 9, 10, 2025,
            };
            ps2_rpl3::noteMc(118u, "mc0:/GAM*", mcSets, 2u, 2u);
            ps2_rpl3::noteTz(ps2_rpl3::kTickUnknown, -420);
            ps2_rpl3::noteRtc(13u, 0x2c75bcu, rtc1);
            // One span so the stream parses.
            ps2_stubs::setPadOverrideState(static_cast<uint16_t>(0xFFFFu & ~kPadBtnCross),
                                           0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(100u);
            runPadRead(ctx, rdram);
            ps2_stubs::setPadRecordTickForTest(110u);
            runPadRead(ctx, rdram);
            ps2_stubs::clearPadOverrideState();
            ps2_stubs::closePadRecordForTest();

            const std::string content = readFileBytes(recPath);
            t.IsTrue(content.rfind("# padrec v3\n", 0) == 0, "recording is v3");
            t.IsTrue(content.find("# NV rtc seq=0 tick=12 ra=0x31ae84 bytes=0056341200160704") !=
                         std::string::npos,
                     "rtc line recorded");
            t.IsTrue(content.find("# NV mc seq=0 tick=118") != std::string::npos,
                     "mc line recorded");
            t.IsTrue(content.find("# NV tz seq=0 tick=* mins=-420") != std::string::npos,
                     "tz line recorded");
            std::vector<ps2_stubs::PadScriptEntry> entries;
            t.IsTrue(ps2_stubs::parsePadScript(content.c_str(), entries),
                     "v3 recording still parses as a script");

            // Replay: the takes serve the recorded values in order.
            ::setenv("PS2X_PAD_SCRIPT", ("@" + recPath).c_str(), 1);
            ps2_rpl3::resetForTest();
            uint8_t got[8] = {0};
            t.IsTrue(ps2_rpl3::takeRtc(got), "first rtc serves");
            t.IsTrue(std::memcmp(got, rtc0, 8u) == 0, "first rtc bytes match");
            t.IsTrue(ps2_rpl3::takeRtc(got), "second rtc serves");
            t.IsTrue(std::memcmp(got, rtc1, 8u) == 0, "second rtc bytes match");
            t.IsTrue(!ps2_rpl3::takeRtc(got), "rtc exhausts");
            uint16_t set[6] = {0};
            for (int i = 0; i < 4; ++i)
            {
                char msg[64];
                std::snprintf(msg, sizeof(msg), "mc date %d serves", i);
                t.IsTrue(ps2_rpl3::takeMcDate(set), msg);
                t.IsTrue(std::memcmp(set, mcSets + i * 6u, sizeof(set)) == 0,
                         "mc date matches record order");
            }
            t.IsTrue(!ps2_rpl3::takeMcDate(set), "mc dates exhaust");
            int tz = 0;
            t.IsTrue(ps2_rpl3::takeTz(tz), "tz serves");
            t.Equals(tz, -420, "tz matches");
            t.IsTrue(ps2_rpl3::takeTz(tz), "tz is sticky");

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_rpl3::resetForTest();
            closePadPort(ctx, rdram);
            std::remove(recPath.c_str());
        });

        tc.Run("rpl3: v2 files take no values and parse as before", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            const std::string v2Path =
                (fs::temp_directory_path() / "rpl3_nv_v2_test.txt").string();
            {
                std::FILE *f = std::fopen(v2Path.c_str(), "w");
                t.IsTrue(f != nullptr, "v2 fixture writable");
                if (f)
                {
                    std::fputs("# padrec v2\n# start_utc=2026-10-11T00:00:00Z\n"
                               "1000:lx=128+ly=128+rx=128+ry=128:500,1500:cross:250\n",
                               f);
                    std::fclose(f);
                }
            }
            Pl1EnvGuard scriptGuard("PS2X_PAD_SCRIPT");
            ::setenv("PS2X_PAD_SCRIPT", ("@" + v2Path).c_str(), 1);
            ps2_rpl3::resetForTest();
            uint8_t rtc[8] = {0};
            uint16_t set[6] = {0};
            int tz = 0;
            t.IsTrue(!ps2_rpl3::takeRtc(rtc), "v2 takes no rtc");
            t.IsTrue(!ps2_rpl3::takeMcDate(set), "v2 takes no mc dates");
            t.IsTrue(!ps2_rpl3::takeTz(tz), "v2 takes no tz");
            std::vector<ps2_stubs::PadScriptEntry> entries;
            t.IsTrue(ps2_stubs::parsePadScriptFile(v2Path.c_str(), entries),
                     "v2 still parses");
            t.Equals(static_cast<uint32_t>(entries.size()), static_cast<uint32_t>(2),
                     "v2 entries unchanged");
            ps2_rpl3::resetForTest();
            std::remove(v2Path.c_str());
        });

        tc.Run("rpl3: sched gates fire the recorded subset by cycle", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            const std::string gatePath =
                (fs::temp_directory_path() / "rpl3_nv_sched_test.txt").string();
            {
                std::FILE *f = std::fopen(gatePath.c_str(), "w");
                t.IsTrue(f != nullptr, "fixture writable");
                if (f)
                {
                    std::fputs("# padrec v3\n"
                               "# NV sched seq=0 tick=10 cycle=100 nf=1 w=1 f=T,0,90\n"
                               "# NV sched seq=1 tick=20 cycle=200 nf=2 w=0 f=S,0,200;E,0,205\n"
                               "# NV idlerace seq=0 tick=30 cycle=300 timer=310 event=305 "
                               "winner=E agree=1\n"
                               "1000:cross:250\n",
                               f);
                    std::fclose(f);
                }
            }
            Pl1EnvGuard scriptGuard("PS2X_PAD_SCRIPT");
            ::setenv("PS2X_PAD_SCRIPT", ("@" + gatePath).c_str(), 1);
            ps2_rpl3::resetForTest();
            const ps2_rpl3::SchedEvent due0[] = {{6u, 0u, 90u}, {1u, 0u, 100u}};
            bool fire0[2] = {true, true};
            t.IsTrue(!ps2_rpl3::takeSched(50u, due0, 2u, fire0), "no gate before first");
            t.IsTrue(ps2_rpl3::takeSched(100u, due0, 2u, fire0), "gate matches cycle");
            t.IsTrue(fire0[0] && !fire0[1], "recorded subset marked");
            t.IsTrue(!ps2_rpl3::takeSched(100u, due0, 2u, fire0), "gate consumed");
            const ps2_rpl3::SchedEvent due1[] = {{1u, 0u, 200u}, {2u, 0u, 205u}, {6u, 0u, 150u}};
            bool fire1[3] = {false, false, true};
            t.IsTrue(ps2_rpl3::takeSched(200u, due1, 3u, fire1), "second gate matches");
            t.IsTrue(fire1[0] && fire1[1] && !fire1[2], "second subset marked");
            t.IsTrue(!ps2_rpl3::takeSched(300u, due1, 3u, fire1), "gates exhausted");
            std::vector<ps2_stubs::PadScriptEntry> entries;
            t.IsTrue(ps2_stubs::parsePadScriptFile(gatePath.c_str(), entries),
                     "stream still parses around sched lines");
            ps2_rpl3::resetForTest();
            std::remove(gatePath.c_str());
        });

        tc.Run("rpl3: sched batches record and serve back", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            const std::string recPath =
                (fs::temp_directory_path() / "rpl3_nv_sched_rec.txt").string();
            std::remove(recPath.c_str());
            Pl1EnvGuard scriptGuard("PS2X_PAD_SCRIPT");
            ::unsetenv("PS2X_PAD_SCRIPT");
            Pl1EnvGuard recordGuard("PS2X_PAD_RECORD");
            ::setenv("PS2X_PAD_RECORD", recPath.c_str(), 1);

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_rpl3::resetForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);
            t.IsTrue(ps2_stubs::setPadRecordForTest(recPath.c_str()), "recorder should arm");
            const ps2_rpl3::SchedEvent fired[] = {{6u, 0u, 90u}};
            ps2_rpl3::noteSched(10u, 100u, fired, 1u, 1u);
            ps2_stubs::closePadRecordForTest();

            const std::string content = readFileBytes(recPath);
            t.IsTrue(content.find("# NV sched seq=0 tick=10 cycle=100 nf=1 w=1 f=T,0,90") !=
                         std::string::npos,
                     "sched line recorded");
            ::setenv("PS2X_PAD_SCRIPT", ("@" + recPath).c_str(), 1);
            ps2_rpl3::resetForTest();
            const ps2_rpl3::SchedEvent due[] = {{6u, 0u, 90u}, {1u, 0u, 100u}};
            bool fire[2] = {false, false};
            t.IsTrue(ps2_rpl3::takeSched(100u, due, 2u, fire), "recorded gate serves");
            t.IsTrue(fire[0] && !fire[1], "recorded subset marked");

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_rpl3::resetForTest();
            closePadPort(ctx, rdram);
            std::remove(recPath.c_str());
        });

        tc.Run("rpl3: idle gates force the recorded winner by cycle", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            const std::string gatePath =
                (fs::temp_directory_path() / "rpl3_nv_idle_test.txt").string();
            {
                std::FILE *f = std::fopen(gatePath.c_str(), "w");
                t.IsTrue(f != nullptr, "fixture writable");
                if (f)
                {
                    std::fputs("# padrec v3\n"
                               "# NV idlerace seq=0 tick=1 cycle=5062755 timer=7381472 event=9830598 "
                               "winner=T agree=0\n"
                               "# NV idlerace seq=1 tick=9 cycle=9000000 timer=9100000 event=9200000 "
                               "winner=E agree=0\n"
                               "1000:cross:250\n",
                               f);
                    std::fclose(f);
                }
            }
            Pl1EnvGuard scriptGuard("PS2X_PAD_SCRIPT");
            ::setenv("PS2X_PAD_SCRIPT", ("@" + gatePath).c_str(), 1);
            ps2_rpl3::resetForTest();
            char winner = 0;
            t.IsTrue(!ps2_rpl3::takeIdle(100u, 7381472u, 9830598u, winner),
                     "no gate before first");
            t.IsTrue(ps2_rpl3::takeIdle(5062755u, 7381472u, 9830598u, winner),
                     "gate matches cycle");
            t.IsTrue(winner == 'T', "recorded winner served");
            t.IsTrue(!ps2_rpl3::takeIdle(5062755u, 7381472u, 9830598u, winner),
                     "gate consumed");
            t.IsTrue(ps2_rpl3::takeIdle(9000000u, 9100000u, 9200000u, winner),
                     "second gate matches");
            t.IsTrue(winner == 'E', "second winner served");
            t.IsTrue(!ps2_rpl3::takeIdle(9900000u, 9100000u, 9200000u, winner),
                     "gates exhausted");
            ps2_rpl3::resetForTest();
            std::remove(gatePath.c_str());
        });

        tc.Run("rpl3: idle notes keep disagreements, drop agreements", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            const std::string recPath =
                (fs::temp_directory_path() / "rpl3_nv_idle_rec.txt").string();
            std::remove(recPath.c_str());
            Pl1EnvGuard scriptGuard("PS2X_PAD_SCRIPT");
            ::unsetenv("PS2X_PAD_SCRIPT");
            Pl1EnvGuard recordGuard("PS2X_PAD_RECORD");
            ::setenv("PS2X_PAD_RECORD", recPath.c_str(), 1);

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_rpl3::resetForTest();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);
            t.IsTrue(ps2_stubs::setPadRecordForTest(recPath.c_str()), "recorder should arm");
            // Leave the stream mid-line (open span, no trailing newline).
            ps2_stubs::setPadOverrideState(static_cast<uint16_t>(0xFFFFu & ~kPadBtnCross),
                                           0x80, 0x80, 0x80, 0x80);
            ps2_stubs::setPadRecordTickForTest(100u);
            runPadRead(ctx, rdram);
            ps2_stubs::clearPadOverrideState();
            ps2_rpl3::noteIdle(1u, 5062755u, 7381472u, 9830598u, 'E', true);
            ps2_rpl3::noteIdle(9u, 9000000u, 9100000u, 9200000u, 'E', false);
            ps2_stubs::closePadRecordForTest();

            const std::string content = readFileBytes(recPath);
            t.IsTrue(content.find("cycle=5062755") == std::string::npos,
                     "agreement leaves no line");
            t.IsTrue(content.find("\n# NV idlerace seq=0 tick=9 cycle=9000000 timer=9100000 "
                                  "event=9200000 winner=E agree=0\n") != std::string::npos,
                     "disagreement recorded on its own line (not glued to the stream)");
            std::vector<ps2_stubs::PadScriptEntry> entries;
            t.IsTrue(ps2_stubs::parsePadScript(content.c_str(), entries),
                     "stream still parses around the NV line");

            ps2_stubs::clearPadScriptForTest();
            ps2_stubs::clearPadRecordForTest();
            ps2_rpl3::resetForTest();
            closePadPort(ctx, rdram);
            std::remove(recPath.c_str());
        });

        tc.Run("rpl3: malformed NV lines are ignored, good ones serve", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            const std::string badPath =
                (fs::temp_directory_path() / "rpl3_nv_bad_test.txt").string();
            {
                std::FILE *f = std::fopen(badPath.c_str(), "w");
                t.IsTrue(f != nullptr, "fixture writable");
                if (f)
                {
                    std::fputs("# padrec v3\n"
                               "# NV rtc seq=0 tick=12 ra=0x31ae84 bytes=ZZZ\n"
                               "# NV mc seq=0 tick=118 q='x' built=1 served=1 n=1 e=1,2\n"
                               "# NV bogus kind=1\n"
                               "# NV rtc seq=1 tick=13 ra=0x2c75bc bytes=0057341200160704\n"
                               "1000:lx=128+ly=128+rx=128+ry=128:500\n",
                               f);
                    std::fclose(f);
                }
            }
            Pl1EnvGuard scriptGuard("PS2X_PAD_SCRIPT");
            ::setenv("PS2X_PAD_SCRIPT", ("@" + badPath).c_str(), 1);
            ps2_rpl3::resetForTest();
            uint8_t rtc[8] = {0};
            const uint8_t want[8] = {0x00, 0x57, 0x34, 0x12, 0x00, 0x16, 0x07, 0x04};
            t.IsTrue(ps2_rpl3::takeRtc(rtc), "good rtc serves");
            t.IsTrue(std::memcmp(rtc, want, 8u) == 0, "good rtc bytes match");
            t.IsTrue(!ps2_rpl3::takeRtc(rtc), "only one rtc loaded");
            uint16_t set[6] = {0};
            t.IsTrue(!ps2_rpl3::takeMcDate(set), "truncated mc line ignored");
            std::vector<ps2_stubs::PadScriptEntry> entries;
            t.IsTrue(ps2_stubs::parsePadScriptFile(badPath.c_str(), entries),
                     "stream still parses around bad NV lines");
            ps2_rpl3::resetForTest();
            std::remove(badPath.c_str());
        });
    });
}
