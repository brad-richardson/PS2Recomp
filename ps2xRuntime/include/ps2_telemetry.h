#pragma once
// TEL1: low-cost play telemetry for long sessions. All knobs default off and
// are output-only: nothing here reads or writes guest state, so the det hash
// is identical with them on. Each knob names a DIRECTORY (created if missing);
// every launch writes fresh files stamped like the perf log
// (<prefix>-YYYYMMDD-HHMMSS.tsv, local time) so sessions never overwrite.
//
//   PS2X_VU_TELEMETRY=<dir>   <dir>/vu1-<stamp>.tsv  VU1 microVU program census
//   PS2X_GS_TELEMETRY=<dir>   <dir>/gs-<stamp>.tsv   GE1 pipeline-compile summary
//   PS2X_GS_TFX_RECORD=<dir>  GE1_TFX_PREWARM=<dir>/tfx-<stamp>.bin when unset
//                             (a fresh file: record-only, no boot prewarm)
//
// vu1 file (TSV; "#" lines are comments). One W row per >= 10 s window of VU1
// activity, preceded by one N row per (image, start PC) entry first seen in
// that window:
//   N t_ms tick img hash pc first_us runs mean_us max_us
//     img: session index of the 16 KiB micro-memory image (hash = its 64-bit
//     content hash; an upload of known bytes reuses the index). first_us: the
//     wall time of the entry's first run (microVU compiles blocks lazily on
//     first execution, so this is the JIT-compile proxy); runs/mean_us/max_us:
//     the later runs of that entry in the same window (the steady cost).
//   W t_ms tick runs resumes run_us max_us uploads new_img new_ent new_us imgs ents
//     run_us: summed wall of all runs; uploads: micro-memory generation
//     changes seen at run time; new_us: summed first_us of new entries;
//     imgs/ents: session totals.
// The window is flushed (fflush) when written, so a killed app loses <= 10 s.
//
// gs file: GE1's PW1 per-vsync CSV (GE1_PIPE_STATS_CSV) is pointed at a pipe
// that a runtime thread drains (no GE1 lib change); only a summary is written:
//   C t_ms vsync new_tfx tfx_us new_spv spv_us flush_us tfx_slow tfx_max_us wall_us
//     one row per vsync with a pipeline compile, cache flush or slow TFX
//     (capped at kGsMaxEventsPerWindow per window; the W row counts drops).
//   W t_ms vsync_first vsync_last vsyncs new_tfx tfx_us new_spv spv_us flush_us
//     tfx_slow tfx_max_us up_kb uploads tex_new tex_new_us wall_max_us
//     wall_gt20ms wall_gt50ms c_dropped
//   P <text>  GE1's own comment rows (the boot prewarm line), passed through.
// Column names come from GE1's CSV header, so a GE1 that adds/drops columns
// still parses (missing columns read 0).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ps2x::telemetry
{
constexpr uint64_t kWindowMs = 10000;
constexpr uint32_t kGsMaxEventsPerWindow = 256;
constexpr uint32_t kVuMaxEntries = 1u << 20; // memory guard; later entries fold into totals
constexpr uint64_t kMaxFileBytes = 64ull * 1024 * 1024; // per-file write cap

// Content hash of a micro-memory image: 4 independent multiply-xor lanes over
// 8-byte words (~1 us for 16 KiB on arm64), tail bytes folded in.
inline uint64_t hashBytes(const uint8_t *p, size_t n)
{
    uint64_t h[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
    constexpr uint64_t kMul = 0xFF51AFD7ED558CCDull;
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int l = 0; l < 4; ++l)
        {
            uint64_t w;
            std::memcpy(&w, p + i + 8 * l, 8);
            h[l] = (h[l] ^ w) * kMul;
            h[l] ^= h[l] >> 29;
        }
    uint64_t tail = 0;
    for (size_t k = 0; i + k < n; ++k)
        tail |= static_cast<uint64_t>(p[i + k]) << (8 * (k & 7));
    uint64_t r = (h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7) ^ tail ^ n) * kMul;
    return r ^ (r >> 31);
}

// VU1 program census (single writer: the MTVU worker). begin() before a
// library run, end() after it; takeWindow() formats N + W rows.
class Vu1Census
{
public:
    static constexpr uint32_t kNone = ~0u;

    // Returns the entry index (kNone for resume runs, which continue a
    // stopped job rather than enter at start_pc). The code is hashed only
    // when the generation differs from the previous call's.
    uint32_t begin(uint64_t generation, const uint8_t *code, size_t codeSize, uint32_t startPc, bool resume,
                   uint64_t tick, uint64_t tMs)
    {
        if (!m_haveGen || generation != m_gen)
        {
            m_haveGen = true;
            m_gen = generation;
            ++m_w.uploads;
            const uint64_t h = hashBytes(code, codeSize);
            auto it = m_images.find(h);
            if (it == m_images.end())
            {
                it = m_images.emplace(h, static_cast<uint32_t>(m_imageHash.size())).first;
                m_imageHash.push_back(h);
                m_pcIndex.resize(m_imageHash.size() * kPcSlots, kNone);
                ++m_w.newImages;
            }
            m_image = it->second;
        }
        ++m_w.runs;
        if (resume)
        {
            ++m_w.resumes;
            return kNone;
        }
        const uint32_t pc = startPc & 0x3ff8u;
        // Direct (image, pc/8) table: no hashing on the per-run path.
        uint32_t &slot = m_pcIndex[static_cast<size_t>(m_image) * kPcSlots + (pc >> 3)];
        if (slot != kNone)
            return slot;
        if (m_entries.size() >= kVuMaxEntries)
            return kNone;
        const uint32_t idx = static_cast<uint32_t>(m_entries.size());
        slot = idx;
        Entry e;
        e.image = m_image;
        e.pc = pc;
        e.firstTick = tick;
        e.firstMs = tMs;
        m_entries.push_back(e);
        m_pending.push_back(idx);
        ++m_w.newEntries;
        return idx;
    }

    void end(uint32_t entry, uint64_t us)
    {
        m_w.runUs += us;
        if (us > m_w.maxUs)
            m_w.maxUs = us;
        if (entry == kNone)
            return;
        Entry &e = m_entries[entry];
        if (!e.ranOnce)
        {
            e.ranOnce = true;
            e.firstUs = us;
            m_w.newUs += us;
            return;
        }
        if (!e.pending)
            return; // steady-state stats only for this window's new entries
        ++e.laterRuns;
        e.laterUs += us;
        if (us > e.laterMaxUs)
            e.laterMaxUs = us;
    }

    bool windowDue(uint64_t tMs) const { return m_w.runs != 0 && tMs >= m_windowStartMs + kWindowMs; }
    void startWindow(uint64_t tMs) { m_windowStartMs = tMs; }

    // Appends this window's N rows and its W row to out; resets the window.
    void takeWindow(uint64_t tMs, uint64_t tick, std::string &out)
    {
        char buf[256];
        for (uint32_t idx : m_pending)
        {
            Entry &e = m_entries[idx];
            const double mean = e.laterRuns ? static_cast<double>(e.laterUs) / e.laterRuns : 0.0;
            std::snprintf(buf, sizeof(buf), "N\t%llu\t%llu\t%u\t%016llx\t0x%04x\t%llu\t%llu\t%.1f\t%llu\n",
                          static_cast<unsigned long long>(e.firstMs), static_cast<unsigned long long>(e.firstTick),
                          e.image, static_cast<unsigned long long>(m_imageHash[e.image]), e.pc,
                          static_cast<unsigned long long>(e.firstUs), static_cast<unsigned long long>(e.laterRuns),
                          mean, static_cast<unsigned long long>(e.laterMaxUs));
            out += buf;
            e.pending = false;
        }
        m_pending.clear();
        std::snprintf(buf, sizeof(buf),
                      "W\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%zu\t%zu\n",
                      static_cast<unsigned long long>(tMs), static_cast<unsigned long long>(tick),
                      static_cast<unsigned long long>(m_w.runs), static_cast<unsigned long long>(m_w.resumes),
                      static_cast<unsigned long long>(m_w.runUs), static_cast<unsigned long long>(m_w.maxUs),
                      static_cast<unsigned long long>(m_w.uploads), static_cast<unsigned long long>(m_w.newImages),
                      static_cast<unsigned long long>(m_w.newEntries), static_cast<unsigned long long>(m_w.newUs),
                      m_imageHash.size(), m_entries.size());
        out += buf;
        m_w = Window{};
        m_windowStartMs = tMs;
    }

    size_t images() const { return m_imageHash.size(); }
    size_t entries() const { return m_entries.size(); }

private:
    struct Entry
    {
        uint32_t image = 0;
        uint32_t pc = 0;
        uint64_t firstTick = 0;
        uint64_t firstMs = 0;
        uint64_t firstUs = 0;
        uint64_t laterRuns = 0;
        uint64_t laterUs = 0;
        uint64_t laterMaxUs = 0;
        bool ranOnce = false;
        bool pending = true;
    };
    struct Window
    {
        uint64_t runs = 0, resumes = 0, runUs = 0, maxUs = 0, uploads = 0, newImages = 0, newEntries = 0,
                 newUs = 0;
    };
    bool m_haveGen = false;
    uint64_t m_gen = 0;
    uint32_t m_image = 0;
    std::unordered_map<uint64_t, uint32_t> m_images;
    std::vector<uint64_t> m_imageHash;
    static constexpr size_t kPcSlots = 0x4000 / 8; // 8 KiB of index per distinct image
    std::vector<uint32_t> m_pcIndex;
    std::vector<Entry> m_entries;
    std::vector<uint32_t> m_pending;
    Window m_w;
    uint64_t m_windowStartMs = 0;
};

// Summarises GE1's PW1 CSV stream (single thread: the drain thread).
class GsCsvSummary
{
public:
    enum Col
    {
        kVsync,
        kNewTfx,
        kTfxUs,
        kNewSpv,
        kSpvUs,
        kFlushUs,
        kTfxSlow,
        kTfxMaxUs,
        kUpKb,
        kUploads,
        kTexNew,
        kTexNewUs,
        kWallUs,
        kReplIndexed, kReplPrecache, kReplLoaded, kReplUsed, kReplCache, kReplFailures, kReplGpu, kHashCache,
        kCols
    };

    // One CSV line without its newline. Appends C/P rows to out.
    void feed(std::string_view line, uint64_t tMs, std::string &out)
    {
        if (line.empty())
            return;
        if (line[0] == '#')
        {
            out += "P\t";
            out.append(line.data(), line.size());
            out += '\n';
            return;
        }
        if (line[0] < '0' || line[0] > '9')
        {
            parseHeader(line);
            return;
        }
        uint64_t v[kCols] = {};
        size_t field = 0, i = 0;
        while (i <= line.size())
        {
            const size_t comma = line.find(',', i);
            const size_t endPos = comma == std::string_view::npos ? line.size() : comma;
            if (field < m_map.size() && m_map[field] >= 0)
            {
                uint64_t x = 0;
                for (size_t k = i; k < endPos && line[k] >= '0' && line[k] <= '9'; ++k)
                    x = x * 10 + static_cast<uint64_t>(line[k] - '0');
                v[m_map[field]] = x;
            }
            ++field;
            if (comma == std::string_view::npos)
                break;
            i = comma + 1;
        }
        if (m_w.vsyncs == 0)
            m_w.first = v[kVsync];
        m_w.last = v[kVsync];
        ++m_w.vsyncs;
        for (int c = kNewTfx; c < kCols; ++c)
            if (c != kTfxMaxUs && c != kWallUs && c < kReplIndexed)
                m_w.sum[c] += v[c];
        for (int c = kReplIndexed; c < kCols; ++c) m_w.sum[c] = v[c];
        if (v[kTfxMaxUs] > m_w.sum[kTfxMaxUs])
            m_w.sum[kTfxMaxUs] = v[kTfxMaxUs];
        if (v[kWallUs] > m_w.sum[kWallUs])
            m_w.sum[kWallUs] = v[kWallUs];
        if (v[kWallUs] > 20000)
            ++m_w.gt20;
        if (v[kWallUs] > 50000)
            ++m_w.gt50;
        if (v[kNewTfx] || v[kNewSpv] || v[kFlushUs] || v[kTfxSlow])
        {
            if (m_w.events >= kGsMaxEventsPerWindow)
            {
                ++m_w.dropped;
                return;
            }
            ++m_w.events;
            char buf[256];
            std::snprintf(buf, sizeof(buf), "C\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n",
                          static_cast<unsigned long long>(tMs), static_cast<unsigned long long>(v[kVsync]),
                          static_cast<unsigned long long>(v[kNewTfx]), static_cast<unsigned long long>(v[kTfxUs]),
                          static_cast<unsigned long long>(v[kNewSpv]), static_cast<unsigned long long>(v[kSpvUs]),
                          static_cast<unsigned long long>(v[kFlushUs]), static_cast<unsigned long long>(v[kTfxSlow]),
                          static_cast<unsigned long long>(v[kTfxMaxUs]), static_cast<unsigned long long>(v[kWallUs]));
            out += buf;
        }
    }

    bool windowDue(uint64_t tMs) const { return m_w.vsyncs != 0 && tMs >= m_windowStartMs + kWindowMs; }
    void startWindow(uint64_t tMs) { m_windowStartMs = tMs; }

    void takeWindow(uint64_t tMs, std::string &out)
    {
        if (m_w.vsyncs == 0)
            return;
        char buf[384];
        const auto u = [](uint64_t x) { return static_cast<unsigned long long>(x); };
        std::snprintf(buf, sizeof(buf),
                      "W\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu"
                      "\t%llu\t%llu\t%llu\n",
                      u(tMs), u(m_w.first), u(m_w.last), u(m_w.vsyncs), u(m_w.sum[kNewTfx]), u(m_w.sum[kTfxUs]),
                      u(m_w.sum[kNewSpv]), u(m_w.sum[kSpvUs]), u(m_w.sum[kFlushUs]), u(m_w.sum[kTfxSlow]),
                      u(m_w.sum[kTfxMaxUs]), u(m_w.sum[kUpKb]), u(m_w.sum[kUploads]), u(m_w.sum[kTexNew]),
                      u(m_w.sum[kTexNewUs]), u(m_w.sum[kWallUs]), u(m_w.gt20), u(m_w.gt50), u(m_w.dropped));
        out += buf;
        if (std::find(m_map.begin(), m_map.end(), kReplIndexed) != m_map.end()) {
            out.pop_back();
            for (int c = kReplIndexed; c < kCols; ++c) {
                out += '\t'; out += std::to_string(m_w.sum[c]);
            }
            out += '\n';
        }
        m_w = Window{};
        m_windowStartMs = tMs;
    }

private:
    void parseHeader(std::string_view line)
    {
        static const char *const kNames[kCols] = {"vsync",   "new_tfx",  "tfx_us",  "new_spv", "spv_us",
                                                  "flush_us", "tfx_slow", "tfx_max_us", "up_kb",   "uploads",
                                                  "tex_new", "tex_new_us", "wall_us", "repl_indexed", "repl_precache_ms1", "repl_loaded", "repl_used", "repl_cache_bytes", "repl_failures", "repl_gpu_bytes", "hash_cache_bytes"};
        m_map.clear();
        size_t i = 0;
        while (i <= line.size())
        {
            const size_t comma = line.find(',', i);
            const std::string_view name =
                line.substr(i, (comma == std::string_view::npos ? line.size() : comma) - i);
            int col = -1;
            for (int c = 0; c < kCols; ++c)
                if (name == kNames[c])
                    col = c;
            m_map.push_back(col);
            if (comma == std::string_view::npos)
                break;
            i = comma + 1;
        }
    }

    struct Window
    {
        uint64_t first = 0, last = 0, vsyncs = 0, gt20 = 0, gt50 = 0, events = 0, dropped = 0;
        uint64_t sum[kCols] = {};
    };
    // Default map = GE1's PW1/SH1 column order (used until a header arrives).
    std::vector<int> m_map{kVsync, kNewTfx, kTfxUs,  kNewSpv, kSpvUs,   kFlushUs, kTfxSlow,
                           kTfxMaxUs, kUpKb, kUploads, kTexNew, kTexNewUs, kWallUs};
    Window m_w;
    uint64_t m_windowStartMs = 0;
};

// Runtime API (src/lib/ps2_telemetry.cpp). Never throws, never aborts: an
// unwritable directory/file logs one stderr line and leaves the knob inert.
// Session file path for a knob directory: <dir>/<prefix>-<stamp>.<ext>.
std::string sessionPath(const std::string &dir, const char *prefix, const char *ext);

// VU1 (MTVU worker only). vuInit() reads PS2X_VU_TELEMETRY once (call after
// the microVU engine is selected); vuOn() is the hot-path check.
extern bool g_vuOn;
void vuInit();
inline bool vuOn() { return g_vuOn; }
uint32_t vuBegin(const uint8_t *code, size_t codeSize, uint64_t generation, uint32_t startPc, bool resume,
                 uint64_t tick, uint64_t &t0Ns);
void vuEnd(uint32_t entry, uint64_t t0Ns, uint64_t tick);
void vuFlush(uint64_t tick); // writes the open window (engine shutdown / state-load reset)

// RAII guard for one library run; inert unless vuOn() at construction.
struct VuRun
{
    bool active = false;
    uint32_t entry = 0;
    uint64_t t0Ns = 0;
    uint64_t tick = 0;
    void begin(const uint8_t *code, size_t codeSize, uint64_t generation, uint32_t startPc, bool resume,
               uint64_t vsyncTick)
    {
        active = true;
        tick = vsyncTick;
        entry = vuBegin(code, codeSize, generation, startPc, resume, vsyncTick, t0Ns);
    }
    ~VuRun()
    {
        if (active)
            vuEnd(entry, t0Ns, tick);
    }
};

// GS: call immediately before / after the GE1 library's ge1_gs_open.
// gsBeforeOpen sets GE1_PIPE_STATS_CSV to a drained pipe (PS2X_GS_TELEMETRY,
// only when GE1_PIPE_STATS_CSV is unset) and GE1_TFX_PREWARM to a fresh
// record file (PS2X_GS_TFX_RECORD, only when GE1_TFX_PREWARM is unset).
void gsBeforeOpen();
void gsAfterOpen(bool opened);
} // namespace ps2x::telemetry
