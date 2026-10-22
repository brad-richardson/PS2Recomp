// SPDX-License-Identifier: GPL-3.0-or-later OR MIT
#pragma once
// Guest-affecting, Tricky only. Preserve this user's complete SET0001
// source fields before a race; never infer clean bytes from a sample save.
// SLUS_207.72 serializer 0x152758 copies these two blocks, packs six
// profile flags and computes its own CRC. Deserializer 0x152bb0 consumes
// only the low nine bits of each packed flag word (remaining bits are padding).
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace ps2_ssx3_tricky_records
{
inline bool exactPath()
{
    static const bool exact = [] {
        const char *v = std::getenv("PS2X_SSX3_TRICKY_RECORDS");
        return v && (std::strcmp(v, "exact") == 0 || std::strcmp(v, "off") == 0);
    }();
    return exact;
}
inline constexpr size_t kSettings = 0x535898, kSettingsSize = 0x288;
inline constexpr size_t kRecords = 0x535c18, kRecordsSize = 0xa28;
inline constexpr size_t kProfiles = 0x535b20, kProfileStride = 28;
struct State
{
    bool have = false, wasInGame = false, menusClean = false;
    std::array<uint8_t, kSettingsSize> settings{};
    std::array<uint8_t, kRecordsSize> records{};
    std::array<uint16_t, 6> flags{};
};
inline State &state() { static State s; return s; }
inline bool validRam(const uint8_t *ram, size_t size)
{
    return ram && size >= kRecords + kRecordsSize;
}
inline void capture(State &s, const uint8_t *ram)
{
    std::memcpy(s.settings.data(), ram + kSettings, s.settings.size());
    std::memcpy(s.records.data(), ram + kRecords, s.records.size());
    for (size_t i = 0; i < s.flags.size(); ++i)
    {
        const auto *p = ram + kProfiles + i * kProfileStride;
        s.flags[i] = ((p[16] >> 1) & 1u) | (uint16_t(p[19]) << 1);
    }
    s.have = true; s.menusClean = false;
}
inline bool restore(const State &s, uint8_t *ram, size_t size)
{
    if (!s.have || s.menusClean || !validRam(ram, size)) return false;
    std::memcpy(ram + kSettings, s.settings.data(), s.settings.size());
    std::memcpy(ram + kRecords, s.records.data(), s.records.size());
    for (size_t i = 0; i < s.flags.size(); ++i)
    {
        auto *p = ram + kProfiles + i * kProfileStride;
        p[16] = (p[16] & ~2u) | ((s.flags[i] & 1u) << 1);
        p[19] = uint8_t(s.flags[i] >> 1);
    }
    return true;
}
// This lifetime deliberately has no dependency on activeEvent. Once the
// guest returns to menus, clean RAM once so later legitimate menu edits
// and a subsequent stock race cannot be overwritten by an old snapshot.
inline void tick(State &s, uint8_t *ram, size_t size, bool inGame, bool tricky, bool exact)
{
    if (exact || !validRam(ram, size)) return;
    if (inGame && !s.wasInGame)
    {
        restore(s, ram, size);
        if (tricky) capture(s, ram);
        else s = {};
    }
    else if (!inGame && s.wasInGame && s.have)
    {
        restore(s, ram, size);
        s.menusClean = true;
    }
    s.wasInGame = inGame;
}
} // namespace ps2_ssx3_tricky_records
