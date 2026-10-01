#include "runtime/ps2_vu0.h"
#include "runtime/ps2_vu1.h"
#include "ps2_vu_upper_impl.h"

template <class D>
void VuCore<D>::execUpper(uint32_t instr)
{
    execUpperImpl(instr);
}

template <class D>
void VuCore<D>::execUpperForTest(uint32_t instr, bool simd, bool directFlags)
{
    m_directFlags = directFlags;
#if PS2X_VU1_FMAC_SIMD_AVAILABLE
    if (simd)
        execUpperImpl<true>(instr);
    else
        execUpperImpl<false>(instr);
#else
    (void)simd;
    execUpperImpl<false>(instr);
#endif
    m_directFlags = false;
}

template <class D>
std::vector<uint64_t> VuCore<D>::fmacStateForTest() const
{
    std::vector<uint64_t> out;
    const auto bits = [&out](float value)
    {
        uint32_t word = 0u;
        std::memcpy(&word, &value, sizeof(word));
        out.push_back(word);
    };
    for (const auto &row : m_state.vf)
        for (float value : row)
            bits(value);
    for (float value : m_state.acc)
        bits(value);
    bits(m_state.q);
    bits(m_state.p);
    bits(m_state.i);
    out.push_back(m_state.mac);
    out.push_back(m_state.clip);
    out.push_back(m_state.status);
    out.push_back(m_flagValidMask);
    for (const FlagPipelineEntry &entry : m_flagPipeline)
    {
        out.push_back(entry.readyCycle);
        out.push_back(entry.issueCycle);
        out.push_back(entry.mac);
        out.push_back(entry.status);
        out.push_back(entry.extraSticky);
        out.push_back(entry.clip);
        out.push_back((entry.valid ? 1u : 0u) | (entry.writesMac ? 2u : 0u) | (entry.writesStatus ? 4u : 0u) |
                      (entry.writesSticky ? 8u : 0u) | (entry.writesClip ? 16u : 0u) |
                      (entry.writesStickyOr ? 32u : 0u));
    }
    out.push_back(m_nextCommitCycle);
    out.push_back(m_directPendingUntil);
    out.push_back(m_cycle);
    out.push_back(m_stopRequested ? 1u : 0u);
    out.push_back(m_currentUpperInstruction);
    return out;
}

#define PS2X_VU_UPPER_INSTANTIATE(U)                                                  \
    template void VuCore<U>::execUpper(uint32_t);                                     \
    template void VuCore<U>::execUpperForTest(uint32_t, bool, bool);                  \
    template std::vector<uint64_t> VuCore<U>::fmacStateForTest() const;
PS2X_VU_UPPER_INSTANTIATE(VU0Interpreter)
PS2X_VU_UPPER_INSTANTIATE(VU1Interpreter)
#undef PS2X_VU_UPPER_INSTANTIATE
