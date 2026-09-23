#pragma once

#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <vector>

class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;

    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;

    // GB3 Part 2: backends that decode GIF themselves (paraLLEl-GS) take the
    // raw stream instead of the frontend's primitive batches. The frontend
    // still decodes every packet (CSR SIGNAL/FINISH/LABEL, transfer state,
    // preferred display source) and still calls Submit/UploadImage, which
    // such a backend ignores. Defaults: not wanted, no-ops.
    virtual bool WantsRawGif() const { return false; }
    virtual void RawGifPacket(uint32_t path, const uint8_t *data, uint32_t sizeBytes)
    {
        (void)path;
        (void)data;
        (void)sizeBytes;
    }
    // HLE register writes that bypass the GIF stream (GS::writeRegister).
    virtual void RawWriteRegister(uint8_t regAddr, uint64_t value)
    {
        (void)regAddr;
        (void)value;
    }
};
