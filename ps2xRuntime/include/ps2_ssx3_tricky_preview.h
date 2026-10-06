#pragma once
// TK52: read the Select Event cursor, rather than the last accepted event.
// Addresses/strides: func_207430, func_39D860. All async ownership remains
// with cFEAsyncManager; this header never allocates a texture or a handle.
#include "ps2_ssx3_tricky_menu.h"

namespace ps2_ssx3_tricky_preview
{
inline constexpr uint32_t kMapInfo = 0x207430u;
inline constexpr uint32_t kFrameBytes = 0x400u;
inline constexpr uint32_t kFrameMagic = 0x544b3532u;
inline bool range(uint32_t a, uint32_t bytes, uint32_t size)
{
    return a && a < size && bytes <= size - a;
}
inline uint32_t widget(const uint8_t *ram, uint32_t size, uint32_t screen, const char *name)
{
    using namespace ps2_ssx3_tricky;
    if (!range(screen, 0x40u, size)) return 0;
    const uint32_t countPtr = rd32(ram, screen + 0x38u);
    const uint32_t objects = rd32(ram, screen + 0x3cu);
    if (!range(countPtr, 4, size)) return 0;
    const uint32_t count = rd32(ram, countPtr);
    if (count > 512 || !range(objects, count * 4, size)) return 0;
    const uint32_t hash = nameHash(name);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t obj = rd32(ram, objects + i * 4);
        if (range(obj, 0x96u, size) && rd32(ram, obj + 0x38u) == hash) return obj;
    }
    return 0;
}
inline int cursorEvent(const uint8_t *ram, uint32_t size, uint32_t controller)
{
    using namespace ps2_ssx3_tricky;
    if (size < 0x4a25a8u || !range(controller, 0x2f0u, size) || rd32(ram, 0x4a259cu) != 2) return -1;
    const uint32_t peak = rd32(ram, 0x4a25a4u), type = rd32(ram, 0x4a25a0u);
    if (peak > 2 || type > 2) return -1;
    const char *types[] = {"Race", "Freestyle", "Freeride"};
    char name[48];
    std::snprintf(name, sizeof(name), "Peak%u%sLocations", peak + 1, types[type]);
    const uint32_t obj = widget(ram, size, rd32(ram, controller + 0x2e8u), name);
    if (!obj) return -1;
    const uint32_t row = ram[obj + 0x95u];
    const uint32_t counts[] = {4, 5, 8};
    const uint32_t bases[] = {0x4781d0u, 0x4786e0u, 0x478d38u};
    if (row >= counts[type]) return -1;
    const uint32_t event = rd32(ram, bases[type] + peak * counts[type] * 0x6cu + row * 0x6cu);
    return event < ps2_ssx3_course::kRows ? static_cast<int>(event) : -1;
}
inline bool course(const uint8_t *ram, uint32_t size, int event)
{
    // Only these TKP4 substitutions; Megaplex stays outside TK52.
    const struct { int event; const char *archive; } rows[] = {
        {0,"GARI"}, {1,"SNOW"}, {2,"MESA"}, {4,"MERQ"}, {6,"ALASKA"},
        {7,"UNTRACK"}, {11,"PIPE"}, {14,"ALOHA"}, {16,"ELYS"}};
    for (const auto &row : rows)
        if (event == row.event)
        {
            const uint32_t a = ps2_ssx3_course::kEventBase + event * ps2_ssx3_course::kEventStride + 68;
            return range(a, 16, size) && std::strcmp(reinterpret_cast<const char *>(ram + a), row.archive) == 0;
        }
    return false;
}
} // namespace ps2_ssx3_tricky_preview
