#include "MiniTest.h"
#include "ps2_fh1_full120.h"
#include "runtime/ps2_savestate.h"

#include <cstdint>
#include <string>

void ps2_fh1_linkSavestateSection();

namespace
{
    // Every field the fh1 section carries, set to distinct non-default values.
    void setDistinct()
    {
        using namespace ps2_fh1;
        g_schedActive = g_commitActive = g_guestActive = g_flipPending = false;
        g_stockInit = true;
        g_patched = false;
        g_rngOdd = true;
        g_lcgHeld = true;
        g_passRan = g_anyAir = g_finished = g_clockRan = g_startEdge = g_rcActive = g_postArmed = true;
        g_divNext = 2u;
        g_divThis = 3u;
        g_divEnded = 4u;
        g_producerCalls = 5u;
        g_lastStamp10s = 6u;
        g_raceTickCalls = 7u;
        g_raceTick2Calls = 8u;
        g_sessionCalls = 9u;
        g_rngUpdates = 10u;
        g_lcgSaved = 11u;
        g_drawK = 12u;
        g_stockHalf.store(13u);
        g_stockHalfExact.store(14u);
        g_drawn = 15u;
        g_drawSkipped = 16u;
        g_lastTick = 17u;
        g_rcEntryHalf = 18u;
        g_rcOffset = -19;
        g_rcEntryS = 20;
        g_post.target = 21u;
        g_post.sp = 22u;
        g_post.obj = 23u;
        g_post.kind = 2u;
        for (uint32_t i = 0; i < 6u; ++i)
            g_post.saved[i] = 30u + i;
        for (uint32_t i = 0; i < g_bonusSeen.size(); ++i)
            g_bonusSeen[i] = {0x5c2400u + i * 0x100u, 40u + i};
    }

    void setFresh()
    {
        using namespace ps2_fh1;
        g_schedActive = g_commitActive = g_guestActive = g_flipPending = g_stockInit = g_patched = false;
        g_rngOdd = g_lcgHeld = g_passRan = g_anyAir = g_finished = g_clockRan = g_startEdge = g_rcActive = false;
        g_postArmed = false;
        g_divNext = g_divThis = g_divEnded = 1u;
        g_producerCalls = g_raceTickCalls = g_raceTick2Calls = g_sessionCalls = g_rngUpdates = g_lcgSaved = g_drawK = 0u;
        g_lastStamp10s = 0xffffffffu;
        g_stockHalf.store(0u);
        g_stockHalfExact.store(0u);
        g_drawn = g_drawSkipped = g_lastTick = g_rcEntryHalf = 0u;
        g_rcOffset = g_rcEntryS = 0;
        g_post = PostCall{};
        g_bonusSeen = {};
    }

    std::vector<uint8_t> saveFh1()
    {
        ps2_savestate::Writer w;
        ps2_savestate::registeredSections().at("fh1").save(w);
        return w.buf;
    }

    bool loadFh1(const std::vector<uint8_t> &buf)
    {
        ps2_savestate::Reader r(buf.data(), buf.size());
        return ps2_savestate::registeredSections().at("fh1").load(r) && r.ok() && r.atEnd();
    }
} // namespace

void register_ps2_fh1_savestate_tests()
{
    MiniTest::Case("Ps2Fh1Savestate", [](TestCase &tc)
    {
        tc.Run("fh1 is registered, v1, optional (pre-FH27 files still load)", [](TestCase &t)
        {
            ps2_fh1_linkSavestateSection();
            const auto &sections = ps2_savestate::registeredSections();
            t.IsTrue(sections.count("fh1") == 1u, "section registered");
            const auto &h = sections.at("fh1");
            t.Equals(h.version, 1u, "version 1");
            t.Equals(h.minLoadVersion, 0u, "exact version");
            t.IsTrue(h.optional, "optional");
            t.IsTrue(!sections.at("stub:sif").optional, "other sections stay required");
        });

        tc.Run("round trip restores every field (same mode)", [](TestCase &t)
        {
            setDistinct();
            const std::vector<uint8_t> a = saveFh1();
            setFresh();
            t.IsTrue(loadFh1(a), "loads");
            t.IsTrue(saveFh1() == a, "re-save is byte-identical");
            t.Equals(ps2_fh1::g_stockHalfExact.load(), uint64_t{14u}, "stock-time accounting");
            t.Equals(ps2_fh1::g_rcOffset, int64_t{-19}, "render-clock offset");
            t.Equals(ps2_fh1::g_post.saved[5], 35u, "post-call record");
            t.Equals(ps2_fh1::g_bonusSeen[7].state, 0x5c2b00u, "bonusflip list");
            t.IsTrue(ps2_fh1::g_rngOdd && ps2_fh1::g_lcgHeld, "parity and LCG hold");
            setFresh();
        });

        tc.Run("another mode: converted state refused, inactive state leaves fresh state", [](TestCase &t)
        {
            setDistinct();
            ps2_fh1::g_guestActive = true;
            std::vector<uint8_t> active = saveFh1();
            active[0] = static_cast<uint8_t>((active[0] + 1u) % 3u); // saved under another PS2X_SSX3_FULL120
            setFresh();
            t.IsTrue(!loadFh1(active), "converted guest words refuse");
            for (bool *flag : {&ps2_fh1::g_schedActive, &ps2_fh1::g_commitActive, &ps2_fh1::g_flipPending,
                               &ps2_fh1::g_patched})
            {
                setDistinct();
                *flag = true;
                std::vector<uint8_t> b = saveFh1();
                b[0] = static_cast<uint8_t>((b[0] + 1u) % 3u);
                setFresh();
                t.IsTrue(!loadFh1(b), "each converted flag refuses");
            }
            setDistinct();
            std::vector<uint8_t> idle = saveFh1();
            idle[0] = static_cast<uint8_t>((idle[0] + 1u) % 3u);
            setFresh();
            t.IsTrue(loadFh1(idle), "inactive state loads");
            t.Equals(ps2_fh1::g_stockHalfExact.load(), uint64_t{0u}, "fresh accounting kept");
            t.IsTrue(!ps2_fh1::g_stockInit, "fresh: re-derived at the next VBlank");
            setDistinct();
            std::vector<uint8_t> fix = saveFh1();
            fix[1] ^= 1u; // another FIX mask, same mode
            ps2_fh1::g_guestActive = true;
            std::vector<uint8_t> fixActive = saveFh1();
            fixActive[1] ^= 1u;
            setFresh();
            t.IsTrue(loadFh1(fix), "inactive, other FIX mask: loads");
            t.IsTrue(!loadFh1(fixActive), "converted, other FIX mask: refused");
            setFresh();
        });

        tc.Run("truncated payload refuses", [](TestCase &t)
        {
            setFresh();
            std::vector<uint8_t> a = saveFh1();
            a.resize(a.size() - 1u);
            t.IsTrue(!loadFh1(a), "refused");
            setFresh();
        });
    });
}
