#pragma once
// RR1 DEV-ONLY (default off): find the EE source of A+D ALPHA writes in
// VIF1/GIF DMA data. PS2X_RR1_ALPHA_SRC=<value> arms it (e.g. 0x1);
// PS2X_RR1_FROM/PS2X_RR1_TO bound the vsync window; at most 400 lines.
// Each hit prints the delivered-buffer offset, the EE address (from the
// E40 source spans, or the transfer base) and the three qwords before it.
#include "ps2_vif_src_span.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2_rr1
{
    struct AlphaCfg
    {
        bool on = false;
        uint64_t value = 0;
        uint64_t from = 0;
        uint64_t to = ~0ull;
    };

    inline const AlphaCfg &alphaCfg()
    {
        static const AlphaCfg cfg = [] {
            AlphaCfg c;
            const char *e = std::getenv("PS2X_RR1_ALPHA_SRC");
            if (e && e[0])
            {
                c.on = true;
                c.value = std::strtoull(e, nullptr, 0);
            }
            if (const char *f = std::getenv("PS2X_RR1_FROM"))
                c.from = std::strtoull(f, nullptr, 0);
            if (const char *t = std::getenv("PS2X_RR1_TO"))
                c.to = std::strtoull(t, nullptr, 0);
            return c;
        }();
        return cfg;
    }

    inline bool alphaTapOn() { return alphaCfg().on; }

    inline uint32_t eeFor(uint32_t off, const Ps2VifSrcSpan *spans, uint32_t n, uint32_t baseEe,
                          uint32_t *tagAt)
    {
        *tagAt = 0u;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (off >= spans[i].bufOff && off < spans[i].bufOff + spans[i].len)
            {
                *tagAt = spans[i].tagAt;
                return spans[i].eeAddr + (off - spans[i].bufOff);
            }
        }
        return n ? 0xFFFFFFFFu : baseEe + off;
    }

    inline void scanAlpha(uint64_t vsync, const char *chan, const uint8_t *buf, uint32_t len,
                          const Ps2VifSrcSpan *spans, uint32_t nspans, uint32_t baseEe)
    {
        const AlphaCfg &c = alphaCfg();
        if (!c.on || vsync < c.from || vsync > c.to || len < 16u)
            return;
        static std::atomic<uint32_t> lines{0};
        for (uint32_t o = 0; o + 16u <= len; o += 4u)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, buf + o, 8);
            std::memcpy(&hi, buf + o + 8, 8);
            if (lo != c.value || (hi != 0x42u && hi != 0x43u))
                continue;
            if (lines.fetch_add(1) >= 400u)
                return;
            uint32_t tagAt = 0u;
            const uint32_t ee = eeFor(o, spans, nspans, baseEe, &tagAt);
            std::fprintf(stderr, "[rr1:alpha] vs=%llu chan=%s off=%u ee=0x%08x tagAt=0x%08x reg=0x%llx val=0x%llx prev=",
                         static_cast<unsigned long long>(vsync), chan, o, ee, tagAt,
                         static_cast<unsigned long long>(hi), static_cast<unsigned long long>(lo));
            for (int k = 3; k >= 1; --k)
            {
                if (o >= 16u * static_cast<uint32_t>(k))
                {
                    uint64_t plo, phi;
                    std::memcpy(&plo, buf + o - 16u * k, 8);
                    std::memcpy(&phi, buf + o - 16u * k + 8, 8);
                    std::fprintf(stderr, "%016llx:%016llx ", static_cast<unsigned long long>(phi),
                                 static_cast<unsigned long long>(plo));
                }
            }
            std::fprintf(stderr, "\n");
        }
    }
}

#include <cstdarg>
namespace ps2_rr1
{
    // RR1 DEV-ONLY (default off): PS2X_RR1_EV=1 logs PATH3 gating events
    // ([rr1:ev] lines) inside PS2X_RR1_FROM..TO, at most 40000 lines.
    inline bool evOn()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_RR1_EV");
            return e && e[0] == '1';
        }();
        return on;
    }

    inline void ev(uint64_t tick, const char *fmt, ...)
    {
        if (!evOn())
            return;
        const AlphaCfg &c = alphaCfg();
        if (tick < c.from || tick > c.to)
            return;
        static std::atomic<uint32_t> lines{0};
        if (lines.fetch_add(1) >= 40000u)
            return;
        char body[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(body, sizeof(body), fmt, args);
        va_end(args);
        std::fprintf(stderr, "[rr1:ev] t=%llu %s\n", static_cast<unsigned long long>(tick), body);
    }
}
