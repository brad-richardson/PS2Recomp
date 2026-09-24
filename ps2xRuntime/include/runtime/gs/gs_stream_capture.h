#pragma once

#include <cstdint>

namespace ps2x_gs_capture
{
    bool enabled();
    void packet(uint64_t tick, uint8_t path, const uint8_t *data, uint32_t sizeBytes);
    void packetDone(uint64_t tick, uint64_t index, uint8_t path,
                    const uint8_t *vram, uint32_t vramSize);
    void privWrite(uint64_t tick, uint32_t registerOffset, uint64_t value);
    void transfer(uint64_t tick, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg,
                  uint32_t direction);
    void nativeUpload(uint64_t tick, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg,
                      uint64_t trxdir, const uint8_t *data, uint32_t sizeBytes);
    void localToHost(uint64_t tick, uint32_t maxBytes, const uint8_t *data, uint32_t sizeBytes);
    void clearContext(uint64_t tick, uint32_t contextIndex, uint32_t rgba);
    void vblank(uint64_t tick);
    void close();
}
