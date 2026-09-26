#include "runtime/gs/ps2_gif_arbiter.h"
#include "ps2_mtvu.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace
{
    // GB2 capture tap for the queue determinism test's captured stream.
    // PS2X_GS_CAPTURE_DIR set: writes the first PS2X_GS_CAPTURE_N submitted
    // packets (default 256, 64 MiB total cap) as cap-<seq>-p<path>-<size>.bin
    // plus a cap-index.txt manifest. Unset: zero behavior change.
    std::mutex g_captureMutex;
    std::atomic<uint32_t> g_captureCount{0};
    size_t g_captureBytes = 0;
    constexpr size_t kCaptureByteCap = 64u * 1024u * 1024u;

    void capturePacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes)
    {
        static const char *dir = std::getenv("PS2X_GS_CAPTURE_DIR");
        if (!dir || !data || sizeBytes == 0u)
            return;
        static const uint32_t maxPackets = [] {
            if (const char *n = std::getenv("PS2X_GS_CAPTURE_N"))
            {
                char *end = nullptr;
                const long v = std::strtol(n, &end, 10);
                if (end != n && v > 0 && v < 1000000L)
                    return static_cast<uint32_t>(v);
            }
            return 256u;
        }();
        const uint32_t seq = g_captureCount.fetch_add(1u, std::memory_order_relaxed);
        if (seq >= maxPackets)
            return;
        std::lock_guard<std::mutex> lock(g_captureMutex);
        if (g_captureBytes + sizeBytes > kCaptureByteCap)
            return;
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/cap-%06u-p%u-%u.bin", dir, seq,
                      static_cast<unsigned>(pathId), sizeBytes);
        if (FILE *f = std::fopen(path, "wb"))
        {
            std::fwrite(data, 1, sizeBytes, f);
            std::fclose(f);
            g_captureBytes += sizeBytes;
        }
        char indexPath[1024];
        std::snprintf(indexPath, sizeof(indexPath), "%s/cap-index.txt", dir);
        if (FILE *f = std::fopen(indexPath, seq == 0u ? "w" : "a"))
        {
            std::fprintf(f, "%06u path=%u bytes=%u\n", seq, static_cast<unsigned>(pathId), sizeBytes);
            std::fclose(f);
        }
    }
}

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    ps2_mtvu::touch(ps2_mtvu::Site::ArbSubmit); // MT1: unit-owned
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
    capturePacket(pathId, data, sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;
    if (!m_queue.empty())
        ps2_mtvu::touch(ps2_mtvu::Site::ArbDrain); // MT1: unit-owned

    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            // E33: the listener runs first so GS draw attribution lands on
            // this packet's path before the process function draws with it.
            if (m_packetListener)
            {
                m_packetListener(pkt.pathId, static_cast<uint32_t>(pkt.data.size()));
            }
            // G44: shadow observes first, same order, path preserved.
            if (m_shadowFn)
            {
                m_shadowFn(pkt.pathId, pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
            }
            if (m_processPathFn)
                m_processPathFn(pkt.pathId, pkt.data);
            else
                m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        }
    }
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
