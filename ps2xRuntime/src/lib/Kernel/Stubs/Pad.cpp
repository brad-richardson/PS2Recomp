#include "Common.h"
#include "ps2_e3.h"
#include "ps2_e41_trace.h"
#include "ps2_e44_trace.h"
#include "ps2_pad_latch.h"
#include "Pad.h"

#include <chrono>
#include <string>
#include <vector>

namespace ps2_stubs
{
    namespace
    {
        constexpr uint8_t kPadModeDigital = 0x41;
        constexpr uint8_t kPadModeDualShock = 0x73;
        constexpr uint8_t kPadAnalogCenter = 0x80;
        constexpr int32_t kPadTypeDigital = 4;
        constexpr int32_t kPadTypeDualShock = 7;
        constexpr int32_t kPadStateDisconnected = 0;
        constexpr int32_t kPadStateExecCmd = 5;
        constexpr int32_t kPadStateStable = 6;
        constexpr size_t kPadPortCount = 2;
        constexpr size_t kPadSlotCount = 1;

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

        struct PadInputState
        {
            uint16_t buttons = 0xFFFF; // active-low
            uint8_t rx = kPadAnalogCenter;
            uint8_t ry = kPadAnalogCenter;
            uint8_t lx = kPadAnalogCenter;
            uint8_t ly = kPadAnalogCenter;
        };

        struct PadPortState
        {
            bool open = false;
            bool analogMode = false;  // real pads power up DIGITAL (CURID=4, mode 0x41)
            bool pressureEnabled = false;
            bool lastUsedOverride = false;
            bool lastUsedBackend = false;
            bool lastReadOk = false;
            uint16_t buttonMask = 0xFFFFu;
            uint32_t dmaAddr = 0u;
            uint32_t reqState = 0u;
            uint32_t transientState = 0u;
            PadInputState lastInput{};
            uint8_t lastData[32]{};
            uint32_t readCount = 0u;
            uint32_t lastReadDataAddr = 0u;
        };

        std::mutex g_padOverrideMutex;
        std::mutex g_padStateMutex;
        bool g_padOverrideEnabled = false;
        PadInputState g_padOverrideState{};
        PadPortState g_padPorts[kPadPortCount]{};
        int g_padReadLogCount = 0;

        // E2a stimulus hook: env-armed pad-state flip + 326EB0 dormant-arm
        // tripwires. Unset PS2X_PAD_STIM_AFTER (default) = one relaxed atomic
        // check per pad call, zero behavior change.
        struct PadStimulus
        {
            std::mutex mutex;
            bool initDone = false;
            bool enabled = false;
            uint64_t afterReads = 0;
            uint64_t wallMinSec = 0;
            std::chrono::steady_clock::time_point startWall{};
            uint64_t totalReads = 0;
            uint64_t totalGetState = 0;
            uint64_t totalPortOpens = 0;
            uint64_t postFirePortOpens = 0;
            bool fired = false;
            std::vector<uint32_t> preFireGetStateRa;
            std::vector<uint32_t> postFireNewRa;
        };
        PadStimulus g_padStim;
        std::atomic<bool> g_padStimInitDone{false};
        std::atomic<bool> g_padStimArmed{false};

        constexpr uint64_t kPadStimMilestoneReads = 25000ull;
        constexpr uint64_t kPadStimWaitMilestoneReads = 1000ull;
        constexpr uint16_t kPadStimButtons = 0x0000; // active-low: all pressed
        constexpr uint8_t kPadStimLx = 0x00;
        constexpr uint8_t kPadStimLy = 0x00;
        constexpr uint8_t kPadStimRx = 0xFF;
        constexpr uint8_t kPadStimRy = 0xFF;

        uint64_t padStimWallSecLocked()
        {
            const auto now = std::chrono::steady_clock::now();
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(now - g_padStim.startWall).count());
        }

        void padStimInitLocked()
        {
            if (g_padStim.initDone)
            {
                return;
            }
            g_padStim.initDone = true;
            g_padStim.startWall = std::chrono::steady_clock::now();
            const char *after = std::getenv("PS2X_PAD_STIM_AFTER");
            const char *wallMin = std::getenv("PS2X_PAD_STIM_WALLMIN");
            const unsigned long long afterN = after ? std::strtoull(after, nullptr, 10) : 0ull;
            const unsigned long long wallN = wallMin ? std::strtoull(wallMin, nullptr, 10) : 0ull;
            if (afterN > 0ull)
            {
                g_padStim.enabled = true;
                g_padStim.afterReads = static_cast<uint64_t>(afterN);
                g_padStim.wallMinSec = static_cast<uint64_t>(wallN);
                g_padStimArmed.store(true, std::memory_order_relaxed);
                std::fprintf(stderr,
                             "[padstim] armed after=%llu wallmin=%llus "
                             "flip=buttons:0xFFFF->0x0000,lx:0x80->0x00,ly:0x80->0x00,"
                             "rx:0x80->0xFF,ry:0x80->0xFF\n",
                             afterN, wallN);
            }
        }

        void padStimEnsureInit()
        {
            if (g_padStimInitDone.load(std::memory_order_relaxed))
            {
                return;
            }
            std::lock_guard<std::mutex> lock(g_padStim.mutex);
            padStimInitLocked();
            g_padStimInitDone.store(true, std::memory_order_relaxed);
        }

        bool padStimHasRa(const std::vector<uint32_t> &vec, uint32_t ra)
        {
            for (const uint32_t v : vec)
            {
                if (v == ra)
                {
                    return true;
                }
            }
            return false;
        }

        void padStimOnRead(PadInputState &state)
        {
            padStimEnsureInit();
            if (!g_padStimArmed.load(std::memory_order_relaxed))
            {
                return;
            }
            std::lock_guard<std::mutex> lock(g_padStim.mutex);
            if (!g_padStim.enabled)
            {
                return;
            }
            ++g_padStim.totalReads;
            const uint64_t reads = g_padStim.totalReads;
            if (!g_padStim.fired)
            {
                const uint64_t wall = padStimWallSecLocked();
                const bool readsPast = reads >= g_padStim.afterReads;
                if (readsPast && wall >= g_padStim.wallMinSec)
                {
                    g_padStim.fired = true;
                    std::fprintf(stderr,
                                 "[padstim] FIRED reads=%llu wall=%llus getstate=%llu "
                                 "portopens=%llu preFireGetStateRaN=%llu\n",
                                 static_cast<unsigned long long>(reads),
                                 static_cast<unsigned long long>(wall),
                                 static_cast<unsigned long long>(g_padStim.totalGetState),
                                 static_cast<unsigned long long>(g_padStim.totalPortOpens),
                                 static_cast<unsigned long long>(g_padStim.preFireGetStateRa.size()));
                }
                else if (reads % kPadStimMilestoneReads == 0ull ||
                         (readsPast && reads % kPadStimWaitMilestoneReads == 0ull))
                {
                    std::fprintf(stderr,
                                 "[padstim] progress reads=%llu wall=%llus getstate=%llu "
                                 "portopens=%llu readsPast=%d\n",
                                 static_cast<unsigned long long>(reads),
                                 static_cast<unsigned long long>(wall),
                                 static_cast<unsigned long long>(g_padStim.totalGetState),
                                 static_cast<unsigned long long>(g_padStim.totalPortOpens),
                                 readsPast ? 1 : 0);
                }
            }
            else if (reads % kPadStimMilestoneReads == 0ull)
            {
                std::fprintf(stderr,
                             "[padstim] post reads=%llu wall=%llus getstate=%llu "
                             "portopens=%llu postFirePortOpens=%llu postFireNewRaN=%llu\n",
                             static_cast<unsigned long long>(reads),
                             static_cast<unsigned long long>(padStimWallSecLocked()),
                             static_cast<unsigned long long>(g_padStim.totalGetState),
                             static_cast<unsigned long long>(g_padStim.totalPortOpens),
                             static_cast<unsigned long long>(g_padStim.postFirePortOpens),
                             static_cast<unsigned long long>(g_padStim.postFireNewRa.size()));
            }
            if (g_padStim.fired)
            {
                state.buttons = kPadStimButtons;
                state.lx = kPadStimLx;
                state.ly = kPadStimLy;
                state.rx = kPadStimRx;
                state.ry = kPadStimRy;
            }
        }

        void padStimOnGetState(uint32_t ra)
        {
            padStimEnsureInit();
            if (!g_padStimArmed.load(std::memory_order_relaxed))
            {
                return;
            }
            std::lock_guard<std::mutex> lock(g_padStim.mutex);
            if (!g_padStim.enabled)
            {
                return;
            }
            ++g_padStim.totalGetState;
            if (!g_padStim.fired)
            {
                if (g_padStim.preFireGetStateRa.size() < 64 &&
                    !padStimHasRa(g_padStim.preFireGetStateRa, ra))
                {
                    g_padStim.preFireGetStateRa.push_back(ra);
                }
                return;
            }
            if (!padStimHasRa(g_padStim.preFireGetStateRa, ra) &&
                !padStimHasRa(g_padStim.postFireNewRa, ra))
            {
                if (g_padStim.postFireNewRa.size() < 64)
                {
                    g_padStim.postFireNewRa.push_back(ra);
                }
                std::fprintf(stderr,
                             "[padstim] GETSTATE-NEWRA ra=0x%08x reads=%llu wall=%llus "
                             "getstate=%llu\n",
                             ra,
                             static_cast<unsigned long long>(g_padStim.totalReads),
                             static_cast<unsigned long long>(padStimWallSecLocked()),
                             static_cast<unsigned long long>(g_padStim.totalGetState));
            }
        }

        void padStimOnPortOpen(uint32_t ra, int port, int slot)
        {
            padStimEnsureInit();
            if (!g_padStimArmed.load(std::memory_order_relaxed))
            {
                return;
            }
            std::lock_guard<std::mutex> lock(g_padStim.mutex);
            if (!g_padStim.enabled)
            {
                return;
            }
            ++g_padStim.totalPortOpens;
            if (!g_padStim.fired)
            {
                return;
            }
            ++g_padStim.postFirePortOpens;
            if (g_padStim.postFirePortOpens <= 10ull)
            {
                std::fprintf(stderr,
                             "[padstim] PORTOPEN post-fire n=%llu ra=0x%08x port=%d slot=%d "
                             "reads=%llu wall=%llus\n",
                             static_cast<unsigned long long>(g_padStim.postFirePortOpens),
                             ra, port, slot,
                             static_cast<unsigned long long>(g_padStim.totalReads),
                             static_cast<unsigned long long>(padStimWallSecLocked()));
            }
        }

        // E31 DEV-ONLY scripted pad input (PS2X_PAD_SCRIPT). Unset/empty
        // (default) = one relaxed atomic check per pad read, zero behavior
        // change. Format: "t_ms:spec:hold_ms,..." (see parsePadScript).
        // Each entry presses its buttons and/or drives its analog axes while
        // atMs <= nowMs < atMs + holdMs, where nowMs is milliseconds since
        // the first pad call. Overlapping button entries accumulate;
        // overlapping analog entries resolve last-active-wins per axis.
        struct PadScriptRuntimeEntry
        {
            PadScriptEntry entry{};
            bool loggedPress = false;
            bool loggedDone = false;
        };

        struct PadScript
        {
            std::mutex mutex;
            bool initDone = false;
            bool enabled = false;
            // E33: vsync clock. When true, entry atMs/holdMs count guest
            // time (vsyncTick * 1000/59.94 ms) instead of host wall ms.
            bool vsyncClock = false;
            bool testVsyncSet = false;
            uint64_t testVsyncTick = 0u;
            std::chrono::steady_clock::time_point startWall{};
            std::vector<PadScriptRuntimeEntry> entries;
            bool testNowSet = false;
            uint64_t testNowMs = 0u;
        };
        PadScript g_padScript;
        std::atomic<bool> g_padScriptInitDone{false};
        std::atomic<bool> g_padScriptArmed{false};

        bool padScriptParseU64(const std::string &text, uint64_t &out)
        {
            if (text.empty())
            {
                return false;
            }
            uint64_t value = 0u;
            for (const char ch : text)
            {
                if (ch < '0' || ch > '9')
                {
                    return false;
                }
                value = value * 10u + static_cast<uint64_t>(ch - '0');
            }
            out = value;
            return true;
        }

        bool padScriptButtonMask(const std::string &name, uint16_t &mask)
        {
            if (name == "select")
                mask = kPadBtnSelect;
            else if (name == "l3")
                mask = kPadBtnL3;
            else if (name == "r3")
                mask = kPadBtnR3;
            else if (name == "start")
                mask = kPadBtnStart;
            else if (name == "up")
                mask = kPadBtnUp;
            else if (name == "right")
                mask = kPadBtnRight;
            else if (name == "down")
                mask = kPadBtnDown;
            else if (name == "left")
                mask = kPadBtnLeft;
            else if (name == "l2")
                mask = kPadBtnL2;
            else if (name == "r2")
                mask = kPadBtnR2;
            else if (name == "l1")
                mask = kPadBtnL1;
            else if (name == "r1")
                mask = kPadBtnR1;
            else if (name == "triangle")
                mask = kPadBtnTriangle;
            else if (name == "circle")
                mask = kPadBtnCircle;
            else if (name == "cross")
                mask = kPadBtnCross;
            else if (name == "square")
                mask = kPadBtnSquare;
            else
                return false;
            return true;
        }

        bool padScriptParseSpec(const std::string &text, PadScriptEntry &entry)
        {
            if (text.empty())
            {
                return false;
            }
            bool anyToken = false;
            size_t begin = 0u;
            while (begin <= text.size())
            {
                const size_t end = text.find('+', begin);
                const std::string token = text.substr(begin, end == std::string::npos ? end : end - begin);
                if (token.empty())
                {
                    return false;
                }
                anyToken = true;
                const size_t eq = token.find('=');
                if (eq == std::string::npos)
                {
                    uint16_t mask = 0u;
                    if (!padScriptButtonMask(token, mask))
                    {
                        return false;
                    }
                    entry.pressMask = static_cast<uint16_t>(entry.pressMask | mask);
                }
                else
                {
                    const std::string axis = token.substr(0, eq);
                    const std::string valueText = token.substr(eq + 1u);
                    uint64_t value = 0u;
                    if (!padScriptParseU64(valueText, value) || value > 255u)
                    {
                        return false;
                    }
                    const auto byte = static_cast<uint8_t>(value);
                    if (axis == "lx")
                    {
                        entry.hasLx = true;
                        entry.lx = byte;
                    }
                    else if (axis == "ly")
                    {
                        entry.hasLy = true;
                        entry.ly = byte;
                    }
                    else if (axis == "rx")
                    {
                        entry.hasRx = true;
                        entry.rx = byte;
                    }
                    else if (axis == "ry")
                    {
                        entry.hasRy = true;
                        entry.ry = byte;
                    }
                    else
                    {
                        return false;
                    }
                }
                if (end == std::string::npos)
                {
                    break;
                }
                begin = end + 1u;
            }
            return anyToken && (entry.pressMask != 0u || entry.hasLx || entry.hasLy || entry.hasRx || entry.hasRy);
        }

        bool padScriptParse(const char *spec, std::vector<PadScriptEntry> &entries)
        {
            std::vector<PadScriptEntry> parsed;
            if (!spec || spec[0] == '\0')
            {
                return false;
            }
            const std::string text(spec);
            size_t begin = 0u;
            while (begin <= text.size())
            {
                const size_t end = text.find(',', begin);
                const std::string item = text.substr(begin, end == std::string::npos ? end : end - begin);
                const size_t c1 = item.find(':');
                const size_t c2 = (c1 == std::string::npos) ? std::string::npos : item.find(':', c1 + 1u);
                if (c1 == std::string::npos || c2 == std::string::npos || item.find(':', c2 + 1u) != std::string::npos)
                {
                    return false;
                }
                PadScriptEntry entry{};
                if (!padScriptParseU64(item.substr(0, c1), entry.atMs))
                {
                    return false;
                }
                if (!padScriptParseU64(item.substr(c2 + 1u), entry.holdMs) || entry.holdMs == 0u)
                {
                    return false;
                }
                if (!padScriptParseSpec(item.substr(c1 + 1u, c2 - c1 - 1u), entry))
                {
                    return false;
                }
                parsed.push_back(entry);
                if (end == std::string::npos)
                {
                    break;
                }
                begin = end + 1u;
            }
            if (parsed.empty())
            {
                return false;
            }
            entries = parsed;
            return true;
        }

        // E33: guest-vsync clock maps a vsync tick to guest milliseconds.
        // 1000 ms per 59.94 vsyncs: ms = tick * 100000 / 5994.
        uint64_t padScriptVsyncTickToMs(uint64_t tick)
        {
            return (tick * 100000ull) / 5994ull;
        }

        void padScriptInstallLocked(const std::vector<PadScriptEntry> &parsed, const char *source)
        {
            g_padScript.entries.clear();
            for (const PadScriptEntry &entry : parsed)
            {
                PadScriptRuntimeEntry runtime{};
                runtime.entry = entry;
                g_padScript.entries.push_back(runtime);
            }
            g_padScript.startWall = std::chrono::steady_clock::now();
            g_padScript.enabled = true;
            g_padScriptArmed.store(true, std::memory_order_relaxed);
            std::fprintf(stderr, "[padscript] armed n=%llu source=%s clock=%s\n",
                         static_cast<unsigned long long>(g_padScript.entries.size()), source,
                         g_padScript.vsyncClock ? "vsync" : "wall");
        }

        void padScriptInitLocked()
        {
            if (g_padScript.initDone)
            {
                return;
            }
            g_padScript.initDone = true;
            g_padScript.startWall = std::chrono::steady_clock::now();
            if (const char *clock = std::getenv("PS2X_PAD_SCRIPT_CLOCK"))
            {
                g_padScript.vsyncClock = (std::string(clock) == "vsync");
            }
            const char *spec = std::getenv("PS2X_PAD_SCRIPT");
            if (!spec || spec[0] == '\0')
            {
                return;
            }
            std::vector<PadScriptEntry> parsed;
            if (!padScriptParse(spec, parsed))
            {
                std::fprintf(stderr, "[padscript] ignoring malformed PS2X_PAD_SCRIPT\n");
                return;
            }
            padScriptInstallLocked(parsed, "env");
        }

        void padScriptEnsureInit()
        {
            if (g_padScriptInitDone.load(std::memory_order_relaxed))
            {
                return;
            }
            std::lock_guard<std::mutex> lock(g_padScript.mutex);
            padScriptInitLocked();
            g_padScriptInitDone.store(true, std::memory_order_relaxed);
        }

        uint64_t padScriptNowMsLocked(uint64_t guestVsyncTick)
        {
            if (g_padScript.testNowSet)
            {
                return g_padScript.testNowMs;
            }
            if (g_padScript.vsyncClock)
            {
                const uint64_t tick = g_padScript.testVsyncSet ? g_padScript.testVsyncTick : guestVsyncTick;
                return padScriptVsyncTickToMs(tick);
            }
            const auto now = std::chrono::steady_clock::now();
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - g_padScript.startWall).count());
        }

        void padScriptOnRead(PadInputState &state, uint64_t guestVsyncTick)
        {
            padScriptEnsureInit();
            if (!g_padScriptArmed.load(std::memory_order_relaxed))
            {
                return;
            }
            std::lock_guard<std::mutex> lock(g_padScript.mutex);
            if (!g_padScript.enabled)
            {
                return;
            }
            const uint64_t nowMs = padScriptNowMsLocked(guestVsyncTick);
            for (size_t i = 0; i < g_padScript.entries.size(); ++i)
            {
                PadScriptRuntimeEntry &runtime = g_padScript.entries[i];
                const PadScriptEntry &entry = runtime.entry;
                const bool active = nowMs >= entry.atMs && nowMs < entry.atMs + entry.holdMs;
                if (active)
                {
                    state.buttons = static_cast<uint16_t>(state.buttons & ~entry.pressMask);
                    if (entry.hasLx)
                    {
                        state.lx = entry.lx;
                    }
                    if (entry.hasLy)
                    {
                        state.ly = entry.ly;
                    }
                    if (entry.hasRx)
                    {
                        state.rx = entry.rx;
                    }
                    if (entry.hasRy)
                    {
                        state.ry = entry.ry;
                    }
                    if (!runtime.loggedPress)
                    {
                        runtime.loggedPress = true;
                        std::fprintf(stderr,
                                     "[padscript] press i=%llu now=%llums at=%llums hold=%llums "
                                     "buttons=0x%04x\n",
                                     static_cast<unsigned long long>(i),
                                     static_cast<unsigned long long>(nowMs),
                                     static_cast<unsigned long long>(entry.atMs),
                                     static_cast<unsigned long long>(entry.holdMs),
                                     entry.pressMask);
                    }
                }
                else if (nowMs >= entry.atMs + entry.holdMs && !runtime.loggedDone)
                {
                    runtime.loggedDone = true;
                    std::fprintf(stderr, "[padscript] release i=%llu now=%llums\n",
                                 static_cast<unsigned long long>(i),
                                 static_cast<unsigned long long>(nowMs));
                }
            }
        }

        uint8_t axisToByte(float axis)
        {
            axis = std::clamp(axis, -1.0f, 1.0f);
            const float mapped = (axis + 1.0f) * 127.5f;
            return static_cast<uint8_t>(std::lround(mapped));
        }

        void setButton(PadInputState &state, uint16_t mask, bool pressed)
        {
            if (pressed)
            {
                state.buttons = static_cast<uint16_t>(state.buttons & ~mask);
            }
        }

        int findFirstGamepad()
        {
            for (int i = 0; i < 4; ++i)
            {
                if (IsGamepadAvailable(i))
                {
                    return i;
                }
            }
            return -1;
        }

        void applyGamepadState(PadInputState &state)
        {
            if (!IsWindowReady())
            {
                return;
            }

            const int gamepad = findFirstGamepad();
            if (gamepad < 0)
            {
                return;
            }

            // Raylib mapping (PS2 -> raylib buttons/axes):
            // D-Pad -> LEFT_FACE_*, Cross/Circle/Square/Triangle -> RIGHT_FACE_*
            // L1/R1 -> TRIGGER_1, L2/R2 -> TRIGGER_2, L3/R3 -> THUMB
            // Select/Start -> MIDDLE_LEFT/MIDDLE_RIGHT
            state.lx = axisToByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X));
            state.ly = axisToByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_Y));
            state.rx = axisToByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_X));
            state.ry = axisToByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_Y));

            setButton(state, kPadBtnUp, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_UP));
            setButton(state, kPadBtnDown, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN));
            setButton(state, kPadBtnLeft, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT));
            setButton(state, kPadBtnRight, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT));

            setButton(state, kPadBtnCross, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
            setButton(state, kPadBtnCircle, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT));
            setButton(state, kPadBtnSquare, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT));
            setButton(state, kPadBtnTriangle, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP));

            setButton(state, kPadBtnL1, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1));
            setButton(state, kPadBtnR1, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1));
            setButton(state, kPadBtnL2, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2));
            setButton(state, kPadBtnR2, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2));

            setButton(state, kPadBtnL3, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_THUMB));
            setButton(state, kPadBtnR3, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_THUMB));

            setButton(state, kPadBtnSelect, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_LEFT));
            setButton(state, kPadBtnStart, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT));
        }

        void applyKeyboardState(PadInputState &state, bool allowAnalog)
        {
            if (!IsWindowReady())
            {
                return;
            }

            // Keyboard mapping (PS2 -> keys):
            // D-Pad: arrows, Square/Cross/Circle/Triangle: Z/X/C/V
            // L1/R1: Q/E, L2/R2: 1/3, Start/Select: Enter/RightShift
            // L3/R3: LeftCtrl/RightCtrl, Analog left: WASD
            setButton(state, kPadBtnUp, IsKeyDown(KEY_UP));
            setButton(state, kPadBtnDown, IsKeyDown(KEY_DOWN));
            setButton(state, kPadBtnLeft, IsKeyDown(KEY_LEFT));
            setButton(state, kPadBtnRight, IsKeyDown(KEY_RIGHT));

            setButton(state, kPadBtnSquare, IsKeyDown(KEY_Z));
            setButton(state, kPadBtnCross, IsKeyDown(KEY_X));
            setButton(state, kPadBtnCircle, IsKeyDown(KEY_C));
            setButton(state, kPadBtnTriangle, IsKeyDown(KEY_V));

            setButton(state, kPadBtnL1, IsKeyDown(KEY_Q));
            setButton(state, kPadBtnR1, IsKeyDown(KEY_E));
            setButton(state, kPadBtnL2, IsKeyDown(KEY_ONE));
            setButton(state, kPadBtnR2, IsKeyDown(KEY_THREE));

            setButton(state, kPadBtnStart, IsKeyDown(KEY_ENTER));
            setButton(state, kPadBtnSelect, IsKeyDown(KEY_RIGHT_SHIFT));
            setButton(state, kPadBtnL3, IsKeyDown(KEY_LEFT_CONTROL));
            setButton(state, kPadBtnR3, IsKeyDown(KEY_RIGHT_CONTROL));

            if (!allowAnalog)
            {
                return;
            }

            float ax = 0.0f;
            float ay = 0.0f;
            if (IsKeyDown(KEY_D))
                ax += 1.0f;
            if (IsKeyDown(KEY_A))
                ax -= 1.0f;
            if (IsKeyDown(KEY_S))
                ay += 1.0f;
            if (IsKeyDown(KEY_W))
                ay -= 1.0f;

            if (ax != 0.0f || ay != 0.0f)
            {
                state.lx = axisToByte(ax);
                state.ly = axisToByte(ay);
            }
        }

        void resetPadStateLocked()
        {
            for (PadPortState &portState : g_padPorts)
            {
                portState = PadPortState{};
            }
        }

        PadPortState *lookupPadPortStateLocked(int port, int slot)
        {
            if (port < 0 || port >= static_cast<int>(kPadPortCount))
            {
                return nullptr;
            }
            if (slot < 0 || slot >= static_cast<int>(kPadSlotCount))
            {
                return nullptr;
            }
            return &g_padPorts[port];
        }

        void initializePadPortLocked(PadPortState &portState, uint32_t dmaAddr)
        {
            portState.open = true;
            portState.analogMode = false;  // real pads open DIGITAL
            portState.pressureEnabled = false;
            portState.buttonMask = 0xFFFFu;
            portState.dmaAddr = dmaAddr;
            portState.reqState = 0u;
            portState.transientState = 0u;
        }

        void queueExecCmdStateLocked(PadPortState &portState)
        {
            portState.transientState = static_cast<uint32_t>(kPadStateExecCmd);
        }

        uint8_t pressureValue(const PadInputState &state, const PadPortState &portState, uint16_t mask)
        {
            if (!portState.pressureEnabled)
            {
                return 0u;
            }
            if ((portState.buttonMask & mask) == 0u)
            {
                return 0u;
            }
            return ((state.buttons & mask) == 0u) ? 0xFFu : 0u;
        }

        void fillPadStatus(uint8_t *data, const PadInputState &state, const PadPortState &portState)
        {
            std::memset(data, 0, 32);
            data[1] = portState.analogMode ? kPadModeDualShock : kPadModeDigital;
            data[2] = static_cast<uint8_t>(state.buttons & 0xFFu);
            data[3] = static_cast<uint8_t>((state.buttons >> 8) & 0xFFu);
            data[4] = state.rx;
            data[5] = state.ry;
            data[6] = state.lx;
            data[7] = state.ly;
            data[8] = pressureValue(state, portState, kPadBtnRight);
            data[9] = pressureValue(state, portState, kPadBtnLeft);
            data[10] = pressureValue(state, portState, kPadBtnUp);
            data[11] = pressureValue(state, portState, kPadBtnDown);
            data[12] = pressureValue(state, portState, kPadBtnTriangle);
            data[13] = pressureValue(state, portState, kPadBtnCircle);
            data[14] = pressureValue(state, portState, kPadBtnCross);
            data[15] = pressureValue(state, portState, kPadBtnSquare);
            data[16] = pressureValue(state, portState, kPadBtnL1);
            data[17] = pressureValue(state, portState, kPadBtnL2);
            data[18] = pressureValue(state, portState, kPadBtnR1);
            data[19] = pressureValue(state, portState, kPadBtnR2);
        }

        bool readPadPortData(int port, int slot, PS2Runtime *runtime, uint8_t *outData, uint32_t dataAddr)
        {
            if (!outData)
            {
                return false;
            }

            PadPortState portState;
            {
                std::lock_guard<std::mutex> lock(g_padStateMutex);
                const PadPortState *sharedPortState = lookupPadPortStateLocked(port, slot);
                if (!sharedPortState || !sharedPortState->open)
                {
                    return false;
                }
                portState = *sharedPortState;
            }

            PadInputState state;
            bool useOverride = false;
            {
                std::lock_guard<std::mutex> lock(g_padOverrideMutex);
                if (g_padOverrideEnabled)
                {
                    state = g_padOverrideState;
                    useOverride = true;
                }
            }

            bool usedBackend = false;
            if (!useOverride)
            {
                uint8_t backendData[32]{};
                if (runtime && runtime->padBackend().readState(port, slot, backendData, sizeof(backendData)))
                {
                    state.buttons = static_cast<uint16_t>(backendData[2] | (backendData[3] << 8));
                    state.rx = backendData[4];
                    state.ry = backendData[5];
                    state.lx = backendData[6];
                    state.ly = backendData[7];
                    usedBackend = true;
                }
                else
                {
                    applyGamepadState(state);
                    applyKeyboardState(state, portState.analogMode);
                }
            }

            padStimOnRead(state); // E2a: no-op unless PS2X_PAD_STIM_AFTER set
            // E33: vsync-clock scripts read guest time from the GS vsync
            // tick; wall-clock scripts ignore it. Null runtime (tests) = 0.
            const uint64_t guestVsyncTick =
                runtime ? runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u;
            padScriptOnRead(state, guestVsyncTick); // E31 DEV-ONLY: no-op unless PS2X_PAD_SCRIPT set

            // IN2 DEV-ONLY PS2X_PAD_READ_LOG=1: every guest read (vsync
            // tick, final active-low buttons incl. latch + script, wall ms
            // on the padlatch epoch). ~1 line per guest frame.
            if (ps2x::padlatch::readLogEnabled())
            {
                std::fprintf(stderr, "[padread] read tick=%llu port=%d buttons=0x%04x wall=%llums\n",
                             static_cast<unsigned long long>(guestVsyncTick), port, state.buttons,
                             static_cast<unsigned long long>(ps2x::padlatch::wallMs()));
            }

            fillPadStatus(outData, state, portState);

            {
                std::lock_guard<std::mutex> lock(g_padStateMutex);
                if (PadPortState *sharedPortState = lookupPadPortStateLocked(port, slot))
                {
                    sharedPortState->lastInput = state;
                    std::memcpy(sharedPortState->lastData, outData, sizeof(sharedPortState->lastData));
                    sharedPortState->lastUsedOverride = useOverride;
                    sharedPortState->lastUsedBackend = usedBackend;
                    sharedPortState->lastReadOk = true;
                    sharedPortState->lastReadDataAddr = dataAddr;
                    ++sharedPortState->readCount;
                }
            }

            return true;
        }
    }

    void PadSyncCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void scePadEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            resetPadStateLocked();
        }
        setReturnS32(ctx, 1);
    }

    void scePadEnterPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->pressureEnabled = true;
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadExitPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->pressureEnabled = false;
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadGetButtonMask(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        const uint16_t mask = portState ? portState->buttonMask : 0xFFFFu;
        setReturnS32(ctx, static_cast<int32_t>(mask));
    }

    void scePadGetDmaStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        const uint32_t dmaAddr = portState ? portState->dmaAddr : getRegU32(ctx, 6);
        setReturnU32(ctx, dmaAddr);
    }

    void scePadGetFrameCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        static std::atomic<uint32_t> frameCount{0};
        setReturnU32(ctx, frameCount++);
    }

    void scePadGetModVersion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // Arbitrary non-zero module version.
        setReturnS32(ctx, 0x0200);
    }

    void scePadGetPortMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 2);
    }

    void scePadGetReqState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        setReturnS32(ctx, static_cast<int32_t>(portState ? portState->reqState : 0u));
    }

    void scePadGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // Most games use one slot unless multitap is active.
        setReturnS32(ctx, 1);
    }

    void scePadGetState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        int32_t state = kPadStateDisconnected;
        if (portState && portState->open)
        {
            if (portState->transientState != 0u)
            {
                state = static_cast<int32_t>(portState->transientState);
                portState->transientState = 0u;
            }
            else
            {
                state = kPadStateStable;
            }
        }
        setReturnS32(ctx, state);
    }

    void scePadInfoAct(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t act = static_cast<int32_t>(getRegU32(ctx, 6));
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        if (act < 0)
        {
            setReturnS32(ctx, 2); // small + large motors
            return;
        }
        setReturnS32(ctx, (act < 2) ? 1 : 0);
    }

    void scePadInfoComb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // No combined modes reported.
        setReturnS32(ctx, 0);
    }

    void scePadInfoMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const int32_t infoMode = static_cast<int32_t>(getRegU32(ctx, 6)); // a2
        const int32_t index = static_cast<int32_t>(getRegU32(ctx, 7));    // a3
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        const int32_t currentId = portState->analogMode ? kPadTypeDualShock : kPadTypeDigital;
        switch (infoMode)
        {
        case 1: // PAD_MODECURID
            setReturnS32(ctx, currentId);
            return;
        case 2: // PAD_MODECUREXID
            setReturnS32(ctx, currentId);
            return;
        case 3: // PAD_MODECUROFFS
            setReturnS32(ctx, 0);
            return;
        case 4: // PAD_MODETABLE
            if (index == -1)
            {
                setReturnS32(ctx, 1); // one available mode
            }
            else if (index == 0)
            {
                setReturnS32(ctx, currentId);
            }
            else
            {
                setReturnS32(ctx, 0);
            }
            return;
        default:
        {
            // P1w: unrecognized pad info mode answers canned 0.
            char dropArgs[32];
            std::snprintf(dropArgs, sizeof(dropArgs), "mode=%d", infoMode);
            ps2_log::emitDrop("stub/scePadInfoMode", "unknown-info-mode", dropArgs);
            setReturnS32(ctx, 0);
            return;
        }
        }
    }

    void scePadInfoPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        setReturnS32(ctx, (portState && portState->open) ? 1 : 0);
    }

    void scePadInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            resetPadStateLocked();
        }
        setReturnS32(ctx, 1);
    }

    void scePadInit2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        scePadInit(rdram, ctx, runtime);
    }

    void scePadPortClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->open = false;
        portState->pressureEnabled = false;
        portState->reqState = 0u;
        portState->transientState = 0u;
        setReturnS32(ctx, 1);
    }

    void scePadPortOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        padStimOnPortOpen(getRegU32(ctx, 31), static_cast<int>(getRegU32(ctx, 4)),
                          static_cast<int>(getRegU32(ctx, 5))); // E2a tripwire: 3FF708 arm
        const uint32_t dmaAddr = getRegU32(ctx, 6);
        uint8_t *dmaStr = getMemPtr(rdram, dmaAddr);
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || (dmaAddr != 0u && !dmaStr))
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->open = true;
        portState->analogMode = false;     // real pads open DIGITAL
        portState->pressureEnabled = false;
        portState->buttonMask = 0xFFFFu;
        portState->dmaAddr = dmaAddr;
        portState->reqState = 0u;
        portState->transientState = 0u;
        if (dmaStr)
        {
            ps2TraceGuestRangeWrite(rdram, dmaAddr, 32u, "scePadPortOpen", ctx);
            ps2_e3::Tap e3t = ps2_e3::tapBegin(rdram, dmaAddr, 32u); // E3b R3e E2
            std::memset(dmaStr, 0, 32);
        // E44 Part-3 EE watch (dev-only, default off).
        ps2_e44_trace::emitRangeOverlap(rdram, ctx, dmaAddr, 32u, "pad-open", 0u, false, "scePadPortOpen");
            ps2_e3::tapEnd(std::move(e3t), "pad-open", rdram, "fill=0");
            if (ps2_e41_trace::plantArmed()) // E41 plant watch
                ps2_e41_trace::notePlantRange(ps2_e41_trace::lastVsyncTick(), dmaAddr,
                                              32u, rdram, "pad-open", "zero", 0u);
        }
        setReturnS32(ctx, 1);
    }

    void scePadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const int slot = static_cast<int>(getRegU32(ctx, 5));
        const uint32_t dataAddr = getRegU32(ctx, 6);
        uint8_t *data = getMemPtr(rdram, dataAddr);
        if (!data)
        {
            setReturnS32(ctx, 0);
            return;
        }

        ps2TraceGuestRangeWrite(rdram, dataAddr, 32u, "scePadRead", ctx);
        ps2_e3::Tap e3t = ps2_e3::tapBegin(rdram, dataAddr, 32u); // E3b R3e E1
        const bool e3ok = readPadPortData(port, slot, runtime, data, dataAddr);
        // E44 Part-3 EE watch (dev-only, default off).
        ps2_e44_trace::emitRangeOverlap(rdram, ctx, dataAddr, 32u, "pad-read", 0u, false, "scePadRead");
        if (ps2_e41_trace::plantArmed()) // E41 plant watch
            ps2_e41_trace::notePlantRange(ps2_e41_trace::lastVsyncTick(), dataAddr,
                                          32u, rdram, "pad-read", "pad", 0u);
        if (e3t.active)
        {
            char e3x[64];
            std::snprintf(e3x, sizeof(e3x), "port=%d,slot=%d,ok=%d", port, slot, e3ok ? 1 : 0);
            ps2_e3::tapEnd(std::move(e3t), "pad-read", rdram, e3x);
        }
        if (!e3ok)
        {
            setReturnS32(ctx, 0);
            return;
        }

        PS2_IF_AGRESSIVE_LOGS({
            if (g_padReadLogCount < 48)
            {
                const int gamepad = findFirstGamepad();
                const bool gamepadStartPressed =
                    (gamepad >= 0) && IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT);
                const bool startPressed = (data[2] != 0xFFu || data[3] != 0xFFu ||
                                           IsKeyDown(KEY_ENTER) || gamepadStartPressed);
                if (startPressed)
                {
                    const uint32_t guestButtons =
                        (static_cast<uint32_t>(static_cast<uint8_t>(data[2] ^ 0xFFu)) << 8) |
                        static_cast<uint32_t>(static_cast<uint8_t>(data[3] ^ 0xFFu));
                    std::printf("[padread] port=%d slot=%d data2=0x%02x data3=0x%02x guestButtons=0x%04x enter=%d gamepadStart=%d\n",
                                port, slot, data[2], data[3], guestButtons,
                                IsKeyDown(KEY_ENTER) ? 1 : 0, gamepadStartPressed ? 1 : 0);
                    ++g_padReadLogCount;
                }
            }
        });

        setReturnS32(ctx, 1);
    }

    void scePadReqIntToStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t state = getRegU32(ctx, 4);
        const uint32_t strAddr = getRegU32(ctx, 5);
        char *buf = reinterpret_cast<char *>(getMemPtr(rdram, strAddr));
        if (!buf)
        {
            ps2_log::emitDrop("stub/scePadReqIntToStr", "error");
            setReturnS32(ctx, -1);
            return;
        }

        const char *text = (state == 0) ? "COMPLETE" : "BUSY";
        ps2_e3::Tap e3t = ps2_e3::tapBegin(rdram, strAddr, 32); // E3b R3e E3
        std::strncpy(buf, text, 31);
        buf[31] = '\0';
        ps2_e3::tapEnd(std::move(e3t), "pad-str", rdram, "fn=req");
        setReturnS32(ctx, 0);
    }

    void scePadSetActAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 1);
    }

    void scePadSetActDirect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 1);
    }

    void scePadSetButtonInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (portState && portState->open)
        {
            portState->buttonMask = static_cast<uint16_t>(getRegU32(ctx, 6));
            portState->reqState = 0u;
            queueExecCmdStateLocked(*portState);
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetMainMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->analogMode = (getRegU32(ctx, 6) != 0u);
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadSetReqState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (portState && portState->open)
        {
            portState->reqState = static_cast<uint32_t>(getRegU32(ctx, 6) ? 1u : 0u);
            queueExecCmdStateLocked(*portState);
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetVrefParam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 1);
    }

    void scePadSetWarningLevel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void scePadStateIntToStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t state = getRegU32(ctx, 4);
        const uint32_t strAddr = getRegU32(ctx, 5);
        char *buf = reinterpret_cast<char *>(getMemPtr(rdram, strAddr));
        if (!buf)
        {
            ps2_log::emitDrop("stub/scePadStateIntToStr", "error");
            setReturnS32(ctx, -1);
            return;
        }

        const char *text = "UNKNOWN";
        if (state == 6)
        {
            text = "STABLE";
        }
        else if (state == 1)
        {
            text = "FINDPAD";
        }
        else if (state == 5)
        {
            text = "EXECCMD";
        }
        else if (state == 0)
        {
            text = "DISCONNECTED";
        }

        ps2_e3::Tap e3t = ps2_e3::tapBegin(rdram, strAddr, 32); // E3b R3e E3
        std::strncpy(buf, text, 31);
        buf[31] = '\0';
        ps2_e3::tapEnd(std::move(e3t), "pad-str", rdram, "fn=state");
        setReturnS32(ctx, 0);
    }

    PadDebugSnapshot getPadDebugSnapshot()
    {
        PadDebugSnapshot snapshot{};
        {
            std::lock_guard<std::mutex> lock(g_padOverrideMutex);
            snapshot.overrideEnabled = g_padOverrideEnabled;
            snapshot.overrideButtons = g_padOverrideState.buttons;
            snapshot.overrideRx = g_padOverrideState.rx;
            snapshot.overrideRy = g_padOverrideState.ry;
            snapshot.overrideLx = g_padOverrideState.lx;
            snapshot.overrideLy = g_padOverrideState.ly;
        }

        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            snapshot.readLogCount = g_padReadLogCount;
            for (size_t port = 0; port < kPadDebugPortCount; ++port)
            {
                for (size_t slot = 0; slot < kPadDebugSlotCount; ++slot)
                {
                    const PadPortState &src = g_padPorts[port];
                    PadDebugPortSnapshot &dst = snapshot.ports[port][slot];
                    dst.open = src.open;
                    dst.analogMode = src.analogMode;
                    dst.pressureEnabled = src.pressureEnabled;
                    dst.lastUsedOverride = src.lastUsedOverride;
                    dst.lastUsedBackend = src.lastUsedBackend;
                    dst.lastReadOk = src.lastReadOk;
                    dst.buttonMask = src.buttonMask;
                    dst.lastButtons = src.lastInput.buttons;
                    dst.dmaAddr = src.dmaAddr;
                    dst.reqState = src.reqState;
                    dst.readCount = src.readCount;
                    dst.lastReadDataAddr = src.lastReadDataAddr;
                    dst.rx = src.lastInput.rx;
                    dst.ry = src.lastInput.ry;
                    dst.lx = src.lastInput.lx;
                    dst.ly = src.lastInput.ly;
                    std::memcpy(dst.lastData, src.lastData, sizeof(dst.lastData));
                }
            }
        }
        return snapshot;
    }

    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry)
    {
        std::lock_guard<std::mutex> lock(g_padOverrideMutex);
        g_padOverrideEnabled = true;
        g_padOverrideState.buttons = buttons;
        g_padOverrideState.lx = lx;
        g_padOverrideState.ly = ly;
        g_padOverrideState.rx = rx;
        g_padOverrideState.ry = ry;
    }

    void clearPadOverrideState()
    {
        std::lock_guard<std::mutex> lock(g_padOverrideMutex);
        g_padOverrideEnabled = false;
        g_padOverrideState = PadInputState{};
    }

    bool parsePadScript(const char *spec, std::vector<PadScriptEntry> &entries)
    {
        return padScriptParse(spec, entries);
    }

    bool setPadScriptForTest(const char *spec)
    {
        std::vector<PadScriptEntry> parsed;
        if (!padScriptParse(spec, parsed))
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(g_padScript.mutex);
        g_padScript.initDone = true;
        padScriptInstallLocked(parsed, "test");
        g_padScriptInitDone.store(true, std::memory_order_relaxed);
        return true;
    }

    void setPadScriptNowMsForTest(uint64_t nowMs)
    {
        std::lock_guard<std::mutex> lock(g_padScript.mutex);
        g_padScript.testNowSet = true;
        g_padScript.testNowMs = nowMs;
    }

    void setPadScriptVsyncClockForTest(bool vsyncClock)
    {
        std::lock_guard<std::mutex> lock(g_padScript.mutex);
        g_padScript.vsyncClock = vsyncClock;
    }

    void setPadScriptVsyncTickForTest(uint64_t tick)
    {
        std::lock_guard<std::mutex> lock(g_padScript.mutex);
        g_padScript.testVsyncSet = true;
        g_padScript.testVsyncTick = tick;
    }

    void clearPadScriptForTest()
    {
        std::lock_guard<std::mutex> lock(g_padScript.mutex);
        g_padScript.initDone = true;
        g_padScript.enabled = false;
        g_padScript.vsyncClock = false;
        g_padScript.testVsyncSet = false;
        g_padScript.testVsyncTick = 0u;
        g_padScript.entries.clear();
        g_padScript.testNowSet = false;
        g_padScript.testNowMs = 0u;
        g_padScriptArmed.store(false, std::memory_order_relaxed);
        g_padScriptInitDone.store(true, std::memory_order_relaxed);
    }
}
