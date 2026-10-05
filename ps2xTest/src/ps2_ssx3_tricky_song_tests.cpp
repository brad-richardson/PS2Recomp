#include "MiniTest.h"
#include "ps2_ssx3_tricky_song.h"

// TK43c: "It's Tricky" burst player tests. WAV blobs are synthetic (no game
// audio); burst/mix tests drive the player state directly.
namespace
{
using ps2_ssx3_tricky_song::Song;

std::vector<uint8_t> synthWav(uint32_t rate, uint16_t ch, uint16_t bits, uint32_t frames)
{
    std::vector<uint8_t> b;
    const uint32_t bytes = frames * ch * (bits / 8u);
    auto u32 = [&](uint32_t v) {
        b.push_back(static_cast<uint8_t>(v));
        b.push_back(static_cast<uint8_t>(v >> 8));
        b.push_back(static_cast<uint8_t>(v >> 16));
        b.push_back(static_cast<uint8_t>(v >> 24));
    };
    auto u16 = [&](uint16_t v) {
        b.push_back(static_cast<uint8_t>(v));
        b.push_back(static_cast<uint8_t>(v >> 8));
    };
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    u32(36u + bytes);
    b.insert(b.end(), {'W', 'A', 'V', 'E'});
    b.insert(b.end(), {'f', 'm', 't', ' '});
    u32(16u);
    u16(1u);
    u16(ch);
    u32(rate);
    u32(rate * ch * (bits / 8u));
    u16(ch * (bits / 8u));
    u16(bits);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    u32(bytes);
    for (uint32_t i = 0; i < frames * ch; ++i)
        u16(static_cast<uint16_t>(1000 + i));
    return b;
}
} // namespace

void register_ps2_ssx3_tricky_song_tests()
{
    MiniTest::Case("Ps2Ssx3TrickySong", [](TestCase &tc)
                   {
        tc.Run("wav parse accepts staged shape, refuses the rest", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_song;
            Song s = parseSongWav(nullptr, 0);
            t.IsTrue(!s.ok, "null");
            std::vector<uint8_t> good = synthWav(48000u, 2u, 16u, 8u);
            s = parseSongWav(good.data(), good.size());
            t.IsTrue(s.ok && s.frames == 8u, "staged shape");
            t.IsTrue(s.ok && s.pcm[0] == 1000 && s.pcm[15] == 1015, "samples");
            std::vector<uint8_t> bad = synthWav(44100u, 2u, 16u, 8u);
            t.IsTrue(!parseSongWav(bad.data(), bad.size()).ok, "wrong rate");
            bad = synthWav(48000u, 1u, 16u, 8u);
            t.IsTrue(!parseSongWav(bad.data(), bad.size()).ok, "mono");
            bad = synthWav(48000u, 2u, 8u, 8u);
            t.IsTrue(!parseSongWav(bad.data(), bad.size()).ok, "8-bit");
            bad = synthWav(48000u, 2u, 16u, 0u);
            t.IsTrue(!parseSongWav(bad.data(), bad.size()).ok, "empty");
            bad = synthWav(48000u, 2u, 16u, 8u);
            bad[0] = 'X';
            t.IsTrue(!parseSongWav(bad.data(), bad.size()).ok, "bad riff");
            t.IsTrue(!parseSongWav(good.data(), 40u).ok, "truncated");
            t.IsTrue(loadSongFile("/nonexistent-tk43c/x.wav").ok == false, "missing file");
            t.IsTrue(loadSongFile(nullptr).ok == false, "null path"); });
        tc.Run("bursts resume and wrap", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_song;
            Player &p = player();
            // A synthetic 7-frame song (240000 % 7 == 5: non-degenerate
            // resumes); the burst wraps many times, the arithmetic is exact.
            p.song.frames = 7u;
            p.song.pcm.assign(14u, 0);
            p.song.ok = true;
            p.cmd.store(0u, std::memory_order_relaxed);
            p.resume = 0u;
            startBurst(100u);
            uint64_t c = p.cmd.load(std::memory_order_relaxed);
            t.IsTrue((c >> 32) == 0u && (c & 0xffffffffull) == kBurstFrames, "first starts at 0");
            t.IsTrue(p.resume == 5u, "resume advances one burst");
            const uint64_t r1 = p.resume;
            startBurst(200u);
            c = p.cmd.load(std::memory_order_relaxed);
            t.IsTrue((c >> 32) == r1, "second resumes at the previous end");
            t.IsTrue(p.resume == 3u, "resume wraps");
            // Leave the shared player idle for the next run.
            p.cmd.store(0u, std::memory_order_relaxed);
            p.song.ok = false;
            p.song.frames = 0u;
            p.song.pcm.clear();
            p.resume = 0u;
            p.songPeak = 0;
            p.cur = BurstStats{};
            p.last = BurstStats{};
            p.burstsDone = 0u;
            p.burstsStarted = 0u;
            p.probeLastLeft = 0u;
            p.probeActive = false; });
        tc.Run("mix adds, saturates, wraps, idles to no-op", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_song;
            Player &p = player();
            p.song.frames = 4u;
            p.song.pcm = {30000, -30000, 30000, -30000, 100, 200, 300, 400};
            p.song.ok = true;
            p.cmd.store(0u, std::memory_order_relaxed);
            p.resume = 0u;
            int16_t out[8] = {1000, -1000, 0, 0, 0, 0, 0, 0};
            mixInto(out, 4u);
            t.IsTrue(out[0] == 1000 && out[1] == -1000, "idle mixes nothing");
            startBurst(0u); // start=0, resume=240000%4=0
            mixInto(out, 2u);
            t.IsTrue(out[0] == 31000 && out[1] == -31000, "adds frame 0");
            t.IsTrue(out[2] == 30000 && out[3] == -30000, "adds frame 1");
            // off = (start + kBurst - left) % 4 with kBurst % 4 == 0, so
            // start = (off + left) % 4 for a chosen play offset.
            p.cmd.store((0ull << 32) | 1u, std::memory_order_relaxed); // off=3
            int16_t one[2] = {0, 0};
            mixInto(one, 1u);
            t.IsTrue(one[0] == 300 && one[1] == 400, "frame 3");
            p.cmd.store((1ull << 32) | 2u, std::memory_order_relaxed); // off=3, wraps
            int16_t wrap[4] = {0, 0, 0, 0};
            mixInto(wrap, 2u);
            t.IsTrue(wrap[0] == 300 && wrap[1] == 400, "frame 3");
            t.IsTrue(wrap[2] == 30000 && wrap[3] == -30000, "wraps to frame 0");
            p.cmd.store((1ull << 32) | 1u, std::memory_order_relaxed); // off=0
            int16_t sat[2] = {30000, -30000};
            mixInto(sat, 1u);
            t.IsTrue(sat[0] == 32767 && sat[1] == -32768, "saturates");
            p.cmd.store(0u, std::memory_order_relaxed);
            p.song.ok = false;
            p.song.frames = 0u;
            p.song.pcm.clear();
            p.resume = 0u;
            p.songPeak = 0;
            p.cur = BurstStats{};
            p.last = BurstStats{};
            p.burstsDone = 0u;
            p.burstsStarted = 0u;
            p.probeLastLeft = 0u;
            p.probeActive = false; });
        tc.Run("probe counts consumes, peaks, saturations", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_song;
            Player &p = player();
            p.song.frames = 2u;
            p.song.pcm = {1000, -2000, 3000, -4000};
            p.song.ok = true;
            p.cmd.store(0u, std::memory_order_relaxed);
            p.resume = 0u;
            p.songPeak = 0;
            p.cur = BurstStats{};
            p.last = BurstStats{};
            p.burstsDone = 0u;
            p.burstsStarted = 0u;
            p.probeLastLeft = 0u;
            p.probeActive = false;
            startBurst(0u); // start=0, left=240000
            int16_t out[4] = {100, 200, 30000, -30000};
            mixInto(out, 2u, "stretch");
            t.IsTrue(out[0] == 1100 && out[1] == -1800, "mix unchanged");
            t.IsTrue(out[2] == 32767 && out[3] == -32768, "mix saturates");
            t.IsTrue(p.cur.consumed == 2u, "consumed counted");
            t.IsTrue(p.cur.songPeak == 4000, "song peak");
            t.IsTrue(p.cur.busBefore == 30000, "bus peak before");
            t.IsTrue(p.cur.busAfter == 32768, "bus peak after");
            t.IsTrue(p.cur.sat == 2u, "saturations counted");
            t.IsTrue(std::strcmp(p.cur.path, "stretch") == 0, "path labelled");
            t.IsTrue(p.burstsStarted == 1u && p.burstsDone == 0u, "partial burst open");
            // kBurstFrames % 2 == 0, so start = (off + left) % 2: off=1.
            p.cmd.store((0ull << 32) | 1u, std::memory_order_relaxed);
            int16_t one[2] = {0, 0};
            mixInto(one, 1u, "stretch");
            t.IsTrue(one[0] == 3000 && one[1] == -4000, "frame 1");
            t.IsTrue(p.burstsDone == 1u, "completion counted");
            t.IsTrue(p.last.consumed == 3u, "total consumed");
            t.IsTrue(p.last.songPeak == 4000 && p.last.sat == 2u, "snapshot kept");
            t.IsTrue(!p.probeActive, "burst closed");
            p.cmd.store(0u, std::memory_order_relaxed);
            p.song.ok = false;
            p.song.frames = 0u;
            p.song.pcm.clear();
            p.resume = 0u;
            p.songPeak = 0;
            p.cur = BurstStats{};
            p.last = BurstStats{};
            p.burstsDone = 0u;
            p.burstsStarted = 0u;
            p.probeLastLeft = 0u;
            p.probeActive = false; });});
}
