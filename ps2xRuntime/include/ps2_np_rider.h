#pragma once
// np-fix-1 (F7/F8): tick-stamped remote-script rider state and decision logic.
//
// B0 scaffold: Config/loadOnce/findEntry moved VERBATIM from ps2_runtime.cpp
// (same lenient behavior). Pure helpers (parseScriptText, Session, decide)
// expose the logic to unit tests with the OLD lenient semantics; the strict
// fix tightens them. Production hook behavior is unchanged by this move.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace ps2_np_rider
{
constexpr uint32_t kInputConsumer = 0x00111728u;
constexpr uint32_t kAiSourceDesc = 0x004585F0u;

struct Entry
{
    uint64_t tick = 0u;
    uint32_t de4 = 0u;
    uint32_t w0 = 0u;
    uint32_t w1 = 0u;
};

struct Config
{
    bool enabled = false;
    int slot = -1;
    uint64_t dropTick = 0u; // 0 = never; first tick that reverts to AI
    std::vector<Entry> script;
    size_t cursor = 0u; // tick-stamped scripts are consumed in order
    bool loaded = false;
    bool loadError = false;
    // F8 session binding (B0: present but unused by decide; the fix binds).
    uint64_t generation = 0u;
    uint32_t boundActor = 0u;
    bool boundActorValid = false;
};

inline Config &config()
{
    static Config cfg;
    return cfg;
}

// Split file content the way fgets(line, 256) would chunk it: a chunk that
// does not end at a newline while more data follows is a truncated (overlong)
// line. B0 keeps the raw chunks; the strict fix rejects truncated ones.
inline std::vector<std::string> splitFgetsChunks(const std::string &content, size_t width = 256)
{
    std::vector<std::string> chunks;
    size_t pos = 0;
    const size_t cap = width > 1 ? width - 1 : 1;
    while (pos < content.size())
    {
        size_t nl = content.find('\n', pos);
        bool hasNl = nl != std::string::npos;
        size_t end = hasNl ? nl + 1 : content.size();
        while (pos < end)
        {
            size_t take = end - pos;
            bool truncated = false;
            if (take > cap)
            {
                take = cap;
                truncated = true;
            }
            chunks.push_back(content.substr(pos, take));
            (void)truncated;
            pos += take;
        }
        (void)hasNl;
    }
    return chunks;
}

// B0 lenient core (verbatim old semantics): skip #/blank, keep sscanf==4
// lines silently, sort by tick only. error is always empty here; the strict
// fix reports malformed/conflicting/overlong/unreadable input.
inline void parseScriptText(const std::string &content, std::vector<Entry> &entries, std::string &error)
{
    entries.clear();
    error.clear();
    for (const std::string &line : splitFgetsChunks(content))
    {
        if (line.empty() || line[0] == '#' || line[0] == '\n')
            continue;
        unsigned long long tick = 0u;
        unsigned de4 = 0u, w0 = 0u, w1 = 0u;
        if (std::sscanf(line.c_str(), "%llu %x %x %x", &tick, &de4, &w0, &w1) == 4)
            entries.push_back(Entry{static_cast<uint64_t>(tick), de4, w0, w1});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.tick < b.tick; });
}

inline bool readScriptFile(const std::string &path, std::string &content, std::string &error)
{
    content.clear();
    error.clear();
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f)
    {
        error = "cannot read script '" + path + "'";
        return false;
    }
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) != 0u)
        content.append(buf, n);
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok)
        error = "error reading script '" + path + "'";
    return ok;
}

inline void loadOnce()
{
    Config &cfg = config();
    if (cfg.loaded)
        return;
    cfg.loaded = true;
    const char *en = std::getenv("PS2X_NP_RIDER_ENABLE");
    cfg.enabled = en && std::string(en) == "1";
    if (!cfg.enabled)
        return;
    if (const char *s = std::getenv("PS2X_NP_RIDER_SLOT"))
        cfg.slot = std::atoi(s);
    if (const char *d = std::getenv("PS2X_NP_RIDER_DROP_TICK"))
        cfg.dropTick = static_cast<uint64_t>(std::strtoull(d, nullptr, 0));
    const char *spec = std::getenv("PS2X_NP_RIDER_SCRIPT");
    if (!spec || !spec[0])
        return; // trace-only: log, never replace
    std::string path = spec;
    if (path.rfind("@", 0) == 0)
        path = path.substr(1);
    std::string content, error;
    if (!readScriptFile(path, content, error))
    {
        cfg.loadError = true;
        std::fprintf(stderr, "[np-rider] cannot read script '%s'; trace-only\n", path.c_str());
        return;
    }
    parseScriptText(content, cfg.script, error);
    // B0: lenient — error is always empty here, script applies as before.
    std::fprintf(stderr, "[np-rider] slot=%d drop=%llu entries=%zu\n", cfg.slot,
                 (unsigned long long)cfg.dropTick, cfg.script.size());
}

inline const Entry *findEntry(uint64_t tick)
{
    Config &cfg = config();
    while (cfg.cursor < cfg.script.size() && cfg.script[cfg.cursor].tick < tick)
        ++cfg.cursor;
    if (cfg.cursor < cfg.script.size() && cfg.script[cfg.cursor].tick == tick)
        return &cfg.script[cfg.cursor];
    return nullptr;
}

// Per-call consumption state. B0: advance-only cursor mirror; the fix adds
// generation binding, backward-tick re-derivation and ordinal tracking.
struct Session
{
    size_t cursor = 0u;
    uint64_t lastTick = 0u;
    bool hasLastTick = false;
    uint32_t ordinal = 0u;
    uint64_t generation = 0u;
    uint32_t boundActor = 0u;
    bool boundActorValid = false;
};

struct Decision
{
    bool applied = false;
    uint32_t outW0 = 0u;
    uint32_t outW1 = 0u;
    const char *why = "no-entry";
    uint32_t ordinal = 0u;
};

// B0 verbatim hook logic as a pure function (loadError ignored, actor
// binding absent, no rewind, ordinal always 0). The hook keeps calling the
// global path; tests drive this directly with explicit state.
inline Decision decide(Session &s, const Config &cfg, uint64_t tick, bool isTarget, uint32_t actor,
                       uint32_t de4, bool aiOwned, uint32_t w0, uint32_t w1)
{
    (void)actor; // B0: no session/actor generation binding yet (F8 fix binds it).
    Decision d;
    d.outW0 = w0;
    d.outW1 = w1;
    if (!isTarget)
    {
        d.why = "slot0";
        return d;
    }
    while (s.cursor < cfg.script.size() && cfg.script[s.cursor].tick < tick)
        ++s.cursor;
    const Entry *e = (s.cursor < cfg.script.size() && cfg.script[s.cursor].tick == tick)
                         ? &cfg.script[s.cursor]
                         : nullptr;
    const bool pastDrop = cfg.dropTick != 0u && tick >= cfg.dropTick;
    const bool protectedState = de4 == 9u || de4 == 10u || de4 == 13u;
    if (e && !pastDrop && !protectedState && aiOwned && e->de4 == de4)
    {
        d.applied = true;
        d.outW0 = e->w0;
        d.outW1 = e->w1;
        d.why = "applied";
        return d;
    }
    d.why = !e ? "no-entry" : pastDrop ? "dropped" : protectedState ? "protected"
        : !aiOwned           ? "src-mismatch"
                             : "state-mismatch";
    return d;
}

inline void resetSession(Session &s, uint64_t generation = 0u)
{
    s = Session{};
    s.generation = generation;
}

} // namespace ps2_np_rider
