// HDL1: headless ("no-raster") GS referee backend. See the header for the
// guest-visibility contract. Every raster entry point is a no-op; transfer
// and savestate queries report the idle/empty state so the save path never
// defers on backend work that does not exist.

#include "runtime/gs/gs_headless_backend.h"
#include "runtime/ps2_savestate.h"

#include <cstdlib>
#include <cstring>

namespace ps2x_gs_headless
{
bool requested()
{
    const char *env = std::getenv("PS2X_GS_BACKEND");
    return env && std::strcmp(env, "headless") == 0;
}

bool available()
{
    return true;
}

std::unique_ptr<GSRasterBackend> create()
{
    if (!available())
        return nullptr;
    return std::unique_ptr<GSRasterBackend>(new GSHeadlessBackend());
}
} // namespace ps2x_gs_headless

void GSHeadlessBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    (void)vram;
    (void)vramSize;
}

void GSHeadlessBackend::Reset()
{
}

void GSHeadlessBackend::Submit(const GSPrimitiveBatch &batch)
{
    (void)batch;
}

void GSHeadlessBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    (void)tex0;
    (void)texclut;
}

void GSHeadlessBackend::BeginTransfer(const GSTransferCommand &command)
{
    (void)command;
}

void GSHeadlessBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    (void)data;
    (void)sizeBytes;
}

void GSHeadlessBackend::Flush()
{
}

void GSHeadlessBackend::TextureFlush()
{
}

void GSHeadlessBackend::Sync(GSSyncReason reason)
{
    (void)reason;
}

PresentationFrame GSHeadlessBackend::Present(const GSPresentationRequest &request)
{
    (void)request;
    return PresentationFrame{};
}

bool GSHeadlessBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    (void)context;
    (void)rgba;
    return false;
}

uint32_t GSHeadlessBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    // np-fix-1 F2: under CANON the referee backend serves canonical bytes
    // (the same fixed-0xff policy as the VIF1 probe path) instead of
    // nothing, so no guest readback route can succeed with 0 bytes.
    if (dst && maxBytes != 0u && ps2_savestate::canonicalEnabled())
    {
        std::memset(dst, 0xff, maxBytes);
        return maxBytes;
    }
    (void)dst;
    (void)maxBytes;
    return 0u;
}

uint32_t GSHeadlessBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    (void)psm;
    (void)base;
    (void)bw;
    (void)x;
    (void)y;
    return 0u;
}

void GSHeadlessBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    (void)psm;
    (void)base;
    (void)bw;
    (void)x;
    (void)y;
    (void)value;
}

void GSHeadlessBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    out.clear();
}

GSTransferSnapshot GSHeadlessBackend::GetTransferSnapshot() const
{
    return GSTransferSnapshot{};
}
