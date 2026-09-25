#ifndef PS2_VU1_FMAC_IMPL_H
#define PS2_VU1_FMAC_IMPL_H

// VR1: the FMAC result helpers (dest writes, exact-result normalization, product
// sticky, flag queueing), moved verbatim from ps2_vu1_core.cpp and always
// inlined, so a generated pair's constant upper word (m_currentUpperInstruction,
// stored by execUpperImpl) and dest mask fold the op decode and lane loops.

#include "runtime/ps2_vu1.h"
#include "ps2_vu1_detail.h"
#include "ps2_vu1_step_impl.h"

#include <cmath>
#include <cstring>
#include <limits>

PS2X_VU1_ALWAYS_INLINE inline float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    return normalizeOperand(vf[bc & 3u]);
}

PS2X_VU1_ALWAYS_INLINE inline void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    if (dest & 0x8u)
        dst[0] = result[0];
    if (dest & 0x4u)
        dst[1] = result[1];
    if (dest & 0x2u)
        dst[2] = result[2];
    if (dest & 0x1u)
        dst[3] = result[3];
}

PS2X_VU1_ALWAYS_INLINE inline void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

PS2X_VU1_ALWAYS_INLINE inline void VU1Interpreter::normalizeFmacResult(float *result, uint8_t dest,
                                         uint8_t laneFlags[4])
{
    // E57: the exact results for all dest lanes come from one decode of the
    // upper op (calculateFmacExactResults); whether an op has an exact form
    // does not depend on the lane.
    VuWide exactResults[4] = {0.0, 0.0, 0.0, 0.0};
    const bool haveExact = calculateFmacExactResults(dest, exactResults);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;

        if (haveExact)
        {
            laneFlags[component] = normalizeFmacExactResult(result[component], exactResults[component]);
            continue;
        }

        uint32_t flags = 0u;
        result[component] = normalizeResult(result[component], flags);
        laneFlags[component] = static_cast<uint8_t>(flags);
    }
}

PS2X_VU1_ALWAYS_INLINE inline bool VU1Interpreter::calculateFmacExactResults(uint8_t dest, VuWide results[4]) const
{
    // E57: same per-lane expressions as calculateFmacExactResult(), with the
    // op decode hoisted out of the lane loop. Only dest lanes are computed.
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const bool isSpecial = op >= 0x3Cu;
    const uint8_t code = isSpecial
                             ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                             : op;
    const float *fsRow = m_state.vf[FS(upper)];
    const float *ftRow = m_state.vf[FT(upper)];
    const auto operand = [](float value)
    {
        return static_cast<VuWide>(normalizeOperand(value));
    };
    const VuWide q = operand(m_state.q);
    const VuWide i = operand(m_state.i);

    VuWide vs[4];
    VuWide vt[4];
    VuWide acc[4];
    for (uint32_t lane = 0; lane < 4u; ++lane)
    {
        vs[lane] = operand(fsRow[lane]);
        vt[lane] = operand(ftRow[lane]);
        acc[lane] = operand(m_state.acc[lane]);
    }

    uint8_t lanes[4];
    uint32_t laneCount = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) != 0u)
            lanes[laneCount++] = static_cast<uint8_t>(component);
    }
#define E57_FOR_LANES(expr)                              \
    for (uint32_t n = 0; n < laneCount; ++n)             \
    {                                                    \
        const uint32_t c = lanes[n];                     \
        results[c] = (expr);                             \
    }

    if (code <= 0x0Fu || (code >= 0x18u && code <= 0x1Bu))
    {
        const VuWide bc = vt[code & 3u];
        if (code <= 0x03u)
            E57_FOR_LANES(vs[c] + bc)
        else if (code <= 0x07u)
            E57_FOR_LANES(vs[c] - bc)
        else if (code <= 0x0Bu)
            E57_FOR_LANES(acc[c] + vs[c] * bc)
        else if (code <= 0x0Fu)
            E57_FOR_LANES(acc[c] - vs[c] * bc)
        else
            E57_FOR_LANES(vs[c] * bc)
        return true;
    }

    static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
    static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
    switch (code)
    {
    case 0x1Cu: E57_FOR_LANES(vs[c] * q) break;
    case 0x1Eu: E57_FOR_LANES(vs[c] * i) break;
    case 0x20u: E57_FOR_LANES(vs[c] + q) break;
    case 0x21u: E57_FOR_LANES(acc[c] + vs[c] * q) break;
    case 0x22u: E57_FOR_LANES(vs[c] + i) break;
    case 0x23u: E57_FOR_LANES(acc[c] + vs[c] * i) break;
    case 0x24u: E57_FOR_LANES(vs[c] - q) break;
    case 0x25u: E57_FOR_LANES(acc[c] - vs[c] * q) break;
    case 0x26u: E57_FOR_LANES(vs[c] - i) break;
    case 0x27u: E57_FOR_LANES(acc[c] - vs[c] * i) break;
    case 0x28u: E57_FOR_LANES(vs[c] + vt[c]) break;
    case 0x29u: E57_FOR_LANES(acc[c] + vs[c] * vt[c]) break;
    case 0x2Au: E57_FOR_LANES(vs[c] * vt[c]) break;
    case 0x2Cu: E57_FOR_LANES(vs[c] - vt[c]) break;
    case 0x2Du: E57_FOR_LANES(acc[c] - vs[c] * vt[c]) break;
    case 0x2Eu:
        if (isSpecial)
            E57_FOR_LANES(c == 3u ? 0.0 : vs[left[c]] * vt[right[c]])
        else
            E57_FOR_LANES(c == 3u ? 0.0 : acc[c] - vs[left[c]] * vt[right[c]])
        break;
    default:
        return false;
    }
#undef E57_FOR_LANES
    return true;
}

PS2X_VU1_ALWAYS_INLINE inline uint8_t VU1Interpreter::normalizeFmacExactResult(float &value,
                                                  VuWide exactResult) const
{
    const bool negative = std::signbit(exactResult);
    const VuWide magnitude = std::fabs(exactResult);
    const VuWide maximum = static_cast<VuWide>(std::numeric_limits<float>::max());
    const VuWide minimum = static_cast<VuWide>(std::numeric_limits<float>::min());
    uint8_t flags = negative ? 0x2u : 0u;

    uint32_t bits = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0)
    {
        flags |= 0x1u;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude > maximum)
    {
        flags |= 0x8u;
        bits |= 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude < minimum)
    {
        flags |= 0x5u;
        std::memcpy(&value, &bits, sizeof(value));
    }

    return flags;
}

PS2X_VU1_ALWAYS_INLINE inline uint32_t VU1Interpreter::calculateFmacProductSticky(uint8_t dest) const
{
    uint32_t extraSticky = 0u;
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : 0xFFu;
    const bool productSum =
        (op >= 0x08u && op <= 0x0Fu) ||
        op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
        op == 0x29u || op == 0x2Du || op == 0x2Eu ||
        (special >= 0x08u && special <= 0x0Fu) ||
        special == 0x21u || special == 0x23u || special == 0x25u ||
        special == 0x27u || special == 0x29u || special == 0x2Du;
    if (!productSum)
        return 0u;

    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        const uint8_t leftComponent = op == 0x2Eu ? crossLeft[component] : static_cast<uint8_t>(component);
        const float left = normalizeOperand(m_state.vf[fs][leftComponent]);
        float right = 0.0f;
        if ((op >= 0x08u && op <= 0x0Fu) || (special >= 0x08u && special <= 0x0Fu))
        {
            right = normalizeOperand(m_state.vf[ft][(op >= 0x08u && op <= 0x0Fu ? op : special) & 3u]);
        }
        else if (op == 0x21u || op == 0x25u || special == 0x21u || special == 0x25u)
        {
            right = normalizeOperand(m_state.q);
        }
        else if (op == 0x23u || op == 0x27u || special == 0x23u || special == 0x27u)
        {
            right = normalizeOperand(m_state.i);
        }
        else if (op == 0x2Eu)
        {
            right = normalizeOperand(m_state.vf[ft][crossRight[component]]);
        }
        else
        {
            right = normalizeOperand(m_state.vf[ft][component]);
        }

        float product = left * right;
        const VuWide exactProduct = static_cast<VuWide>(left) * static_cast<VuWide>(right);
        const uint8_t productFlags = normalizeFmacExactResult(product, exactProduct);
        // Product-sum instructions report Z/S/U/O from the add/subtract result
        // as current flags, while every product condition accumulates into the
        // corresponding sticky flag.
        extraSticky |= productFlags & 0xFu;
    }
    return extraSticky;
}

PS2X_VU1_ALWAYS_INLINE inline void VU1Interpreter::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
                                     uint32_t extraSticky)
{
    if (dest == 0u)
        return;

    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t lane = laneForComponent(component);
        if ((dest & lane) == 0u)
            continue;

        const uint32_t flags = laneFlags[component];
        if ((flags & 0x1u) != 0u)
            mac |= lane;
        if ((flags & 0x2u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 4;
        if ((flags & 0x4u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 8;
        if ((flags & 0x8u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 12;
        status |= flags;
    }

    // VB1: the flag commit at issue (see issuePair's m_directFlags guard).
    if (m_directFlags)
    {
        m_state.mac = mac;
        const uint32_t current = status & 0xFu;
        m_state.status = (m_state.status & 0xFF0u) | current | ((current | extraSticky) << 6);
        noteDirect(m_cycle + kFmacLatency);
        return;
    }

    const int slot = firstFreeEntry(m_flagValidMask, kMaxFlagEntries);
    if (slot < 0)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }

    FlagPipelineEntry *entry = &m_flagPipeline[slot];
    *entry = {};
    entry->valid = true;
    entry->issueCycle = m_cycle;
    entry->readyCycle = m_cycle + kFmacLatency;
    m_flagValidMask |= 1u << slot;
    noteQueued(entry->readyCycle);
    entry->mac = mac;
    entry->status = status;
    entry->extraSticky = extraSticky;
    entry->writesMac = true;
    entry->writesStatus = true;
}

PS2X_VU1_ALWAYS_INLINE inline void VU1Interpreter::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDest(dst, result, dest);
}

PS2X_VU1_ALWAYS_INLINE inline void VU1Interpreter::applyFmacDestAcc(float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDestAcc(result, dest);
}

#endif
