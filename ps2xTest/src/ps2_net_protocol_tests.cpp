#include "MiniTest.h"
#include "ps2_net_protocol.h"

#include <cstdint>
#include <string>

namespace
{
    void register_ps2_net_protocol_tests_body(TestCase &tc)
    {
        tc.Run("stored login line yields prefill identity and rejects malformed lines", [](TestCase &t)
        {
            struct Fixture { const char *line; bool accepted; const char *name; const char *pass; };
            const Fixture fixtures[] = {
                {"onl9g:secret\r", true, "onl9g", "secret"},
                {"  onl9h:another  ", true, "onl9h", "another"},
                {"onl9g", false, "", ""},
                {":secret", false, "", ""},
                {"onl9g:", false, "", ""},
            };
            for (const Fixture &f : fixtures)
            {
                std::string name = "unchanged", pass = "unchanged";
                const bool accepted = ps2_net_protocol::parseLoginLine(f.line, name, pass);
                t.Equals(accepted, f.accepted, "login fixture acceptance");
                if (accepted)
                {
                    t.Equals(name, std::string(f.name), "prefill name");
                    t.Equals(pass, std::string(f.pass), "prefill password");
                }
                else
                {
                    t.Equals(name, std::string("unchanged"), "malformed line leaves name alone");
                }
            }
            std::string name, pass;
            t.IsFalse(ps2_net_protocol::parseLoginLine(std::string(65, 'n') + ":p", name, pass),
                      "oversize name refuses");
            t.IsFalse(ps2_net_protocol::parseLoginLine("n:" + std::string(65, 'p'), name, pass),
                      "oversize password refuses");
            uint8_t printable[64] = {'n', 'a', 'm', 'e', 0};
            uint8_t binary[64] = {'n', 0x01u, 0};
            uint8_t unterminated[64];
            for (uint8_t &byte : unterminated) byte = 'x';
            t.IsTrue(ps2_net_protocol::canOverwriteLoginField(printable),
                     "submit-window printable target can be filled");
            t.IsFalse(ps2_net_protocol::canOverwriteLoginField(binary),
                      "binary guest target is preserved");
            t.IsFalse(ps2_net_protocol::canOverwriteLoginField(unterminated),
                      "unterminated guest target is preserved");
        });

        tc.Run("ONL11 arming consumes each Online entry once", [](TestCase &t)
        {
            t.Equals(ps2_net_protocol::navMode(nullptr, "online"), 0, "quickplay off");
            t.Equals(ps2_net_protocol::navMode("1", nullptr), 0, "nav unset");
            t.Equals(ps2_net_protocol::navMode("1", "1"), 1, "launch mode");
            t.Equals(ps2_net_protocol::navMode("1", "online"), 2, "online mode");
            t.Equals(ps2_net_protocol::navMode("0", "online"), 0, "quickplay disabled");
            int mode = 2;
            bool armed = false;
            uint64_t seen = 0;
            t.IsFalse(ps2_net_protocol::shouldArmOnline(mode, armed, 0, seen), "dormant before entry");
            t.IsTrue(ps2_net_protocol::shouldArmOnline(mode, armed, 1, seen), "first entry arms");
            armed = true; seen = 1;
            t.IsFalse(ps2_net_protocol::shouldArmOnline(mode, armed, 1, seen), "active does not rearm");
            armed = false; // user cancelled or route ended
            t.IsFalse(ps2_net_protocol::shouldArmOnline(mode, armed, 1, seen), "same entry stays consumed");
            t.IsTrue(ps2_net_protocol::shouldArmOnline(mode, armed, 2, seen), "second entry rearms");
            t.IsFalse(ps2_net_protocol::shouldArmOnline(1, false, 2, seen), "launch mode has no rearm");
        });

        tc.Run("relay registration is bounded and byte exact", [](TestCase &t)
        {
            t.IsTrue(ps2_net_protocol::relayRegisterEnabled(nullptr), "default on");
            t.IsFalse(ps2_net_protocol::relayRegisterEnabled("0"), "off override");
            t.Equals(ps2_net_protocol::relayRegistration('L', "abc"),
                     std::string("SSX3RLY2Labc"), "listen role and token");
            t.Equals(ps2_net_protocol::relayRegistration('C', "xyz"),
                     std::string("SSX3RLY2Cxyz"), "connect role and token");
            const uint64_t conn = 1000, last = 1000;
            t.IsFalse(ps2_net_protocol::relayRegisterDue(1, false, true, 1249, conn, last),
                      "before 250 ms cadence");
            t.IsTrue(ps2_net_protocol::relayRegisterDue(1, false, true, 1250, conn, last),
                     "cadence edge");
            t.IsFalse(ps2_net_protocol::relayRegisterDue(1, true, true, 1250, conn, last),
                      "connected listen side stops");
            t.IsTrue(ps2_net_protocol::relayRegisterDue(2, false, true, 1250, conn, last),
                     "connect side registers");
            t.IsFalse(ps2_net_protocol::relayRegisterDue(2, true, true, 1250, conn, last),
                      "connected connect side stops");
            t.IsFalse(ps2_net_protocol::relayRegisterDue(0, false, true, 1250, conn, last),
                      "non-relay mode never registers");
            t.IsFalse(ps2_net_protocol::relayRegisterDue(1, false, false, 1250, conn, last),
                      "override disabled");
            t.IsFalse(ps2_net_protocol::relayRegisterDue(1, false, true, 31000, conn, last),
                      "30 second cap");
        });

        tc.Run("connect backoff doubles with a cap, jitter stays in half-open half", [](TestCase &t)
        {
            t.Equals(ps2_net_protocol::connectBackoffBaseMs(0), static_cast<uint64_t>(0), "no failures");
            t.Equals(ps2_net_protocol::connectBackoffBaseMs(1), static_cast<uint64_t>(1000), "first");
            t.Equals(ps2_net_protocol::connectBackoffBaseMs(2), static_cast<uint64_t>(2000), "second");
            t.Equals(ps2_net_protocol::connectBackoffBaseMs(5), static_cast<uint64_t>(16000), "fifth");
            t.Equals(ps2_net_protocol::connectBackoffBaseMs(6), static_cast<uint64_t>(30000), "capped");
            t.Equals(ps2_net_protocol::connectBackoffBaseMs(100), static_cast<uint64_t>(30000),
                     "huge stays capped");
            t.Equals(ps2_net_protocol::connectBackoffDelayMs(1000, 0.0), static_cast<uint64_t>(500),
                     "equal jitter floor");
            t.Equals(ps2_net_protocol::connectBackoffDelayMs(1000, 1.0), static_cast<uint64_t>(1000),
                     "equal jitter ceiling");
            t.Equals(ps2_net_protocol::connectBackoffDelayMs(1000, -1.0), static_cast<uint64_t>(500),
                     "rand clamps low");
            t.Equals(ps2_net_protocol::connectBackoffDelayMs(1000, 2.0), static_cast<uint64_t>(1000),
                     "rand clamps high");
            t.IsTrue(ps2_net_protocol::mayAttemptConnect(5000, 0, 0, 0.0), "first attempt");
            t.IsFalse(ps2_net_protocol::mayAttemptConnect(500, 1000, 1, 0.0), "clock ran back");
            t.IsFalse(ps2_net_protocol::mayAttemptConnect(1200, 1000, 1, 0.0),
                      "200 ms inside the 500 ms floor");
            t.IsTrue(ps2_net_protocol::mayAttemptConnect(1500, 1000, 1, 0.0),
                     "500 ms floor elapsed");
            t.IsTrue(ps2_net_protocol::mayAttemptConnect(1000, 1000, 3, 1.0) == false,
                     "4000 ms base needs its window");
        });

        tc.Run("120-only policy switch is permissive until armed", [](TestCase &t)
        {
            t.IsTrue(ps2_net_protocol::onlineAllowedByMode(nullptr, false), "switch off, unset");
            t.IsTrue(ps2_net_protocol::onlineAllowedByMode("", false), "switch off, empty");
            t.IsTrue(ps2_net_protocol::onlineAllowedByMode("events", false), "switch off, 120");
            t.IsTrue(ps2_net_protocol::onlineAllowedByMode("events", true), "switch on, 120");
            t.IsFalse(ps2_net_protocol::onlineAllowedByMode(nullptr, true), "switch on, unset");
            t.IsFalse(ps2_net_protocol::onlineAllowedByMode("", true), "switch on, stock 60");
            t.IsFalse(ps2_net_protocol::kOnlineRequires120, "ships dormant (Brad 10-15 tentative)");
        });

        tc.Run("DNS give-up fails fast only after the deadline", [](TestCase &t)
        {
            t.IsFalse(ps2_net_protocol::dnsGiveUp(1000, 1000, false), "just started");
            t.IsFalse(ps2_net_protocol::dnsGiveUp(1000, 10999, false), "before the deadline");
            t.IsTrue(ps2_net_protocol::dnsGiveUp(1000, 11000, false), "deadline edge");
            t.IsFalse(ps2_net_protocol::dnsGiveUp(1000, 60000, true), "resolved never gives up");
            t.IsFalse(ps2_net_protocol::dnsGiveUp(5000, 1000, false), "clock ran back");
        });
    }
}

void register_ps2_net_protocol_tests()
{
    MiniTest::Case("Ps2NetProtocol", register_ps2_net_protocol_tests_body);
}
