#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_gfx_stats.h"
#include "ps2_vu1_detail.h"
#include "ps2_vu1_trace.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <ps2_log.h>

namespace
{
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }
}

void VU1Interpreter::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (lanes == 0u)
        return;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
        {
            usage.vfRead[index].lanes |= lanes;
            return;
        }
    }
    if (usage.vfReadCount < usage.vfRead.size())
        usage.vfRead[usage.vfReadCount++] = {reg, lanes};
}

void VU1Interpreter::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

uint8_t VU1Interpreter::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

VU1Interpreter::VU1Interpreter(Unit unit)
    : m_unit(unit)
{
    reset();
}

void VU1Interpreter::resetScheduler()
{
    m_flagPipeline = {};
    m_fdiv = {};
    m_efu = {};
    m_storePipeline = {};
    m_vfWritePipeline = {};
    m_viWritePipeline = {};
    m_accWritePipeline = {};
    m_xgkick = {};
    m_vfReady = {};
    m_viReady = {};
    m_accReady = {};
    m_vfLatestWrite = {};
    m_viLatestWrite = {};
    m_accLatestWrite = {};
    m_nextWriteSequence = 0;
    m_efuResourceReady = 0;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    resetScheduler();
}

float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    return normalizeOperand(vf[bc & 3u]);
}

float VU1Interpreter::normalizeOperand(float value) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u)
    {
        bits &= 0x80000000u;
    }
    else if (exponent == 0xFFu)
    {
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float VU1Interpreter::normalizeResult(float value, uint32_t &laneFlags) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;

    laneFlags = sign != 0u ? 0x2u : 0u;
    if (magnitude == 0u)
    {
        laneFlags |= 0x1u;
    }
    else if (exponent == 0u)
    {
        laneFlags |= 0x5u;
        bits = sign;
    }
    else if (exponent == 0xFFu)
    {
        laneFlags |= 0x8u;
        bits = sign | 0x7F7FFFFFu;
    }

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t VU1Interpreter::microAddressMask() const
{
    return m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu;
}

int32_t VU1Interpreter::readBranchVi(uint8_t reg) const
{
    if (reg == 0u)
        return 0;
    if (m_viBranchBackupValid &&
        m_viBranchBackupReg == reg)
    {
        return m_viBranchBackupValue;
    }
    return m_state.vi[reg];
}

void VU1Interpreter::recordViWriteForBranch(uint8_t reg, int32_t oldValue)
{
    if (reg == 0u)
        return;
    m_viBranchBackupValue = oldValue;
    m_viBranchBackupReg = reg;
    m_viBranchBackupValid = true;
}

void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
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

void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

void VU1Interpreter::normalizeFmacResult(float *result, uint8_t dest,
                                         uint8_t laneFlags[4])
{
    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;

        long double exactResult = 0.0L;
        if (calculateFmacExactResult(component, exactResult))
        {
            laneFlags[component] = normalizeFmacExactResult(result[component], exactResult);
            continue;
        }

        uint32_t flags = 0u;
        result[component] = normalizeResult(result[component], flags);
        laneFlags[component] = static_cast<uint8_t>(flags);
    }
}

bool VU1Interpreter::calculateFmacExactResult(uint32_t component,
                                               long double &result) const
{
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu
                                ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                                : 0xFFu;
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);

    const auto operand = [this](float value)
    {
        return static_cast<long double>(normalizeOperand(value));
    };
    const auto vs = [&](uint32_t lane)
    {
        return operand(m_state.vf[fs][lane]);
    };
    const auto vt = [&](uint32_t lane)
    {
        return operand(m_state.vf[ft][lane]);
    };
    const auto acc = [&](uint32_t lane)
    {
        return operand(m_state.acc[lane]);
    };

    const long double q = operand(m_state.q);
    const long double i = operand(m_state.i);

    if (op < 0x3Cu)
    {
        if (op <= 0x03u)
            result = vs(component) + vt(op & 3u);
        else if (op <= 0x07u)
            result = vs(component) - vt(op & 3u);
        else if (op <= 0x0Bu)
            result = acc(component) + vs(component) * vt(op & 3u);
        else if (op <= 0x0Fu)
            result = acc(component) - vs(component) * vt(op & 3u);
        else if (op >= 0x18u && op <= 0x1Bu)
            result = vs(component) * vt(op & 3u);
        else
        {
            switch (op)
            {
            case 0x1Cu:
                result = vs(component) * q;
                break;
            case 0x1Eu:
                result = vs(component) * i;
                break;
            case 0x20u:
                result = vs(component) + q;
                break;
            case 0x21u:
                result = acc(component) + vs(component) * q;
                break;
            case 0x22u:
                result = vs(component) + i;
                break;
            case 0x23u:
                result = acc(component) + vs(component) * i;
                break;
            case 0x24u:
                result = vs(component) - q;
                break;
            case 0x25u:
                result = acc(component) - vs(component) * q;
                break;
            case 0x26u:
                result = vs(component) - i;
                break;
            case 0x27u:
                result = acc(component) - vs(component) * i;
                break;
            case 0x28u:
                result = vs(component) + vt(component);
                break;
            case 0x29u:
                result = acc(component) + vs(component) * vt(component);
                break;
            case 0x2Au:
                result = vs(component) * vt(component);
                break;
            case 0x2Cu:
                result = vs(component) - vt(component);
                break;
            case 0x2Du:
                result = acc(component) - vs(component) * vt(component);
                break;
            case 0x2Eu:
            {
                static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
                static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
                result = component == 3u
                             ? 0.0L
                             : acc(component) - vs(left[component]) * vt(right[component]);
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

    if (special <= 0x03u)
        result = vs(component) + vt(special & 3u);
    else if (special <= 0x07u)
        result = vs(component) - vt(special & 3u);
    else if (special <= 0x0Bu)
        result = acc(component) + vs(component) * vt(special & 3u);
    else if (special <= 0x0Fu)
        result = acc(component) - vs(component) * vt(special & 3u);
    else if (special >= 0x18u && special <= 0x1Bu)
        result = vs(component) * vt(special & 3u);
    else
    {
        switch (special)
        {
        case 0x1Cu:
            result = vs(component) * q;
            break;
        case 0x1Eu:
            result = vs(component) * i;
            break;
        case 0x20u:
            result = vs(component) + q;
            break;
        case 0x21u:
            result = acc(component) + vs(component) * q;
            break;
        case 0x22u:
            result = vs(component) + i;
            break;
        case 0x23u:
            result = acc(component) + vs(component) * i;
            break;
        case 0x24u:
            result = vs(component) - q;
            break;
        case 0x25u:
            result = acc(component) - vs(component) * q;
            break;
        case 0x26u:
            result = vs(component) - i;
            break;
        case 0x27u:
            result = acc(component) - vs(component) * i;
            break;
        case 0x28u:
            result = vs(component) + vt(component);
            break;
        case 0x29u:
            result = acc(component) + vs(component) * vt(component);
            break;
        case 0x2Au:
            result = vs(component) * vt(component);
            break;
        case 0x2Cu:
            result = vs(component) - vt(component);
            break;
        case 0x2Du:
            result = acc(component) - vs(component) * vt(component);
            break;
        case 0x2Eu:
        {
            static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            result = component == 3u
                         ? 0.0L
                         : vs(left[component]) * vt(right[component]);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

uint8_t VU1Interpreter::normalizeFmacExactResult(float &value,
                                                  long double exactResult) const
{
    const bool negative = std::signbit(exactResult);
    const long double magnitude = std::fabs(exactResult);
    const long double maximum = static_cast<long double>(std::numeric_limits<float>::max());
    const long double minimum = static_cast<long double>(std::numeric_limits<float>::min());
    uint8_t flags = negative ? 0x2u : 0u;

    uint32_t bits = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0L)
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

uint32_t VU1Interpreter::calculateFmacProductSticky(uint8_t dest) const
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
        const long double exactProduct = static_cast<long double>(left) * static_cast<long double>(right);
        const uint8_t productFlags = normalizeFmacExactResult(product, exactProduct);
        // Product-sum instructions report Z/S/U/O from the add/subtract result
        // as current flags, while every product condition accumulates into the
        // corresponding sticky flag.
        extraSticky |= productFlags & 0xFu;
    }
    return extraSticky;
}

void VU1Interpreter::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
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

    FlagPipelineEntry *entry = nullptr;
    for (FlagPipelineEntry &candidate : m_flagPipeline)
    {
        if (!candidate.valid)
        {
            entry = &candidate;
            break;
        }
    }
    if (!entry)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }

    *entry = {};
    entry->valid = true;
    entry->issueCycle = m_cycle;
    entry->readyCycle = m_cycle + kFmacLatency;
    entry->mac = mac;
    entry->status = status;
    entry->extraSticky = extraSticky;
    entry->writesMac = true;
    entry->writesStatus = true;
}

void VU1Interpreter::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDest(dst, result, dest);
}

void VU1Interpreter::applyFmacDestAcc(float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDestAcc(result, dest);
}

void VU1Interpreter::queueFsset(uint16_t immediate)
{
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesStatus = false;
    }

    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.status = static_cast<uint32_t>(immediate) & 0xFC0u;
            entry.writesSticky = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFEu);
}

void VU1Interpreter::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFFDu);
}

void VU1Interpreter::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesClip = false;
    }
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFAu);
}

void VU1Interpreter::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
}

void VU1Interpreter::queueP(float value, uint32_t latency)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
        {
            entry.valid = true;
            entry.readyCycle = m_cycle + latency;
            entry.value = value;
            // EFU throughput is one cycle shorter than result visibility.
            m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}

void VU1Interpreter::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid)
        {
            store.valid = true;
            store.readyCycle = m_cycle + 1u;
            store.address = address;
            store.laneMask = laneMask;
            std::copy(words, words + 4, store.words.begin());
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFCu);
}

void VU1Interpreter::queueVfWrite(uint8_t reg, uint8_t laneMask,
                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    for (PendingVfWrite &write : m_vfWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_vfLatestWrite[reg][component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF7u);
}

void VU1Interpreter::queueViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.value = value;
            m_viLatestWrite[reg] = write.sequence;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF6u);
}

void VU1Interpreter::queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            write.sequence = ++m_nextWriteSequence;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_accLatestWrite[component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFF5u);
}

void VU1Interpreter::commitReadyPipelines()
{
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid || entry.readyCycle > m_cycle)
            continue;

        if (entry.writesMac)
            m_state.mac = entry.mac;
        if (entry.writesStatus)
        {
            const uint32_t current = entry.status & 0xFu;
            m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
        }
        if (entry.writesSticky)
        {
            m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
        }
        if (entry.writesClip)
            m_state.clip = entry.clip;
        entry = {};
    }

    if (m_fdiv.valid && m_fdiv.readyCycle <= m_cycle)
    {
        m_state.q = m_fdiv.value;
        const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
        m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
        m_fdiv = {};
    }

    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (entry.valid && entry.readyCycle <= m_cycle)
        {
            m_state.p = entry.value;
            entry = {};
        }
    }

    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid || store.readyCycle > m_cycle)
            continue;
        if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
        {
            uint32_t oldWords[4]{};
            std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((store.laneMask & laneForComponent(component)) != 0u)
                    oldWords[component] = store.words[component];
            }
            std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
        }
        store = {};
    }

    for (PendingVfWrite &write : m_vfWritePipeline)
    {
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_vfLatestWrite[write.reg][component] == write.sequence)
            {
                m_state.vf[write.reg][component] = write.value[component];
            }
        }
        write = {};
    }

    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        if (m_viLatestWrite[write.reg] == write.sequence)
            m_state.vi[write.reg] = static_cast<int16_t>(write.value);
        write = {};
    }

    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_accLatestWrite[component] == write.sequence)
            {
                m_state.acc[component] = write.value[component];
            }
        }
        write = {};
    }
}

void VU1Interpreter::progressXgkick()
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    ++m_xgkick.cycleCredit;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        for (uint32_t i = 0; i < 16u; ++i)
        {
            const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes + i) % m_activeVuDataSize;
            m_xgkick.packet[m_xgkick.copiedBytes + i] = m_activeVuData[source];
        }
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                reportReservedInstruction(false, 0xFFFFFFF8u);
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    if (m_activeMemory)
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);
    else if (m_activeGs)
        m_activeGs->processGIFPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
    m_xgkick.active = false;
}

void VU1Interpreter::startXgkick(uint32_t qwordAddress)
{
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;

    // E33: one relaxed check when stats are off.
    ps2_gfx_stats::noteXgkick();
    // E36: per-program XGKICK count for the dev-only trace. Counted on
    // every enabled run (not just histogram-armed ones) so each census
    // line carries its own program's count.
    if (m_traceCountKicks)
    {
        ++m_traceXgkick;
    }

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    m_xgkick = {};
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
}

void VU1Interpreter::advanceOneCycle()
{
    ++m_cycle;
    m_state.cycles = m_cycle;
    // LSU commits become visible at the cycle boundary before PATH1 consumes
    // its next qword from VU memory.
    commitReadyPipelines();
    progressXgkick();
}

void VU1Interpreter::advanceTo(uint64_t targetCycle)
{
    while (m_cycle < targetCycle)
        advanceOneCycle();
}

bool VU1Interpreter::pipelinesPending() const
{
    if (m_fdiv.valid || m_xgkick.active)
        return true;
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            return true;
    for (const FlagPipelineEntry &entry : m_flagPipeline)
        if (entry.valid)
            return true;
    for (const PendingStore &store : m_storePipeline)
        if (store.valid)
            return true;
    for (const PendingVfWrite &write : m_vfWritePipeline)
        if (write.valid)
            return true;
    for (const PendingViWrite &write : m_viWritePipeline)
        if (write.valid)
            return true;
    for (const PendingAccWrite &write : m_accWritePipeline)
        if (write.valid)
            return true;
    return false;
}

void VU1Interpreter::flushPipelines()
{
    while (pipelinesPending())
        advanceOneCycle();
}

uint64_t VU1Interpreter::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    uint64_t ready = m_cycle;
    const InstructionUsage *usages[2] = {
        &decoded.upperUsage,
        &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        if (!usage)
            continue;
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((access.lanes & laneForComponent(component)) != 0u)
                    ready = std::max(ready, m_vfReady[access.reg][component]);
            }
        }
        for (uint32_t reg = 1; reg < m_viReady.size(); ++reg)
        {
            if ((usage->viRead & (1u << reg)) != 0u)
                ready = std::max(ready, m_viReady[reg]);
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((usage->accRead & laneForComponent(component)) != 0u)
                ready = std::max(ready, m_accReady[component]);
        }
    }

    if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (decoded.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (decoded.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}

void VU1Interpreter::markPairWrites(const DecodedInstructionPair &decoded)
{
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u &&
        decoded.suppressedLowerVf != lowerWrite.reg)
    {
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((lowerWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[lowerWrite.reg][component] = m_cycle + latency;
        }
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        const uint32_t latency = decoded.upperUsage.vfLatency != 0u
                                     ? decoded.upperUsage.vfLatency
                                     : decoded.upperUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((upperWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[upperWrite.reg][component] = m_cycle + latency;
        }
    }

    for (uint32_t reg = 1; reg < m_viReady.size(); ++reg)
    {
        if ((decoded.lowerUsage.viWrite & (1u << reg)) != 0u)
            m_viReady[reg] = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
    }
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((decoded.upperUsage.accWrite & laneForComponent(component)) != 0u)
            m_accReady[component] = m_cycle + kAccForwardLatency;
    }
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeUpperUsage(uint32_t upper) const
{
    InstructionUsage usage;
    usage.pipeline = PipelineFmac;
    usage.latency = kFmacLatency;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t dest = DEST(upper);
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t fd = FD(upper);

    if (op <= 0x2Fu)
    {
        addVfRead(usage, fs, dest);
        addVfWrite(usage, fd, dest);
        if (op <= 0x1Bu)
            addVfRead(usage, ft, laneForComponent(op & 3u));
        else if (op >= 0x28u)
            addVfRead(usage, ft, op == 0x2Eu ? 0xEu : dest);
        if (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
            op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if (op >= 0x3Cu)
    {
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        const bool writesAcc =
            special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) ||
            special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        if (writesAcc)
        {
            addVfRead(usage, fs, dest);
            if (special <= 0x1Bu)
                addVfRead(usage, ft, laneForComponent(special & 3u));
            else if ((special >= 0x28u && special <= 0x2Eu))
                addVfRead(usage, ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if ((special >= 0x08u && special <= 0x0Fu) ||
                special == 0x21u || special == 0x23u || special == 0x25u ||
                special == 0x27u || special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if (special >= 0x10u && special <= 0x17u)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Du)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Fu)
        {
            addVfRead(usage, fs, 0xEu);
            addVfRead(usage, ft, 0x1u);
            usage.writesClip = true;
        }
        else if (special != 0x2Fu && special != 0x30u)
        {
            usage.reserved = true;
        }
        return usage;
    }

    usage.reserved = true;
    return usage;
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeLowerUsage(uint32_t lower) const
{
    InstructionUsage usage;
    if (lower == 0u || lower == 0x8000033Cu)
        return usage;

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t vfT = FT(lower);
    const uint8_t vfS = FS(lower);
    const uint8_t viT = VIT(lower);
    const uint8_t viS = VIS(lower);
    const uint8_t viD = VID(lower);
    const uint8_t dest = DEST(lower);
    auto readVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    auto writeVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWrite |= static_cast<uint16_t>(1u << reg);
    };

    switch (opHi)
    {
    case 0x00:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        return usage;
    case 0x01:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viT);
        addVfRead(usage, vfS, dest);
        return usage;
    case 0x04:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x05:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x08:
    case 0x09:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x10:
    case 0x12:
    case 0x13:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(1u);
        return usage;
    case 0x11:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        usage.writesClip = true;
        return usage;
    case 0x14:
    case 0x16:
    case 0x17:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x15:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        return usage;
    case 0x18:
    case 0x1A:
    case 0x1B:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x1C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(viT);
        return usage;
    case 0x20:
        usage.pipeline = PipelineBranch;
        return usage;
    case 0x21:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x24:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x25:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x28:
    case 0x29:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x40:
        break;
    default:
        usage.reserved = true;
        return usage;
    }

    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        readVi(viT);
        writeVi(viD);
        return usage;
    }
    if (direct == 0x32u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    }
    if (direct < 0x3Cu)
    {
        usage.reserved = true;
        return usage;
    }

    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    switch (special)
    {
    case 0x30:
    case 0x31:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfRead(usage, vfS, special == 0x31u ? 0xFu : dest);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x34:
    case 0x36:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        usage.viLatency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x35:
    case 0x37:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viT);
        writeVi(viT);
        addVfRead(usage, vfS, dest);
        break;
    case 0x38:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x39:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3A:
        usage.pipeline = PipelineFdiv;
        usage.latency = 13u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3B:
        usage.pipeline = PipelineFdiv;
        usage.waitQ = true;
        break;
    case 0x3C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        writeVi(viT);
        break;
    case 0x3D:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x3E:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        break;
    case 0x3F:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        break;
    case 0x40:
    case 0x41:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x42:
    case 0x43:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x64:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x68:
    case 0x69:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        break;
    case 0x6C:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineXgkick;
        usage.latency = 2u;
        readVi(viS);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        switch (special)
        {
        case 0x70:
            usage.latency = 11u;
            break;
        case 0x71:
        case 0x72:
        case 0x77:
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7C:
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78:
        case 0x7A:
            usage.latency = 12u;
            break;
        case 0x79:
            usage.latency = 29u;
            break;
        case 0x7D:
            usage.latency = 44u;
            break;
        default:
            break;
        }
        if (special >= 0x70u && special <= 0x73u)
            addVfRead(usage, vfS, 0xEu);
        else if (special == 0x74u)
            addVfRead(usage, vfS, 0xCu);
        else if (special == 0x75u)
            addVfRead(usage, vfS, 0xAu);
        else if (special == 0x76u)
            addVfRead(usage, vfS, 0xFu);
        else
            addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x7B:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        usage.waitP = true;
        break;
    default:
        usage.reserved = true;
        break;
    }
    return usage;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
{
    DecodedInstructionPair decoded;
    std::memcpy(&decoded.lower, vuCode + pc, sizeof(decoded.lower));
    std::memcpy(&decoded.upper, vuCode + pc + sizeof(decoded.lower), sizeof(decoded.upper));
    decoded.iBit = (decoded.upper & 0x80000000u) != 0u;
    decoded.eBit = (decoded.upper & 0x40000000u) != 0u;
    decoded.mBit = (decoded.upper & 0x20000000u) != 0u;
    decoded.dBit = (decoded.upper & 0x10000000u) != 0u;
    decoded.tBit = (decoded.upper & 0x08000000u) != 0u;
    decoded.upperUsage = decodeUpperUsage(decoded.upper);
    if (!decoded.iBit)
        decoded.lowerUsage = decodeLowerUsage(decoded.lower);

    const uint8_t upperWriteReg = decoded.upperUsage.vfWrite.reg;
    if (upperWriteReg != 0u && (vfReadLanes(decoded.lowerUsage, upperWriteReg) != 0u || decoded.lowerUsage.vfWrite.reg == upperWriteReg))
    {
        decoded.upperVfShadowReg = upperWriteReg;
        if (decoded.lowerUsage.vfWrite.reg == upperWriteReg)
            decoded.suppressedLowerVf = upperWriteReg;
    }
    return decoded;
}

void VU1Interpreter::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
                                             const PS2Memory *memory, uint64_t generation)
{
    const uint32_t pairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    for (uint32_t i = 0; i < pairCount; ++i)
        m_decodedCodeCache[i] = decodeInstructionPair(vuCode, i * 8u);

    m_cachedVuCode = vuCode;
    m_cachedMemory = memory;
    m_cachedCodeSize = codeSize;
    m_cachedCodeGeneration = generation;
    m_decodedCodeCacheValid = true;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
        return decodeInstructionPair(vuCode, pc);

    const bool trackedVu1Code = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedVu1Code)
        return decodeInstructionPair(vuCode, pc);

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    const uint32_t pairIndex = pc / 8u;
    if (pairIndex >= kMaxDecodedPairs)
        return decodeInstructionPair(vuCode, pc);
    return m_decodedCodeCache[pairIndex];
}

void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t instruction)
{
    RUNTIME_ERROR(
        "[VU" << (m_unit == Unit::VU1 ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << m_cycle
              << " pc=0x" << std::hex << m_state.pc
              << " instruction=0x" << instruction
              << std::dec << '\n');
    m_stopRequested = true;
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    resetScheduler();
    m_state.pc = startPC & microAddressMask();
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    // E36: key trace arming/dedupe on the MSCAL startPC; pick up the VIF1
    // snapshot stashed by noteMscal (absent for direct callers).
    m_traceProgramPC = startPC & microAddressMask();
    m_traceCtxValid = (m_unit == Unit::VU1) && ps2_vu1_trace::consumeContext(m_traceCtx);
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    // E36: an MSCNT continues the program keyed at the last execute().
    if (m_unit == Unit::VU1)
    {
        m_traceCtxValid = ps2_vu1_trace::consumeContext(m_traceCtx);
    }
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeGs = &gs;
    m_activeMemory = memory;

    const int previousRoundingMode = std::fegetround();
    const bool useVuRounding = std::fesetround(FE_TOWARDZERO) == 0;
    const uint64_t budgetEnd = m_cycle + maxCycles;
    const uint64_t entryCycle = m_cycle;
    bool programEnded = false;
    // E36: arm the dev-only per-program trace (one member branch per pair
    // when disarmed; histogram vectors only when armed).
    m_traceArmed = false;
    m_traceCountKicks = (m_unit == Unit::VU1) && ps2_vu1_trace::enabled();
    if (m_traceCountKicks)
    {
        m_traceXgkick = 0u;
    }
    if (m_traceCountKicks && ps2_vu1_trace::armFor(m_traceProgramPC))
    {
        m_traceArmed = true;
        const size_t pairCount = codeSize / 8u;
        m_traceHist.assign(pairCount, 0u);
        m_traceTaken.assign(pairCount, 0u);
        snapshotTraceHeaders(vuData, dataSize);
    }
    while (m_cycle < budgetEnd && !m_stopRequested)
    {
        commitReadyPipelines();
        if (m_state.pc + 8u > codeSize)
            break;

        const uint32_t traceIssuePc = m_state.pc;
        const uint32_t traceIssueIdx = traceIssuePc / 8u;
        if (m_traceArmed && traceIssueIdx < m_traceHist.size())
        {
            ++m_traceHist[traceIssueIdx];
        }
        // A branch "takes" when this pair newly raises branchPending (or
        // retargets it). A delay-slot branch to the identical target is
        // indistinguishable here; noted, negligible for hot-loop readout.
        const bool traceWasBranchPending = m_state.branchPending;
        const uint32_t traceWasBranchTarget = m_state.branchTarget;

        const DecodedInstructionPair decoded = getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
        if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
        {
            reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
            break;
        }

        uint64_t readyCycle = calculatePairReadyCycle(decoded);
        while (readyCycle > m_cycle)
        {
            if (readyCycle >= budgetEnd)
            {
                advanceTo(budgetEnd);
                break;
            }
            advanceTo(readyCycle);
            readyCycle = calculatePairReadyCycle(decoded);
        }
        if (m_cycle >= budgetEnd)
            break;

        uint8_t writtenVi = 0u;
        int32_t oldVi = 0;
        for (uint32_t reg = 1; reg < 16u; ++reg)
        {
            if ((decoded.lowerUsage.viWrite & (1u << reg)) != 0u)
            {
                writtenVi = static_cast<uint8_t>(reg);
                oldVi = m_state.vi[reg];
                break;
            }
        }

        const VfAccess upperWrite = decoded.upperUsage.vfWrite;
        const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
        const bool hasUpperWrite = upperWrite.reg != 0u;
        const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
        const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);
        float oldUpperVf[4]{};
        float newUpperVf[4]{};
        float oldLowerVf[4]{};
        float newLowerVf[4]{};
        float oldAcc[4]{};
        float newAcc[4]{};
        if (hasUpperWrite)
            std::memcpy(oldUpperVf, m_state.vf[upperWrite.reg], sizeof(oldUpperVf));
        if (hasDistinctLowerWrite)
            std::memcpy(oldLowerVf, m_state.vf[lowerWrite.reg], sizeof(oldLowerVf));
        if (decoded.upperUsage.accWrite != 0u)
            std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

        if (decoded.iBit)
        {
            execUpper(decoded.upper);
            float immediate = 0.0f;
            std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
            m_state.i = normalizeOperand(immediate);
        }
        else if (decoded.upperVfShadowReg != 0u)
        {
            float oldVf[4]{};
            float upperVf[4]{};
            std::memcpy(oldVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(oldVf));
            execUpper(decoded.upper);
            std::memcpy(upperVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(upperVf));
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        oldVf,
                        sizeof(oldVf));
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        upperVf,
                        sizeof(upperVf));
        }
        else
        {
            execUpper(decoded.upper);
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
        }

        m_viBranchBackupValid = false;

        if (m_traceArmed && m_state.branchPending &&
            (!traceWasBranchPending || m_state.branchTarget != traceWasBranchTarget) &&
            traceIssueIdx < m_traceTaken.size())
        {
            ++m_traceTaken[traceIssueIdx];
        }

        if (hasUpperWrite)
        {
            std::memcpy(newUpperVf, m_state.vf[upperWrite.reg], sizeof(newUpperVf));
            std::memcpy(m_state.vf[upperWrite.reg], oldUpperVf, sizeof(oldUpperVf));
            const uint32_t latency =
                decoded.upperUsage.vfLatency != 0u
                    ? decoded.upperUsage.vfLatency
                    : decoded.upperUsage.latency;
            queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
        }
        if (hasDistinctLowerWrite)
        {
            std::memcpy(newLowerVf, m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
            std::memcpy(m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(oldLowerVf));
            const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                         ? decoded.lowerUsage.vfLatency
                                         : decoded.lowerUsage.latency;
            queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
        }
        if (decoded.upperUsage.accWrite != 0u)
        {
            std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
            std::memcpy(m_state.acc, oldAcc, sizeof(oldAcc));
            // ACC is forwarded to the next upper instruction. Its arithmetic
            // flags still use the normal four-cycle FMAC timeline.
            queueAccWrite(decoded.upperUsage.accWrite, newAcc,
                          kAccForwardLatency);
        }
        if (writtenVi != 0u)
        {
            const int32_t newVi = m_state.vi[writtenVi];
            m_state.vi[writtenVi] = oldVi;
            const uint32_t latency =
                decoded.lowerUsage.viLatency != 0u
                    ? decoded.lowerUsage.viLatency
                    : decoded.lowerUsage.latency;
            queueViWrite(writtenVi, newVi, latency);
        }

        markPairWrites(decoded);
        if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
            recordViWriteForBranch(writtenVi, oldVi);

        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        m_state.vi[0] = 0;

        uint32_t nextPc = m_state.pc + 8u;
        if (nextPc >= codeSize)
            nextPc = 0u;
        m_state.pc = nextPc;

        if (m_state.branchPending)
        {
            if (m_state.branchDelay == 0u)
            {
                m_state.pc = m_state.branchTarget & microAddressMask();
                m_state.branchPending = false;
            }
            else
            {
                --m_state.branchDelay;
            }
        }

        const bool dHalt = decoded.dBit && m_state.dBitEnabled;
        const bool tHalt = decoded.tBit && m_state.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;

        if (m_state.haltAfterDelaySlot)
        {
            m_state.stoppedByD = m_pendingHaltD;
            m_state.stoppedByT = m_pendingHaltT;
            programEnded = true;
        }
        else if (m_state.ebit)
            programEnded = true;
        else if (haltBit && !haltBranch)
        {
            m_state.stoppedByD = dHalt;
            m_state.stoppedByT = tHalt;
            programEnded = true;
        }
        else if (decoded.eBit)
            m_state.ebit = true;
        else if (haltBranch)
        {
            m_state.haltAfterDelaySlot = true;
            m_pendingHaltD = dHalt;
            m_pendingHaltT = tHalt;
        }

        advanceOneCycle();
        if (programEnded)
            break;
    }

    if (programEnded)
    {
        flushPipelines();
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    // E33: budget census. A program that leaves the loop without an end
    // marker (E/D/T bit, halt delay slot) exactly at/over its cycle budget
    // was truncated: its remaining draws never issue. One relaxed check
    // when stats are off.
    const uint64_t cyclesUsed = m_cycle - entryCycle;
    const bool budgetExhausted = !programEnded && !m_stopRequested && m_cycle >= budgetEnd;
    ps2_gfx_stats::noteVuRun(cyclesUsed, budgetExhausted);
    // E36: dev-only per-program trace. Census for every exhausted program
    // in the window; a detail block for the first 40 distinct startPCs.
    if (m_unit == Unit::VU1 && ps2_vu1_trace::enabled() && budgetExhausted)
    {
        ps2_vu1_trace::noteCensus(m_traceProgramPC, cyclesUsed, m_traceXgkick);
        if (m_traceArmed)
        {
            std::string block;
            buildTraceDetail(vuCode, codeSize, memory, cyclesUsed, block);
            ps2_vu1_trace::emitDetail(m_traceProgramPC, block);
        }
    }
    m_traceArmed = false;
    m_traceCountKicks = false;
    m_state.cycles = m_cycle;
    if (useVuRounding && previousRoundingMode != -1)
        std::fesetround(previousRoundingMode);
}

// E36: dev-only trace helpers. The mini-decoders below mirror the
// dispatch in execLower (bits 31:25 primary opcode, Lower1 funct in the
// low 6 bits, Lower1-special funct2 per DobieStation). They name loop
// control, integer couners, flag tests and XGKICK/X TOP ops; anything
// else prints as opHi/funct hex. Upper pairs print raw except NOP.

namespace
{
    struct TraceLowerDesc
    {
        std::string text;
        const char *branchOp = nullptr; // set for B/BAL/JR/JALR/IBcc
        bool targetStatic = false;
        int16_t imm = 0;
    };

    const char *traceLower1SpecialName(uint8_t funct2)
    {
        switch (funct2)
        {
        case 0x30: return "MOVE";
        case 0x31: return "MR32";
        case 0x34: return "LQI";
        case 0x35: return "SQI";
        case 0x36: return "LQD";
        case 0x37: return "SQD";
        case 0x38: return "DIV";
        case 0x39: return "SQRT";
        case 0x3A: return "RSQRT";
        case 0x3B: return "WAITQ";
        case 0x3C: return "MTIR";
        case 0x3D: return "MFIR";
        case 0x3E: return "ILWR";
        case 0x3F: return "ISWR";
        case 0x40: return "RNEXT";
        case 0x41: return "RGET";
        case 0x42: return "RINIT";
        case 0x43: return "RXOR";
        case 0x64: return "MFP";
        case 0x68: return "XTOP";
        case 0x69: return "XITOP";
        case 0x6C: return "XGKICK";
        case 0x70: return "ESADD";
        case 0x71: return "ERSADD";
        case 0x72: return "ELENG";
        case 0x73: return "ERLENG";
        case 0x74: return "EATANxy";
        case 0x75: return "EATANxz";
        case 0x76: return "ESUM";
        case 0x77: return "ERSQRT";
        case 0x78: return "ESQRT";
        case 0x79: return "ESIN";
        case 0x7A: return "ERCPR";
        case 0x7B: return "WAITP";
        case 0x7C: return "EATAN";
        case 0x7D: return "EEXP";
        default: return nullptr;
        }
    }

    bool traceIsFlagOpName(const std::string &text)
    {
        static const char *kFlagOps[] = {
            "FCAND", "FSAND", "FMAND", "FMOR", "FMEQ",
            "FCEQ", "FSEQ", "FCSET", "FSSET", "FCOR", "FSOR", "FCGET",
        };
        for (const char *op : kFlagOps)
        {
            if (text.compare(0, std::strlen(op), op) == 0)
            {
                return true;
            }
        }
        return false;
    }

    TraceLowerDesc traceDescribeLower(uint32_t w)
    {
        TraceLowerDesc d;
        char buf[96];
        if (w == 0x00000000u || w == 0x8000033Cu)
        {
            d.text = "NOP";
            return d;
        }
        const uint32_t opHi = (w >> 25) & 0x7Fu;
        const uint8_t it = VIT(w);
        const uint8_t is = VIS(w);
        const uint8_t id = VID(w);
        const int16_t imm = IMM11(w);
        switch (opHi)
        {
        case 0x00:
            std::snprintf(buf, sizeof(buf), "LQ vf%u,%d(vi%u)", FT(w), imm, is);
            d.text = buf;
            return d;
        case 0x01:
            std::snprintf(buf, sizeof(buf), "SQ vf%u,%d(vi%u)", FS(w), imm, it);
            d.text = buf;
            return d;
        case 0x04:
            std::snprintf(buf, sizeof(buf), "ILW vi%u,%d(vi%u)", it, imm, is);
            d.text = buf;
            return d;
        case 0x05:
            std::snprintf(buf, sizeof(buf), "ISW vi%u,%d(vi%u)", it, imm, is);
            d.text = buf;
            return d;
        case 0x08:
        case 0x09:
        {
            const int imm15 = (int)(int16_t)((w & 0x7FFu) | ((w >> 10) & 0x7800u));
            std::snprintf(buf, sizeof(buf), "%s vi%u,vi%u,%d",
                          opHi == 0x08 ? "IADDIU" : "ISUBIU", it, is, imm15);
            d.text = buf;
            return d;
        }
        case 0x10:
            std::snprintf(buf, sizeof(buf), "FCEQ 0x%x", w & 0xFFFFFFu);
            d.text = buf;
            return d;
        case 0x11:
            std::snprintf(buf, sizeof(buf), "FCSET 0x%x", w & 0xFFFFFFu);
            d.text = buf;
            return d;
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
        {
            static const char *kNames[] = {"?", "?", "FCAND", "FCOR", "FSEQ", "FSSET", "FSAND", "FSOR"};
            std::snprintf(buf, sizeof(buf), "%s vi%u,0x%x", kNames[opHi - 0x10], it, w & 0x7FFu);
            d.text = buf;
            return d;
        }
        case 0x18:
            std::snprintf(buf, sizeof(buf), "FMEQ vi%u,vi%u", it, is);
            d.text = buf;
            return d;
        case 0x1A:
            std::snprintf(buf, sizeof(buf), "FMAND vi%u,vi%u", it, is);
            d.text = buf;
            return d;
        case 0x1B:
            std::snprintf(buf, sizeof(buf), "FMOR vi%u,vi%u", it, is);
            d.text = buf;
            return d;
        case 0x1C:
            std::snprintf(buf, sizeof(buf), "FCGET vi%u", it);
            d.text = buf;
            return d;
        case 0x20:
            std::snprintf(buf, sizeof(buf), "B %d", imm);
            d.text = buf;
            d.branchOp = "B";
            d.targetStatic = true;
            d.imm = imm;
            return d;
        case 0x21:
            std::snprintf(buf, sizeof(buf), "BAL vi%u,%d", it, imm);
            d.text = buf;
            d.branchOp = "BAL";
            d.targetStatic = true;
            d.imm = imm;
            return d;
        case 0x24:
            std::snprintf(buf, sizeof(buf), "JR (vi%u)", is);
            d.text = buf;
            d.branchOp = "JR";
            return d;
        case 0x25:
            std::snprintf(buf, sizeof(buf), "JALR vi%u,(vi%u)", it, is);
            d.text = buf;
            d.branchOp = "JALR";
            return d;
        case 0x28:
        case 0x29:
        case 0x2C:
        case 0x2D:
        case 0x2E:
        case 0x2F:
        {
            const char *name = "?";
            switch (opHi)
            {
            case 0x28: name = "IBEQ"; break;
            case 0x29: name = "IBNE"; break;
            case 0x2C: name = "IBLTZ"; break;
            case 0x2D: name = "IBGTZ"; break;
            case 0x2E: name = "IBLEZ"; break;
            case 0x2F: name = "IBGEZ"; break;
            }
            if (opHi == 0x28 || opHi == 0x29)
            {
                std::snprintf(buf, sizeof(buf), "%s vi%u,vi%u,%d", name, it, is, imm);
            }
            else
            {
                std::snprintf(buf, sizeof(buf), "%s vi%u,%d", name, is, imm);
            }
            d.text = buf;
            d.branchOp = name;
            d.targetStatic = true;
            d.imm = imm;
            return d;
        }
        case 0x40:
        {
            const uint8_t funct = w & 0x3Fu;
            if (funct == 0x30 || funct == 0x31 || funct == 0x32 ||
                funct == 0x34 || funct == 0x35)
            {
                const char *name = "?";
                switch (funct)
                {
                case 0x30: name = "IADD"; break;
                case 0x31: name = "ISUB"; break;
                case 0x32: name = "IADDI"; break;
                case 0x34: name = "IAND"; break;
                case 0x35: name = "IOR"; break;
                }
                if (funct == 0x32)
                {
                    const int imm5 = (int)((int32_t)((w >> 6) & 0x1F) << 27 >> 27);
                    std::snprintf(buf, sizeof(buf), "IADDI vi%u,vi%u,%d", it, is, imm5);
                }
                else
                {
                    std::snprintf(buf, sizeof(buf), "%s vi%u,vi%u,vi%u", name, id, is, it);
                }
                d.text = buf;
                return d;
            }
            if (funct >= 0x3Cu)
            {
                const uint8_t funct2 = (uint8_t)((w & 0x3u) | ((w >> 4) & 0x7Cu));
                const char *name = traceLower1SpecialName(funct2);
                if (name != nullptr)
                {
                    if (funct2 == 0x68 || funct2 == 0x69)
                    {
                        std::snprintf(buf, sizeof(buf), "%s vi%u", name, it);
                    }
                    else if (funct2 == 0x6C)
                    {
                        std::snprintf(buf, sizeof(buf), "XGKICK (vi%u)", is);
                    }
                    else if (funct2 == 0x3C || funct2 == 0x3D)
                    {
                        std::snprintf(buf, sizeof(buf), "%s vi%u,vf%u", name,
                                      funct2 == 0x3C ? id : it,
                                      funct2 == 0x3C ? FS(w) : FT(w));
                    }
                    else
                    {
                        std::snprintf(buf, sizeof(buf), "%s vf%u,vf%u", name, FT(w), FS(w));
                    }
                    d.text = buf;
                    return d;
                }
            }
            std::snprintf(buf, sizeof(buf), "lower1:0x%02x", funct);
            d.text = buf;
            return d;
        }
        default:
            std::snprintf(buf, sizeof(buf), "opHi=0x%02x", opHi);
            d.text = buf;
            return d;
        }
    }
} // namespace

void VU1Interpreter::snapshotTraceHeaders(const uint8_t *vuData, uint32_t dataSize)
{
    m_traceTopQw.fill(0u);
    m_traceItopQw.fill(0u);
    if (vuData == nullptr || dataSize == 0u)
    {
        return;
    }
    for (uint32_t slot = 0u; slot < 2u; ++slot)
    {
        const uint32_t row = (slot == 0u ? m_state.top : m_state.itop) & 0x3FFu;
        const uint64_t base = static_cast<uint64_t>(row) * 16u;
        std::array<uint32_t, 32> &dst = (slot == 0u ? m_traceTopQw : m_traceItopQw);
        for (uint32_t i = 0u; i < 32u; ++i)
        {
            const uint64_t off = base + static_cast<uint64_t>(i) * 4u;
            if (off + 4u <= dataSize)
            {
                uint32_t word = 0u;
                std::memcpy(&word, vuData + off, sizeof(word));
                dst[i] = word;
            }
        }
    }
}

void VU1Interpreter::buildTraceDetail(const uint8_t *vuCode, uint32_t codeSize,
                                      PS2Memory *memory, uint64_t cyclesUsed,
                                      std::string &out)
{
    std::ostringstream s;
    s << std::hex;
    const uint32_t pcMask = microAddressMask();
    char num[32];

    // Header: MSCAL context + run totals.
    const char *ctxName = "none";
    if (m_traceCtxValid)
    {
        ctxName = m_traceCtx.isMscnt ? "mscnt" : "mscal";
    }
    std::snprintf(num, sizeof(num), "0x%x", m_traceProgramPC);
    s << "detail startPC=" << num;
    if (m_traceCtxValid)
    {
        s << " top=" << std::dec << m_traceCtx.top
          << " itop=" << m_traceCtx.itop
          << " base=" << m_traceCtx.base
          << " ofst=" << m_traceCtx.ofst
          << " tops=" << m_traceCtx.tops
          << " itops=" << m_traceCtx.itops
          << " dbf=" << (m_traceCtx.dbf ? 1 : 0) << std::hex;
    }
    else
    {
        s << " top=- itop=- base=- ofst=- tops=- itops=- dbf=-";
    }
    s << " ctx=" << ctxName;
    s << std::dec << " cycles=" << cyclesUsed << " xgkick=" << m_traceXgkick << "\n";

    // Input header qwords at TOP and ITOP (program-start snapshot).
    s << "topq";
    for (uint32_t w : m_traceTopQw)
    {
        std::snprintf(num, sizeof(num), " %08x", w);
        s << num;
    }
    s << "\nitopq";
    for (uint32_t w : m_traceItopQw)
    {
        std::snprintf(num, sizeof(num), " %08x", w);
        s << num;
    }
    s << "\nvi";
    for (uint32_t r = 0u; r < 16u; ++r)
    {
        std::snprintf(num, sizeof(num), " %08x", static_cast<uint32_t>(m_state.vi[r]));
        s << num;
    }
    std::snprintf(num, sizeof(num), " mac=%08x status=%08x clip=%08x",
                  m_state.mac, m_state.status, m_state.clip);
    s << num;
    std::snprintf(num, sizeof(num), " endpc=0x%x", m_state.pc);
    s << num << "\n";

    // PC-visit and branch-taken histograms, top 10 each.
    std::vector<std::pair<uint32_t, uint32_t>> hist; // (count, pc)
    std::vector<std::pair<uint32_t, uint32_t>> taken;
    for (size_t i = 0u; i < m_traceHist.size(); ++i)
    {
        if (m_traceHist[i] != 0u)
        {
            hist.emplace_back(m_traceHist[i], static_cast<uint32_t>(i * 8u));
        }
        if (i < m_traceTaken.size() && m_traceTaken[i] != 0u)
        {
            taken.emplace_back(m_traceTaken[i], static_cast<uint32_t>(i * 8u));
        }
    }
    const auto byCountDesc = [](const auto &a, const auto &b)
    {
        if (a.first != b.first)
        {
            return a.first > b.first;
        }
        return a.second < b.second;
    };
    std::sort(hist.begin(), hist.end(), byCountDesc);
    std::sort(taken.begin(), taken.end(), byCountDesc);
    s << "hist";
    if (hist.empty())
    {
        s << " none";
    }
    for (size_t i = 0u; i < hist.size() && i < 10u; ++i)
    {
        std::snprintf(num, sizeof(num), " 0x%x=%u", hist[i].second, hist[i].first);
        s << num;
    }
    s << "\ntaken";
    if (taken.empty())
    {
        s << " none";
    }
    for (size_t i = 0u; i < taken.size() && i < 10u; ++i)
    {
        std::snprintf(num, sizeof(num), " 0x%x=%u", taken[i].second, taken[i].first);
        s << num;
    }
    s << "\n";

    // Hottest backward branch: max taken count among taken branches whose
    // static target runs backward (or is the branch itself); JR/JALR are
    // dynamic and only win when no static-backward branch took.
    bool haveBranch = false;
    bool branchBackward = false;
    uint32_t branchPc = 0u;
    TraceLowerDesc branchDesc;
    uint32_t branchTaken = 0u;
    uint32_t branchTarget = 0u;
    bool branchTargetKnown = false;
    // `taken` is hottest-first: the first entry is the hottest taken
    // branch overall; the first static-backward entry is the hottest loop.
    for (const auto &t : taken)
    {
        const uint32_t pc = t.second;
        if (pc + 8u > codeSize)
        {
            continue;
        }
        uint32_t lo = 0u;
        std::memcpy(&lo, vuCode + pc, sizeof(lo));
        TraceLowerDesc d = traceDescribeLower(lo);
        if (d.branchOp == nullptr)
        {
            continue;
        }
        bool backward = false;
        uint32_t target = 0u;
        bool known = false;
        if (d.targetStatic)
        {
            target = (pc + 8u + static_cast<uint32_t>(d.imm * 8)) & pcMask;
            known = true;
            backward = target <= pc;
        }
        if (!haveBranch || (backward && !branchBackward))
        {
            haveBranch = true;
            branchBackward = backward;
            branchPc = pc;
            branchDesc = d;
            branchTaken = t.first;
            branchTarget = target;
            branchTargetKnown = known;
        }
    }
    uint32_t branchVisits = 0u;
    if (haveBranch && branchPc / 8u < m_traceHist.size())
    {
        branchVisits = m_traceHist[branchPc / 8u];
    }
    // Flag ops inside the loop span (static target..branch PC).
    std::string bodyFlags = "none";
    if (haveBranch && branchTargetKnown)
    {
        std::string acc;
        for (uint32_t pc = branchTarget; pc <= branchPc && pc + 8u <= codeSize; pc += 8u)
        {
            uint32_t lo = 0u;
            std::memcpy(&lo, vuCode + pc, sizeof(lo));
            TraceLowerDesc d = traceDescribeLower(lo);
            if (traceIsFlagOpName(d.text) && acc.find(d.text.substr(0, d.text.find(' '))) == std::string::npos)
            {
                if (!acc.empty())
                {
                    acc += ",";
                }
                acc += d.text.substr(0, d.text.find(' '));
            }
        }
        if (!acc.empty())
        {
            bodyFlags = acc;
        }
    }
    if (haveBranch)
    {
        uint32_t lo = 0u;
        std::memcpy(&lo, vuCode + branchPc, sizeof(lo));
        const uint8_t vis = VIS(lo);
        const uint8_t vit = VIT(lo);
        std::snprintf(num, sizeof(num), "0x%x", branchPc);
        s << "branch pc=" << num << " op=" << branchDesc.branchOp;
        const uint32_t opHi = (lo >> 25) & 0x7Fu;
        if (opHi == 0x20)
        {
            std::snprintf(num, sizeof(num), " imm=%d", branchDesc.imm);
            s << " is=- it=-" << num;
        }
        else if (opHi == 0x21 || opHi == 0x24 || opHi == 0x25)
        {
            s << " is=" << std::dec << static_cast<unsigned>(vis)
              << " it=" << static_cast<unsigned>(vit) << std::hex;
        }
        else
        {
            s << " is=" << std::dec << static_cast<unsigned>(vis)
              << " it=" << static_cast<unsigned>(vit);
            std::snprintf(num, sizeof(num), " imm=%d", branchDesc.imm);
            s << num << std::hex;
        }
        if (branchTargetKnown)
        {
            std::snprintf(num, sizeof(num), " target=0x%x", branchTarget);
            s << num;
        }
        else
        {
            s << " target=dyn";
        }
        s << std::dec << " taken=" << branchTaken << " visits=" << branchVisits;
        s << " vi_is=" << m_state.vi[vis] << " vi_it=" << m_state.vi[vit];
        s << " bodyflags=" << bodyFlags << "\n";
    }
    else
    {
        s << "branch none\n";
    }

    // Loop body disassembly: the static span target..PC plus the branch
    // delay slot. Spans over 32 PCs keep the 32 ending at the delay slot
    // (branch context over loop head); the flag-op scan above already
    // covers the whole span. Without a static branch, the 32 most-visited
    // PCs in ascending order.
    std::vector<uint32_t> bodyPcs;
    if (haveBranch && branchTargetKnown)
    {
        std::vector<uint32_t> span;
        for (uint32_t pc = branchTarget; pc <= branchPc && span.size() < 4096u; pc += 8u)
        {
            span.push_back(pc);
        }
        if (branchPc + 8u <= codeSize)
        {
            span.push_back(branchPc + 8u); // delay slot
        }
        const size_t keep = 32u;
        const size_t skip = span.size() > keep ? span.size() - keep : 0u;
        for (size_t i = skip; i < span.size(); ++i)
        {
            bodyPcs.push_back(span[i]);
        }
    }
    else
    {
        std::vector<std::pair<uint32_t, uint32_t>> byPc = hist;
        std::sort(byPc.begin(), byPc.end(),
                  [](const auto &a, const auto &b)
                  { return a.second < b.second; });
        for (size_t i = 0u; i < byPc.size() && bodyPcs.size() < 32u; ++i)
        {
            bodyPcs.push_back(byPc[i].second);
        }
    }
    for (uint32_t pc : bodyPcs)
    {
        if (pc + 8u > codeSize)
        {
            continue;
        }
        uint32_t lo = 0u, up = 0u;
        std::memcpy(&lo, vuCode + pc, sizeof(lo));
        std::memcpy(&up, vuCode + pc + 4u, sizeof(up));
        DecodedInstructionPair decoded = getDecodedInstructionPairForPc(vuCode, codeSize, memory, pc);
        TraceLowerDesc ld = traceDescribeLower(lo);
        std::string upText = (up == 0x000002FFu) ? "NOP" : "upper";
        std::snprintf(num, sizeof(num), "0x%x", pc);
        s << "body " << num;
        std::snprintf(num, sizeof(num), " lo=0x%08x up=0x%08x", lo, up);
        s << num;
        if (decoded.eBit)
        {
            s << " E";
        }
        if (decoded.dBit)
        {
            s << " D";
        }
        if (decoded.tBit)
        {
            s << " T";
        }
        if (decoded.iBit)
        {
            s << " I";
        }
        s << " " << ld.text << " | " << upText << "\n";
    }
    // Entry path: visited PCs outside the emitted body (cap 16, ascending),
    // i.e. how the program reaches the loop and arms its limit/counter.
    s << "entry";
    {
        std::vector<bool> inBody(codeSize / 8u + 1u, false);
        for (uint32_t pc : bodyPcs)
        {
            if (pc / 8u < inBody.size())
            {
                inBody[pc / 8u] = true;
            }
        }
        std::vector<std::pair<uint32_t, uint32_t>> outside; // (pc, count)
        for (size_t i = 0u; i < m_traceHist.size(); ++i)
        {
            if (m_traceHist[i] != 0u && (i >= inBody.size() || !inBody[i]))
            {
                outside.emplace_back(static_cast<uint32_t>(i * 8u), m_traceHist[i]);
            }
        }
        std::sort(outside.begin(), outside.end(),
                  [](const auto &a, const auto &b)
                  { return a.first < b.first; });
        if (outside.empty())
        {
            s << " none";
        }
        for (size_t i = 0u; i < outside.size() && i < 16u; ++i)
        {
            std::snprintf(num, sizeof(num), " 0x%x=%u", outside[i].first, outside[i].second);
            s << num;
        }
        if (outside.size() > 16u)
        {
            std::snprintf(num, sizeof(num), " +%u more", static_cast<unsigned>(outside.size() - 16u));
            s << num;
        }
    }
    s << "\n";
    out = s.str();
}
