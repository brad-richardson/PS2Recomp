#pragma once

// IN4 Part B: input-diagnostics knob PS2X_INPUT_DIAG=1 (default off;
// output-only: guest det-hash IDENTICAL, no new key). A lock-free
// single-producer ring per thread family (host ring: render thread;
// guest ring: EE thread), flushed off the host loop the PT3 way
// (background worker, at most one in flight) to
// Documents/perf/input-<stamp>.log (./perf on desktop,
// $PS2X_INPUT_DIAG_DIR when set).
//
// Layers (each line carries wall ms on the padlatch epoch + guest tick):
//   1. SDL layer: every SDL_CONTROLLERBUTTONDOWN/UP and trigger-axis
//      crossing of 0.1 (which, button, value), every controller
//      added/removed. Seen via SDL_AddEventWatch before raylib drains them.
//   2. Sampled layer: each transition of the host pad word published to the
//      IN2 latch, with the bits changed.
//   3. Guest layer: each transition of the port-0 pad word the guest
//      consumed (PSPadBackend::readState output, Pad.cpp site).
//   4. Rider layer (cheap): edges of the player rider action bits L1
//      0x40000 / R2 0x80000 / square (0x10000 edge, 0x20000 hold, 0x2000
//      alt state), ported from IN3's in3-in tap (branch in3, 4774566).
//   5. Brad's marker: a two-finger tap anywhere (ps2_touch_events.h),
//      visible to us, never reaching the guest (virtualPadTouches drops
//      multi-peer touches while a controller is connected; and with a
//      controller in use the vpad branch is skipped anyway, so the overlay
//      path never publishes touches).
//   5b. Touch-source layer (Brad 10-02: touch d-pad misses, mid-screen, so
//      not edge gating): every raw SDL finger down/up (id + normalised
//      position) plus the vpad pressed-bit transitions from updatePad per
//      render pass. A touch press that never becomes vpad bits (one-pump
//      drop, VT1 inert owner) shows here as touch-without-vpad; with Part
//      A on, the merge leg is exact and any loss sits in updatePad
//      ownership. One ride on touch or the Backbone shows where a press
//      stops: touch -> vpad -> pub -> guest -> rider (touch) or
//      sdl -> pub -> guest -> rider (controller).
//   6. Once per second: vs/s, presents, and the raylib gamepad slot in use
//      (IsGamepadAvailable index vs SDL which ids: the IN3 D check).
//
// Budget: with the knob on, producers do one static-bool check per call
// plus a compare, and a ring push only on transitions (button edges are
// rare); zero cost off (one cached bool per call site).
//
// Line format (v1), parsed by local/research/IN4/tooling/input_read.py:
//   [input-sdl] wall=<ms>ms tick=<t> btn which=<w> button=<name> to=down|up
//   [input-sdl] wall=<ms>ms tick=<t> axis which=<w> axis=<ltrigger|rtrigger> value=<v>
//   [input-sdl] wall=<ms>ms tick=<t> device added|removed which=<w> name="<...>"
//   [input-pub] wall=<ms>ms tick=<t> pub 0x<old>->0x<new> +<names> -<names> (active-high pressed)
//   [input-guest] wall=<ms>ms tick=<t> port0 0x<old>->0x<new> +<names> -<names> (active-low word;
//     + = newly pressed)
//   [input-rider] wall=<ms>ms tick=<t> P=0x<p> 0x<old>->0x<new> +<names> -<names>
//   [input-marker] wall=<ms>ms tick=<t> two_finger_tap x=<fx> y=<fy> ctl=<0|1>
//   [input-touch] wall=<ms>ms tick=<t> finger <down|up> id=<u64> x=<fx> y=<fy> (normalised 0..1)
//   [input-vpad] wall=<ms>ms tick=<t> vpad 0x<old>->0x<new> +<names> -<names> (updatePad bits)
//   [input-stat] wall=<ms>ms tick=<t> vsyncs_per_s=<f> presents=<n>
//     slots="<i:0/1 ...>" which="<w ...>" drops=<n>

#include "ps2_pad_latch.h"
#include "ps2_touch_events.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ps2x::inputdiag
{
inline bool enabledFromEnv(const char *value)
{
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

enum class Layer : uint8_t
{
    Sdl = 0,
    Publish = 1,
    Guest = 2,
    Rider = 3,
    Marker = 4,
    Stat = 5,
    Touch = 6, // raw SDL finger down/up (a/b = id lo/hi, x/y normalised)
    Vpad = 7, // updatePad pressed-bit transitions per render pass
};

enum class SdlCode : uint8_t
{
    Btn = 0, // a=which, b=button, c=down(1)/up(0)
    Axis = 1, // a=which, b=axis, c=value (Sint16)
    Device = 2, // a=which-or-index, c=added(1)/removed(0), text=name
};

enum class TouchCode : uint8_t
{
    Down = 0,
    Up = 1,
};

// One structured event (formatted on the flush worker, so producers only
// copy ~104 bytes on transitions).
struct Event
{
    uint64_t wallMs = 0u;
    uint64_t tick = 0u;
    Layer layer = Layer::Stat;
    uint8_t code = 0u;
    uint8_t pad[6] = {};
    uint32_t a = 0u;
    uint32_t b = 0u;
    int32_t c = 0;
    float x = 0.0f;
    float y = 0.0f;
    char text[96] = {}; // device name / stat slots+which payload
};

// SDL game-controller button names by SDL_GameControllerButton number
// (SDL_gamecontroller.h order; stable ABI).
inline const char *sdlButtonName(uint32_t button)
{
    static constexpr const char *kNames[] = {
        "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
        "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2", "paddle3",
        "paddle4", "touchpad",
    };
    return button < sizeof(kNames) / sizeof(kNames[0]) ? kNames[button] : nullptr;
}

// SDL game-controller axis names by SDL_GameControllerAxis number.
inline const char *sdlAxisName(uint32_t axis)
{
    switch (axis)
    {
    case 0:
        return "leftx";
    case 1:
        return "lefty";
    case 2:
        return "rightx";
    case 3:
        return "righty";
    case 4:
        return "ltrigger";
    case 5:
        return "rtrigger";
    default:
        return nullptr;
    }
}

// Rider watch bits (IN3 action-bit table): L1/R2 grabs are level bits held
// for the whole press; square has an edge, a hold and an alt-state bit.
constexpr uint32_t kRiderWatch = 0x40000u | 0x80000u | 0x10000u | 0x20000u | 0x2000u;

inline const char *riderBitName(uint32_t bit)
{
    switch (bit)
    {
    case 0x40000u:
        return "grab_l1";
    case 0x80000u:
        return "grab_r2";
    case 0x10000u:
        return "sq_edge";
    case 0x20000u:
        return "sq_hold";
    case 0x2000u:
        return "sq_alt";
    default:
        return nullptr;
    }
}

// Changed-bit name list for active-HIGH pressed masks (publish layer),
// "+a+b -c" over the rose/fell sets; multi-bit or empty sides print hex.
inline std::string formatHighChanges(uint32_t rose, uint32_t fell)
{
    std::string out;
    char buf[32];
    const auto one = [&](uint32_t bit) -> const char * {
        if (bit & 0xFFFFu)
            return ps2x::padlatch::buttonName(static_cast<uint16_t>(bit));
        return nullptr;
    };
    out += '+';
    if (rose == 0u)
        out += "none";
    else if (const char *n = (rose & (rose - 1u)) == 0u ? one(rose) : nullptr)
        out += n;
    else
    {
        std::snprintf(buf, sizeof(buf), "0x%04x", rose);
        out += buf;
    }
    out += " -";
    if (fell == 0u)
        out += "none";
    else if (const char *n = (fell & (fell - 1u)) == 0u ? one(fell) : nullptr)
        out += n;
    else
    {
        std::snprintf(buf, sizeof(buf), "0x%04x", fell);
        out += buf;
    }
    return out;
}

// Changed-bit name list for the rider watch mask.
inline std::string formatRiderChanges(uint32_t rose, uint32_t fell)
{
    std::string out = "+";
    bool first = true;
    for (uint32_t bit = 1u; bit <= 0x80000u; bit <<= 1u)
    {
        if ((rose & bit) == 0u)
            continue;
        const char *n = riderBitName(bit);
        if (!first)
            out += '+';
        first = false;
        if (n)
            out += n;
        else
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%x", bit);
            out += buf;
        }
    }
    if (first)
        out += "none";
    out += " -";
    first = true;
    for (uint32_t bit = 1u; bit <= 0x80000u; bit <<= 1u)
    {
        if ((fell & bit) == 0u)
            continue;
        const char *n = riderBitName(bit);
        if (!first)
            out += '-';
        first = false;
        if (n)
            out += n;
        else
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%x", bit);
            out += buf;
        }
    }
    if (first)
        out += "none";
    return out;
}

inline std::string formatEvent(const Event &e)
{
    char head[160];
    switch (e.layer)
    {
    case Layer::Sdl:
    {
        if (e.code == static_cast<uint8_t>(SdlCode::Btn))
        {
            const char *n = sdlButtonName(e.b);
            if (n)
                std::snprintf(head, sizeof(head), "[input-sdl] wall=%llums tick=%llu btn which=%u button=%s to=%s",
                              static_cast<unsigned long long>(e.wallMs), static_cast<unsigned long long>(e.tick),
                              e.a, n, e.c ? "down" : "up");
            else
                std::snprintf(head, sizeof(head), "[input-sdl] wall=%llums tick=%llu btn which=%u button=%u to=%s",
                              static_cast<unsigned long long>(e.wallMs), static_cast<unsigned long long>(e.tick),
                              e.a, e.b, e.c ? "down" : "up");
            return head;
        }
        if (e.code == static_cast<uint8_t>(SdlCode::Axis))
        {
            const char *n = sdlAxisName(e.b);
            if (n)
                std::snprintf(head, sizeof(head), "[input-sdl] wall=%llums tick=%llu axis which=%u axis=%s value=%d",
                              static_cast<unsigned long long>(e.wallMs), static_cast<unsigned long long>(e.tick),
                              e.a, n, static_cast<int>(e.c));
            else
                std::snprintf(head, sizeof(head), "[input-sdl] wall=%llums tick=%llu axis which=%u axis=%u value=%d",
                              static_cast<unsigned long long>(e.wallMs), static_cast<unsigned long long>(e.tick),
                              e.a, e.b, static_cast<int>(e.c));
            return head;
        }
        std::snprintf(head, sizeof(head), "[input-sdl] wall=%llums tick=%llu device %s which=%u name=\"%s\"",
                      static_cast<unsigned long long>(e.wallMs), static_cast<unsigned long long>(e.tick),
                      e.c ? "added" : "removed", e.a, e.text);
        return head;
    }
    case Layer::Publish:
    {
        const uint32_t rose = e.b & ~e.a;
        const uint32_t fell = e.a & ~e.b;
        std::snprintf(head, sizeof(head), "[input-pub] wall=%llums tick=%llu pub 0x%04x->0x%04x %s",
                      static_cast<unsigned long long>(e.wallMs), static_cast<unsigned long long>(e.tick), e.a, e.b,
                      formatHighChanges(rose, fell).c_str());
        return head;
    }
    case Layer::Guest:
    {
        // Active-low word: pressed = ~word. Newly pressed = ~new & old.
        const uint32_t rose = ~e.b & e.a;
        const uint32_t fell = ~e.a & e.b;
        std::snprintf(head, sizeof(head), "[input-guest] wall=%llums tick=%llu port0 0x%04x->0x%04x %s",
                      static_cast<unsigned long long>(e.wallMs), static_cast<unsigned long long>(e.tick), e.a, e.b,
                      formatHighChanges(rose & 0xFFFFu, fell & 0xFFFFu).c_str());
        return head;
    }
    case Layer::Rider:
    {
        const uint32_t oldBits = e.b;
        const uint32_t newBits = static_cast<uint32_t>(e.c);
        const uint32_t rose = newBits & ~oldBits;
        const uint32_t fell = oldBits & ~newBits;
        std::snprintf(head, sizeof(head), "[input-rider] wall=%llums tick=%llu P=0x%x 0x%05x->0x%05x %s",
                      static_cast<unsigned long long>(e.wallMs), static_cast<unsigned long long>(e.tick), e.a,
                      oldBits, newBits, formatRiderChanges(rose, fell).c_str());
        return head;
    }
    case Layer::Marker:
        std::snprintf(head, sizeof(head), "[input-marker] wall=%llums tick=%llu two_finger_tap x=%.3f y=%.3f ctl=%u",
                      static_cast<unsigned long long>(e.wallMs), static_cast<unsigned long long>(e.tick), e.x, e.y,
                      e.a);
        return head;
    case Layer::Touch:
    {
        const uint64_t id = (static_cast<uint64_t>(e.b) << 32) | e.a;
        std::snprintf(head, sizeof(head), "[input-touch] wall=%llums tick=%llu finger %s id=%llu x=%.4f y=%.4f",
                      static_cast<unsigned long long>(e.wallMs), static_cast<unsigned long long>(e.tick),
                      e.code == static_cast<uint8_t>(TouchCode::Down) ? "down" : "up",
                      static_cast<unsigned long long>(id), e.x, e.y);
        return head;
    }
    case Layer::Vpad:
    {
        const uint32_t rose = e.b & ~e.a;
        const uint32_t fell = e.a & ~e.b;
        std::snprintf(head, sizeof(head), "[input-vpad] wall=%llums tick=%llu vpad 0x%04x->0x%04x %s",
                      static_cast<unsigned long long>(e.wallMs), static_cast<unsigned long long>(e.tick), e.a, e.b,
                      formatHighChanges(rose, fell).c_str());
        return head;
    }
    case Layer::Stat:
        // text carries the preformatted slots + which payload (built at push
        // time, 1/s on the render thread): slots="0:1 1:0" which="1 5".
        // a = ring drops so far. x = vsyncs/s, b = presents.
        std::snprintf(head, sizeof(head),
                      "[input-stat] wall=%llums tick=%llu vsyncs_per_s=%.2f presents=%llu %s drops=%u",
                      static_cast<unsigned long long>(e.wallMs), static_cast<unsigned long long>(e.tick),
                      e.x, static_cast<unsigned long long>(e.b), e.text, e.a);
        return head;
    }
    return "[input-?]";
}

// Fixed-size single-producer ring (one writer thread + the flush worker;
// same release/acquire shape as perflog::StageRing). push() is wait-free:
// a full ring counts the event as dropped (reported on the stat line, no
// producer ever blocks; the EE thread in particular never waits).
template <size_t Cap> class EventRing
{
public:
    static_assert((Cap & (Cap - 1)) == 0, "power of two");

    bool push(const Event &e)
    {
        const uint64_t i = m_head.load(std::memory_order_relaxed);
        if (i - m_consumedCache >= Cap)
        {
            m_consumedCache = m_consumed.load(std::memory_order_acquire);
            if (i - m_consumedCache >= Cap)
            {
                m_drops.fetch_add(1u, std::memory_order_relaxed);
                return false;
            }
        }
        m_slots[i & (Cap - 1)] = e;
        m_head.store(i + 1u, std::memory_order_release);
        return true;
    }

    // Consumer (flush worker): drain new events in order into out, up to
    // maxOut. Returns the events taken.
    size_t drain(Event *out, size_t maxOut)
    {
        const uint64_t head = m_head.load(std::memory_order_acquire);
        uint64_t c = m_consumed.load(std::memory_order_relaxed);
        size_t n = 0;
        while (c < head && n < maxOut)
        {
            out[n++] = m_slots[c & (Cap - 1)];
            ++c;
        }
        m_consumed.store(c, std::memory_order_release);
        return n;
    }

    uint64_t drops() const { return m_drops.load(std::memory_order_relaxed); }
    void resetForTest()
    {
        m_head.store(0u, std::memory_order_relaxed);
        m_consumed.store(0u, std::memory_order_relaxed);
        m_consumedCache = 0u;
        m_drops.store(0u, std::memory_order_relaxed);
    }

private:
    std::array<Event, Cap> m_slots{};
    std::atomic<uint64_t> m_head{0};
    std::atomic<uint64_t> m_consumed{0};
    uint64_t m_consumedCache = 0; // producer-side only
    std::atomic<uint64_t> m_drops{0};
};

// Runtime API (src/lib/ps2_input_diag.cpp). enabled() caches the knob;
// every other entry early-outs on it (zero cost off).
bool enabled();
void noteSdlButton(uint32_t which, uint32_t button, bool down, uint64_t tick);
void noteSdlAxis(uint32_t which, uint32_t axis, int value, uint64_t tick);
void noteSdlDevice(uint32_t whichOrIndex, bool added, const char *name, uint64_t tick);
// Render thread: host pad word published to the latch (transition only).
void notePublish(uint16_t mask, uint64_t tick);
// EE thread: port-0 active-low pad word the guest consumed (transition).
void noteGuest(uint16_t word, uint64_t tick);
// EE thread: rider action-block watch-bit edges (transition, per P slot).
void noteRider(uint32_t p, uint32_t oldBits, uint32_t newBits, uint64_t tick);
// Render thread: raw SDL finger down/up (transition-free: every finger).
void noteTouch(uint64_t fingerId, bool down, float x, float y, uint64_t tick);
// Render thread: updatePad pressed bits per render pass (transition only).
void noteVpad(uint16_t mask, uint64_t tick);
// Render thread: one present shown.
void notePresent();
// Render thread, every loop iteration: installs the SDL watch once, emits
// the 1 s stat line, and timer-flushes every 5 s (all no-ops unless on,
// except the once-only watch install for Part A touches).
void pollHost(uint64_t tick);
// "timer" (async worker, at most one in flight) / "pause" + "shutdown"
// (synchronous; join the worker first). No-op unless active.
void flush(const char *reason);
// Test seams.
void resetForTest();
uint64_t dropCountForTest();
#if defined(PS2X_IOS)
// Seams for ps2_ios_runtime.mm (Part A merge) and the host test.
namespace touchseam
{
touchev::Table &table();
bool eventsOn();
} // namespace touchseam
#endif
} // namespace ps2x::inputdiag
