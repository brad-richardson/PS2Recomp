#pragma once
// HUD4 (from HUD3): GPU composite for the Tricky HUD (output-only). Draws
// the HUD scene (ps2_ssx3_tricky_hud.h HudScene) as GLES3 passes over the
// presented image: P0 blits the region to a temp texture, P1 draws the
// smears sampling the temp, P2 draws the sprite quads sampling the atlas
// texture (uploaded once), blended in draw order. Any failure returns false
// and the caller falls back to the CPU stamp, so the overlay never drops.
//
// iOS only: EAGL context + texture-cache frame texture
// (ps2_tricky_hud_gl_eagl). Android composites inside GE1 instead
// (ge1_gs_hud_scene): this Adreno EGL rejects AHB images (HUD3 §3), so the
// EGL glue is dropped. The pass logic, shaders and scene upload are shared
// (ps2_tricky_hud_gl).
#include <cstddef>
#include <cstdint>

namespace ps2_ssx3_tricky_hud
{
struct Atlas;
struct HudSprites;
struct HudVisualKey;
} // namespace ps2_ssx3_tricky_hud

struct HudGlBackend;

HudGlBackend *hudGlCreate(); // null when GLES3 is unavailable (fail-closed)
void hudGlDestroy(HudGlBackend *b);
// Composite one packet into the platform image (iOS: IOSurfaceRef), fw x fh.
// False on any failure (CPU fallback).
bool hudGlComposite(HudGlBackend *b, void *platformImage, int fw, int fh,
                    const ps2_ssx3_tricky_hud::Atlas &atlas,
                    const ps2_ssx3_tricky_hud::HudSprites &sprites,
                    const ps2_ssx3_tricky_hud::HudVisualKey &key);
// Drop cached GL objects for a retired platform image (pool retire).
void hudGlReleaseImage(HudGlBackend *b, void *platformImage);
// One-line stats for the periodic backend line (composites, avg us).
void hudGlStats(const HudGlBackend *b, char *out, unsigned size);

// --- lane diag (HUD4, diag knob; dropped or kept per the push gate) ---
// Composite into a transient RGBA scratch (full fw x fh, uploaded from
// fullFrame) with optional R/B-swapped shader math, and read the region
// back into outRegion (region w x h x 4, from the same scene). Lets the
// device prove the real GPU output (both channel orders) against the CPU
// stamp on real packets x real frames.
bool hudGlDiagScratch(HudGlBackend *b, const uint8_t *fullFrame, int fw, int fh, bool swapRB,
                      const ps2_ssx3_tricky_hud::Atlas &atlas,
                      const ps2_ssx3_tricky_hud::HudSprites &sprites,
                      const ps2_ssx3_tricky_hud::HudVisualKey &key, uint8_t *outRegion);
struct HudGlCompare
{
    uint64_t diffPx = 0;
    unsigned maxErr = 0;
    uint64_t firstOff = 0;
    uint8_t expFirst[4] = {0, 0, 0, 0};
    uint8_t gotFirst[4] = {0, 0, 0, 0};
};
// Compare two regions (w x h x 4, tight rows): differing pixels (any
// channel), max abs channel error, first difference.
HudGlCompare hudGlCompareRegion(const uint8_t *exp, const uint8_t *got, int w, int h);

// --- platform glue (one implementation per platform) ---
struct HudGlPlatform;
HudGlPlatform *hudGlPlatformCreate(); // context + ext checks; null on failure
void hudGlPlatformDestroy(HudGlPlatform *p);
bool hudGlPlatformMakeCurrent(HudGlPlatform *p);
// GL_TEXTURE_2D of the platform image (cached per image; 0 on failure).
// Sets rbNative when the texture stores R/B swapped vs RGBA (iOS BGRA).
unsigned hudGlPlatformFrameTexture(HudGlPlatform *p, void *platformImage, int w, int h,
                                   bool &rbNative);
void hudGlPlatformReleaseImage(HudGlPlatform *p, void *platformImage);
void hudGlPlatformFinish(HudGlPlatform *p); // glFinish + cache flush
