#include "MiniTest.h"
#include "ps2_net_link.h"

#include <cstdint>
#include <string>

namespace
{
    void register_ps2_net_link_tests_body(TestCase &tc)
    {
        tc.Run("netsec-fix-1 F9: COMMIT window admits the lead, rejects the rest", [](TestCase &t)
        {
            using ps2x::nplink::commitAcceptable;
            using ps2x::nplink::kCommitWindow;
            // firstTick=1000, sendUpTo=1100 (in-flight lead 100), nothing applied yet.
            t.IsTrue(commitAcceptable(1000u, 1100u, false, 0u, 1000u), "first tick admits");
            t.IsTrue(commitAcceptable(1000u, 1100u, false, 0u, 1100u), "sendUpTo admits");
            t.IsTrue(commitAcceptable(1000u, 1100u, false, 0u, 1100u + kCommitWindow),
                     "window edge admits");
            t.IsFalse(commitAcceptable(1000u, 1100u, false, 0u, 999u), "before first rejects");
            t.IsFalse(commitAcceptable(1000u, 1100u, false, 0u, 1100u + kCommitWindow + 1u),
                      "past window rejects");
            t.IsFalse(commitAcceptable(1000u, 1100u, false, 0u, 1000000u), "far future rejects");
            // After applying tick 1050, replays and older ticks reject.
            t.IsTrue(commitAcceptable(1000u, 1100u, true, 1050u, 1051u), "next admits");
            t.IsFalse(commitAcceptable(1000u, 1100u, true, 1050u, 1050u), "replay rejects");
            t.IsFalse(commitAcceptable(1000u, 1100u, true, 1050u, 1001u), "old rejects");
        });

        tc.Run("netsec-fix-1 F10: corrupt frame sizes throw SessionError", [](TestCase &t)
        {
            using ps2x::nplink::checkLinkFrameSize;
            checkLinkFrameSize(44u);
            t.IsTrue(true, "min passes");
            checkLinkFrameSize(16384u);
            t.IsTrue(true, "max passes");
            bool zero = false;
            try
            {
                checkLinkFrameSize(0u);
            }
            catch (const ps2x::nplink::SessionError &)
            {
                zero = true;
            }
            t.IsTrue(zero, "zero throws");
            bool small = false;
            try
            {
                checkLinkFrameSize(43u);
            }
            catch (const ps2x::nplink::SessionError &)
            {
                small = true;
            }
            t.IsTrue(small, "short throws");
            bool big = false;
            try
            {
                checkLinkFrameSize(16385u);
            }
            catch (const ps2x::nplink::SessionError &)
            {
                big = true;
            }
            t.IsTrue(big, "oversize throws");
            // SessionError stays a std::exception with its message intact.
            try
            {
                throw ps2x::nplink::SessionError("relay closed/bad COMMIT");
            }
            catch (const std::exception &e)
            {
                t.IsTrue(std::string(e.what()) == "relay closed/bad COMMIT", "message survives");
            }
        });
    }
} // namespace

void register_ps2_net_link_tests()
{
    MiniTest::Case("Ps2NetLink", register_ps2_net_link_tests_body);
}
