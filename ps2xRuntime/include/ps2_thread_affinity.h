#pragma once

// N11: PS2X_GAME_THREAD_CPUS ("6,7") — parse a comma-separated cpu list and
// pin the calling thread to it. Unset/empty = no change (default off). The
// game thread applies this to itself at start (ps2_runtime.cpp), which needs
// no privilege; the shell cannot set another UID's affinity (N11 Part 1).

#include <string_view>
#include <vector>

#if defined(__linux__) || defined(__ANDROID__)
#include <sched.h>
#endif

namespace ps2x
{
// Parses "6,7" (surrounding whitespace tolerated) into cpu ids. Items that
// are empty, non-numeric, or negative are skipped; values are NOT range
// checked here (CPU_SET clamps, sched_setaffinity reports EINVAL).
inline std::vector<int> parseCpuList(std::string_view text)
{
    std::vector<int> out;
    std::size_t i = 0;
    while (i <= text.size())
    {
        std::size_t end = text.find(',', i);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view item = text.substr(i, end - i);
        const std::size_t first = item.find_first_not_of(" \t");
        const std::size_t last = item.find_last_not_of(" \t");
        if (first != std::string_view::npos)
        {
            item = item.substr(first, last - first + 1);
            bool digits = !item.empty();
            for (char c : item)
                digits = digits && (c >= '0' && c <= '9');
            if (digits)
            {
                long value = 0;
                for (char c : item)
                {
                    value = value * 10 + (c - '0');
                    if (value > 1000000)
                        break;
                }
                if (value <= 1000000)
                    out.push_back(static_cast<int>(value));
            }
        }
        i = end + 1;
    }
    return out;
}

// Pins the calling thread to cpus. Returns the sched_setaffinity rc
// (0 = ok), or -1 when cpus is empty or the platform lacks affinity.
inline int pinCurrentThreadToCpus(const std::vector<int> &cpus)
{
#if defined(__linux__) || defined(__ANDROID__)
    if (cpus.empty())
        return -1;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus)
    {
        if (c >= 0 && c < CPU_SETSIZE)
            CPU_SET(c, &set);
    }
    return sched_setaffinity(0, sizeof(set), &set);
#else
    (void)cpus;
    return -1;
#endif
}

} // namespace ps2x
