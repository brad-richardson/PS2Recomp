#pragma once
// np-fix-1 (F7/F8): tick-stamped remote-script rider state and decision logic.
//
// Config/loadOnce moved from ps2_runtime.cpp; the strict fix adds F7 loader
// strictness (malformed/conflicting/overlong/unreadable scripts refuse the
// run) and the F8 per-call consumption contract (session/actor generation
// binding, backward-tick cursor re-derivation, per-tick ordinals).
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

// (fgets(256) chunking is emulated inside parseScriptText so overlong lines
// are detected exactly as the old file reader split them.)

// np-fix-1 F7: strict loader. Malformed lines, truncated (overlong) lines,
// conflicting duplicate ticks and unreadable files are errors that refuse
// the whole run; only byte-identical duplicate ticks dedup. error is empty
// on success and the script is sorted by tick.
inline void parseScriptText(const std::string &content, std::vector<Entry> &entries, std::string &error)
{
    entries.clear();
    error.clear();
    struct Chunk
    {
        std::string text;
        bool truncated = false;
    };
    std::vector<Chunk> chunks;
    {
        const size_t cap = 255u; // fgets(line, 256) chunking
        size_t pos = 0u;
        while (pos < content.size())
        {
            size_t nl = content.find('\n', pos);
            const size_t end = (nl == std::string::npos) ? content.size() : nl + 1u;
            while (pos < end)
            {
                size_t take = end - pos;
                bool trunc = false;
                if (take > cap)
                {
                    take = cap;
                    trunc = true;
                }
                chunks.push_back(Chunk{content.substr(pos, take), trunc});
                pos += take;
            }
        }
    }
    std::vector<Entry> parsed;
    size_t lineno = 0u;
    for (const Chunk &c : chunks)
    {
        ++lineno;
        const std::string &line = c.text;
        if (line.empty() || line[0] == '#' || line[0] == '\n')
            continue;
        if (c.truncated)
        {
            error = "overlong line " + std::to_string(lineno);
            entries.clear();
            return;
        }
        unsigned long long tick = 0u;
        unsigned de4 = 0u, w0 = 0u, w1 = 0u;
        if (std::sscanf(line.c_str(), "%llu %x %x %x", &tick, &de4, &w0, &w1) != 4)
        {
            error = "malformed line " + std::to_string(lineno);
            entries.clear();
            return;
        }
        parsed.push_back(Entry{static_cast<uint64_t>(tick), de4, w0, w1});
    }
    std::sort(parsed.begin(), parsed.end(),
              [](const Entry &a, const Entry &b) { return a.tick < b.tick; });
    for (size_t i = 0u; i < parsed.size();)
    {
        size_t j = i + 1u;
        while (j < parsed.size() && parsed[j].tick == parsed[i].tick)
        {
            if (parsed[j].de4 != parsed[i].de4 || parsed[j].w0 != parsed[i].w0 ||
                parsed[j].w1 != parsed[i].w1)
            {
                error = "conflicting duplicate tick " + std::to_string(parsed[i].tick);
                entries.clear();
                return;
            }
            ++j; // byte-identical duplicate: keep one
        }
        entries.push_back(parsed[i]);
        i = j;
    }
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
    if (!error.empty())
    {
        // np-fix-1 F7: a malformed script refuses the run (fail closed with
        // a clear error) instead of silently driving a partial script.
        cfg.loadError = true;
        cfg.script.clear();
        std::fprintf(stderr, "[np-rider] FATAL: strict script '%s': %s; refusing applies\n",
                     path.c_str(), error.c_str());
        return;
    }
    std::fprintf(stderr, "[np-rider] slot=%d drop=%llu entries=%zu\n", cfg.slot,
                 (unsigned long long)cfg.dropTick, cfg.script.size());
}

// (The old global-cursor findEntry is retired: decide() carries the cursor
// in its Session, with backward-tick re-derivation.)

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

// np-fix-1 F8: per-call consumption contract with a session/actor
// generation. The first apply in a session binds (generation, actor); a
// foreign actor in the same generation is refused (slot reuse/retire
// cannot hijack the drive). A backward tick (load/rewind) resets the
// cursor and the binding so the cursor is re-derived. Same-tick calls
// share the entry's words with distinct ordinals (idempotent re-apply).
inline Decision decide(Session &s, const Config &cfg, uint64_t tick, bool isTarget, uint32_t actor,
                       uint32_t de4, bool aiOwned, uint32_t w0, uint32_t w1)
{
    Decision d;
    d.outW0 = w0;
    d.outW1 = w1;
    if (cfg.loadError)
    {
        d.why = "load-error";
        return d;
    }
    if (!isTarget)
    {
        d.why = "slot0";
        return d;
    }
    if (!s.hasLastTick || tick != s.lastTick)
    {
        if (s.hasLastTick && tick < s.lastTick)
        {
            s.cursor = 0u; // backward tick: re-derive from the script start
            s.boundActorValid = false;
        }
        s.lastTick = tick;
        s.hasLastTick = true;
        s.ordinal = 0u;
    }
    d.ordinal = s.ordinal++;
    while (s.cursor < cfg.script.size() && cfg.script[s.cursor].tick < tick)
        ++s.cursor;
    const Entry *e = (s.cursor < cfg.script.size() && cfg.script[s.cursor].tick == tick)
                         ? &cfg.script[s.cursor]
                         : nullptr;
    const bool pastDrop = cfg.dropTick != 0u && tick >= cfg.dropTick;
    const bool protectedState = de4 == 9u || de4 == 10u || de4 == 13u;
    if (e && !pastDrop && !protectedState && aiOwned && e->de4 == de4)
    {
        if (!s.boundActorValid)
        {
            s.boundActor = actor;
            s.boundActorValid = true;
        }
        if (s.boundActor != actor)
        {
            d.why = "gen-mismatch";
            return d;
        }
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
