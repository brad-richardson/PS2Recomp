// tk-audio-1 step 3: Tricky-gated SSX 3 DJ/PA commentary.
//
// On Tricky courses the guest still posts stock radio/PA speech events
// (file-read schedule + guest-PCM NCC evidence in local/research/tk-audio-1):
// DJ_Artist_Intro_2_eng (race ticks 4327/4445/4525/4653) and PA_Venue_Intro_eng
// (race start 2459/2561) are present in the mix under the Tricky base song.
// Only event IDs with a TK25-established bank mapping are gated; banks whose
// event ID is unknown (DJ_Radio_Big_Intro, DJ_Aggression, PA_Sponsor_Intro)
// are left alone and reported as residual risk.
#pragma once

#include <cstdint>
#include <cstdlib>

namespace ps2_ssx3_tricky_dj
{
// Speech-queue event IDs (guest 0x2B1458 a2; TK25 mechanism anchors).
constexpr uint32_t kDjArtistIntroEvent = 0x20cfu; // DJ_Artist_Intro bank
constexpr uint32_t kPaVenueIntroEvent = 0x20bbu;  // PA_Venue_Intro bank

inline bool isGatedEvent(uint32_t a2)
{
    return a2 == kDjArtistIntroEvent || a2 == kPaVenueIntroEvent;
}

// PS2X_SSX3_TRICKY_DJ_GATE=1 drops the gated posts on Tricky courses.
// Default off: the wrapper calls the original for every post.
inline bool gateEnabled()
{
    const char *env = std::getenv("PS2X_SSX3_TRICKY_DJ_GATE");
    return env && env[0] == '1';
}
} // namespace ps2_ssx3_tricky_dj
