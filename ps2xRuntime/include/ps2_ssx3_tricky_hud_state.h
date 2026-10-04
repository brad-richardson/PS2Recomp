#pragma once
// TK43e: the TRICKY meter's per-frame state decision, shared by the GL call
// site (trickyHudOverlay in ps2_runtime.cpp, main thread) and the VK/AHB
// call site (queuePendingAhb in ps2_gs_external_backend.cpp, GS worker).
// Reads committed guest words; writes no guest memory, so the det-hash
// cannot move. Output-only, like the overlay itself.

#include "ps2_ssx3_course_manifest.h"
#include "ps2_ssx3_tricky_hud.h"
#include "ps2_ssx3_tricky_song.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2_ssx3_tricky_hud
{

// Caller holds st.mu. Decides whether this tick draws, and with what
// values; advances the letters machine, the first-full splash edge (which
// opens the song burst) and the atlas load. Every failure (knob off, Stock
// course, no art, unreadable rider, invalid words) returns draw=false.
inline HudParams updateHudStateLocked(HudState &st, const uint8_t *rdram, size_t ramSize, uint64_t tick)
{
    HudParams p;
    if (!st.wantedInit)
    {
        st.wantedInit = true;
        const char *env = std::getenv("PS2X_SSX3_TRICKY_HUD");
        st.wanted = env && env[0] == '1';
    }
    if (!st.wanted || !rdram || ramSize == 0u)
        return p;
    ps2_ssx3_course::Modes &ms = ps2_ssx3_course::courseModes();
    if (!ms.armed || ps2_ssx3_course::modeCurrent(ms, rdram) == 0u)
    {
        st.lastFull = false; // re-arm the first-full splash for the next race
        // TK43c: leaving Tricky mode resets the letters and swallows any
        // taps that landed outside Tricky courses (letters are Tricky-only).
        st.letters.seen = uberTap().count.load(std::memory_order_relaxed);
        st.letters.lit = 0;
        st.letters.flashUntil = 0;
        st.lettersInTricky = false;
        return p;
    }
    if (!st.atlasTried)
    {
        st.atlasTried = true;
        const char *art = std::getenv("PS2X_SSX3_TRICKY_HUD_ART");
        st.atlas = loadAtlasFile(art);
        std::fprintf(stderr, "[ssx3-tricky-hud] art '%s': %s\n", art ? art : "(unset)",
                     st.atlas.ok ? "loaded" : "missing/invalid, overlay off");
    }
    if (!st.atlas.ok)
        return p;
    if (!st.lettersPresetInit)
    {
        st.lettersPresetInit = true;
        st.lettersPreset = parseLettersPreset(std::getenv("PS2X_SSX3_TRICKY_LETTERS_PRESET"));
    }
    // TK43c: consume uber taps (even when the meter words are unreadable,
    // so no stale backlog lights letters late). Mode entry applies the
    // diag preset, if any; otherwise the session starts unlit.
    {
        const uint64_t taps = uberTap().count.load(std::memory_order_relaxed);
        if (!st.lettersInTricky)
        {
            st.lettersInTricky = true;
            st.letters.seen = taps;
            st.letters.lit = st.lettersPreset >= 0 ? st.lettersPreset : 0;
            st.letters.flashUntil = 0;
            if (st.lettersPreset >= 0)
                std::fprintf(stderr, "[ssx3-tricky-hud] letters preset=%d tick=%llu\n",
                             st.lettersPreset, static_cast<unsigned long long>(tick));
        }
        const int wasLit = st.letters.lit;
        const uint64_t wasFlash = st.letters.flashUntil;
        updateLetters(st.letters, taps, tick);
        if (st.letters.lit != wasLit && st.letters.lit > 0)
        {
            static const char kName[7] = "TRICKY";
            std::fprintf(stderr, "[ssx3-tricky-hud] letter %c lit (%d/6) tick=%llu\n",
                         kName[st.letters.lit - 1], st.letters.lit,
                         static_cast<unsigned long long>(tick));
        }
        if (st.letters.flashUntil != 0u && wasFlash == 0u)
            std::fprintf(stderr, "[ssx3-tricky-hud] TRICKY spelled: fanfare flash until=%llu\n",
                         static_cast<unsigned long long>(st.letters.flashUntil));
        if (st.letters.lit == 0 && wasLit == 6)
            std::fprintf(stderr, "[ssx3-tricky-hud] letters reset tick=%llu\n",
                         static_cast<unsigned long long>(tick));
    }
    // TK43d: PS2X_TK12_AP_PTR is an override when SET; otherwise the HUD
    // follows the game-owned chain (normal play sets no AP pointer).
    const char *apEnv = std::getenv("PS2X_TK12_AP_PTR");
    uint32_t apR = 0u;
    if (apEnv)
    {
        const uint32_t ptrAddr = static_cast<uint32_t>(std::strtoul(apEnv, nullptr, 0));
        const uint32_t slot = ptrAddr & kRamMask;
        if (slot + 4u <= ramSize)
            std::memcpy(&apR, rdram + slot, 4);
    }
    const uint32_t chainR = resolveChainR(rdram, ramSize);
    const uint32_t r = apEnv ? apR : chainR;
    const MeterFrame mf = readMeterFrameAt(rdram, ramSize, r);
    if (!mf.ok)
        return p;
    // TK43a Part 2 diag: forced DISPLAY values for screenshots (unlisted knob,
    // class diag; the guest words above are still only read, never written).
    if (!st.forcedInit)
    {
        st.forcedInit = true;
        st.forced = parseForce(std::getenv("PS2X_SSX3_TRICKY_HUD_FORCE"));
    }
    const float shownFill = st.forced.ok ? st.forced.fill : mf.fill;
    const bool full = (st.forced.ok ? st.forced.level : mf.level) >= 1;
    if (full && !st.lastFull)
    {
        st.splashUntil = tick + 45u;
        // TK43c: the meter's empty->full transition opens a ~5 s song burst
        // (no-op when no song is staged).
        ps2_ssx3_tricky_song::startBurst(tick);
    }
    st.lastFull = full;
    p.draw = true;
    p.atlas = &st.atlas;
    p.fill = shownFill;
    p.full = full;
    p.splashUntil = st.splashUntil;
    p.litLetters = st.letters.lit;
    p.flashUntil = st.letters.flashUntil;
    return p;
}

} // namespace ps2_ssx3_tricky_hud
