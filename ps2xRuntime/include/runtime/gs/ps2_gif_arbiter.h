#ifndef PS2_GIF_ARBITER_H
#define PS2_GIF_ARBITER_H

#include <cstdint>
#include <functional>
#include <vector>

enum class GifPathId : uint8_t
{
    Path1 = 1,
    Path2 = 2,
    Path3 = 3,
};

struct GifArbiterPacket
{
    GifPathId pathId;
    bool path2DirectHl = false;
    bool path3Image = false;
    std::vector<uint8_t> data;
};

class GifArbiter
{
public:
    using ProcessPacketFn = std::function<void(const uint8_t *, uint32_t)>;
    // E33: fired from drain() before each packet reaches the process
    // function, so observers (per-path GIF census, GS draw attribution) see
    // the path the process function itself does not carry.
    using PacketListenerFn = std::function<void(GifPathId, uint32_t)>;
    // G44 shadow tap: observes each drained packet WITH its path, in the same
    // order the CPU backend sees it. Unset = zero behavior change.
    using ShadowPacketFn = std::function<void(GifPathId, const uint8_t *, uint32_t)>;
    // GF1 (PS2X_GS_HANDOFF_DIET): when set, drain() hands each packet to this
    // instead of the process function, with its path and its own byte vector
    // (the callee may take the bytes). Called at the same point, after the
    // listener and the shadow tap.
    using ProcessPathPacketFn = std::function<void(GifPathId, std::vector<uint8_t> &)>;

    GifArbiter() = default;
    explicit GifArbiter(ProcessPacketFn processFn);

    void setProcessPacketFn(ProcessPacketFn fn) { m_processFn = std::move(fn); }
    void setPacketListener(PacketListenerFn fn) { m_packetListener = std::move(fn); }
    void setShadowPacketFn(ShadowPacketFn fn) { m_shadowFn = std::move(fn); }
    void setProcessPathPacketFn(ProcessPathPacketFn fn) { m_processPathFn = std::move(fn); }

    void submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl = false);

    void drain();
    bool empty() const { return m_queue.empty(); }

private:
    ProcessPacketFn m_processFn;
    PacketListenerFn m_packetListener;
    ShadowPacketFn m_shadowFn;
    ProcessPathPacketFn m_processPathFn;
    std::vector<GifArbiterPacket> m_queue;

    static bool isImagePacket(const uint8_t *data, uint32_t sizeBytes);
    static uint8_t pathPriority(GifPathId id);
};

#endif
