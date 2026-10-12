#include "Common.h"
#include "game_overrides.h"
#include "ps2_build_id.h"
#include "ps2_fh1_full120.h"
#include "runtime/ee_guest_unwind.h"
#include "ps2_e41_trace.h"
#include "ps2_pad_latch.h"
#include "ps2_input_diag.h"
#include "ps2_record_env.h"
#include "ps2_ssx3_course_manifest.h"
#include "runtime/ps2_savestate.h"
#include "Pad.h"
#include "ps2_log.h"
#include "ps2_rpl3_nv.h" // RPL3: recorded host values (padrec v3)
#include "ps2_turbo_boot.h" // BTS1: boot turbo input yield

#include <algorithm>
#include <cmath>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
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
        // SS1: scePadGetFrameCount's counter (guest-visible), at file scope
        // so a save state can carry it.
        std::atomic<uint32_t> g_padFrameCount{0};
        int g_padReadLogCount = 0;

        // E31 DEV-ONLY scripted pad input (PS2X_PAD_SCRIPT). Unset/empty
        // (default) = one relaxed atomic check per pad read, zero behavior
        // change. Format: "t_ms:spec:hold_ms,..." (see parsePadScript).
        // RP2: a spec starting with '@' loads the script from that path
        // instead (same grammar); a missing/unreadable/malformed file is
        // FATAL, never idle. Each entry presses its buttons and/or drives
        // its analog axes while
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
            // FH17: events mode reads the FH5 stock-time accumulator (half a
            // tick late while active) for '# padrec v1' recordings, so they
            // replay as recorded; everything else reads the exact one.
            bool legacyEventsClock = false;
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
            // IR1b: full-line '#' comments and blank lines are ignored, so
            // recordings (which carry a header block) parse as scripts.
            // Entry text itself is unchanged: no '#' or newline may appear
            // inside an entry. A trailing '\r' per line is tolerated.
            std::string text;
            {
                const std::string raw(spec);
                size_t lineBegin = 0u;
                while (lineBegin <= raw.size())
                {
                    const size_t lineEnd = raw.find('\n', lineBegin);
                    std::string line = raw.substr(
                        lineBegin, lineEnd == std::string::npos ? lineEnd : lineEnd - lineBegin);
                    if (!line.empty() && line.back() == '\r')
                    {
                        line.pop_back();
                    }
                    if (!line.empty() && line[0] != '#')
                    {
                        text += line;
                    }
                    if (lineEnd == std::string::npos)
                    {
                        break;
                    }
                    lineBegin = lineEnd + 1u;
                }
            }
            if (text.empty())
            {
                return false;
            }
            size_t begin = 0u;
            while (begin <= text.size())
            {
                const size_t end = text.find(',', begin);
                const std::string item = text.substr(begin, end == std::string::npos ? end : end - begin);
                // RP2/GQ1: a trailing comma leaves an empty tail item; skip
                // it instead of rejecting the whole script (a non-trailing
                // empty item still falls through to the malformed return).
                if (item.empty() && end == std::string::npos)
                {
                    break;
                }
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
        uint64_t padScriptVsyncTickToMs(uint64_t tick, bool legacyEventsClock = false)
        {
            // FH1: full120 guest VBlanks are half-periods; keep stock ms.
            // FH5 events mode: the stock-time accumulator (half-periods).
            if (ps2_fh1::eventsMode())
                return ((legacyEventsClock ? ps2_fh1::stockHalfTicksLegacy() : ps2_fh1::stockHalfTicks()) *
                        100000ull) / (5994ull * 2ull);
            return (tick * 100000ull) / (5994ull * ps2_fh1::vblankDivisor());
        }

        // FH17: a recording stamped on the FH5 events clock keeps that clock
        // on replay. Such a file is a '# padrec v1' recording made in events
        // mode; v1 carries no full120 knob, so the stamps tell: only a 120 Hz
        // read lands on an odd stock half-tick, and FH5 events recordings put
        // about half their rows there (PB11: 2472 of 4990), stock ones none
        // (FV1/FH1 cuts: 0-1; SPR1 retired split120). PS2X_PAD_SCRIPT_EVENTS_CLOCK=
        // fh5|exact overrides this for any script.
        bool padScriptWantsLegacyEventsClock(const std::string &text, const std::vector<PadScriptEntry> &parsed)
        {
            if (const char *v = std::getenv("PS2X_PAD_SCRIPT_EVENTS_CLOCK"))
            {
                if (std::strcmp(v, "fh5") == 0)
                    return true;
                if (std::strcmp(v, "exact") == 0)
                    return false;
            }
            if (text.rfind("# padrec v1\n", 0) != 0 && text.rfind("# padrec v1\r\n", 0) != 0)
                return false;
            size_t odd = 0u;
            for (const PadScriptEntry &e : parsed)
            {
                // Smallest half-tick h with floor(h * 100000 / 11988) >= atMs.
                const uint64_t h = (e.atMs * 11988ull + 99999ull) / 100000ull;
                if ((h * 100000ull) / 11988ull == e.atMs && (h & 1u) != 0u)
                    ++odd;
            }
            return odd >= 8u && odd * 10u >= parsed.size();
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
            std::fprintf(stderr, "[padscript] armed n=%llu source=%s clock=%s events_clock=%s\n",
                         static_cast<unsigned long long>(g_padScript.entries.size()), source,
                         g_padScript.vsyncClock ? "vsync" : "wall",
                         g_padScript.legacyEventsClock ? "fh5" : "exact");
        }

        // RP2: reads a '@'-file script into `out`. False on any failure
        // (missing, unreadable, short read); the caller reports loudly.
        bool padScriptReadFile(const char *path, std::string &out)
        {
            if (!path || path[0] == '\0')
            {
                return false;
            }
            std::FILE *f = std::fopen(path, "rb");
            if (!f)
            {
                return false;
            }
            std::string content;
            char buf[65536];
            size_t n = 0;
            bool ok = true;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            {
                content.append(buf, n);
            }
            if (std::ferror(f))
            {
                ok = false;
            }
            std::fclose(f);
            if (!ok)
            {
                return false;
            }
            out = content;
            return true;
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
            // RP2: '@/path' loads the script from a file, parsed with the
            // same grammar as inline specs. A file that cannot be read, or
            // whose content does not parse, is FATAL: silently idling here
            // replayed PG3's brad leg as no input at all.
            if (spec[0] == '@')
            {
                const char *path = spec + 1;
                std::string content;
                if (!padScriptReadFile(path, content))
                {
                    std::fprintf(stderr,
                                 "FATAL: [padscript] cannot read PS2X_PAD_SCRIPT file '%s'\n",
                                 path);
                    std::abort();
                }
                std::vector<PadScriptEntry> parsed;
                if (!padScriptParse(content.c_str(), parsed))
                {
                    std::fprintf(stderr,
                                 "FATAL: [padscript] malformed PS2X_PAD_SCRIPT file '%s'\n",
                                 path);
                    std::abort();
                }
                g_padScript.legacyEventsClock = padScriptWantsLegacyEventsClock(content, parsed);
                padScriptInstallLocked(parsed, "file");
                return;
            }
            std::vector<PadScriptEntry> parsed;
            if (!padScriptParse(spec, parsed))
            {
                std::fprintf(stderr, "[padscript] ignoring malformed PS2X_PAD_SCRIPT\n");
                return;
            }
            g_padScript.legacyEventsClock = padScriptWantsLegacyEventsClock(spec, parsed);
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
                return padScriptVsyncTickToMs(tick, g_padScript.legacyEventsClock);
            }
            const auto now = std::chrono::steady_clock::now();
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - g_padScript.startWall).count());
        }

        // APH1 dev harness (same class as PS2X_PAD_SCRIPT, default off):
        // closed-loop steering along a route so a QA boot can ride a whole
        // race. Off unless PS2X_TK12_AP_ROUTE names a file of "x y" lines
        // (world XY, +Z up). Rider R = [PS2X_TK12_AP_PTR], position R+0x110.
        // Active on guest vsync ticks [PS2X_TK12_AP_FROM, PS2X_TK12_AP_TO];
        // drives lx (proportional) and d-pad left/right (|err| > DPAD deg)
        // toward the route point LOOK units ahead of the rider's projection.
        // Left turns the XY heading counter-clockwise (memory: ssx3 rider).
        struct Tk12Autopilot
        {
            bool init = false;
            bool on = false;
            bool done = false;
            uint32_t ptr = 0x53FF4Cu;
            uint64_t from = 0;
            uint64_t to = ~0ull;
            uint64_t logEvery = 30;
            uint64_t lastLog = 0;
            double look = 1500.0;
            double fullDeg = 35.0;
            double dpadDeg = 10.0;
            std::vector<double> xs, ys, zs, cum;
            bool has3d = false;
            size_t idx = 0;
            bool haveAnchor = false;
            double ax = 0.0, ay = 0.0;
            bool haveHeading = false;
            double heading = 0.0;
        };
        Tk12Autopilot g_tk12;
        std::mutex g_tk12Mutex;

        void tk12Init()
        {
            g_tk12.init = true;
            const char *route = std::getenv("PS2X_TK12_AP_ROUTE");
            if (!route || !*route)
                return;
            auto envU = [](const char *k, uint64_t &out) {
                if (const char *v = std::getenv(k)) out = std::strtoull(v, nullptr, 0);
            };
            auto envD = [](const char *k, double &out) {
                if (const char *v = std::getenv(k)) out = std::strtod(v, nullptr);
            };
            uint64_t ptr = g_tk12.ptr;
            envU("PS2X_TK12_AP_PTR", ptr);
            g_tk12.ptr = static_cast<uint32_t>(ptr);
            envU("PS2X_TK12_AP_FROM", g_tk12.from);
            envU("PS2X_TK12_AP_TO", g_tk12.to);
            envU("PS2X_TK12_AP_LOG_EVERY", g_tk12.logEvery);
            envD("PS2X_TK12_AP_LOOK", g_tk12.look);
            envD("PS2X_TK12_AP_FULL_DEG", g_tk12.fullDeg);
            envD("PS2X_TK12_AP_DPAD_DEG", g_tk12.dpadDeg);
            std::FILE *f = std::fopen(route, "r");
            if (!f)
            {
                std::fprintf(stderr, "[tk12-ap] cannot open route '%s'\n", route);
                return;
            }
            char line[256];
            while (std::fgets(line, sizeof(line), f))
            {
                double x = 0, y = 0, z = 0;
                const int n = std::sscanf(line, "%lf %lf %lf", &x, &y, &z);
                if (n < 2)
                    continue;
                g_tk12.xs.push_back(x);
                g_tk12.ys.push_back(y);
                g_tk12.zs.push_back(z);
                g_tk12.has3d = g_tk12.has3d || n == 3;
            }
            std::fclose(f);
            if (g_tk12.xs.size() < 2)
            {
                std::fprintf(stderr, "[tk12-ap] route '%s' has < 2 points\n", route);
                return;
            }
            g_tk12.cum.assign(g_tk12.xs.size(), 0.0);
            for (size_t i = 1; i < g_tk12.xs.size(); ++i)
                g_tk12.cum[i] = g_tk12.cum[i - 1] + std::hypot(g_tk12.xs[i] - g_tk12.xs[i - 1], g_tk12.ys[i] - g_tk12.ys[i - 1]);
            g_tk12.on = true;
            std::fprintf(stderr, "[tk12-ap] armed: %zu points, length %.0f, ptr 0x%x, ticks %llu..%llu, look %.0f\n",
                         g_tk12.xs.size(), g_tk12.cum.back(), g_tk12.ptr,
                         static_cast<unsigned long long>(g_tk12.from), static_cast<unsigned long long>(g_tk12.to), g_tk12.look);
        }

        void tk12OnRead(PadInputState &state, uint64_t tick, PS2Runtime *runtime, int port)
        {
            std::lock_guard<std::mutex> lock(g_tk12Mutex);
            if (!g_tk12.init)
                tk12Init();
            if (!g_tk12.on || g_tk12.done || port != 0 || !runtime || tick < g_tk12.from || tick > g_tk12.to)
                return;
            uint8_t *rdram = runtime->memory().getRDRAM();
            if (!rdram)
                return;
            uint32_t r = 0;
            std::memcpy(&r, rdram + (g_tk12.ptr & PS2_RAM_MASK), 4);
            if (r == 0 || (r & PS2_RAM_MASK) + 0x120u > PS2_RAM_SIZE)
                return;
            float p[3];
            std::memcpy(p, rdram + ((r + 0x110u) & PS2_RAM_MASK), sizeof(p));
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || std::fabs(p[0]) > 2e6f || std::fabs(p[1]) > 2e6f ||
                (p[0] == 0.0f && p[1] == 0.0f))
                return;
            const double px = p[0], py = p[1], pz = p[2];
            // Heading from motion (anchor refreshed every >= 20 units; a jump > 3000 is a reset).
            if (!g_tk12.haveAnchor)
            {
                g_tk12.ax = px; g_tk12.ay = py; g_tk12.haveAnchor = true;
            }
            else
            {
                const double d = std::hypot(px - g_tk12.ax, py - g_tk12.ay);
                if (d > 3000.0)
                {
                    g_tk12.ax = px; g_tk12.ay = py; g_tk12.haveHeading = false;
                }
                else if (d >= 20.0)
                {
                    g_tk12.heading = std::atan2(py - g_tk12.ay, px - g_tk12.ax);
                    g_tk12.haveHeading = true;
                    g_tk12.ax = px; g_tk12.ay = py;
                }
            }
            // Project onto the route near the last index (full search when far off).
            const auto &xs = g_tk12.xs, &ys = g_tk12.ys, &zs = g_tk12.zs, &cum = g_tk12.cum;
            const size_t nseg = xs.size() - 1;
            auto project = [&](size_t lo, size_t hi, double &bestD, double &bestS, size_t &bestI) {
                bestD = 1e30;
                for (size_t i = lo; i < hi && i < nseg; ++i)
                {
                    const double dx = xs[i + 1] - xs[i], dy = ys[i + 1] - ys[i];
                    const double l2 = dx * dx + dy * dy;
                    double t = l2 > 0 ? ((px - xs[i]) * dx + (py - ys[i]) * dy) / l2 : 0.0;
                    t = t < 0 ? 0 : (t > 1 ? 1 : t);
                    // 3D distance when the route has z (stacked course sections, e.g. Aloha).
                    const double dz = g_tk12.has3d ? pz - (zs[i] + t * (zs[i + 1] - zs[i])) : 0.0;
                    const double d = std::sqrt(std::pow(px - (xs[i] + t * dx), 2) + std::pow(py - (ys[i] + t * dy), 2) + dz * dz);
                    if (d < bestD) { bestD = d; bestS = cum[i] + t * std::sqrt(l2); bestI = i; }
                }
            };
            double dist = 0, s = 0;
            size_t bi = g_tk12.idx;
            project(g_tk12.idx > 10 ? g_tk12.idx - 10 : 0, g_tk12.idx + 60, dist, s, bi);
            if (dist > 4000.0)
                project(0, nseg, dist, s, bi);
            g_tk12.idx = bi;
            const double total = cum.back();
            if (s >= total - 1.0)
            {
                g_tk12.done = true;
                std::fprintf(stderr, "[tk12-ap] route end at tick %llu pos (%.1f, %.1f, %.1f)\n",
                             static_cast<unsigned long long>(tick), px, py, static_cast<double>(p[2]));
                return;
            }
            const double want = std::min(total, s + g_tk12.look);
            size_t j = bi;
            while (j + 1 < nseg && cum[j + 1] < want) ++j;
            const double segLen = cum[j + 1] - cum[j];
            const double tt = segLen > 0 ? (want - cum[j]) / segLen : 0.0;
            const double tx = xs[j] + tt * (xs[j + 1] - xs[j]), ty = ys[j] + tt * (ys[j + 1] - ys[j]);
            double err = 0.0;
            uint8_t lx = kPadAnalogCenter;
            if (g_tk12.haveHeading)
            {
                err = std::atan2(ty - py, tx - px) - g_tk12.heading;
                while (err > M_PI) err -= 2 * M_PI;
                while (err < -M_PI) err += 2 * M_PI;
                const double deg = err * 180.0 / M_PI;
                double u = deg / g_tk12.fullDeg;
                u = u > 1 ? 1 : (u < -1 ? -1 : u);
                lx = static_cast<uint8_t>(std::lround(128.0 - u * 127.0));
                state.lx = lx;
                if (deg > g_tk12.dpadDeg)
                    state.buttons = static_cast<uint16_t>(state.buttons & ~kPadBtnLeft);
                else if (deg < -g_tk12.dpadDeg)
                    state.buttons = static_cast<uint16_t>(state.buttons & ~kPadBtnRight);
            }
            if (g_tk12.logEvery && tick >= g_tk12.lastLog + g_tk12.logEvery)
            {
                g_tk12.lastLog = tick;
                std::fprintf(stderr, "[tk12-ap] t=%llu pos=(%.1f,%.1f,%.1f) s=%.0f/%.0f off=%.0f err=%.1f lx=%u\n",
                             static_cast<unsigned long long>(tick), px, py, static_cast<double>(p[2]), s, total, dist,
                             err * 180.0 / M_PI, lx);
            }
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

        // IR1 DEV-ONLY pad recorder (PS2X_PAD_RECORD=<path>). Unset/empty
        // (default) = one relaxed atomic check per pad read, zero behavior
        // change. When armed, each port-0 pad read appends the effective
        // guest-visible state (post stimulation/script, pre read-log) keyed
        // by guest vsync tick, emitted directly as PS2X_PAD_SCRIPT
        // (vsync-clock) entries: one entry per state change, contiguous and
        // non-overlapping, each carrying the full button mask plus all four
        // analog bytes, so replay reproduces every recorded tick exactly
        // (tick ms are strictly increasing, so [ms(T0),ms(T1)) covers the
        // recorded ticks [T0,T1) exactly). The open tail entry is split
        // every 600 ticks and the file flushed, so a force-stop keeps all
        // but ~10 s; a clean exit finalizes the tail via atexit.
        // IR1b: PS2X_PAD_RECORD_DIR=<dir> (when PAD_RECORD is unset) writes
        // one file per session (padrec-<UTC>.txt) and prunes to the newest
        // PS2X_PAD_RECORD_KEEP files (default 30) at arm time. Every
        // recording starts with a '#' header block (start UTC, knobs, save
        // hash); the script parser skips full-line comments, so recordings
        // replay directly.
        // PL1: dir mode also snapshots the effective port-0 card root into
        // a sibling padrec-<UTC>.mc/ dir at arm time (<= 16 MiB, else a
        // header note), pruned together with its recording; the header
        // carries build/env_sha plus the snapshot's name and hash, so any
        // session replays standalone via padrec_replay_env.py.
        enum class PadRecordSnap
        {
            None, // file mode, or no card root resolved
            Ok,
            TooLarge, // over the 16 MiB bound (no partial dir kept)
            Unreadable, // not a dir, or an I/O error mid-copy
        };
        struct PadRecord
        {
            std::mutex mutex;
            bool initDone = false;
            bool enabled = false;
            bool capped = false;
            bool finalized = false;
            std::FILE *file = nullptr;
            std::string path;
            bool haveOpen = false;
            uint64_t openTick = 0u;
            uint64_t openMs = 0u; // script-clock ms of openTick (PR3)
            PadInputState openState{};
            uint64_t lastTick = 0u;
            uint64_t lastNextMs = 0u; // script-clock ms of lastTick + 1 (PR3)
            bool firstEntry = true;
            uint64_t lastFlushTick = 0u;
            uint64_t entriesSinceFlush = 0u;
            uint64_t totalEntries = 0u;
            bool testTickSet = false;
            uint64_t testTick = 0u;
            std::string snapRoot; // effective port-0 card root ("" = none)
            std::string snapDirName; // sibling dir leaf ("padrec-<UTC>.mc")
            uint64_t snapHash = 0u; // FNV-1a 64 over relpaths + copied bytes
            uint64_t snapFiles = 0u;
            PadRecordSnap snapStatus = PadRecordSnap::None;
            // DS1: dir-mode re-arm state (a mid-session load opens a new
            // session file in the same dir) + the one-shot load note the
            // next header carries.
            bool dirMode = false;
            std::string dir;
            uint64_t keep = 30u;
            bool haveLoadNote = false;
            uint64_t loadTickNote = 0u;
        };
        PadRecord g_padRecord;
        std::atomic<bool> g_padRecordInitDone{false};
        std::atomic<bool> g_padRecordArmed{false};

        constexpr uint64_t kPadRecordSplitTicks = 600u;
        constexpr uint64_t kPadRecordFlushEntries = 64u;
        constexpr uint64_t kPadRecordMaxEntries = 1000000u;

        struct PadRecordButtonName
        {
            uint16_t mask;
            const char *name;
        };
        constexpr PadRecordButtonName kPadRecordButtons[] = {
            {kPadBtnSelect, "select"}, {kPadBtnL3, "l3"},
            {kPadBtnR3, "r3"}, {kPadBtnStart, "start"},
            {kPadBtnUp, "up"}, {kPadBtnRight, "right"},
            {kPadBtnDown, "down"}, {kPadBtnLeft, "left"},
            {kPadBtnL2, "l2"}, {kPadBtnR2, "r2"},
            {kPadBtnL1, "l1"}, {kPadBtnR1, "r1"},
            {kPadBtnTriangle, "triangle"}, {kPadBtnCircle, "circle"},
            {kPadBtnCross, "cross"}, {kPadBtnSquare, "square"},
        };

        bool padRecordSameState(const PadInputState &a, const PadInputState &b)
        {
            return a.buttons == b.buttons && a.lx == b.lx && a.ly == b.ly &&
                   a.rx == b.rx && a.ry == b.ry;
        }

        // PR3: the script-clock ms of a read at `tick` (next: of tick + 1),
        // sampled at read time. FH5 events mode keeps the script clock on the
        // stock-time accumulator, which only knows "now": converting ticks
        // later (at emit) gave every span at == end and dropped every row.
        uint64_t padRecordReadMs(uint64_t tick, bool next)
        {
            if (ps2_fh1::eventsMode() && !g_padRecord.testTickSet)
            {
                const uint64_t half = ps2_fh1::stockHalfTicks() + (next ? ps2_fh1::stockHalfStepNext() : 0u);
                return (half * 100000ull) / (5994ull * 2ull);
            }
            return padScriptVsyncTickToMs(next ? tick + 1u : tick);
        }

        void padRecordEmitLocked(uint64_t atMs, uint64_t endMs)
        {
            if (!g_padRecord.file)
            {
                return;
            }
            if (endMs <= atMs)
            {
                // Cannot happen (stock ms strictly increase per tick); loud
                // so an empty recording never goes unnoticed again (PR3).
                static uint32_t lines = 0u;
                if (lines++ < 8u)
                    std::fprintf(stderr, "[padrecord] dropped empty span at=%llums end=%llums\n",
                                 static_cast<unsigned long long>(atMs),
                                 static_cast<unsigned long long>(endMs));
                return;
            }
            std::string spec;
            const uint16_t pressed = static_cast<uint16_t>(~g_padRecord.openState.buttons);
            for (const PadRecordButtonName &b : kPadRecordButtons)
            {
                if ((pressed & b.mask) != 0u)
                {
                    if (!spec.empty())
                    {
                        spec += '+';
                    }
                    spec += b.name;
                }
            }
            // Full analog state every entry: replay sets exact bytes.
            char axes[48];
            std::snprintf(axes, sizeof(axes), "lx=%u+ly=%u+rx=%u+ry=%u",
                          g_padRecord.openState.lx, g_padRecord.openState.ly,
                          g_padRecord.openState.rx, g_padRecord.openState.ry);
            if (!spec.empty())
            {
                spec += '+';
            }
            spec += axes;
            std::fprintf(g_padRecord.file, "%s%llu:%s:%llu",
                         g_padRecord.firstEntry ? "" : ",",
                         static_cast<unsigned long long>(atMs), spec.c_str(),
                         static_cast<unsigned long long>(endMs - atMs));
            g_padRecord.firstEntry = false;
            ++g_padRecord.totalEntries;
            ++g_padRecord.entriesSinceFlush;
        }

        void padRecordFinalizeLocked()
        {
            if (g_padRecord.finalized)
            {
                return;
            }
            g_padRecord.finalized = true;
            if (!g_padRecord.file)
            {
                return;
            }
            if (g_padRecord.haveOpen && !g_padRecord.capped)
            {
                // Tail: cover through the last observed read tick.
                padRecordEmitLocked(g_padRecord.openMs, g_padRecord.lastNextMs);
                g_padRecord.haveOpen = false;
            }
            std::fflush(g_padRecord.file);
            std::fclose(g_padRecord.file);
            g_padRecord.file = nullptr;
            std::fprintf(stderr, "[padrecord] finalized entries=%llu lastTick=%llu\n",
                         static_cast<unsigned long long>(g_padRecord.totalEntries),
                         static_cast<unsigned long long>(g_padRecord.lastTick));
        }

        void padRecordExitFlush()
        {
            // Best-effort tail finalize on clean exit; a force-stop keeps
            // everything flushed so far. try_to_lock: never block exit.
            std::unique_lock<std::mutex> lock(g_padRecord.mutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                return;
            }
            padRecordFinalizeLocked();
        }

        const char *padRecordKnob(const char *name)
        {
            const char *value = std::getenv(name);
            return (value && value[0] != '\0') ? value : "unset";
        }

        void padRecordSanitize(char *text)
        {
            // Keep header lines single-line: no newlines or returns.
            for (char *p = text; *p; ++p)
            {
                if (*p == '\n' || *p == '\r')
                {
                    *p = '_';
                }
            }
        }

        void padRecordUtc(char *out, size_t outSize, std::time_t when, const char *fmt)
        {
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &when);
#else
            gmtime_r(&when, &tm);
#endif
            std::strftime(out, outSize, fmt, &tm);
        }

        // RPL3: recorded host values ("# NV ..." lines, padrec v3). The guest
        // reads a handful of host values (CD clock bytes, card dates, the OSD
        // timezone); the recorder notes each served value and replay serves
        // it back in order. Notes before the padrec arms (boot RTC reads run
        // before the first pad read) buffer in pending and flush in the
        // header; later notes append inline with a flush per line, so a
        // force-stop loses none. Replay loads lazily from the PS2X_PAD_SCRIPT
        // @file at the first take* call. Lock rule: g_rpl3Mutex is never held
        // while taking g_padRecord.mutex (drains take record outer, rpl3
        // inner, briefly).
        struct Rpl3McDateSet
        {
            uint16_t v[6]{};
        };
        struct Rpl3SchedGate
        {
            uint64_t cycle = 0u;
            std::vector<ps2_rpl3::SchedEvent> fired;
        };
        struct Rpl3State
        {
            bool recordEnvChecked = false;
            bool recordWanted = false;
            uint64_t rtcSeq = 0u;
            uint64_t mcSeq = 0u;
            uint64_t tzSeq = 0u;
            uint64_t schedSeq = 0u;
            uint64_t idleSeq = 0u;
            std::vector<std::string> pending;
            bool replayLoaded = false;
            struct RtcBytes
            {
                uint8_t b[8]{};
            };
            std::vector<RtcBytes> rtc;
            size_t rtcCur = 0u;
            std::vector<Rpl3McDateSet> mc;
            size_t mcCur = 0u;
            uint64_t mcReplayCalls = 0u;
            bool haveTz = false;
            int tz = 0;
            std::vector<Rpl3SchedGate> sched;
            size_t schedCur = 0u;
            std::vector<ps2_rpl3::IdleGate> idle;
            size_t idleCur = 0u;
            uint64_t ignored = 0u;
            bool rtcExhaustWarned = false;
            bool mcExhaustWarned = false;
            bool seqWarned = false;
            bool schedStaleWarned = false;
            bool schedMismatchWarned = false;
            bool idleStaleWarned = false;
            bool idleMismatchWarned = false;
        };
        Rpl3State g_rpl3;
        std::mutex g_rpl3Mutex;

        bool rpl3RecordWantedLocked()
        {
            if (!g_rpl3.recordEnvChecked)
            {
                g_rpl3.recordEnvChecked = true;
                const char *rec = std::getenv("PS2X_PAD_RECORD");
                const char *dir = std::getenv("PS2X_PAD_RECORD_DIR");
                g_rpl3.recordWanted = (rec && rec[0] != '\0') || (dir && dir[0] != '\0');
            }
            // Armed covers the test seam (which arms without env).
            return g_rpl3.recordWanted || g_padRecordArmed.load(std::memory_order_relaxed);
        }

        std::string rpl3FormatTick(uint64_t tick)
        {
            if (tick == ps2_rpl3::kTickUnknown)
                return "*";
            return std::to_string(tick);
        }

        std::string rpl3SanitizeQuery(const char *query)
        {
            std::string out = query ? query : "";
            if (out.size() > 128u)
                out.resize(128u);
            for (char &c : out)
            {
                const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-' ||
                                c == '.' || c == '*' || c == '?';
                if (!ok)
                    c = '_';
            }
            return out;
        }

        void rpl3EmitLine(std::string line)
        {
            {
                std::lock_guard<std::mutex> lock(g_rpl3Mutex);
                if (!rpl3RecordWantedLocked())
                    return;
                g_rpl3.pending.push_back(std::move(line));
            }
            std::lock_guard<std::mutex> recordLock(g_padRecord.mutex);
            if (!g_padRecordArmed.load(std::memory_order_relaxed) || !g_padRecord.file)
                return; // stays pending for the header flush
            std::vector<std::string> drain;
            {
                std::lock_guard<std::mutex> lock(g_rpl3Mutex);
                drain.swap(g_rpl3.pending);
            }
            // Leading newline: the stream writer leaves the file mid-line
            // between splits, and a '#' glued mid-line would corrupt the
            // script. Blank lines are skipped by both parsers.
            for (const std::string &l : drain)
                std::fprintf(g_padRecord.file, "\n%s\n", l.c_str());
            std::fflush(g_padRecord.file);
        }

        void rpl3FlushPendingToFile()
        {
            if (!g_padRecord.file)
                return;
            std::vector<std::string> drain;
            {
                std::lock_guard<std::mutex> lock(g_rpl3Mutex);
                drain.swap(g_rpl3.pending);
            }
            for (const std::string &l : drain)
                std::fprintf(g_padRecord.file, "%s\n", l.c_str());
        }

        bool rpl3ParseHex8(const char *p, uint8_t out[8])
        {
            for (int i = 0; i < 8; ++i)
            {
                unsigned v = 0u;
                for (int k = 0; k < 2; ++k)
                {
                    const char c = *p++;
                    v <<= 4;
                    if (c >= '0' && c <= '9')
                        v |= static_cast<unsigned>(c - '0');
                    else if (c >= 'a' && c <= 'f')
                        v |= static_cast<unsigned>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F')
                        v |= static_cast<unsigned>(c - 'A' + 10);
                    else
                        return false;
                }
                out[i] = static_cast<uint8_t>(v);
            }
            return *p == '\0' || *p == ' ' || *p == '\r' || *p == '\n';
        }

        void rpl3CheckSeq(const char *kind, uint64_t seq, size_t have)
        {
            if (seq != have && !g_rpl3.seqWarned)
            {
                g_rpl3.seqWarned = true;
                std::fprintf(stderr, "[rpl3] %s seq=%llu but %zu loaded; serving in file order\n",
                             kind, static_cast<unsigned long long>(seq), have);
            }
        }

        void rpl3ParseNvLine(const std::string &line)
        {
            const char *p = line.c_str() + 5; // caller checked the "# NV " prefix
            if (std::strncmp(p, "rtc ", 4) == 0)
            {
                unsigned long long seq = 0u;
                char tick[32] = {0};
                unsigned ra = 0u;
                char bytes[32] = {0};
                if (std::sscanf(p, "rtc seq=%llu tick=%31s ra=%x bytes=%31s", &seq, tick, &ra,
                                bytes) != 4)
                {
                    ++g_rpl3.ignored;
                    return;
                }
                uint8_t b[8]{};
                if (!rpl3ParseHex8(bytes, b))
                {
                    ++g_rpl3.ignored;
                    return;
                }
                rpl3CheckSeq("rtc", seq, g_rpl3.rtc.size());
                Rpl3State::RtcBytes rec{};
                std::memcpy(rec.b, b, sizeof(rec.b));
                g_rpl3.rtc.push_back(rec);
                return;
            }
            if (std::strncmp(p, "tz ", 3) == 0)
            {
                unsigned long long seq = 0u;
                char tick[32] = {0};
                int mins = 0;
                if (std::sscanf(p, "tz seq=%llu tick=%31s mins=%d", &seq, tick, &mins) != 3)
                {
                    ++g_rpl3.ignored;
                    return;
                }
                if (g_rpl3.haveTz)
                {
                    ++g_rpl3.ignored; // first wins; the OSD inits once
                    return;
                }
                g_rpl3.haveTz = true;
                g_rpl3.tz = mins;
                return;
            }
            if (std::strncmp(p, "mc ", 3) == 0)
            {
                unsigned long long seq = 0u;
                char tick[32] = {0};
                char query[136] = {0};
                unsigned long long built = 0u, served = 0u, n = 0u;
                int epos = 0;
                if (std::sscanf(p, "mc seq=%llu tick=%31s q='%135[^']' built=%llu served=%llu n=%llu e=%n",
                                &seq, tick, query, &built, &served, &n, &epos) != 6 ||
                    epos <= 0 || n > 100000u)
                {
                    ++g_rpl3.ignored;
                    return;
                }
                rpl3CheckSeq("mc", seq, g_rpl3.mcReplayCalls);
                const char *e = p + epos;
                std::vector<Rpl3McDateSet> sets;
                const char *cur = e;
                for (unsigned long long i = 0u; i < n; ++i)
                {
                    const char *sep = std::strchr(cur, ';');
                    const size_t len = sep ? static_cast<size_t>(sep - cur) : std::strlen(cur);
                    if (len == 0u || len > 128u)
                        break;
                    char group[136] = {0};
                    std::memcpy(group, cur, len);
                    unsigned f[12] = {0u};
                    if (std::sscanf(group, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", &f[0], &f[1],
                                    &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8], &f[9],
                                    &f[10], &f[11]) != 12)
                        break;
                    bool rangeOk = true;
                    for (unsigned k = 0u; k < 12u; ++k)
                        rangeOk = rangeOk && f[k] <= 65535u;
                    if (!rangeOk)
                        break;
                    Rpl3McDateSet c{}, m{};
                    for (unsigned k = 0u; k < 6u; ++k)
                    {
                        c.v[k] = static_cast<uint16_t>(f[k]);
                        m.v[k] = static_cast<uint16_t>(f[6u + k]);
                    }
                    sets.push_back(c);
                    sets.push_back(m);
                    if (!sep)
                    {
                        cur += len;
                        break;
                    }
                    cur = sep + 1;
                }
                if (sets.size() != n * 2u)
                {
                    ++g_rpl3.ignored;
                    return;
                }
                g_rpl3.mc.insert(g_rpl3.mc.end(), sets.begin(), sets.end());
                ++g_rpl3.mcReplayCalls;
                return;
            }
            if (std::strncmp(p, "sched ", 6) == 0)
            {
                unsigned long long seq = 0u, cycle = 0u, nf = 0u, w = 0u;
                char tick[32] = {0};
                int fpos = 0;
                if (std::sscanf(p, "sched seq=%llu tick=%31s cycle=%llu nf=%llu w=%llu f=%n",
                                &seq, tick, &cycle, &nf, &w, &fpos) != 5 ||
                    fpos <= 0 || nf > 64u)
                {
                    ++g_rpl3.ignored;
                    return;
                }
                rpl3CheckSeq("sched", seq, g_rpl3.sched.size());
                Rpl3SchedGate gate{};
                gate.cycle = cycle;
                const char *cur = p + fpos;
                bool ok = true;
                for (unsigned long long i = 0u; i < nf; ++i)
                {
                    // Triple: <mnemonic>,<id>,<cycle>.
                    const char m = cur[0];
                    uint32_t type = 0u;
                    unsigned long long id = 0u, c = 0u;
                    int nch = 0;
                    if (cur[0] == '\0' || cur[1] != ',' ||
                        !ps2_rpl3::schedCharType(m, type) ||
                        std::sscanf(cur + 2, "%llu,%llu%n", &id, &c, &nch) != 2 || nch <= 0 ||
                        id > 0xFFFFFFFFu)
                    {
                        ok = false;
                        break;
                    }
                    ps2_rpl3::SchedEvent ev{};
                    ev.type = type;
                    ev.id = static_cast<uint32_t>(id);
                    ev.cycle = c;
                    gate.fired.push_back(ev);
                    cur += 2 + nch;
                    if (i + 1u < nf)
                    {
                        if (*cur != ';')
                        {
                            ok = false;
                            break;
                        }
                        ++cur;
                    }
                }
                if (!ok || gate.fired.size() != nf)
                {
                    ++g_rpl3.ignored;
                    return;
                }
                g_rpl3.sched.push_back(std::move(gate));
                return;
            }
            if (std::strncmp(p, "idlerace ", 9) == 0)
            {
                unsigned long long seq = 0u, cycle = 0u, timer = 0u, event = 0u;
                char tick[32] = {0};
                char winner = 0;
                int agree = -1;
                if (std::sscanf(p,
                                "idlerace seq=%llu tick=%31s cycle=%llu timer=%llu event=%llu "
                                "winner=%c agree=%d",
                                &seq, tick, &cycle, &timer, &event, &winner, &agree) != 7 ||
                    (winner != 'T' && winner != 'E') || (agree != 0 && agree != 1))
                {
                    ++g_rpl3.ignored;
                    return;
                }
                rpl3CheckSeq("idlerace", seq, g_rpl3.idle.size());
                ps2_rpl3::IdleGate gate{};
                gate.cycle = cycle;
                gate.timerCycle = timer;
                gate.eventCycle = event;
                gate.winner = winner;
                g_rpl3.idle.push_back(gate);
                return;
            }
            ++g_rpl3.ignored;
        }

        void rpl3EnsureReplayLoadedLocked()
        {
            if (g_rpl3.replayLoaded)
                return;
            g_rpl3.replayLoaded = true;
            const char *spec = std::getenv("PS2X_PAD_SCRIPT");
            if (!spec || spec[0] != '@' || spec[1] == '\0')
                return;
            std::string content;
            if (!padScriptReadFile(spec + 1, content))
                return; // the script loader fatals; replay just stays inactive
            size_t begin = 0u;
            while (begin <= content.size())
            {
                const size_t end = content.find('\n', begin);
                std::string line = content.substr(begin, end == std::string::npos
                                                             ? std::string::npos
                                                             : end - begin);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.compare(0, 5u, "# NV ") == 0)
                    rpl3ParseNvLine(line);
                if (end == std::string::npos)
                    break;
                begin = end + 1u;
            }
            if (!g_rpl3.rtc.empty() || !g_rpl3.mc.empty() || g_rpl3.haveTz || !g_rpl3.sched.empty() ||
                !g_rpl3.idle.empty())
            {
                std::fprintf(stderr,
                             "[rpl3] replay values from %s: rtc=%zu mc_dates=%zu tz=%s sched=%zu "
                             "idle=%zu ignored=%llu\n",
                             spec + 1, g_rpl3.rtc.size(), g_rpl3.mc.size(),
                             g_rpl3.haveTz ? "1" : "0", g_rpl3.sched.size(), g_rpl3.idle.size(),
                             static_cast<unsigned long long>(g_rpl3.ignored));
                const char *det = std::getenv("PS2X_DETERMINISTIC");
                if (!det || std::strcmp(det, "1") != 0)
                    std::fprintf(stderr, "[rpl3] WARNING: replaying recorded values without "
                                         "PS2X_DETERMINISTIC=1 (event order is host-timed)\n");
            }
        }

        uint64_t padRecordFnv1a(const void *data, size_t size, uint64_t hash)
        {
            const uint8_t *bytes = static_cast<const uint8_t *>(data);
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= 1099511628211ull;
            }
            return hash;
        }

        // Sorted relative paths of the regular files under a card root.
        // Best-effort: unreadable roots yield false, never an exception.
        bool padRecordListSaveFiles(const char *root, std::vector<std::string> &relsOut)
        {
            namespace fs = std::filesystem;
            relsOut.clear();
            if (!root || root[0] == '\0')
            {
                return false;
            }
            std::error_code ec;
            if (!fs::is_directory(root, ec) || ec)
            {
                return false;
            }
            fs::recursive_directory_iterator it(root, ec);
            const fs::recursive_directory_iterator end;
            while (!ec && it != end)
            {
                std::error_code ec2;
                if (it->is_regular_file(ec2) && !ec2)
                {
                    std::error_code ec3;
                    std::string rel = fs::relative(it->path(), root, ec3).string();
                    if (!ec3)
                    {
                        relsOut.push_back(rel);
                    }
                }
                it.increment(ec);
            }
            if (ec)
            {
                return false;
            }
            std::sort(relsOut.begin(), relsOut.end());
            return true;
        }

        // FNV-1a 64 over the sorted relative paths + bytes of the regular
        // files under mcRoot. Bounded (4096 files / 64 MiB); best-effort:
        // unreadable roots yield false, never an exception.
        bool padRecordHashSaveSet(const char *mcRoot, uint64_t &hashOut, uint64_t &filesOut)
        {
            hashOut = 14695981039346656037ull;
            filesOut = 0u;
            std::vector<std::string> rels;
            if (!padRecordListSaveFiles(mcRoot, rels))
            {
                return false;
            }
            uint64_t hash = 14695981039346656037ull;
            uint64_t bytes = 0u;
            constexpr uint64_t kMaxFiles = 4096u;
            constexpr uint64_t kMaxBytes = 64u << 20;
            for (const std::string &rel : rels)
            {
                if (filesOut >= kMaxFiles || bytes >= kMaxBytes)
                {
                    break;
                }
                hash = padRecordFnv1a(rel.data(), rel.size(), hash);
                hash = padRecordFnv1a("\0", 1, hash);
                std::FILE *f = std::fopen((std::string(mcRoot) + "/" + rel).c_str(), "rb");
                if (!f)
                {
                    return false;
                }
                char buf[8192];
                size_t n = 0;
                while (bytes < kMaxBytes && (n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                {
                    hash = padRecordFnv1a(buf, n, hash);
                    bytes += n;
                }
                std::fclose(f);
                ++filesOut;
            }
            hashOut = hash;
            return true;
        }

        // The card root the guest actually uses for port 0: the PS2X_MC_ROOT
        // override when set, else the IoPaths root main configured (the ELF
        // dir's mc0 by default). "" when neither resolves.
        std::string padRecordEffectiveMcRoot()
        {
            if (const char *env = std::getenv("PS2X_MC_ROOT"))
            {
                if (env[0] != '\0')
                {
                    return env;
                }
            }
            return PS2Runtime::getIoPaths().mcRoot.string();
        }

        // Copy a card root's files into destDir (created here; the caller
        // picked a name that doesn't exist), hashing relpaths + copied bytes
        // with the same scheme as padRecordHashSaveSet, so a later mcsave
        // over the snapshot reproduces snapHash. Bounded (4096 files /
        // 16 MiB); over the byte bound, or on any I/O error, the partial dir
        // is removed and the outcome is a header note, never a half copy.
        PadRecordSnap padRecordSnapshotMc(const std::string &root, const std::string &destDir,
                                          uint64_t &hashOut, uint64_t &filesOut)
        {
            namespace fs = std::filesystem;
            hashOut = 14695981039346656037ull;
            filesOut = 0u;
            constexpr uint64_t kMaxFiles = 4096u;
            constexpr uint64_t kMaxBytes = 16u << 20;
            std::vector<std::string> rels;
            if (!padRecordListSaveFiles(root.c_str(), rels))
            {
                return PadRecordSnap::Unreadable;
            }
            std::error_code ec;
            fs::create_directories(destDir, ec);
            if (ec)
            {
                return PadRecordSnap::Unreadable;
            }
            uint64_t hash = 14695981039346656037ull;
            uint64_t bytes = 0u;
            for (const std::string &rel : rels)
            {
                if (filesOut >= kMaxFiles)
                {
                    break;
                }
                hash = padRecordFnv1a(rel.data(), rel.size(), hash);
                hash = padRecordFnv1a("\0", 1, hash);
                const std::string src = root + "/" + rel;
                const std::string dst = destDir + "/" + rel;
                std::FILE *in = std::fopen(src.c_str(), "rb");
                if (!in)
                {
                    fs::remove_all(destDir, ec);
                    return PadRecordSnap::Unreadable;
                }
                fs::create_directories(fs::path(dst).parent_path(), ec);
                if (ec)
                {
                    std::fclose(in);
                    fs::remove_all(destDir, ec);
                    return PadRecordSnap::Unreadable;
                }
                std::FILE *out = std::fopen(dst.c_str(), "wb");
                if (!out)
                {
                    std::fclose(in);
                    fs::remove_all(destDir, ec);
                    return PadRecordSnap::Unreadable;
                }
                char buf[8192];
                size_t n = 0;
                bool tooLarge = false;
                bool ioError = false;
                while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0)
                {
                    if (bytes + n > kMaxBytes)
                    {
                        tooLarge = true;
                        break;
                    }
                    if (std::fwrite(buf, 1, n, out) != n)
                    {
                        ioError = true;
                        break;
                    }
                    hash = padRecordFnv1a(buf, n, hash);
                    bytes += n;
                }
                if (std::ferror(in))
                {
                    ioError = true; // short read: the copy would lie
                }
                std::fclose(in);
                std::fclose(out);
                if (tooLarge || ioError)
                {
                    fs::remove_all(destDir, ec);
                    return tooLarge ? PadRecordSnap::TooLarge : PadRecordSnap::Unreadable;
                }
                ++filesOut;
            }
            hashOut = hash;
            return PadRecordSnap::Ok;
        }

        void padRecordWriteHeaderLocked(std::time_t when)
        {
            std::FILE *f = g_padRecord.file;
            if (!f)
            {
                return;
            }
            char startUtc[32] = {0};
            padRecordUtc(startUtc, sizeof(startUtc), when, "%Y-%m-%dT%H:%M:%SZ");
            char vu1[64], mtvu[64], finish[64], det[64], vfloat[64], revdma[64], mcroot[512];
            std::snprintf(vu1, sizeof(vu1), "%s", padRecordKnob("PS2X_VU1_ENGINE"));
            std::snprintf(mtvu, sizeof(mtvu), "%s", padRecordKnob("PS2X_MTVU"));
            std::snprintf(finish, sizeof(finish), "%s", padRecordKnob("PS2X_GS_FINISH_TIMING"));
            std::snprintf(det, sizeof(det), "%s", padRecordKnob("PS2X_DETERMINISTIC"));
            std::snprintf(vfloat, sizeof(vfloat), "%s", padRecordKnob("PS2X_VU_FLOAT"));
            std::snprintf(revdma, sizeof(revdma), "%s", padRecordKnob("PS2X_VIF1_REVERSE_DMA"));
            std::snprintf(mcroot, sizeof(mcroot), "%s", padRecordKnob("PS2X_MC_ROOT"));
            padRecordSanitize(vu1);
            padRecordSanitize(mtvu);
            padRecordSanitize(finish);
            padRecordSanitize(det);
            padRecordSanitize(vfloat);
            padRecordSanitize(revdma);
            padRecordSanitize(mcroot);
            const char *mcRaw = std::getenv("PS2X_MC_ROOT");
            uint64_t saveHash = 0u, saveFiles = 0u;
            const bool saveOk = padRecordHashSaveSet(mcRaw, saveHash, saveFiles);
            // FH17: v2 = stamps are guest time in stock ms in every mode;
            // v1 = events-mode spans may be stamped on the FH5 clock (half a
            // tick early), which replays keep (padScriptWantsLegacyEventsClock).
            // RPL3: v3 = v2 stamps plus "# NV ..." recorded host values.
            const bool fh5Stamps = ps2_fh1::eventsMode() && !ps2_fh1::exactStockClock();
            std::fprintf(f,
                         "# padrec %s\n"
                         "# start_utc=%s\n"
                         "# build=%s\n"
                         "# env_sha=%s\n"
                         "# knobs VU1_ENGINE=%s MTVU=%s FINISH_TIMING=%s "
                         "DETERMINISTIC=%s VU_FLOAT=%s VIF1_REVERSE_DMA=%s\n",
                         fh5Stamps ? "v1" : "v3", startUtc, ps2x::buildId(), ps2x::recordedEnvFileHash(),
                         vu1, mtvu, finish, det, vfloat, revdma);
            {
                char f120[64], fix[256];
                std::snprintf(f120, sizeof(f120), "%s", padRecordKnob("PS2X_SSX3_FULL120"));
                std::snprintf(fix, sizeof(fix), "%s", padRecordKnob("PS2X_SSX3_FULL120_FIX"));
                padRecordSanitize(f120);
                padRecordSanitize(fix);
                std::fprintf(f, "# full120 FULL120=%s FULL120_FIX=%s events_clock=%s\n", f120, fix,
                             fh5Stamps ? "fh5" : "exact");
            }
            if (saveOk)
            {
                std::fprintf(f, "# mcroot=%s mcsave=%016llx mcfiles=%llu\n", mcroot,
                             static_cast<unsigned long long>(saveHash),
                             static_cast<unsigned long long>(saveFiles));
            }
            else
            {
                std::fprintf(f, "# mcroot=%s mcsave=unreadable mcfiles=0\n", mcroot);
            }
            {
                const char *snapName = "none";
                if (g_padRecord.snapStatus == PadRecordSnap::Ok)
                    snapName = g_padRecord.snapDirName.c_str();
                else if (g_padRecord.snapStatus == PadRecordSnap::TooLarge)
                    snapName = "too-large";
                else if (g_padRecord.snapStatus == PadRecordSnap::Unreadable)
                    snapName = "unreadable";
                std::string snapSrc =
                    g_padRecord.snapRoot.empty() ? std::string("none") : g_padRecord.snapRoot;
                for (char &c : snapSrc)
                {
                    if (c == '\n' || c == '\r')
                        c = '_';
                }
                if (g_padRecord.snapStatus == PadRecordSnap::Ok)
                {
                    std::fprintf(f, "# mcsnap=%s mcsrc=%s mcsnap_sha=%016llx mcsnap_files=%llu\n",
                                 snapName, snapSrc.c_str(),
                                 static_cast<unsigned long long>(g_padRecord.snapHash),
                                 static_cast<unsigned long long>(g_padRecord.snapFiles));
                }
                else
                {
                    std::fprintf(f, "# mcsnap=%s mcsrc=%s mcsnap_sha=none mcsnap_files=0\n", snapName,
                                 snapSrc.c_str());
                }
            }
            if (g_padRecord.haveLoadNote)
            {
                std::fprintf(f, "# load_tick=%llu (new segment after a mid-session state load)\n",
                             static_cast<unsigned long long>(g_padRecord.loadTickNote));
            }
            // RPL3: host values noted before the arm (boot RTC reads precede
            // the first pad read) flush here, right after the header.
            rpl3FlushPendingToFile();
            std::fflush(f);
        }

        void padRecordFinishArmLocked(const char *source, std::time_t when)
        {
            // g_padRecord.file and .path must be set by the caller; dir mode
            // also sets the snapshot outcome before this runs.
            g_padRecord.snapRoot = padRecordEffectiveMcRoot();
            padRecordWriteHeaderLocked(when);
            g_padRecord.enabled = true;
            g_padRecordArmed.store(true, std::memory_order_relaxed);
            std::fprintf(stderr, "[padrecord] armed path=%s source=%s\n",
                         g_padRecord.path.c_str(), source);
        }

        void padRecordArmLocked(const char *path, const char *source)
        {
            g_padRecord.dirMode = false;
            g_padRecord.file = std::fopen(path, "w");
            if (!g_padRecord.file)
            {
                std::fprintf(stderr, "[padrecord] cannot open %s (%s); recording off\n",
                             path, source);
                return;
            }
            g_padRecord.path = path;
            padRecordFinishArmLocked(source, std::time(nullptr));
        }

        uint64_t padRecordParseKeep(const char *value)
        {
            constexpr uint64_t kDefaultKeep = 30u;
            if (!value || value[0] == '\0')
            {
                return kDefaultKeep;
            }
            uint64_t keep = 0u;
            for (const char *p = value; *p; ++p)
            {
                if (*p < '0' || *p > '9')
                {
                    return kDefaultKeep;
                }
                keep = keep * 10u + static_cast<uint64_t>(*p - '0');
            }
            return keep;
        }

        void padRecordPruneDirLocked(const char *dir, uint64_t keep, const char *currentName)
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            std::vector<std::string> names;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            {
                std::error_code ec2;
                if (!it->is_regular_file(ec2) || ec2)
                {
                    continue;
                }
                const std::string name = it->path().filename().string();
                if (name.size() > 11u && name.compare(0, 7u, "padrec-") == 0 &&
                    name.compare(name.size() - 4u, 4u, ".txt") == 0)
                {
                    names.push_back(name);
                }
            }
            if (ec)
            {
                std::fprintf(stderr, "[padrecord] prune: cannot list %s; keeping all\n", dir);
                return;
            }
            std::sort(names.begin(), names.end());
            uint64_t pruned = 0u, survivors = 0u, snapsPruned = 0u;
            for (auto it = names.rbegin(); it != names.rend(); ++it)
            {
                if (survivors < keep)
                {
                    ++survivors;
                    continue;
                }
                if (currentName && *it == currentName)
                {
                    continue; // never delete the live file (clock skew)
                }
                std::error_code ec3;
                fs::remove(fs::path(dir) / *it, ec3);
                if (!ec3)
                {
                    ++pruned;
                    // The snapshot goes with its recording (same stamp,
                    // ".mc" sibling); directories only, never the live one
                    // (it survives above as a survivor or currentName).
                    const std::string snap = it->substr(0, it->size() - 4) + ".mc";
                    const fs::path snapPath = fs::path(dir) / snap;
                    std::error_code ec4;
                    if (fs::is_directory(snapPath, ec4) && !ec4)
                    {
                        std::error_code ec5;
                        fs::remove_all(snapPath, ec5);
                        if (!ec5)
                        {
                            ++snapsPruned;
                        }
                    }
                }
            }
            std::fprintf(stderr, "[padrecord] prune dir=%s keep=%llu pruned=%llu snaps=%llu files=%llu\n",
                         dir, static_cast<unsigned long long>(keep),
                         static_cast<unsigned long long>(pruned),
                         static_cast<unsigned long long>(snapsPruned),
                         static_cast<unsigned long long>(names.size() - pruned));
        }

        void padRecordArmDirLocked(const char *dir, uint64_t keep, const char *source)
        {
            namespace fs = std::filesystem;
            g_padRecord.dirMode = true;
            g_padRecord.dir = dir ? dir : "";
            g_padRecord.keep = keep;
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (ec)
            {
                std::fprintf(stderr, "[padrecord] cannot create %s (%s); recording off\n",
                             dir, source);
                return;
            }
            const std::time_t when = std::time(nullptr);
            char stamp[32] = {0};
            padRecordUtc(stamp, sizeof(stamp), when, "%Y%m%d-%H%M%S");
            // One file per session; -N on same-second collision.
            for (int n = 0; n <= 100; ++n)
            {
                char name[64];
                if (n == 0)
                {
                    std::snprintf(name, sizeof(name), "padrec-%s.txt", stamp);
                }
                else
                {
                    std::snprintf(name, sizeof(name), "padrec-%s-%d.txt", stamp, n + 1);
                }
                const std::string full = std::string(dir) + "/" + name;
                // The card snapshot is a sibling dir sharing the stamp; both
                // names must be free (a stale .mc from a killed session
                // collides like a stale .txt).
                const std::string leaf(name); // always ends in ".txt"
                const std::string snapLeaf = leaf.substr(0, leaf.size() - 4) + ".mc";
                const std::string snapFull = std::string(dir) + "/" + snapLeaf;
                std::error_code ec2;
                if (fs::exists(full, ec2) || ec2)
                {
                    continue;
                }
                if (fs::exists(snapFull, ec2) || ec2)
                {
                    continue;
                }
                g_padRecord.file = std::fopen(full.c_str(), "w");
                if (!g_padRecord.file)
                {
                    std::fprintf(stderr, "[padrecord] cannot open %s (%s); recording off\n",
                                 full.c_str(), source);
                    return;
                }
                g_padRecord.path = full;
                // Snapshot the card before the header goes out (the header
                // carries the outcome). Best-effort: any failure is a
                // header note, and recording continues regardless.
                {
                    const std::string root = padRecordEffectiveMcRoot();
                    if (!root.empty())
                    {
                        uint64_t snapHash = 0u, snapFiles = 0u;
                        g_padRecord.snapStatus =
                            padRecordSnapshotMc(root, snapFull, snapHash, snapFiles);
                        if (g_padRecord.snapStatus == PadRecordSnap::Ok)
                        {
                            g_padRecord.snapDirName = snapLeaf;
                            g_padRecord.snapHash = snapHash;
                            g_padRecord.snapFiles = snapFiles;
                        }
                        std::fprintf(stderr,
                                     "[padrecord] snapshot root=%s status=%d files=%llu\n",
                                     root.c_str(), static_cast<int>(g_padRecord.snapStatus),
                                     static_cast<unsigned long long>(snapFiles));
                    }
                }
                padRecordFinishArmLocked(source, when);
                padRecordPruneDirLocked(dir, keep, name);
                return;
            }
            std::fprintf(stderr, "[padrecord] no free session name in %s (%s); recording off\n",
                         dir, source);
        }

        void padRecordResetLocked()
        {
            if (g_padRecord.file)
            {
                std::fflush(g_padRecord.file);
                std::fclose(g_padRecord.file);
            }
            // Field by field: the struct holds a mutex (not assignable).
            g_padRecord.enabled = false;
            g_padRecord.capped = false;
            g_padRecord.finalized = false;
            g_padRecord.file = nullptr;
            g_padRecord.path.clear();
            g_padRecord.haveOpen = false;
            g_padRecord.openTick = 0u;
            g_padRecord.openMs = 0u;
            g_padRecord.openState = PadInputState{};
            g_padRecord.lastTick = 0u;
            g_padRecord.lastNextMs = 0u;
            g_padRecord.firstEntry = true;
            g_padRecord.lastFlushTick = 0u;
            g_padRecord.entriesSinceFlush = 0u;
            g_padRecord.totalEntries = 0u;
            g_padRecord.snapRoot.clear();
            g_padRecord.snapDirName.clear();
            g_padRecord.snapHash = 0u;
            g_padRecord.snapFiles = 0u;
            g_padRecord.snapStatus = PadRecordSnap::None;
            g_padRecord.dirMode = false;
            g_padRecord.dir.clear();
            g_padRecord.keep = 30u;
            g_padRecord.haveLoadNote = false;
            g_padRecord.loadTickNote = 0u;
            g_padRecordArmed.store(false, std::memory_order_relaxed);
        }

        void padRecordInitLocked()
        {
            if (g_padRecord.initDone)
            {
                return;
            }
            g_padRecord.initDone = true;
            // File mode wins; dir mode (one file per session + prune) next.
            if (const char *path = std::getenv("PS2X_PAD_RECORD"))
            {
                if (path[0] != '\0')
                {
                    std::atexit(padRecordExitFlush);
                    padRecordArmLocked(path, "env");
                    return;
                }
            }
            if (const char *dir = std::getenv("PS2X_PAD_RECORD_DIR"))
            {
                if (dir[0] != '\0')
                {
                    std::atexit(padRecordExitFlush);
                    padRecordArmDirLocked(
                        dir, padRecordParseKeep(std::getenv("PS2X_PAD_RECORD_KEEP")), "env");
                    return;
                }
            }
        }

        void padRecordEnsureInit()
        {
            if (g_padRecordInitDone.load(std::memory_order_relaxed))
            {
                return;
            }
            std::lock_guard<std::mutex> lock(g_padRecord.mutex);
            padRecordInitLocked();
            g_padRecordInitDone.store(true, std::memory_order_relaxed);
        }

        void padRecordOnRead(const PadInputState &state, uint64_t guestVsyncTick, int port, int slot)
        {
            padRecordEnsureInit();
            if (!g_padRecordArmed.load(std::memory_order_relaxed))
            {
                return;
            }
            std::lock_guard<std::mutex> lock(g_padRecord.mutex);
            if (!g_padRecord.enabled || g_padRecord.capped)
            {
                return;
            }
            if (port != 0 || slot != 0)
            {
                return;
            }
            const uint64_t tick = g_padRecord.testTickSet ? g_padRecord.testTick : guestVsyncTick;
            const uint64_t nowMs = padRecordReadMs(tick, false);
            if (!g_padRecord.haveOpen)
            {
                g_padRecord.haveOpen = true;
                g_padRecord.openTick = tick;
                g_padRecord.openMs = nowMs;
                g_padRecord.openState = state;
                g_padRecord.lastTick = tick;
                g_padRecord.lastNextMs = padRecordReadMs(tick, true);
                g_padRecord.lastFlushTick = tick;
                return;
            }
            if (tick > g_padRecord.lastTick)
            {
                g_padRecord.lastTick = tick;
                g_padRecord.lastNextMs = padRecordReadMs(tick, true);
            }
            if (padRecordSameState(state, g_padRecord.openState))
            {
                if (tick >= g_padRecord.openTick + kPadRecordSplitTicks)
                {
                    // Split the long tail so a force-stop keeps all but ~10 s.
                    padRecordEmitLocked(g_padRecord.openMs, nowMs);
                    g_padRecord.openTick = tick;
                    g_padRecord.openMs = nowMs;
                    std::fflush(g_padRecord.file);
                    g_padRecord.lastFlushTick = tick;
                    g_padRecord.entriesSinceFlush = 0u;
                }
                return;
            }
            if (tick == g_padRecord.openTick)
            {
                // Sub-tick change: unrepresentable on the tick clock; latest wins.
                g_padRecord.openState = state;
                return;
            }
            if (tick < g_padRecord.openTick)
            {
                return; // clock moved backwards; keep the open entry (cannot happen)
            }
            padRecordEmitLocked(g_padRecord.openMs, nowMs);
            g_padRecord.openTick = tick;
            g_padRecord.openMs = nowMs;
            g_padRecord.openState = state;
            if (g_padRecord.totalEntries >= kPadRecordMaxEntries)
            {
                g_padRecord.capped = true;
                std::fflush(g_padRecord.file);
                std::fclose(g_padRecord.file);
                g_padRecord.file = nullptr;
                std::fprintf(stderr, "[padrecord] entry cap %llu hit; recording stopped (file valid)\n",
                             static_cast<unsigned long long>(kPadRecordMaxEntries));
                return;
            }
            if (g_padRecord.entriesSinceFlush >= kPadRecordFlushEntries ||
                tick >= g_padRecord.lastFlushTick + kPadRecordSplitTicks)
            {
                std::fflush(g_padRecord.file);
                g_padRecord.lastFlushTick = tick;
                g_padRecord.entriesSinceFlush = 0u;
            }
        }

        // LIM1 DEV-ONLY input marker (PS2X_INPUT_MARKER=1). Unset/anything-else
        // (default) = one relaxed atomic check per port-0 pad read, zero behavior
        // change, and no guest-function wrappers (the game override below
        // returns early). When armed, every port-0 read pushes one row onto a
        // host-side ring (4,096 rows: ~10 s at 120 Hz even at two reads per
        // update) carrying the raw pad, the update's 60/120 phase, the FIX
        // echo, a read-time input2 bank snapshot, and the latest read-only
        // guest-call latches (stick-angle tracker 0x133308, completion gate
        // 0x114db8).
        // Pressing L3+R3 within 100 ms of each other (the chord passes through
        // to the guest unchanged) stamps `# MARK <ms>` into the active padrec
        // and, 2 s later, writes markers/marker-<UTC>.tsv with the rows in
        // [trigMs-8000, trigMs+2000]. The tracker/gate latches take the
        // player's rider only (TK43d chain; rival AI calls bump *_others)
        // when the chain resolves, every call unfiltered when it dangles
        // (menus). Cap 50 markers/session. The dir is
        // <PS2X_PAD_RECORD_DIR>/../markers (Odin files/markers/); iOS falls
        // back to $HOME/Documents/markers and desktop to ./markers. All guest
        // taps are reads; the det hash is unaffected (proven by the knob-on/off
        // det gate in local/research/LIM1/REPORT.md).
        struct MarkerTrackerLatch
        {
            bool have = false;
            uint64_t tick = 0u; // guest vsync tick at the call
            uint64_t half = 0u; // stockHalfTicks() at the call
            uint32_t s = 0u;    // a0: tracker struct
            uint32_t in0 = 0u;  // [a1+0]: digital input word (INP6: d-pad visible here)
            uint32_t in1 = 0u;  // [a1+4]
            uint32_t s0 = 0u;   // S+0x0 (grab-window flag word)
            uint32_t grab = 0u; // S+0x4 (grab stage 0/1/2/3/5; INP6)
            uint32_t grabId = 0u; // S+0x8 (grab id, -1 = none)
            uint32_t phase = 0u;  // S+0xC (directional phase; 3 = idle)
            uint32_t target = 0u; // S+0x10 (spin target, float bits)
            uint32_t a30 = 0u;    // S+0x30 (accumulated sequence rotation)
            uint32_t r = 0u;      // [S+0x58]: rider struct (0 = unread)
            uint32_t px = 0u, py = 0u, pz = 0u; // R+0x110/4/8 position, float bits (13e0dc proves R+0x110)
            uint32_t vx = 0u, vy = 0u, vz = 0u; // R+0x1e0/4/8 velocity, float bits
            uint32_t meter = 0u;                // R+0x2f4 (meter level; facts.md TK43d)
            uint32_t w438 = 0u;                 // R+0x438 (FH26 word)
            uint32_t wde0 = 0u, wde4 = 0u;      // R+0xde0/4 (control state)
            uint32_t v0 = 0u;                   // post-call v0
            uint32_t others = 0u; // rival-tracker 0x133308 calls since this latch (player filter)
        };
        struct MarkerGateLatch
        {
            bool have = false;
            uint64_t tick = 0u, half = 0u;
            uint32_t a0 = 0u, a1 = 0u, a2 = 0u, a3 = 0u; // a0 = R (rider ptr; INP6)
            uint32_t f0 = 0u, f1 = 0u, f12 = 0u, f20 = 0u, f21 = 0u, f13 = 0u,
                     f14 = 0u; // float bits (srcTap set)
            uint32_t v0 = 0u, v1 = 0u; // post-call: v0 = gate outcome (0 = admit path)
            uint32_t others = 0u; // rival-rider 0x114db8 calls since this latch (player filter)
        };
        struct MarkerRow
        {
            uint64_t tick = 0u, ms = 0u;
            uint16_t buttons = 0xFFFFu; // active-low word handed to the guest
            uint8_t lx = 0x80u, ly = 0x80u, rx = 0x80u, ry = 0x80u;
            uint8_t f120 = 0u; // ps2_fh1::Mode as int (0 off, 1 always, 2 events)
            uint8_t half = 0u; // events: stockHalfTicks()&1; else 0
            uint64_t fix = 0u;
            uint32_t fix12 = 0u;
            MarkerTrackerLatch tr{};
            MarkerGateLatch g{};
            // input2 bank snapshot, taken at read time (the bank updates
            // every update, faster than the ~1/6-update tracker; the 0x321298
            // wrapper only discovers bank/count, never reads records, so wrap
            // order vs input2's own wrapper cannot matter).
            uint32_t i2Bank = 0u;
            uint32_t i2Count = 0u;
            uint64_t i2Wrapped = 0u; // cumulative intercepted 0x321298 calls
            ps2_fh1::Input2Record i2[24]{};
        };
        struct MarkerPending
        {
            uint64_t trigMs = 0u, trigTick = 0u;
            std::time_t wall = 0;
            uint32_t index = 0u;
        };
        struct PadMarker
        {
            std::mutex mutex;
            bool enabled = false;
            std::string dir;
            std::vector<MarkerRow> ring;
            uint64_t head = 0u; // next write slot
            uint64_t count = 0u; // rows held (<= cap)
            uint16_t prevButtons = 0xFFFFu;
            uint64_t l3Ms = 0u, r3Ms = 0u; // rising-edge ms while held
            bool haveL3 = false, haveR3 = false;
            bool chordLatched = false; // chord already fired for the current joint hold
            std::vector<MarkerPending> pending;
            uint32_t markersDone = 0u; // written + pending (cap 50)
            bool capLogged = false;
            bool testTickSet = false;
            uint64_t testTick = 0u;
            uint64_t lastMs = 0u;
        };
        PadMarker g_padMarker;
        std::atomic<bool> g_padMarkerArmed{false};
        // Latches live outside the mutex: production writes them on the EE
        // thread only (same thread that pushes ring rows); tests inject them
        // with no EE running.
        MarkerTrackerLatch g_lim1TrackerLatch;
        MarkerGateLatch g_lim1GateLatch;
        uint32_t g_lim1Input2Bank = 0u, g_lim1Input2Count = 0u;
        uint64_t g_lim1Input2Wrapped = 0u;
        const uint8_t *g_lim1TestRdram = nullptr; // test hook only; production reads runtime RAM

        constexpr uint64_t kPadMarkerCap = 4096u;
        constexpr uint64_t kPadMarkerChordMs = 100u;
        constexpr uint64_t kPadMarkerBeforeMs = 8000u;
        constexpr uint64_t kPadMarkerAfterMs = 2000u;
        constexpr uint32_t kPadMarkerCap50 = 50u;

        uint64_t padMarkerReadMs(uint64_t tick, bool next)
        {
            // Same script clock as the recorder (PR3): events mode reads the
            // stock-time accumulator, which only knows "now".
            if (ps2_fh1::eventsMode() && !g_padMarker.testTickSet)
            {
                const uint64_t half = ps2_fh1::stockHalfTicks() + (next ? ps2_fh1::stockHalfStepNext() : 0u);
                return (half * 100000ull) / (5994ull * 2ull);
            }
            return padScriptVsyncTickToMs(next ? tick + 1u : tick);
        }

        std::string padMarkerDefaultDir()
        {
            if (const char *rec = std::getenv("PS2X_PAD_RECORD_DIR"))
            {
                if (rec[0] != '\0')
                {
                    namespace fs = std::filesystem;
                    const fs::path parent = fs::path(rec).parent_path();
                    if (!parent.empty())
                    {
                        return (parent / "markers").string();
                    }
                    return "markers";
                }
            }
#if defined(PS2X_IOS)
            if (const char *home = std::getenv("HOME"))
            {
                if (home[0] != '\0')
                {
                    return std::string(home) + "/Documents/markers";
                }
            }
            return {};
#elif defined(__ANDROID__)
            // The app files dir is not visible here; the Odin play env always
            // sets PS2X_PAD_RECORD_DIR, so without it stay disarmed (loud).
            return {};
#else
            return "markers";
#endif
        }

        void padMarkerArmLocked(const std::string &dir, const char *source)
        {
            namespace fs = std::filesystem;
            if (dir.empty())
            {
                std::fprintf(stderr, "input-marker: PS2X_INPUT_MARKER=1 but no output dir; marker off\n");
                return;
            }
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (ec)
            {
                std::fprintf(stderr, "input-marker: cannot create %s (%s); marker off\n",
                             dir.c_str(), source);
                return;
            }
            g_padMarker.dir = dir;
            g_padMarker.ring.assign(kPadMarkerCap, MarkerRow{});
            g_padMarker.enabled = true;
            g_padMarkerArmed.store(true, std::memory_order_relaxed);
            std::fprintf(stderr, "input-marker: armed dir=%s\n", dir.c_str());
        }

        bool padMarkerArm(const std::string &dir, const char *source)
        {
            std::lock_guard<std::mutex> lock(g_padMarker.mutex);
            if (g_padMarker.enabled)
            {
                return true;
            }
            padMarkerArmLocked(dir, source);
            return g_padMarker.enabled;
        }

        void padMarkerPushLocked(const MarkerRow &row)
        {
            g_padMarker.ring[g_padMarker.head % kPadMarkerCap] = row;
            ++g_padMarker.head;
            if (g_padMarker.count < kPadMarkerCap)
            {
                ++g_padMarker.count;
            }
        }

        // `# MARK <ms>` into the active padrec (the script parser skips
        // full-line '#' comments; the load marker in padRecordNoteLoad uses the
        // same shape). Lock order: marker mutex -> record mutex; no path takes
        // them in the reverse order.
        void padMarkerStampPadrec(uint64_t ms)
        {
            if (!g_padRecordArmed.load(std::memory_order_relaxed))
            {
                return;
            }
            std::lock_guard<std::mutex> lock(g_padRecord.mutex);
            if (!g_padRecord.enabled || g_padRecord.capped || !g_padRecord.file)
            {
                return;
            }
            std::fprintf(g_padRecord.file, "\n# MARK %llu\n", static_cast<unsigned long long>(ms));
            std::fflush(g_padRecord.file);
        }

        double padMarkerF32(uint32_t bits)
        {
            float f = 0.0f;
            std::memcpy(&f, &bits, 4u);
            return static_cast<double>(f);
        }

        void padMarkerWriteInput2(std::FILE *f, const MarkerRow &row)
        {
            // Compact bank dump: i:raw,press,release,level,repeat,countdown,guard;... (hex).
            for (uint32_t i = 0; i < row.i2Count && i < 24u; ++i)
            {
                const ps2_fh1::Input2Record &r = row.i2[i];
                std::fprintf(f, "%s%u:%x,%x,%x,%x,%x,%x,%x", i == 0u ? "" : ";", i, r.raw,
                             r.press, r.release, r.level, r.repeat, r.countdown, r.guard);
            }
        }

        void padMarkerWriteRow(std::FILE *f, const MarkerRow &row)
        {
            const MarkerTrackerLatch &tr = row.tr;
            const MarkerGateLatch &g = row.g;
            std::fprintf(f, "%llu\t%llu\t%04x\t%u\t%u\t%u\t%u\t%u\t%u\t%016llx\t%08x",
                         static_cast<unsigned long long>(row.tick),
                         static_cast<unsigned long long>(row.ms), row.buttons, row.lx, row.ly,
                         row.rx, row.ry, row.f120, row.half,
                         static_cast<unsigned long long>(row.fix), row.fix12);
            std::fprintf(f, "\t%u\t%llu\t%llu\t%08x\t%08x\t%08x\t%08x\t%x\t%x\t%x\t%.9g\t%08x\t%08x",
                         tr.have ? 1u : 0u, static_cast<unsigned long long>(tr.tick),
                         static_cast<unsigned long long>(tr.half), tr.s, tr.in0, tr.in1, tr.s0,
                         tr.grab, tr.grabId, tr.phase, padMarkerF32(tr.target), tr.a30, tr.r);
            std::fprintf(f, "\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%08x\t%08x\t%08x\t%08x\t%08x\t%u",
                         padMarkerF32(tr.px), padMarkerF32(tr.py), padMarkerF32(tr.pz),
                         padMarkerF32(tr.vx), padMarkerF32(tr.vy), padMarkerF32(tr.vz), tr.meter,
                         tr.w438, tr.wde0, tr.wde4, tr.v0, tr.others);
            std::fprintf(f, "\t%08x\t%u\t%llu\t", row.i2Bank, row.i2Count,
                         static_cast<unsigned long long>(row.i2Wrapped));
            padMarkerWriteInput2(f, row);
            std::fprintf(f, "\t%u\t%llu\t%llu\t%08x\t%08x\t%08x\t%08x",
                         g.have ? 1u : 0u, static_cast<unsigned long long>(g.tick),
                         static_cast<unsigned long long>(g.half), g.a0, g.a1, g.a2, g.a3);
            std::fprintf(f, "\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%08x\t%08x\t%u\n",
                         padMarkerF32(g.f0), padMarkerF32(g.f1), padMarkerF32(g.f12),
                         padMarkerF32(g.f20), padMarkerF32(g.f21), padMarkerF32(g.f13),
                         padMarkerF32(g.f14), g.v0, g.v1, g.others);
        }

        void padMarkerWriteLocked(const MarkerPending &p, bool truncated)
        {
            namespace fs = std::filesystem;
            char stamp[32] = {0};
            padRecordUtc(stamp, sizeof(stamp), p.wall, "%Y%m%d-%H%M%S");
            std::string path;
            std::FILE *f = nullptr;
            for (int n = 0; n <= 100; ++n)
            {
                char name[64];
                if (n == 0)
                {
                    std::snprintf(name, sizeof(name), "marker-%s.tsv", stamp);
                }
                else
                {
                    std::snprintf(name, sizeof(name), "marker-%s-%d.tsv", stamp, n + 1);
                }
                path = g_padMarker.dir + "/" + name;
                std::error_code ec;
                if (fs::exists(path, ec) || ec)
                {
                    path.clear();
                    continue;
                }
                f = std::fopen(path.c_str(), "w");
                if (f)
                {
                    break;
                }
                path.clear();
            }
            if (!f)
            {
                std::fprintf(stderr, "input-marker: cannot open a session file in %s; marker #%u lost\n",
                             g_padMarker.dir.c_str(), p.index);
                return;
            }
            char fix[256], f120[64];
            std::snprintf(f120, sizeof(f120), "%s", padRecordKnob("PS2X_SSX3_FULL120"));
            std::snprintf(fix, sizeof(fix), "%s", padRecordKnob("PS2X_SSX3_FULL120_FIX"));
            padRecordSanitize(f120);
            padRecordSanitize(fix);
            std::fprintf(f, "# input-marker v1 #%u trigger_ms=%llu trigger_tick=%llu%s\n",
                         p.index, static_cast<unsigned long long>(p.trigMs),
                         static_cast<unsigned long long>(p.trigTick), truncated ? " truncated" : "");
            std::fprintf(f, "# build=%s full120 FULL120=%s FULL120_FIX=%s\n", ps2x::buildId(), f120,
                         fix);
            std::fprintf(f, "# window_ms=[%llu,%llu] buttons=active-low-as-handed-to-guest "
                            "target=float(S+0x10) pos=R+0x110 vel=R+0x1e0\n",
                         static_cast<unsigned long long>(p.trigMs > kPadMarkerBeforeMs
                                                             ? p.trigMs - kPadMarkerBeforeMs
                                                             : 0u),
                         static_cast<unsigned long long>(p.trigMs + kPadMarkerAfterMs));
            std::fprintf(f, "tick\tms\tbuttons\tlx\tly\trx\try\tf120\thalf\tfix\tfix12\t"
                            "tr_have\ttr_tick\ttr_half\ts\tin0\tin1\ts0\tgrab\tgrabid\tphase\ttarget\ta30\tr\t"
                            "px\tpy\tpz\tvx\tvy\tvz\tmeter\tw438\twde0\twde4\ttr_v0\ttr_others\t"
                            "i2_bank\ti2_count\ti2_wrapped\ti2_recs\t"
                            "g_have\tg_tick\tg_half\ta0\ta1\ta2\ta3\tf0\tf1\tf12\tf20\tf21\tf13\tf14\tv0\tv1\tg_others\n");
            const uint64_t loMs = p.trigMs > kPadMarkerBeforeMs ? p.trigMs - kPadMarkerBeforeMs : 0u;
            const uint64_t hiMs = truncated ? g_padMarker.lastMs : p.trigMs + kPadMarkerAfterMs;
            for (uint64_t i = 0u; i < g_padMarker.count; ++i)
            {
                const MarkerRow &row =
                    g_padMarker.ring[(g_padMarker.head - g_padMarker.count + i) % kPadMarkerCap];
                if (row.ms < loMs || row.ms > hiMs)
                {
                    continue;
                }
                padMarkerWriteRow(f, row);
            }
            std::fflush(f);
            std::fclose(f);
            std::fprintf(stderr, "input-marker: wrote %s\n", path.c_str());
        }

        void padMarkerExitFlush()
        {
            // Best-effort finalize of triggers younger than 2 s at exit (the
            // boot's stop tick may land inside the post window); a force-stop
            // loses those, but the padrec MARK line already stamped them.
            std::unique_lock<std::mutex> lock(g_padMarker.mutex, std::try_to_lock);
            if (!lock.owns_lock() || !g_padMarker.enabled)
            {
                return;
            }
            for (const MarkerPending &p : g_padMarker.pending)
            {
                padMarkerWriteLocked(p, true);
            }
            g_padMarker.pending.clear();
        }

        void padMarkerResetLocked()
        {
            g_padMarker.enabled = false;
            g_padMarker.dir.clear();
            g_padMarker.ring.clear();
            g_padMarker.head = 0u;
            g_padMarker.count = 0u;
            g_padMarker.prevButtons = 0xFFFFu;
            g_padMarker.l3Ms = 0u;
            g_padMarker.r3Ms = 0u;
            g_padMarker.haveL3 = false;
            g_padMarker.haveR3 = false;
            g_padMarker.chordLatched = false;
            g_padMarker.pending.clear();
            g_padMarker.markersDone = 0u;
            g_padMarker.capLogged = false;
            g_padMarker.testTickSet = false;
            g_padMarker.testTick = 0u;
            g_padMarker.lastMs = 0u;
            g_lim1TrackerLatch = MarkerTrackerLatch{};
            g_lim1GateLatch = MarkerGateLatch{};
            g_lim1Input2Bank = 0u;
            g_lim1Input2Count = 0u;
            g_lim1Input2Wrapped = 0u;
            g_lim1TestRdram = nullptr;
            g_padMarkerArmed.store(false, std::memory_order_relaxed);
        }

        void padMarkerOnRead(const PadInputState &state, uint64_t guestVsyncTick, int port, int slot,
                                 PS2Runtime *runtime)
        {
            if (!g_padMarkerArmed.load(std::memory_order_relaxed))
            {
                return;
            }
            std::lock_guard<std::mutex> lock(g_padMarker.mutex);
            if (!g_padMarker.enabled)
            {
                return;
            }
            if (port != 0 || slot != 0)
            {
                return;
            }
            const uint64_t tick = g_padMarker.testTickSet ? g_padMarker.testTick : guestVsyncTick;
            const uint64_t nowMs = padMarkerReadMs(tick, false);
            g_padMarker.lastMs = nowMs;
            MarkerRow row{};
            row.tick = tick;
            row.ms = nowMs;
            row.buttons = state.buttons;
            row.lx = state.lx;
            row.ly = state.ly;
            row.rx = state.rx;
            row.ry = state.ry;
            row.f120 = static_cast<uint8_t>(ps2_fh1::mode());
            row.half = (ps2_fh1::eventsMode() && !g_padMarker.testTickSet)
                           ? static_cast<uint8_t>(ps2_fh1::stockHalfTicks() & 1u)
                           : 0u;
            row.fix = ps2_fh1::fixMask();
            row.fix12 = ps2_fh1::fixMask12();
            row.tr = g_lim1TrackerLatch;
            row.g = g_lim1GateLatch;
            row.i2Bank = g_lim1Input2Bank;
            row.i2Count = g_lim1Input2Count;
            row.i2Wrapped = g_lim1Input2Wrapped;
            const uint8_t *rdram =
                g_lim1TestRdram ? g_lim1TestRdram
                                : (runtime ? runtime->memory().getRDRAM() : nullptr);
            if (row.i2Count > 0u && row.i2Count <= 24u &&
                row.i2Bank <= 0x2000000u - 4u - 24u * 28u && rdram)
            {
                for (uint32_t i = 0; i < row.i2Count; ++i)
                {
                    std::memcpy(&row.i2[i], rdram + row.i2Bank + 4u + i * 28u, 28u);
                }
            }
            else
            {
                row.i2Count = 0u;
            }
            padMarkerPushLocked(row);
            // Chord edges on the effective (guest-visible) state.
            const bool l3Down = (state.buttons & kPadBtnL3) == 0u;
            const bool r3Down = (state.buttons & kPadBtnR3) == 0u;
            const bool wasL3Down = (g_padMarker.prevButtons & kPadBtnL3) == 0u;
            const bool wasR3Down = (g_padMarker.prevButtons & kPadBtnR3) == 0u;
            if (l3Down && !wasL3Down)
            {
                g_padMarker.l3Ms = nowMs;
                g_padMarker.haveL3 = true;
            }
            if (!l3Down && wasL3Down)
            {
                g_padMarker.haveL3 = false;
                g_padMarker.chordLatched = false;
            }
            if (r3Down && !wasR3Down)
            {
                g_padMarker.r3Ms = nowMs;
                g_padMarker.haveR3 = true;
            }
            if (!r3Down && wasR3Down)
            {
                g_padMarker.haveR3 = false;
                g_padMarker.chordLatched = false;
            }
            g_padMarker.prevButtons = state.buttons;
            if (!g_padMarker.chordLatched && l3Down && r3Down && g_padMarker.haveL3 &&
                g_padMarker.haveR3)
            {
                const uint64_t gap = g_padMarker.l3Ms > g_padMarker.r3Ms
                                         ? g_padMarker.l3Ms - g_padMarker.r3Ms
                                         : g_padMarker.r3Ms - g_padMarker.l3Ms;
                if (gap <= kPadMarkerChordMs)
                {
                    g_padMarker.chordLatched = true;
                    if (g_padMarker.markersDone >= kPadMarkerCap50)
                    {
                        if (!g_padMarker.capLogged)
                        {
                            g_padMarker.capLogged = true;
                            std::fprintf(stderr,
                                         "input-marker: marker cap 50 reached; ignoring further chords\n");
                        }
                    }
                    else
                    {
                        MarkerPending p{};
                        p.trigMs = nowMs;
                        p.trigTick = tick;
                        p.wall = std::time(nullptr);
                        p.index = g_padMarker.markersDone + 1u;
                        g_padMarker.pending.push_back(p);
                        ++g_padMarker.markersDone;
                        std::fprintf(stderr, "input-marker: trigger #%u ms=%llu tick=%llu\n",
                                     p.index, static_cast<unsigned long long>(nowMs),
                                     static_cast<unsigned long long>(tick));
                        padMarkerStampPadrec(nowMs);
                        // UX1: the course picker confirms chords the same way
                        // (status line + Android toast; store-only elsewhere).
                        ps2_savestate::noteQuickStatus("input marked");
                    }
                }
            }
            // Flush triggers whose 2 s post window has elapsed.
            for (auto it = g_padMarker.pending.begin(); it != g_padMarker.pending.end();)
            {
                if (nowMs >= it->trigMs + kPadMarkerAfterMs)
                {
                    padMarkerWriteLocked(*it, false);
                    it = g_padMarker.pending.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // ---- LIM1 guest-call latches (read-only; armed only) ----
        PS2Runtime::RecompiledFunction g_lim1TrackerNext = nullptr;
        PS2Runtime::RecompiledFunction g_lim1GateNext = nullptr;
        PS2Runtime::RecompiledFunction g_lim1Input2Next = nullptr;

        uint64_t lim1VsyncTick(PS2Runtime *runtime)
        {
            return runtime ? runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u;
        }

        uint32_t lim1FpuBits(R5900Context *ctx, int f)
        {
            uint32_t bits = 0u;
            if (ctx && f >= 0 && f < 32)
            {
                std::memcpy(&bits, &ctx->f[f], 4u);
            }
            return bits;
        }

        // Player-rider identity (facts.md TK43d, course-independent, verified
        // through wipeouts/resets): R = [[[0x4a28a8]+0x84]+0xC]+0x28. False
        // when any link is unreadable or null (menus / no race object), in
        // which case the latches below take every call unfiltered.
        bool lim1PlayerR(const uint8_t *rdram, uint32_t &out)
        {
            uint32_t w = 0u;
            if (!ps2_fh1::rd32(rdram, 0x4a28a8u, w) || w == 0u)
            {
                return false;
            }
            if (!ps2_fh1::rd32(rdram, w + 0x84u, w) || w == 0u)
            {
                return false;
            }
            if (!ps2_fh1::rd32(rdram, w + 0x0cu, w) || w == 0u)
            {
                return false;
            }
            if (!ps2_fh1::rd32(rdram, w + 0x28u, w) || w == 0u)
            {
                return false;
            }
            out = w;
            return true;
        }

        void lim1TrackerWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // Player filter (A3 showed rival AI trackers interleaving on the
            // same call site): when the TK43d chain resolves, only the
            // player's tracker latches; rival calls just bump others. When it
            // dangles (menus), every call latches unfiltered.
            bool latched = false;
            if (g_padMarkerArmed.load(std::memory_order_relaxed))
            {
                const uint32_t s = getRegU32(ctx, 4);
                uint32_t r = 0u;
                ps2_fh1::rd32(rdram, s + 0x58u, r);
                uint32_t player = 0u;
                if (!lim1PlayerR(rdram, player))
                {
                    player = r; // chain dangling: take every call unfiltered
                }
                if (r != player)
                {
                    ++g_lim1TrackerLatch.others;
                }
                else
                {
                    latched = true;
                    MarkerTrackerLatch lat{};
                    lat.have = true;
                    lat.tick = lim1VsyncTick(runtime);
                    lat.half = ps2_fh1::stockHalfTicks();
                    lat.s = s;
                    lat.r = r;
                    const uint32_t in = getRegU32(ctx, 5);
                    uint32_t w = 0u;
                    if (ps2_fh1::rd32(rdram, in, w))
                    {
                        lat.in0 = w;
                    }
                    if (ps2_fh1::rd32(rdram, in + 4u, w))
                    {
                        lat.in1 = w;
                    }
                    ps2_fh1::rd32(rdram, lat.s + 0x0u, lat.s0);
                    ps2_fh1::rd32(rdram, lat.s + 0x4u, lat.grab);
                    ps2_fh1::rd32(rdram, lat.s + 0x8u, lat.grabId);
                    ps2_fh1::rd32(rdram, lat.s + 0xcu, lat.phase);
                    ps2_fh1::rd32(rdram, lat.s + 0x10u, lat.target);
                    ps2_fh1::rd32(rdram, lat.s + 0x30u, lat.a30);
                    if (lat.r != 0u)
                    {
                        ps2_fh1::rd32(rdram, lat.r + 0x110u, lat.px);
                        ps2_fh1::rd32(rdram, lat.r + 0x114u, lat.py);
                        ps2_fh1::rd32(rdram, lat.r + 0x118u, lat.pz);
                        ps2_fh1::rd32(rdram, lat.r + 0x1e0u, lat.vx);
                        ps2_fh1::rd32(rdram, lat.r + 0x1e4u, lat.vy);
                        ps2_fh1::rd32(rdram, lat.r + 0x1e8u, lat.vz);
                        ps2_fh1::rd32(rdram, lat.r + 0x2f4u, lat.meter);
                        ps2_fh1::rd32(rdram, lat.r + 0x438u, lat.w438);
                        ps2_fh1::rd32(rdram, lat.r + 0xde0u, lat.wde0);
                        ps2_fh1::rd32(rdram, lat.r + 0xde4u, lat.wde4);
                    }
                    g_lim1TrackerLatch = lat;
                }
            }
            const uint32_t ra = getRegU32(ctx, 31);
            g_lim1TrackerNext(rdram, ctx, runtime);
            if (latched && ctx->pc == ra && !ps2_guest_unwind::pending())
            {
                g_lim1TrackerLatch.v0 = getRegU32(ctx, 2);
            }
        }

        void lim1GateWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // Same player filter as the tracker (a0 = R; INP6): rival-rider
            // gate calls just bump others.
            bool latched = false;
            if (g_padMarkerArmed.load(std::memory_order_relaxed))
            {
                const uint32_t a0 = getRegU32(ctx, 4);
                uint32_t player = 0u;
                if (!lim1PlayerR(rdram, player))
                {
                    player = a0; // chain dangling: take every call unfiltered
                }
                if (a0 != player)
                {
                    ++g_lim1GateLatch.others;
                }
                else
                {
                    latched = true;
                    MarkerGateLatch lat{};
                    lat.have = true;
                    lat.tick = lim1VsyncTick(runtime);
                    lat.half = ps2_fh1::stockHalfTicks();
                    lat.a0 = a0;
                    lat.a1 = getRegU32(ctx, 5);
                    lat.a2 = getRegU32(ctx, 6);
                    lat.a3 = getRegU32(ctx, 7);
                    lat.f0 = lim1FpuBits(ctx, 0);
                    lat.f1 = lim1FpuBits(ctx, 1);
                    lat.f12 = lim1FpuBits(ctx, 12);
                    lat.f20 = lim1FpuBits(ctx, 20);
                    lat.f21 = lim1FpuBits(ctx, 21);
                    lat.f13 = lim1FpuBits(ctx, 13);
                    lat.f14 = lim1FpuBits(ctx, 14);
                    g_lim1GateLatch = lat;
                }
            }
            const uint32_t ra = getRegU32(ctx, 31);
            g_lim1GateNext(rdram, ctx, runtime);
            if (latched && ctx->pc == ra && !ps2_guest_unwind::pending())
            {
                g_lim1GateLatch.v0 = getRegU32(ctx, 2);
                g_lim1GateLatch.v1 = getRegU32(ctx, 3);
            }
        }

        void lim1Input2Wrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // Bank discovery only (same guard as input2's own wrapper); each
            // ring row snapshots the records at read time, so wrap order vs
            // input2's own wrapper cannot matter.
            if (g_padMarkerArmed.load(std::memory_order_relaxed))
            {
                const uint32_t bank = getRegU32(ctx, 4) & 0x1ffffffu;
                const uint32_t count = getRegU32(ctx, 5);
                if (count <= 24u && bank <= 0x2000000u - 4u - 24u * 28u)
                {
                    g_lim1Input2Bank = bank;
                    g_lim1Input2Count = count;
                    ++g_lim1Input2Wrapped;
                }
            }
            g_lim1Input2Next(rdram, ctx, runtime);
        }

        void lim1Apply(PS2Runtime &runtime)
        {
            const char *v = std::getenv("PS2X_INPUT_MARKER");
            if (!v || std::strcmp(v, "1") != 0)
            {
                return;
            }
            if (!padMarkerArm(padMarkerDefaultDir(), "env"))
            {
                return;
            }
            std::atexit(padMarkerExitFlush);
            int tracker = 0, gate = 0, input2 = 0;
            g_lim1TrackerNext = runtime.lookupFunction(0x133308u);
            if (g_lim1TrackerNext && runtime.replaceFunction(0x133308u, &lim1TrackerWrapper))
            {
                tracker = 1;
            }
            g_lim1GateNext = runtime.lookupFunction(0x114db8u);
            if (g_lim1GateNext && runtime.replaceFunction(0x114db8u, &lim1GateWrapper))
            {
                gate = 1;
            }
            g_lim1Input2Next = runtime.lookupFunction(0x321298u);
            if (g_lim1Input2Next && runtime.replaceFunction(0x321298u, &lim1Input2Wrapper))
            {
                input2 = 1;
            }
            std::fprintf(stderr, "input-marker: hooks tracker=%d gate=%d input2=%d\n", tracker,
                         gate, input2);
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
            uint16_t backendWord = 0u; // IN4: pre-script port word for the guest layer
            if (!useOverride)
            {
                uint8_t backendData[32]{};
                if (runtime && runtime->padBackend().readState(port, slot, backendData, sizeof(backendData)))
                {
                    state.buttons = static_cast<uint16_t>(backendData[2] | (backendData[3] << 8));
                    backendWord = state.buttons;
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

            // E33: vsync-clock scripts read guest time from the GS vsync
            // tick; wall-clock scripts ignore it. Null runtime (tests) = 0.
            const uint64_t guestVsyncTick =
                runtime ? runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u;
            padScriptOnRead(state, guestVsyncTick); // E31 DEV-ONLY: no-op unless PS2X_PAD_SCRIPT set
            tk12OnRead(state, guestVsyncTick, runtime, port); // APH1 DEV-ONLY: no-op unless PS2X_TK12_AP_ROUTE set
            padRecordOnRead(state, guestVsyncTick, port, slot); // IR1 DEV-ONLY: no-op unless PS2X_PAD_RECORD set
            padMarkerOnRead(state, guestVsyncTick, port, slot,
                              runtime); // LIM1 DEV-ONLY: no-op unless PS2X_INPUT_MARKER=1

            // IN4 DEV-ONLY PS2X_INPUT_DIAG=1: guest layer, each transition of
            // the port-0 pad word the guest consumed (backend word,
            // pre-script; transition only, zero cost off).
            if (port == 0 && usedBackend)
                ps2x::inputdiag::noteGuest(backendWord, guestVsyncTick);
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
        setReturnU32(ctx, g_padFrameCount++);
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
            std::memset(dmaStr, 0, 32);
        // E44 Part-3 EE watch (dev-only, default off).
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
        const bool e3ok = readPadPortData(port, slot, runtime, data, dataAddr);
        // E44 Part-3 EE watch (dev-only, default off).
        if (ps2_e41_trace::plantArmed()) // E41 plant watch
            ps2_e41_trace::notePlantRange(ps2_e41_trace::lastVsyncTick(), dataAddr,
                                          32u, rdram, "pad-read", "pad", 0u);
        if (!e3ok)
        {
            setReturnS32(ctx, 0);
            return;
        }
        // TK7: course picker chord (no-op unless PS2X_SSX3_COURSE_PICKER=1 armed it).
        if (port == 0)
        {
            std::string pickerStatus;
            if (ps2_ssx3_course::pickerOnPadRead(
                    rdram, data, runtime ? runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u,
                    pickerStatus))
                ps2_savestate::noteQuickStatus(pickerStatus); // status line + Android toast (UX1)
        }
        // BTS1: boot turbo yields on any pad button press (active-low bytes;
        // sticks ignored so rest jitter cannot end the turbo). One bool check
        // when the knob is off.
        if (data[2] != 0xFFu || data[3] != 0xFFu)
            ps2_turbo_boot::notePadInput(
                runtime ? runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u);

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
                    char padReadLine[160];
                    std::snprintf(padReadLine, sizeof(padReadLine),
                                  "[padread] port=%d slot=%d data2=0x%02x data3=0x%02x guestButtons=0x%04x enter=%d gamepadStart=%d",
                                  port, slot, data[2], data[3], guestButtons,
                                  IsKeyDown(KEY_ENTER) ? 1 : 0, gamepadStartPressed ? 1 : 0);
                    ps2_log::emitLine(padReadLine); // LG1: line-atomic (was stdout printf)
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
        std::strncpy(buf, text, 31);
        buf[31] = '\0';
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

        std::strncpy(buf, text, 31);
        buf[31] = '\0';
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

    bool parsePadScriptFile(const char *path, std::vector<PadScriptEntry> &entries)
    {
        std::string content;
        if (!padScriptReadFile(path, content))
        {
            return false;
        }
        return padScriptParse(content.c_str(), entries);
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

    bool setPadScriptFromFileForTest(const char *path)
    {
        std::vector<PadScriptEntry> parsed;
        if (!parsePadScriptFile(path, parsed))
        {
            return false;
        }
        std::string content;
        padScriptReadFile(path, content);
        std::lock_guard<std::mutex> lock(g_padScript.mutex);
        g_padScript.initDone = true;
        g_padScript.legacyEventsClock = padScriptWantsLegacyEventsClock(content, parsed);
        padScriptInstallLocked(parsed, "test");
        g_padScriptInitDone.store(true, std::memory_order_relaxed);
        return true;
    }

    bool padScriptLegacyEventsClockForTest()
    {
        std::lock_guard<std::mutex> lock(g_padScript.mutex);
        return g_padScript.legacyEventsClock;
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

    // APH1 test hooks: reset the autopilot latch and report whether the
    // route env arms it (inert unless PS2X_TK12_AP_ROUTE names a route).
    // Production code paths never call these.
    void clearTk12ForTest()
    {
        std::lock_guard<std::mutex> lock(g_tk12Mutex);
        g_tk12 = Tk12Autopilot{};
    }

    bool tk12ArmedForTest()
    {
        std::lock_guard<std::mutex> lock(g_tk12Mutex);
        if (!g_tk12.init)
            tk12Init();
        return g_tk12.on;
    }

    void clearPadScriptForTest()
    {
        std::lock_guard<std::mutex> lock(g_padScript.mutex);
        g_padScript.initDone = true;
        g_padScript.enabled = false;
        g_padScript.vsyncClock = false;
        g_padScript.legacyEventsClock = false;
        g_padScript.testVsyncSet = false;
        g_padScript.testVsyncTick = 0u;
        g_padScript.entries.clear();
        g_padScript.testNowSet = false;
        g_padScript.testNowMs = 0u;
        g_padScriptArmed.store(false, std::memory_order_relaxed);
        g_padScriptInitDone.store(true, std::memory_order_relaxed);
    }

    bool setPadRecordForTest(const char *path)
    {
        if (!path || path[0] == '\0')
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(g_padRecord.mutex);
        padRecordResetLocked();
        g_padRecord.initDone = true;
        padRecordArmLocked(path, "test");
        g_padRecordInitDone.store(true, std::memory_order_relaxed);
        return g_padRecord.enabled;
    }

    void setPadRecordTickForTest(uint64_t tick)
    {
        std::lock_guard<std::mutex> lock(g_padRecord.mutex);
        g_padRecord.testTickSet = true;
        g_padRecord.testTick = tick;
    }

    void closePadRecordForTest()
    {
        std::lock_guard<std::mutex> lock(g_padRecord.mutex);
        padRecordFinalizeLocked();
        padRecordResetLocked();
        g_padRecord.initDone = true;
        g_padRecord.testTickSet = false;
        g_padRecord.testTick = 0u;
        g_padRecordInitDone.store(true, std::memory_order_relaxed);
    }

    void clearPadRecordForTest()
    {
        std::lock_guard<std::mutex> lock(g_padRecord.mutex);
        padRecordResetLocked();
        g_padRecord.initDone = true;
        g_padRecord.testTickSet = false;
        g_padRecord.testTick = 0u;
        g_padRecordInitDone.store(true, std::memory_order_relaxed);
    }

    bool setPadRecordDirForTest(const char *dir, uint64_t keep)
    {
        if (!dir || dir[0] == '\0')
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(g_padRecord.mutex);
        padRecordResetLocked();
        g_padRecord.initDone = true;
        padRecordArmDirLocked(dir, keep, "test");
        g_padRecordInitDone.store(true, std::memory_order_relaxed);
        return g_padRecord.enabled;
    }

    void padRecordFlushNow(const char *reason)
    {
        // PR3: emit the open span through the last observed read and keep
        // recording from the next tick with the same state (the file stays
        // contiguous), then flush, so a kill after this point loses nothing.
        if (!g_padRecordArmed.load(std::memory_order_relaxed))
        {
            return;
        }
        std::lock_guard<std::mutex> lock(g_padRecord.mutex);
        if (!g_padRecord.enabled || g_padRecord.capped || !g_padRecord.file)
        {
            return;
        }
        if (g_padRecord.haveOpen)
        {
            padRecordEmitLocked(g_padRecord.openMs, g_padRecord.lastNextMs);
            g_padRecord.openTick = g_padRecord.lastTick + 1u;
            g_padRecord.openMs = g_padRecord.lastNextMs;
        }
        std::fflush(g_padRecord.file);
        g_padRecord.lastFlushTick = g_padRecord.lastTick;
        g_padRecord.entriesSinceFlush = 0u;
        std::fprintf(stderr, "[padrecord] flushed reason=%s entries=%llu lastTick=%llu\n",
                     reason ? reason : "?", static_cast<unsigned long long>(g_padRecord.totalEntries),
                     static_cast<unsigned long long>(g_padRecord.lastTick));
    }

    void padRecordNoteLoad(uint64_t loadedTick)
    {
        padRecordEnsureInit();
        std::lock_guard<std::mutex> lock(g_padRecord.mutex);
        if (!g_padRecord.enabled || g_padRecord.capped)
        {
            return;
        }
        // Close the pre-load segment: emit the open span through the last
        // observed tick so the file stays contiguous.
        if (g_padRecord.haveOpen && g_padRecord.file)
        {
            padRecordEmitLocked(g_padRecord.openMs, g_padRecord.lastNextMs);
            g_padRecord.haveOpen = false;
        }
        if (g_padRecord.dirMode)
        {
            if (g_padRecord.file)
            {
                std::fflush(g_padRecord.file);
                std::fclose(g_padRecord.file);
                g_padRecord.file = nullptr;
            }
            // A new session file in the same dir; its header notes the load
            // (same keep; the prune counts the just-closed segment).
            g_padRecord.haveLoadNote = true;
            g_padRecord.loadTickNote = loadedTick;
            g_padRecord.firstEntry = true;
            padRecordArmDirLocked(g_padRecord.dir.c_str(), g_padRecord.keep, "load");
            g_padRecord.haveLoadNote = false;
            if (!g_padRecord.file)
            {
                // Arm failed loudly; switch off (a null file with enabled
                // set would crash the next flush).
                g_padRecord.enabled = false;
                g_padRecordArmed.store(false, std::memory_order_relaxed);
                return;
            }
        }
        else if (g_padRecord.file)
        {
            // One file: a comment marker (the script parser skips full-line
            // '#' comments) plus a fresh segment. Replay tools split at it:
            // the post-load entries rewind past the pre-load ones.
            char stamp[32] = {0};
            padRecordUtc(stamp, sizeof(stamp), std::time(nullptr), "%Y-%m-%dT%H:%M:%SZ");
            std::fprintf(g_padRecord.file, "\n# --- new segment: state loaded (tick %llu) at %s ---\n",
                         static_cast<unsigned long long>(loadedTick), stamp);
            std::fflush(g_padRecord.file);
        }
        // Fresh open state at the rewound clock. Without this OnRead drops
        // every post-load read as "clock moved backwards" until the tick
        // catches up, then emits a span with the stale pre-load state.
        g_padRecord.haveOpen = false;
        g_padRecord.openTick = 0u;
        g_padRecord.openMs = 0u;
        g_padRecord.openState = PadInputState{};
        g_padRecord.lastTick = 0u;
        g_padRecord.lastNextMs = 0u;
        g_padRecord.lastFlushTick = 0u;
        g_padRecord.entriesSinceFlush = 0u;
        std::fprintf(stderr, "[padrecord] new segment after state load (tick %llu) path=%s\n",
                     static_cast<unsigned long long>(loadedTick), g_padRecord.path.c_str());
    }

    bool setPadMarkerDirForTest(const char *dir)
    {
        if (!dir || dir[0] == '\0')
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(g_padMarker.mutex);
        padMarkerResetLocked();
        padMarkerArmLocked(dir, "test");
        return g_padMarker.enabled;
    }

    void setPadMarkerTickForTest(uint64_t tick)
    {
        std::lock_guard<std::mutex> lock(g_padMarker.mutex);
        g_padMarker.testTickSet = true;
        g_padMarker.testTick = tick;
    }

    void setPadMarkerTrackerForTest(uint32_t s, uint32_t in0, uint32_t grab, uint32_t phase,
                                   uint32_t target)
    {
        std::lock_guard<std::mutex> lock(g_padMarker.mutex);
        MarkerTrackerLatch lat{};
        lat.have = true;
        lat.tick = g_padMarker.testTick;
        lat.s = s;
        lat.in0 = in0;
        lat.in1 = 0x5a5a5a5au;
        lat.s0 = 0x11u;
        lat.grab = grab;
        lat.grabId = 0xFFFFFFFFu;
        lat.phase = phase;
        lat.target = target;
        lat.a30 = 0x22u;
        lat.r = 0x01400000u;
        lat.px = 0x3F800000u; // 1.0
        lat.py = 0x40000000u; // 2.0
        lat.pz = 0x40400000u; // 3.0
        lat.vx = 0x40800000u; // 4.0
        lat.vy = 0x40900000u; // 4.5
        lat.vz = 0x40A00000u; // 5.0
        lat.meter = 0x33u;
        lat.w438 = 0x44u;
        lat.wde0 = 0x55u;
        lat.wde4 = 0x66u;
        lat.v0 = 0x77u;
        g_lim1TrackerLatch = lat;
    }

    void setPadMarkerGateForTest(uint32_t a0, uint32_t v0)
    {
        std::lock_guard<std::mutex> lock(g_padMarker.mutex);
        MarkerGateLatch lat{};
        lat.have = true;
        lat.tick = g_padMarker.testTick;
        lat.a0 = a0;
        lat.a1 = 0x11111111u;
        lat.a2 = 0x22222222u;
        lat.a3 = 0x33333333u;
        lat.f0 = 0x3F800000u; // 1.0
        lat.f1 = 0x40000000u; // 2.0
        lat.f12 = 0x40400000u; // 3.0
        lat.f20 = 0x40800000u; // 4.0
        lat.f21 = 0x40A00000u; // 5.0
        lat.f13 = 0x40C00000u; // 6.0
        lat.f14 = 0x40E00000u; // 7.0
        lat.v0 = v0;
        lat.v1 = 0x99u;
        g_lim1GateLatch = lat;
    }

    void setPadMarkerInput2ForTest(uint32_t bank, uint32_t count)
    {
        std::lock_guard<std::mutex> lock(g_padMarker.mutex);
        g_lim1Input2Bank = bank;
        g_lim1Input2Count = count <= 24u ? count : 0u;
        ++g_lim1Input2Wrapped;
    }

    void setPadMarkerRdramForTest(const uint8_t *rdram)
    {
        std::lock_guard<std::mutex> lock(g_padMarker.mutex);
        g_lim1TestRdram = rdram;
    }

    void closePadMarkerForTest()
    {
        std::lock_guard<std::mutex> lock(g_padMarker.mutex);
        for (const MarkerPending &p : g_padMarker.pending)
        {
            padMarkerWriteLocked(p, true);
        }
        padMarkerResetLocked();
    }

    void clearPadMarkerForTest()
    {
        std::lock_guard<std::mutex> lock(g_padMarker.mutex);
        padMarkerResetLocked();
    }

    // LIM1: arm the marker + wrap the tracker/gate/input2 call sites when
    // PS2X_INPUT_MARKER=1. Knob off: nothing is wrapped (zero cost, det-identical).
    PS2_REGISTER_GAME_OVERRIDE("ssx3-input-marker", "SLUS_207.72", 0x00100008u, 0u, lim1Apply);
}

// RPL3: recorded host values (ps2_rpl3_nv.h). Record and replay key off
// existing env (record path / script path); play without either is one extra
// call plus a relaxed check per host read.
namespace ps2_rpl3
{
    using namespace ps2_stubs;

    void noteRtc(uint64_t tick, uint32_t ra, const uint8_t bytes[8])
    {
        uint64_t seq = 0u;
        {
            std::lock_guard<std::mutex> lock(g_rpl3Mutex);
            if (!rpl3RecordWantedLocked())
                return;
            seq = g_rpl3.rtcSeq++;
        }
        char hex[17] = {0};
        static const char *digits = "0123456789abcdef";
        for (int i = 0; i < 8; ++i)
        {
            hex[2 * i] = digits[bytes[i] >> 4];
            hex[2 * i + 1] = digits[bytes[i] & 0xF];
        }
        char line[128];
        std::snprintf(line, sizeof(line), "# NV rtc seq=%llu tick=%s ra=0x%x bytes=%s",
                      static_cast<unsigned long long>(seq), rpl3FormatTick(tick).c_str(), ra,
                      hex);
        rpl3EmitLine(line);
    }

    void noteMc(uint64_t tick, const char *query, const uint16_t *sets, size_t entryCount,
                size_t served)
    {
        if (!sets && entryCount != 0u)
            return;
        uint64_t seq = 0u;
        {
            std::lock_guard<std::mutex> lock(g_rpl3Mutex);
            if (!rpl3RecordWantedLocked())
                return;
            seq = g_rpl3.mcSeq++;
        }
        std::string line = "# NV mc seq=" + std::to_string(seq) + " tick=" +
                           rpl3FormatTick(tick) + " q='" + rpl3SanitizeQuery(query) + "' built=" +
                           std::to_string(entryCount) + " served=" + std::to_string(served) +
                           " n=" + std::to_string(entryCount) + " e=";
        char group[160];
        for (size_t i = 0u; i < entryCount; ++i)
        {
            const uint16_t *s = sets + i * 12u;
            std::snprintf(group, sizeof(group), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", s[0],
                          s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11]);
            if (i != 0u)
                line += ';';
            line += group;
        }
        rpl3EmitLine(std::move(line));
    }

    void noteTz(uint64_t tick, int minutes)
    {
        uint64_t seq = 0u;
        {
            std::lock_guard<std::mutex> lock(g_rpl3Mutex);
            if (!rpl3RecordWantedLocked())
                return;
            seq = g_rpl3.tzSeq++;
        }
        char line[96];
        std::snprintf(line, sizeof(line), "# NV tz seq=%llu tick=%s mins=%d",
                      static_cast<unsigned long long>(seq), rpl3FormatTick(tick).c_str(),
                      minutes);
        rpl3EmitLine(line);
    }

    bool takeRtc(uint8_t out[8])
    {
        std::lock_guard<std::mutex> lock(g_rpl3Mutex);
        rpl3EnsureReplayLoadedLocked();
        if (g_rpl3.rtcCur >= g_rpl3.rtc.size())
        {
            if (!g_rpl3.rtc.empty() && !g_rpl3.rtcExhaustWarned)
            {
                g_rpl3.rtcExhaustWarned = true;
                std::fprintf(stderr,
                             "[rpl3] rtc values exhausted at seq=%zu; host behavior resumes\n",
                             g_rpl3.rtcCur);
            }
            return false;
        }
        std::memcpy(out, g_rpl3.rtc[g_rpl3.rtcCur].b, 8u);
        ++g_rpl3.rtcCur;
        return true;
    }

    bool takeMcDate(uint16_t out[6])
    {
        std::lock_guard<std::mutex> lock(g_rpl3Mutex);
        rpl3EnsureReplayLoadedLocked();
        if (g_rpl3.mcCur >= g_rpl3.mc.size())
        {
            if (!g_rpl3.mc.empty() && !g_rpl3.mcExhaustWarned)
            {
                g_rpl3.mcExhaustWarned = true;
                std::fprintf(stderr,
                             "[rpl3] mc dates exhausted at seq=%zu; host behavior resumes\n",
                             g_rpl3.mcCur);
            }
            return false;
        }
        std::memcpy(out, g_rpl3.mc[g_rpl3.mcCur].v, sizeof(uint16_t) * 6u);
        ++g_rpl3.mcCur;
        return true;
    }

    bool takeTz(int &out)
    {
        std::lock_guard<std::mutex> lock(g_rpl3Mutex);
        rpl3EnsureReplayLoadedLocked();
        if (!g_rpl3.haveTz)
            return false;
        out = g_rpl3.tz;
        return true;
    }

    void noteSched(uint64_t tick, uint64_t cycle, const SchedEvent *fired, size_t firedCount,
                   size_t withheldCount)
    {
        if (!fired && firedCount != 0u)
            return;
        uint64_t seq = 0u;
        {
            std::lock_guard<std::mutex> lock(g_rpl3Mutex);
            if (!rpl3RecordWantedLocked())
                return;
            seq = g_rpl3.schedSeq++;
        }
        std::string line = "# NV sched seq=" + std::to_string(seq) + " tick=" +
                           rpl3FormatTick(tick) + " cycle=" + std::to_string(cycle) +
                           " nf=" + std::to_string(firedCount) + " w=" +
                           std::to_string(withheldCount) + " f=";
        char triple[64];
        for (size_t i = 0u; i < firedCount; ++i)
        {
            std::snprintf(triple, sizeof(triple), "%c,%u,%llu", schedTypeChar(fired[i].type),
                          fired[i].id, static_cast<unsigned long long>(fired[i].cycle));
            if (i != 0u)
                line += ';';
            line += triple;
        }
        rpl3EmitLine(std::move(line));
    }

    bool takeSched(uint64_t cycle, const SchedEvent *due, size_t dueCount, bool *fire)
    {
        if (!due && dueCount != 0u)
            return false;
        std::lock_guard<std::mutex> lock(g_rpl3Mutex);
        rpl3EnsureReplayLoadedLocked();
        while (g_rpl3.schedCur < g_rpl3.sched.size() && g_rpl3.sched[g_rpl3.schedCur].cycle < cycle)
        {
            if (!g_rpl3.schedStaleWarned)
            {
                g_rpl3.schedStaleWarned = true;
                std::fprintf(stderr,
                             "[rpl3] sched gate at cycle=%llu skipped (replay at %llu); "
                             "already diverged\n",
                             static_cast<unsigned long long>(g_rpl3.sched[g_rpl3.schedCur].cycle),
                             static_cast<unsigned long long>(cycle));
            }
            ++g_rpl3.schedCur;
        }
        if (g_rpl3.schedCur >= g_rpl3.sched.size() || g_rpl3.sched[g_rpl3.schedCur].cycle != cycle)
            return false;
        for (size_t i = 0u; i < dueCount; ++i)
            fire[i] = false;
        for (const SchedEvent &g : g_rpl3.sched[g_rpl3.schedCur].fired)
        {
            bool found = false;
            for (size_t i = 0u; i < dueCount; ++i)
            {
                if (!fire[i] && due[i].type == g.type && due[i].id == g.id &&
                    due[i].cycle == g.cycle)
                {
                    fire[i] = true;
                    found = true;
                    break;
                }
            }
            if (!found && !g_rpl3.schedMismatchWarned)
            {
                g_rpl3.schedMismatchWarned = true;
                std::fprintf(stderr,
                             "[rpl3] sched gate at cycle=%llu names %c,%u,%llu, not cycle-due; "
                             "already diverged\n",
                             static_cast<unsigned long long>(cycle), schedTypeChar(g.type), g.id,
                             static_cast<unsigned long long>(g.cycle));
            }
        }
        ++g_rpl3.schedCur;
        return true;
    }

    void noteIdle(uint64_t tick, uint64_t cycle, uint64_t timerCycle, uint64_t eventCycle,
                  char winner, bool agree)
    {
        if (agree)
            return; // replay's cycle rule picks the agreed side; no line needed
        uint64_t seq = 0u;
        {
            std::lock_guard<std::mutex> lock(g_rpl3Mutex);
            if (!rpl3RecordWantedLocked())
                return;
            seq = g_rpl3.idleSeq++;
        }
        char line[160];
        std::snprintf(line, sizeof(line),
                      "# NV idlerace seq=%llu tick=%s cycle=%llu timer=%llu event=%llu winner=%c "
                      "agree=%d",
                      static_cast<unsigned long long>(seq), rpl3FormatTick(tick).c_str(),
                      static_cast<unsigned long long>(cycle), static_cast<unsigned long long>(timerCycle),
                      static_cast<unsigned long long>(eventCycle), winner, agree ? 1 : 0);
        rpl3EmitLine(line);
    }

    bool takeIdle(uint64_t cycle, uint64_t timerCycle, uint64_t eventCycle, char &winner)
    {
        std::lock_guard<std::mutex> lock(g_rpl3Mutex);
        rpl3EnsureReplayLoadedLocked();
        while (g_rpl3.idleCur < g_rpl3.idle.size() && g_rpl3.idle[g_rpl3.idleCur].cycle < cycle)
        {
            if (!g_rpl3.idleStaleWarned)
            {
                g_rpl3.idleStaleWarned = true;
                std::fprintf(stderr,
                             "[rpl3] idle gate at cycle=%llu skipped (replay at %llu); "
                             "already diverged\n",
                             static_cast<unsigned long long>(g_rpl3.idle[g_rpl3.idleCur].cycle),
                             static_cast<unsigned long long>(cycle));
            }
            ++g_rpl3.idleCur;
        }
        if (g_rpl3.idleCur >= g_rpl3.idle.size() || g_rpl3.idle[g_rpl3.idleCur].cycle != cycle)
            return false;
        const IdleGate &gate = g_rpl3.idle[g_rpl3.idleCur];
        if ((gate.timerCycle != timerCycle || gate.eventCycle != eventCycle) &&
            !g_rpl3.idleMismatchWarned)
        {
            g_rpl3.idleMismatchWarned = true;
            std::fprintf(stderr,
                         "[rpl3] idle gate at cycle=%llu has timer=%llu event=%llu, replay has "
                         "%llu/%llu; already diverged\n",
                         static_cast<unsigned long long>(cycle),
                         static_cast<unsigned long long>(gate.timerCycle),
                         static_cast<unsigned long long>(gate.eventCycle),
                         static_cast<unsigned long long>(timerCycle),
                         static_cast<unsigned long long>(eventCycle));
        }
        winner = gate.winner;
        ++g_rpl3.idleCur;
        return true;
    }

    void resetForTest()
    {
        std::lock_guard<std::mutex> lock(g_rpl3Mutex);
        g_rpl3 = Rpl3State{};
    }
} // namespace ps2_rpl3

// SS1 save states: pad ports and override. The pad script needs no cursor:
// it recomputes from gs().vsyncTick, so a loaded run needs the same env.
#include "runtime/ps2_savestate.h"
namespace
{
    void padSavestateSave(ps2_savestate::Writer &w)
    {
        using namespace ps2_stubs;
        static_assert(std::is_trivially_copyable_v<PadPortState>, "PadPortState");
        static_assert(std::is_trivially_copyable_v<PadInputState>, "PadInputState");
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        std::lock_guard<std::mutex> overrideLock(g_padOverrideMutex);
        w.pod(g_padPorts);
        w.b(g_padOverrideEnabled);
        w.pod(g_padOverrideState);
        w.u32(g_padFrameCount.load());
    }
    bool padSavestateLoad(ps2_savestate::Reader &r)
    {
        using namespace ps2_stubs;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        std::lock_guard<std::mutex> overrideLock(g_padOverrideMutex);
        r.pod(g_padPorts);
        g_padOverrideEnabled = r.b();
        r.pod(g_padOverrideState);
        g_padFrameCount.store(r.u32());
        return r.ok();
    }
    const bool kPadSavestateRegistered =
        ps2_savestate::registerSection("stub:pad", {1u, &padSavestateSave, &padSavestateLoad, nullptr});
}
