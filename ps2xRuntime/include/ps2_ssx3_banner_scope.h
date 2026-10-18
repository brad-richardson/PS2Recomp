#pragma once
// tk-banner-2: mode-scoped race-banner titles + brand + album.
//
// tk-banner-1 installed a boot-scoped MUSIC.INF alias plus a rodata brand
// blank (both gated on TRICKY_MENU=1 + TRICKY_MUSIC_INF), but the guest
// parses MUSIC.INF once at boot and caches per-slot TITLE/ARTIST, and stock
// and Tricky pools share the 35 MUSDATA basenames: one boot file cannot be
// correct for both, so stock races showed Tricky titles with a blank brand
// (tk-banner-2 repro: Happiness showed blank/"Adam's Revenge"; expected
// "EA RADIO BIG"/"Ride"/"Deepsky").
//
// Forensics (savestate scans, REPORT): the cache is three 35x100B
// NUL-padded tables in INF section order (titles, artists, albums). The
// boot serves the stock INF (P13); the rewrite installs all three tables
// per mode, so stock races get exact stock banners (brand/title/artist/
// album) and Tricky races get Tricky titles/artists with blank albums (no
// album data on Tricky's disc). The Tricky album set is all empty, so the
// album table is discovered best-effort via the stock order.
//
// Rule (pool-aware): titles/artists/albums/brand follow the pool that
// will serve the race. The Tricky set installs only when the target mode
// aliases MUSIC.BIG to an existing host file (Android pool; the overlay
// carries no pool files). Otherwise the stock set installs (stock
// courses, iOS which ships no pool by design, missing composites). Table
// bases are discovered by strict order+stride scan, never assumed: a
// guest-side move degrades to a loud refusal with no writes. Knob off
// (either env unset, or no armed modes): no reads, no writes
// (det-IDENTICAL).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "ps2_ssx3_banner_tables.h"

namespace ps2_ssx3_course {
namespace banner {

inline constexpr size_t kRecords = 35u;
inline constexpr size_t kField = 100u;
inline constexpr uint32_t kNoTable = 0xFFFFFFFFu;
inline constexpr uint32_t kRamBytes = 32u * 1024u * 1024u;
inline constexpr uint32_t kBrandAddr = 0x46EAE8u;
inline constexpr char kBrand[] = "EA RADIO BIG";
inline constexpr char kBlank[] = "            "; // 12 spaces, same width as kBrand
static_assert(sizeof(kBrand) == sizeof(kBlank), "brand and blank share the field width");

// Master switch for the banner scoping (same knobs the play env sets).
// Unsetting PS2X_SSX3_TRICKY_MUSIC_INF disables every banner write: the
// boot serves the stock INF and the cache stays stock (env-only fallback).
inline bool knob()
{
    const char *menu = std::getenv("PS2X_SSX3_TRICKY_MENU");
    const char *host = std::getenv("PS2X_SSX3_TRICKY_MUSIC_INF");
    return menu && std::strcmp(menu, "1") == 0 && host && *host;
}

inline bool fieldFits(const char *s)
{
    return std::strlen(s) < kField;
}

// True when all 35 records prefix-match with NUL terminators at the stride.
inline bool tableMatches(const uint8_t *ram, uint32_t base, const char *const *titles)
{
    if (base + kRecords * kField > kRamBytes)
        return false;
    for (size_t i = 0; i < kRecords; ++i)
    {
        const size_t tl = std::strlen(titles[i]);
        const uint8_t *rec = ram + base + i * kField;
        if (std::memcmp(rec, titles[i], tl) != 0 || rec[tl] != 0)
            return false;
    }
    return true;
}

// Strict order+stride discovery: every record must prefix-match its title
// with a NUL terminator (rejects the raw INF text, where titles sit inside
// TITLE = "..." lines). Anchors on the first non-empty record (some sets
// open with empty strings) and verifies all 35 at the 100B stride.
// Returns the table base or kNoTable.
inline uint32_t findTable(const uint8_t *ram, const char *const *titles)
{
    const size_t span = kRecords * kField;
    size_t anchor = 0;
    while (anchor < kRecords && titles[anchor][0] == 0)
        ++anchor;
    if (anchor == kRecords)
        return kNoTable;
    const char *first = titles[anchor];
    const size_t fl = std::strlen(first);
    const size_t prefix = anchor * kField;
    uint32_t off = 0;
    while (off + span <= kRamBytes)
    {
        const void *hit = std::memchr(ram + off, first[0], kRamBytes - span - off);
        if (!hit)
            return kNoTable;
        const uint32_t found = static_cast<uint32_t>(static_cast<const uint8_t *>(hit) - ram);
        if (found < prefix)
        {
            off = found + 1;
            continue;
        }
        off = found - static_cast<uint32_t>(prefix);
        if (tableMatches(ram, off, titles))
            return off;
        off = found + 1;
    }
    return kNoTable;
}

// Anchor record used by the fast path (first non-empty entry of a set).
inline size_t anchorIndex(const char *const *titles)
{
    size_t anchor = 0;
    while (anchor < kRecords && titles[anchor][0] == 0)
        ++anchor;
    return anchor;
}

// Which set the cached tables hold: 1 tricky, 0 stock, -1 neither/torn
// (a savestate load or guest reallocation leaves stale bases behind;
// callers re-discover on -1 instead of writing to them). The album table
// is checked when its base is known; the Tricky album set is all empty,
// so a Tricky-cached album table reads as 3500 zero bytes.
inline bool albumsMatch(const uint8_t *ram, uint32_t albumBase, bool tricky)
{
    if (albumBase == kNoTable)
        return true;
    if (albumBase + kRecords * kField > kRamBytes)
        return false;
    if (tricky)
    {
        for (size_t i = 0; i < kRecords * kField; ++i)
            if (ram[albumBase + i] != 0)
                return false;
        return true;
    }
    return tableMatches(ram, albumBase, kBannerStockAlbum);
}

inline int cachedSet(const uint8_t *ram, uint32_t titleBase, uint32_t artistBase, uint32_t albumBase)
{
    const bool tt = tableMatches(ram, titleBase, kBannerTrickyTitle);
    const bool ta = tableMatches(ram, artistBase, kBannerTrickyArtist);
    const bool st = tableMatches(ram, titleBase, kBannerStockTitle);
    const bool sa = tableMatches(ram, artistBase, kBannerStockArtist);
    if (tt && ta && albumsMatch(ram, albumBase, true))
        return 1;
    if (st && sa && albumsMatch(ram, albumBase, false))
        return 0;
    return -1;
}

struct SyncState
{
    uint32_t titleBase = kNoTable;
    uint32_t artistBase = kNoTable;
    uint32_t albumBase = kNoTable; // best-effort: see syncForMode
    int set = -1; // -1 unknown, 0 stock, 1 tricky
    uint64_t nextTryTick = 0;
    bool refusedLogged = false;
};

inline SyncState &syncState()
{
    static SyncState s;
    return s;
}

inline void resetForTest()
{
    syncState() = SyncState{};
}

// Tricky set only when the target mode serves the Tricky pool: a MUSIC.BIG
// mode alias whose host file exists. (The CD overlay carries worlds/UI
// only, so the alias is the sole pool path.)
template <typename ModeT>
inline bool poolLive(const ModeT *target)
{
    if (!target)
        return false;
    for (const auto &a : target->aliases)
    {
        if (a.disc != "/DATA/AUDIO/MUSIC.BIG")
            continue;
        // Silent: the caller logs the fallback when it applies a set.
        std::error_code ec;
        return std::filesystem::is_regular_file(a.host, ec);
    }
    return false;
}

// True when the mode's pool alias names a missing host (diagnostic only).
template <typename ModeT>
inline bool poolHostMissing(const ModeT *target)
{
    if (!target)
        return false;
    for (const auto &a : target->aliases)
    {
        if (a.disc != "/DATA/AUDIO/MUSIC.BIG")
            continue;
        std::error_code ec;
        return !std::filesystem::is_regular_file(a.host, ec);
    }
    return false;
}

inline bool ramHasN(const uint8_t *ram, uint32_t addr, const char *s)
{
    return std::memcmp(ram + addr, s, std::strlen(s)) == 0 && ram[addr + std::strlen(s)] == 0;
}

// Overwrite the tables with one set plus the brand line. Every field is
// length-guarded; any refusal writes nothing further and returns false.
// albumBase == kNoTable skips album writes (best-effort discovery).
inline bool applySet(uint8_t *ram, bool tricky, uint32_t titleBase, uint32_t artistBase, uint32_t albumBase,
                     std::vector<std::string> *writes)
{
    const char *const *T = tricky ? kBannerTrickyTitle : kBannerStockTitle;
    const char *const *A = tricky ? kBannerTrickyArtist : kBannerStockArtist;
    const char *const *L = tricky ? kBannerTrickyAlbum : kBannerStockAlbum;
    for (size_t i = 0; i < kRecords; ++i)
    {
        if (!fieldFits(T[i]) || !fieldFits(A[i]) || !fieldFits(L[i]))
        {
            std::fprintf(stderr, "[ssx3-course] banner: refusing over-long %s record %zu\n",
                         tricky ? "tricky" : "stock", i);
            return false;
        }
    }
    for (size_t i = 0; i < kRecords; ++i)
    {
        const size_t tl = std::strlen(T[i]);
        std::memcpy(ram + titleBase + i * kField, T[i], tl);
        std::memset(ram + titleBase + i * kField + tl, 0, kField - tl);
        const size_t al = std::strlen(A[i]);
        std::memcpy(ram + artistBase + i * kField, A[i], al);
        std::memset(ram + artistBase + i * kField + al, 0, kField - al);
        if (albumBase != kNoTable)
        {
            const size_t ll = std::strlen(L[i]);
            std::memcpy(ram + albumBase + i * kField, L[i], ll);
            std::memset(ram + albumBase + i * kField + ll, 0, kField - ll);
        }
    }
    const size_t brandLen = sizeof(kBrand) - 1;
    if (tricky)
    {
        if (std::memcmp(ram + kBrandAddr, kBrand, brandLen) == 0)
            std::memcpy(ram + kBrandAddr, kBlank, brandLen);
        else if (std::memcmp(ram + kBrandAddr, kBlank, brandLen) != 0)
        {
            std::fprintf(stderr, "[ssx3-course] banner: brand holds neither brand nor blank, leaving it\n");
            return false;
        }
    }
    else
    {
        if (std::memcmp(ram + kBrandAddr, kBlank, brandLen) == 0)
            std::memcpy(ram + kBrandAddr, kBrand, brandLen);
        else if (std::memcmp(ram + kBrandAddr, kBrand, brandLen) != 0)
        {
            std::fprintf(stderr, "[ssx3-course] banner: brand holds neither blank nor brand, leaving it\n");
            return false;
        }
    }
    if (writes)
    {
        char buf[160];
        if (albumBase != kNoTable)
            std::snprintf(buf, sizeof(buf), "banner titles: %s set @0x%06x/0x%06x/0x%06x + brand %s",
                          tricky ? "tricky" : "stock", titleBase, artistBase, albumBase,
                          tricky ? "blank" : "restored");
        else
            std::snprintf(buf, sizeof(buf), "banner titles: %s set @0x%06x/0x%06x (no album table) + brand %s",
                          tricky ? "tricky" : "stock", titleBase, artistBase, tricky ? "blank" : "restored");
        writes->push_back(buf);
    }
    return true;
}

// Discover both tables (trying the Tricky order first, then stock: the
// cache holds whichever set was last written) and install `tricky`. Loud
// refusal with no writes when the layout is unrecognized.
template <typename ModeT>
inline bool syncForMode(uint8_t *ram, const ModeT *target, std::vector<std::string> *writes)
{
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    SyncState &st = syncState();
    const bool tricky = poolLive(target);
    // Validate the destination before every rewrite: a savestate load or
    // guest reallocation leaves stale bases behind. Re-discover on any
    // mismatch instead of writing to them. The album table is
    // best-effort (a Tricky-cached table of zeros is not locatable by
    // content); titles/artists still heal without it.
    if (st.titleBase == kNoTable || st.artistBase == kNoTable ||
        cachedSet(ram, st.titleBase, st.artistBase, st.albumBase) < 0)
    {
        st.titleBase = kNoTable;
        st.artistBase = kNoTable;
        st.albumBase = kNoTable;
        uint32_t t = findTable(ram, kBannerTrickyTitle);
        if (t == kNoTable)
            t = findTable(ram, kBannerStockTitle);
        uint32_t a = findTable(ram, kBannerTrickyArtist);
        if (a == kNoTable)
            a = findTable(ram, kBannerStockArtist);
        if (t == kNoTable || a == kNoTable)
        {
            std::fprintf(stderr, "[ssx3-course] banner: tables not found, keeping boot titles\n");
            return false;
        }
        uint32_t l = findTable(ram, kBannerStockAlbum);
        if (l == kNoTable || cachedSet(ram, t, a, l) < 0)
        {
            if (l == kNoTable)
                std::fprintf(stderr, "[ssx3-course] banner: album table not found, scoping titles only\n");
            else
            {
                std::fprintf(stderr, "[ssx3-course] banner: tables disagree, keeping boot titles\n");
                return false;
            }
        }
        else
            st.albumBase = l;
        st.titleBase = t;
        st.artistBase = a;
    }
    if (!tricky && poolHostMissing(target))
    {
        std::fprintf(stderr, "[ssx3-course] banner: pool host missing, keeping stock titles\n");
    }
    if (!applySet(ram, tricky, st.titleBase, st.artistBase, st.albumBase, writes))
        return false;
    st.set = tricky ? 1 : 0;
    return true;
}

// Lazy post-parse sync for the boot window: the guest parses MUSIC.INF
// after loadELF (before the first menu), so the first sync can only run
// once the tables exist. Called on pad reads while modes are armed; scans
// at most every 300 ticks until discovered, then fast-paths on record 0
// (the only writer is syncForMode, so a record-0 match means in sync; a
// savestate load shows up as a mismatch and re-syncs).
// Fast-path equality: anchor records plus the brand line (a savestate
// load can restore matching titles with a foreign brand).
inline bool inSync(const uint8_t *ram, const SyncState &st, bool tricky)
{
    if (st.titleBase == kNoTable || st.artistBase == kNoTable || st.set != (tricky ? 1 : 0))
        return false;
    const char *const *T = tricky ? kBannerTrickyTitle : kBannerStockTitle;
    const char *const *A = tricky ? kBannerTrickyArtist : kBannerStockArtist;
    const size_t ta = anchorIndex(T);
    const size_t aa = anchorIndex(A);
    if (!ramHasN(ram, st.titleBase + static_cast<uint32_t>(ta * kField), T[ta]))
        return false;
    if (!ramHasN(ram, st.artistBase + static_cast<uint32_t>(aa * kField), A[aa]))
        return false;
    if (st.albumBase != kNoTable)
    {
        if (tricky)
        {
            for (size_t i = 0; i < kRecords * kField; ++i)
                if (ram[st.albumBase + i] != 0)
                    return false;
        }
        else if (!ramHasN(ram, st.albumBase, kBannerStockAlbum[0]))
            return false;
    }
    const size_t brandLen = sizeof(kBrand) - 1;
    if (tricky)
        return std::memcmp(ram + kBrandAddr, kBlank, brandLen) == 0;
    return std::memcmp(ram + kBrandAddr, kBrand, brandLen) == 0;
}

template <typename ModesT>
inline void ensureSynced(uint8_t *ram, const ModesT &ms, uint64_t tick)
{
    if (!knob())
        return;
    SyncState &st = syncState();
    const size_t cur = modeCurrent(ms, ram);
    const auto *target = cur ? &ms.modes[cur - 1u] : nullptr;
    const bool tricky = poolLive(target);
    if (inSync(ram, st, tricky))
        return;
    if (tick < st.nextTryTick)
        return;
    st.nextTryTick = tick + 300;
    std::vector<std::string> writes;
    if (syncForMode(ram, target, &writes))
    {
        for (const std::string &w : writes)
            std::fprintf(stderr, "[ssx3-course] tick=%llu %s\n", static_cast<unsigned long long>(tick), w.c_str());
    }
    else if (!st.refusedLogged)
    {
        st.refusedLogged = true;
        std::fprintf(stderr, "[ssx3-course] tick=%llu banner sync refused (keeping boot titles)\n",
                     static_cast<unsigned long long>(tick));
    }
}

} // namespace banner
} // namespace ps2_ssx3_course
