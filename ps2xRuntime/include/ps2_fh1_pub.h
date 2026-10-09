// JDR2 shadowpub: render-facing publication of the query-held ground frame on
// the odd update of a pair (included by ps2_fh1_full120.h after the parity
// and query definitions; namespace ps2_fh1).
//
// With `query` the cruise ground query runs on even updates only, so the
// ground frame it writes (R+0x370 normal, R+0x3a0 forward, R+0xaac/R+0xab0
// ground-plane values) moves once per pair while the rider and the camera
// move every update. Two visual consumers drew it held, which reads as an
// A-B-A-B flip-flop at the board (JDR1 2768):
//   0x128f80 -> 0x2e8938  per-rider effect pass, the light bar across the
//                         board (reads R+0x370/R+0x3a0);
//   0x121928 -> 0x2ed490  per-rider projection, the dark board-under shape
//                         (reads R+0xaac/R+0xab0).
// On a query-skipped update those two calls see held value + half the last
// even step (normal/forward renormalised), written just before the call and
// restored when it returns or is unwound, for every rider whose query was
// skipped this update. Physics keeps the held packet (JDR2 Part 3: pos/vel/
// quat/normal/forward bit-identical knob on vs off). Publication needs two
// consecutive even samples (2 updates apart) and the live words unchanged,
// else the call runs on the held value (a miss is never stale).

struct PubRecord
{
    uint32_t target = 0u, sp = 0u, n = 0u;
    uint32_t addr[64] = {};
    uint32_t saved[64] = {};
};
inline PubRecord g_pub;
inline bool g_pubArmed = false;

inline void pubRestore(uint8_t *ram) noexcept
{
    if (!g_pubArmed)
        return;
    for (uint32_t i = 0u; i < g_pub.n; ++i)
        wr32(ram, g_pub.addr[i], g_pub.saved[i]);
    g_pub = PubRecord{};
    g_pubArmed = false;
}

inline void pubOnReturn(uint8_t *ram, R5900Context *ctx, uint32_t targetPc) noexcept
{
    if (!g_pubArmed || !ctx || targetPc != g_pub.target || getRegU32(ctx, 29) != g_pub.sp)
        return;
    pubRestore(ram); // returned or suspended: the guest never keeps a published word
}

inline bool pubPush(uint8_t *ram, uint32_t a, float v) noexcept
{
    uint32_t old = 0u, bits = 0u;
    if (g_pub.n >= 64u || !rd32(ram, a, old))
        return false;
    std::memcpy(&bits, &v, 4);
    g_pub.addr[g_pub.n] = a;
    g_pub.saved[g_pub.n] = old;
    ++g_pub.n;
    return wr32(ram, a, bits);
}

template <uint32_t N>
struct PubSlot
{
    uint32_t key = 0u;
    uint32_t updCur = 0xffffffffu, updPrev = 0xffffffffu, skippedAt = 0xffffffffu;
    uint32_t cur[N] = {}, prev[N] = {};
};

template <uint32_t N, size_t S>
inline PubSlot<N> *pubSlot(std::array<PubSlot<N>, S> &slots, uint32_t key) noexcept
{
    PubSlot<N> *empty = nullptr;
    for (auto &s : slots)
    {
        if (s.key == key)
            return &s;
        if (!empty && s.key == 0u)
            empty = &s;
    }
    if (empty)
        empty->key = key;
    return empty;
}

// Even-update sample (idempotent within one update: a second call refreshes cur).
template <uint32_t N>
inline void pubSample(PubSlot<N> &s, const uint32_t *w, uint32_t upd) noexcept
{
    if (s.updCur != upd)
    {
        std::memcpy(s.prev, s.cur, sizeof(s.cur));
        s.updPrev = s.updCur;
        s.updCur = upd;
    }
    std::memcpy(s.cur, w, sizeof(s.cur));
}

// Pure (unit-tested): odd update `upd` may publish when the last two even
// samples are 2 updates apart, the newest one is from the previous update,
// and the live words still equal it (held).
template <uint32_t N>
inline bool pubReady(const PubSlot<N> &s, const uint32_t *live, uint32_t upd) noexcept
{
    return s.updCur + 1u == upd && s.updPrev + 3u == upd && std::memcmp(live, s.cur, sizeof(s.cur)) == 0;
}

inline float pubHalfStep(uint32_t cur, uint32_t prev) noexcept
{
    float c, p;
    std::memcpy(&c, &cur, 4);
    std::memcpy(&p, &prev, 4);
    return c + 0.5f * (c - p);
}

// Ground frame words, offsets from R: normal xyz, forward xyz, ground plane.
inline constexpr uint32_t kShadowPubOffs[8] = {0x370u, 0x374u, 0x378u, 0x3a0u, 0x3a4u, 0x3a8u, 0xaacu, 0xab0u};
inline constexpr uint32_t kShadowPubWords = 8u;
inline constexpr uint32_t kShadowPubBarSite = 0x128f80u, kShadowPubBar = 0x2e8938u;
inline constexpr uint32_t kShadowPubShapeSite = 0x121928u, kShadowPubShape = 0x2ed490u;
inline std::array<PubSlot<kShadowPubWords>, 8> g_shadowPub{};

inline bool shadowPubFix() noexcept
{
    static const bool on = enabled() && (fixMask() & kFixShadowPub) != 0u && (fixMask() & kFixQuery) != 0u;
    return on;
}

inline bool shadowPubRead(uint8_t *ram, uint32_t r, uint32_t *w) noexcept
{
    for (uint32_t i = 0u; i < kShadowPubWords; ++i)
        if (!rd32(ram, r + kShadowPubOffs[i], w[i]))
            return false;
    return true;
}

// Even update: the query just returned for owner P (R = [P+0x18]).
inline void shadowPubSample(uint8_t *ram, uint32_t pa) noexcept
{
    if (!shadowPubFix())
        return;
    uint32_t r = 0u, w[kShadowPubWords];
    if (!rd32(ram, pa + 0x18u, r) || !r || !shadowPubRead(ram, r, w))
        return;
    if (auto *s = pubSlot(g_shadowPub, r))
        pubSample(*s, w, g_rngUpdates);
}

// Odd update: the query was skipped for owner P (held packet restored).
inline void shadowPubMarkSkip(uint8_t *ram, uint32_t pa) noexcept
{
    if (!shadowPubFix())
        return;
    uint32_t r = 0u;
    if (!rd32(ram, pa + 0x18u, r) || !r)
        return;
    if (auto *s = pubSlot(g_shadowPub, r))
        s->skippedAt = g_rngUpdates;
}

inline void shadowPubUnit(uint8_t *ram, uint32_t r, const PubSlot<kShadowPubWords> &s, uint32_t i0, bool &ok) noexcept
{
    float v[3];
    for (uint32_t i = 0u; i < 3u; ++i)
        v[i] = pubHalfStep(s.cur[i0 + i], s.prev[i0 + i]);
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(n > 0.0f))
    {
        ok = false;
        return;
    }
    for (uint32_t i = 0u; i < 3u && ok; ++i)
        ok = pubPush(ram, r + kShadowPubOffs[i0 + i], v[i] / n);
}

inline void shadowPubPreHook(uint8_t *ram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc) noexcept
{
    const bool bar = sourcePc == kShadowPubBarSite && targetPc == kShadowPubBar;
    const bool shape = sourcePc == kShadowPubShapeSite && targetPc == kShadowPubShape;
    if ((!bar && !shape) || !ctx || g_pubArmed)
        return;
    g_pub = PubRecord{};
    bool ok = true;
    for (auto &s : g_shadowPub)
    {
        if (!s.key || s.skippedAt != g_rngUpdates)
            continue;
        uint32_t live[kShadowPubWords];
        if (!shadowPubRead(ram, s.key, live) || !pubReady(s, live, g_rngUpdates) ||
            std::memcmp(s.cur, s.prev, sizeof(s.cur)) == 0)
            continue;
        if (bar)
        {
            shadowPubUnit(ram, s.key, s, 0u, ok);
            shadowPubUnit(ram, s.key, s, 3u, ok);
        }
        else
            for (uint32_t i = 6u; i < 8u && ok; ++i)
                ok = pubPush(ram, s.key + kShadowPubOffs[i], pubHalfStep(s.cur[i], s.prev[i]));
        if (!ok)
            break;
    }
    if (g_pub.n == 0u)
        return;
    g_pubArmed = true;
    if (!ok)
    {
        pubRestore(ram);
        return;
    }
    g_pub.target = targetPc;
    g_pub.sp = getRegU32(ctx, 29);
}

inline void pubReset() noexcept
{
    g_shadowPub = {};
    g_pub = PubRecord{};
    g_pubArmed = false;
}
