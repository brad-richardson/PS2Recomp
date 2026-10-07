#ifndef PS2_ADDRESS_H
#define PS2_ADDRESS_H

#include <cstdint>
#include <array>

#include "runtime/ps2_memory.h"

static inline constexpr uint32_t PS2_EE_UNCACHED_RAM_MIRROR_BASE = 0x20000000u;
static inline constexpr uint32_t PS2_EE_UNCACHED_RAM_MIRROR_SIZE = 0x20000000u;
static inline constexpr uint32_t PS2_KSEG0_BASE = 0x80000000u;
static inline constexpr uint32_t PS2_KSEG0_KSEG1_SIZE = 0x40000000u;
static inline constexpr uint32_t PS2_KSEG2_BASE = 0xC0000000u;

static inline constexpr bool Ps2AddressInRange(uint32_t value, uint32_t base, uint32_t size)
{
    return (value - base) < size;
}

static inline constexpr bool Ps2IsUncachedRamMirrorAddress(uint32_t addr)
{
    return Ps2AddressInRange(addr, PS2_EE_UNCACHED_RAM_MIRROR_BASE, PS2_EE_UNCACHED_RAM_MIRROR_SIZE);
}

static inline constexpr bool Ps2IsKseg01Address(uint32_t addr)
{
    return Ps2AddressInRange(addr, PS2_KSEG0_BASE, PS2_KSEG0_KSEG1_SIZE);
}

static inline constexpr bool Ps2IsKseg23Address(uint32_t addr)
{
    return addr >= PS2_KSEG2_BASE;
}

static inline constexpr uint32_t Ps2DirectMappedPhysicalAddress(uint32_t addr)
{
    return addr & 0x1FFFFFFFu;
}

static inline constexpr uint32_t Ps2PhysicalAddress(uint32_t addr)
{
    return (addr >= PS2_KSEG0_BASE) ? Ps2DirectMappedPhysicalAddress(addr) : addr;
}

struct Ps2SpecialRange
{
    uint32_t base;
    uint32_t size;
};

// Keep the four disjoint VU banks separate. Their sizes and bases are shared
// with PS2Memory's bank mapper, including its VU1 sync and code-write path.
static inline constexpr std::array<Ps2SpecialRange, 8> PS2_PHYSICAL_SPECIAL_RANGES{{
    {PS2_BIOS_BASE, PS2_BIOS_SIZE},
    {PS2_SCRATCHPAD_BASE, PS2_SCRATCHPAD_SIZE},
    {PS2_IO_BASE, PS2_IO_SIZE},
    {PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE},
    {PS2_VU0_CODE_BASE, PS2_VU0_CODE_SIZE},
    {PS2_VU0_DATA_BASE, PS2_VU0_DATA_SIZE},
    {PS2_VU1_CODE_BASE, PS2_VU1_CODE_SIZE},
    {PS2_VU1_DATA_BASE, PS2_VU1_DATA_SIZE},
}};

static inline constexpr bool Ps2IsPhysicalSpecialAddress(uint32_t physAddr, uint32_t width = 1u)
{
    if (width == 0u)
        return false;
    const uint64_t end = static_cast<uint64_t>(physAddr) + width;
    for (const Ps2SpecialRange range : PS2_PHYSICAL_SPECIAL_RANGES)
    {
        if (physAddr < static_cast<uint64_t>(range.base) + range.size && end > range.base)
            return true;
    }
    return false;
}

static inline constexpr bool Ps2IsSpecialAddress(uint32_t addr, uint32_t width = 1u)
{
    if (Ps2IsKseg23Address(addr))
        return true;

    return Ps2IsPhysicalSpecialAddress(Ps2PhysicalAddress(addr), width);
}

#endif // PS2_ADDRESS_H
