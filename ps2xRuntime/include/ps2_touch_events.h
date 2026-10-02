#pragma once

// IN4 Part A: event-fed iOS touches (portable record + merge; no SDL here so
// the host suite can test it).
//
// The render loop used to sample only SDL's live finger list after each pump
// (ps2_ios_runtime.mm touchPoints): a touch down+up inside one pump was never
// seen (IN3 loss path A), and a top/bottom-edge press held by iOS's system
// gesture recognizers arrived as began+ended together and dropped the same
// way (IN3 loss path B). An SDL_AddEventWatch (ps2_input_diag.cpp, which also
// feeds the IN4 input log) records every FINGERDOWN/UP/MOTION into a Table;
// touchPoints merges the table with the live list, so a touch that began and
// ended inside one pump is still returned for at least one render pass,
// classified at its touch-down point. PS2X_IOS_TOUCH_EVENTS=0 keeps the old
// live-list-only path. Det IDENTICAL by construction (det boots have no
// touches: an empty table + an empty live list merges to zero, as before).
//
// The same watch feeds the IN4 two-finger-tap marker: two finger downs
// within kTapPairMs with no third finger between, both up within kTapSpanMs
// of the first down and neither sliding past kTapSlop. takeMarker reports it
// once. virtualPadTouches drops marker-candidate (multi-peer) touches while
// a controller is connected, so the marker never reaches the guest.

#include <cstdint>

namespace ps2x::touchev
{
// Live finger from SDL_GetTouchFinger (normalised 0..1, x right, y down).
struct LiveFinger
{
    int64_t id;
    float x, y;
};

// Merged touch out (normalised 0..1; the caller scales to window points).
struct OutTouch
{
    int64_t id;
    float x, y;
};

class Table
{
public:
    static constexpr int kMax = 8;
    // Two-finger-tap marker windows (Brad's marker press).
    static constexpr uint64_t kTapPairMs = 300; // second down within this of the first
    static constexpr uint64_t kTapSpanMs = 1000; // both up within this of the first down
    static constexpr float kTapSlop = 0.05f; // normalised units of slide allowed

    void down(int64_t id, float x, float y, uint64_t nowMs)
    {
        Slot *s = find(id);
        if (s)
        {
            // Re-down without an up (SDL reuses ids only after UP, but stay
            // total): treat as a fresh press.
            *s = Slot{};
        }
        else
        {
            s = freeSlot();
            if (!s)
                return; // full: drop (bounded; 8 fingers max anyway)
        }
        s->used = true;
        s->id = id;
        s->downX = x;
        s->downY = y;
        s->x = x;
        s->y = y;
        s->downMs = nowMs;
        s->upMs = 0u;
        s->consumed = false;
        s->multipeer = false;
        // A second finger down shortly after the first opens a tap pair.
        int peers = 0;
        Slot *peer = nullptr;
        for (Slot &o : m_slots)
        {
            if (o.used && o.upMs == 0u && o.id != id)
            {
                ++peers;
                peer = &o;
            }
        }
        if (peers == 1 && peer && nowMs >= peer->downMs && nowMs - peer->downMs <= kTapPairMs)
        {
            s->multipeer = true;
            peer->multipeer = true;
            m_pair = true;
            m_pairA = peer->id;
            m_pairB = id;
            m_pairDownMs = peer->downMs;
            m_pairViolated = false;
            m_pairUpCount = 0;
        }
        else if (peers >= 1)
        {
            // Third (or late second) finger: not a tap, and it spoils an
            // open pair.
            s->multipeer = peers >= 1;
            if (m_pair)
                m_pairViolated = true;
        }
        if (m_pair && nowMs - m_pairDownMs > kTapSpanMs)
            m_pair = false; // stale pair (no ups came): close it
        m_lastDownMs = nowMs;
    }

    void up(int64_t id, uint64_t nowMs)
    {
        Slot *s = find(id);
        if (!s)
            return; // never saw the down (watch registered mid-touch): ignore
        s->upMs = nowMs;
        if (m_pair && (id == m_pairA || id == m_pairB))
        {
            if (m_pairViolated || nowMs - m_pairDownMs > kTapSpanMs)
            {
                m_pair = false;
            }
            else
            {
                const float dx = s->x - s->downX;
                const float dy = s->y - s->downY;
                if (dx * dx + dy * dy > kTapSlop * kTapSlop)
                    m_pairViolated = true;
                if (++m_pairUpCount == 2)
                {
                    if (!m_pairViolated)
                    {
                        // Marker: report once, at the midpoint of the two
                        // touch-down points.
                        m_marker = true;
                        m_markerMs = nowMs;
                        const Slot *a = find(m_pairA);
                        const Slot *b = find(m_pairB);
                        if (a && b)
                        {
                            m_markerX = 0.5f * (a->downX + b->downX);
                            m_markerY = 0.5f * (a->downY + b->downY);
                        }
                        else
                        {
                            m_markerX = s->downX;
                            m_markerY = s->downY;
                        }
                    }
                    m_pair = false;
                }
            }
        }
        if (m_pair && nowMs - m_pairDownMs > kTapSpanMs)
            m_pair = false;
    }

    void motion(int64_t id, float x, float y, uint64_t nowMs)
    {
        Slot *s = find(id);
        if (!s)
            return;
        s->x = x;
        s->y = y;
        if (m_pair && (id == m_pairA || id == m_pairB))
        {
            const float dx = x - s->downX;
            const float dy = y - s->downY;
            if (dx * dx + dy * dy > kTapSlop * kTapSlop)
                m_pairViolated = true;
        }
        if (m_pair && nowMs - m_pairDownMs > kTapSpanMs)
            m_pair = false;
    }

    // Merge the live list with down-seen records. A record whose finger is
    // still live is already in the live list (mark consumed, skip it); a
    // record gone from the live list is returned once at its touch-down
    // point, then consumed. While controllerConnected, multi-peer (tap-pair)
    // records are dropped so the two-finger marker never reaches the guest.
    // Returns the touches written (<= max).
    int merge(const LiveFinger *live, int nLive, bool controllerConnected, OutTouch *out, int max)
    {
        int n = 0;
        for (int i = 0; i < nLive && n < max; ++i)
        {
            out[n].id = live[i].id;
            out[n].x = live[i].x;
            out[n].y = live[i].y;
            ++n;
            if (Slot *s = find(live[i].id))
                s->consumed = true;
        }
        for (Slot &s : m_slots)
        {
            if (!s.used || s.consumed || n >= max)
                continue;
            bool isLive = false;
            for (int i = 0; i < nLive; ++i)
            {
                if (live[i].id == s.id)
                {
                    isLive = true;
                    break;
                }
            }
            if (isLive)
            {
                s.consumed = true;
                continue;
            }
            if (controllerConnected && s.multipeer)
            {
                s.consumed = true; // marker member: never reaches the guest
                continue;
            }
            out[n].id = s.id;
            out[n].x = s.downX; // classified at its touch-down point
            out[n].y = s.downY;
            ++n;
            s.consumed = true;
        }
        // Prune records that have been seen and lifted.
        for (Slot &s : m_slots)
        {
            if (s.used && s.consumed && s.upMs != 0u)
                s = Slot{};
        }
        return n;
    }

    bool takeMarker(uint64_t &wallMs, float &x, float &y)
    {
        if (!m_marker)
            return false;
        m_marker = false;
        wallMs = m_markerMs;
        x = m_markerX;
        y = m_markerY;
        return true;
    }

    int usedForTest() const
    {
        int n = 0;
        for (const Slot &s : m_slots)
            n += s.used ? 1 : 0;
        return n;
    }

private:
    struct Slot
    {
        bool used = false;
        int64_t id = 0;
        float downX = 0.0f, downY = 0.0f;
        float x = 0.0f, y = 0.0f;
        uint64_t downMs = 0u, upMs = 0u;
        bool consumed = false;
        bool multipeer = false;
    };

    Slot *find(int64_t id)
    {
        for (Slot &s : m_slots)
        {
            if (s.used && s.id == id)
                return &s;
        }
        return nullptr;
    }

    const Slot *find(int64_t id) const
    {
        for (const Slot &s : m_slots)
        {
            if (s.used && s.id == id)
                return &s;
        }
        return nullptr;
    }

    Slot *freeSlot()
    {
        for (Slot &s : m_slots)
        {
            if (!s.used)
                return &s;
        }
        // Full: evict the oldest consumed-and-lifted record first.
        Slot *best = nullptr;
        for (Slot &s : m_slots)
        {
            if (s.consumed && s.upMs != 0u && (!best || s.downMs < best->downMs))
                best = &s;
        }
        if (best)
        {
            *best = Slot{};
            return best;
        }
        return nullptr;
    }

    Slot m_slots[kMax]{};
    uint64_t m_lastDownMs = 0u;
    // Open two-finger-tap pair.
    bool m_pair = false;
    int64_t m_pairA = 0, m_pairB = 0;
    uint64_t m_pairDownMs = 0u;
    bool m_pairViolated = false;
    int m_pairUpCount = 0;
    // Pending marker.
    bool m_marker = false;
    uint64_t m_markerMs = 0u;
    float m_markerX = 0.0f, m_markerY = 0.0f;
};
} // namespace ps2x::touchev
