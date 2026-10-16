#pragma once

// snd-seam-1 family A: the generic host-audio PCM handoff. This ring used to
// live in the SSX 3 SND header (`ps2_snd_spike.h`, now the game module's);
// the class below is byte-identical, only the namespace changed. One
// EE-thread producer (today the SND tick mix) and one miniaudio callback
// consumer. Packed atomic frames avoid data races when the producer
// overwrites an old slot on overflow.
//
// NOTE: `push()` documents the SND tag-1 planar layout and has no production
// caller left (the tick mix uses `pushFrame`); it is a delete-batch
// candidate, kept here verbatim so this move stays reviewably identical.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ps2_pcm_ring
{

inline constexpr uint32_t kPcmRingFrames = 1u << 15;

// One EE-thread producer and one miniaudio callback consumer. Packed atomic
// frames avoid data races when the producer overwrites an old slot on overflow.
class PcmRing
{
public:
    // The tag-1 payload is planar: 384 s16 for one channel, then 384 for the
    // other (SNDDRV SNDIOP_ee36_iop24_spu48 steps its EE input by 0x300 per
    // channel). PCSX2's SPU2 input carries the first block on the right and
    // the second on the left. The ring holds interleaved L|R frames.
    void push(const uint8_t *pcm, size_t bytes)
    {
        if (!pcm)
            return;
        const size_t frames = bytes / sizeof(uint32_t);
        const uint8_t *left = pcm + frames * sizeof(uint16_t);
        for (size_t i = 0; i < frames; ++i)
        {
            uint16_t l = 0, r = 0;
            std::memcpy(&l, left + i * sizeof(l), sizeof(l));
            std::memcpy(&r, pcm + i * sizeof(r), sizeof(r));
            const uint32_t frame = static_cast<uint32_t>(l) | (static_cast<uint32_t>(r) << 16);
            const uint64_t write = m_write.load(std::memory_order_relaxed);
            uint64_t read = m_read.load(std::memory_order_acquire);
            while (write - read >= kPcmRingFrames)
            {
                if (m_read.compare_exchange_weak(read, read + 1u, std::memory_order_acq_rel))
                {
                    m_overflows.fetch_add(1u, std::memory_order_relaxed);
                    break;
                }
            }
            m_frames[write & (kPcmRingFrames - 1u)].store(frame, std::memory_order_relaxed);
            m_write.store(write + 1u, std::memory_order_release);
        }
    }

    void pushFrame(uint32_t frame)
    {
        const uint64_t write = m_write.load(std::memory_order_relaxed);
        uint64_t read = m_read.load(std::memory_order_acquire);
        while (write - read >= kPcmRingFrames)
        {
            if (m_read.compare_exchange_weak(read, read + 1u, std::memory_order_acq_rel))
            {
                m_overflows.fetch_add(1u, std::memory_order_relaxed);
                break;
            }
        }
        m_frames[write & (kPcmRingFrames - 1u)].store(frame, std::memory_order_relaxed);
        m_write.store(write + 1u, std::memory_order_release);
    }

    bool pop(uint32_t &frame)
    {
        uint64_t read = m_read.load(std::memory_order_acquire);
        if (read >= m_write.load(std::memory_order_acquire))
            return false;
        frame = m_frames[read & (kPcmRingFrames - 1u)].load(std::memory_order_relaxed);
        return m_read.compare_exchange_strong(read, read + 1u, std::memory_order_acq_rel);
    }

    // IP6: lifetime cursors for the perf log's [perf-audio] rates (frames
    // pushed = writeTotal delta; frames consumed = readTotal delta minus
    // overflow drops).
    uint64_t writeTotal() const { return m_write.load(std::memory_order_relaxed); }
    uint64_t readTotal() const { return m_read.load(std::memory_order_relaxed); }
    void noteUnderrun() { m_underruns.fetch_add(1u, std::memory_order_relaxed); }
    uint64_t underruns() const { return m_underruns.load(std::memory_order_relaxed); }
    uint64_t overflows() const { return m_overflows.load(std::memory_order_relaxed); }
    // DS1: drop buffered frames (a mid-session state load: the buffered PCM
    // belonged to the pre-load timeline). The consumer then underruns once
    // and resumes on fresh post-load audio.
    void dropAll() { m_read.store(m_write.load(std::memory_order_acquire), std::memory_order_release); }
    // AT1: frames currently buffered (producer pushes minus consumer pops).
    // Relaxed: the stretch control loop tolerates a slightly stale depth.
    uint64_t size() const
    {
        const uint64_t write = m_write.load(std::memory_order_relaxed);
        const uint64_t read = m_read.load(std::memory_order_relaxed);
        return write >= read ? write - read : 0u;
    }

private:
    std::array<std::atomic<uint32_t>, kPcmRingFrames> m_frames{};
    std::atomic<uint64_t> m_read{0};
    std::atomic<uint64_t> m_write{0};
    std::atomic<uint64_t> m_underruns{0};
    std::atomic<uint64_t> m_overflows{0};
};

inline PcmRing &pcmRing()
{
    static PcmRing ring;
    return ring;
}

} // namespace ps2_pcm_ring
