#include "Common.h"
#include "ps2_build_id.h"
#include "ps2_e3.h"
#include "ps2_fh1_full120.h"
#include "ps2_e41_trace.h"
#include "ps2_e44_trace.h"
#include "ps2_pad_latch.h"
#include "ps2_record_env.h"
#include "ps2_ssx3_course_manifest.h"
#include "runtime/ps2_savestate.h"
#include "Pad.h"
#include "ps2_log.h"

#include <algorithm>
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

        // FH17: a recording stamped on the FH5 events clock ('# padrec v1'
        // header) keeps that clock on replay. PS2X_PAD_SCRIPT_EVENTS_CLOCK=
        // fh5|exact overrides the header for any script.
        bool padScriptWantsLegacyEventsClock(const std::string &text)
        {
            if (const char *v = std::getenv("PS2X_PAD_SCRIPT_EVENTS_CLOCK"))
            {
                if (std::strcmp(v, "fh5") == 0)
                    return true;
                if (std::strcmp(v, "exact") == 0)
                    return false;
            }
            return text.rfind("# padrec v1\n", 0) == 0 || text.rfind("# padrec v1\r\n", 0) == 0;
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
                g_padScript.legacyEventsClock = padScriptWantsLegacyEventsClock(content);
                padScriptInstallLocked(parsed, "file");
                return;
            }
            std::vector<PadScriptEntry> parsed;
            if (!padScriptParse(spec, parsed))
            {
                std::fprintf(stderr, "[padscript] ignoring malformed PS2X_PAD_SCRIPT\n");
                return;
            }
            g_padScript.legacyEventsClock = padScriptWantsLegacyEventsClock(spec);
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
            char sim[64], vu1[64], mtvu[64], finish[64], det[64], vfloat[64], revdma[64], mcroot[512];
            std::snprintf(sim, sizeof(sim), "%s", padRecordKnob("PS2X_SSX3_SIM_MODE"));
            std::snprintf(vu1, sizeof(vu1), "%s", padRecordKnob("PS2X_VU1_ENGINE"));
            std::snprintf(mtvu, sizeof(mtvu), "%s", padRecordKnob("PS2X_MTVU"));
            std::snprintf(finish, sizeof(finish), "%s", padRecordKnob("PS2X_GS_FINISH_TIMING"));
            std::snprintf(det, sizeof(det), "%s", padRecordKnob("PS2X_DETERMINISTIC"));
            std::snprintf(vfloat, sizeof(vfloat), "%s", padRecordKnob("PS2X_VU_FLOAT"));
            std::snprintf(revdma, sizeof(revdma), "%s", padRecordKnob("PS2X_VIF1_REVERSE_DMA"));
            std::snprintf(mcroot, sizeof(mcroot), "%s", padRecordKnob("PS2X_MC_ROOT"));
            padRecordSanitize(sim);
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
            // v1 = events-mode spans stamped on the FH5 clock (half a tick
            // early), which replays keep (padScriptWantsLegacyEventsClock).
            const bool fh5Stamps = ps2_fh1::eventsMode() && !ps2_fh1::exactStockClock();
            std::fprintf(f,
                         "# padrec %s\n"
                         "# start_utc=%s\n"
                         "# build=%s\n"
                         "# env_sha=%s\n"
                         "# knobs SIM_MODE=%s VU1_ENGINE=%s MTVU=%s FINISH_TIMING=%s "
                         "DETERMINISTIC=%s VU_FLOAT=%s VIF1_REVERSE_DMA=%s\n",
                         fh5Stamps ? "v1" : "v2", startUtc, ps2x::buildId(), ps2x::recordedEnvFileHash(), sim,
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
            padRecordOnRead(state, guestVsyncTick, port, slot); // IR1 DEV-ONLY: no-op unless PS2X_PAD_RECORD set

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
        // TK7: course picker chord (no-op unless PS2X_SSX3_COURSE_PICKER=1 armed it).
        if (port == 0)
        {
            std::string pickerStatus;
            if (ps2_ssx3_course::pickerOnPadRead(
                    rdram, data, runtime ? runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u,
                    pickerStatus))
                ps2_savestate::noteQuickStatus(pickerStatus); // status line + Android toast (UX1)
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
        g_padScript.legacyEventsClock = padScriptWantsLegacyEventsClock(content);
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
}

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
