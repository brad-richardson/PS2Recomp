#pragma once
// tricky-records-1 v3: a Tricky-mapped launch must never persist SSX 3
// records. Proven: a mapped ev14 launch boards the donor Top5 slot exactly
// like stock (trickymap card byte-identical to stockjam-boardsave), and the
// save is a single 3276-B McWrite (src=staging image, file SET0001; the
// fd varies with card state, so the gate is size+mapped, never fd).
// save9-vs-boardsave differ in EXACTLY 16 runs / 49 bytes (rank-shift
// PLAYER1-insertion: 24-bit LE scores, names, rank-pad bytes, the @1568
// score mirror, and the @3272 checksum); all other bytes are identical
// across every on-disk card. So instead of racing the guest serializer,
// stamp the 16 runs with save9's bytes over the staged image at the
// SET0001 McWrite (guest wrapper in the module, size-gated): the
// written image is byte-exact save9, checksum included, with NO checksum
// math and NO RAM snapshot. The write lands on post-compose staging
// scratch the game never rereads (single write per save); in-session
// board and results flow are untouched.
//
// Guest-affecting class, Tricky launches only: the predicate requires a
// mapped launch (course-map activeEvent >= 0; stock never maps). Knob
// PS2X_SSX3_TRICKY_RECORDS (default none = persist nothing; "exact" keeps
// the exact guest bytes for reference/comparison). Knob exact or stock:
// the wrapper chains immediately (det-identical).

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace ps2_ssx3_tricky_records
{

// The exact-path knob (lane-owned, default none). Cached: knobs never
// change mid-run.
inline bool exactPath()
{
    static const bool exact = [] {
        const char *v = std::getenv("PS2X_SSX3_TRICKY_RECORDS");
        return v && (std::strcmp(v, "exact") == 0 || std::strcmp(v, "off") == 0);
    }();
    return exact;
}

inline constexpr std::size_t kImageSize = 3276u;
// SET0001 (BASLUS-20772-SET0001) is written with this size on the
// observed save path (single 3276-B McWrite; icon.sys=964, ssx1.ico=73144,
// GAM0001=39777). The fd is card-state-dependent (3 on a fresh card,
// 6 with a profile loaded) and is never part of the gate.

struct PatchRun
{
    std::uint32_t off;
    std::uint32_t len;
    std::uint8_t bytes[8];
};

// Clean (save9) bytes at every image offset where a boarded save differs.
// Provenance: stockjam-save9-t22000 vs stockjam-boardsave-t24000 cards
// (49 differing bytes in 16 runs; everything else identical across all 10
// on-disk cards). Generated, not hand-copied (see lane REPORT).
inline constexpr PatchRun kPatchRuns[] = {
    {1348u, 3u, {0xb8, 0xf9, 0x02}},
    {1356u, 8u, {0x42, 0x4f, 0x54, 0x52, 0x4f, 0x53, 0x00, 0x00}},
    {1368u, 3u, {0xf0, 0x49, 0x02}},
    {1372u, 1u, {0x06}},
    {1376u, 6u, {0x4a, 0x55, 0x4e, 0x00, 0x00, 0x00}},
    {1388u, 3u, {0x28, 0x9a, 0x01}},
    {1392u, 1u, {0x07}},
    {1397u, 4u, {0x41, 0x4d, 0x45, 0x53}},
    {1408u, 2u, {0xd0, 0x7e}},
    {1412u, 1u, {0x08}},
    {1416u, 5u, {0x4b, 0x45, 0x4e, 0x00, 0x00}},
    {1428u, 2u, {0x98, 0x34}},
    {1432u, 1u, {0x09}},
    {1436u, 4u, {0x4d, 0x49, 0x4b, 0x45}},
    {1568u, 3u, {0xb8, 0xf9, 0x02}},
    {3272u, 2u, {0x0e, 0x13}},
};

inline constexpr std::size_t kPatchRunCount = sizeof(kPatchRuns) / sizeof(kPatchRuns[0]);

// Pure predicate (unit-tested): patch only a mapped launch's SET0001
// McWrite (size signature; every observed 3276-B write is the records
// file, and gameplay emits no McWrite traffic at all). exactOff carries
// the exact-path knob (caller reads exactPath()).
inline bool shouldPatch(bool mapped, bool exactOff, std::size_t size)
{
    if (!mapped || exactOff)
        return false;
    return size == kImageSize;
}

// Stamp the clean runs over a 3276-B staged image. Out-of-range runs are
// skipped, never clamped. Operates on a host-side copy; the guest image
// is never modified.
inline void patchImage(std::uint8_t *image, std::size_t size)
{
    if (!image)
        return;
    for (std::size_t i = 0; i < kPatchRunCount; ++i)
    {
        const PatchRun &r = kPatchRuns[i];
        if (r.len > sizeof(r.bytes) || r.off + r.len > size || r.off + r.len < r.off)
            continue;
        std::memcpy(image + r.off, r.bytes, r.len);
    }
}

} // namespace ps2_ssx3_tricky_records
