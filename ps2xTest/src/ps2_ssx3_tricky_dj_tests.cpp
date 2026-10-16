#include "MiniTest.h"
#include "ps2_ssx3_tricky_dj.h"

#include <cstdlib>
#include <string>

// tk-audio-1 step 3: gated-event predicate and knob default.
namespace
{
struct EnvGuard
{
    std::string name;
    std::string old;
    bool had = false;
    explicit EnvGuard(const char *n) : name(n)
    {
        if (const char *v = std::getenv(n))
        {
            old = v;
            had = true;
        }
    }
    ~EnvGuard()
    {
        if (had)
            ::setenv(name.c_str(), old.c_str(), 1);
        else
            ::unsetenv(name.c_str());
    }
};
} // namespace

void register_ps2_ssx3_tricky_dj_tests()
{
    MiniTest::Case("Ps2Ssx3TrickyDj", [](TestCase &tc)
                   {
        tc.Run("gated IDs are the evidenced DJ and PA banks", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_dj;
            t.IsTrue(isGatedEvent(0x20cfu), "DJ_Artist_Intro");
            t.IsTrue(isGatedEvent(0x20bbu), "PA_Venue_Intro");
            t.IsTrue(!isGatedEvent(0x2133u), "uber post passes through");
            t.IsTrue(!isGatedEvent(0x20ccu), "unmapped Event_Intro passes through");
            t.IsTrue(!isGatedEvent(0x212fu), "unmapped High_Trick_Score passes through");
            t.IsTrue(!isGatedEvent(0u), "zero passes through");
            t.IsTrue(!isGatedEvent(0xffffffffu), "wild passes through"); });

        tc.Run("knob defaults off, =1 arms", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_dj;
            EnvGuard g("PS2X_SSX3_TRICKY_DJ_GATE");
            ::unsetenv("PS2X_SSX3_TRICKY_DJ_GATE");
            t.IsTrue(!gateEnabled(), "unset is off");
            ::setenv("PS2X_SSX3_TRICKY_DJ_GATE", "0", 1);
            t.IsTrue(!gateEnabled(), "0 is off");
            ::setenv("PS2X_SSX3_TRICKY_DJ_GATE", "", 1);
            t.IsTrue(!gateEnabled(), "empty is off");
            ::setenv("PS2X_SSX3_TRICKY_DJ_GATE", "1", 1);
            t.IsTrue(gateEnabled(), "=1 is on"); }); });
}
