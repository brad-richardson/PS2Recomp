// HL2: the guest PCs whose 32-bit loads ps2_ts2_split60::halfLoad converts.
//
// Single source shared by the runtime switch (ps2_ts2_split60.h) and the
// generator (ps2xRecomp instruction_translator.cpp), which emits the
// READ32_SPLIT hook form only at these PCs and the plain READ32 elsewhere.
// All twelve are `lwc1 $fX, off($gp)` sites (ee-at, HL2 REPORT §2); the four
// 0x1139xx sites additionally gate on the td->helper record, which stays a
// runtime check inside halfLoad.
#pragma once

#include <cstdint>

namespace ps2_ts2_splitsites
{
// Rider-pass H constants (sub_00113648).
inline constexpr uint32_t kSite113808 = 0x113808u;
inline constexpr uint32_t kSite113860 = 0x113860u;
inline constexpr uint32_t kSite113888 = 0x113888u;
// Shared 1139a0 helper: stock values while td->helper is set.
inline constexpr uint32_t kSite1139a4 = 0x1139a4u;
inline constexpr uint32_t kSite1139c4 = 0x1139c4u;
inline constexpr uint32_t kSite1139dc = 0x1139dcu;
inline constexpr uint32_t kSite113a0c = 0x113a0cu;
// Shared physics helpers.
inline constexpr uint32_t kSite121e64 = 0x121e64u;
inline constexpr uint32_t kSite137d68 = 0x137d68u;
inline constexpr uint32_t kSite139a48 = 0x139a48u;
inline constexpr uint32_t kSite13d8e4 = 0x13d8e4u;
inline constexpr uint32_t kSite13ee80 = 0x13ee80u;

inline constexpr uint32_t kSites[] = {
    kSite113808, kSite113860, kSite113888,
    kSite1139a4, kSite1139c4, kSite1139dc, kSite113a0c,
    kSite121e64, kSite137d68, kSite139a48, kSite13d8e4, kSite13ee80,
};

constexpr bool isSite(uint32_t pc) noexcept
{
    for (uint32_t s : kSites)
        if (s == pc) return true;
    return false;
}
} // namespace ps2_ts2_splitsites
