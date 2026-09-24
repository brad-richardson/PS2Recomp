#pragma once

#include "runtime/gs/gs_backend.h"

#include <array>
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

// GB7B bounded spatial title-glyph candidate probe (default OFF).
// Replay-only diagnostic: the harness sets the current GIF packet context
// before each processGIFPacket; DrawPrimitive logs textured/untextured
// batches whose clipped rect overlaps the title crops. With the probe
// closed/disabled every hook returns early and rendering is byte-identical.
struct Gb7bPacketContext
{
    uint64_t tick = 0;
    uint64_t packetIndex = 0;
    unsigned path = 0;
};
void ps2xGb7bProbeOpen(const char *path);
void ps2xGb7bProbeClose();
void ps2xGb7bSetPacketContext(uint64_t tick, uint64_t packetIndex, unsigned path);
bool ps2xGb7bProbeEnabled();

// GB7C2 bounded CPU pixel/ROI title-text chain probe (default OFF).
// Replay-only diagnostic for the single predeclared chain tick600 path2
// packet47176 (off-screen fbp=0 T4 sprites) -> tick601 path3 packet47240
// (same-column fbp=112/tbp0=0 display blit). The harness sets the current
// GIF packet context before each processGIFPacket; the backend assigns an
// intra-packet batch id per DrawPrimitive, logs a small fixed set of
// interior pixels per candidate sprite with independent old/new reads,
// and diffs the off-screen ROI per candidate batch. With the probe
// closed/disabled every hook returns early and rendering is byte-identical.
struct Gb7c2PacketContext
{
    uint64_t tick = 0;
    uint64_t packetIndex = 0;
    unsigned path = 0;
    uint64_t batch = 0;
};
void ps2xGb7c2ProbeOpen(const char *path);
void ps2xGb7c2ProbeClose();
void ps2xGb7c2SetPacketContext(uint64_t tick, uint64_t packetIndex, unsigned path);
bool ps2xGb7c2ProbeEnabled();

// GB7C4 actual carrier glyph-row sample probe (default OFF).
// Replay-only diagnostic for the four GB7C3 candidate glyph-row pairs:
// tick600 path2 packet47176 C1 source words (fbp=0 T4 sprites, STQ) ->
// tick601 path3 packet47240 carrier destinations (fbp=112/tbp0=0 blit,
// FST). The harness sets the current GIF packet context before each
// processGIFPacket; the backend assigns an intra-packet batch id per
// DrawPrimitive and logs the exact candidate pixels with pre/post-CLAMP
// UV, all four filtered tap coordinates plus swizzled VRAM word addresses
// from the runtime address helper, mask/blend/TEST state and independent
// old/new reads. With the probe closed/disabled every hook returns early
// and rendering is byte-identical.
struct Gb7c4PacketContext
{
    uint64_t tick = 0;
    uint64_t packetIndex = 0;
    unsigned path = 0;
    uint64_t batch = 0;
};
void ps2xGb7c4ProbeOpen(const char *path);
void ps2xGb7c4ProbeClose();
void ps2xGb7c4SetPacketContext(uint64_t tick, uint64_t packetIndex, unsigned path);
bool ps2xGb7c4ProbeEnabled();

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
    void NoteGb7bCandidate(const GSPrimitiveBatch &batch);
    // GB7C2 chain probe hooks (no-ops unless the probe is open).
    // BeginBatch assigns the intra-packet batch id and snapshots the
    // off-screen ROI for candidate batches; EndBatch diffs it.
    // TraceSpritePixel logs one interior pixel around its WritePixel call.
    uint64_t NoteGb7c2BatchBegin(const GSPrimitiveBatch &batch);
    void NoteGb7c2BatchEnd(const GSPrimitiveBatch &batch, uint64_t batchId);
    void TraceGb7c2Pixel(const GSDrawState &state, int x, int y, uint32_t z,
                         int su, int sv, int wu, int wv,
                         uint32_t texel, uint32_t rawTex, int clutIndex,
                         uint8_t combR, uint8_t combG, uint8_t combB, uint8_t combA,
                         uint32_t oldVal);
    // GB7C4 glyph-row carrier probe hooks (no-ops unless the probe is open).
    // BatchBegin assigns the intra-packet batch id and flags candidate
    // C1/carrier batches. The pixel tracers log one candidate pixel each
    // around its WritePixel call with an independent old read.
    uint64_t NoteGb7c4BatchBegin(const GSPrimitiveBatch &batch);
    void TraceGb7c4C1Pixel(const GSDrawState &state, int x, int y, uint32_t z,
                           float texUf, float texVf,
                           uint32_t texel,
                           uint8_t combR, uint8_t combG, uint8_t combB, uint8_t combA,
                           uint32_t oldVal, int candIdx);
    void TraceGb7c4CarrierPixel(const GSDrawState &state, int x, int y, uint32_t z,
                                float texUf, float texVf,
                                uint16_t sampleU, uint16_t sampleV,
                                uint32_t texel,
                                uint8_t combR, uint8_t combG, uint8_t combB, uint8_t combA,
                                uint8_t vrtR, uint8_t vrtG, uint8_t vrtB, uint8_t vrtA,
                                uint32_t oldVal, int candIdx);
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
