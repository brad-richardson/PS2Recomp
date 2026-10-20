#include "MiniTest.h"
#include "ps2_net_dirtysock.h"
#include "ps2_net_protocol.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
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

        tc.Run("navigator cancel ignores buttons held at arm time", [](TestCase &t)
        {
            using ps2_net_protocol::qpNavNewPress;
            constexpr uint16_t kNone = 0xFFFFu, kCross = 0x4000u, kCircle = 0x2000u;
            const auto down = [](uint16_t mask) { return static_cast<uint16_t>(0xFFFFu & ~mask); };
            uint16_t ignore = kCross; // Cross still down from choosing Online
            t.IsFalse(qpNavNewPress(down(kCross), ignore), "held Cross from the Online pick: no cancel");
            t.IsFalse(qpNavNewPress(down(kCross), ignore), "still held: no cancel");
            t.IsTrue(qpNavNewPress(down(kCross | kCircle), ignore), "a new button while Cross is held cancels");
            ignore = kCross;
            t.IsFalse(qpNavNewPress(kNone, ignore), "release: no cancel");
            t.Equals(static_cast<unsigned>(ignore), 0u, "released button leaves the ignore set");
            t.IsTrue(qpNavNewPress(down(kCross), ignore), "pressing Cross again cancels");
            ignore = 0u;
            t.IsFalse(qpNavNewPress(kNone, ignore), "idle pad: no cancel");
            t.IsTrue(qpNavNewPress(down(kCircle), ignore), "any fresh press cancels");
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

        tc.Run("online-card-1: outrun catch-up rejoins the guest at persona or filter", [](TestCase &t)
        {
            using ps2_net_protocol::qpNavFilterShowing;
            using ps2_net_protocol::qpNavPersonaShowing;
            t.IsTrue(qpNavPersonaShowing(4u, 0x02u), "persona screen: catch up");
            t.IsFalse(qpNavPersonaShowing(3u, 0x16u), "login screen: no persona jump");
            t.IsFalse(qpNavPersonaShowing(4u, 0x04u), "online main: no persona jump");
            t.IsFalse(qpNavPersonaShowing(0u, 0u), "unreadable words: no jump");
            t.IsTrue(qpNavFilterShowing(7u, 0x16u), "filter screen: catch up");
            t.IsFalse(qpNavFilterShowing(4u, 0x04u), "online main: no filter jump");
            t.IsFalse(qpNavFilterShowing(0u, 0u), "unreadable words: no jump");
        });

        tc.Run("Quick Play waits for a stable entered screen, not repeated pad polls", [](TestCase &t)
        {
            ps2_net_protocol::QpNavMenuSettle state;
            t.IsFalse(state.ready(4u, 2u, 50u), "unobserved state is not ready");
            state.observe(4u, 2u, 100u);
            for (int i = 0; i < 100; ++i) state.observe(4u, 2u, 100u);
            t.IsFalse(state.ready(4u, 2u, 50u), "same-tick polls cannot bypass settle");
            state.observe(4u, 2u, 149u);
            t.IsFalse(state.ready(4u, 2u, 50u), "transition still settling");
            state.observe(4u, 2u, 150u);
            t.IsTrue(state.ready(4u, 2u, 50u), "stable persona state can accept input");
            state.observe(3u, 2u, 151u);
            state.observe(4u, 2u, 152u);
            t.IsFalse(state.ready(4u, 2u, 50u), "intervening modal resets settle");
            state.observe(4u, 4u, 220u);
            t.IsFalse(state.ready(4u, 4u, 50u), "online main gets its own settle");
            state.observe(4u, 4u, 270u);
            t.IsTrue(state.ready(4u, 4u, 50u), "online main settles independently");
            state.observe(4u, 4u, 10u);
            t.IsFalse(state.ready(4u, 4u, 50u), "guest clock rewind resets settle");
        });

        tc.Run("online-card-1: title detector gates the dance recovery", [](TestCase &t)
        {
            using ps2_net_protocol::qpNavTitleShowing;
            t.IsTrue(qpNavTitleShowing(1u, 7u), "title words: recover");
            t.IsFalse(qpNavTitleShowing(2u, 9u), "menus: no recovery");
            t.IsFalse(qpNavTitleShowing(3u, 0x16u), "login: no recovery");
            t.IsFalse(qpNavTitleShowing(0u, 0u), "unreadable words: no recovery");
        });

        tc.Run("online-stock-2: ST keep-alives are state only when STOCK120 is on", [](TestCase &t)
        {
            const uint8_t st[] = {'O', '5', 'K', 'A', 'S', 'T', 1, 2, 3};
            const uint8_t ka[] = {'O', '5', 'K', 'A'};
            const uint8_t pk[] = {'O', '5', 'P', 'K', 'S', 'T', 1};
            t.IsTrue(ps2_net_protocol::isStateDatagram(st, sizeof(st), true), "enabled: state");
            t.IsFalse(ps2_net_protocol::isStateDatagram(st, sizeof(st), false), "knob off: plain keep-alive");
            t.IsFalse(ps2_net_protocol::isStateDatagram(ka, sizeof(ka), true), "bare keep-alive");
            t.IsFalse(ps2_net_protocol::isStateDatagram(pk, sizeof(pk), true), "other tag");
            t.IsFalse(ps2_net_protocol::isStateDatagram(st, 6u, true), "no payload");
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

        tc.Run("netsec-fix-1 F1: invalid TCP frame sizes reject, valid hold or deliver",
               [](TestCase &t)
               {
                   using ps2_net_protocol::classifyTcpFrame;
                   using ps2_net_protocol::TcpFrameAction;
                   t.IsTrue(classifyTcpFrame(0u, 100u) == TcpFrameAction::Reject, "zero rejects");
                   t.IsTrue(classifyTcpFrame(11u, 100u) == TcpFrameAction::Reject, "short rejects");
                   t.IsTrue(classifyTcpFrame(65537u, 70000u) == TcpFrameAction::Reject,
                            "oversize rejects");
                   t.IsTrue(classifyTcpFrame(0xFFFFFFFFu, 100u) == TcpFrameAction::Reject,
                            "hostile u32 rejects");
                   t.IsTrue(classifyTcpFrame(12u, 11u) == TcpFrameAction::Wait, "partial waits");
                   t.IsTrue(classifyTcpFrame(100u, 50u) == TcpFrameAction::Wait, "half waits");
                   t.IsTrue(classifyTcpFrame(12u, 12u) == TcpFrameAction::Deliver, "min delivers");
                   t.IsTrue(classifyTcpFrame(100u, 100u) == TcpFrameAction::Deliver, "exact delivers");
                   t.IsTrue(classifyTcpFrame(65536u, 70000u) == TcpFrameAction::Deliver,
                            "max delivers with trailing");
               });

        tc.Run("netsec-fix-1 F2: O5PK bodies need length == datagram, deliveries must tile",
               [](TestCase &t)
               {
                   uint8_t good[44];
                   std::memset(good, 0, sizeof(good));
                   good[0] = 44u;
                   good[1] = 0u;
                   t.IsTrue(ps2_net_protocol::udpGameBodyValid(good, sizeof(good)), "44-byte record");
                   uint8_t bad[44];
                   std::memcpy(bad, good, sizeof(bad));
                   bad[0] = 43u;
                   t.IsFalse(ps2_net_protocol::udpGameBodyValid(bad, sizeof(bad)), "short declare");
                   bad[0] = 45u;
                   t.IsFalse(ps2_net_protocol::udpGameBodyValid(bad, sizeof(bad)), "long declare");
                   bad[0] = 0xFFu;
                   bad[1] = 0xFFu;
                   t.IsFalse(ps2_net_protocol::udpGameBodyValid(bad, sizeof(bad)), "huge declare");
                   t.IsFalse(ps2_net_protocol::udpGameBodyValid(good, 1u), "under 2 bytes");
                   t.IsFalse(ps2_net_protocol::udpGameBodyValid(nullptr, 44u), "null body");
                   t.IsTrue(ps2_net_protocol::udpConcatBodyValid(good, sizeof(good)), "single tiles");
                   uint8_t two[88];
                   std::memcpy(two, good, 44u);
                   std::memcpy(two + 44, good, 44u);
                   t.IsTrue(ps2_net_protocol::udpConcatBodyValid(two, sizeof(two)), "pair tiles");
                   two[44] = 43u;
                   t.IsFalse(ps2_net_protocol::udpConcatBodyValid(two, sizeof(two)),
                             "second short breaks tiling");
                   uint8_t trunc[80];
                   std::memcpy(trunc, two, sizeof(trunc));
                   t.IsFalse(ps2_net_protocol::udpConcatBodyValid(trunc, sizeof(trunc)),
                             "truncated tail fails");
               });

        tc.Run("netsec-fix-1 F6: relay pin needs exact endpoint plus current token",
               [](TestCase &t)
               {
                   const std::string tok(32u, 'a');
                   const std::string other(32u, 'b');
                   t.IsTrue(ps2_net_protocol::relaySourceAllowed(0x0100007Fu, 3658u, tok, tok,
                                                                 0x0100007Fu, 3658u),
                            "pinned endpoint passes");
                   t.IsFalse(ps2_net_protocol::relaySourceAllowed(0x0100007Fu, 3658u, tok, tok,
                                                                  0x0100007Fu, 3659u),
                             "wrong port drops");
                   t.IsFalse(ps2_net_protocol::relaySourceAllowed(0x0100007Fu, 3658u, tok, tok,
                                                                  0x0200007Fu, 3658u),
                             "wrong IP drops");
                   t.IsFalse(ps2_net_protocol::relaySourceAllowed(0x0100007Fu, 3658u, tok, other,
                                                                  0x0100007Fu, 3658u),
                             "rotated token drops");
                   t.IsFalse(ps2_net_protocol::relaySourceAllowed(0x0100007Fu, 3658u, tok, "",
                                                                  0x0100007Fu, 3658u),
                             "cleared token drops");
                   t.IsFalse(ps2_net_protocol::relaySourceAllowed(0x0100007Fu, 3658u, "", "",
                                                                  0x0100007Fu, 3658u),
                             "unpinned drops");
               });
        tc.Run("config-fix-1 item 1: no session active on a fresh host", [](TestCase &t)
        {
            // No lobby socket and no P2P match link exist in the host
            // suite, so configured-but-idle must read inactive here —
            // even with the public preset values (NET=1 + server) set.
            t.IsFalse(ps2_net::sessionActive(), "idle host has no active online session");
            ::setenv("PS2X_NET", "1", 1);
            ::setenv("PS2X_NET_SERVER", "ssx.bradr.dev", 1);
            t.IsFalse(ps2_net::sessionActive(), "NET=1 configured with no session stays inactive");
            ::unsetenv("PS2X_NET");
            ::unsetenv("PS2X_NET_SERVER");
        });
        tc.Run("resume-ios-1 Part 2: session active only while paired or racing",
               [](TestCase &t)
               {
                   using ps2_net_protocol::onlineSessionActive;
                   t.IsFalse(onlineSessionActive(false, 0u, 0u),
                             "fresh host (menus-only visit, idle sockets) inactive");
                   t.IsTrue(onlineSessionActive(true, 0u, 0u),
                            "race link up reads active");
                   t.IsTrue(onlineSessionActive(true, 3u, 3u),
                            "race link up wins over settled pairing");
                   t.IsTrue(onlineSessionActive(false, 1u, 0u),
                            "fresh +ses pairing (matchup staging) reads active");
                   t.IsFalse(onlineSessionActive(false, 1u, 1u),
                             "post-race link teardown reads inactive");
                   t.IsTrue(onlineSessionActive(false, 2u, 1u),
                            "re-pair after teardown reads active");
               });
    }
}

void register_ps2_net_protocol_tests()
{
    MiniTest::Case("Ps2NetProtocol", register_ps2_net_protocol_tests_body);
}
