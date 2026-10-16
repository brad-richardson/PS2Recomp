// snd-seam-1 family #8: Tricky-state persistence owned by the module.
// The "trickygems" section (name, version, registration) lives here in the
// K1 static-registration pattern, and the TK34 patch-grow kernel-section
// field is provided here (save/validate/load at the exact field position).
// Bodies moved verbatim; serialized bytes are unchanged, so old saves keep
// loading. Without a module the gems section is absent and the kernel
// patch-grow field fails closed (a no-module binary never round-trips
// game state; the suites prove it).
#include "Ssx3TrickyState.h"

#include "ps2_ssx3_patch_grow_state.h"
#include "ps2_ssx3_tricky_gems.h"
#include "ps2_ssx3_tricky_layer.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_savestate.h"

#include <cmath>
#include <cstdint>

namespace
{
using ps2_savestate::Reader;
using ps2_savestate::Writer;
} // namespace

namespace ps2_tricky_state
{

void trickyGemsSave(Writer &w)
{
    const auto &s = ps2_tk45c::state();
    w.b(ps2_ssx3_tricky_layer::config().gems);
    w.u32(static_cast<uint32_t>(ps2_ssx3_tricky_layer::config().gems ? ps2_tk45c::table().size() : 0u));
    for (bool b : {s.init, s.wasTricky, s.prevValid, s.scanDone, s.pendingClear, s.rehideArmed,
                   s.raceClock.init, s.raceClock.seen, s.spinLogged}) w.b(b);
    w.bytes(s.prev, sizeof(s.prev));
    w.u32(s.lastR); w.u32(s.lastClock); w.u32(s.scanCursor);
    w.u32(s.raceClock.last); w.u64(s.raceClock.lastTick); w.u64(s.raceClock.adoptUntil);
    w.bytes(s.collected, sizeof(s.collected));
    w.bytes(s.spinSeen, sizeof(s.spinSeen));
}

bool trickyGemsLoad(Reader &r)
{
    ps2_tk45c::State next{};
    const bool savedEnabled = r.b();
    const uint32_t savedCount = r.u32();
    const bool nowEnabled = ps2_ssx3_tricky_layer::config().gems;
    if (savedEnabled != nowEnabled ||
        savedCount != (nowEnabled ? ps2_tk45c::table().size() : 0u))
        return r.fail("trickygems: mode or table size differs");
    next.init = r.b(); next.wasTricky = r.b(); next.prevValid = r.b();
    next.scanDone = r.b(); next.pendingClear = r.b(); next.rehideArmed = r.b();
    next.raceClock.init = r.b(); next.raceClock.seen = r.b(); next.spinLogged = r.b();
    r.bytes(next.prev, sizeof(next.prev));
    next.lastR = r.u32(); next.lastClock = r.u32(); next.scanCursor = r.u32();
    next.raceClock.last = r.u32(); next.raceClock.lastTick = r.u64();
    next.raceClock.adoptUntil = r.u64();
    r.bytes(next.collected, sizeof(next.collected));
    r.bytes(next.spinSeen, sizeof(next.spinSeen));
    if (!r.ok()) return false;
    if (next.scanCursor > PS2_RAM_SIZE || next.scanCursor % ps2_tk45c::kScanChunk != 0u ||
        next.scanDone != (next.scanCursor == PS2_RAM_SIZE))
        return r.fail("trickygems: invalid scan cursor");
    for (float x : next.prev)
        if (!std::isfinite(x)) return r.fail("trickygems: invalid segment");
    if (!r.validationOnly())
        ps2_tk45c::state() = next; // derived lookup is rebuilt from restored RAM after all sections.
    return true;
}

void patchGrowSave(ps2_savestate::Writer &w)
{
    const auto growLayout = ps2_ssx3_patch_grow::layout();
    const auto growState = ps2_ssx3_patch_grow::snapshot();
    w.u32(growLayout.base);
    w.u32(growLayout.capacity);
    w.b(growState.active);
    w.b(growState.refused);
    w.u32(growState.cache);
    w.u64(growState.moved.size());
    for (const auto &[to, from] : growState.moved)
    {
        w.u32(to);
        w.u32(from);
    }
}

bool patchGrowValidate(ps2_savestate::Reader &r)
{
    const auto layout = ps2_ssx3_patch_grow::layout();
    const uint32_t base = r.u32(), capacity = r.u32();
    if (base != layout.base || capacity != layout.capacity)
        return r.fail("TK34 grow layout differs");
    ps2_ssx3_patch_grow::State grow;
    grow.active = r.b(); grow.refused = r.b(); grow.cache = r.u32();
    grow.moved.resize(static_cast<size_t>(r.count(8u)));
    for (auto &[to, from] : grow.moved) { to = r.u32(); from = r.u32(); }
    if (!r.ok() || !ps2_ssx3_patch_grow::valid(grow))
        return r.fail("invalid TK34 grow state");
    return r.ok();
}

bool patchGrowLoad(ps2_savestate::Reader &r)
{
    const auto growLayout = ps2_ssx3_patch_grow::layout();
    const uint32_t savedGrowBase = r.u32();
    const uint32_t savedGrowCapacity = r.u32();
    if (savedGrowBase != growLayout.base || savedGrowCapacity != growLayout.capacity)
        return r.fail("TK34 grow layout differs");
    ps2_ssx3_patch_grow::State growState;
    growState.active = r.b();
    growState.refused = r.b();
    growState.cache = r.u32();
    growState.moved.resize(static_cast<size_t>(r.count(8u)));
    for (auto &[to, from] : growState.moved)
    {
        to = r.u32();
        from = r.u32();
    }
    if (!r.ok() || !ps2_ssx3_patch_grow::restore(growState))
        return r.fail("invalid TK34 grow state");
    return true;
}

} // namespace ps2_tricky_state

namespace
{
// K1 pattern: static registration in the family TU (this TU is always
// linked — it defines the module entry — and section order is key-sorted,
// so registration timing is inert). Version stays 1: old saves keep
// loading through the unchanged provider bodies.
const bool kTrickyGemsRegistered = ps2_savestate::registerSection(
    "trickygems",
    {1u, &ps2_tricky_state::trickyGemsSave, &ps2_tricky_state::trickyGemsLoad, nullptr, 0u, false,
     &ps2_tricky_state::trickyGemsLoad});
} // namespace
