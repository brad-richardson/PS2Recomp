#pragma once

// HDL1: headless ("no-raster") GS referee backend.
//
// A GSRasterBackend behind PS2X_GS_BACKEND=headless (default off) that
// consumes nothing itself: the GS frontend still decodes every GIF packet
// for guest-visible state (registers, privileged CSR/SIGNAL/LABEL,
// transfer bookkeeping, submit-side FINISH), but no primitive is
// rasterized, no texture is uploaded, no VRAM byte is written, no window
// is opened and no frame is ever presented.
//
// Guest-visibility contract (same as the NP0A canonical profile):
//   - CSR (FINISH/SIGNAL), IMR, SIGLBLID: frontend-owned, unaffected.
//   - GIF/PATH3 transfer completion: EE-side DMA state, unaffected.
//   - Reverse-DMA readbacks: canonical mode answers fixed bytes through
//     guest RAM without touching the backend; outside canonical mode this
//     backend serves nothing (0 bytes), so readback-on routes must use a
//     raster backend. Under CANON the HLE store-image route additionally
//     fills canonical bytes here (np-fix-1 F2), so even that route never
//     succeeds with 0 bytes.
// The VU1 microprograms still execute; only the GS draw is skipped.

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <memory>

namespace ps2x_gs_headless
{
// PS2X_GS_BACKEND=headless (exact match).
bool requested();

// Always built (no extra linkage); true.
bool available();

std::unique_ptr<GSRasterBackend> create();
} // namespace ps2x_gs_headless

class GSHeadlessBackend final : public GSRasterBackend
{
public:
    GSHeadlessBackend() = default;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;

    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;
};
