#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_worker.h"
#include "ps2_mtvu.h"
#include <algorithm>
#include <cassert>
#include <cstring>

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
    if (m_pool) // GP4 H5: pooled buffer when one fits, else a fresh vector
        pkt.data = m_pool->acquire(sizeBytes);
    if (m_noZeroFill)
    {
        // PKB1 item 1: the copy below overwrites every byte, so skip the
        // resize's zero-fill. assign() copies straight into the buffer
        // (reusing pooled capacity when it fits).
        pkt.data.assign(data, data + sizeBytes);
#ifndef NDEBUG
        // The buffer must hold exactly the source bytes: no stale pooled
        // byte may survive beside or beneath the copy.
        assert(pkt.data.size() == sizeBytes && std::memcmp(pkt.data.data(), data, sizeBytes) == 0);
#endif
    }
    else
    {
        pkt.data.resize(sizeBytes);
        std::memcpy(pkt.data.data(), data, sizeBytes);
    }
    m_queue.push_back(std::move(pkt));
}

std::vector<uint8_t> GifArbiter::copyForSubmit(const uint8_t *data, uint32_t sizeBytes) const
{
    // Same bytes as submit(): pooled buffer when one fits, then the copy.
    std::vector<uint8_t> out;
    if (m_pool)
        out = m_pool->acquire(sizeBytes);
    if (m_noZeroFill)
    {
        // PKB1 item 1: see submit(). assign() overwrites every byte.
        out.assign(data, data + sizeBytes);
#ifndef NDEBUG
        assert(out.size() == sizeBytes && std::memcmp(out.data(), data, sizeBytes) == 0);
#endif
    }
    else
    {
        out.resize(sizeBytes);
        std::memcpy(out.data(), data, sizeBytes);
    }
    return out;
}

void GifArbiter::submitStaged(GifPathId pathId, std::vector<uint8_t> &&bytes, bool path2DirectHl)
{
    ps2_mtvu::touch(ps2_mtvu::Site::ArbSubmit); // MT1: unit-owned
    const uint32_t sizeBytes = static_cast<uint32_t>(bytes.size());
    if (bytes.empty() || sizeBytes < 16 || !m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(bytes.data(), sizeBytes);
    pkt.data = std::move(bytes);
    m_queue.push_back(std::move(pkt));
}

bool GifArbiter::copyViewForSubmit(const uint8_t *data, uint32_t sizeBytes, GsGifArenaRef &outRef,
                                     uint32_t &outOff)
{
    // The one copy: VU1/memory bytes straight into the open arena. The
    // caller falls back to copyForSubmit when this returns false.
    if (!m_arenaPool || !data || sizeBytes < 16u || sizeBytes > GsGifArena::kBytes)
        return false;
    if (!m_arenaCur || static_cast<size_t>(m_arenaUsed) + sizeBytes > GsGifArena::kBytes)
    {
        // Seal the full arena (dropping this handle's ref; live views keep
        // it alive) and open the next one.
        m_arenaCur.reset();
        m_arenaCur = GsGifArenaRef(m_arenaPool->acquire());
        m_arenaUsed = 0u;
    }
    outOff = m_arenaUsed;
    std::memcpy(m_arenaCur.get()->bytes + m_arenaUsed, data, sizeBytes);
    m_arenaUsed += sizeBytes;
    outRef = m_arenaCur; // copy addrefs
    return true;
}

void GifArbiter::submitStagedView(GifPathId pathId, GsGifArenaRef arena, uint32_t off, uint32_t len,
                                  bool path2DirectHl)
{
    ps2_mtvu::touch(ps2_mtvu::Site::ArbSubmit); // MT1: unit-owned
    if (!arena || len < 16u || !m_processFn)
        return;
    if (static_cast<size_t>(off) + len > GsGifArena::kBytes)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(arena.get()->bytes + off, len);
    pkt.arena = std::move(arena);
    pkt.arenaOff = off;
    pkt.arenaLen = len;
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
        // GSB2: a view packet carries its bytes in an arena; an owned packet
        // in data. Both observe the same listener/shadow/scope order below.
        const bool isView = static_cast<bool>(pkt.arena);
        const uint8_t *bytes = nullptr;
        uint32_t size = 0u;
        if (isView)
        {
            bytes = pkt.arena.get()->bytes + pkt.arenaOff;
            size = pkt.arenaLen;
        }
        else if (!pkt.data.empty())
        {
            bytes = pkt.data.data();
            size = static_cast<uint32_t>(pkt.data.size());
        }
        if (bytes != nullptr)
        {
            // E33: the listener runs first so GS draw attribution lands on
            // this packet's path before the process function draws with it.
            if (m_packetListener)
            {
                m_packetListener(pkt.pathId, size);
            }
            // G44: shadow observes first, same order, path preserved.
            if (m_shadowFn)
            {
                m_shadowFn(pkt.pathId, bytes, size);
            }
            const ps2_mtvu::GifEmitPathScope emitPath(static_cast<uint8_t>(pkt.pathId)); // MQ2
            if (isView && m_processViewFn)
                m_processViewFn(pkt.pathId, std::move(pkt.arena), pkt.arenaOff, pkt.arenaLen);
            else if (isView)
            {
                // No view function (arena half-wired): copy the view and run
                // today's path. Production always sets the view function with
                // the pool, so this is defensive only.
                std::vector<uint8_t> owned(bytes, bytes + size);
                if (m_processPathFn)
                    m_processPathFn(pkt.pathId, owned);
                else
                    m_processFn(owned.data(), size);
            }
            else if (m_processPathFn)
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
