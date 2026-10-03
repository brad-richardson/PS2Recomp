#pragma once

// Pure arithmetic of the full-120 events restamp of the 0x1e1458 periodic
// stamp (ps2_fh1_full120.h restamp10s). The guest fires 0x1e14c0(1) when
// int32(A+0x1c - stamp) >= 601 (signed slti at 0x1e1494) and restamps with the
// update count. Active mode counts 120 Hz updates: every new stamp is shifted
// +601, so a period is 1202 active updates (10 s). Separate header so the unit
// tests need no runtime.

#include <cstdint>

namespace ps2_fh1_restamp
{
    inline constexpr int32_t kPeriod = 601;

    // Entry: E elapsed stock updates of 601 -> 2E of 1202 active updates. The
    // stamp lands in the future (stamp' = upd + 601 - 2E); a negative elapsed
    // (already-future stamp) counts as 0.
    constexpr uint32_t enter(uint32_t upd, uint32_t stamp)
    {
        int32_t e = static_cast<int32_t>(upd - stamp);
        if (e < 0) e = 0;
        if (e > kPeriod) e = kPeriod;
        return upd + static_cast<uint32_t>(kPeriod) - 2u * static_cast<uint32_t>(e);
    }

    // Exit, FH5 rule: negative elapsed clamped to 0 before the conversion, so
    // a future stamp (every exit within 601 active updates of an entry or an
    // active restamp) keeps only 300 stock updates (FCR1 §3).
    constexpr uint32_t exitClamped(uint32_t upd, uint32_t stamp)
    {
        int32_t e = static_cast<int32_t>(upd - stamp);
        if (e < 0) e = 0;
        int32_t r = kPeriod - e;
        if (r < 0) r = 0;
        return upd - static_cast<uint32_t>(kPeriod) + static_cast<uint32_t>(r / 2);
    }

    // Exit, FH26 clocksign: keep the signed elapsed e in [-601, 601]. Remaining
    // active updates R = 601 - e in [0, 1202] -> ceil(R/2) stock updates. Odd R
    // means half a stock tick ran: rounding up never fires before the stock
    // deadline (an unfinished half tick does not count as elapsed stock time),
    // and an entry followed by an exit with no active update is exact (R even).
    constexpr uint32_t exitSigned(uint32_t upd, uint32_t stamp)
    {
        int32_t e = static_cast<int32_t>(upd - stamp);
        if (e < -kPeriod) e = -kPeriod;
        if (e > kPeriod) e = kPeriod;
        const int32_t r = kPeriod - e;
        return upd - static_cast<uint32_t>(kPeriod) + static_cast<uint32_t>((r + 1) / 2);
    }

    // Stock updates until the next fire (the first update with upd - stamp >= 601).
    constexpr int32_t stockRemaining(uint32_t upd, uint32_t stamp)
    {
        return kPeriod - static_cast<int32_t>(upd - stamp);
    }
}
