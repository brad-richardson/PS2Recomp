#pragma once
// tk-banner-2: mode-scoped race-banner titles + brand.
//
// tk-banner-1 installed a boot-scoped MUSIC.INF alias plus a rodata brand
// blank (both gated on TRICKY_MENU=1 + TRICKY_MUSIC_INF), but the guest
// parses MUSIC.INF once at boot and caches per-slot TITLE/ARTIST, and stock
// and Tricky pools share the 35 MUSDATA basenames: one boot file cannot be
// correct for both, so stock races showed Tricky titles with a blank brand
// (tk-banner-2 repro: Happiness showed blank/"Adam's Revenge"; expected
// "EA RADIO BIG"/"Ride"/"Deepsky").
//
// Forensics (savestate scan, REPORT): the cache is two 35x100B NUL-padded
// tables in INF section order (titles, then artists); no album table
// nearby (the banner's ALBUM fallback reads the boot-served raw file, so
// the boot keeps serving the Tricky composite whose ALBUMs are cleared).
// No stock pool ARTIST is empty, so title+artist rewrites fully determine
// stock banners.
//
// Rule (pool-aware): titles/artists/brand follow the pool that will serve
// the race. The Tricky set installs only when the target mode aliases
// MUSIC.BIG to an existing host file (Android pool; the overlay carries no
// pool files). Otherwise the stock set installs (stock courses, iOS which
// ships no pool by design, missing composites). Table bases are discovered
// by strict order+stride scan, never assumed: a guest-side move degrades to
// a loud refusal with no writes. Knob off (either env unset, or no armed
// modes): no reads, no writes (det-IDENTICAL).
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
inline constexpr uint32_t kBrandAddr = 0x46EAE8u;
inline constexpr char kBrand[] = "EA RADIO BIG";

// Same guard as bootBannerFromEnv: without the boot-served Tricky INF the
// cache is stock and there is nothing to scope.
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

// Strict order+stride discovery: every record must prefix-match its title
// with a NUL terminator (rejects the raw INF text, where titles sit inside
// TITLE = "..." lines). Returns the table base or kNoTable.
inline uint32_t findTable(const uint8_t *ram, const char *const *titles)
{
    static constexpr uint32_t kRamBytes = 32u * 1024u * 1024u;
    const size_t span = kRecords * kField;
    const char *first = titles[0];
    const size_t fl = std::strlen(first);
    uint32_t off = 0;
    while (off + span <= kRamBytes)
    {
        const void *hit = std::memchr(ram + off, first[0], kRamBytes - span - off);
        if (!hit)
            return kNoTable;
        off = static_cast<uint32_t>(static_cast<const uint8_t *>(hit) - ram);
        bool ok = std::memcmp(ram + off, first, fl) == 0 && ram[off + fl] == 0;
        for (size_t i = 0; ok && i < kRecords; ++i)
        {
            const size_t tl = std::strlen(titles[i]);
            const uint8_t *rec = ram + off + i * kField;
            ok = std::memcmp(rec, titles[i], tl) == 0 && rec[tl] == 0;
        }
        if (ok)
            return off;
        ++off;
    }
    return kNoTable;
}

struct SyncState
{
    uint32_t titleBase = kNoTable;
    uint32_t artistBase = kNoTable;
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
        std::error_code ec;
        if (std::filesystem::is_regular_file(a.host, ec))
            return true;
        std::fprintf(stderr, "[ssx3-course] banner: pool host missing, keeping stock titles (%s)\n",
                     a.host.c_str());
        return false;
    }
    return false;
}

inline bool ramHasN(const uint8_t *ram, uint32_t addr, const char *s)
{
    return std::memcmp(ram + addr, s, std::strlen(s)) == 0 && ram[addr + std::strlen(s)] == 0;
}

// Overwrite both tables with one set plus the brand line. Every field is
// length-guarded; any refusal writes nothing further and returns false.
inline bool applySet(uint8_t *ram, bool tricky, uint32_t titleBase, uint32_t artistBase,
                     std::vector<std::string> *writes)
{
    const char *const *T = tricky ? kBannerTrickyTitle : kBannerStockTitle;
    const char *const *A = tricky ? kBannerTrickyArtist : kBannerStockArtist;
    for (size_t i = 0; i < kRecords; ++i)
    {
        if (!fieldFits(T[i]) || !fieldFits(A[i]))
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
    }
    const size_t brandLen = sizeof(kBrand) - 1;
    if (tricky)
    {
        if (std::memcmp(ram + kBrandAddr, kBrand, brandLen) == 0)
            std::memset(ram + kBrandAddr, ' ', brandLen);
        else if (std::memcmp(ram + kBrandAddr, "            ", brandLen) != 0)
        {
            std::fprintf(stderr, "[ssx3-course] banner: brand holds neither brand nor blank, leaving it\n");
            return false;
        }
    }
    else
    {
        if (std::memcmp(ram + kBrandAddr, "            ", brandLen) == 0)
            std::memcpy(ram + kBrandAddr, kBrand, brandLen);
        else if (std::memcmp(ram + kBrandAddr, kBrand, brandLen) != 0)
        {
            std::fprintf(stderr, "[ssx3-course] banner: brand holds neither blank nor brand, leaving it\n");
            return false;
        }
    }
    if (writes)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "banner titles: %s set @0x%06x/0x%06x + brand %s", tricky ? "tricky" : "stock",
                      titleBase, artistBase, tricky ? "blank" : "restored");
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
    if (st.titleBase == kNoTable)
    {
        uint32_t t = findTable(ram, kBannerTrickyTitle);
        if (t == kNoTable)
            t = findTable(ram, kBannerStockTitle);
        if (t == kNoTable)
        {
            std::fprintf(stderr, "[ssx3-course] banner: title table not found, keeping boot titles\n");
            return false;
        }
        st.titleBase = t;
    }
    if (st.artistBase == kNoTable)
    {
        uint32_t a = findTable(ram, kBannerTrickyArtist);
        if (a == kNoTable)
            a = findTable(ram, kBannerStockArtist);
        if (a == kNoTable)
        {
            std::fprintf(stderr, "[ssx3-course] banner: artist table not found, keeping boot titles\n");
            return false;
        }
        st.artistBase = a;
    }
    if (!applySet(ram, tricky, st.titleBase, st.artistBase, writes))
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
template <typename ModesT, typename ModeT>
inline void ensureSynced(uint8_t *ram, const ModesT &ms, uint64_t tick)
{
    if (!knob())
        return;
    SyncState &st = syncState();
    const size_t cur = modeCurrent(ms, ram);
    const ModeT *target = cur ? &ms.modes[cur - 1u] : nullptr;
    const bool tricky = poolLive(target);
    if (st.titleBase != kNoTable && st.artistBase != kNoTable && st.set == (tricky ? 1 : 0))
    {
        const char *const *T = tricky ? kBannerTrickyTitle : kBannerStockTitle;
        const char *const *A = tricky ? kBannerTrickyArtist : kBannerStockArtist;
        if (ramHasN(ram, st.titleBase, T[0]) && ramHasN(ram, st.artistBase, A[0]))
            return;
    }
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
