// tricky-records-1 v3 product guard implementation. See
// Ssx3TrickyRecordsPatch.h and ps2_ssx3_tricky_records.h.
#include "Ssx3TrickyRecordsPatch.h"

#include "ps2_ssx3_tricky_list.h"
#include "ps2_ssx3_tricky_records.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"

#include <cstdio>
#include <cstring>

namespace
{
// McWrite card-write entry (SLUS_207.72): addiu sp,sp,-0x80; sd s4,0x50(sp).
constexpr uint32_t kMcWritePc = 0x0040a1a8u;
constexpr uint32_t kMcWriteW0 = 0x27bdff80u;
constexpr uint32_t kMcWriteW1 = 0xffb40050u;

PS2Runtime::RecompiledFunction g_mcWriteOrig = nullptr;

uint32_t rd32(const uint8_t *ram, uint32_t va)
{
    uint32_t v = 0;
    std::memcpy(&v, ram + va, 4);
    return v;
}

void patchMcWriteWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static unsigned hits = 0;
    ++hits;
    const uint32_t fd = getRegU32(ctx, 4);
    const uint32_t src = getRegU32(ctx, 5);
    const uint32_t size = getRegU32(ctx, 6);
    const int mapped = ps2_ssx3_tricky_list::mapState().activeEvent;
    if (hits <= 8)
    {
        const unsigned long long tick =
            static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed));
        std::fprintf(stderr,
                     "[ssx3-tricky-records] tick=%llu mcwrite hit=%u fd=%u src=0x%x size=%u mapped=%d\n",
                     tick, hits, fd, src, size, mapped);
    }
    if (rdram &&
        ps2_ssx3_tricky_records::shouldPatch(mapped >= 0, ps2_ssx3_tricky_records::exactPath(),
                                             static_cast<size_t>(size)) &&
        src < PS2_RAM_SIZE &&
        ps2_ssx3_tricky_records::kImageSize <= PS2_RAM_SIZE - src)
    {
        ps2_ssx3_tricky_records::patchImage(rdram + src, ps2_ssx3_tricky_records::kImageSize);
        std::fprintf(stderr, "[ssx3-tricky-records] patched SET0001 clean (mapped save)\n");
    }
    g_mcWriteOrig(rdram, ctx, runtime);
}
} // namespace

namespace ps2_ssx3_tricky_records_patch
{

void applyTrickyRecordsPatch(PS2Runtime &runtime)
{
    const uint8_t *ram = runtime.memory().getRDRAM();
    if (!ram)
    {
        std::fprintf(stderr, "[ssx3-tricky-records] refused: no RAM\n");
        return;
    }
    if (rd32(ram, kMcWritePc) != kMcWriteW0 || rd32(ram, kMcWritePc + 4u) != kMcWriteW1)
    {
        std::fprintf(stderr, "[ssx3-tricky-records] refused: 0x%x entry mismatch; nothing wrapped\n",
                     kMcWritePc);
        return;
    }
    if (!(g_mcWriteOrig = runtime.lookupFunction(kMcWritePc)) ||
        !runtime.replaceFunction(kMcWritePc, &patchMcWriteWrap))
    {
        std::fprintf(stderr, "[ssx3-tricky-records] cannot wrap 0x%x\n", kMcWritePc);
        std::abort();
    }
    std::fprintf(stderr, "[ssx3-tricky-records] armed: McWrite guard at 0x%x\n", kMcWritePc);
}

} // namespace ps2_ssx3_tricky_records_patch
