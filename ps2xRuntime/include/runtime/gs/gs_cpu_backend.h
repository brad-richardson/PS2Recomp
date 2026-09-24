#pragma once

#include "runtime/gs/gs_backend.h"

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

// Field-presentation control for interlaced field-mode output (SMODE2
// interlaced, frame mode off): PS2X_DEINTERLACE, default "weave" (present
// every line of the full-height buffer). "bob" restores the historical
// behavior of showing one field's lines doubled, alternating each vsync.
bool ps2xDeinterlaceBobValue(const char *value);
uint32_t ps2xDeinterlaceSourceLine(uint32_t y, uint32_t height, bool oddField, bool bob);
void ps2xSetDeinterlaceBobForTest(bool bob);
void ps2xClearDeinterlaceBobForTest();

// N8D7M5 (Mac-only test diagnostic, default OFF): counts executed CPU
// VRAM-write operations by kind at four entry points (draw = DrawPrimitive,
// transfer = UploadImage/PerformLocalToLocalTransfer, clear =
// ClearFramebuffer). Counters advance only while enabled; the replay harness
// enables them when PS2X_GS_REPLAY_WORDS is set and attributes watched-word
// transitions to the kind(s) that advanced during the executing packet.
struct Ps2xN8D7M5Counts
{
    uint64_t draw = 0u;
    uint64_t transfer = 0u;
    uint64_t clear = 0u;
};
void ps2xN8D7M5SetCounting(bool enabled);
Ps2xN8D7M5Counts ps2xN8D7M5Counts();

class GSCpuBackend final : public GSRasterBackend
{
public:
    GSCpuBackend();

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
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

private:
    void ResetUnlocked();
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    void DrawPrimitive(const GSPrimitiveBatch &batch);
    void DrawSprite(const GSPrimitiveBatch &batch);
    void DrawTriangle(const GSPrimitiveBatch &batch);
    void DrawLine(const GSPrimitiveBatch &batch);
    void WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);
    uint32_t SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v);
    uint32_t LookupCLUT(const GSDrawState &state, uint8_t index, uint32_t cbp, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm);

    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY) const;

    using WriteVramFunc = std::function<void(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>;
    using ReadVramFunc = std::function<uint32_t(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t)>;

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;
};
