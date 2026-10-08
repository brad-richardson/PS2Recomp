#pragma once
// VUP1: specialized VIF UNPACK decoders for the heavy-window shapes (GTR1 rank 2).
//
// Exact: byte-identical VU stores (inline loop) and VifRec bytes (staged loop)
// to the generic per-vector loop. Behind PS2X_VIF_UNPACK_FAST=1 (default off).
// Used by both UNPACK loops in ps2_vif1_interpreter.cpp; unit-tested directly
// (ps2_vif_unpack_tests.cpp) and differentially (knob off vs on).
//
// Fast-path contract per UNPACK command (see eligible()): wl == 1 (cyclePos is
// always 0, so only mask row 0 applies and every write has source), mode == 0
// (no row add/accumulate), a covered (vl, components) shape, and -- when masked
// -- a row 0 with only data(0)/row(1) lane specs (no col/protect). Every lane
// of every vector is then written unconditionally, so no read-modify-write.
#include <atomic>
#include <cstdint>
#include <cstring>

namespace ps2_vif_unpack_fast
{

// Covered (vl, components) pairs: every non-bulk shape in the heavy window
// (masked V3-16/V2-16/V3-32/V2-32, V4-5, V4-16, V2-8, unmasked V4-32 with
// row lanes). S splats, V3-8 and the unhandled vl==3/vn!=3 shapes stay generic.
inline bool shapeCovered(uint32_t vl, uint32_t components)
{
    if (components == 2u)
        return vl <= 2u;
    if (components == 3u)
        return vl <= 1u;
    if (components == 4u)
        return vl <= 3u;
    return false;
}

// True when mask row 0 (the only row wl == 1 ever reads) uses only data(0)
// and row-fill(1) lane specs. Bit 1 of any lane pair means col(2)/protect(3).
inline bool row0DataOrRow(uint32_t mask)
{
    return ((mask & 0xFFu) & 0xAAu) == 0u;
}

// cl is post-fixup (>= 1u) at both call sites, so wl == 1u implies cl >= wl
// (source available every cycle, sourceVectorCount == writeVectorCount).
inline bool eligible(uint32_t vl, uint32_t components, bool maskEnable, uint32_t mode, uint32_t mask,
                     uint32_t wl)
{
    return wl == 1u && mode == 0u && shapeCovered(vl, components) &&
           (!maskEnable || row0DataOrRow(mask));
}

inline uint32_t extend16(uint16_t raw, bool zeroExtend)
{
    if (zeroExtend)
        return static_cast<uint32_t>(raw);
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
}

inline uint32_t extend8(uint8_t raw, bool zeroExtend)
{
    if (zeroExtend)
        return static_cast<uint32_t>(raw);
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
}

// V3 w-from-next-source-vector rule, mirroring the generic loop exactly: w
// comes from the next source vector's first element, 0 when that read crosses
// a source QW boundary or runs past the buffer end. mySrc/srcBase/dataStartPos
// (the UNPACK payload start) /sizeBytes match the generic loop's values.
inline uint32_t v3WFromNext(const uint8_t *srcBase, uint32_t mySrc, uint32_t bytesPerVector,
                            uint32_t vl, bool zeroExtend, uint32_t dataStartPos, uint32_t sizeBytes)
{
    const uint32_t readLen = (vl == 0u) ? 4u : ((vl == 1u) ? 2u : 1u);
    const uint64_t readOff =
        static_cast<uint64_t>(mySrc + 1u) * static_cast<uint64_t>(bytesPerVector);
    const uint64_t avail = static_cast<uint64_t>(sizeBytes - dataStartPos);
    const uint32_t phase = static_cast<uint32_t>((static_cast<uint64_t>(dataStartPos) + readOff) & 15u);
    uint32_t w = 0u;
    if (phase + readLen <= 16u && readOff + readLen <= avail)
    {
        const uint8_t *next = srcBase + readOff;
        if (vl == 0u)
        {
            std::memcpy(&w, next, sizeof(w));
        }
        else if (vl == 1u)
        {
            uint16_t raw = 0u;
            std::memcpy(&raw, next, sizeof(raw));
            w = extend16(raw, zeroExtend);
        }
        else
        {
            w = extend8(next[0], zeroExtend);
        }
    }
    return w;
}

// Decode one source vector for a covered shape, mirroring the generic
// vl/components branches plus the UV1 V2/V3 lane rules exactly. Call only when
// eligible() holds (decoded && handledFormat are then always true).
inline void decodeVector(uint32_t vl, uint32_t components, const uint8_t *srcVec, bool zeroExtend,
                         bool qwAligned, const uint8_t *srcBase, uint32_t mySrc,
                         uint32_t bytesPerVector, uint32_t dataStartPos, uint32_t sizeBytes,
                         uint32_t (&out)[4])
{
    if (vl == 0u)
    {
        for (uint32_t c = 0u; c < components; ++c)
        {
            uint32_t scalar = 0u;
            std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
            out[c] = scalar;
        }
    }
    else if (vl == 1u)
    {
        for (uint32_t c = 0u; c < components; ++c)
        {
            uint16_t raw = 0u;
            std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
            out[c] = extend16(raw, zeroExtend);
        }
    }
    else if (vl == 2u)
    {
        for (uint32_t c = 0u; c < components; ++c)
            out[c] = extend8(srcVec[c], zeroExtend);
    }
    else
    {
        // vl == 3u with components == 4u is V4-5 (components == vn + 1):
        // RGBA5551 in one 16-bit value, expanded as in PCSX2's UNPACK_V4_5.
        uint16_t packed = 0u;
        std::memcpy(&packed, srcVec, sizeof(packed));
        out[0] = (packed & 0x1Fu) << 3;
        out[1] = ((packed >> 5) & 0x1Fu) << 3;
        out[2] = ((packed >> 10) & 0x1Fu) << 3;
        out[3] = ((packed >> 15) & 0x01u) << 7;
    }

    if (components == 2u)
    {
        out[2] = out[0];
        out[3] = (vl == 0u && qwAligned) ? 0u : out[1];
    }
    else if (components == 3u)
    {
        out[3] =
            v3WFromNext(srcBase, mySrc, bytesPerVector, vl, zeroExtend, dataStartPos, sizeBytes);
    }
}

// Fast-path engagement counter (test hook): incremented once per UNPACK
// command the fast path handles, on either loop. Zero cost with the knob off
// (the increment sits inside the fast block).
inline std::atomic<uint64_t> &hitsCounter()
{
    static std::atomic<uint64_t> hits{0u};
    return hits;
}

inline void noteFastUnpack()
{
    hitsCounter().fetch_add(1u, std::memory_order_relaxed);
}

inline uint64_t fastUnpackHits()
{
    return hitsCounter().load(std::memory_order_relaxed);
}

inline void resetFastUnpackHits()
{
    hitsCounter().store(0u, std::memory_order_relaxed);
}

} // namespace ps2_vif_unpack_fast
