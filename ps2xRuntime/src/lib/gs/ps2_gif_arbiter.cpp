#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_worker.h"
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

namespace ps2_gif_digest
{
    namespace
    {
        constexpr uint64_t kPrime = 1099511628211ull;
        constexpr uint64_t kEvery = 16384u;
        std::mutex g_mutex;
        uint64_t g_fnv = 14695981039346656037ull;
        uint64_t g_packets = 0;
        uint64_t g_bytes = 0;
        uint64_t g_markers = 0;

        void mixByte(uint8_t b)
        {
            g_fnv ^= b;
            g_fnv *= kPrime;
        }
        void mixU64(uint64_t v)
        {
            for (int i = 0; i < 8; ++i)
                mixByte(static_cast<uint8_t>(v >> (i * 8)));
        }
        void printLine(const char *tag)
        {
            std::fprintf(stderr, "[gif-digest] n=%llu fnv=%016llx bytes=%llu markers=%llu%s\n",
                         static_cast<unsigned long long>(g_packets), static_cast<unsigned long long>(g_fnv),
                         static_cast<unsigned long long>(g_bytes), static_cast<unsigned long long>(g_markers), tag);
        }
        void atExit()
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            printLine(" final");
        }
    }

    bool enabled()
    {
        static const bool on = [] {
            const char *env = std::getenv("PS2X_GIF_DIGEST");
            const bool v = env && std::strcmp(env, "1") == 0;
            if (v)
                std::atexit(atExit);
            return v;
        }();
        return on;
    }

    void mixPacket(uint8_t path, const uint8_t *data, uint32_t sizeBytes)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        mixByte(0xA0u);
        mixByte(path);
        mixU64(sizeBytes);
        for (uint32_t i = 0; i < sizeBytes; ++i)
            mixByte(data[i]);
        g_bytes += sizeBytes;
        if (++g_packets % kEvery == 0u)
            printLine("");
    }

    void mixMarker(uint8_t kind, uint64_t value)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        mixByte(0xB0u);
        mixByte(kind);
        mixU64(value);
        ++g_markers;
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

    if (ps2_mtvu::mp2Census()) // MP2: per-path GIF bytes (logged, not hashed)
        ps2_mtvu::noteMp2GifSubmit(static_cast<int>(pathId), sizeBytes);
    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    if (m_pool) // GP4 H5: pooled buffer when one fits, else a fresh vector
        pkt.data = m_pool->acquire(sizeBytes);
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
    capturePacket(pathId, data, sizeBytes);
    m_queue.push_back(std::move(pkt));
}

std::vector<uint8_t> GifArbiter::copyForSubmit(const uint8_t *data, uint32_t sizeBytes) const
{
    // Same bytes as submit(): pooled buffer when one fits, then resize+memcpy.
    std::vector<uint8_t> out;
    if (m_pool)
        out = m_pool->acquire(sizeBytes);
    out.resize(sizeBytes);
    std::memcpy(out.data(), data, sizeBytes);
    return out;
}

void GifArbiter::submitStaged(GifPathId pathId, std::vector<uint8_t> &&bytes, bool path2DirectHl)
{
    ps2_mtvu::touch(ps2_mtvu::Site::ArbSubmit); // MT1: unit-owned
    const uint32_t sizeBytes = static_cast<uint32_t>(bytes.size());
    if (bytes.empty() || sizeBytes < 16 || !m_processFn)
        return;

    if (ps2_mtvu::mp2Census()) // MP2: per-path GIF bytes (logged, not hashed)
        ps2_mtvu::noteMp2GifSubmit(static_cast<int>(pathId), sizeBytes);
    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(bytes.data(), sizeBytes);
    capturePacket(pathId, bytes.data(), sizeBytes);
    pkt.data = std::move(bytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;
    if (!m_queue.empty())
        ps2_mtvu::touch(ps2_mtvu::Site::ArbDrain); // MT1: unit-owned

    // MP1 L3: libc++ stable_sort takes a temporary buffer on every call for
    // this element type (one alloc/free per drain; most drains hold one
    // packet). Up to two packets the result is fixed by one compare in both
    // libc++ and libstdc++, so do that without the call. drainsBefore is not
    // a strict weak ordering (the DIRECTHL/IMAGE rule), so three or more
    // packets always go through stable_sort itself.
    if (m_queue.size() <= 2u)
    {
        if (m_queue.size() == 2u && drainsBefore(m_queue[1], m_queue[0]))
            std::swap(m_queue[0], m_queue[1]);
    }
    else
    {
        std::stable_sort(m_queue.begin(), m_queue.end(), drainsBefore);
    }

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            if (ps2_gif_digest::enabled()) // VPL1 (logged only)
                ps2_gif_digest::mixPacket(static_cast<uint8_t>(pkt.pathId), pkt.data.data(),
                                          static_cast<uint32_t>(pkt.data.size()));
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
            const ps2_mtvu::GifEmitPathScope emitPath(static_cast<uint8_t>(pkt.pathId)); // MQ2
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

bool GifArbiter::drainsBefore(const GifArbiterPacket &a, const GifArbiterPacket &b)
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
}
