#include "MiniTest.h"
#include "runtime/gs/ps2_present_owner.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <random>
#include <thread>
#include <vector>

// PSO1 (PRV1 §3): shared present-surface ownership. A fake "pixel store"
// records which content id each slot holds; the producer writes it only
// after reserve() returns the slot, so a consumer whose acquired content id
// changes before its read retires has seen a reused frame.
namespace
{
using ps2x_present_own::Acquire;
using ps2x_present_own::FrameInfo;
using ps2x_present_own::Pool;
using ps2x_present_own::SlotState;

struct Producer
{
    Pool &pool;
    std::vector<uint64_t> pixels; // content id per slot
    uint64_t seq = 0;
    explicit Producer(Pool &p) : pool(p), pixels(static_cast<size_t>(p.slotCount()), 0u) {}
    // Reserve + write + complete. Returns the slot or -1.
    int produce(uint64_t now, bool ok = true, uint32_t w = 640, uint32_t h = 480)
    {
        uint64_t gen = 0;
        const int slot = pool.reserve(&gen, now);
        if (slot < 0)
            return -1;
        ++seq;
        pixels[static_cast<size_t>(slot)] = seq;
        FrameInfo f;
        f.surface = reinterpret_cast<void *>(static_cast<uintptr_t>(0x1000 + slot));
        f.width = w;
        f.height = h;
        f.seq = seq;
        f.tick = seq * 2;
        pool.complete(slot, gen, ok, f, now + 1);
        return slot;
    }
};
} // namespace

void register_ps2_present_owner_tests()
{
    MiniTest::Case("Ps2PresentOwner", [](TestCase &tc)
                   {
        tc.Run("produce, acquire, repeat; CURRENT is never reserved", [](TestCase &t)
               {
            Pool pool(3);
            Producer p(pool);
            FrameInfo f;
            int slot = -1;
            t.IsTrue(pool.acquire(&f, &slot, 1) == Acquire::None, "nothing yet");
            const int s1 = p.produce(10);
            t.IsTrue(s1 >= 0, "reserved");
            t.IsTrue(pool.state(s1) == SlotState::Ready, "READY after completion");
            t.IsTrue(pool.acquire(&f, &slot, 20) == Acquire::New && slot == s1 && f.seq == 1u, "acquired seq 1");
            t.IsTrue(pool.state(s1) == SlotState::Current, "CURRENT");
            pool.noteRead(1, 21);
            pool.retireReadsThrough(1, 22);
            t.IsTrue(pool.state(s1) == SlotState::Current, "a completed read does not release CURRENT");
            t.IsTrue(pool.acquire(&f, &slot, 30) == Acquire::Repeat && f.seq == 1u, "repeat");
            for (int i = 0; i < 10; ++i)
            {
                const int s = p.produce(40 + i);
                t.IsTrue(s != s1, "producer never takes the CURRENT slot");
            }
            t.IsTrue(p.pixels[static_cast<size_t>(s1)] == 1u, "CURRENT pixels untouched");
            t.Equals(pool.counters().unsafe, 0ull, "no unsafe requests"); });

        tc.Run("delayed consumer never sees its frame reused (PRV1 §2 schedule)", [](TestCase &t)
               {
            Pool pool(3);
            Producer p(pool);
            FrameInfo f;
            int slot = -1;
            p.produce(1);
            pool.acquire(&f, &slot, 2);          // acquire A
            const uint64_t acquired = f.seq;
            for (int i = 0; i < 50; ++i)          // consumer stalls before submitting its draw
                p.produce(3 + i);
            t.IsTrue(p.pixels[static_cast<size_t>(slot)] == acquired, "A's pixels intact at draw time");
            pool.noteRead(7, 60);                 // draw submitted with fence 7
            // a newer frame replaces A while A's read is outstanding
            t.IsTrue(pool.acquire(&f, &slot, 61) == Acquire::New, "newer frame");
            const int sB = slot;
            int retiringSlot = -1;
            for (int i = 0; i < pool.slotCount(); ++i)
                if (pool.state(i) == SlotState::Retiring)
                    retiringSlot = i;
            t.IsTrue(retiringSlot >= 0, "A is RETIRING until its fence completes");
            for (int i = 0; i < 20; ++i)
                p.produce(70 + i);
            t.IsTrue(p.pixels[static_cast<size_t>(retiringSlot)] == acquired, "A's pixels intact until fence 7");
            pool.retireReadsThrough(6, 100);
            t.IsTrue(pool.state(retiringSlot) == SlotState::Retiring, "fence 6 does not cover read 7");
            pool.retireReadsThrough(7, 101);
            t.IsTrue(pool.state(retiringSlot) == SlotState::Free, "fence 7 frees A");
            t.IsTrue(pool.state(sB) == SlotState::Current, "B still CURRENT");
            t.Equals(pool.counters().unsafe, 0ull, "no unsafe requests"); });

        tc.Run("exhaustion: CURRENT + RETIRING + READY leaves no slot; READY is superseded", [](TestCase &t)
               {
            Pool pool(3);
            Producer p(pool);
            FrameInfo f;
            int slot = -1;
            p.produce(1);
            pool.acquire(&f, &slot, 2);
            pool.noteRead(1, 3);
            p.produce(4);
            pool.acquire(&f, &slot, 5); // first -> RETIRING (fence 1 outstanding)
            pool.noteRead(2, 6);
            const int third = p.produce(7);
            t.IsTrue(third >= 0, "third slot READY");
            t.IsTrue(p.produce(8) == third, "no FREE slot: the unacquired READY frame is reclaimed");
            t.Equals(pool.counters().reclaimed, 1ull, "one reclaimed");
            FrameInfo shown;
            int shownSlot = -1;
            // CURRENT + RETIRING + PRODUCING (export in flight) -> no slot
            uint64_t g = 0;
            const int inFlight = pool.reserve(&g, 9);
            t.IsTrue(inFlight == third, "READY reclaimed again for the in-flight export");
            const uint64_t before = pool.counters().noSlot;
            t.IsTrue(p.produce(10) < 0, "no FREE or READY slot");
            t.Equals(pool.counters().noSlot, before + 1ull, "noSlot counted");
            t.IsTrue(pool.acquire(&shown, &shownSlot, 11) == Acquire::Repeat, "CURRENT still shown");
            pool.retireReadsThrough(1, 12);
            t.IsTrue(p.produce(13) >= 0, "retired slot reusable"); });

        tc.Run("acquire without a read frees the old CURRENT at once", [](TestCase &t)
               {
            Pool pool(3);
            Producer p(pool);
            FrameInfo f;
            int slot = -1;
            p.produce(1);
            pool.acquire(&f, &slot, 2);
            const int first = slot;
            p.produce(3);
            pool.acquire(&f, &slot, 4);
            t.IsTrue(pool.state(first) == SlotState::Free, "never-read CURRENT -> FREE");
            t.Equals(pool.counters().retiredImmediate, 1ull, "counted"); });

        tc.Run("failed export and stale generation free the slot; teardown keeps CURRENT", [](TestCase &t)
               {
            Pool pool(3);
            Producer p(pool);
            FrameInfo f;
            int slot = -1;
            const int bad = p.produce(1, false);
            t.IsTrue(pool.state(bad) == SlotState::Free, "failed -> FREE");
            p.produce(2);
            pool.acquire(&f, &slot, 3);
            pool.noteRead(1, 4);
            const int cur = slot;
            // one export in flight across a backend teardown, one READY
            uint64_t genInFlight = 0;
            const int inFlight = pool.reserve(&genInFlight, 5);
            p.produce(6); // READY of the old generation
            pool.bumpGeneration();
            FrameInfo late;
            late.seq = 99;
            late.surface = reinterpret_cast<void *>(0x9999);
            t.IsFalse(pool.complete(inFlight, genInFlight, true, late, 7), "late completion not published");
            t.IsTrue(pool.state(inFlight) == SlotState::Free, "stale -> FREE");
            t.Equals(pool.counters().staleGen, 1ull, "stale counted");
            t.IsTrue(pool.acquire(&f, &slot, 8) == Acquire::Repeat && slot == cur,
                     "old READY never shown; CURRENT repeats across teardown");
            // resize: the next generation's frame (new size) replaces CURRENT
            const int s2 = p.produce(9, true, 2360, 1328);
            t.IsTrue(s2 >= 0, "new-generation export");
            t.IsTrue(pool.acquire(&f, &slot, 10) == Acquire::New && f.width == 2360u, "resized frame");
            t.IsTrue(pool.state(cur) == SlotState::Retiring, "old-size frame waits for its read");
            pool.retireReadsThrough(1, 11);
            t.IsTrue(pool.state(cur) == SlotState::Free, "then FREE");
            t.Equals(pool.counters().unsafe, 0ull, "no unsafe requests"); });

        tc.Run("out-of-order completion keeps the newest frame", [](TestCase &t)
               {
            Pool pool(3);
            uint64_t g1 = 0, g2 = 0;
            const int a = pool.reserve(&g1, 1);
            const int b = pool.reserve(&g2, 2);
            FrameInfo fa, fb;
            fa.seq = 1;
            fb.seq = 2;
            t.IsTrue(pool.complete(b, g2, true, fb, 3), "newer completes first");
            t.IsFalse(pool.complete(a, g1, true, fa, 4), "older dropped");
            t.Equals(pool.counters().outOfOrder, 1ull, "counted");
            FrameInfo f;
            int slot = -1;
            pool.acquire(&f, &slot, 5);
            t.IsTrue(f.seq == 2u, "newest shown"); });

        tc.Run("illegal requests are counted, not obeyed", [](TestCase &t)
               {
            Pool pool(3);
            FrameInfo f;
            t.IsFalse(pool.complete(0, 1, true, f, 1), "complete on a FREE slot");
            pool.noteRead(1, 2); // no CURRENT
            t.Equals(pool.counters().unsafe, 2ull, "two unsafe requests"); });

        tc.Run("threaded stress: delayed consumer + lagging fences, zero reuse", [](TestCase &t)
               {
            Pool pool(3);
            std::vector<std::atomic<uint64_t>> pixels(3);
            for (auto &px : pixels)
                px.store(0u);
            std::atomic<bool> stop{false};
            std::atomic<uint64_t> produced{0}, violations{0};
            std::thread producer([&] {
                std::mt19937 rng(1);
                uint64_t seq = 0;
                while (!stop.load())
                {
                    uint64_t gen = 0;
                    const int slot = pool.reserve(&gen, 0);
                    if (slot < 0)
                    {
                        std::this_thread::yield();
                        continue;
                    }
                    ++seq;
                    pixels[static_cast<size_t>(slot)].store(seq); // "Metal write"
                    if (rng() % 4u == 0u)
                        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 200u));
                    FrameInfo f;
                    f.seq = seq;
                    pool.complete(slot, gen, rng() % 50u != 0u, f, 0);
                    produced.fetch_add(1u);
                    if (rng() % 500u == 0u)
                        pool.bumpGeneration();
                }
            });
            std::mt19937 rng(2);
            struct Read
            {
                uint64_t fence;
                int slot;
                uint64_t seq;
            };
            std::deque<Read> outstanding; // simulated GL stream
            uint64_t fence = 0, reads = 0;
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            while (std::chrono::steady_clock::now() < end)
            {
                // fences complete late and in order
                while (!outstanding.empty() && rng() % 3u == 0u)
                {
                    const Read r = outstanding.front();
                    if (pixels[static_cast<size_t>(r.slot)].load() != r.seq)
                        violations.fetch_add(1u); // overwritten before the read completed
                    outstanding.pop_front();
                    pool.retireReadsThrough(r.fence, 0);
                }
                FrameInfo f;
                int slot = -1;
                if (pool.acquire(&f, &slot, 0) == Acquire::None)
                    continue;
                if (pixels[static_cast<size_t>(slot)].load() != f.seq)
                    violations.fetch_add(1u);
                if (rng() % 5u == 0u) // delay between acquisition and draw submission
                    std::this_thread::sleep_for(std::chrono::microseconds(rng() % 500u));
                if (pixels[static_cast<size_t>(slot)].load() != f.seq)
                    violations.fetch_add(1u);
                pool.noteRead(++fence, 0);
                outstanding.push_back({fence, slot, f.seq});
                ++reads;
            }
            stop.store(true);
            producer.join();
            t.Equals(violations.load(), 0ull, "no frame reused while held or read");
            t.Equals(pool.counters().unsafe, 0ull, "no unsafe requests");
            t.IsTrue(produced.load() > 100u && reads > 100u, "stress made progress"); });

        tc.Run("legacy first-free-slot mailbox reproduces the PRV1 §2 reuse (model)", [](TestCase &t)
               {
            // Pre-PSO1 protocol: busy cleared at completion, mailbox = metadata copy.
            bool busy[3] = {false, false, false};
            uint64_t pixels[3] = {0, 0, 0};
            uint64_t seq = 0;
            auto produce = [&]() {
                for (int i = 0; i < 3; ++i)
                    if (!busy[i])
                    {
                        busy[i] = true;
                        pixels[i] = ++seq;
                        busy[i] = false; // completion: publish, then clear busy
                        return i;
                    }
                return -1;
            };
            const int a = produce();       // publish A in slot 0
            const uint64_t acquired = pixels[a]; // latest() copies A's metadata
            produce();                      // next export claims the first free slot: slot 0 again
            t.IsTrue(pixels[a] != acquired, "legacy: A overwritten before its draw"); }); });
}
