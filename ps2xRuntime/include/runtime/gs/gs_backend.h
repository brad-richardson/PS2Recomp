#pragma once

#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <string>
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
    // raw stream instead of the frontend's primitive batches. By default the
    // frontend still decodes every packet for state and transfers; a backend
    // can request the smaller guest-control path below. Defaults: no-ops.
    virtual bool WantsRawGif() const { return false; }
    // A live external GS owns drawing state. The frontend only needs the
    // guest-visible control registers and transfer/FIFO bookkeeping.
    virtual bool WantsMinimalGifDecode() const { return false; }
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
    // GE2: HLE-decoded packed-GIF packets (GS::processNativePackedGIFPacket)
    // never enter the raw GIF stream, but the capture records them as
    // packets, so a recording/external backend must see them too to prove
    // no dropped operation. Defaults: no-op (existing backends unaffected).
    virtual void RawNativePackedPacket(const uint8_t *data, uint32_t sizeBytes)
    {
        (void)data;
        (void)sizeBytes;
    }
    // GE2: guest VSync at every VBlank, independent of host presents. The
    // frontend enqueues one fire-and-forget command per VBlankStart (after
    // the CSR FIELD update) iff the backend opts in. `field` is the CSR
    // FIELD bit (0/1); a PCSX2 adapter maps it to GSvsync's convention
    // (FIELD ? 0 : 1) at its own boundary. Defaults: opted out, no-op.
    virtual bool WantsGuestVsync() const { return false; }
    virtual void GuestVsync(uint64_t tick, uint32_t field)
    {
        (void)tick;
        (void)field;
    }
    // GE2: privileged-register mirroring in stream order. After every
    // GS::privWrite apply, the frontend diffs the 19 small priv registers
    // and forwards each changed (offset, value) iff the backend opts in.
    // The frontend keeps owning CSR/SIGNAL/FINISH/LABEL semantics; this is
    // a value mirror for an external GS (PCSX2 GSPrivRegSet/GSwriteCSR).
    // Defaults: opted out, no-op.
    virtual bool WantsPrivMirror() const { return false; }
    virtual void PrivMirrored(uint32_t registerOffset, uint64_t value)
    {
        (void)registerOffset;
        (void)value;
    }
    // SS1 save states (on the GS thread, after a drain). Idle = no transfer
    // or local->host bytes in flight. Save/Load carry state that is not in
    // PS2Memory (the CPU backend's VRAM is PS2Memory's, so it has none).
    // SS3: SavestateBusyReason names the deferral ("gs-transfer") when idle
    // is false for a specific new cause; "" keeps the generic text.
    // SQ1: SavestateQuiesce settles host-side-only pending work (a render
    // tail, palette uploads) so a requested save can land; true when it
    // flushed anything. It never invents guest data: state that still
    // awaits the guest keeps reporting busy. Runs only on the save path.
    virtual bool SavestateIdle() const
    {
        const GSTransferSnapshot t = GetTransferSnapshot();
        return t.localToHostPendingBytes == 0u && t.copiedPixels >= t.totalPixels;
    }
    virtual std::string SavestateBusyReason() const { return {}; }
    virtual bool SavestateQuiesce() { return false; }
    virtual void SavestateSave(std::vector<uint8_t> &out) { out.clear(); }
    virtual bool SavestateLoad(const uint8_t *data, size_t size)
    {
        (void)data;
        return size == 0u;
    }

    // BG1: persist host-side caches (external GS: the Vulkan pipeline cache
    // plus newly recorded TFX selectors). Default no-op; only the external
    // backend implements it. Runs on the GS worker at stream position.
    virtual void FlushCaches() {}
};
