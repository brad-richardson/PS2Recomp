#pragma once
// TK2: SSX 3 course manifest (PS2X_SSX3_COURSE_MANIFEST=<file>, default off).
//
// The PS2 port of the GameCube route's SSX_COURSE_MANIFEST (TK1 §1, §3.1).
// SLUS_207.72 keeps the frontend's course rows as plain .data in guest RAM:
//   events     0x43D950, 23 x 100 B: +0 index, +4 name[32], +36 short[16],
//              +52 SDB location code[16], +68 world archive[16] ("BAM")
//   topology   0x442488, 23 x 40 B: +0 index, +16 location-table id
//   discipline 0x442820, 23 x 8 B:  +0 index, +4 discipline
// Applied once after the ELF is in RAM and before the EE runs. It first
// checks all 69 row-index words (row i starts with i in each table) and
// refuses the whole manifest on any mismatch, so another executable is never
// written. Every write is logged with its old value.
//
// Manifest: one `key = value` per line, `#` comments, blank lines ignored.
// `event = N` (0-22) opens a block; the keys below apply to it until the
// next `event`. Each key at most once per block; a block sets >= 1 field.
//   name (<= 31 printable ASCII), short/code/archive (<= 15), location
//   (0-49), discipline (1 station/debug, 2 race, 3 slopestyle, 4 big air,
//   5 halfpipe, 6 backcountry). Strings are NUL-padded to the field width.
// The world archive's BIGF members must be named data/worlds/<archive>.*
// (the path builder 0x22F6B0 derives member names from the archive field).
//
// Guest-affecting when set (it rewrites guest .data); unset = no reads, no
// writes. Platform-neutral so the host unit test compiles it.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ps2_ssx3_course
{

inline constexpr uint32_t kEventBase = 0x43D950u;
inline constexpr uint32_t kEventStride = 100u;
inline constexpr uint32_t kTopoBase = 0x442488u;
inline constexpr uint32_t kTopoStride = 40u;
inline constexpr uint32_t kDiscBase = 0x442820u;
inline constexpr uint32_t kDiscStride = 8u;
inline constexpr uint32_t kRows = 23u;
inline constexpr uint32_t kRamSize = 32u * 1024u * 1024u;

struct StringField
{
    const char *key;
    uint32_t offset; // in the event record
    uint32_t width;  // bytes including the NUL
};
inline constexpr StringField kStringFields[] = {
    {"name", 4u, 32u}, {"short", 36u, 16u}, {"code", 52u, 16u}, {"archive", 68u, 16u}};

struct Block
{
    int event = -1;
    bool has[4] = {false, false, false, false};
    std::string str[4];
    bool hasLocation = false;
    int32_t location = 0;
    bool hasDiscipline = false;
    int32_t discipline = 0;
};

inline std::string trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

inline bool parseInt(const std::string &v, int lo, int hi, int32_t &out)
{
    if (v.empty())
        return false;
    char *end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (!end || *end != '\0' || n < lo || n > hi)
        return false;
    out = static_cast<int32_t>(n);
    return true;
}

// Returns false with `err` set (line number + reason) on any malformed input.
inline bool parse(const std::string &text, std::vector<Block> &out, std::string &err)
{
    out.clear();
    std::istringstream in(text);
    std::string raw;
    int lineNo = 0;
    auto fail = [&](const std::string &why)
    {
        err = "line " + std::to_string(lineNo) + ": " + why;
        return false;
    };
    auto blockEmpty = [](const Block &b)
    { return !(b.has[0] || b.has[1] || b.has[2] || b.has[3] || b.hasLocation || b.hasDiscipline); };
    while (std::getline(in, raw))
    {
        ++lineNo;
        const size_t hash = raw.find('#');
        const std::string line = trim(hash == std::string::npos ? raw : raw.substr(0, hash));
        if (line.empty())
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            return fail("expected key = value");
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (key == "event")
        {
            if (!out.empty() && blockEmpty(out.back()))
                return fail("event " + std::to_string(out.back().event) + " sets no field");
            Block b;
            int32_t n = 0;
            if (!parseInt(val, 0, static_cast<int>(kRows) - 1, n))
                return fail("event must be 0-22");
            for (const Block &o : out)
                if (o.event == n)
                    return fail("event " + val + " appears twice");
            b.event = n;
            out.push_back(b);
            continue;
        }
        if (out.empty())
            return fail("'" + key + "' before any event line");
        Block &b = out.back();
        bool known = false;
        for (size_t i = 0; i < 4; ++i)
        {
            if (key != kStringFields[i].key)
                continue;
            known = true;
            if (b.has[i])
                return fail("'" + key + "' twice in one block");
            if (val.empty() || val.size() > kStringFields[i].width - 1u)
                return fail("'" + key + "' must be 1-" + std::to_string(kStringFields[i].width - 1u) + " bytes");
            for (char c : val)
                if (c < 0x20 || c > 0x7E)
                    return fail("'" + key + "' must be printable ASCII");
            b.has[i] = true;
            b.str[i] = val;
        }
        if (known)
            continue;
        if (key == "location")
        {
            if (b.hasLocation)
                return fail("'location' twice in one block");
            if (!parseInt(val, 0, 49, b.location))
                return fail("location must be 0-49");
            b.hasLocation = true;
        }
        else if (key == "discipline")
        {
            if (b.hasDiscipline)
                return fail("'discipline' twice in one block");
            if (!parseInt(val, 1, 6, b.discipline))
                return fail("discipline must be 1-6");
            b.hasDiscipline = true;
        }
        else
        {
            return fail("unknown key '" + key + "'");
        }
    }
    if (out.empty())
    {
        err = "no event blocks";
        return false;
    }
    if (blockEmpty(out.back()))
    {
        err = "event " + std::to_string(out.back().event) + " sets no field";
        return false;
    }
    return true;
}

inline uint32_t rd32(const uint8_t *ram, uint32_t addr)
{
    uint32_t v = 0;
    std::memcpy(&v, ram + addr, 4);
    return v;
}

inline std::string rdStr(const uint8_t *ram, uint32_t addr, uint32_t width)
{
    std::string s;
    for (uint32_t i = 0; i < width && ram[addr + i] != 0; ++i)
        s.push_back(static_cast<char>(ram[addr + i]));
    return s;
}

// Checks the 69 row-index words. Returns the number of mismatches; the first
// one goes to `err`.
inline uint32_t verifyTables(const uint8_t *ram, std::string &err)
{
    struct T
    {
        const char *name;
        uint32_t base, stride;
    };
    const T tables[] = {{"event", kEventBase, kEventStride},
                        {"topology", kTopoBase, kTopoStride},
                        {"discipline", kDiscBase, kDiscStride}};
    uint32_t bad = 0;
    for (const T &t : tables)
        for (uint32_t i = 0; i < kRows; ++i)
        {
            const uint32_t addr = t.base + i * t.stride;
            const uint32_t v = rd32(ram, addr);
            if (v != i)
            {
                if (bad++ == 0)
                {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "%s row %u at 0x%06x reads %u", t.name, i, addr, v);
                    err = buf;
                }
            }
        }
    return bad;
}

// Applies verified blocks. `log` receives one line per write. Returns the
// number of writes, or -1 when the tables fail verification (nothing written).
template <typename Log>
inline int apply(uint8_t *ram, const std::vector<Block> &blocks, Log &&log)
{
    std::string err;
    const uint32_t bad = verifyTables(ram, err);
    if (bad != 0u)
    {
        log("refused: " + std::to_string(bad) + " of 69 row-index words differ (" + err + "); nothing written");
        return -1;
    }
    int writes = 0;
    char buf[256];
    for (const Block &b : blocks)
    {
        const uint32_t rec = kEventBase + static_cast<uint32_t>(b.event) * kEventStride;
        for (size_t i = 0; i < 4; ++i)
        {
            if (!b.has[i])
                continue;
            const StringField &f = kStringFields[i];
            const uint32_t addr = rec + f.offset;
            const std::string old = rdStr(ram, addr, f.width);
            std::memset(ram + addr, 0, f.width);
            std::memcpy(ram + addr, b.str[i].data(), b.str[i].size());
            std::snprintf(buf, sizeof(buf), "event %d %s at 0x%06x: \"%s\" -> \"%s\"", b.event, f.key, addr,
                          old.c_str(), b.str[i].c_str());
            log(buf);
            ++writes;
        }
        if (b.hasLocation)
        {
            const uint32_t addr = kTopoBase + static_cast<uint32_t>(b.event) * kTopoStride + 16u;
            const int32_t old = static_cast<int32_t>(rd32(ram, addr));
            std::memcpy(ram + addr, &b.location, 4);
            std::snprintf(buf, sizeof(buf), "event %d location at 0x%06x: %d -> %d", b.event, addr, old, b.location);
            log(buf);
            ++writes;
        }
        if (b.hasDiscipline)
        {
            const uint32_t addr = kDiscBase + static_cast<uint32_t>(b.event) * kDiscStride + 4u;
            const int32_t old = static_cast<int32_t>(rd32(ram, addr));
            std::memcpy(ram + addr, &b.discipline, 4);
            std::snprintf(buf, sizeof(buf), "event %d discipline at 0x%06x: %d -> %d", b.event, addr, old,
                          b.discipline);
            log(buf);
            ++writes;
        }
    }
    return writes;
}

// Runtime entry: called once from loadELF. Unset or empty knob = no-op.
inline void applyFromEnv(uint8_t *ram)
{
    const char *path = std::getenv("PS2X_SSX3_COURSE_MANIFEST");
    if (!path || !*path || !ram)
        return;
    auto log = [](const std::string &s) { std::fprintf(stderr, "[ssx3-course] %s\n", s.c_str()); };
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        log(std::string("refused: cannot open ") + path);
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<Block> blocks;
    std::string err;
    if (!parse(ss.str(), blocks, err))
    {
        log(std::string("refused: ") + path + " " + err + "; nothing written");
        return;
    }
    log(std::string("applying ") + path);
    const int n = apply(ram, blocks, log);
    if (n >= 0)
        log("applied " + std::to_string(n) + " writes in " + std::to_string(blocks.size()) + " event blocks");
}

} // namespace ps2_ssx3_course
