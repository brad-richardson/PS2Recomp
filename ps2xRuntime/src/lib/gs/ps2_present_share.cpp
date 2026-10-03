// HR1 shared-frame mailbox for PS2X_PRESENT_ZERO_COPY (see ps2_present_share.h).
// CN2b: moved verbatim out of ps2_gs_parallel_backend.cpp when paraLLEl-GS was
// removed; the GE1 iOS IOSurface export publishes here too.
#include "runtime/gs/ps2_present_share.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>
#include <cstring>
#include <mutex>

namespace ps2x_present_share
{
namespace
{
std::mutex g_shareMutex;
SharedFrame g_shareFrame;
} // namespace

bool enabled()
{
#if defined(__APPLE__) // macOS: GL blit (HR1 prototype); iOS: GLES texture cache (HR1 spike)
    static const bool on = [] {
        const char *v = std::getenv("PS2X_PRESENT_ZERO_COPY");
        return v && std::strcmp(v, "1") == 0;
    }();
    return on;
#else
    return false;
#endif
}

// PSO1: compiled default for PS2X_PRESENT_OWNERSHIP (off until the
// orchestrator's gate flips it; 0 keeps the pre-PSO1 mailbox).
constexpr bool kOwnershipDefault = false;

bool ownershipEnabled()
{
#if defined(__APPLE__)
    static const bool on = [] {
        const char *v = std::getenv("PS2X_PRESENT_OWNERSHIP");
        if (!v || !*v)
            return kOwnershipDefault;
        return std::strcmp(v, "1") == 0;
    }();
    return on;
#else
    return false;
#endif
}

namespace
{
struct CaptureGate
{
    std::vector<uint64_t> targets;
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> blockedSinceNs{0};
    bool on = false;
};
CaptureGate &gate()
{
    static CaptureGate *g = [] {
        auto *c = new CaptureGate;
        const char *dir = std::getenv("PS2X_PRESENT_CAPTURE_DIR");
        const char *ticks = std::getenv("PS2X_PRESENT_CAPTURE_TICKS");
        if (dir && *dir && ticks && *ticks)
        {
            std::string v(ticks);
            size_t at = 0;
            while (at <= v.size())
            {
                const size_t comma = v.find(',', at);
                const std::string item = v.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (!item.empty())
                    c->targets.push_back(std::strtoull(item.c_str(), nullptr, 10));
                if (comma == std::string::npos)
                    break;
                at = comma + 1;
            }
            std::sort(c->targets.begin(), c->targets.end());
            c->on = !c->targets.empty();
        }
        return c;
    }();
    return *g;
}
uint64_t nowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
} // namespace

uint64_t captureGatePending()
{
    CaptureGate &g = gate();
    if (!g.on)
        return 0u;
    const size_t i = g.next.load(std::memory_order_acquire);
    return i < g.targets.size() ? g.targets[i] : 0u;
}

bool captureGateAllowsExport(uint64_t tick)
{
    const uint64_t t = captureGatePending();
    if (t == 0u || tick == t || tick + 4u < t)
        return true;
    uint64_t expected = 0u;
    gate().blockedSinceNs.compare_exchange_strong(expected, nowNs());
    return false;
}

uint64_t captureGateBlockedSinceNs()
{
    return gate().blockedSinceNs.load(std::memory_order_acquire);
}

void captureGateDone(uint64_t target)
{
    CaptureGate &g = gate();
    const size_t i = g.next.load(std::memory_order_acquire);
    if (i < g.targets.size() && g.targets[i] == target)
    {
        g.blockedSinceNs.store(0u, std::memory_order_release);
        g.next.store(i + 1u, std::memory_order_release);
    }
}

void publish(const SharedFrame &frame)
{
    std::lock_guard<std::mutex> lock(g_shareMutex);
    g_shareFrame = frame;
}

bool latest(SharedFrame &out)
{
    std::lock_guard<std::mutex> lock(g_shareMutex);
    out = g_shareFrame;
    return out.surface != nullptr;
}
} // namespace ps2x_present_share
