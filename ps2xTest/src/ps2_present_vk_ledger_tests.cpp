#include "MiniTest.h"
#include "runtime/gs/ps2_present_vk_ledger.h"

#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

// VK2: the Android Vulkan present's ownership bookkeeping (RV5 B1, B2, S1)
// driven by a fake SurfaceFlinger that knows which buffers it really holds.
namespace
{
using ps2x_present_vk::Ledger;
using ps2x_present_vk::LayerGeometry;

struct FakeSF final : ps2x_present_vk::Platform
{
    struct Layer
    {
        bool attached = true, released = false;
        void *displayed = nullptr;
    };
    struct Tx
    {
        void *layer;
        void *buffer; // nullptr: detach
        uint64_t token;
    };
    struct Callback
    {
        uint64_t token;
        void *layer;
        bool inStats;
        int fd;
    };
    std::map<void *, Layer> layers;
    std::deque<Tx> pending;
    std::deque<Callback> callbacks;
    std::vector<bool> fenceSignalled;          // by fence id
    std::map<int, int> fds;                    // open fd -> fence id
    std::multimap<void *, int> releasing;      // buffer -> fence id (release not yet signalled)
    std::set<void *> stuck;                    // detachEarlyRelease: removed, reported -1, still scanned out
    std::set<void *> releasedBuffers;
    int nextFd = 100;
    uintptr_t nextLayer = 0x1000, nextBuffer = 0x9000;
    // knobs
    int eintrLeft = 0;
    bool eintrForever = false;
    std::set<int> nvalFences; // fence ids polled as POLLNVAL
    bool detachAbsent = false;      // stats of a detach transaction omit the layer
    bool detachEarlyRelease = false; // detach reports -1 while the buffer's release is still pending
    bool failCreate = false;
    // violations
    int useAfterRelease = 0, releasedWithPending = 0, badClose = 0, doubleRelease = 0, badTokenLayer = 0;

    void *createLayer(void *) override
    {
        if (failCreate)
            return nullptr;
        void *sc = reinterpret_cast<void *>(nextLayer += 16);
        layers[sc] = Layer{};
        return sc;
    }
    void releaseLayer(void *layer) override
    {
        Layer &l = layers[layer];
        for (const Tx &t : pending)
            releasedWithPending += t.layer == layer;
        for (const Callback &c : callbacks)
            releasedWithPending += c.layer == layer;
        l.released = true;
    }
    void applyBuffer(void *layer, void *buffer, const LayerGeometry *, uint64_t token) override
    {
        useAfterRelease += layers[layer].released;
        pending.push_back({layer, buffer, token});
    }
    void applyDetach(void *layer, uint64_t token) override
    {
        useAfterRelease += layers[layer].released;
        pending.push_back({layer, nullptr, token});
    }
    void releaseBuffer(void *buffer) override { doubleRelease += !releasedBuffers.insert(buffer).second; }
    int newFd(int fence)
    {
        fds[nextFd] = fence;
        return nextFd++;
    }
    int dupFd(int fd) override
    {
        const auto it = fds.find(fd);
        return it == fds.end() ? -1 : newFd(it->second);
    }
    void closeFd(int fd) override { badClose += fds.erase(fd) == 0; }
    int pollFd(int fd, int, short &revents, int &err) override
    {
        if (eintrForever || eintrLeft > 0)
        {
            eintrLeft -= eintrLeft > 0;
            err = EINTR;
            return -1;
        }
        const auto it = fds.find(fd);
        if (it == fds.end() || nvalFences.count(it->second))
        {
            revents = POLLNVAL;
            return 1;
        }
        if (!fenceSignalled[it->second])
            return 0;
        revents = POLLIN;
        return 1;
    }

    void *newBuffer() { return reinterpret_cast<void *>(nextBuffer += 64); }
    // SurfaceFlinger latches every pending transaction; the replaced/removed
    // buffer gets an unsignalled release fence; completions are queued.
    void latch()
    {
        while (!pending.empty())
        {
            const Tx t = pending.front();
            pending.pop_front();
            Layer &l = layers[t.layer];
            void *prev = l.displayed;
            if (t.buffer)
                l.displayed = t.buffer;
            else
            {
                l.attached = false;
                l.displayed = nullptr;
            }
            int fd = -1;
            if (prev)
            {
                const int fence = static_cast<int>(fenceSignalled.size());
                fenceSignalled.push_back(false);
                if (t.buffer == nullptr && detachEarlyRelease)
                    stuck.insert(prev); // compositor claims release (-1) but never lets go
                else
                {
                    releasing.emplace(prev, fence);
                    fd = newFd(fence);
                }
            }
            const bool inStats = !(t.buffer == nullptr && detachAbsent);
            if (!inStats && fd >= 0)
            {
                fds.erase(fd);
                fd = -1;
            }
            callbacks.push_back({t.token, t.layer, inStats, fd});
        }
    }
    void deliver(Ledger &l, const Callback &c)
    {
        // The glue reads the layer from the token first: it must still be alive.
        void *sc = l.tokenLayer(c.token);
        badTokenLayer += sc != c.layer || layers[c.layer].released;
        l.complete(c.token, c.inStats, c.fd, 0);
    }
    void deliverAll(Ledger &l)
    {
        while (!callbacks.empty())
        {
            const Callback c = callbacks.front();
            callbacks.pop_front();
            deliver(l, c);
        }
    }
    void signalAll()
    {
        for (size_t i = 0; i < fenceSignalled.size(); ++i)
            fenceSignalled[i] = true;
        releasing.clear();
    }
    void signalRandom(std::mt19937 &rng, double p)
    {
        std::uniform_real_distribution<double> u(0, 1);
        for (auto it = releasing.begin(); it != releasing.end();)
        {
            if (u(rng) < p)
            {
                fenceSignalled[it->second] = true;
                it = releasing.erase(it);
            }
            else
                ++it;
        }
    }
    // Ground truth: SurfaceFlinger (or the display) may still read the buffer.
    bool held(void *buffer) const
    {
        for (const auto &kv : layers)
            if (kv.second.displayed == buffer)
                return true;
        for (const Tx &t : pending)
            if (t.buffer == buffer)
                return true;
        return releasing.count(buffer) != 0 || stuck.count(buffer) != 0;
    }
};

// The backend's use of the ledger (ps2_gs_parallel_backend.cpp presentVk),
// pipelined: queue the previous frame, pick a reusable slot, "blit" into it.
struct SimBackend
{
    Ledger &l;
    FakeSF &sf;
    uint64_t ids[4] = {};
    void *handles[4] = {};
    bool live = false;
    uint32_t epoch = 0;
    int prev = -1, index = 0;
    int heldWrites = 0, writes = 0, skips = 0, pools = 0;

    void create()
    {
        epoch = l.epoch();
        for (int i = 0; i < 4; ++i)
        {
            handles[i] = sf.newBuffer();
            ids[i] = l.registerBuffer(handles[i]);
        }
        live = true;
        prev = -1;
        index = 0;
        ++pools;
    }
    void retire()
    {
        if (!live)
            return;
        for (uint64_t id : ids)
            l.retireBuffer(id);
        live = false;
        prev = -1;
    }
    void present(int timeoutMs = 0)
    {
        if (l.broken())
        {
            retire();
            return;
        }
        if (!live || l.epoch() != epoch)
        {
            retire();
            create();
        }
        if (prev >= 0)
            l.queue(ids[prev], 512, 448);
        prev = -1;
        const Ledger::Pick p = l.pick(ids, 4, -1, index, timeoutMs);
        if (p.index < 0)
        {
            ++skips;
            if (p.giveUp)
                l.fallBack("test: release waits failing");
            return;
        }
        heldWrites += sf.held(handles[p.index]);
        ++writes;
        index = (p.index + 1) % 4;
        prev = p.index;
    }
};

// Two-slot probe helper: queue ids[i] directly (non-pipelined).
bool queueNow(Ledger &l, uint64_t id) { return l.queue(id, 512, 448); }
} // namespace

void register_ps2_present_vk_ledger_tests()
{
    MiniTest::Case("Ps2PresentVkLedger", [](TestCase &tc)
                   {
        tc.Run("B1: a delayed completion keeps the buffer held (no ownership on timeout)", [](TestCase &t)
               {
            FakeSF sf;
            Ledger l(sf);
            l.setLog(false);
            void *win = reinterpret_cast<void *>(0x77);
            l.setWindow(win, 0, 796, 448);
            void *ha = sf.newBuffer(), *hb = sf.newBuffer();
            const uint64_t ids[2] = {l.registerBuffer(ha), l.registerBuffer(hb)};
            t.IsTrue(queueNow(l, ids[0]), "A queued");
            t.IsTrue(queueNow(l, ids[1]), "B queued (replaces A)");
            sf.latch(); // completions not delivered yet
            const auto before = std::chrono::steady_clock::now();
            const Ledger::Pick p = l.pick(ids, 2, -1, 0, 5);
            t.IsTrue(p.index < 0 && p.result == Ledger::Wait::Timeout, "no buffer reusable before its completion");
            t.IsTrue(std::chrono::steady_clock::now() - before >= std::chrono::milliseconds(5), "waited the bound");
            t.IsFalse(queueNow(l, ids[0]), "queue refuses a held buffer");
            Ledger::Counts c = l.counts();
            t.Equals(c.cbTimeouts, 1ull, "callback timeout counted");
            t.Equals(c.fenceTimeouts, 0ull, "not a fence timeout");
            sf.deliverAll(l); // A's release fence arrives, unsignalled
            const Ledger::Pick q = l.pick(ids, 2, -1, 0, 5);
            t.IsTrue(q.index < 0 && q.result == Ledger::Wait::Timeout, "unsignalled fence: still held");
            c = l.counts();
            t.Equals(c.fenceTimeouts, 1ull, "fence timeout counted separately");
            t.Equals(c.openFences, 1u, "fence kept, not closed on timeout");
            t.IsTrue(sf.held(ha), "SF really still holds A");
            sf.signalAll();
            const Ledger::Pick r = l.pick(ids, 2, -1, 0, 5);
            t.Equals(r.index, 0, "A reusable once its fence signalled");
            t.Equals(l.counts().openFences, 0u, "fence closed after it signalled");
            t.Equals(sf.fds.size(), static_cast<size_t>(0), "no fd leaked (dups closed)"); });

        tc.Run("B1: a completion arriving during the wait wakes it", [](TestCase &t)
               {
            FakeSF sf;
            Ledger l(sf);
            l.setLog(false);
            l.setWindow(reinterpret_cast<void *>(0x77), 0, 796, 448);
            const uint64_t ids[2] = {l.registerBuffer(sf.newBuffer()), l.registerBuffer(sf.newBuffer())};
            queueNow(l, ids[0]);
            queueNow(l, ids[1]);
            sf.latch();
            sf.signalAll(); // fence for A signalled already
            std::thread cb([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                sf.deliverAll(l);
            });
            const Ledger::Pick p = l.pick(ids, 2, -1, 1, 2000);
            cb.join();
            t.Equals(p.index, 0, "A picked after the completion arrived");
            t.Equals(l.counts().cbTimeouts, 0ull, "no timeout"); });

        tc.Run("B1: EINTR is retried within one deadline; POLLNVAL never grants ownership", [](TestCase &t)
               {
            FakeSF sf;
            Ledger l(sf);
            l.setLog(false);
            l.setWindow(reinterpret_cast<void *>(0x77), 0, 796, 448);
            void *h[3] = {sf.newBuffer(), sf.newBuffer(), sf.newBuffer()};
            const uint64_t ids[3] = {l.registerBuffer(h[0]), l.registerBuffer(h[1]), l.registerBuffer(h[2])};
            queueNow(l, ids[0]);
            queueNow(l, ids[1]);
            sf.latch();
            sf.deliverAll(l);
            sf.signalAll();
            sf.eintrLeft = 3;
            const uint64_t onlyA[1] = {ids[0]};
            const Ledger::Pick p = l.pick(onlyA, 1, -1, 0, 50);
            t.Equals(p.index, 0, "ready after 3 EINTRs");
            t.Equals(l.counts().eintr, 3ull, "EINTRs counted");
            // A back on screen, B released with a fence that polls as POLLNVAL.
            queueNow(l, ids[0]);
            sf.latch();
            sf.deliverAll(l);
            sf.nvalFences.insert(static_cast<int>(sf.fenceSignalled.size()) - 1);
            const uint64_t onlyB[1] = {ids[1]};
            const Ledger::Pick q = l.pick(onlyB, 1, -1, 0, 5);
            t.IsTrue(q.index < 0 && q.result == Ledger::Wait::Error, "POLLNVAL -> error, no ownership");
            t.Equals(l.counts().fenceErrors, 1ull, "fence error counted");
            const Ledger::Pick q2 = l.pick(onlyB, 1, -1, 0, 0);
            t.IsTrue(q2.index < 0, "a buffer with an unknowable release is never reused");
            t.IsFalse(queueNow(l, ids[1]), "nor queued");
            const Ledger::Pick q3 = l.pick(ids, 3, -1, 1, 0);
            t.Equals(q3.index, 2, "a genuinely free buffer is used instead");
            // EINTR forever: bounded by the deadline, fence kept.
            queueNow(l, ids[2]);
            sf.latch();
            sf.deliverAll(l); // A released with an unsignalled fence
            sf.eintrForever = true;
            const auto t0 = std::chrono::steady_clock::now();
            const Ledger::Pick r = l.pick(onlyA, 1, -1, 0, 20);
            const auto took = std::chrono::steady_clock::now() - t0;
            t.IsTrue(r.index < 0 && r.result == Ledger::Wait::Timeout, "EINTR storm -> timeout");
            t.IsTrue(took < std::chrono::milliseconds(500), "within one deadline");
            t.Equals(l.counts().openFences, 2u, "A's fence kept (and the lost B's, until retired)"); });

        tc.Run("B1: persistent failure gives up after kMaxConsecutiveSkips", [](TestCase &t)
               {
            FakeSF sf;
            Ledger l(sf);
            l.setLog(false);
            l.setWindow(reinterpret_cast<void *>(0x77), 0, 796, 448);
            SimBackend be{l, sf};
            for (int i = 0; i < 4; ++i)
            {
                be.present();
                sf.latch(); // completions never delivered
            }
            int frames = 0;
            while (!l.broken() && frames < 100)
            {
                be.present();
                ++frames;
            }
            t.IsTrue(l.broken(), "fell back");
            t.IsTrue(frames <= static_cast<int>(Ledger::kMaxConsecutiveSkips) + 1, "after the bound");
            t.Equals(be.heldWrites, 0, "never wrote a held buffer"); });

        tc.Run("B2: same-pointer TERM/INIT_WINDOW with completions outstanding", [](TestCase &t)
               {
            FakeSF sf;
            Ledger l(sf);
            l.setLog(false);
            void *win = reinterpret_cast<void *>(0x77);
            l.setWindow(win, 0, 796, 448);
            SimBackend be{l, sf};
            for (int i = 0; i < 4; ++i)
            {
                be.present();
                sf.latch();
                sf.deliverAll(l);
                sf.signalAll();
            }
            const uint32_t e0 = l.epoch();
            const uint64_t oldIds[4] = {be.ids[0], be.ids[1], be.ids[2], be.ids[3]};
            be.present(); // queue one more; its completion stays outstanding
            sf.latch();
            l.windowLost();
            l.setWindow(win, 0, 796, 448); // same ANativeWindow pointer
            t.Equals(sf.layers.size(), static_cast<size_t>(2), "a new layer for the reused pointer");
            t.IsTrue(l.epoch() != e0, "epoch bumped: old pool retired");
            t.IsFalse(l.queue(oldIds[0], 512, 448), "old-epoch buffer refused on the new layer");
            for (int i = 0; i < 6; ++i)
                be.present(); // new pool, new layer; old completions still outstanding
            const Ledger::Counts mid = l.counts();
            t.Equals(mid.retiredLayers, 1u, "old layer kept while completions can name it");
            t.Equals(sf.layers.begin()->second.released, false, "not released early");
            sf.latch();
            sf.deliverAll(l); // old-layer completions arrive late, then the new layer's
            sf.signalAll();
            be.present();
            be.present();
            sf.latch();
            sf.deliverAll(l);
            const Ledger::Counts c = l.counts();
            t.Equals(c.retiredLayers, 0u, "old layer released after its last completion");
            t.Equals(c.layersReleased, 1ull, "exactly one layer released");
            t.Equals(sf.layers.begin()->second.released, true, "ASurfaceControl_release called");
            t.Equals(c.records, 4u, "old pool records freed, new pool kept");
            t.Equals(c.staleCallbacks, 0ull, "every completion resolved its own submission");
            t.Equals(be.heldWrites, 0, "no write into a held buffer");
            t.Equals(sf.releasedWithPending + sf.useAfterRelease + sf.badTokenLayer + sf.badClose + sf.doubleRelease,
                     0, "no layer/fd misuse"); });

        tc.Run("B2: a detach that reports release early cannot reach a reused buffer", [](TestCase &t)
               {
            FakeSF sf;
            sf.detachEarlyRelease = true; // pessimistic compositor: -1 while still releasing
            Ledger l(sf);
            l.setLog(false);
            void *win = reinterpret_cast<void *>(0x77);
            l.setWindow(win, 0, 796, 448);
            SimBackend be{l, sf};
            for (int cycle = 0; cycle < 20; ++cycle)
            {
                for (int i = 0; i < 5; ++i)
                {
                    be.present();
                    sf.latch();
                    sf.deliverAll(l);
                    sf.signalAll(); // normal releases signal; the detached buffers stay stuck
                }
                l.windowLost();
                l.setWindow(win, 0, 796, 448);
            }
            t.IsFalse(l.broken(), "kept presenting");
            t.IsTrue(be.writes >= 80, "frames written");
            t.Equals(be.heldWrites, 0, "old-window buffers are never written again"); });

        tc.Run("B2: resolution change mid-flight; late completion vs a reused handle address", [](TestCase &t)
               {
            FakeSF sf;
            Ledger l(sf);
            l.setLog(false);
            l.setWindow(reinterpret_cast<void *>(0x77), 0, 796, 448);
            void *ha = sf.newBuffer(), *hb = sf.newBuffer();
            const uint64_t a = l.registerBuffer(ha), b = l.registerBuffer(hb);
            queueNow(l, a);
            queueNow(l, b);
            sf.latch(); // completions outstanding
            l.retireBuffer(a); // size change: the backend drops the pool
            l.retireBuffer(b);
            t.Equals(sf.releasedBuffers.size(), static_cast<size_t>(0), "references kept while submissions pend");
            // The allocator hands back the same address for the new pool.
            const uint64_t a2 = l.registerBuffer(ha);
            t.IsTrue(a2 != a, "a new allocation id for the same pointer");
            sf.deliverAll(l); // A's late completion
            const Ledger::Counts c = l.counts();
            t.Equals(sf.releasedBuffers.count(ha), static_cast<size_t>(1), "old A released after its completion");
            t.Equals(c.records, 2u, "B (still shown, retired) + A2");
            const uint64_t ids[1] = {a2};
            t.Equals(l.pick(ids, 1, -1, 0, 0).index, 0, "the new allocation is free (no stale fence attached)");
            t.Equals(c.staleCallbacks, 0ull, "no stale mutation"); });

        tc.Run("S1: 100 recreate cycles keep layers, fds and records bounded", [](TestCase &t)
               {
            FakeSF sf;
            Ledger l(sf);
            l.setLog(false);
            SimBackend be{l, sf};
            uint32_t maxLayers = 0, maxRecords = 0, maxFences = 0;
            size_t maxFds = 0;
            for (int cycle = 0; cycle < 100; ++cycle)
            {
                // alternate a reused pointer and a new one
                void *win = reinterpret_cast<void *>(cycle % 2 ? 0x77 : 0x88 + cycle);
                l.setWindow(win, 0, 796, 448);
                for (int i = 0; i < 8; ++i)
                {
                    be.present();
                    if (i % 3 != 2)
                        sf.latch();
                    if (i % 2)
                        sf.deliverAll(l);
                    if (i % 4 == 3)
                        sf.signalAll();
                    const Ledger::Counts c = l.counts();
                    maxLayers = std::max(maxLayers, c.liveLayers);
                    maxRecords = std::max(maxRecords, c.records);
                    maxFences = std::max(maxFences, c.openFences);
                    maxFds = std::max(maxFds, sf.fds.size());
                }
                l.windowLost(); // leaves completions outstanding across the change
            }
            sf.latch();
            sf.deliverAll(l);
            sf.signalAll();
            be.retire();
            const Ledger::Counts c = l.counts();
            t.Equals(c.layersCreated, 100ull, "one layer per window");
            t.Equals(c.layersReleased, 100ull, "every layer released");
            t.Equals(c.liveLayers, 0u, "none left");
            t.Equals(c.records, 0u, "no buffer records left");
            t.Equals(c.tokens, 0u, "no tokens left");
            t.Equals(sf.fds.size(), static_cast<size_t>(0), "no fds left");
            t.Equals(sf.releasedBuffers.size(), static_cast<size_t>(4 * be.pools), "every buffer reference released");
            t.IsTrue(maxLayers <= 3u, "live layers bounded");
            t.IsTrue(maxRecords <= 12u, "records bounded");
            t.IsTrue(maxFences <= 8u && maxFds <= 8u, "fds bounded");
            t.Equals(be.heldWrites, 0, "no write into a held buffer");
            t.Equals(sf.releasedWithPending + sf.useAfterRelease + sf.badTokenLayer + sf.badClose + sf.doubleRelease,
                     0, "no layer/fd misuse"); });

        tc.Run("S1: completions that never arrive stop layer creation at the bound", [](TestCase &t)
               {
            FakeSF sf;
            Ledger l(sf);
            l.setLog(false);
            for (int cycle = 0; cycle < 20 && !l.broken(); ++cycle)
            {
                l.setWindow(reinterpret_cast<void *>(0x100 + cycle), 0, 796, 448);
                l.windowLost();
            }
            t.IsTrue(l.broken(), "fell back to GL");
            t.IsTrue(l.counts().liveLayers <= Ledger::kMaxRetiredLayers, "live layers bounded"); });

        tc.Run("randomized: 20k frames of delays, EINTR, window churn and size changes", [](TestCase &t)
               {
            for (unsigned seed = 1; seed <= 4; ++seed)
            {
                std::mt19937 rng(seed);
                std::uniform_real_distribution<double> u(0, 1);
                FakeSF sf;
                sf.detachAbsent = seed == 3;
                Ledger l(sf);
                l.setLog(false);
                SimBackend be{l, sf};
                void *win = reinterpret_cast<void *>(0x77);
                l.setWindow(win, 0, 796, 448);
                bool gone = false;
                uint32_t maxLayers = 0, maxRecords = 0;
                size_t maxFds = 0;
                for (int f = 0; f < 5000; ++f)
                {
                    if (!gone && u(rng) < 0.01)
                    {
                        l.windowLost();
                        gone = true;
                    }
                    else if (gone && u(rng) < 0.2)
                    {
                        if (u(rng) < 0.5)
                            win = reinterpret_cast<void *>(0x1000 + f);
                        gone = false;
                    }
                    l.setWindow(gone ? nullptr : win, 0, 796, 448);
                    if (u(rng) < 0.005)
                        be.retire(); // size change: next present makes a new pool
                    if (u(rng) < 0.05)
                        sf.eintrLeft = 2;
                    be.present();
                    if (u(rng) < 0.7)
                        sf.latch();
                    while (!sf.callbacks.empty() && u(rng) < 0.6)
                    {
                        // occasionally out of order
                        const size_t k = u(rng) < 0.1 ? sf.callbacks.size() - 1 : 0;
                        const FakeSF::Callback c = sf.callbacks[k];
                        sf.callbacks.erase(sf.callbacks.begin() + static_cast<long>(k));
                        sf.deliver(l, c);
                    }
                    sf.signalRandom(rng, 0.5);
                    const Ledger::Counts c = l.counts();
                    maxLayers = std::max(maxLayers, c.liveLayers);
                    maxRecords = std::max(maxRecords, c.records);
                    maxFds = std::max(maxFds, sf.fds.size());
                }
                l.windowLost();
                sf.latch();
                sf.deliverAll(l);
                sf.signalAll();
                be.retire();
                const Ledger::Counts c = l.counts();
                const std::string s = " (seed " + std::to_string(seed) + ")";
                t.Equals(be.heldWrites, 0, "no write into a held buffer" + s);
                t.IsTrue(be.writes > 1000, "frames were presented" + s);
                t.Equals(c.liveLayers + c.records + c.tokens, 0u, "everything released at the end" + s);
                t.Equals(sf.fds.size(), static_cast<size_t>(0), "no fds left" + s);
                t.IsTrue(maxLayers <= Ledger::kMaxRetiredLayers + 1u && maxRecords <= 40u && maxFds <= 40u,
                         "bounded during the run" + s);
                t.Equals(c.staleCallbacks, 0ull, "no stale completions" + s);
                t.Equals(sf.releasedWithPending + sf.useAfterRelease + sf.badTokenLayer + sf.badClose +
                             sf.doubleRelease,
                         0, "no layer/fd misuse" + s);
            } });

        tc.Run("DP1: PS2X_DISPLAY_HZ is 60 unless exactly 120", [](TestCase &t)
               {
            t.Equals(ps2x_present_vk::displayHzFromEnv(nullptr), 60, "unset is 60");
            t.Equals(ps2x_present_vk::displayHzFromEnv("60"), 60, "60 is 60");
            t.Equals(ps2x_present_vk::displayHzFromEnv("120"), 120, "120 is 120");
            t.Equals(ps2x_present_vk::displayHzFromEnv(""), 60, "empty is 60");
            t.Equals(ps2x_present_vk::displayHzFromEnv("144"), 60, "other rates are 60");
            t.Equals(ps2x_present_vk::displayHzFromEnv("120 "), 60, "trailing space is 60");
            t.Equals(ps2x_present_vk::displayHzFromEnv("0120"), 60, "leading zero is 60"); });

        // DSP3: the SF-latch window behind [perf-present] latched=/latch_short=.
        // complete() counts the latch times; the tests feed synthetic streams
        // straight into complete() (unknown tokens: only the latch side runs)
        // and read the window back with takeSfLatchWindow().
        tc.Run("DSP3: 8.33 ms latch stream reads ~120 latched, all short", [](TestCase &t)
               {
            FakeSF sf;
            Ledger l(sf);
            l.setLog(false);
            int64_t latch = 1000000000;
            for (int i = 0; i < 120; ++i)
            {
                l.complete(0x1000u + static_cast<uint64_t>(i), false, -1, latch);
                latch += 8333333; // 8.33 ms: a 120 Hz SF grid
            }
            const Ledger::SfLatchWindow w = l.takeSfLatchWindow();
            t.Equals(w.latched, 120ull, "120 distinct latch times");
            t.Equals(w.intervals, 119ull, "one interval per latch after the first");
            t.Equals(w.shortIntervals, 119ull, "every 8.33 ms interval is short");
            const Ledger::SfLatchWindow empty = l.takeSfLatchWindow();
            t.Equals(empty.latched + empty.intervals + empty.shortIntervals, 0ull, "drain resets"); });

        tc.Run("DSP3: 16.67 ms latch stream reads ~60 latched, none short", [](TestCase &t)
               {
            FakeSF sf;
            Ledger l(sf);
            l.setLog(false);
            int64_t latch = 2000000000;
            for (int i = 0; i < 60; ++i)
            {
                l.complete(0x2000u + static_cast<uint64_t>(i), false, -1, latch);
                latch += 16666667; // 16.67 ms: a stuck-at-60 SF grid
            }
            const Ledger::SfLatchWindow w = l.takeSfLatchWindow();
            t.Equals(w.latched, 60ull, "60 distinct latch times");
            t.Equals(w.intervals, 59ull, "one interval per latch after the first");
            t.Equals(w.shortIntervals, 0ull, "no 16.67 ms interval is short"); });

        tc.Run("DSP3: duplicates and zeros don't count; the boundary interval survives the drain",
               [](TestCase &t)
               {
            FakeSF sf;
            Ledger l(sf);
            l.setLog(false);
            l.complete(1, false, -1, 3000000000);
            l.complete(2, false, -1, 3000000000); // same latch twice (two completions, one latch)
            l.complete(3, false, -1, 0);          // no latch time
            l.complete(4, false, -1, -7);         // no latch time
            Ledger::SfLatchWindow w = l.takeSfLatchWindow();
            t.Equals(w.latched, 1ull, "one distinct latch");
            t.Equals(w.intervals, 0ull, "no interval from one latch");
            l.complete(5, false, -1, 3000000000 + 8333333); // next window's first latch
            w = l.takeSfLatchWindow();
            t.Equals(w.latched, 1ull, "one latch after the drain");
            t.Equals(w.intervals, 1ull, "the boundary interval counts (last latch is kept)");
            t.Equals(w.shortIntervals, 1ull, "8.33 ms is short"); });

        tc.Run("OUT1: PS2X_PRESENT_PREROTATE parses 1|2, else off", [](TestCase &t)
               {
            t.Equals(ps2x_present_vk::prerotateFromEnv(nullptr), 0, "unset is off");
            t.Equals(ps2x_present_vk::prerotateFromEnv(""), 0, "empty is off");
            t.Equals(ps2x_present_vk::prerotateFromEnv("1"), 1, "1 is sense 1");
            t.Equals(ps2x_present_vk::prerotateFromEnv("2"), 2, "2 is sense 2");
            t.Equals(ps2x_present_vk::prerotateFromEnv("0"), 0, "0 is off");
            t.Equals(ps2x_present_vk::prerotateFromEnv("3"), 0, "3 is off");
            t.Equals(ps2x_present_vk::prerotateFromEnv("12"), 0, "12 is off");
            t.Equals(ps2x_present_vk::prerotateFromEnv("1 "), 0, "trailing space is off");
            t.Equals(ps2x_present_vk::prerotateTransform(0), 0, "off maps to transform 0");
            t.Equals(ps2x_present_vk::prerotateTransform(1), 0x07, "sense 1 maps to ROT_270");
            t.Equals(ps2x_present_vk::prerotateTransform(2), 0x07, "sense 2 maps to ROT_270");
            t.Equals(ps2x_present_vk::prerotateTransform(3), 0, "bogus maps to 0"); });

        tc.Run("OUT1: prerotate map/unmap corners + round-trip", [](TestCase &t)
               {
            using ps2x_present_vk::prerotateMap;
            using ps2x_present_vk::prerotateUnmap;
            // Sense 1 (CCW), 1920x1080 frame at origin: (x, y) -> (y, 1919 - x).
            auto a = prerotateMap(1, 0, 0, 1920, 1080, 0, 0);
            t.Equals(a.x, 0, "s1 TL x");
            t.Equals(a.y, 1919, "s1 TL y");
            a = prerotateMap(1, 1919, 0, 1920, 1080, 0, 0);
            t.Equals(a.x, 0, "s1 TR x");
            t.Equals(a.y, 0, "s1 TR y");
            a = prerotateMap(1, 0, 1079, 1920, 1080, 0, 0);
            t.Equals(a.x, 1079, "s1 BL x");
            t.Equals(a.y, 1919, "s1 BL y");
            a = prerotateMap(1, 1919, 1079, 1920, 1080, 0, 0);
            t.Equals(a.x, 1079, "s1 BR x");
            t.Equals(a.y, 0, "s1 BR y");
            // Sense 2 (CW): (x, y) -> (1079 - y, x).
            auto b = prerotateMap(2, 0, 0, 1920, 1080, 0, 0);
            t.Equals(b.x, 1079, "s2 TL x");
            t.Equals(b.y, 0, "s2 TL y");
            b = prerotateMap(2, 1919, 0, 1920, 1080, 0, 0);
            t.Equals(b.x, 1079, "s2 TR x");
            t.Equals(b.y, 1919, "s2 TR y");
            b = prerotateMap(2, 0, 1079, 1920, 1080, 0, 0);
            t.Equals(b.x, 0, "s2 BL x");
            t.Equals(b.y, 0, "s2 BL y");
            // Origin offset applies after the rotation.
            a = prerotateMap(1, 0, 0, 1920, 1080, 10, 20);
            t.Equals(a.x, 10, "s1 origin x");
            t.Equals(a.y, 1939, "s1 origin y");
            // Round-trip over corners, edges and centre, both senses.
            const int xs[] = {0, 1, 959, 960, 1918, 1919};
            const int ys[] = {0, 1, 539, 540, 1078, 1079};
            for (int sense = 1; sense <= 2; ++sense)
            {
                for (int x : xs)
                {
                    for (int y : ys)
                    {
                        const auto q = prerotateMap(sense, x, y, 1920, 1080, 10, 20);
                        const auto r = prerotateUnmap(sense, q.x, q.y, 1920, 1080, 10, 20);
                        t.Equals(r.x, x, "round-trip x");
                        t.Equals(r.y, y, "round-trip y");
                    }
                }
            } });

        tc.Run("OUT1: prerotate region matches the mapped corners", [](TestCase &t)
               {
            using ps2x_present_vk::prerotateMap;
            using ps2x_present_vk::prerotateRegion;
            // Tricky-ish region in a 1920x1080 frame.
            const int rx = 100, ry = 200, rw = 480, rh = 864;
            const auto p1 = prerotateRegion(1, rx, ry, rw, rh, 1920, 1080);
            t.Equals(p1.x, 200, "s1 region x");
            t.Equals(p1.y, 1340, "s1 region y");
            t.Equals(p1.w, 864, "s1 region w");
            t.Equals(p1.h, 480, "s1 region h");
            const auto p2 = prerotateRegion(2, rx, ry, rw, rh, 1920, 1080);
            t.Equals(p2.x, 16, "s2 region x");
            t.Equals(p2.y, 100, "s2 region y");
            t.Equals(p2.w, 864, "s2 region w");
            t.Equals(p2.h, 480, "s2 region h");
            // Every mapped region corner lands on the portrait rect's corners.
            const int cx[] = {rx, rx + rw - 1};
            const int cy[] = {ry, ry + rh - 1};
            for (int sense = 1; sense <= 2; ++sense)
            {
                const auto pr = sense == 1 ? p1 : p2;
                for (int x : cx)
                {
                    for (int y : cy)
                    {
                        const auto q = prerotateMap(sense, x - rx, y - ry, rw, rh, pr.x, pr.y);
                        const bool inside = q.x >= pr.x && q.x < pr.x + pr.w && q.y >= pr.y &&
                                            q.y < pr.y + pr.h;
                        t.Equals(inside, true, "mapped corner inside the portrait rect");
                    }
                }
            } });
    });
}
