#pragma once
// VK2: the bookkeeping behind the Android Vulkan present (ps2_present_vk.h),
// kept free of NDK calls so the Mac suite can drive it with a fake
// SurfaceFlinger (ps2xTest/src/ps2_present_vk_ledger_tests.cpp).
//
// Ownership rules (Android: ASurfaceTransaction_OnComplete and
// ASurfaceTransactionStats_getPreviousReleaseFenceFd):
// - A buffer is written only when every submission of it has been resolved by
//   the completion of the transaction that replaced or removed it, and every
//   release fence those completions delivered has signalled (POLLIN).
// - A timeout or a poll error never grants ownership: the buffer stays held,
//   its fences stay open, and the caller skips the frame (or uses another
//   buffer that is genuinely free).
// - Each transaction carries a unique token naming the layer and the buffer
//   submission it releases. Completions resolve only their own token; a token
//   or buffer the ledger no longer knows closes its fd and changes nothing.
// - A window change or fallback detaches the layer with a completion
//   registered (so the last shown buffer still gets its release), bumps the
//   buffer epoch (buffers of an older epoch are never queued again; the
//   backend retires that pool), and releases the layer once no completion can
//   name it. Detached layers still waiting are bounded (kMaxRetiredLayers).
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ps2x_present_vk
{
struct LayerGeometry
{
    int32_t srcW = 0, srcH = 0;                        // buffer size
    int32_t left = 0, top = 0, right = 0, bottom = 0; // destination, parent buffer pixels
    int32_t z = 1;                                     // +1 above the GL window, -1 under it
};

// The platform side: NDK SurfaceControl/AHardwareBuffer/poll on Android, a fake
// SurfaceFlinger in the tests. Called with the ledger's mutex held; must not
// call back into the ledger synchronously (completions arrive later, on
// another thread, through Ledger::complete).
struct Platform
{
    virtual ~Platform() = default;
    virtual void *createLayer(void *window) = 0; // child layer of the window, nullptr on failure
    virtual void releaseLayer(void *layer) = 0;
    // One transaction: `buffer` on `layer` (+ geometry when geom), completion -> Ledger::complete(token).
    virtual void applyBuffer(void *layer, void *buffer, const LayerGeometry *geom, uint64_t token) = 0;
    // One transaction: reparent `layer` to null, completion -> Ledger::complete(token).
    virtual void applyDetach(void *layer, uint64_t token) = 0;
    virtual void releaseBuffer(void *buffer) = 0; // the ledger's (the app's) reference
    virtual int dupFd(int fd) = 0;
    virtual void closeFd(int fd) = 0;
    // poll(2) for POLLIN on one fd: returns poll's result; on -1, err = errno.
    virtual int pollFd(int fd, int timeoutMs, short &revents, int &err) = 0;
};

class Ledger
{
public:
    static constexpr uint32_t kMaxRetiredLayers = 8u;     // detached layers still owed completions
    static constexpr uint32_t kMaxConsecutiveSkips = 30u; // frames with no reusable buffer -> give up
    static constexpr uint32_t kMaxDropsWithWindow = 240u; // frames with a window but no layer -> give up

    explicit Ledger(Platform &platform) : m_p(platform) {}
    Ledger(const Ledger &) = delete;
    Ledger &operator=(const Ledger &) = delete;

    void setLog(bool on) { m_log = on; }

    // --- main thread ---
    // Current window (nullptr while gone), presenter aspect and the window's
    // buffer size. Returns true when the window changed (old layer detached,
    // new one made).
    bool setWindow(void *window, int aspect, int bufW, int bufH);
    void windowLost(); // APP_CMD_TERM_WINDOW: detach, even if the pointer comes back
    void setUnderlay(bool under);
    bool underlay();

    // --- any thread ---
    void fallBack(const char *why); // give up on the layer for this run
    bool broken() const;
    bool active() const; // not broken and a buffer has been queued
    bool layerLive();    // the current layer has been given a buffer
    uint32_t windowGeneration() const;
    uint32_t epoch(); // buffers registered under an older epoch must be retired
    bool gameRect(int &left, int &top, int &right, int &bottom);

    // --- GsWorker ---
    uint64_t registerBuffer(void *handle); // takes over the caller's reference; stamped with epoch()
    uint32_t bufferEpoch(uint64_t id);
    // The backend no longer uses `id`. The reference is released once every
    // submission of it has been resolved (immediately if none).
    void retireBuffer(uint64_t id);
    enum class Wait
    {
        Ready,
        Timeout,
        Error
    };
    struct Pick
    {
        int index = -1; // into ids[], -1 when no buffer is reusable
        Wait result = Wait::Timeout;
        bool giveUp = false; // kMaxConsecutiveSkips reached: fall back
    };
    // Choose a reusable buffer among ids[0..n) (skipping index `skip`, starting at
    // `start`), waiting up to timeoutMs for a completion and then its fences.
    Pick pick(const uint64_t *ids, int n, int skip, int start, int timeoutMs);
    // Queue a buffer that pick() returned (w x h valid). False = not shown
    // (no layer, broken, or an older epoch), counted.
    bool queue(uint64_t id, uint32_t w, uint32_t h);

    // --- completion thread ---
    // The layer a token's transaction touched (valid until complete(token)).
    void *tokenLayer(uint64_t token);
    // layerInStats: the layer appeared in the transaction's stats; fd: its
    // previous release fence (ownership passes to the ledger; -1 = released).
    void complete(uint64_t token, bool layerInStats, int fd, int64_t latchNs);

    struct Counts
    {
        uint64_t queued = 0, dropped = 0, skipped = 0, callbacks = 0;
        uint64_t cbTimeouts = 0, fenceTimeouts = 0, fenceErrors = 0, eintr = 0;
        uint64_t staleCallbacks = 0, absentCallbacks = 0, refusedQueues = 0;
        uint64_t layersCreated = 0, layersReleased = 0, buffersReleased = 0;
        uint32_t liveLayers = 0, retiredLayers = 0, records = 0, openFences = 0, tokens = 0;
        uint64_t waitNs = 0, applyNs = 0;
        uint64_t latchIntervals = 0;
        int64_t latchIntervalSumNs = 0;
    };
    Counts counts();

private:
    struct Buf
    {
        void *handle = nullptr;
        uint32_t epoch = 0;
        uint32_t refs = 0;       // submissions not yet resolved by a completion
        std::vector<int> fences; // delivered release fences not yet seen signalled
        bool retired = false;    // backend dropped it: release when refs reach 0
        bool lost = false;       // ownership unknowable (poll error, layer absent): never reused
    };
    struct Layer
    {
        void *sc = nullptr;
        bool current = false;
        uint32_t outstanding = 0; // tokens whose completion has not arrived
        uint64_t shown = 0;       // buffer the next transaction on this layer releases
    };
    struct Token
    {
        uint64_t layer = 0;
        uint64_t releases = 0; // buffer submission this transaction resolves (0: none)
    };

    void detachLocked(const char *why);
    void maybeFreeBufLocked(uint64_t id);
    void maybeReleaseLayerLocked(uint64_t layer);
    uint32_t retiredLayersLocked() const;
    // Waits for all of buf's fences (dup'd, polled unlocked). Called and returns locked.
    Wait waitFencesLocked(std::unique_lock<std::mutex> &lock, uint64_t id, int timeoutMs);

    Platform &m_p;
    mutable std::mutex m_m;
    std::condition_variable m_cv;
    bool m_log = true;
    bool m_broken = false;
    bool m_active = false;
    bool m_under = false;
    bool m_liveOnWindow = false;
    bool m_geometrySet = false;
    void *m_window = nullptr;
    int m_aspect = 0;
    int m_bufW = 0, m_bufH = 0;
    uint32_t m_lastW = 0, m_lastH = 0;
    LayerGeometry m_geom;
    uint32_t m_epoch = 1;
    uint32_t m_windowGen = 0;
    uint32_t m_dropsWithWindow = 0;
    uint32_t m_consecutiveSkips = 0;
    uint64_t m_nextId = 1;
    uint64_t m_current = 0; // current layer id (0: none)
    std::unordered_map<uint64_t, Buf> m_bufs;
    std::map<uint64_t, Layer> m_layers;
    std::unordered_map<uint64_t, Token> m_tokens;
    Counts m_c;
    int64_t m_lastLatch = 0;
};
} // namespace ps2x_present_vk
