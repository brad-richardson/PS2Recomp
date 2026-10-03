#pragma once
// PSO1 (PRV1 §3): ownership of the shared present surfaces (iOS GE1 IOSurface
// export -> GLES presenter). Pure C++ bookkeeping, unit-tested on the Mac.
//
// Slot states:
//   FREE      -> PRODUCING  reserve() (producer may write the pixels)
//   PRODUCING -> READY      complete(ok, current generation)
//   PRODUCING -> FREE       complete(failed or stale generation)
//   READY     -> FREE       superseded by a newer READY before acquisition,
//                           or retired by bumpGeneration()
//   READY     -> PRODUCING  reserve() reclaims the oldest unacquired READY
//                           frame when no slot is FREE
//   READY     -> CURRENT    acquire() (presenter's CPU hold)
//   CURRENT   -> CURRENT    repeat draws (noteRead() moves the last-read fence)
//   CURRENT   -> RETIRING   a newer frame is acquired (or dropCurrent())
//   RETIRING  -> FREE       retireReadsThrough(fence >= slot's last read)
// A slot is writable only in FREE: no producer work, no READY reference, no
// current-frame hold and no outstanding GPU read. A completed read does not
// release CURRENT: a paused game draws it again.
//
// Fences are the presenter's GL stream sequence numbers (1, 2, ...); a later
// fence covers every earlier read on the same stream. 0 = no read.
//
// PS2X_PRESENT_OWNERSHIP=1 turns this on (iOS); 0 = the pre-PSO1 mailbox.
#include <cstdint>
#include <mutex>
#include <vector>

namespace ps2x_present_own
{
enum class SlotState : uint8_t
{
    Free,
    Producing,
    Ready,
    Current,
    Retiring
};

const char *stateName(SlotState s);

struct FrameInfo
{
    void *surface = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t seq = 0;          // content sequence (publication order)
    uint64_t tick = 0;         // guest tick
    uint64_t submitWallNs = 0; // export-submit wall (PL3 frame age birth)
};

enum class Acquire
{
    None,   // nothing ever produced (or the current frame was dropped)
    New,    // a newer READY frame became CURRENT
    Repeat, // no newer frame: CURRENT again
};

struct Counters
{
    uint64_t reserves = 0;       // FREE -> PRODUCING
    uint64_t noSlot = 0;         // reserve() found no FREE slot (export dropped)
    uint64_t completesOk = 0;    // PRODUCING -> READY
    uint64_t completesFailed = 0;// PRODUCING -> FREE (export failed)
    uint64_t staleGen = 0;       // PRODUCING -> FREE (older generation)
    uint64_t outOfOrder = 0;     // completion older than the newest READY/CURRENT
    uint64_t superseded = 0;     // READY -> FREE without being acquired
    uint64_t reclaimed = 0;      // of which: READY reclaimed by reserve() (no FREE slot)
    uint64_t acquires = 0;       // READY -> CURRENT
    uint64_t repeats = 0;        // CURRENT drawn again
    uint64_t reads = 0;          // noteRead() calls
    uint64_t retired = 0;        // RETIRING -> FREE
    uint64_t retiredImmediate = 0; // CURRENT -> FREE (no outstanding read)
    uint64_t unsafe = 0;         // illegal transition requests (must stay 0)
    uint64_t generation = 0;
};

// Per-frame timestamps (ns, steady clock), recorded at FREE.
struct FrameTimes
{
    uint64_t seq = 0, tick = 0;
    uint64_t reserveNs = 0, completeNs = 0, acquireNs = 0, firstReadNs = 0, lastReadNs = 0, freeNs = 0;
    uint32_t reads = 0;
};

class Pool
{
public:
    explicit Pool(int slots = 3, size_t ringCapacity = 512);

    int slotCount() const { return static_cast<int>(m_slots.size()); }

    // Producer (any thread). Returns the slot, or -1 (no FREE slot). The
    // generation to hand back to complete() is written to *gen.
    int reserve(uint64_t *gen, uint64_t nowNs);
    // Completion (any thread). Publishes READY when ok and the generation is
    // current; else frees. Returns true if published.
    bool complete(int slot, uint64_t gen, bool ok, const FrameInfo &info, uint64_t nowNs);

    // Presenter thread.
    Acquire acquire(FrameInfo *out, int *slot, uint64_t nowNs);
    // The CURRENT frame's read was submitted and is covered by `fence`.
    void noteRead(uint64_t fence, uint64_t nowNs);
    // Every read with fence <= `fence` has completed on the GPU.
    void retireReadsThrough(uint64_t fence, uint64_t nowNs);
    // The presenter stopped drawing the shared frame: CURRENT -> RETIRING/FREE.
    void dropCurrent(uint64_t nowNs);

    // Teardown / resize / backend replacement: in-flight and READY frames of
    // older generations never become CURRENT. CURRENT stays until replaced.
    void bumpGeneration();

    bool hasFrame() const; // a READY or CURRENT frame exists
    SlotState state(int slot) const;
    uint64_t slotSeq(int slot) const;
    uint64_t completedFence() const;
    Counters counters() const;
    // Copies the timestamp ring (oldest first).
    std::vector<FrameTimes> times() const;

private:
    struct Slot
    {
        SlotState state = SlotState::Free;
        uint64_t gen = 0;
        FrameInfo info;
        uint64_t lastReadFence = 0;
        FrameTimes t;
    };
    void freeLocked(Slot &s, uint64_t nowNs);

    mutable std::mutex m_mutex;
    std::vector<Slot> m_slots;
    uint64_t m_gen = 1;
    uint64_t m_completedFence = 0;
    uint64_t m_newestPublished = 0; // highest seq made READY
    int m_current = -1;
    Counters m_c;
    std::vector<FrameTimes> m_ring;
    size_t m_ringNext = 0;
    bool m_ringFull = false;
};

// The process-wide pool for the iOS IOSurface export (3 slots, PRV1 §3).
Pool &sharedPool();

// Witness (both modes): counts producer writes per slot so the presenter can
// tell whether the pixels it acquired were overwritten before it read them
// (PRV1 §2's schedule). Lock-free; slot < 0 is ignored.
void witnessWrite(int slot);
uint64_t witnessWrites(int slot);

// Percentile helper for the summary line (nearest-rank; 0 when empty).
uint64_t percentile(std::vector<uint64_t> v, double p);
} // namespace ps2x_present_own
