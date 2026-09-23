// E40 Part-3: EE source span for bytes appended to a VIF DMA chain buffer.
//
// Standalone (no runtime dependencies) so both runtime/ps2_memory.h
// (PendingTransfer cargo) and ps2_mpg_src_trace.h (active-map lookup)
// can include it without an include cycle.

#pragma once

#include <cstdint>

struct Ps2VifSrcSpan
{
    uint32_t bufOff = 0u; // offset in the delivered buffer
    uint32_t len = 0u;    // span length in bytes
    uint32_t eeAddr = 0u; // EE address of the first byte
    int32_t tagId = -1;   // chain tag id, or -1 when not from a tag
    uint32_t tagAt = 0u;  // EE address of the tag, 0 when none
};
