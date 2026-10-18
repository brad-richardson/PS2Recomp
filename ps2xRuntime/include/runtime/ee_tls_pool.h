#pragma once

// HLE TLS pool free list (tls-ring-1): `allocTlsAddr`
// (Syscalls/Helpers/Runtime.h) carved 256-byte TLS blocks from a 256-slot
// ring (`g_tls_index++ % 256`) and never freed them, so after 256 lazy
// per-thread allocations a new thread's block aliased a live thread's — the
// same churn pattern as the async-stack leak fixed by `5da76a70` (whose
// `ee_invocation_stacks.h` this mirrors).
//
// A thread going dormant (exit, terminate; delete requires dormant) returns
// its slot to `free`, and the next allocation takes the highest freed slot
// before carving a new one. With an empty free list the carve order is the
// old ring order, so the first 256 allocations keep identical addresses.
// When the pool is genuinely exhausted (256 live holders, no freed slot)
// allocation fails closed with `kNone` instead of aliasing a live block,
// matching the bootmode pool and the async-callback band conventions.
//
// Savestate format is unchanged: only `g_tls_index` (high-water mark) and
// the threads' `tlsBase` fields are persisted, as before. The free list is
// transient and rebuilt from those two after a load (every carved slot below
// the high-water mark that no loaded thread holds is free).

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ps2_tls_pool
{
inline constexpr uint32_t kNone = 0xFFFFFFFFu;

// Highest freed slot, or kNone when the free list is empty.
inline uint32_t takeFree(std::vector<uint32_t> &free)
{
    if (free.empty())
        return kNone;
    uint32_t top = 0u;
    for (const uint32_t slot : free)
        top = slot > top ? slot : top;
    free.erase(std::find(free.begin(), free.end(), top));
    return top;
}

// Return `slot` to the free list. Ignores slots outside the carved range
// and slots already free (double free), so a stray or repeated release can
// never hand one block to two threads.
inline void release(std::vector<uint32_t> &free, uint32_t slot, uint32_t highWater, uint32_t count)
{
    if (slot >= count || slot >= highWater)
        return;
    if (std::find(free.begin(), free.end(), slot) != free.end())
        return;
    free.push_back(slot);
}

// Rebuild the free list from the persisted high-water mark and the sorted
// live slots: every carved slot no live thread holds is free.
inline void rebuild(std::vector<uint32_t> &free,
                    uint32_t highWater,
                    const std::vector<uint32_t> &liveSlots,
                    uint32_t count)
{
    free.clear();
    const uint32_t carved = highWater < count ? highWater : count;
    for (uint32_t slot = 0u; slot < carved; ++slot)
    {
        if (!std::binary_search(liveSlots.begin(), liveSlots.end(), slot))
            free.push_back(slot);
    }
}

// Slot index for a pool address, or kNone when the address is outside the
// pool or misaligned. Pure arithmetic: no guest memory is touched.
inline uint32_t slotOf(uint32_t addr, uint32_t poolBase, uint32_t blockSize, uint32_t count)
{
    if (blockSize == 0u || addr < poolBase)
        return kNone;
    const uint32_t offset = addr - poolBase;
    if (offset % blockSize != 0u)
        return kNone;
    const uint32_t slot = offset / blockSize;
    return slot < count ? slot : kNone;
}

inline uint32_t addrOf(uint32_t slot, uint32_t poolBase, uint32_t blockSize)
{
    return poolBase + slot * blockSize;
}
} // namespace ps2_tls_pool
