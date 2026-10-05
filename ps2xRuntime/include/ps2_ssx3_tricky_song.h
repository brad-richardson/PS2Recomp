#pragma once
// TK43c: "It's Tricky" song bursts on Tricky courses (output-only).
//
// When the boost meter becomes full, the host mixer plays ~5 s of the
// staged song (PS2X_SSX3_TRICKY_SONG=<48 kHz stereo 16-bit WAV path>),
// resuming where the last burst left off and wrapping at the song end.
// Mixed at the output stage after the SPU mix (the runtime cannot separate
// the game's music bus: a single mixed ring feeds the callbacks), at a
// level matched to the game's music bed (TK25b's +/-1 LU rule; the gain is
// baked into the staged file). Saturating add; the WAV taps capture the
// burst because the mix lands before them.
//
// The meter-full edge is detected by trickyHudOverlay (ps2_runtime.cpp),
// which calls startBurst; the audio callbacks call mixInto. The resume
// offset is in-session only (not persisted). Knob unset/invalid = the
// mixer is a no-op and the output is byte-identical.
//
// TK47: burst probe. mixInto counts what it consumes (frames, song peak,
// mix-bus peak before/after, saturations, callback path) and dumps one
// stderr line per finished burst; startBurst/initFromEnv log the Player
// address so a duplicated static across .so boundaries would show as an
// address mismatch. No new knob: nothing is logged unless a song staged.
//
// Change class: output-only. No guest reads or writes; the guest det-hash
// cannot move. Threading: the song PCM is immutable after initFromEnv
// (audio init, before callbacks start); startBurst runs on the game thread
// and publishes one atomic; mixInto runs on the audio thread and consumes
// it with CAS. All probe fields below are audio-thread-only (the dumps
// read them from the same thread).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ps2_ssx3_tricky_song
{

constexpr uint32_t kRate = 48000u;
constexpr uint32_t kChannels = 2u;
constexpr uint64_t kBurstFrames = 240000u; // 5.000 s at 48 kHz

struct Song
{
    std::vector<int16_t> pcm; // frames*2 interleaved, immutable after load
    uint32_t frames = 0;
    bool ok = false;
};

inline uint32_t rdU32le(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t rdU16le(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

// Minimal RIFF/WAVE parser. Strict: PCM16, stereo, 48 kHz, non-empty data.
// Refuses anything else (the staging script writes exactly this shape).
inline Song parseSongWav(const uint8_t *data, size_t size)
{
    Song s;
    if (!data || size < 44u || std::memcmp(data, "RIFF", 4) != 0 ||
        std::memcmp(data + 8, "WAVE", 4) != 0)
        return s;
    size_t off = 12u;
    bool haveFmt = false;
    const uint8_t *samples = nullptr;
    size_t sampleBytes = 0u;
    while (off + 8u <= size)
    {
        const uint8_t *h = data + off;
        const uint32_t len = rdU32le(h + 4);
        if (off + 8u + len > size)
            return s;
        if (std::memcmp(h, "fmt ", 4) == 0)
        {
            if (len < 16u)
                return s;
            const uint8_t *f = h + 8;
            const uint16_t tag = rdU16le(f);
            const uint16_t ch = rdU16le(f + 2);
            const uint32_t rate = rdU32le(f + 4);
            const uint16_t bits = rdU16le(f + 14);
            if (tag != 1u || ch != kChannels || rate != kRate || bits != 16u)
                return s;
            haveFmt = true;
        }
        else if (std::memcmp(h, "data", 4) == 0)
        {
            samples = h + 8;
            sampleBytes = len;
        }
        off += 8u + len + (len & 1u);
    }
    if (!haveFmt || !samples || sampleBytes == 0u || (sampleBytes % 4u) != 0u)
        return s;
    s.frames = static_cast<uint32_t>(sampleBytes / 4u);
    s.pcm.resize(static_cast<size_t>(s.frames) * 2u);
    std::memcpy(s.pcm.data(), samples, sampleBytes);
    s.ok = true;
    return s;
}

inline Song loadSongFile(const char *path)
{
    Song s;
    if (!path || !*path)
        return s;
    std::FILE *f = std::fopen(path, "rb");
    if (!f)
        return s;
    std::vector<uint8_t> buf;
    uint8_t chunk[65536];
    size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0u)
        buf.insert(buf.end(), chunk, chunk + n);
    std::fclose(f);
    return parseSongWav(buf.data(), buf.size());
}

// TK47 probe: per-burst consume stats (audio thread only).
struct BurstStats
{
    uint64_t consumed = 0;
    int songPeak = 0;
    int busBefore = 0;
    int busAfter = 0;
    uint64_t sat = 0;
    const char *path = "?";
};

inline void dumpBurstStats(const BurstStats &b, const char *how, uint64_t n)
{
    std::fprintf(stderr,
                 "[ssx3-tricky-song] probe %s #%llu path=%s consumed=%llu songPeak=%d "
                 "busBefore=%d busAfter=%d sat=%llu\n",
                 how, static_cast<unsigned long long>(n), b.path,
                 static_cast<unsigned long long>(b.consumed), b.songPeak, b.busBefore,
                 b.busAfter, static_cast<unsigned long long>(b.sat));
}

struct Player
{
    Song song;
    // 0 (low 32 = 0) = idle; else (startFrame << 32) | framesLeft.
    std::atomic<uint64_t> cmd{0};
    uint64_t resume = 0; // game-thread only
    int songPeak = 0;    // max |sample| over the staged PCM (initFromEnv)
    // TK47 probe (audio thread only; see the header comment).
    BurstStats cur;
    BurstStats last;
    uint64_t burstsDone = 0;
    uint64_t burstsStarted = 0;
    uint32_t probeLastLeft = 0;
    bool probeActive = false;
    bool probeMixLogged = false;
};

inline Player &player()
{
    static Player p;
    return p;
}

inline void initFromEnv()
{
    const char *path = std::getenv("PS2X_SSX3_TRICKY_SONG");
    if (!path || !*path)
        return;
    Player &p = player();
    p.song = loadSongFile(path);
    std::fprintf(stderr, "[ssx3-tricky-song] file '%s': %s\n", path,
                 p.song.ok ? "loaded" : "missing/invalid, song off");
    if (p.song.ok)
    {
        int peak = 0;
        for (const int16_t v : p.song.pcm)
        {
            const int a = v < 0 ? -(int)v : (int)v;
            if (a > peak)
                peak = a;
        }
        p.songPeak = peak;
        std::fprintf(stderr,
                     "[ssx3-tricky-song] frames=%u (%.1f s), burst=%llu frames, peak=%d, player=%p\n",
                     p.song.frames, p.song.frames / static_cast<double>(kRate),
                     static_cast<unsigned long long>(kBurstFrames), peak,
                     static_cast<const void *>(&p));
    }
}

// Game thread: open a burst at the resume offset. A burst already playing
// is restarted from the resume offset (a re-full within 5 s of a full);
// the resume only ever advances by whole bursts, wrapping at the song end.
inline void startBurst(uint64_t tick)
{
    Player &p = player();
    if (!p.song.ok || p.song.frames == 0u)
        return;
    const uint64_t start = p.resume % p.song.frames;
    p.resume = (start + kBurstFrames) % p.song.frames;
    p.cmd.store((start << 32) | kBurstFrames, std::memory_order_release);
    std::fprintf(stderr,
                 "[ssx3-tricky-song] burst start=%llu end=%llu resume=%llu tick=%llu player=%p songPeak=%d\n",
                 static_cast<unsigned long long>(start),
                 static_cast<unsigned long long>(start + kBurstFrames),
                 static_cast<unsigned long long>(p.resume),
                 static_cast<unsigned long long>(tick),
                 static_cast<const void *>(&p), p.songPeak);
}

inline int16_t satAdd(int16_t a, int16_t b)
{
    const int v = static_cast<int>(a) + static_cast<int>(b);
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return static_cast<int16_t>(v);
}

// Audio thread: saturating-add up to `frames` burst samples over `out`.
// No-op when idle or unloaded (output byte-identical with the knob off).
// `path` labels the calling callback ("direct"/"stretch") for the probe.
inline void mixInto(int16_t *out, size_t frames, const char *path = "direct")
{
    Player &p = player();
    if (!p.song.ok || !out || frames == 0u)
        return;
    if (!p.probeMixLogged)
    {
        p.probeMixLogged = true;
        std::fprintf(stderr, "[ssx3-tricky-song] mix player=%p ok=%d frames=%u peak=%d path=%s\n",
                     static_cast<const void *>(&p), p.song.ok ? 1 : 0, p.song.frames,
                     p.songPeak, path ? path : "?");
    }
    uint64_t s = p.cmd.load(std::memory_order_acquire);
    while ((s & 0xffffffffull) != 0u)
    {
        const uint32_t left = static_cast<uint32_t>(s & 0xffffffffull);
        const uint32_t start = static_cast<uint32_t>(s >> 32);
        const size_t n = left < frames ? left : frames;
        const uint64_t next = (static_cast<uint64_t>(start) << 32) | (left - n);
        if (!p.cmd.compare_exchange_weak(s, next, std::memory_order_acq_rel,
                                         std::memory_order_acquire))
            continue; // a restart raced us; mix from the fresh command
        // TK47 probe: exactly one accounting pass per call (past the CAS).
        // `left` only grows via startBurst, so a larger `left` than the
        // last consume means a restart overwrote this burst mid-flight.
        if (!p.probeActive || left > p.probeLastLeft)
        {
            if (p.probeActive && p.cur.consumed > 0u)
                dumpBurstStats(p.cur, "restart", p.burstsDone + 1u);
            p.cur = BurstStats{};
            p.cur.path = (path && *path) ? path : "?";
            p.probeActive = true;
            ++p.burstsStarted;
        }
        p.probeLastLeft = left - static_cast<uint32_t>(n);
        const uint64_t off = (static_cast<uint64_t>(start) + (kBurstFrames - left)) %
                             p.song.frames;
        for (size_t i = 0; i < n; ++i)
        {
            const uint64_t f = (off + i) % p.song.frames;
            for (int c = 0; c < 2; ++c)
            {
                const size_t k = 2u * i + static_cast<size_t>(c);
                const int16_t song = p.song.pcm[2u * f + static_cast<size_t>(c)];
                const int before = static_cast<int>(out[k]);
                const int v = before + static_cast<int>(song);
                const int aSong = song < 0 ? -(int)song : (int)song;
                const int aBefore = before < 0 ? -before : before;
                if (aSong > p.cur.songPeak)
                    p.cur.songPeak = aSong;
                if (aBefore > p.cur.busBefore)
                    p.cur.busBefore = aBefore;
                if (v > 32767 || v < -32768)
                    ++p.cur.sat;
                out[k] = satAdd(out[k], song);
                const int aAfter = out[k] < 0 ? -(int)out[k] : (int)out[k];
                if (aAfter > p.cur.busAfter)
                    p.cur.busAfter = aAfter;
            }
        }
        p.cur.consumed += n;
        if (p.probeLastLeft == 0u)
        {
            p.last = p.cur;
            ++p.burstsDone;
            dumpBurstStats(p.cur, "done", p.burstsDone);
            p.probeActive = false;
        }
        return;
    }
}

} // namespace ps2_ssx3_tricky_song
