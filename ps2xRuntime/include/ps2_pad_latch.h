#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

// IN2: per-button press latch between host input and the guest's pad read.
//
// At low guest speed (Brad, 09-25: race ~0.1x on the iPhone, taps need
// spamming) a guest frame lasts ~150 ms wall while the host render loop
// samples input every ~16 ms. The game polls the pad once per guest frame
// (scePadRead -> readPadPortData -> PSPadBackend::readState), so a tap that
// presses and releases between two guest reads is never seen, and menus
// that need a press edge (up on one read, down on the next) miss it.
//
// The render thread publishes the union of virtual-pad + raylib buttons at
// host frame rate via SharedLatch::publish (edge-detecting); the game
// thread takes one presented mask per guest read via SharedLatch::consume.
// A host press is shown as pressed until at least one guest read has
// returned it pressed AND the host has released it; a release is shown as
// up for at least one guest read before a new press can show. Analog
// sticks bypass the latch; the pad script applies on top as before.
//
// Two full taps between two guest reads coalesce into ONE press (sticky
// bits, not a queue): replaying every tap would stack stale presses at low
// guest speed, so spam-tapping a menu yields one press, not N. Holds are
// continuous (live-follow).
//
// DEV-ONLY PS2X_PAD_LATCH=0 restores the pre-IN2 path (direct sampling in
// readState) for A/B; default on. DEV-ONLY PS2X_PAD_READ_LOG=1 logs host
// edges (publish side) and every guest read (Pad.cpp side) to stderr.
namespace ps2x::padlatch
{
    // PS2X_PAD_LATCH: unset or anything but "0" = latch on.
    inline bool enabledFromEnv(const char *value)
    {
        return !(value && value[0] == '0');
    }

    inline bool latchEnabled()
    {
        return enabledFromEnv(std::getenv("PS2X_PAD_LATCH"));
    }

    inline bool readLogEnabled()
    {
        const char *env = std::getenv("PS2X_PAD_READ_LOG");
        return env && env[0] == '1';
    }

    // Shared wall clock for the read log and PS2X_VPAD_TEST_TAP: ms since
    // the first call (the render loop's first frame sets the epoch).
    inline uint64_t wallMs()
    {
        using clock = std::chrono::steady_clock;
        static const clock::time_point start = clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count());
    }

    // Single-bit button name for the read log (pad-script names); multi-bit
    // or zero masks log as hex.
    inline const char *buttonName(uint16_t bit)
    {
        switch (bit)
        {
        case 1u << 0: return "select";
        case 1u << 1: return "l3";
        case 1u << 2: return "r3";
        case 1u << 3: return "start";
        case 1u << 4: return "up";
        case 1u << 5: return "right";
        case 1u << 6: return "down";
        case 1u << 7: return "left";
        case 1u << 8: return "l2";
        case 1u << 9: return "r2";
        case 1u << 10: return "l1";
        case 1u << 11: return "r1";
        case 1u << 12: return "triangle";
        case 1u << 13: return "circle";
        case 1u << 14: return "cross";
        case 1u << 15: return "square";
        default: return nullptr;
        }
    }

    // The state machine (single-threaded; SharedLatch guards it). Masks are
    // active-high pressed bits (1 = pressed).
    struct Latch
    {
        uint16_t live = 0u;     // last published host mask (producer)
        uint16_t pendDown = 0u; // press edges since the last guest read
        uint16_t pendUp = 0u;   // release edges since the last guest read
        uint16_t owedDown = 0u; // latched presses not yet shown (consumer)
        uint16_t owedUp = 0u;   // latched releases not yet shown (consumer)
        uint16_t shown = 0u;    // last presented mask (consumer)

        // Producer: call at host input rate with the current host mask.
        void noteSample(uint16_t mask)
        {
            const uint16_t rose = static_cast<uint16_t>(mask & ~live);
            const uint16_t fell = static_cast<uint16_t>(live & ~mask);
            live = mask;
            pendDown = static_cast<uint16_t>(pendDown | rose);
            pendUp = static_cast<uint16_t>(pendUp | fell);
        }

        // Consumer: call once per guest read; returns the presented mask.
        uint16_t consumeRead()
        {
            owedDown = static_cast<uint16_t>(owedDown | pendDown);
            owedUp = static_cast<uint16_t>(owedUp | pendUp);
            pendDown = 0u;
            pendUp = 0u;
            uint16_t out = 0u;
            for (uint16_t bit = 1u; bit != 0u; bit = static_cast<uint16_t>(bit << 1))
            {
                const bool wasShown = (shown & bit) != 0u;
                const bool isLive = (live & bit) != 0u;
                const bool downOwed = (owedDown & bit) != 0u;
                const bool upOwed = (owedUp & bit) != 0u;
                if (wasShown)
                {
                    if (upOwed)
                    {
                        // The release gets its up-read; a re-press owed
                        // alongside it shows on the next read.
                        owedUp = static_cast<uint16_t>(owedUp & ~bit);
                    }
                    else if (isLive || downOwed)
                    {
                        out = static_cast<uint16_t>(out | bit);
                    }
                }
                else if (downOwed)
                {
                    out = static_cast<uint16_t>(out | bit);
                    owedDown = static_cast<uint16_t>(owedDown & ~bit);
                }
                else if (isLive)
                {
                    out = static_cast<uint16_t>(out | bit);
                }
            }
            shown = out;
            return out;
        }

        uint16_t peekLive() const
        {
            return live;
        }
    };

    // Cross-thread latch: render thread publishes, game thread consumes.
    class SharedLatch
    {
    public:
        void publish(uint16_t mask)
        {
            uint16_t rose = 0u, fell = 0u;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                rose = static_cast<uint16_t>(mask & ~m_latch.live);
                fell = static_cast<uint16_t>(m_latch.live & ~mask);
                m_latch.noteSample(mask);
            }
            if ((rose | fell) != 0u && readLogEnabled())
            {
                const char *r = (rose & (rose - 1u)) == 0u ? buttonName(rose) : nullptr;
                const char *f = (fell & (fell - 1u)) == 0u ? buttonName(fell) : nullptr;
                if (r && f)
                    std::fprintf(stderr, "[padread] host +%s -%s wall=%llums\n", r, f,
                                 static_cast<unsigned long long>(wallMs()));
                else if (r)
                    std::fprintf(stderr, "[padread] host +%s wall=%llums\n", r,
                                 static_cast<unsigned long long>(wallMs()));
                else if (f)
                    std::fprintf(stderr, "[padread] host -%s wall=%llums\n", f,
                                 static_cast<unsigned long long>(wallMs()));
                else
                    std::fprintf(stderr, "[padread] host +0x%04x -0x%04x wall=%llums\n", rose, fell,
                                 static_cast<unsigned long long>(wallMs()));
            }
        }

        uint16_t consume()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_latch.consumeRead();
        }

        uint16_t peekLive()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_latch.peekLive();
        }

        void resetForTest()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_latch = Latch{};
        }

    private:
        std::mutex m_mutex;
        Latch m_latch;
    };

    inline SharedLatch &sharedLatch()
    {
        static SharedLatch latch;
        return latch;
    }
}
