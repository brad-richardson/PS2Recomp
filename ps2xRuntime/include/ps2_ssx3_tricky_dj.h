// tk-audio-1 step 3: Tricky-gated SSX 3 DJ/PA commentary.
//
// On Tricky courses the guest still posts stock radio/PA speech events
// (file-read schedule in local/research/tk-audio-1): DJ_Artist_Intro_2_eng,
// DJ_Radio_Big_Intro_eng, DJ_Aggression_eng, PA_Venue_Intro_eng and
// PA_Sponsor_Intro_eng are all fetched mid-race under the Tricky base song.
// Event IDs come from the guest's own 0x2B1458 post args (FH1 tap): 0x20cf
// precedes the Artist reads, 0x20e5 precedes the Radio reads, 0x20bb precedes
// the Venue reads. Aggression/Sponsor reads share those posts (one post can
// fetch a bundle), so gating the three IDs covers every DJ/PA bank read
// observed on either race route.
#pragma once

#include <cstdint>
#include <cstdlib>

namespace ps2_ssx3_tricky_dj
{
// Speech-queue event IDs (guest 0x2B1458 a2; post-arg mapping, tk-audio-1).
constexpr uint32_t kDjArtistIntroEvent = 0x20cfu; // DJ_Artist_Intro_2 bank
constexpr uint32_t kPaVenueIntroEvent = 0x20bbu;  // PA_Venue_Intro bank
constexpr uint32_t kDjRadioIntroEvent = 0x20e5u;  // DJ_Radio_Big_Intro bank

inline bool isGatedEvent(uint32_t a2)
{
    return a2 == kDjArtistIntroEvent || a2 == kPaVenueIntroEvent || a2 == kDjRadioIntroEvent;
}

// PS2X_SSX3_TRICKY_DJ_GATE=1 drops the gated posts on Tricky courses.
// Default off: the wrapper calls the original for every post.
inline bool gateEnabled()
{
    const char *env = std::getenv("PS2X_SSX3_TRICKY_DJ_GATE");
    return env && env[0] == '1';
}
} // namespace ps2_ssx3_tricky_dj
