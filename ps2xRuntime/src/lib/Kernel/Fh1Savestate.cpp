// FH27: save-state section "fh1" for the full-120 host state
// (ps2_fh1_full120.h): event request/commit/flip flags, stock-time
// accounting, update parity and the RNG hold, the ordinal call counters,
// the render-clock offsets, the pending post-call record and the bonusflip
// state list. Without it a state saved inside an events window reloaded as
// a stock machine with converted guest words (FCR1 Determinism table).
//
// Optional (files from before FH27 still load and keep the fresh-process
// state, as they always did). A state taken while the guest words are
// converted (an events flip committed, requested or applied, or always-mode
// patched) loads only into the same full-120 mode and FIX masks; an
// inactive one into another mode leaves the fresh state alone.

#include "ps2_fh1_full120.h"
#include "runtime/ps2_savestate.h"

namespace
{
    using ps2_savestate::Reader;
    using ps2_savestate::Writer;

    constexpr uint32_t kFh1Version = 1u;

    bool converted(bool sched, bool commit, bool guest, bool flip, bool patched)
    {
        return sched || commit || guest || flip || patched;
    }

    void fh1Save(Writer &w)
    {
        using namespace ps2_fh1;
        w.u8(static_cast<uint8_t>(mode()));
        w.u64(fixMask());
        w.u32(fixMask12());
        for (bool v : {g_schedActive, g_commitActive, g_guestActive, g_flipPending, g_stockInit, g_patched, g_rngOdd,
                       g_lcgHeld, g_passRan, g_anyAir, g_finished, g_clockRan, g_startEdge, g_rcActive, g_postArmed})
            w.b(v);
        for (uint32_t v : {g_divNext, g_divThis, g_divEnded, g_producerCalls, g_lastStamp10s, g_raceTickCalls,
                           g_raceTick2Calls, g_sessionCalls, g_rngUpdates, g_lcgSaved, g_drawK})
            w.u32(v);
        for (uint64_t v : {g_stockHalf.load(std::memory_order_relaxed), g_stockHalfExact.load(std::memory_order_relaxed),
                           g_drawn, g_drawSkipped, g_lastTick, g_rcEntryHalf})
            w.u64(v);
        w.pod(g_rcOffset);
        w.pod(g_rcEntryS);
        w.pod(g_post);
        for (const BonusSeen &e : g_bonusSeen)
        {
            w.u32(e.state);
            w.u64(e.tick);
        }
    }

    bool fh1Load(Reader &r)
    {
        using namespace ps2_fh1;
        const uint8_t savedMode = r.u8();
        const uint64_t savedFix = r.u64();
        const uint32_t savedFix12 = r.u32();
        bool b[15] = {};
        for (bool &v : b)
            v = r.b();
        uint32_t u[11] = {};
        for (uint32_t &v : u)
            v = r.u32();
        uint64_t q[6] = {};
        for (uint64_t &v : q)
            v = r.u64();
        int64_t rcOffset = 0, rcEntryS = 0;
        r.pod(rcOffset);
        r.pod(rcEntryS);
        PostCall post;
        r.pod(post);
        std::array<BonusSeen, 8> seen{};
        for (BonusSeen &e : seen)
        {
            e.state = r.u32();
            e.tick = r.u64();
        }
        if (!r.ok())
            return false;
        // FLK2: flare only skips odd-update probe-loop calls (no converted words, no state of its own beyond
        // the shared update parity), so a state loads with it toggled either way.
        const uint32_t kStateless12 = kFix12Flare;
        const bool same = savedMode == static_cast<uint8_t>(mode()) && savedFix == fixMask() &&
                          (savedFix12 & ~kStateless12) == (fixMask12() & ~kStateless12);
        if (!same)
        {
            if (converted(b[0], b[1], b[2], b[3], b[5]))
                return r.fail("fh1: state saved with full-120 guest words converted (mode " +
                              std::to_string(savedMode) + ") needs the same PS2X_SSX3_FULL120 mode and FIX masks");
            std::fprintf(stderr, "[savestate] fh1: saved in mode %u, running mode %u; full-120 state left fresh\n",
                         savedMode, static_cast<unsigned>(mode()));
            return true;
        }
        g_schedActive = b[0];
        g_commitActive = b[1];
        g_guestActive = b[2];
        g_flipPending = b[3];
        g_stockInit = b[4];
        g_patched = b[5];
        g_rngOdd = b[6];
        g_lcgHeld = b[7];
        g_passRan = b[8];
        g_anyAir = b[9];
        g_finished = b[10];
        g_clockRan = b[11];
        g_startEdge = b[12];
        g_rcActive = b[13];
        g_postArmed = b[14];
        g_divNext = u[0];
        g_divThis = u[1];
        g_divEnded = u[2];
        g_producerCalls = u[3];
        g_lastStamp10s = u[4];
        g_raceTickCalls = u[5];
        g_raceTick2Calls = u[6];
        g_sessionCalls = u[7];
        g_rngUpdates = u[8];
        g_lcgSaved = u[9];
        g_drawK = u[10];
        g_stockHalf.store(q[0], std::memory_order_relaxed);
        g_stockHalfExact.store(q[1], std::memory_order_relaxed);
        g_drawn = q[2];
        g_drawSkipped = q[3];
        g_lastTick = q[4];
        g_rcEntryHalf = q[5];
        g_rcOffset = rcOffset;
        g_rcEntryS = rcEntryS;
        g_post = post;
        g_bonusSeen = seen;
        return true;
    }

    void fh32Save(Writer &w) { w.pod(ps2_fh1::g_jcam2Shadows); }
    bool fh32Load(Reader &r)
    {
        if (ps2_savestate::loadingSectionVersion()==1u)
        {
            // A's inactive stock seeds contain zero phase/pending state.
            // Active A states cannot silently acquire C's different contract.
            std::array<uint8_t,236> legacy{}; r.pod(legacy);
            if (!r.ok()) return false;
            for (uint8_t b:legacy) if(b) return r.fail("fh32: active design A state cannot load design C");
            ps2_fh1::jcam2Reset(); return true;
        }
        decltype(ps2_fh1::g_jcam2Shadows) shadows{};
        r.pod(shadows); if (!r.ok()) return false;
        ps2_fh1::g_jcam2Shadows=shadows; return true;
    }
    void life2Save(Writer &w)
    {
        using namespace ps2_fh1;
        w.b(life2Fix());
        for (const auto &world : g_life2Worlds)
        { w.u32(world.object); for (auto x:world.stamps) w.u32(x); for (auto x:world.half) w.u8(x); }
        w.u32(static_cast<uint32_t>(g_life2Anchors.size()));
        for (const auto &a:g_life2Anchors) {w.u32(a.input);w.u32(a.timeline);w.u32(a.ordinal);}
    }
    bool life2Load(Reader &r)
    {
        using namespace ps2_fh1;
        const bool enabledAtSave=r.b();
        decltype(g_life2Worlds) worlds{};
        for (auto &world:worlds)
        { world.object=r.u32(); for (auto &x:world.stamps) x=r.u32(); for (auto &x:world.half) {x=r.u8();if(x>1u)return r.fail("life2: invalid half age");} }
        const uint32_t n=r.u32();
        if(n>16384u)return r.fail("life2: invalid anchor count");
        std::vector<Life2Anchor> anchors;
        for(uint32_t i=0;i<n;++i) anchors.push_back({r.u32(),r.u32(),r.u32()});
        if(!r.ok())return false;
        if(enabledAtSave!=life2Fix())
        {
            if(enabledAtSave)return r.fail("life2: recording/streaming metadata needs life2 enabled");
            g_life2Worlds={};g_life2Anchors.clear();return true;
        }
        g_life2Worlds=worlds;g_life2Anchors=std::move(anchors);return true;
    }
    const bool kLife2Registered=ps2_savestate::registerSection(
        "life2", {1u,&life2Save,&life2Load,nullptr,0u,/*optional=*/true});
    const bool kFh32Registered = ps2_savestate::registerSection(
        "fh32", {2u,&fh32Save,&fh32Load,nullptr,1u,/*optional=*/true});
    const bool kFh1Registered = ps2_savestate::registerSection(
        "fh1", {kFh1Version, &fh1Save, &fh1Load, nullptr, 0u, /*optional=*/true});
} // namespace

// Referenced from the scheduler TU so a static-library link keeps this one.
void ps2_fh1_linkSavestateSection()
{
    (void)kFh1Registered;
    (void)kFh32Registered;
    (void)kLife2Registered;
}
