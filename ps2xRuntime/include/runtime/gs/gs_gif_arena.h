#ifndef PS2_GS_GIF_ARENA_H
#define PS2_GS_GIF_ARENA_H

// GSB2 (PS2X_GS_GIF_ARENA=1, default off): zero-copy GIF batch transport.
//
// GSB1 copies every packet twice: copyForSubmit copies it out of VU1 memory
// into a pooled per-packet vector (the next job rewrites VU1 memory, so one
// copy is unavoidable), and the batch append copies it again into a fresh
// 64 KiB batch vector. The arena makes the first copy the only copy:
// copyForSubmit appends straight into the current batch arena, a pooled
// 64 KiB buffer, and the GIF-stage op, the arbiter packet and the batch
// sub-packet table carry views (arena + offset + length) instead of vectors.
// The worker runs each view through today's per-packet code in drain order,
// so the GE1 call sequence, the capture stream, the consumed-stream digest
// and guest state are unchanged: exact by construction.
//
// Lifetime: arenas are appended on the MTVU thread, viewed from the GIF
// stage ring and the arbiter drain on the MTVU-GIF thread, and read on the
// GS worker. Ownership is an intrusive refcount (GsGifArenaRef is the RAII
// holder: copy addrefs, move transfers, destruction releases); the last
// release returns the arena to its pool. Each arena holds its pool weakly:
// a last release rejoins a live pool and frees to a dead one, so releases
// stay valid at any teardown point with no ordering requirements and no
// retain cycle. The arena bytes are append-only from one thread into a fixed
// buffer (never reallocated), and every reader only touches bytes committed
// before its own view, so published prefixes are immutable while later
// appends land on disjoint ranges.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class GsGifArenaPool;

// One pooled batch arena: a fixed 64 KiB payload with an intrusive refcount.
// Default-constructed with one ref (the acquirer's); pooled arenas are reset
// to one ref on reissue. Plain struct, no virtuals.
struct GsGifArena
{
    static constexpr size_t kBytes = 64u * 1024u;

    std::atomic<uint32_t> refs{1u};
    std::weak_ptr<GsGifArenaPool> pool; // rejoined at the last release when live
    uint8_t bytes[kBytes];
};

// RAII holder for one arena ref. Copy addrefs (atomic, uncontended: each ref
// is touched by one thread at a time as views flow MTVU -> MTVU-GIF ->
// worker), move transfers, destruction releases back to the pool.
class GsGifArenaRef
{
public:
    GsGifArenaRef() = default;
    GsGifArenaRef(const GsGifArenaRef &other) : m_arena(other.m_arena)
    {
        addRef();
    }
    GsGifArenaRef(GsGifArenaRef &&other) noexcept : m_arena(other.m_arena)
    {
        other.m_arena = nullptr;
    }
    GsGifArenaRef &operator=(const GsGifArenaRef &other)
    {
        if (this != &other)
        {
            release();
            m_arena = other.m_arena;
            addRef();
        }
        return *this;
    }
    GsGifArenaRef &operator=(GsGifArenaRef &&other) noexcept
    {
        if (this != &other)
        {
            release();
            m_arena = other.m_arena;
            other.m_arena = nullptr;
        }
        return *this;
    }
    ~GsGifArenaRef() { release(); }

    explicit GsGifArenaRef(GsGifArena *adopt) : m_arena(adopt) {} // adopts one ref, no addref
    void reset()
    {
        release();
        m_arena = nullptr;
    }
    GsGifArena *get() const { return m_arena; }
    explicit operator bool() const { return m_arena != nullptr; }

private:
    void addRef()
    {
        if (m_arena)
            m_arena->refs.fetch_add(1u, std::memory_order_relaxed);
    }
    void release(); // defined after GsGifArenaPool (returns the last ref there)

    GsGifArena *m_arena = nullptr;
};

class GsGifArenaPool : public std::enable_shared_from_this<GsGifArenaPool>
{
public:
    // Uniform 64 KiB buffers: an acquire is a stack pop, a release a push.
    // Sized for the in-flight depth: GSB1 measured ~7.4 MB of GIF bytes per
    // Elysium update (~116 arenas of content), and the GS queue caps queued
    // bytes at 16 MiB (256 arenas); a miss allocates a fresh arena that frees
    // at its last release (or joins the pool when space frees), so pressure
    // degrades to allocation, never to a stall.
    static constexpr size_t kMaxArenas = 256u;

    static std::shared_ptr<GsGifArenaPool> create() { return std::make_shared<GsGifArenaPool>(); }

    // Take an arena with one ref (the caller's). Pool hit when one is free,
    // else a fresh arena. Never null.
    GsGifArena *acquire()
    {
        s_acquires.fetch_add(1u, std::memory_order_relaxed);
        Lock lock(m_lock);
        if (!m_free.empty())
        {
            GsGifArena *out = m_free.back();
            m_free.pop_back();
            s_poolHits.fetch_add(1u, std::memory_order_relaxed);
            out->refs.store(1u, std::memory_order_relaxed);
            return out;
        }
        lock.release();
        // No parens: default-init leaves the 64 KiB indeterminate (a
        // value-init would memset it; every byte is overwritten by appends).
        GsGifArena *fresh = new GsGifArena;
        fresh->pool = shared_from_this();
        return fresh;
    }

    // Return a last-ref arena (called only at refcount zero). Rejoins a
    // live pool when there is room, else freed (a dead pool frees too). Any
    // thread may call; the locked self keeps a live pool alive through it.
    static void releaseRef(GsGifArena *arena)
    {
        if (std::shared_ptr<GsGifArenaPool> self = arena->pool.lock())
        {
            Lock lock(self->m_lock);
            if (self->m_free.size() < kMaxArenas)
            {
                self->m_free.push_back(arena);
                return;
            }
        }
        delete arena;
    }

    size_t pooledCount() const
    {
        Lock lock(m_lock);
        return m_free.size();
    }

    // Receipts (process-wide, like GsWorker's): arenas issued and pool hits.
    static uint64_t acquireCount() { return s_acquires.load(std::memory_order_relaxed); }
    static uint64_t poolHitCount() { return s_poolHits.load(std::memory_order_relaxed); }

    ~GsGifArenaPool()
    {
        for (GsGifArena *arena : m_free)
            delete arena;
    }

private:
    struct Lock
    {
        explicit Lock(std::atomic_flag &flag) : m_flag(flag), m_owns(true)
        {
            while (m_flag.test_and_set(std::memory_order_acquire))
            {
                // Brief critical sections, 2-3 threads; spin, don't sleep.
            }
        }
        void release()
        {
            if (m_owns)
            {
                m_owns = false;
                m_flag.clear(std::memory_order_release);
            }
        }
        ~Lock() { release(); }
        Lock(const Lock &) = delete;
        Lock &operator=(const Lock &) = delete;
        std::atomic_flag &m_flag;
        bool m_owns;
    };

    mutable std::atomic_flag m_lock; // C++20: default-constructs clear
    std::vector<GsGifArena *> m_free; // guarded by m_lock

    inline static std::atomic<uint64_t> s_acquires{0u};
    inline static std::atomic<uint64_t> s_poolHits{0u};
};

inline void GsGifArenaRef::release()
{
    if (!m_arena)
        return;
    GsGifArena *arena = m_arena;
    m_arena = nullptr;
    if (arena->refs.fetch_sub(1u, std::memory_order_acq_rel) == 1u)
        GsGifArenaPool::releaseRef(arena);
}

#endif
