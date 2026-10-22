#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Pure decisions used by the DirtySock HLE and Quick Play navigator. Keeping
// these free of sockets, clocks, and guest RAM makes recorded inputs testable.
namespace ps2_net_protocol
{
    inline int navMode(const char *quickplay, const char *nav)
    {
        if (!quickplay || std::string(quickplay) != "1" || !nav)
            return 0;
        if (std::string(nav) == "1")
            return 1;
        if (std::string(nav) == "online")
            return 2;
        if (std::string(nav) == "direct")
            return 3; // astra-qp-direct-1: guest calls, no pad navigator
        return 0;
    }

    inline bool shouldArmOnline(int mode, bool armed, uint64_t count, uint64_t seen)
    {
        return mode == 2 && !armed && count != 0u && count != seen;
    }

    // resume-ios-1 Part 2: an online session is ACTIVE only while paired or
    // racing — never merely because lobby/buddy TCP sockets are still open
    // (the game leaves those up after backing out of Online; every later
    // background autosave was refused for Brad's 10-19 sessions). raceLinkUp
    // is any gmlk link up (every race mode creates one); a +ses pairing that
    // arrived after the last link teardown is matchup staging. Generation
    // counters (not the token string) so a post-race teardown reads inactive.
    inline bool onlineSessionActive(bool raceLinkUp, uint64_t sesGen, uint64_t sesGenAtDest)
    {
        if (raceLinkUp)
            return true;
        return sesGen != sesGenAtDest;
    }

    // ONL10 navigator cancel gate (Brad 10-17, live on iOS): only a new
    // press cancels. Buttons already held when the navigator armed (the
    // Cross that chose Online, still down at netw/crea) are ignored until
    // released; pressing one again then cancels. Active-low pad word.
    inline bool qpNavNewPress(uint16_t buttons, uint16_t &ignore)
    {
        const uint16_t pressed = static_cast<uint16_t>(~buttons);
        ignore = static_cast<uint16_t>(ignore & pressed);
        return static_cast<uint16_t>(pressed & static_cast<uint16_t>(~ignore)) != 0u;
    }

    // online-card-1: title detector for the dance title-recovery (menu
    // 1/7, the title-start phase pair). A blind back-out that overshoots
    // the main menu lands here; the dance presses Start to re-enter
    // instead of firing menu presses into the title.
    inline bool qpNavTitleShowing(uint32_t menuMain, uint32_t menuSub)
    {
        return menuMain == 1u && menuSub == 7u;
    }

    // online-card-1: guest-outrun catch-up screens. With the HLE prefill
    // in place the guest sometimes auto-submits the login (no typing, no
    // navigator submit) and runs ahead to persona or the filter on its
    // own; the navigator jumps forward to rejoin it instead of timing
    // out on a screen the guest already left. Persona 4/0x2 and filter
    // 7/0x16 are the D1-calibrated pairs the phases already use.
    // Menu IDs change before newly entered widgets accept input. Track
    // observed state in guest ticks; repeated pad reads do not advance it.
    struct QpNavMenuSettle
    {
        uint32_t main = 0u, sub = 0u;
        uint64_t since = 0u, last = 0u;
        bool seen = false;

        void observe(uint32_t nextMain, uint32_t nextSub, uint64_t tick)
        {
            if (!seen || nextMain != main || nextSub != sub || tick < last)
                since = tick;
            main = nextMain;
            sub = nextSub;
            last = tick;
            seen = true;
        }
        bool ready(uint32_t wantMain, uint32_t wantSub, uint64_t ticks) const
        {
            return seen && main == wantMain && sub == wantSub && last - since >= ticks;
        }
    };

    inline bool qpNavPersonaShowing(uint32_t menuMain, uint32_t menuSub)
    {
        return menuMain == 4u && menuSub == 0x2u;
    }
    inline bool qpNavFilterShowing(uint32_t menuMain, uint32_t menuSub)
    {
        return menuMain == 7u && menuSub == 0x16u;
    }

    // qp-label-4: the "Online (Alpha)" label lives at main-menu build time
    // now (ssx3-quickplay-label override on 0x194D18, via the CMNAMER entry
    // the row's text resolves from). The per-read scan/patch helpers were
    // removed with it: their writes outlived rebuilt menus (qp-label-2/3).

    inline bool parseLoginLine(std::string line, std::string &name, std::string &pass)
    {
        if (line.size() > 256u)
            return false;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                 line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.erase(line.begin());
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0u || colon + 1u >= line.size() ||
            colon > 64u || line.size() - colon - 1u > 64u)
            return false;
        name = line.substr(0u, colon);
        pass = line.substr(colon + 1u);
        return true;
    }

    inline bool canOverwriteLoginField(const uint8_t *field)
    {
        if (!field)
            return false;
        for (size_t i = 0u; i < 64u; ++i)
        {
            if (field[i] == 0u)
                return true;
            if (field[i] < 32u || field[i] >= 127u)
                return false;
        }
        return false;
    }

    inline bool relayRegisterEnabled(const char *value)
    {
        return !value || !*value || value[0] != '0';
    }

    inline bool relayRegisterDue(uint32_t mode, bool connected, bool enabled,
                                 uint64_t nowMs, uint64_t connMs, uint64_t lastRegMs)
    {
        return (mode & 3u) && !connected && enabled && nowMs - connMs < 30000u &&
               nowMs - lastRegMs >= 250u;
    }

    // online-stock-2: the STOCK120 state side channel is an "O5KA" keep-alive
    // carrying "ST" + payload. Only a side that runs STOCK120 (enabled)
    // treats it as state; otherwise it stays an ordinary keep-alive (no
    // inbox, no connection-state change).
    inline bool isStateDatagram(const uint8_t *buf, size_t n, bool enabled)
    {
        return enabled && buf && n > 6u && buf[0] == 'O' && buf[1] == '5' && buf[2] == 'K' && buf[3] == 'A' &&
               buf[4] == 'S' && buf[5] == 'T';
    }

    inline std::string relayRegistration(char role, const std::string &token)
    {
        return "SSX3RLY2" + std::string(1u, role) + token;
    }

    // Availability policy (online-default-1; Brad 10-15 tentative:
    // online is 120-only). THE ONE-LINE SWITCH: flip kOnlineRequires120
    // to true and stock-60 builds go offline (the net HLE stays dormant)
    // with no preset reshuffle.
    // False today: identical behavior in both modes.
    constexpr bool kOnlineRequires120 = false;

    // True when full120Value (PS2X_SSX3_FULL120, non-empty = 120 mode)
    // satisfies the policy. Pure; unit-tested both branches.
    inline bool onlineAllowedByMode(const char *full120Value, bool requires120)
    {
        return !requires120 || (full120Value && *full120Value);
    }

    // Unavailable-service recovery (online-default-1): bounded connect
    // timeout + backoff with equal jitter. Pure decisions (clocks and
    // randomness stay with the caller), unit-tested in
    // ps2_net_protocol_tests.cpp.
    constexpr uint64_t kConnectTimeoutMs = 8000u;
    constexpr uint64_t kConnectBackoffBaseMs = 1000u;
    constexpr uint64_t kConnectBackoffCapMs = 30000u;
    constexpr uint64_t kDnsGiveUpMs = 10000u;

    // Doubling backoff base for consecFails consecutive failures
    // (1-indexed: first failure -> 1 s), capped at 30 s. 0 failures = 0.
    inline uint64_t connectBackoffBaseMs(unsigned consecFails)
    {
        if (consecFails == 0u)
            return 0u;
        uint64_t base = kConnectBackoffBaseMs;
        for (unsigned i = 1u; i < consecFails && base < kConnectBackoffCapMs; ++i)
        {
            base *= 2u;
            if (base > kConnectBackoffCapMs)
                base = kConnectBackoffCapMs;
        }
        return base;
    }

    // Equal jitter: half the base is guaranteed spacing, the other half is
    // random. rand01 in [0,1]; values outside clamp. Returns the ms to wait
    // after a failure before the next real SYN.
    inline uint64_t connectBackoffDelayMs(uint64_t baseMs, double rand01)
    {
        if (rand01 < 0.0)
            rand01 = 0.0;
        if (rand01 > 1.0)
            rand01 = 1.0;
        const uint64_t half = baseMs / 2u;
        return half + static_cast<uint64_t>(rand01 * static_cast<double>(baseMs - half));
    }

    // True when a new real connect attempt may start: first attempt, a past
    // success (consecFails == 0), or the backoff window has elapsed.
    inline bool mayAttemptConnect(uint64_t nowMs, uint64_t lastAttemptMs, unsigned consecFails,
                                  double rand01)
    {
        if (consecFails == 0u)
            return true;
        if (nowMs < lastAttemptMs)
            return false;
        return nowMs - lastAttemptMs >=
               connectBackoffDelayMs(connectBackoffBaseMs(consecFails), rand01);
    }

    // DNS give-up: the async resolver thread may block indefinitely (dead
    // DNS). After kDnsGiveUpMs with no resolution the client fails fast
    // into the refused-connection path instead of polling name/done
    // forever. A late resolution still heals later entries (the thread
    // keeps writing the address).
    inline bool dnsGiveUp(uint64_t resolveStartMs, uint64_t nowMs, bool resolved)
    {
        if (resolved)
            return false;
        if (nowMs < resolveStartMs)
            return false;
        return nowMs - resolveStartMs >= kDnsGiveUpMs;
    }

    // qp-freeze-1: pre-dial reachability probe kinds. Pure mapping to the
    // player-facing copy (same words as the HLE dial-failure toasts);
    // unit-tested in ps2_net_protocol_tests.cpp.
    enum class ProbeFail { Dns, Timeout, Refused };
    inline const char *probeFailCopy(ProbeFail f)
    {
        switch (f)
        {
        case ProbeFail::Dns:
            return "Online unavailable (address lookup failed). Offline play works - check connection.";
        case ProbeFail::Timeout:
            return "Online unavailable (no answer). Offline play works - retry from the Online menu.";
        case ProbeFail::Refused:
            return "Online unavailable (server refused). Offline play works - retry from the Online menu.";
        }
        return "Online unavailable. Offline play works - retry from the Online menu.";
    }

    // netsec-fix-1 F1: DirtySock TCP frame admission. The 12-byte header
    // carries a big-endian total size (header included). Valid sizes are
    // 12..65536; anything else is a corrupt frame: Reject (drop the frame
    // and close the session), never whole-buffer delivery. A valid size
    // with fewer bytes held waits for the rest.
    constexpr uint32_t kTcpFrameMin = 12u;
    constexpr uint32_t kTcpFrameMax = 65536u;

    enum class TcpFrameAction
    {
        Deliver,
        Wait,
        Reject,
    };

    inline TcpFrameAction classifyTcpFrame(uint32_t declaredSize, size_t heldBytes)
    {
        if (declaredSize < kTcpFrameMin || declaredSize > kTcpFrameMax)
            return TcpFrameAction::Reject;
        if (heldBytes < declaredSize)
            return TcpFrameAction::Wait;
        return TcpFrameAction::Deliver;
    }

    // netsec-fix-1 F2: one O5PK game body (tag stripped) is a single
    // length-prefixed record: a little-endian u16 total length at offset 0
    // equal to the datagram body length. Anything else is dropped before
    // queueing or concatenating. Exact equality also pins the signed
    // interpretation the guest parser uses (bodies are datagram-sized, far
    // below 0x8000). Proven against 150,729/150,729 wire bodies in the
    // online-stock-2 captures: every legit body is exactly one record.
    inline bool udpGameBodyValid(const uint8_t *body, size_t n)
    {
        if (!body || n < 2u)
            return false;
        const uint32_t declared = static_cast<uint32_t>(body[0]) |
                                  (static_cast<uint32_t>(body[1]) << 8);
        return declared == n;
    }

    // netsec-fix-1 F2: a delivery concatenation must tile exactly into
    // single records (each record's declared length consumes its slice).
    // Proven against 142,518/142,518 delivery bodies in the same captures.
    inline bool udpConcatBodyValid(const uint8_t *body, size_t n)
    {
        if (!body || n < 2u)
            return false;
        size_t off = 0u;
        while (off < n)
        {
            if (off + 2u > n)
                return false;
            const size_t declared = static_cast<size_t>(body[off]) |
                                    (static_cast<size_t>(body[off + 1]) << 8);
            if (declared < 2u || off + declared > n)
                return false;
            off += declared;
        }
        return off == n;
    }

    // netsec-fix-1 F6: relay-pinned peer source check. With a lobby-issued
    // relay route the util pins the relay endpoint (IP + port) and the
    // session token at conn time; afterwards only datagrams from that exact
    // endpoint under the still-current token are accepted.
    inline bool relaySourceAllowed(uint32_t pinnedIp, uint16_t pinnedPort,
                                   const std::string &pinnedToken, const std::string &curToken,
                                   uint32_t fromIp, uint16_t fromPort)
    {
        if (pinnedToken.empty() || pinnedToken != curToken)
            return false;
        return fromIp == pinnedIp && fromPort == pinnedPort;
    }

    // qp-failclosed-1: NAV=direct fail-closed send decision. Without a
    // usable login the wire-identity rewrite is a no-op and a game-sent
    // auth would still carry the Mc-served bootstrap identity, so the
    // direct path refuses the send (drops it, sends nothing) instead of
    // failing open. Only auth messages carry the identity; every other
    // type is unaffected. Other NAV modes keep their existing behavior
    // (the walk-path B2(b) recovery stays out of scope).
    inline bool shouldRefuseDirectAuthSend(bool isAuth, int navModeValue, bool loginUsable)
    {
        return isAuth && navModeValue == 3 && !loginUsable;
    }
}
