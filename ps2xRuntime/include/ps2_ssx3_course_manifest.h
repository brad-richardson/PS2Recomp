#pragma once
// TK2: SSX 3 course manifest (PS2X_SSX3_COURSE_MANIFEST=<file>, default off).
//
// The PS2 port of the GameCube route's SSX_COURSE_MANIFEST (TK1 §1, §3.1).
// SLUS_207.72 keeps the frontend's course rows as plain .data in guest RAM:
//   events     0x43D950, 23 x 100 B: +0 index, +4 name[32], +36 short[16],
//              +52 SDB location code[16], +68 world archive[16] ("BAM")
//   topology   0x442488, 23 x 40 B: +0 index, +8 sky, +12 TRANSP, +16 location-table id
//   discipline 0x442820, 23 x 8 B:  +0 index, +4 discipline
//   nav nodes  (.rodata, 108 B): race 0x4781D0 [3][4], freestyle 0x4786E0 [3][5],
//              freeride 0x478D38 [3][8]; +0 event, +4 type (7 = DONOTUSE padding),
//              +0x14 label[30]
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
//   5 halfpipe, 6 backcountry), sky / transp (-1 none, 0-49: topology +8 /
//   +12), node = <race|freestyle|freeride>:<peak>:<slot>:<template slot>
//   (TK6: turns one DONOTUSE padding node into a menu entry for this event:
//   the node becomes a copy of the same peak's template node with +0 = this
//   event). Strings are NUL-padded to the field width. A node target that is
//   not DONOTUSE (event 23, type 7), or a template that is, refuses the whole
//   manifest before anything is written.
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
inline constexpr uint32_t kNodeSize = 0x6Cu;

struct NodeTable
{
    const char *key;
    uint32_t base, peakStride, slots;
};
inline constexpr NodeTable kNodeTables[] = {
    {"race", 0x4781D0u, 0x1B0u, 4u}, {"freestyle", 0x4786E0u, 0x21Cu, 5u}, {"freeride", 0x478D38u, 0x360u, 8u}};
inline constexpr uint32_t kPeaks = 3u;

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
    bool hasSky = false;
    int32_t sky = 0;
    bool hasTransp = false;
    int32_t transp = 0;
    bool hasNode = false;
    uint32_t nodeTable = 0, nodePeak = 0, nodeSlot = 0, nodeTemplate = 0;
};

inline bool parseInt(const std::string &v, int lo, int hi, int32_t &out);

inline uint32_t nodeAddr(uint32_t table, uint32_t peak, uint32_t slot)
{
    const NodeTable &t = kNodeTables[table];
    return t.base + peak * t.peakStride + slot * kNodeSize;
}

// "<table>:<peak>:<slot>:<template slot>", peak 0-2, slots within the table.
inline bool parseNode(const std::string &v, Block &b)
{
    const size_t c1 = v.find(':');
    if (c1 == std::string::npos)
        return false;
    const std::string name = v.substr(0, c1);
    uint32_t table = 0;
    for (; table < 3u; ++table)
        if (name == kNodeTables[table].key)
            break;
    if (table == 3u)
        return false;
    int32_t nums[3] = {0, 0, 0};
    size_t pos = c1 + 1;
    for (int i = 0; i < 3; ++i)
    {
        const size_t c = v.find(':', pos);
        if ((i < 2) != (c != std::string::npos))
            return false;
        const std::string part = v.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
        const int hi = i == 0 ? static_cast<int>(kPeaks) - 1 : static_cast<int>(kNodeTables[table].slots) - 1;
        if (!parseInt(part, 0, hi, nums[i]))
            return false;
        pos = c + 1;
    }
    if (nums[1] == nums[2])
        return false;
    b.nodeTable = table;
    b.nodePeak = static_cast<uint32_t>(nums[0]);
    b.nodeSlot = static_cast<uint32_t>(nums[1]);
    b.nodeTemplate = static_cast<uint32_t>(nums[2]);
    return true;
}

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
    {
        return !(b.has[0] || b.has[1] || b.has[2] || b.has[3] || b.hasLocation || b.hasDiscipline || b.hasSky ||
                 b.hasTransp || b.hasNode);
    };
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
        else if (key == "sky" || key == "transp")
        {
            const bool sky = key == "sky";
            bool &has = sky ? b.hasSky : b.hasTransp;
            if (has)
                return fail("'" + key + "' twice in one block");
            if (!parseInt(val, -1, 49, sky ? b.sky : b.transp))
                return fail("'" + key + "' must be -1-49");
            has = true;
        }
        else if (key == "node")
        {
            if (b.hasNode)
                return fail("'node' twice in one block");
            if (!parseNode(val, b))
                return fail("node must be <race|freestyle|freeride>:<peak 0-2>:<slot>:<template slot>, slots differ");
            for (const Block &o : out)
                if (&o != &b && o.hasNode && o.nodeTable == b.nodeTable && o.nodePeak == b.nodePeak &&
                    o.nodeSlot == b.nodeSlot)
                    return fail("node " + val + " targeted twice");
            b.hasNode = true;
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

// Checks every node target (DONOTUSE: event 23, type 7) and template (a real
// node). Returns false with `err` set on the first failure.
inline bool verifyNodes(const uint8_t *ram, const std::vector<Block> &blocks, std::string &err)
{
    char buf[160];
    for (const Block &b : blocks)
    {
        if (!b.hasNode)
            continue;
        const uint32_t dst = nodeAddr(b.nodeTable, b.nodePeak, b.nodeSlot);
        const uint32_t tpl = nodeAddr(b.nodeTable, b.nodePeak, b.nodeTemplate);
        if (rd32(ram, dst) != 23u || rd32(ram, dst + 4u) != 7u)
        {
            std::snprintf(buf, sizeof(buf), "node target 0x%06x is not DONOTUSE (event %u, type %u)", dst,
                          rd32(ram, dst), rd32(ram, dst + 4u));
            err = buf;
            return false;
        }
        if (rd32(ram, tpl) >= kRows || rd32(ram, tpl + 4u) == 7u)
        {
            std::snprintf(buf, sizeof(buf), "node template 0x%06x is not a real node (event %u, type %u)", tpl,
                          rd32(ram, tpl), rd32(ram, tpl + 4u));
            err = buf;
            return false;
        }
    }
    return true;
}

// Applies verified blocks. `log` receives one line per write. Returns the
// number of writes, or -1 when the tables or node targets fail verification
// (nothing written).
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
    if (!verifyNodes(ram, blocks, err))
    {
        log("refused: " + err + "; nothing written");
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
        for (int k = 0; k < 2; ++k)
        {
            if (!(k == 0 ? b.hasSky : b.hasTransp))
                continue;
            const uint32_t addr = kTopoBase + static_cast<uint32_t>(b.event) * kTopoStride + (k == 0 ? 8u : 12u);
            const int32_t old = static_cast<int32_t>(rd32(ram, addr));
            const int32_t v = k == 0 ? b.sky : b.transp;
            std::memcpy(ram + addr, &v, 4);
            std::snprintf(buf, sizeof(buf), "event %d %s at 0x%06x: %d -> %d", b.event, k == 0 ? "sky" : "transp",
                          addr, old, v);
            log(buf);
            ++writes;
        }
        if (b.hasNode)
        {
            const uint32_t dst = nodeAddr(b.nodeTable, b.nodePeak, b.nodeSlot);
            const uint32_t tpl = nodeAddr(b.nodeTable, b.nodePeak, b.nodeTemplate);
            const std::string oldLabel = rdStr(ram, dst + 0x14u, 30u);
            const uint32_t tplEvent = rd32(ram, tpl);
            std::memmove(ram + dst, ram + tpl, kNodeSize);
            const uint32_t ev = static_cast<uint32_t>(b.event);
            std::memcpy(ram + dst, &ev, 4);
            std::snprintf(buf, sizeof(buf), "event %d node %s peak %u slot %u at 0x%06x: \"%s\" -> copy of slot %u (event %u) with event %d",
                          b.event, kNodeTables[b.nodeTable].key, b.nodePeak, b.nodeSlot, dst, oldLabel.c_str(),
                          b.nodeTemplate, tplEvent, b.event);
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
