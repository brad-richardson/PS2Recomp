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
            t.IsTrue(isGatedEvent(0x20e5u), "DJ_Radio_Big_Intro");
            t.IsTrue(!isGatedEvent(0x2133u), "uber post passes through");
            t.IsTrue(!isGatedEvent(0x2088u), "rider Wipeout passes through");
            t.IsTrue(!isGatedEvent(0x20a8u), "uber payoff passes through");
            t.IsTrue(!isGatedEvent(0x20bcu), "menu greeting passes through");
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
            t.IsTrue(gateEnabled(), "=1 is on"); });

        tc.Run("venue alias detected by manifest row, not course name", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_dj;
            using ps2_ssx3_course::Mode;
            t.IsTrue(!modeHasVenueAlias(nullptr), "null (Stock) has no alias");
            Mode plain;
            t.IsTrue(!modeHasVenueAlias(&plain), "mode without aliases");
            Mode speechOnly;
            speechOnly.aliases.push_back({"/DATA/AUDIO/SPEECH.BIG", "speech-comp"});
            t.IsTrue(!modeHasVenueAlias(&speechOnly), "SPEECH-only alias is not venue");
            Mode venue;
            venue.name = "GARI";
            venue.aliases.push_back({"/DATA/AUDIO/ENGLISH.BIG", "english-comp"});
            t.IsTrue(modeHasVenueAlias(&venue), "ENGLISH alias row detected"); });

        tc.Run("Part 3c: venue/artist pass with alias; radio always drops",
               [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_dj;
            using ps2_ssx3_course::Mode;
            Mode venue;
            venue.aliases.push_back({"/DATA/AUDIO/ENGLISH.BIG", "english-comp"});
            Mode plain;
            // Curated banks voice with the alias; stock banks drop.
            t.IsTrue(!shouldDrop(0x20bbu, &venue), "venue passes with alias");
            t.IsTrue(shouldDrop(0x20bbu, &plain), "venue drops without alias");
            t.IsTrue(shouldDrop(0x20bbu, nullptr), "venue drops on Stock");
            t.IsTrue(!shouldDrop(0x20cfu, &venue), "artist passes with alias");
            t.IsTrue(shouldDrop(0x20cfu, &plain), "artist drops without alias");
            t.IsTrue(shouldDrop(0x20e5u, &venue), "radio drops with alias");
            t.IsTrue(shouldDrop(0x20e5u, &plain), "radio drops without alias");
            t.IsTrue(!shouldDrop(0x2088u, &plain), "rider post never drops");
            t.IsTrue(!shouldDrop(0x2133u, &venue), "uber post never drops");
            // Wrapper: gate off passes everything.
            t.IsTrue(!shouldDropPost(0x20e5u, &venue, false), "gate off passes");
            t.IsTrue(!shouldDropPost(0x20bbu, &plain, false), "gate off passes");
            t.IsTrue(shouldDropPost(0x20e5u, &venue, true), "gate on drops radio");
            t.IsTrue(!shouldDropPost(0x20cfu, &venue, true), "gate on passes artist"); }); });
}
