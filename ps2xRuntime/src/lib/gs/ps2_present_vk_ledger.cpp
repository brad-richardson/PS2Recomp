// VK2: platform-neutral bookkeeping of the Android Vulkan present. See
// runtime/gs/ps2_present_vk_ledger.h for the ownership rules.
#include "runtime/gs/ps2_present_vk_ledger.h"
#include "ps2_present_geometry.h"

#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>

namespace ps2x_present_vk
{
namespace
{
using Clock = std::chrono::steady_clock;

int msUntil(Clock::time_point deadline)
{
    const auto now = Clock::now();
    if (now >= deadline)
        return 0;
    // Round up so a sub-millisecond remainder still waits instead of spinning.
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
    return static_cast<int>((us + 999) / 1000);
}

uint64_t nsSince(Clock::time_point t0)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}
} // namespace

bool Ledger::setWindow(void *window, int aspect, int bufW, int bufH)
{
    std::lock_guard<std::mutex> lock(m_m);
    if (aspect != m_aspect)
    {
        m_aspect = aspect;
        m_geometrySet = false;
    }
    if (bufW > 0 && bufH > 0 && (bufW != m_bufW || bufH != m_bufH))
    {
        m_bufW = bufW;
        m_bufH = bufH;
        m_geometrySet = false;
        if (m_log)
            std::fprintf(stderr, "[present-vk] parent buffer %dx%d\n", bufW, bufH);
    }
    if (window == m_window)
        return false;
    detachLocked("window changed");
    m_liveOnWindow = false;
    m_dropsWithWindow = 0;
    ++m_windowGen;
    m_window = window;
    m_geometrySet = false;
    if (window && !m_broken)
    {
        if (retiredLayersLocked() >= kMaxRetiredLayers)
        {
            // Completions for detached layers are not arriving: stop making layers.
            m_broken = true;
            std::fprintf(stderr, "[present-vk] FALLBACK to the GL present: %u detached layers still owed completions\n",
                         retiredLayersLocked());
        }
        else if (void *sc = m_p.createLayer(window))
        {
            const uint64_t id = m_nextId++;
            Layer &l = m_layers[id];
            l.sc = sc;
            l.current = true;
            m_current = id;
            ++m_c.layersCreated;
        }
    }
    m_cv.notify_all();
    return true;
}

void Ledger::windowLost()
{
    std::lock_guard<std::mutex> lock(m_m);
    detachLocked("APP_CMD_TERM_WINDOW");
    ++m_windowGen;
    m_window = nullptr;
    m_liveOnWindow = false;
    m_geometrySet = false;
    m_cv.notify_all();
}

void Ledger::setUnderlay(bool under)
{
    std::lock_guard<std::mutex> lock(m_m);
    if (under != m_under)
    {
        m_under = under;
        m_geometrySet = false;
        if (m_log)
            std::fprintf(stderr, "[present-vk] child %s the GL window\n", under ? "UNDER (overlay on)" : "above");
    }
}

bool Ledger::underlay()
{
    std::lock_guard<std::mutex> lock(m_m);
    return m_under;
}

void Ledger::fallBack(const char *why)
{
    std::lock_guard<std::mutex> lock(m_m);
    if (m_broken)
        return;
    m_broken = true;
    std::fprintf(stderr, "[present-vk] FALLBACK to the GL present: %s\n", why ? why : "?");
    detachLocked("fallback");
    m_cv.notify_all();
}

bool Ledger::broken() const
{
    std::lock_guard<std::mutex> lock(m_m);
    return m_broken;
}

bool Ledger::active() const
{
    std::lock_guard<std::mutex> lock(m_m);
    return m_active && !m_broken;
}

bool Ledger::layerLive()
{
    std::lock_guard<std::mutex> lock(m_m);
    return m_current != 0 && m_liveOnWindow && !m_broken;
}

uint32_t Ledger::windowGeneration() const
{
    std::lock_guard<std::mutex> lock(m_m);
    return m_windowGen;
}

uint32_t Ledger::epoch()
{
    std::lock_guard<std::mutex> lock(m_m);
    return m_epoch;
}

bool Ledger::gameRect(int &left, int &top, int &right, int &bottom)
{
    std::lock_guard<std::mutex> lock(m_m);
    if (m_geom.right <= m_geom.left || m_geom.bottom <= m_geom.top)
        return false;
    left = m_geom.left;
    top = m_geom.top;
    right = m_geom.right;
    bottom = m_geom.bottom;
    return true;
}

uint64_t Ledger::registerBuffer(void *handle)
{
    std::lock_guard<std::mutex> lock(m_m);
    const uint64_t id = m_nextId++;
    Buf &b = m_bufs[id];
    b.handle = handle;
    b.epoch = m_epoch;
    return id;
}

uint32_t Ledger::bufferEpoch(uint64_t id)
{
    std::lock_guard<std::mutex> lock(m_m);
    const auto it = m_bufs.find(id);
    return it == m_bufs.end() ? 0u : it->second.epoch;
}

void Ledger::retireBuffer(uint64_t id)
{
    std::lock_guard<std::mutex> lock(m_m);
    const auto it = m_bufs.find(id);
    if (it == m_bufs.end())
        return;
    it->second.retired = true;
    maybeFreeBufLocked(id);
}

Ledger::Wait Ledger::waitFencesLocked(std::unique_lock<std::mutex> &lock, uint64_t id, int timeoutMs)
{
    auto it = m_bufs.find(id);
    if (it == m_bufs.end())
        return Wait::Error;
    if (it->second.fences.empty())
        return Wait::Ready;
    // Poll duplicates without the lock (completions keep flowing); the
    // originals stay owned by the record until they are seen signalled. Only
    // this thread (GsWorker) removes fences from a buffer that has no refs.
    struct Fd
    {
        int orig, dup;
    };
    std::vector<Fd> fds;
    for (const int fd : it->second.fences)
    {
        const int d = m_p.dupFd(fd);
        if (d < 0)
        {
            for (const Fd &f : fds)
                m_p.closeFd(f.dup);
            return Wait::Timeout; // out of fds: try again later, ownership not granted
        }
        fds.push_back({fd, d});
    }
    lock.unlock();
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));
    Wait result = Wait::Ready;
    uint64_t eintr = 0;
    size_t signalled = 0;
    for (const Fd &f : fds)
    {
        for (;;)
        {
            short revents = 0;
            int err = 0;
            const int r = m_p.pollFd(f.dup, msUntil(deadline), revents, err);
            if (r < 0)
            {
                if (err == EINTR || err == EAGAIN)
                {
                    ++eintr; // retried within the same deadline
                    if (Clock::now() >= deadline)
                    {
                        result = Wait::Timeout;
                        break;
                    }
                    continue;
                }
                result = Wait::Error;
                break;
            }
            if (r == 0)
                result = Wait::Timeout;
            else if ((revents & (POLLNVAL | POLLERR)) != 0 || (revents & POLLIN) == 0)
                result = Wait::Error; // not a signalled fence
            break;
        }
        if (result != Wait::Ready)
            break;
        ++signalled;
    }
    for (const Fd &f : fds)
        m_p.closeFd(f.dup);
    lock.lock();
    m_c.eintr += eintr;
    it = m_bufs.find(id);
    if (it == m_bufs.end())
        return Wait::Error;
    Buf &b = it->second;
    for (size_t i = 0; i < signalled; ++i)
    {
        const auto pos = std::find(b.fences.begin(), b.fences.end(), fds[i].orig);
        if (pos != b.fences.end())
        {
            m_p.closeFd(*pos);
            b.fences.erase(pos);
        }
    }
    if (result == Wait::Error)
    {
        b.lost = true; // e.g. POLLNVAL: whether the compositor is done cannot be known
        if (m_log && m_c.fenceErrors == 0)
            std::fprintf(stderr, "[present-vk] release fence poll error: buffer %llu retired from use\n",
                         static_cast<unsigned long long>(id));
        ++m_c.fenceErrors;
    }
    return result;
}

Ledger::Pick Ledger::pick(const uint64_t *ids, int n, int skip, int start, int timeoutMs)
{
    const auto t0 = Clock::now();
    const auto deadline = t0 + std::chrono::milliseconds(std::max(0, timeoutMs));
    Pick out;
    std::unique_lock<std::mutex> lock(m_m);
    auto usable = [&](int i) {
        if (i == skip)
            return false;
        const auto it = m_bufs.find(ids[i]);
        return it != m_bufs.end() && !it->second.retired && !it->second.lost && it->second.refs == 0u;
    };
    auto anyUsable = [&] {
        for (int i = 0; i < n; ++i)
            if (usable(i))
                return true;
        return false;
    };
    if (!m_cv.wait_until(lock, deadline, anyUsable))
    {
        ++m_c.cbTimeouts; // every candidate is still on screen or owed a completion
        out.result = Wait::Timeout;
    }
    else
    {
        // Free without waiting first (in ring order), then wait on the first candidate.
        for (int k = 0; k < n && out.index < 0; ++k)
        {
            const int i = (start + k) % n;
            if (!usable(i))
                continue;
            const Wait w = waitFencesLocked(lock, ids[i], 0);
            if (w == Wait::Ready)
                out = {i, Wait::Ready, false};
            else if (w == Wait::Error)
                out.result = Wait::Error; // that buffer is now lost; try the others
        }
        for (int k = 0; k < n && out.index < 0; ++k)
        {
            const int i = (start + k) % n;
            if (!usable(i))
                continue;
            const Wait w = waitFencesLocked(lock, ids[i], msUntil(deadline));
            if (w == Wait::Ready)
                out = {i, Wait::Ready, false};
            else
            {
                out.result = w;
                if (w == Wait::Timeout)
                    ++m_c.fenceTimeouts; // the compositor has not released it yet
            }
            break;
        }
    }
    m_c.waitNs += nsSince(t0);
    if (out.index >= 0)
        m_consecutiveSkips = 0;
    else
    {
        ++m_c.skipped;
        out.giveUp = ++m_consecutiveSkips >= kMaxConsecutiveSkips;
    }
    return out;
}

bool Ledger::queue(uint64_t id, uint32_t w, uint32_t h)
{
    std::lock_guard<std::mutex> lock(m_m);
    if (m_broken)
        return false;
    const auto it = m_bufs.find(id);
    if (it == m_bufs.end() || it->second.retired || it->second.lost || it->second.refs != 0u ||
        !it->second.fences.empty() || it->second.epoch != m_epoch)
    {
        // Never show a buffer the compositor may still hold, or one from a
        // detached window's pool (the backend retires that pool).
        ++m_c.refusedQueues;
        return false;
    }
    Buf &b = it->second;
    if (m_current == 0 || m_bufW <= 0 || m_bufH <= 0)
    {
        ++m_c.dropped;
        // A window that exists but never gets a usable child (createLayer
        // failed, no EGL size) must not leave the screen black: give up.
        if (m_window && ++m_dropsWithWindow > kMaxDropsWithWindow)
        {
            std::fprintf(stderr, "[present-vk] FALLBACK to the GL present: %u drops with a window (layer=%d buf=%dx%d)\n",
                         m_dropsWithWindow, m_current != 0 ? 1 : 0, m_bufW, m_bufH);
            m_broken = true;
            detachLocked("fallback");
        }
        return false;
    }
    m_dropsWithWindow = 0;
    const auto t0 = Clock::now();
    Layer &l = m_layers[m_current];
    const LayerGeometry *geom = nullptr;
    if (!m_geometrySet || w != m_lastW || h != m_lastH)
    {
        // Parent buffer space (VK1 V1: display pixels here were scaled again by
        // the parent's 796x448 -> 1920x1080 buffer scaling: a zoomed picture).
        const ps2x::present::Rect r = ps2x::present::presentRect(
            static_cast<float>(m_bufW), static_cast<float>(m_bufH), static_cast<float>(w), static_cast<float>(h),
            static_cast<ps2x::present::Aspect>(m_aspect));
        // Round the size, then centre it: rounding the edges separately could
        // add a pixel (4:3 in 796x448: 598 wide = 1442 panel px instead of
        // 597 = exactly 1440). Bars are what the GL window shows (black).
        const int32_t dw = std::min<int32_t>(m_bufW, static_cast<int32_t>(r.w + 0.5f));
        const int32_t dh = std::min<int32_t>(m_bufH, static_cast<int32_t>(r.h + 0.5f));
        m_geom.srcW = static_cast<int32_t>(w);
        m_geom.srcH = static_cast<int32_t>(h);
        m_geom.left = (m_bufW - dw) / 2;
        m_geom.top = (m_bufH - dh) / 2;
        m_geom.right = m_geom.left + dw;
        m_geom.bottom = m_geom.top + dh;
        // Above raylib's GL window when nothing is drawn over the game (the Odin
        // default); under it when GL draws an overlay (virtual pad) with the
        // game rect cleared to transparent.
        m_geom.z = m_under ? -1 : 1;
        geom = &m_geom;
        if (m_log)
            std::fprintf(stderr, "[present-vk] geometry src %ux%u -> dst [%d,%d %d,%d] in parent buffer %dx%d aspect=%d\n",
                         w, h, m_geom.left, m_geom.top, m_geom.right, m_geom.bottom, m_bufW, m_bufH, m_aspect);
        m_geometrySet = true;
        m_lastW = w;
        m_lastH = h;
    }
    const uint64_t token = m_nextId++;
    m_tokens[token] = Token{m_current, l.shown};
    ++l.outstanding;
    l.shown = id;
    ++b.refs;
    m_p.applyBuffer(l.sc, b.handle, geom, token);
    m_liveOnWindow = true;
    m_active = true;
    ++m_c.queued;
    m_c.applyNs += nsSince(t0);
    if (m_c.queued == 1u && m_log)
        std::fprintf(stderr, "[present-vk] first buffer queued %ux%u\n", w, h);
    return true;
}

void *Ledger::tokenLayer(uint64_t token)
{
    std::lock_guard<std::mutex> lock(m_m);
    const auto t = m_tokens.find(token);
    if (t == m_tokens.end())
        return nullptr;
    const auto l = m_layers.find(t->second.layer);
    return l == m_layers.end() ? nullptr : l->second.sc;
}

void Ledger::complete(uint64_t token, bool layerInStats, int fd, int64_t latchNs)
{
    std::lock_guard<std::mutex> lock(m_m);
    ++m_c.callbacks;
    if (latchNs > 0)
    {
        if (m_lastLatch > 0 && latchNs > m_lastLatch)
        {
            m_c.latchIntervalSumNs += latchNs - m_lastLatch;
            ++m_c.latchIntervals;
        }
        m_lastLatch = latchNs;
    }
    const auto t = m_tokens.find(token);
    if (t == m_tokens.end())
    {
        // Not ours to resolve (already resolved, or never issued): change nothing.
        if (fd >= 0)
            m_p.closeFd(fd);
        ++m_c.staleCallbacks;
        return;
    }
    const Token tok = t->second;
    m_tokens.erase(t);
    if (tok.releases != 0u)
    {
        const auto b = m_bufs.find(tok.releases);
        if (b == m_bufs.end())
        {
            if (fd >= 0)
                m_p.closeFd(fd);
            ++m_c.staleCallbacks;
        }
        else
        {
            if (!layerInStats)
            {
                // No release information for this buffer: never reuse it.
                if (fd >= 0)
                    m_p.closeFd(fd);
                if (m_log && m_c.absentCallbacks == 0)
                    std::fprintf(stderr, "[present-vk] completion without its layer: buffer %llu retired from use\n",
                                 static_cast<unsigned long long>(tok.releases));
                b->second.lost = true;
                ++m_c.absentCallbacks;
            }
            else if (fd >= 0)
                b->second.fences.push_back(fd);
            if (b->second.refs > 0u)
                --b->second.refs;
            maybeFreeBufLocked(tok.releases);
        }
    }
    else if (fd >= 0)
        m_p.closeFd(fd);
    const auto l = m_layers.find(tok.layer);
    if (l != m_layers.end())
    {
        if (l->second.outstanding > 0u)
            --l->second.outstanding;
        maybeReleaseLayerLocked(tok.layer);
    }
    m_cv.notify_all();
}

void Ledger::detachLocked(const char *why)
{
    if (m_current == 0)
        return;
    const uint64_t id = m_current;
    Layer &l = m_layers[id];
    // Registered completion: the transaction that removes the layer reports the
    // release of the buffer it was showing.
    const uint64_t token = m_nextId++;
    m_tokens[token] = Token{id, l.shown};
    ++l.outstanding;
    l.shown = 0;
    l.current = false;
    m_p.applyDetach(l.sc, token);
    m_current = 0;
    ++m_epoch; // the backend retires the buffers this layer used
    m_liveOnWindow = false;
    m_geometrySet = false;
    if (m_log)
        std::fprintf(stderr, "[present-vk] %s: child layer detached (%u detached layers owed completions)\n", why,
                     retiredLayersLocked());
}

void Ledger::maybeFreeBufLocked(uint64_t id)
{
    const auto it = m_bufs.find(id);
    if (it == m_bufs.end() || !it->second.retired || it->second.refs != 0u)
        return;
    for (const int fd : it->second.fences)
        m_p.closeFd(fd); // never written again: nothing to wait for
    m_p.releaseBuffer(it->second.handle);
    m_bufs.erase(it);
    ++m_c.buffersReleased;
}

void Ledger::maybeReleaseLayerLocked(uint64_t id)
{
    const auto it = m_layers.find(id);
    if (it == m_layers.end() || it->second.current || it->second.outstanding != 0u)
        return;
    m_p.releaseLayer(it->second.sc); // no completion can name it any more
    m_layers.erase(it);
    ++m_c.layersReleased;
}

uint32_t Ledger::retiredLayersLocked() const
{
    uint32_t n = 0;
    for (const auto &kv : m_layers)
        n += kv.second.current ? 0u : 1u;
    return n;
}

Ledger::Counts Ledger::counts()
{
    std::lock_guard<std::mutex> lock(m_m);
    Counts c = m_c;
    c.liveLayers = static_cast<uint32_t>(m_layers.size());
    c.retiredLayers = retiredLayersLocked();
    c.records = static_cast<uint32_t>(m_bufs.size());
    c.openFences = 0;
    for (const auto &kv : m_bufs)
        c.openFences += static_cast<uint32_t>(kv.second.fences.size());
    c.tokens = static_cast<uint32_t>(m_tokens.size());
    return c;
}
} // namespace ps2x_present_vk
