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
        return 0;
    }

    inline bool shouldArmOnline(int mode, bool armed, uint64_t count, uint64_t seen)
    {
        return mode == 2 && !armed && count != 0u && count != seen;
    }

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

    inline bool relayRegisterDue(uint32_t mode, bool peerKnown, bool enabled,
                                 uint64_t nowMs, uint64_t connMs, uint64_t lastRegMs)
    {
        return (mode & 1u) && !peerKnown && enabled && nowMs - connMs < 30000u &&
               nowMs - lastRegMs >= 250u;
    }

    inline std::string relayRegistration(const std::string &token)
    {
        return "SSX3RLY1" + token;
    }
}
