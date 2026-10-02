// IN4 Part B: PS2X_INPUT_DIAG=1 implementation. See ps2_input_diag.h for the
// layer map and line format. Producers: render thread (SDL watch, publish,
// stat) -> host ring; EE thread (guest, rider) -> guest ring. The flush
// worker (PT3 shape: background thread, utility QoS on Apple, at most one in
// flight) formats and appends to Documents/perf/input-<stamp>.log.
#include "ps2_input_diag.h"
#include "ps2_host_backend.h" // raylib.h (IsGamepadAvailable)
#include "ps2_touch_events.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#if __has_include(<pthread/qos.h>)
#include <pthread/qos.h> // utility QoS for the background flush worker (PT3 shape)
#endif
#endif

#if defined(PS2X_IOS)
// SDL_MAIN_HANDLED: this TU owns no main.
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#endif

namespace ps2x::inputdiag
{
namespace
{
bool diagOn()
{
    static const bool on = enabledFromEnv(std::getenv("PS2X_INPUT_DIAG"));
    return on;
}

bool touchEventsOn()
{
    // IN4 Part A knob (default on; 0 = the old live-list-only path).
    static const bool on = [] {
        const char *v = std::getenv("PS2X_IOS_TOUCH_EVENTS");
        return !(v && v[0] == '0');
    }();
    return on;
}

EventRing<2048> &hostRing()
{
    static EventRing<2048> r;
    return r;
}

EventRing<2048> &guestRing()
{
    static EventRing<2048> r;
    return r;
}

uint64_t dropTotal()
{
    return hostRing().drops() + guestRing().drops();
}

// ---- log file (mirrors perflog defaultDir; own override knob) ----
std::string diagDir()
{
    if (const char *override = std::getenv("PS2X_INPUT_DIAG_DIR"); override && override[0] != '\0')
        return override;
#if defined(PS2X_IOS)
    if (const char *home = std::getenv("HOME"); home && home[0] != '\0')
        return std::string(home) + "/Documents/perf";
    return {};
#elif defined(__ANDROID__)
    return {};
#else
    return "perf";
#endif
}

std::string inputStamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "input-%Y%m%d-%H%M%S", &tm);
    return buf;
}

struct Sink
{
    std::string path;
    bool openFailed = false;
};

Sink &sink()
{
    static Sink s;
    return s;
}

void ensureSink()
{
    Sink &s = sink();
    if (!s.path.empty() || s.openFailed)
        return;
    const std::string dir = diagDir();
    if (dir.empty())
    {
        s.openFailed = true;
        std::fprintf(stderr, "[input] no log dir (HOME/PS2X_INPUT_DIAG_DIR); disabled\n");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    s.path = dir + "/" + inputStamp() + ".log";
    if (std::FILE *probe = std::fopen(s.path.c_str(), "a"))
    {
        std::fclose(probe);
        std::fprintf(stderr, "[input] logging to %s\n", s.path.c_str());
    }
    else
    {
        std::fprintf(stderr, "[input] cannot open %s; disabled\n", s.path.c_str());
        s.openFailed = true;
        s.path.clear();
    }
}

// ---- flush worker (PT3 shape) ----
std::atomic<bool> flushBusy{false};
std::thread flushThread;

void flushWrite(const char *reason)
{
    ensureSink();
    Sink &s = sink();
    if (s.path.empty())
        return;
    Event batch[512];
    uint64_t entries = 0u;
    uint64_t maxTick = 0u;
    std::FILE *out = std::fopen(s.path.c_str(), "a");
    if (!out)
    {
        std::fprintf(stderr, "[input] cannot open %s; flush skipped\n", s.path.c_str());
        return;
    }
    // Both rings, oldest-first per ring; input_read.py sorts by wall across
    // layers (the two producers share the padlatch wall epoch).
    for (int pass = 0; pass < 2; ++pass)
    {
        EventRing<2048> &ring = pass == 0 ? hostRing() : guestRing();
        size_t n = 0;
        do
        {
            n = ring.drain(batch, sizeof(batch) / sizeof(batch[0]));
            for (size_t i = 0; i < n; ++i)
            {
                std::fprintf(out, "%s\n", formatEvent(batch[i]).c_str());
                if (batch[i].tick > maxTick)
                    maxTick = batch[i].tick;
            }
            entries += n;
        } while (n == sizeof(batch) / sizeof(batch[0]));
    }
    std::fprintf(out, "[input-flush] reason=%s tick=%llu entries=%llu drops=%llu\n", reason ? reason : "?",
                 static_cast<unsigned long long>(maxTick), static_cast<unsigned long long>(entries),
                 static_cast<unsigned long long>(dropTotal()));
    std::fflush(out);
    std::fclose(out);
}

void flushSync(const char *reason)
{
    if (flushThread.joinable())
    {
        flushThread.join();
        flushBusy.store(false, std::memory_order_release);
    }
    flushWrite(reason);
}

void requestFlush(const char *reason)
{
    if (flushBusy.exchange(true, std::memory_order_acq_rel))
        return; // one in flight: skip this cadence (the next one retries)
    if (flushThread.joinable())
        flushThread.join();
    flushThread = std::thread([reason] {
#if defined(__APPLE__) && defined(QOS_CLASS_UTILITY)
        (void)::pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#else
        (void)0;
#endif
        flushWrite(reason);
        flushBusy.store(false, std::memory_order_release);
    });
}

// ---- SDL event watch (iOS only: raylib's SDL backend) ----
#if defined(PS2X_IOS)
touchev::Table &touchTable()
{
    static touchev::Table t;
    return t;
}

// Trigger axes (SDL_CONTROLLER_AXIS_TRIGGERLEFT/RIGHT = 4/5): last side of
// the 0.1 line per (which, axis) slot.
struct TrigState
{
    uint32_t which = 0u;
    uint32_t axis = 0u;
    bool above = false;
    bool used = false;
};
TrigState trigStates[8]{};

// SDL which ids seen since the last stat line (IN3 D: instance id vs slot).
uint32_t seenWhich[16]{};
size_t seenWhichN = 0u;

void noteWhich(uint32_t which)
{
    for (size_t i = 0; i < seenWhichN; ++i)
    {
        if (seenWhich[i] == which)
            return;
    }
    if (seenWhichN < sizeof(seenWhich) / sizeof(seenWhich[0]))
        seenWhich[seenWhichN++] = which;
}

bool controllerConnectedNow()
{
    for (int i = 0; i < 8; ++i)
    {
        if (IsGamepadAvailable(i))
            return true;
    }
    return false;
}

// Last render-thread tick for watch-callback lines (set by pollHost and the
// publish path; the watch runs on the render thread during the pump).
std::atomic<uint64_t> renderTick{0};

int sdlWatch(void * /*userdata*/, SDL_Event *ev)
{
    using namespace touchev;
    const uint64_t wall = padlatch::wallMs();
    const uint64_t tick = renderTick.load(std::memory_order_relaxed);
    switch (ev->type)
    {
    case SDL_FINGERMOTION:
        touchTable().motion(static_cast<int64_t>(ev->tfinger.fingerId), ev->tfinger.x, ev->tfinger.y, wall);
        break;
    case SDL_FINGERDOWN:
        touchTable().down(static_cast<int64_t>(ev->tfinger.fingerId), ev->tfinger.x, ev->tfinger.y, wall);
        break;
    case SDL_FINGERUP:
    {
        touchTable().up(static_cast<int64_t>(ev->tfinger.fingerId), wall);
        uint64_t mMs = 0u;
        float mx = 0.0f, my = 0.0f;
        if (touchTable().takeMarker(mMs, mx, my) && diagOn())
        {
            Event e;
            e.wallMs = mMs;
            e.tick = tick;
            e.layer = Layer::Marker;
            e.a = controllerConnectedNow() ? 1u : 0u;
            e.x = mx;
            e.y = my;
            hostRing().push(e);
        }
        break;
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    {
        const uint32_t which = static_cast<uint32_t>(ev->cbutton.which);
        noteWhich(which);
        if (!diagOn())
            break;
        Event e;
        e.wallMs = wall;
        e.tick = tick;
        e.layer = Layer::Sdl;
        e.code = static_cast<uint8_t>(SdlCode::Btn);
        e.a = which;
        e.b = ev->cbutton.button;
        e.c = ev->type == SDL_CONTROLLERBUTTONDOWN ? 1 : 0;
        hostRing().push(e);
        break;
    }
    case SDL_CONTROLLERAXISMOTION:
    {
        // Triggers only (axes 4/5): pressed at axis > 0.1, matching
        // rcore_desktop_sdl.c.
        if (ev->caxis.axis != 4 && ev->caxis.axis != 5)
            break;
        const uint32_t which = static_cast<uint32_t>(ev->caxis.which);
        noteWhich(which);
        if (!diagOn())
            break;
        const bool above = ev->caxis.value > static_cast<int>(0.1f * 32767.0f);
        TrigState *slot = nullptr;
        for (TrigState &t : trigStates)
        {
            if (t.used && t.which == which && t.axis == ev->caxis.axis)
            {
                slot = &t;
                break;
            }
            if (!t.used && !slot)
                slot = &t;
        }
        if (!slot)
            break;
        if (!slot->used)
        {
            slot->used = true;
            slot->which = which;
            slot->axis = ev->caxis.axis;
            slot->above = above;
            break; // baseline, no edge yet
        }
        if (above != slot->above)
        {
            slot->above = above;
            Event e;
            e.wallMs = wall;
            e.tick = tick;
            e.layer = Layer::Sdl;
            e.code = static_cast<uint8_t>(SdlCode::Axis);
            e.a = which;
            e.b = ev->caxis.axis;
            e.c = ev->caxis.value;
            hostRing().push(e);
        }
        break;
    }
    case SDL_CONTROLLERDEVICEADDED:
    {
        // which = device index here (not instance id).
        const int index = ev->cdevice.which;
        noteWhich(static_cast<uint32_t>(index));
        if (!diagOn())
            break;
        Event e;
        e.wallMs = wall;
        e.tick = tick;
        e.layer = Layer::Sdl;
        e.code = static_cast<uint8_t>(SdlCode::Device);
        e.a = static_cast<uint32_t>(index);
        e.c = 1;
        if (const char *name = SDL_GameControllerNameForIndex(index))
            std::snprintf(e.text, sizeof(e.text), "%s", name);
        hostRing().push(e);
        break;
    }
    case SDL_CONTROLLERDEVICEREMOVED:
    {
        const uint32_t which = static_cast<uint32_t>(ev->cdevice.which);
        noteWhich(which);
        if (!diagOn())
            break;
        Event e;
        e.wallMs = wall;
        e.tick = tick;
        e.layer = Layer::Sdl;
        e.code = static_cast<uint8_t>(SdlCode::Device);
        e.a = which;
        e.c = 0;
        hostRing().push(e);
        break;
    }
    default:
        break;
    }
    return 0; // keep the event in the queue for raylib
}

void ensureSdlWatch()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    SDL_AddEventWatch(sdlWatch, nullptr);
}
#endif // PS2X_IOS

// ---- per-thread last-state (transitions only) ----
struct HostState
{
    bool init = false;
    uint16_t lastPublish = 0u;
    uint64_t presents = 0u;
    uint64_t statTick = 0u;
    uint64_t statPresents = 0u;
    uint64_t statWall = 0u;
    std::chrono::steady_clock::time_point lastFlush{};
    bool flushedOnce = false;
};

HostState &hostState()
{
    static HostState s;
    return s;
}

struct GuestState
{
    bool init = false;
    uint16_t lastGuest = 0u; // active-low port-0 word
    uint32_t riderP[8] = {};
    uint32_t riderBits[8] = {};
    uint32_t riderLines = 0u;
};

GuestState &guestState()
{
    static GuestState s;
    return s;
}
} // namespace

bool enabled()
{
    return diagOn();
}

void notePublish(uint16_t mask, uint64_t tick)
{
#if defined(PS2X_IOS)
    renderTick.store(tick, std::memory_order_relaxed);
#else
    (void)tick;
#endif
    if (!diagOn())
        return;
    HostState &s = hostState();
    if (!s.init)
    {
        s.init = true;
        s.lastPublish = mask;
        return; // baseline, no edge yet
    }
    if (mask == s.lastPublish)
        return;
    Event e;
    e.wallMs = padlatch::wallMs();
    e.tick = tick;
    e.layer = Layer::Publish;
    e.a = s.lastPublish;
    e.b = mask;
    s.lastPublish = mask;
    hostRing().push(e);
}

void noteGuest(uint16_t word, uint64_t tick)
{
    if (!diagOn())
        return;
    GuestState &s = guestState();
    if (!s.init)
    {
        s.init = true;
        s.lastGuest = word;
        return;
    }
    if (word == s.lastGuest)
        return;
    Event e;
    e.wallMs = padlatch::wallMs();
    e.tick = tick;
    e.layer = Layer::Guest;
    e.a = s.lastGuest;
    e.b = word;
    s.lastGuest = word;
    guestRing().push(e);
}

void noteRider(uint32_t p, uint32_t oldBits, uint32_t newBits, uint64_t tick)
{
    if (!diagOn())
        return;
    GuestState &s = guestState();
    if (s.riderLines >= 20000u)
        return; // rider cap: watch-bit edges for a full leg fit; count on
    uint32_t slot = 0u;
    while (slot < 8u && s.riderP[slot] != 0u && s.riderP[slot] != p)
        ++slot;
    if (slot == 8u)
        return;
    const bool known = s.riderP[slot] == p;
    s.riderP[slot] = p;
    if (!known)
    {
        s.riderBits[slot] = newBits;
        return; // baseline per rider
    }
    const uint32_t changed = (oldBits ^ newBits) & kRiderWatch;
    if (changed == 0u)
    {
        s.riderBits[slot] = newBits;
        return;
    }
    s.riderBits[slot] = newBits;
    ++s.riderLines;
    Event e;
    e.wallMs = padlatch::wallMs();
    e.tick = tick;
    e.layer = Layer::Rider;
    e.a = p;
    e.b = oldBits;
    e.c = static_cast<int32_t>(newBits);
    guestRing().push(e);
}

void noteSdlButton(uint32_t which, uint32_t button, bool down, uint64_t tick)
{
    if (!diagOn())
        return;
    Event e;
    e.wallMs = padlatch::wallMs();
    e.tick = tick;
    e.layer = Layer::Sdl;
    e.code = static_cast<uint8_t>(SdlCode::Btn);
    e.a = which;
    e.b = button;
    e.c = down ? 1 : 0;
    hostRing().push(e);
}

void noteSdlAxis(uint32_t which, uint32_t axis, int value, uint64_t tick)
{
    if (!diagOn())
        return;
    Event e;
    e.wallMs = padlatch::wallMs();
    e.tick = tick;
    e.layer = Layer::Sdl;
    e.code = static_cast<uint8_t>(SdlCode::Axis);
    e.a = which;
    e.b = axis;
    e.c = value;
    hostRing().push(e);
}

void noteSdlDevice(uint32_t whichOrIndex, bool added, const char *name, uint64_t tick)
{
    if (!diagOn())
        return;
    Event e;
    e.wallMs = padlatch::wallMs();
    e.tick = tick;
    e.layer = Layer::Sdl;
    e.code = static_cast<uint8_t>(SdlCode::Device);
    e.a = whichOrIndex;
    e.c = added ? 1 : 0;
    if (name)
        std::snprintf(e.text, sizeof(e.text), "%s", name);
    hostRing().push(e);
}

void notePresent()
{
    if (!diagOn())
        return;
    ++hostState().presents;
}

void pollHost(uint64_t tick)
{
#if defined(PS2X_IOS)
    ensureSdlWatch(); // Part A touches need the watch even with diag off
    renderTick.store(tick, std::memory_order_relaxed);
#endif
    if (!diagOn())
        return;
    HostState &s = hostState();
    const uint64_t wall = padlatch::wallMs();
    if (s.statWall == 0u)
    {
        s.statWall = wall;
        s.statTick = tick;
        s.statPresents = s.presents;
        s.lastFlush = std::chrono::steady_clock::now();
        return;
    }
    if (wall - s.statWall >= 1000u)
    {
        const double dtS = static_cast<double>(wall - s.statWall) / 1000.0;
        Event e;
        e.wallMs = wall;
        e.tick = tick;
        e.layer = Layer::Stat;
        e.x = static_cast<float>(static_cast<double>(tick - s.statTick) / dtS);
        e.b = s.presents - s.statPresents;
        e.a = static_cast<uint32_t>(dropTotal());
        // slots="i:0/1" (raylib device index -> available) + SDL which ids.
        char *p = e.text;
        size_t left = sizeof(e.text);
        int w = std::snprintf(p, left, "slots=\"");
        if (w > 0 && static_cast<size_t>(w) < left)
        {
            p += w;
            left -= static_cast<size_t>(w);
        }
        for (int i = 0; i < 8 && left > 1; ++i)
        {
            w = std::snprintf(p, left, "%s%d:%d", i ? " " : "", i, IsGamepadAvailable(i) ? 1 : 0);
            if (w <= 0 || static_cast<size_t>(w) >= left)
                break;
            p += w;
            left -= static_cast<size_t>(w);
        }
        w = std::snprintf(p, left, "\" which=\"");
        if (w > 0 && static_cast<size_t>(w) < left)
        {
            p += w;
            left -= static_cast<size_t>(w);
        }
#if defined(PS2X_IOS)
        for (size_t i = 0; i < seenWhichN && left > 1; ++i)
        {
            w = std::snprintf(p, left, "%s%u", i ? " " : "", seenWhich[i]);
            if (w <= 0 || static_cast<size_t>(w) >= left)
                break;
            p += w;
            left -= static_cast<size_t>(w);
        }
        seenWhichN = 0u;
#else
        w = std::snprintf(p, left, "n/a");
        if (w > 0 && static_cast<size_t>(w) < left)
        {
            p += w;
            left -= static_cast<size_t>(w);
        }
#endif
        std::snprintf(p, left, "\"");
        hostRing().push(e);
        s.statWall = wall;
        s.statTick = tick;
        s.statPresents = s.presents;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!s.flushedOnce || std::chrono::duration<double>(now - s.lastFlush).count() >= 5.0)
    {
        s.flushedOnce = true;
        s.lastFlush = now;
        requestFlush("timer");
    }
}

void flush(const char *reason)
{
    if (!diagOn())
        return;
    const bool pause = reason && std::strcmp(reason, "pause") == 0;
    const bool shutdown = reason && std::strcmp(reason, "shutdown") == 0;
    if (pause || shutdown)
        flushSync(reason);
    else
        requestFlush(reason ? reason : "timer");
}

void resetForTest()
{
    hostRing().resetForTest();
    guestRing().resetForTest();
    hostState() = HostState{};
    guestState() = GuestState{};
    if (flushThread.joinable())
    {
        flushThread.join();
        flushBusy.store(false, std::memory_order_release);
    }
    sink() = Sink{};
}

uint64_t dropCountForTest()
{
    return dropTotal();
}

#if defined(PS2X_IOS)
// Seams for ps2_ios_runtime.mm (Part A merge) and the host test.
namespace touchseam
{
touchev::Table &table()
{
    return touchTable();
}
bool eventsOn()
{
    return touchEventsOn();
}
} // namespace touchseam
#endif
} // namespace ps2x::inputdiag
