#pragma once
// TK11: SSX 3 frontend entry for Tricky mode (PS2X_SSX3_TRICKY_MENU=1,
// default off; the mode itself is TK9's course mode 1, or the TK7 picker).
//
// Three pieces, all installed only when the knob is 1 (knob off: nothing is
// wrapped, nothing is read or written):
//
// (b) Main menu. The dead "Online" row becomes "Tricky Courses": it leads to
//     Single Event with Tricky mode on; any other main-menu entry (and every
//     main-menu build, i.e. backing out of Single Event or quitting to the
//     title) turns the mode off, so plain Single Event stays stock.
//     - Routing (TK11 t1/t2 taps + disassembly): the main-menu build calls
//       0x194D18(this, item) once per row, which stores the row's next
//       frontend state in item+0x18 (widget hash at item+0x38: "2quick play"
//       -> 9, "5online" -> 0x31). On X the menu calls its event handler
//       0x194EA8(this, item, event 5), which switches on item+0x18 (jump
//       table 0x4600B0: state 9 -> 0x145108(gm, 1), Single Event; state 0x31
//       -> 0x145108(gm, 2) + the network bring-up). The accept hook rewrites
//       item+0x18 = 9 before the handler reads it, so "Online" runs the
//       Single Event case exactly.
//     - Label: the row's text key 0x12F325 ("Online") is in CMNAMER.LOC,
//       which the game keeps in RAM from boot (t63). The LOC is LOCH header,
//       LOCT {u32 keyHash, u32 index} entries from +0x24, then LOCL at the
//       pool offset (+0x10): {"LOCL", size, 0, count, u32 offsets[count]}
//       and UTF-16 strings (offsets relative to the pool); in RAM the
//       loader has replaced each LOCT index with the string's address. "Tricky Courses"
//       does not fit in "Online"'s 6 characters, so the hook writes it over
//       string 940 ("The SSX Lobby Server could not be located...", 89
//       characters, shown only after an online connection) and points
//       Online's LOCT entry at index 940. The Online help line (FEAMER.LOC key
//       0x1537AE5, 47 characters) is rewritten in place.
// (a) Select Event list refresh. The Map screen (Select Peak / Goal / Event)
//     fills its row texts Peak%d{Race,Freestyle,Freeride}Loc%d from the event
//     rows in 0x200DE8(controller), called once per Map-screen entry from
//     0x186610 -> 0x200A70(a1 = 1) (t1: t1010 and t1940). A picker switch made
//     inside the Map screen (TK7's lag) therefore needs one more fill. After
//     a switch, the scePadRead glue 0x3FFA58 returns into 0x200DE8 with
//     ra = the trampoline 0x200ABC (a padding nop after 0x200A70, never a
//     branch target); the trampoline restores v0/v1/ra and returns to the
//     real caller. Both hops go through the scheduler (ctx->pc), so an EE
//     checkpoint inside the fill resumes like any other guest call.
//     The controller comes from the fill's own entry (a0) and is forgotten at
//     each main-menu build and whenever an event is live.
// (c) Not built: per-cursor course pictures in the Map pane (TK11 report).
//
// Guest-affecting when on (it rewrites guest .data/heap and redirects one
// return); off by default. Platform-neutral so the host unit test compiles it.

#include "ps2_ssx3_course_manifest.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ps2_ssx3_tricky
{

inline constexpr uint32_t kMainMenuItemState = 0x194D18u; // (this, item): item+0x18 = next state
inline constexpr uint32_t kMainMenuEvent = 0x194EA8u;     // (this, item, event)
inline constexpr uint32_t kMapFill = 0x200DE8u;           // (controller): Select Event row texts
inline constexpr uint32_t kTrampoline = 0x200ABCu;        // nop after 0x200A70 (jr ra at 0x200AB4)
inline constexpr uint32_t kPadReadGlue = 0x3FFA58u;       // generated scePadRead stub entry
inline constexpr uint32_t kLuiLoad = 0x397C40u;           // lui_load(obj, "data/ui/xx")

// First instruction words, checked before any wrapper is installed.
struct EntryWord
{
    uint32_t pc;
    uint32_t word;
};
inline constexpr EntryWord kEntryWords[] = {
    {kMainMenuItemState, 0x27BDFFD0u}, // addiu sp, sp, -0x30
    {kMainMenuEvent, 0x27BDFFC0u},     // addiu sp, sp, -0x40
    {kMapFill, 0x27BDFCC0u},           // addiu sp, sp, -0x340
    {0x200AB4u, 0x03E00008u},          // jr ra (end of 0x200A70)
    {kTrampoline, 0x00000000u},        // nop padding
    {kLuiLoad, 0x27BDFF60u},           // addiu sp, sp, -0xA0
};

inline constexpr uint32_t kItemState = 0x18u;
inline constexpr uint32_t kItemWidgetHash = 0x38u;
inline constexpr uint32_t kEventAccept = 5u;
inline constexpr uint32_t kStateSingleEvent = 9u;
inline constexpr uint32_t kStateOnline = 0x31u;

// func_317618: PJW variant, h ^= g >> 23 (TK8 §1.2).
inline uint32_t nameHash(const char *s)
{
    uint32_t h = 0;
    for (; *s; ++s)
    {
        h = (h << 4) + static_cast<uint32_t>(static_cast<int32_t>(static_cast<signed char>(*s)));
        const uint32_t g = h & 0xF0000000u;
        if (g)
        {
            h ^= g >> 23;
            h ^= g;
        }
    }
    return h;
}

// ---- Locale patches ---------------------------------------------------------
struct LocFile
{
    const char *name;
    uint32_t pool;  // LOCH +0x10
    uint32_t id;    // LOCH +0x1C
    uint32_t count; // LOCL +0x0C
};
inline constexpr LocFile kCmnAmer = {"CMNAMER.LOC", 0x2B74u, 0x3F691952u, 1386u};
inline constexpr LocFile kFeAmer = {"FEAMER.LOC", 0x1A0Cu, 0x3F691951u, 829u};

inline constexpr uint32_t kKeyOnline = 0x0012F325u;     // CMNAMER, index 375 "Online"
inline constexpr uint32_t kOnlineIndex = 375u;
inline constexpr uint32_t kDonorIndex = 940u;           // "The SSX Lobby Server could not be located..."
inline constexpr uint32_t kKeyOnlineHelp = 0x01537AE5u; // FEAMER, index 6
inline constexpr uint32_t kOnlineHelpIndex = 6u;
inline constexpr const char *kLabel = "Tricky Courses";
inline constexpr const char *kHelp = "Ride the courses of SSX Tricky.";

inline uint32_t rd32(const uint8_t *ram, uint32_t a)
{
    uint32_t v = 0;
    std::memcpy(&v, ram + a, 4);
    return v;
}
inline void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + a, &v, 4); }

// True when a LOC file with this header sits at `a`.
inline bool locAt(const uint8_t *ram, uint32_t ramSize, uint32_t a, const LocFile &f)
{
    if (a + f.pool + 16u > ramSize)
        return false;
    return std::memcmp(ram + a, "LOCH", 4) == 0 && rd32(ram, a + 0x10u) == f.pool &&
           std::memcmp(ram + a + 0x14u, "LOCT", 4) == 0 && rd32(ram, a + 0x1Cu) == f.id &&
           std::memcmp(ram + a + f.pool, "LOCL", 4) == 0 && rd32(ram, a + f.pool + 12u) == f.count;
}

// First match at or above `from` (4-byte steps), or 0.
inline uint32_t findLoc(const uint8_t *ram, uint32_t ramSize, const LocFile &f, uint32_t from = 0x100000u)
{
    for (uint32_t a = from & ~3u; a + 0x40u < ramSize; a += 4u)
        if (ram[a] == 'L' && std::memcmp(ram + a, "LOCH", 4) == 0 && locAt(ram, ramSize, a, f))
            return a;
    return 0u;
}

// Address of the LOCT entry for `key`, or 0.
inline uint32_t loctEntry(const uint8_t *ram, uint32_t base, const LocFile &f, uint32_t key)
{
    for (uint32_t p = base + 0x24u; p + 8u <= base + f.pool; p += 8u)
        if (rd32(ram, p) == key)
            return p;
    return 0u;
}

inline uint32_t locString(const uint8_t *ram, uint32_t base, const LocFile &f, uint32_t index)
{
    return base + f.pool + rd32(ram, base + f.pool + 16u + 4u * index);
}

inline std::string utf16At(const uint8_t *ram, uint32_t ramSize, uint32_t a, size_t max = 256)
{
    std::string s;
    for (size_t i = 0; i < max && a + 2u * i + 1u < ramSize; ++i)
    {
        const uint16_t c = static_cast<uint16_t>(ram[a + 2u * i] | (ram[a + 2u * i + 1u] << 8));
        if (!c)
            break;
        s.push_back(c < 0x80u ? static_cast<char>(c) : '?');
    }
    return s;
}

inline void writeUtf16(uint8_t *ram, uint32_t a, const char *s)
{
    size_t i = 0;
    for (; s[i]; ++i)
    {
        ram[a + 2u * i] = static_cast<uint8_t>(s[i]);
        ram[a + 2u * i + 1u] = 0u;
    }
    ram[a + 2u * i] = 0u;
    ram[a + 2u * i + 1u] = 0u;
}

// A LOCT entry's value: the string index on disc; the game's loader turns it
// into the string's address in RAM (TK11 k1: Online's entry read 0xAD841C).
// Returns the string address either way, or 0.
inline uint32_t entryString(const uint8_t *ram, uint32_t ramSize, uint32_t base, const LocFile &f, uint32_t value)
{
    if (value < f.count)
        return locString(ram, base, f, value);
    const uint32_t a = value & 0x1FFFFFFFu;
    return a > base && a < ramSize ? a : 0u;
}

// The value that makes an entry point at string `index`, in the form `like` uses.
inline uint32_t entryValue(const uint8_t *ram, uint32_t base, const LocFile &f, uint32_t like, uint32_t index)
{
    return like < f.count ? index : (like & 0xE0000000u) | locString(ram, base, f, index);
}

// Relabels Online in the CMNAMER copy at `base`. 1 = patched now, 0 = already
// patched, -1 = refused (layout or strings differ; nothing written).
inline int patchLabel(uint8_t *ram, uint32_t ramSize, uint32_t base, std::string &msg)
{
    const LocFile &f = kCmnAmer;
    const uint32_t e = locAt(ram, ramSize, base, f) ? loctEntry(ram, base, f, kKeyOnline) : 0u;
    if (!e)
    {
        msg = "label: no Online entry in CMNAMER";
        return -1;
    }
    const uint32_t value = rd32(ram, e + 4u);
    const uint32_t target = entryString(ram, ramSize, base, f, value);
    const uint32_t donor = locString(ram, base, f, kDonorIndex);
    const std::string cur = utf16At(ram, ramSize, donor);
    if (target == donor && cur == kLabel)
        return 0;
    if (target != locString(ram, base, f, kOnlineIndex) || utf16At(ram, ramSize, target) != "Online" ||
        cur.rfind("The SSX Lobby Server", 0) != 0 || cur.size() < std::strlen(kLabel))
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "label: CMNAMER strings differ (entry 0x%x, donor \"%s\"); nothing written",
                      value, cur.substr(0, 24).c_str());
        msg = buf;
        return -1;
    }
    const uint32_t next = entryValue(ram, base, f, value, kDonorIndex);
    writeUtf16(ram, donor, kLabel);
    wr32(ram, e + 4u, next);
    char buf[192];
    std::snprintf(buf, sizeof(buf), "label: CMNAMER at 0x%06x, key 0x%x entry 0x%x -> 0x%x (\"%s\" at 0x%06x)", base,
                  kKeyOnline, value, next, kLabel, donor);
    msg = buf;
    return 1;
}

// Rewrites the Online help line in the FEAMER copy at `base` (in place, shorter).
inline int patchHelp(uint8_t *ram, uint32_t ramSize, uint32_t base, std::string &msg)
{
    const LocFile &f = kFeAmer;
    const uint32_t e = locAt(ram, ramSize, base, f) ? loctEntry(ram, base, f, kKeyOnlineHelp) : 0u;
    const uint32_t s = e ? entryString(ram, ramSize, base, f, rd32(ram, e + 4u)) : 0u;
    if (!s || s != locString(ram, base, f, kOnlineHelpIndex))
    {
        msg = "help: no Online help entry in FEAMER";
        return -1;
    }
    const std::string cur = utf16At(ram, ramSize, s);
    if (cur == kHelp)
        return 0;
    if (cur.rfind("Compete against", 0) != 0 || cur.size() < std::strlen(kHelp))
    {
        msg = "help: FEAMER string differs (\"" + cur.substr(0, 24) + "\"); nothing written";
        return -1;
    }
    writeUtf16(ram, s, kHelp);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "help: FEAMER at 0x%06x, string %u at 0x%06x -> \"%s\"", base, kOnlineHelpIndex, s,
                  kHelp);
    msg = buf;
    return 1;
}

// ---- Mode -------------------------------------------------------------------
// Tricky mode on = TK9 course mode 1 (the manifest's first `mode` block), off =
// Stock. Without modes, the TK7 picker slot stands in: entry 1 on, entry 0 off.
// TK9 only cycles (Stock -> 1 -> ... -> Stock), so this steps modeNext until
// modeCurrent is the target; a refusal comes on the first step, before any
// write (pickerBlocked, foreign poke bytes). Returns true when RAM changed;
// `msg` gets the log line (empty = no-op).
inline bool modeSet(uint8_t *ram, bool on, std::string &msg)
{
    using namespace ps2_ssx3_course;
    msg.clear();
    Modes &ms = courseModes();
    if (ms.armed)
    {
        const size_t want = on ? 1u : 0u;
        size_t cur = modeCurrent(ms, ram);
        if (cur == want)
            return false;
        std::string step;
        for (size_t k = 0; k <= ms.modes.size() && cur != want; ++k)
        {
            if (modeNext(ms, ram, step) < 0)
            {
                msg = "mode: " + step;
                return k != 0;
            }
            cur = modeCurrent(ms, ram);
        }
        msg = std::string("mode: Tricky ") + (on ? "on (course mode " + ms.modes[0].name + ")" : "off (Stock)");
        return true;
    }
    Picker &p = picker();
    if (!p.armed || p.entries.size() < 2u)
    {
        if (on)
            msg = "mode: no course modes or picker armed (PS2X_SSX3_COURSE_PICKER=1 + manifest); stock rows";
        return false;
    }
    const size_t want = on ? 1u : 0u;
    const uint32_t topo = kTopoBase + static_cast<uint32_t>(p.event) * kTopoStride + 16u;
    const int32_t cur = static_cast<int32_t>(ps2_ssx3_course::rd32(ram, topo));
    if (cur == p.entries[want].location)
        return false;
    const std::string why = pickerBlocked(ram);
    if (!why.empty())
    {
        msg = "mode: not now, " + why;
        return false;
    }
    const PickerEntry &e = p.entries[want];
    std::memcpy(ram + topo, &e.location, 4);
    wrStr(ram, kEventBase + static_cast<uint32_t>(p.event) * kEventStride + 4u, 32u, e.name);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "mode: Tricky %s, event %d -> %s (location %d -> %d)", on ? "on" : "off", p.event,
                  e.name.c_str(), cur, e.location);
    msg = buf;
    return true;
}

// The bytes a list refresh depends on: the 23 event rows (names) and the
// topology table (picker locations). A change across one scePadRead = a chord.
inline constexpr uint32_t kRowsBytes = ps2_ssx3_course::kRows * ps2_ssx3_course::kEventStride;
inline constexpr uint32_t kTopoBytes = ps2_ssx3_course::kRows * ps2_ssx3_course::kTopoStride;
inline bool rowsArmed()
{
    return ps2_ssx3_course::picker().armed || ps2_ssx3_course::courseModes().armed;
}

// ---- Host state -------------------------------------------------------------
struct State
{
    bool on = false; // knob
    bool tricky = false;
    uint32_t mapController = 0u; // 0x200DE8's a0 since the last main-menu build / event
    uint32_t cmnamer = 0u;       // patched CMNAMER copy
    bool labelDone = false;
    bool labelRefused = false;
    // Refresh redirect in flight (scePadRead glue -> 0x200DE8 -> trampoline).
    bool redirect = false;
    uint32_t returnPc = 0u;
    uint32_t sp = 0u;
    uint32_t refreshes = 0u;
};

inline State &state()
{
    static State s;
    return s;
}

inline bool knob()
{
    const char *v = std::getenv("PS2X_SSX3_TRICKY_MENU");
    return v && std::strcmp(v, "1") == 0;
}

// Accept on a main-menu row: returns the state to store in item+0x18 before
// the handler reads it (unchanged unless the row is Online).
inline uint32_t onMainMenuAccept(uint32_t widgetHash, uint32_t itemState, bool &online)
{
    online = widgetHash == nameHash("5online");
    return online ? kStateSingleEvent : itemState;
}

} // namespace ps2_ssx3_tricky
