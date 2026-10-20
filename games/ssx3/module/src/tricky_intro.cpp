// astra-flyover-1: use the prepared start pose during Tricky's donor intro.
#include "Ssx3TrickyIntro.h"
#include "ps2_runtime.h"
#include "ps2_ssx3_course_manifest.h"
#include "runtime/ps2_memory.h"

#include <cstdlib>
#include <cstring>

namespace
{
constexpr uint32_t kScriptPose = 0x00169828u;
constexpr uint32_t kCopyPose = 0x001673a0u;
PS2Runtime::RecompiledFunction g_scriptPose = nullptr;
PS2Runtime::RecompiledFunction g_copyPose = nullptr;

bool inRam(uint32_t address, uint32_t size)
{
    return address >= 0x1000u && !(address & 3u) && address <= PS2_RAM_SIZE - size;
}

uint32_t read32(const uint8_t *ram, uint32_t address)
{
    uint32_t value = 0;
    if (inRam(address, 4u))
        std::memcpy(&value, ram + address, sizeof(value));
    return value;
}

uint32_t member(const uint8_t *ram, uint32_t object, uint32_t offset)
{
    return inRam(object, offset + 4u) ? read32(ram, object + offset) : 0u;
}

void fixedIntroPose(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const auto &modes = ps2_ssx3_course::courseModes();
    const uint32_t caller = getRegU32(ctx, 31);
    // Only camera composition calls: singleton node or a blended node list.
    if (ram && ctx->pc == kScriptPose && modes.armed &&
        ps2_ssx3_course::modeCurrent(modes, ram) != 0u &&
        (caller == 0x0015e72cu || caller == 0x0015e7e0u))
    {
        const uint32_t root = read32(ram, 0x004a28a8u);
        const uint32_t race = member(ram, root, 0x84u);
        const uint32_t manager = member(ram, race, 0x84u);
        const uint32_t camera = member(ram, manager, 4u);
        const uint32_t script = member(ram, camera, 0xacu);
        const uint32_t chase = member(ram, camera, 0xa8u);
        const uint32_t list = member(ram, chase, 8u);
        const uint32_t link = member(ram, list, 0u);
        const uint32_t node = member(ram, link, 0u);
        const uint32_t vtable = member(ram, node, 0x10u);
        if (member(ram, race, 0x214u) == 1u && camera != 0u &&
            getRegU32(ctx, 17) == camera && script != 0u &&
            member(ram, camera, 0xa0u) == script &&
            member(ram, list, 4u) == 1u &&
            member(ram, vtable, 0x14u) == kCopyPose &&
            inRam(node, 0x80u) && inRam(getRegU32(ctx, 4), 0x20u))
        {
            // The Chase node was positioned at the course start by the guest.
            // Its controller remains inactive, so this pose stays fixed. Leave
            // Script updates, intro state, input and audio alone. Tail-call the
            // guest's leaf pose copier; no pointer swap or host-side restore
            // can be stranded by a guest yield or savestate load.
            ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(node)));
            ctx->pc = kCopyPose;
            g_copyPose(ram, ctx, runtime);
            return;
        }
    }
    g_scriptPose(ram, ctx, runtime);
}
} // namespace

namespace ps2_ssx3_tricky_intro
{
void applyTrickyFixedIntro(PS2Runtime &runtime)
{
    const char *setting = std::getenv("PS2X_SSX3_TRICKY_FIXED_INTRO");
    if (setting && std::strcmp(setting, "0") == 0)
        return;
    const uint8_t *ram = runtime.memory().getRDRAM();
    if (!ram || read32(ram, kScriptPose) != 0x27bdfeb0u ||
        read32(ram, kScriptPose + 4u) != 0x3c020050u ||
        read32(ram, kCopyPose) != 0x78a60060u ||
        read32(ram, kCopyPose + 4u) != 0x0080102du)
        return;
    g_scriptPose = runtime.lookupFunction(kScriptPose);
    g_copyPose = runtime.lookupFunction(kCopyPose);
    if (g_scriptPose && g_copyPose)
        runtime.replaceFunction(kScriptPose, &fixedIntroPose);
}
} // namespace ps2_ssx3_tricky_intro
