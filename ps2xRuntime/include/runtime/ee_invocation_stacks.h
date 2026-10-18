#pragma once

// EE invocation stacks (online-stock-2, IPB41-os2 iOS exit): each (guest
// thread id, invocation depth) gets a 16 KB guest stack carved from the
// async callback band, cached in one map. Entries used to live forever, and
// thread ids cycle 2..255, so every new thread that took an interrupt or
// callback leaked a slot; under PS2X_HLE_POOLS_LOW=1 the band holds 19
// slots and online's extra short-lived threads exhausted it at race load
// ("EE invocation stack space exhausted", clean exit). A thread going
// dormant (exit, terminate; delete requires dormant) now returns its slots
// to a free list that the next new key takes before carving.
//
// The free list lives in the same map, under a thread-id word no guest
// thread can have (ids are -N..255), with the stack top as the low word, so
// the savestate format is unchanged and old states load (no free entries).

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ps2_ee_invocation_stacks
{
using Map = std::unordered_map<uint64_t, uint32_t>;

inline constexpr uint32_t kFreeThreadWord = 0x7FFFFFFFu;

inline uint64_t key(int threadId, uint32_t depth)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(threadId)) << 32u) | depth;
}

inline bool isFree(uint64_t k)
{
    return static_cast<uint32_t>(k >> 32u) == kFreeThreadWord;
}

// The stack for `k`: cached, else the highest freed stack, else carve()
// (0 when the band is exhausted).
template <typename Carve>
uint32_t take(Map &map, uint64_t k, Carve &&carve)
{
    if (const auto it = map.find(k); it != map.end())
        return it->second;
    uint32_t top = 0u;
    for (const auto &[entry, stack] : map)
        if (isFree(entry) && stack > top)
            top = stack;
    if (top != 0u)
        map.erase((static_cast<uint64_t>(kFreeThreadWord) << 32u) | top);
    else
        top = carve();
    if (top != 0u)
        map.emplace(k, top);
    return top;
}

// Return every stack thread `threadId` holds to the free list.
inline size_t release(Map &map, int threadId)
{
    const uint32_t word = static_cast<uint32_t>(threadId);
    std::vector<uint64_t> owned;
    for (const auto &[entry, stack] : map)
        if (static_cast<uint32_t>(entry >> 32u) == word)
            owned.push_back(entry);
    std::sort(owned.begin(), owned.end());
    for (const uint64_t entry : owned)
    {
        const uint32_t top = map[entry];
        map.erase(entry);
        map.emplace((static_cast<uint64_t>(kFreeThreadWord) << 32u) | top, top);
    }
    return owned.size();
}
} // namespace ps2_ee_invocation_stacks
