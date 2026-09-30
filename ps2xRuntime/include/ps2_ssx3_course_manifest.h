#pragma once
// TK2: SSX 3 course manifest (PS2X_SSX3_COURSE_MANIFEST=<file>, default off).
//
// The PS2 port of the GameCube route's SSX_COURSE_MANIFEST (TK1 §1, §3.1).
// SLUS_207.72 keeps the frontend's course rows as plain .data in guest RAM:
//   events     0x43D950, 23 x 100 B: +0 index, +4 name[32], +36 short[16],
//              +52 SDB location code[16], +68 world archive[16] ("BAM")
//   topology   0x442488, 23 x 40 B: +0 index, +8 sky, +12 TRANSP, +16 location-table id
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
//   5 halfpipe, 6 backcountry), sky / transp (-1 none, 0-49: topology +8 /
//   +12). Strings are NUL-padded to the field width.
//   picker = <location 0-49>:<name> (TK7, repeatable, <= 7 per block, one
//   block per manifest): extra courses for this event's slot. See the picker
//   section below; inert unless PS2X_SSX3_COURSE_PICKER=1.
// `node` (TK6's DONOTUSE nav-node writer) is refused: the padding slots have
// no menu widget, so the menu aborts at Select Peak (TK6 p1/p2). The nav
// tables for a later menu-row lane: race 0x4781D0 [3][4], freestyle 0x4786E0
// [3][5], freeride 0x478D38 [3][8] (108 B nodes: +0 event, +4 type, 7 =
// DONOTUSE padding, +0x14 label[30]).
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
#include <mutex>
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
inline constexpr size_t kPickerMax = 7u;

struct StringField
{
    const char *key;
    uint32_t offset; // in the event record
    uint32_t width;  // bytes including the NUL
};
inline constexpr StringField kStringFields[] = {
    {"name", 4u, 32u}, {"short", 36u, 16u}, {"code", 52u, 16u}, {"archive", 68u, 16u}};

struct PickerEntry
{
    int32_t location = 0;
    std::string name;
};

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
    std::vector<PickerEntry> picker;
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

inline bool printableName(const std::string &v, size_t maxLen)
{
    if (v.empty() || v.size() > maxLen)
        return false;
    for (char c : v)
        if (c < 0x20 || c > 0x7E)
            return false;
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
                 b.hasTransp || !b.picker.empty());
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
            if (!printableName(val, kStringFields[i].width - 1u))
                return fail("'" + key + "' must be 1-" + std::to_string(kStringFields[i].width - 1u) +
                            " printable ASCII bytes");
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
        else if (key == "picker")
        {
            for (const Block &o : out)
                if (&o != &b && !o.picker.empty())
                    return fail("picker entries in two event blocks (one picker slot per manifest)");
            if (b.picker.size() >= kPickerMax)
                return fail("at most " + std::to_string(kPickerMax) + " picker entries");
            const size_t colon = val.find(':');
            PickerEntry e;
            if (colon == std::string::npos || !parseInt(trim(val.substr(0, colon)), 0, 49, e.location))
                return fail("picker must be <location 0-49>:<name>");
            e.name = trim(val.substr(colon + 1));
            if (!printableName(e.name, kStringFields[0].width - 1u))
                return fail("picker name must be 1-31 printable ASCII bytes");
            for (const PickerEntry &o : b.picker)
                if (o.location == e.location)
                    return fail("picker location " + std::to_string(e.location) + " listed twice");
            b.picker.push_back(e);
        }
        else if (key == "node")
        {
            return fail("'node' is not supported (TK6: DONOTUSE nav slots have no menu widget; the menu aborts)");
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

inline void wrStr(uint8_t *ram, uint32_t addr, uint32_t width, const std::string &v)
{
    std::memset(ram + addr, 0, width);
    std::memcpy(ram + addr, v.data(), v.size());
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
// number of writes, or -1 when the tables fail verification (nothing
// written). Picker entries write nothing here (see armPicker).
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
            wrStr(ram, addr, f.width, b.str[i]);
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
    }
    return writes;
}

// ---- Course picker (TK7; PS2X_SSX3_COURSE_PICKER=1, default off) ----------
// One event's slot cycles through a list of courses from the menus: entry 0
// is the row as the manifest left it (e.g. Snow Jam, ARA1), then the
// manifest's `picker` entries (e.g. 49:Garibaldi, the added `dbg` location).
// Each switch rewrites two RAM fields of that event: the name (event +4)
// and the topology location (+16, read by event_load_locations 0x22D088
// when the event loads; the launch maps the menu's event id +0x5C to the row
// at 0x302398). Sky/TRANSP and the connector list stay the event's own.
// Where the name shows (TK7 proof1/proof2): the event intro card
// ("Garibaldi - Race") reads the row live; the Select Event list copies the
// names when the frontend is built (boot, or after Quit to title), so the
// list shows a pick only from the next frontend build. The status line /
// Android toast (the caller) shows it at once.
//
// Input: L3+R3 pressed together (edge) with SELECT up, read from the pad
// buffer the guest gets from scePadRead (port 0), so a pad script drives it
// deterministically. DS1 owns SELECT+L3/R3; the stock game ignores L3+R3 at
// Select Event (TK7 e1). While the chord is held the picker hides L3+R3
// from the guest.
//
// Safe point: no event is live and nothing is loading.
//   - Live event: the app-update dispatch (0x3171B4, `jalr` through
//     [[[0x4A5B64]] + 0x34]; 0x4A5B64 = gp+0x2A74 -> the app manager A,
//     [A] = the current app object) targets the game update 0x2306B8 in an
//     event, pause included, and never in the frontend or after "Quit to
//     title" (FH1 tap upd counts, TK7 e5). The rider pointer [0x53FF4C] is
//     no signal: it stays set after a quit (TK7 e5).
//   - Loading: no ELF location slot (0x442168, 50 x 16 B, +8 state) is in
//     state 8 (load requested; TK5 d1).
// Outside that the chord is refused and logged, so a running or loading
// event never sees its row change.
//
// The current entry is derived from RAM (the event's topology location) on
// every press, so a savestate load keeps the picker consistent.
inline constexpr uint32_t kAppMgrPtr = 0x4A5B64u;
inline constexpr uint32_t kAppUpdateSlot = 0x34u; // app vtable: update function
inline constexpr uint32_t kGameUpdate = 0x2306B8u;
inline constexpr uint32_t kSlotBase = 0x442168u;
inline constexpr uint32_t kSlotStride = 16u;
inline constexpr uint32_t kSlots = 50u;
inline constexpr uint32_t kSlotRequested = 8u;
inline constexpr uint16_t kBtnSelect = 1u << 0; // active-low pad word (data[2] | data[3] << 8)
inline constexpr uint16_t kBtnL3 = 1u << 1;
inline constexpr uint16_t kBtnR3 = 1u << 2;

struct Picker
{
    bool armed = false;
    int event = -1;
    std::vector<PickerEntry> entries; // [0] = the event's row after apply
    bool chordWas = false;
};

inline Picker &picker()
{
    static Picker p;
    return p;
}

inline bool pickerKnob()
{
    const char *v = std::getenv("PS2X_SSX3_COURSE_PICKER");
    return v && std::strcmp(v, "1") == 0;
}

// Arms `p` from applied blocks: entry 0 is read back from RAM. Returns false
// (and leaves `p` disarmed) when no block has picker entries.
template <typename Log>
inline bool armPicker(Picker &p, const uint8_t *ram, const std::vector<Block> &blocks, Log &&log)
{
    p = Picker{};
    for (const Block &b : blocks)
    {
        if (b.picker.empty())
            continue;
        PickerEntry stock;
        stock.location =
            static_cast<int32_t>(rd32(ram, kTopoBase + static_cast<uint32_t>(b.event) * kTopoStride + 16u));
        stock.name = rdStr(ram, kEventBase + static_cast<uint32_t>(b.event) * kEventStride + 4u, 32u);
        p.entries.push_back(stock);
        for (const PickerEntry &e : b.picker)
        {
            if (e.location == stock.location)
            {
                log("picker: entry " + std::to_string(e.location) + ":" + e.name +
                    " repeats the row's own location; skipped");
                continue;
            }
            p.entries.push_back(e);
        }
        if (p.entries.size() < 2u)
        {
            p = Picker{};
            return false;
        }
        p.event = b.event;
        p.armed = true;
        std::string list;
        for (const PickerEntry &e : p.entries)
            list += (list.empty() ? "" : ", ") + std::to_string(e.location) + ":" + e.name;
        log("picker: armed on event " + std::to_string(b.event) + " [" + list + "], chord L3+R3 in the menus");
        return true;
    }
    return false;
}

// Guest pointer -> RAM offset (KSEG bits dropped); 0 when null, unaligned
// or out of RAM.
inline uint32_t guestPtr(const uint8_t *ram, uint32_t addr)
{
    const uint32_t v = rd32(ram, addr) & 0x1FFFFFFFu;
    return (v == 0u || (v & 3u) || v > kRamSize - 4u) ? 0u : v;
}

// The current app's update function (0 when the chain is not set up yet).
inline uint32_t appUpdateFn(const uint8_t *ram)
{
    const uint32_t mgr = guestPtr(ram, kAppMgrPtr);
    const uint32_t app = mgr ? guestPtr(ram, mgr) : 0u;
    const uint32_t vt = app ? guestPtr(ram, app) : 0u;
    return vt && vt + kAppUpdateSlot <= kRamSize - 4u ? rd32(ram, vt + kAppUpdateSlot) : 0u;
}

// Why a switch is unsafe right now, or empty when it is safe.
inline std::string pickerBlocked(const uint8_t *ram)
{
    char buf[96];
    if (appUpdateFn(ram) == kGameUpdate)
        return "an event is live";
    for (uint32_t i = 0; i < kSlots; ++i)
        if (rd32(ram, kSlotBase + i * kSlotStride + 8u) == kSlotRequested)
        {
            std::snprintf(buf, sizeof(buf), "location %u is loading", i);
            return buf;
        }
    return {};
}

// Moves the event's slot to the next entry. Returns the entry index now in
// RAM, or -1 (nothing written) when blocked; `msg` gets the status line.
inline int pickerNext(Picker &p, uint8_t *ram, std::string &msg)
{
    const uint32_t topo = kTopoBase + static_cast<uint32_t>(p.event) * kTopoStride + 16u;
    const uint32_t name = kEventBase + static_cast<uint32_t>(p.event) * kEventStride + 4u;
    const std::string why = pickerBlocked(ram);
    if (!why.empty())
    {
        msg = "course picker: not now, " + why;
        return -1;
    }
    const int32_t cur = static_cast<int32_t>(rd32(ram, topo));
    size_t idx = 0;
    for (size_t i = 0; i < p.entries.size(); ++i)
        if (p.entries[i].location == cur)
            idx = i;
    const size_t next = (idx + 1u) % p.entries.size();
    const PickerEntry &e = p.entries[next];
    std::memcpy(ram + topo, &e.location, 4);
    wrStr(ram, name, 32u, e.name);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "course picker: event %d -> %s (location %d -> %d)", p.event, e.name.c_str(), cur,
                  e.location);
    msg = buf;
    return static_cast<int>(next);
}

// Called for every guest scePadRead (port 0) with the 32-byte pad buffer
// the guest receives. `vsyncTick` only labels the log line. Returns true
// with `status` set when the chord fired (the caller shows it: status line
// + Android toast via ps2_savestate::noteQuickStatus).
inline bool pickerOnPadRead(uint8_t *ram, uint8_t *pad, uint64_t vsyncTick, std::string &status)
{
    Picker &p = picker();
    if (!p.armed || !ram || !pad) // armed is set once in loadELF, before the EE runs
        return false;
    static std::mutex mu; // pad reads come from the EE thread; the lock only guards chordWas
    std::lock_guard<std::mutex> lock(mu);
    const uint16_t buttons = static_cast<uint16_t>(pad[2] | (pad[3] << 8));
    const bool chord = (buttons & (kBtnL3 | kBtnR3)) == 0u && (buttons & kBtnSelect) != 0u;
    const bool fired = chord && !p.chordWas;
    if (fired)
    {
        std::string msg;
        const int idx = pickerNext(p, ram, msg);
        std::fprintf(stderr, "[ssx3-course] tick=%llu %s\n", static_cast<unsigned long long>(vsyncTick),
                     msg.c_str());
        status = idx >= 0 ? "Course: " + p.entries[static_cast<size_t>(idx)].name : msg;
    }
    p.chordWas = chord;
    if (chord)
        pad[2] = static_cast<uint8_t>(pad[2] | kBtnL3 | kBtnR3);
    return fired;
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
    if (n < 0)
        return;
    log("applied " + std::to_string(n) + " writes in " + std::to_string(blocks.size()) + " event blocks");
    bool hasPicker = false;
    for (const Block &b : blocks)
        hasPicker = hasPicker || !b.picker.empty();
    if (!hasPicker)
        return;
    if (!pickerKnob())
    {
        log("picker: entries ignored (PS2X_SSX3_COURSE_PICKER is not 1)");
        return;
    }
    armPicker(picker(), ram, blocks, log);
}

} // namespace ps2_ssx3_course
