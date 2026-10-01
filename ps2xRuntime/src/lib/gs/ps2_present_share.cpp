// HR1 shared-frame mailbox for PS2X_PRESENT_ZERO_COPY (see ps2_present_share.h).
// CN2b: moved verbatim out of ps2_gs_parallel_backend.cpp when paraLLEl-GS was
// removed; the GE1 iOS IOSurface export publishes here too.
#include "runtime/gs/ps2_present_share.h"

#include <cstdlib>
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
