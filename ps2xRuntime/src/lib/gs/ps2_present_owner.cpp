// PSO1: shared present-surface ownership (see ps2_present_owner.h).
#include "runtime/gs/ps2_present_owner.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace ps2x_present_own
{
const char *stateName(SlotState s)
{
    switch (s)
    {
    case SlotState::Free: return "FREE";
    case SlotState::Producing: return "PRODUCING";
    case SlotState::Ready: return "READY";
    case SlotState::Current: return "CURRENT";
    case SlotState::Retiring: return "RETIRING";
    }
    return "?";
}

Pool::Pool(int slots, size_t ringCapacity) : m_slots(static_cast<size_t>(std::max(1, slots))), m_ring(ringCapacity)
{
    m_c.generation = m_gen;
}

void Pool::freeLocked(Slot &s, uint64_t nowNs)
{
    s.t.freeNs = nowNs;
    if (!m_ring.empty() && s.t.acquireNs != 0u)
    {
        m_ring[m_ringNext] = s.t;
        m_ringNext = (m_ringNext + 1u) % m_ring.size();
        if (m_ringNext == 0u)
            m_ringFull = true;
    }
    s.state = SlotState::Free;
    s.info = FrameInfo{};
    s.lastReadFence = 0u;
    s.t = FrameTimes{};
}

int Pool::reserve(uint64_t *gen, uint64_t nowNs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = 0; i < m_slots.size(); ++i)
    {
        Slot &s = m_slots[i];
        if (s.state != SlotState::Free)
            continue;
        s.state = SlotState::Producing;
        s.gen = m_gen;
        s.lastReadFence = 0u;
        s.t = FrameTimes{};
        s.t.reserveNs = nowNs;
        ++m_c.reserves;
        if (gen)
            *gen = m_gen;
        return static_cast<int>(i);
    }
    // No FREE slot: drop this export (NoSlot). The READY frame is kept for
    // the presenter: reclaiming it (PSO1 first cut) left the 60 Hz presenter
    // with only a PRODUCING slot at acquire time under 120 exports/s, so it
    // repeated CURRENT (iPad: 0.739 unique presents vs 1.000 legacy).
    ++m_c.noSlot;
    return -1;
}

bool Pool::complete(int slot, uint64_t gen, bool ok, const FrameInfo &info, uint64_t nowNs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (slot < 0 || static_cast<size_t>(slot) >= m_slots.size() || m_slots[slot].state != SlotState::Producing ||
        m_slots[slot].gen != gen)
    {
        ++m_c.unsafe; // completion for a slot the producer does not own
        return false;
    }
    Slot &s = m_slots[slot];
    s.t.completeNs = nowNs;
    if (!ok)
    {
        ++m_c.completesFailed;
        freeLocked(s, nowNs);
        return false;
    }
    if (gen != m_gen)
    {
        ++m_c.staleGen;
        freeLocked(s, nowNs);
        return false;
    }
    if (info.seq <= m_newestPublished)
    {
        ++m_c.outOfOrder; // an older export finished after a newer one
        freeLocked(s, nowNs);
        return false;
    }
    // Supersede any unacquired READY frame (never CURRENT or RETIRING).
    for (Slot &o : m_slots)
    {
        if (&o != &s && o.state == SlotState::Ready)
        {
            ++m_c.superseded;
            freeLocked(o, nowNs);
        }
    }
    s.info = info;
    s.t.seq = info.seq;
    s.t.tick = info.tick;
    s.state = SlotState::Ready;
    m_newestPublished = info.seq;
    ++m_c.completesOk;
    return true;
}

Acquire Pool::acquire(FrameInfo *out, int *slot, uint64_t nowNs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    int ready = -1;
    for (size_t i = 0; i < m_slots.size(); ++i)
    {
        if (m_slots[i].state == SlotState::Ready &&
            (ready < 0 || m_slots[i].info.seq > m_slots[static_cast<size_t>(ready)].info.seq))
            ready = static_cast<int>(i);
    }
    if (ready >= 0)
    {
        // Retire the old CURRENT only now that the replacement is held.
        if (m_current >= 0)
        {
            Slot &old = m_slots[static_cast<size_t>(m_current)];
            if (old.lastReadFence != 0u && old.lastReadFence > m_completedFence)
                old.state = SlotState::Retiring;
            else
            {
                ++m_c.retiredImmediate;
                freeLocked(old, nowNs);
            }
        }
        Slot &s = m_slots[static_cast<size_t>(ready)];
        s.state = SlotState::Current;
        s.t.acquireNs = nowNs;
        m_current = ready;
        ++m_c.acquires;
        if (out)
            *out = s.info;
        if (slot)
            *slot = ready;
        return Acquire::New;
    }
    if (m_current >= 0)
    {
        ++m_c.repeats;
        if (out)
            *out = m_slots[static_cast<size_t>(m_current)].info;
        if (slot)
            *slot = m_current;
        return Acquire::Repeat;
    }
    if (slot)
        *slot = -1;
    return Acquire::None;
}

void Pool::noteRead(uint64_t fence, uint64_t nowNs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_current < 0)
    {
        ++m_c.unsafe; // a read with no held frame
        return;
    }
    Slot &s = m_slots[static_cast<size_t>(m_current)];
    s.lastReadFence = std::max(s.lastReadFence, fence);
    if (s.t.firstReadNs == 0u)
        s.t.firstReadNs = nowNs;
    s.t.lastReadNs = nowNs;
    ++s.t.reads;
    ++m_c.reads;
}

void Pool::retireReadsThrough(uint64_t fence, uint64_t nowNs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (fence <= m_completedFence)
        return;
    m_completedFence = fence;
    for (Slot &s : m_slots)
    {
        if (s.state == SlotState::Retiring && s.lastReadFence <= fence)
        {
            ++m_c.retired;
            freeLocked(s, nowNs);
        }
    }
}

void Pool::dropCurrent(uint64_t nowNs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_current < 0)
        return;
    Slot &old = m_slots[static_cast<size_t>(m_current)];
    if (old.lastReadFence != 0u && old.lastReadFence > m_completedFence)
        old.state = SlotState::Retiring;
    else
    {
        ++m_c.retiredImmediate;
        freeLocked(old, nowNs);
    }
    m_current = -1;
}

void Pool::bumpGeneration()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_gen;
    m_c.generation = m_gen;
    for (Slot &s : m_slots)
    {
        if (s.state == SlotState::Ready && s.gen != m_gen)
        {
            ++m_c.superseded;
            freeLocked(s, 0u);
        }
    }
}

bool Pool::hasFrame() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_current >= 0)
        return true;
    for (const Slot &s : m_slots)
        if (s.state == SlotState::Ready)
            return true;
    return false;
}

SlotState Pool::state(int slot) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slots.at(static_cast<size_t>(slot)).state;
}

uint64_t Pool::slotSeq(int slot) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slots.at(static_cast<size_t>(slot)).info.seq;
}

uint64_t Pool::completedFence() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_completedFence;
}

Counters Pool::counters() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_c;
}

std::vector<FrameTimes> Pool::times() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<FrameTimes> out;
    if (m_ringFull)
        out.insert(out.end(), m_ring.begin() + static_cast<std::ptrdiff_t>(m_ringNext), m_ring.end());
    out.insert(out.end(), m_ring.begin(), m_ring.begin() + static_cast<std::ptrdiff_t>(m_ringNext));
    return out;
}

Pool &sharedPool()
{
    static Pool pool(kSharedSlots);
    return pool;
}

namespace
{
constexpr int kWitnessSlots = 8;
std::atomic<uint64_t> g_witness[kWitnessSlots];
} // namespace

void witnessWrite(int slot)
{
    if (slot >= 0 && slot < kWitnessSlots)
        g_witness[slot].fetch_add(1u, std::memory_order_acq_rel);
}

uint64_t witnessWrites(int slot)
{
    return (slot >= 0 && slot < kWitnessSlots) ? g_witness[slot].load(std::memory_order_acquire) : 0u;
}

uint64_t percentile(std::vector<uint64_t> v, double p)
{
    if (v.empty())
        return 0u;
    const size_t rank = static_cast<size_t>(std::ceil(p * static_cast<double>(v.size())));
    const size_t idx = std::min(v.size() - 1u, rank == 0u ? 0u : rank - 1u);
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx), v.end());
    return v[idx];
}
} // namespace ps2x_present_own
