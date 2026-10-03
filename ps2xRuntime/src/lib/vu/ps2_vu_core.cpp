#include "runtime/ps2_vu0.h"
#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_gfx_stats.h"
#include "ps2_vu_detail.h"
#include "ps2_vu_step_impl.h"
#include "ps2_vu_fmac_impl.h"

#include <algorithm>
#include <bit>
#include "ps2_fpmode.h"
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
}

template <class D>
void VuCore<D>::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
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

template <class D>
void VuCore<D>::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

template <class D>
uint8_t VuCore<D>::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

template <class D>
void VuCore<D>::resetScheduler()
{
    m_flagPipeline = {};
    m_fdiv = {};
    m_efu = {};
    m_storePipeline = {};
    m_vfWritePipeline = {};
    m_viWritePipeline = {};
    m_accWritePipeline = {};
    m_flagValidMask = 0;
    m_storeValidMask = 0;
    m_vfWriteValidMask = 0;
    m_viWriteValidMask = 0;
    m_accWriteValidMask = 0;
    m_nextCommitCycle = ~0ull;
    m_directPendingUntil = 0;
    m_directStores = false;
    m_directFlags = false;
    if constexpr (isVu1())
        derived().m_xgkick.reset();
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

template <class D>
void VuCore<D>::reset()
{
#if PS2X_ENABLE_DET_HASH_TAP
    m_programStartCount = 0;
#endif
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    resetScheduler();
}

// VX1: the unit-independent half of the old VU1Interpreter constructor. The
// derived constructor finishes (VU1's flag-elide knob) and calls reset(),
// which needs the derived members constructed.
template <class D>
VuCore<D>::VuCore()
{
    const char *floatMode = std::getenv("PS2X_VU_FLOAT");
    m_pcsx2Float = floatMode != nullptr && std::strcmp(floatMode, "pcsx2") == 0;
}

template <class D>
void VuCore<D>::beginProgram(uint32_t startPC, uint32_t top, uint32_t itop)
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
}

template <class D>
void VuCore<D>::beginResume(uint32_t top, uint32_t itop)
{
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
}

template <class D>
int32_t VuCore<D>::readBranchVi(uint8_t reg) const
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

template <class D>
void VuCore<D>::recordViWriteForBranch(uint8_t reg, int32_t oldValue)
{
    if (reg == 0u)
        return;
    m_viBranchBackupValue = oldValue;
    m_viBranchBackupReg = reg;
    m_viBranchBackupValid = true;
}

template <class D>
bool VuCore<D>::calculateFmacExactResult(uint32_t component,
                                               VuWide &result) const
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
        return static_cast<VuWide>(normalizeOperand(value));
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

    const VuWide q = operand(m_state.q);
    const VuWide i = operand(m_state.i);

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
                             ? 0.0
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
                         ? 0.0
                         : vs(left[component]) * vt(right[component]);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

template <class D>
void VuCore<D>::queueFsset(uint16_t immediate)
{
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesStatus = false;
    }

    const int slot = firstFreeEntry(m_flagValidMask, kMaxFlagEntries);
    if (slot >= 0)
    {
        FlagPipelineEntry &entry = m_flagPipeline[slot];
        entry = {};
        entry.valid = true;
        entry.issueCycle = m_cycle;
        entry.readyCycle = m_cycle + kFmacLatency;
        entry.status = static_cast<uint32_t>(immediate) & 0xFC0u;
        entry.writesSticky = true;
        m_flagValidMask |= 1u << slot;
        noteQueued(entry.readyCycle);
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFFEu);
}

template <class D>
void VuCore<D>::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    const bool directFlags = directFlagsNow();
    if (directFlags)
    {
        if (m_flagValidMask != 0u)
            demoteQueuedFlags(false, true);
        m_state.clip = m_workingClip;
        noteDirect(m_cycle + kFmacLatency);
        return;
    }
    const int slot = firstFreeEntry(m_flagValidMask, kMaxFlagEntries);
    if (slot >= 0)
    {
        FlagPipelineEntry &entry = m_flagPipeline[slot];
        entry = {};
        entry.valid = true;
        entry.issueCycle = m_cycle;
        entry.readyCycle = m_cycle + kFmacLatency;
        entry.clip = m_workingClip;
        entry.writesClip = true;
        m_flagValidMask |= 1u << slot;
        noteQueued(entry.readyCycle);
        return;
    }
    reportReservedInstruction(true, 0xFFFFFFFDu);
}

template <class D>
void VuCore<D>::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesClip = false;
    }
    const int slot = firstFreeEntry(m_flagValidMask, kMaxFlagEntries);
    if (slot >= 0)
    {
        FlagPipelineEntry &entry = m_flagPipeline[slot];
        entry = {};
        entry.valid = true;
        entry.issueCycle = m_cycle;
        entry.readyCycle = m_cycle + kFmacLatency;
        entry.clip = m_workingClip;
        entry.writesClip = true;
        m_flagValidMask |= 1u << slot;
        noteQueued(entry.readyCycle);
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFFAu);
}

template <class D>
void VuCore<D>::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
    noteQueued(m_fdiv.readyCycle);
}

template <class D>
void VuCore<D>::queueP(float value, uint32_t latency)
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
            noteQueued(entry.readyCycle);
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}

template <class D>
void VuCore<D>::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    const int slot = firstFreeEntry(m_storeValidMask, kMaxPendingStores);
    if (slot >= 0)
    {
        PendingStore &store = m_storePipeline[slot];
        store.valid = true;
        store.readyCycle = m_cycle + 1u;
        store.address = address;
        store.laneMask = laneMask;
        std::copy(words, words + 4, store.words.begin());
        m_storeValidMask |= 1u << slot;
        noteQueued(store.readyCycle);
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFFCu);
}

template <class D>
void VuCore<D>::queueVfWrite(uint8_t reg, uint8_t laneMask,
                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    const int slot = firstFreeEntry(m_vfWriteValidMask, kMaxPendingVfWrites);
    if (slot >= 0)
    {
        PendingVfWrite &write = m_vfWritePipeline[slot];
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
        m_vfWriteValidMask |= 1u << slot;
        noteQueued(write.readyCycle);
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFF7u);
}

template <class D>
void VuCore<D>::queueViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    const int slot = firstFreeEntry(m_viWriteValidMask, kMaxPendingViWrites);
    if (slot >= 0)
    {
        PendingViWrite &write = m_viWritePipeline[slot];
        write = {};
        write.valid = true;
        write.readyCycle = m_cycle + latency;
        write.sequence = ++m_nextWriteSequence;
        write.reg = reg;
        write.value = value;
        m_viLatestWrite[reg] = write.sequence;
        m_viWriteValidMask |= 1u << slot;
        noteQueued(write.readyCycle);
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFF6u);
}

template <class D>
void VuCore<D>::queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    const int slot = firstFreeEntry(m_accWriteValidMask, kMaxPendingAccWrites);
    if (slot >= 0)
    {
        PendingAccWrite &write = m_accWritePipeline[slot];
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
        m_accWriteValidMask |= 1u << slot;
        noteQueued(write.readyCycle);
        return;
    }
    reportReservedInstruction(true, 0xFFFFFFF5u);
}

template <class D>
void VuCore<D>::commitReadyPipelines()
{
    // E57: nothing queued is due yet, so a full scan would change nothing.
    if (m_cycle < m_nextCommitCycle)
        return;
    uint64_t nextReady = ~0ull;

    for (uint32_t pending = m_flagValidMask; pending != 0u; pending &= pending - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(pending));
        FlagPipelineEntry &entry = m_flagPipeline[index];
        if (entry.readyCycle > m_cycle)
        {
            nextReady = std::min(nextReady, entry.readyCycle);
            continue;
        }

        if (entry.writesMac)
            m_state.mac = entry.mac;
        if (entry.writesStatus)
        {
            const uint32_t current = entry.status & 0xFu;
            m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
        }
        if (entry.writesStickyOr)
        {
            // VB1 d3: sticky part of a superseded entry (see demoteQueuedFlags).
            const uint32_t current = entry.status & 0xFu;
            m_state.status |= ((current | entry.extraSticky) << 6) & 0xFF0u;
        }
        if (entry.writesSticky)
        {
            m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
        }
        if (entry.writesClip)
            m_state.clip = entry.clip;
        entry = {};
        m_flagValidMask &= ~(1u << index);
    }

    if (m_fdiv.valid)
    {
        if (m_fdiv.readyCycle <= m_cycle)
        {
            m_state.q = m_fdiv.value;
            const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
            m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
            m_fdiv = {};
        }
        else
            nextReady = std::min(nextReady, m_fdiv.readyCycle);
    }

    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
            continue;
        if (entry.readyCycle <= m_cycle)
        {
            m_state.p = entry.value;
            entry = {};
        }
        else
            nextReady = std::min(nextReady, entry.readyCycle);
    }

    for (uint32_t pending = m_storeValidMask; pending != 0u; pending &= pending - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(pending));
        PendingStore &store = m_storePipeline[index];
        if (store.readyCycle > m_cycle)
        {
            nextReady = std::min(nextReady, store.readyCycle);
            continue;
        }
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
        m_storeValidMask &= ~(1u << index);
    }

    for (uint32_t pending = m_vfWriteValidMask; pending != 0u; pending &= pending - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(pending));
        PendingVfWrite &write = m_vfWritePipeline[index];
        if (write.readyCycle > m_cycle)
        {
            nextReady = std::min(nextReady, write.readyCycle);
            continue;
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_vfLatestWrite[write.reg][component] == write.sequence)
            {
                m_state.vf[write.reg][component] = write.value[component];
            }
        }
        write = {};
        m_vfWriteValidMask &= ~(1u << index);
    }

    for (uint32_t pending = m_viWriteValidMask; pending != 0u; pending &= pending - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(pending));
        PendingViWrite &write = m_viWritePipeline[index];
        if (write.readyCycle > m_cycle)
        {
            nextReady = std::min(nextReady, write.readyCycle);
            continue;
        }
        if (m_viLatestWrite[write.reg] == write.sequence)
            m_state.vi[write.reg] = static_cast<int16_t>(write.value);
        write = {};
        m_viWriteValidMask &= ~(1u << index);
    }

    for (uint32_t pending = m_accWriteValidMask; pending != 0u; pending &= pending - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(pending));
        PendingAccWrite &write = m_accWritePipeline[index];
        if (write.readyCycle > m_cycle)
        {
            nextReady = std::min(nextReady, write.readyCycle);
            continue;
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_accLatestWrite[component] == write.sequence)
            {
                m_state.acc[component] = write.value[component];
            }
        }
        write = {};
        m_accWriteValidMask &= ~(1u << index);
    }
    m_nextCommitCycle = nextReady;
}


template <class D>
void VuCore<D>::advanceTo(uint64_t targetCycle)
{
    while (m_cycle < targetCycle)
        advanceOneCycle();
}

template <class D>
bool VuCore<D>::pipelinesPending() const
{
    if (m_fdiv.valid)
        return true;
    if constexpr (isVu1())
    {
        if (derived().m_xgkick.active)
            return true;
    }
    // VB1: writes committed at issue still count until their readyCycle.
    if (m_cycle < m_directPendingUntil)
        return true;
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            return true;
    return (m_flagValidMask | m_storeValidMask | m_vfWriteValidMask |
            m_viWriteValidMask | m_accWriteValidMask) != 0u;
}

template <class D>
void VuCore<D>::flushPipelines()
{
    while (pipelinesPending())
        advanceOneCycle();
}

template <class D>
typename VuCore<D>::InstructionUsage VuCore<D>::decodeUpperUsage(uint32_t upper) const
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

template <class D>
typename VuCore<D>::InstructionUsage VuCore<D>::decodeLowerUsage(uint32_t lower) const
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
        if (!isVu1())
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
        if (!isVu1())
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
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
    case 0x7E:
        // E53: EFU slots per PCSX2 LowerOP_T3 (0x78 ESQRT, 0x79 ERSQRT, 0x7A ERCPR,
        // 0x7C ESIN, 0x7D EATAN, 0x7E EEXP; 0x77 is undefined).
        if (!isVu1())
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
        case 0x79: // ERSQRT
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7D: // EATAN
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78: // ESQRT
        case 0x7A: // ERCPR
            usage.latency = 12u;
            break;
        case 0x7C: // ESIN
            usage.latency = 29u;
            break;
        case 0x7E: // EEXP
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
        if (!isVu1())
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

template <class D>
typename VuCore<D>::DecodedInstructionPair VuCore<D>::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
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

template <class D>
void VuCore<D>::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
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

template <class D>
const typename VuCore<D>::DecodedInstructionPair &VuCore<D>::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
        return m_decodeScratch = decodeInstructionPair(vuCode, pc);

    const bool trackedVu1Code = memory != nullptr &&
                                vuCode == (isVu1() ? memory->getVU1Code() : memory->getVU0Code());
    if (!trackedVu1Code)
        return m_decodeScratch = decodeInstructionPair(vuCode, pc);

    const uint64_t generation = isVu1() ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
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
        return m_decodeScratch = decodeInstructionPair(vuCode, pc);
    return m_decodedCodeCache[pairIndex];
}

template <class D>
void VuCore<D>::reportReservedInstruction(bool upper, uint32_t instruction)
{
    RUNTIME_ERROR(
        "[VU" << (isVu1() ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << m_cycle
              << " pc=0x" << std::hex << m_state.pc
              << " instruction=0x" << instruction
              << std::dec << '\n');
    m_stopRequested = true;
}


template <class D>
void VuCore<D>::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeGs = &gs;
    m_activeMemory = memory;

    // LX1d: E53 scopes manage MXCSR only, but glibc fegetround() reads
    // the x87 word (b4cb476) — save/restore via fenv reset MXCSR RC to
    // nearest on x86 at VU1 first run (tick-94 host split). Use the same
    // control word the scopes use.
    const uint64_t previousControl = ps2_fpmode::readControl();
    ps2_fpmode::writeControl(ps2_fpmode::ps2Control(previousControl));
    const uint64_t budgetEnd = m_cycle + maxCycles;
    const uint64_t entryCycle = m_cycle;
    RunContext ctx;
    ctx.vuCode = vuCode;
    ctx.codeSize = codeSize;
    ctx.vuData = vuData;
    ctx.dataSize = dataSize;
    ctx.gs = &gs;
    ctx.memory = memory;
    ctx.budgetEnd = budgetEnd;
    ctx.programEnded = false;
    // VR1: generated code for this code image, if one was compiled in (VX2:
    // VU0 only; for VU1 the lookup only keys the tracked direct-commit map).
    const RecompProgram *recomp = lookupRecompProgram(vuCode, codeSize, memory);
    // VBK1: the block table (same pcs; leaders run a group of pairs per call).
    // Part 2: the guarded group table (mode 2), same pcs again.
    const int blocksMode = recomp == nullptr ? 0 : m_blocksOverride >= 0 ? m_blocksOverride : vu0BlocksMode();
    const RecompPairFn *recompPairs = recomp == nullptr ? nullptr
                                      : blocksMode == 2 && recomp->groupPairs != nullptr ? recomp->groupPairs
                                      : blocksMode >= 1 && recomp->blockPairs != nullptr ? recomp->blockPairs
                                                                                         : recomp->pairs;
    // VB1: direct commit (VU1 always; VX2 dropped PS2X_VU1_DIRECT, whose
    // default was on). VR3: VU0 behind its own knob (PS2X_VU0_DIRECT=1;
    // default off).
    constexpr bool vu1 = isVu1();
    m_directRunOk = m_directOverride >= 0 ? m_directOverride != 0 : vu1 || vu0DirectEnabled();
    m_directFlagSafe = m_directRunOk
                           ? directFlagMap(vuCode, codeSize,
                                           memory != nullptr &&
                                               vuCode == (vu1 ? memory->getVU1Code() : memory->getVU0Code()))
                           : nullptr;
    while (m_cycle < budgetEnd && !m_stopRequested)
    {
        commitReadyPipelines();
        if (m_state.pc + 8u > codeSize)
            break;

        if (recomp != nullptr && (m_state.pc & 7u) == 0u && (m_state.pc >> 3) < recomp->pairCount)
        {
            if (const RecompPairFn fn = recompPairs[m_state.pc >> 3])
            {
                const uint64_t startCycle = m_cycle;
                const bool stop = fn(derived(), ctx);
                m_recompCycles += m_cycle - startCycle;
                if (stop)
                    break;
                continue;
            }
        }

        const uint64_t interpStartCycle = m_cycle;
        const DecodedInstructionPair &decoded = getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
        if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
        {
            reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
            break;
        }

        const bool stop = issuePair<false>(decoded, ctx);
        m_interpCycles += m_cycle - interpStartCycle;
        if (stop)
            break;
    }
    const bool programEnded = ctx.programEnded;

    // VR2 2C: an error stop (reportReservedInstruction: reserved op, a full
    // queue, PATH1 overflow) drains the pipelines like a program end. Before,
    // writes still in flight at the stop never landed (resume() does nothing
    // until execute() resets the scheduler, which drops the queue), so the
    // direct-commit path (VB1), which applies them at issue, was visible where
    // the queued model was not. Draining lands them in every mode; the cycle
    // count agrees because m_directPendingUntil tracks direct landings exactly.
    if (m_stopRequested && !programEnded)
        flushPipelines();
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
    m_state.cycles = m_cycle;
    ps2_fpmode::writeControl(previousControl);
}

// VX1: one engine instance per unit; the members defined in this file.
template class VuCore<VU0Interpreter>;
template class VuCore<VU1Interpreter>;
