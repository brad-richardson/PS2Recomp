// ACH2: local achievements runtime (see ps2_ach.h). Single owner of the
// vendored rc_runtime; called on the EE thread from EeScheduler VBlankStart.
// Read-only wrt the guest: the only guest contact is MemView reads inside
// rc_runtime_do_frame, so det hashes are identical with it running.

#include "ps2_ach.h"

#include "ps2_fh1_full120.h"
#include "ps2_runtime.h"
#include "ps2_ssx3_course_manifest.h"
#include "ps2_ssx3_tricky_menu.h"
#include "ps2_ui_toast.h"

#include "rc_consoles.h"
#include "rc_error.h"
#include "rc_hash.h"
#include "rc_runtime.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ps2_ach
{

bool trickyActive()
{
    const char *overlay = std::getenv("PS2X_CD_OVERLAY");
    if (overlay && overlay[0] != '\0')
        return true;
    return ps2_ssx3_course::pickerKnob() || ps2_ssx3_tricky::knob();
}

uint32_t readGuestMemory(uint32_t address, uint8_t *buffer, uint32_t numBytes, const MemView &view)
{
    if (!buffer || numBytes == 0u)
        return 0u;
    const uint8_t *base = nullptr;
    uint32_t avail = 0u;
    if (address < kRdramSize)
    {
        base = view.rdram;
        avail = kRdramSize - address;
    }
    else if (address >= kScratchBase && address < kScratchBase + kScratchSize)
    {
        base = view.scratch;
        avail = kScratchBase + kScratchSize - address;
    }
    else
    {
        return 0u;
    }
    if (!base)
        return 0u;
    if (numBytes > avail)
        numBytes = avail;
    std::memcpy(buffer, base + (address < kRdramSize ? address : address - kScratchBase), numBytes);
    return numBytes;
}

// ---- Minimal JSON (objects, arrays, strings + escapes, numbers) ------------
namespace json
{

struct Value
{
    enum Type { Nul, Bool, Num, Str, Arr, Obj } type = Nul;
    bool boolean = false;
    double number = 0.0;
    std::string str; // Str: decoded; Num: raw spelling
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;
    const Value *find(const char *key) const
    {
        for (const auto &kv : obj)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }
};

struct Parser
{
    const char *p;
    const char *end;
    std::string err;
    explicit Parser(const std::string &text) : p(text.data()), end(text.data() + text.size()) {}
    void skipWs()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }
    bool fail(const char *msg)
    {
        err = msg;
        return false;
    }
    static void encodeUtf8(std::string &out, uint32_t cp)
    {
        if (cp < 0x80u)
            out.push_back(static_cast<char>(cp));
        else if (cp < 0x800u)
        {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
        else
        {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    bool parseString(std::string &out)
    {
        if (p >= end || *p != '"')
            return fail("want string");
        ++p;
        while (p < end && *p != '"')
        {
            if (*p != '\\')
            {
                out.push_back(*p++);
                continue;
            }
            if (++p >= end)
                break;
            switch (*p++)
            {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
            {
                if (end - p < 4)
                    return fail("bad \\u");
                uint32_t cp = 0;
                for (int i = 0; i < 4; ++i)
                {
                    char c = *p++;
                    cp <<= 4;
                    if (c >= '0' && c <= '9')
                        cp |= static_cast<uint32_t>(c - '0');
                    else if (c >= 'a' && c <= 'f')
                        cp |= static_cast<uint32_t>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F')
                        cp |= static_cast<uint32_t>(c - 'A' + 10);
                    else
                        return fail("bad \\u");
                }
                encodeUtf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        if (p >= end)
            return fail("unterminated string");
        ++p;
        return true;
    }
    bool parseNumber(Value &v)
    {
        const char *start = p;
        if (p < end && (*p == '-' || *p == '+'))
            ++p;
        while (p < end && (std::isdigit(static_cast<unsigned char>(*p)) || *p == '.' || *p == 'e' ||
                            *p == 'E' || *p == '+' || *p == '-'))
            ++p;
        if (p == start)
            return fail("want value");
        v.type = Value::Num;
        v.str.assign(start, p);
        v.number = std::strtod(v.str.c_str(), nullptr);
        return true;
    }
    bool parseValue(Value &v)
    {
        skipWs();
        if (p >= end)
            return fail("want value");
        if (*p == '{')
        {
            ++p;
            v.type = Value::Obj;
            skipWs();
            if (p < end && *p == '}')
            {
                ++p;
                return true;
            }
            while (true)
            {
                skipWs();
                std::string key;
                if (!parseString(key))
                    return false;
                skipWs();
                if (p >= end || *p != ':')
                    return fail("want :");
                ++p;
                Value child;
                if (!parseValue(child))
                    return false;
                v.obj.emplace_back(std::move(key), std::move(child));
                skipWs();
                if (p >= end)
                    return fail("unterminated object");
                if (*p == '}')
                {
                    ++p;
                    return true;
                }
                if (*p != ',')
                    return fail("want ,");
                ++p;
            }
        }
        if (*p == '[')
        {
            ++p;
            v.type = Value::Arr;
            skipWs();
            if (p < end && *p == ']')
            {
                ++p;
                return true;
            }
            while (true)
            {
                Value child;
                if (!parseValue(child))
                    return false;
                v.arr.push_back(std::move(child));
                skipWs();
                if (p >= end)
                    return fail("unterminated array");
                if (*p == ']')
                {
                    ++p;
                    return true;
                }
                if (*p != ',')
                    return fail("want ,");
                ++p;
            }
        }
        if (*p == '"')
        {
            v.type = Value::Str;
            return parseString(v.str);
        }
        if (end - p >= 4 && std::strncmp(p, "true", 4) == 0)
        {
            p += 4;
            v.type = Value::Bool;
            v.boolean = true;
            return true;
        }
        if (end - p >= 5 && std::strncmp(p, "false", 5) == 0)
        {
            p += 5;
            v.type = Value::Bool;
            return true;
        }
        if (end - p >= 4 && std::strncmp(p, "null", 4) == 0)
        {
            p += 4;
            v.type = Value::Nul;
            return true;
        }
        return parseNumber(v);
    }
    bool run(Value &v)
    {
        if (!parseValue(v))
            return false;
        skipWs();
        if (p != end)
            return fail("trailing text");
        return true;
    }
};

inline const Value *field(const Value &o, const char *upper, const char *lower)
{
    if (o.type != Value::Obj)
        return nullptr;
    if (const Value *v = o.find(upper))
        return v;
    return o.find(lower);
}

inline bool asUint(const Value *v, uint32_t &out)
{
    if (!v)
        return false;
    if (v->type == Value::Num)
    {
        if (v->number < 0.0 || v->number > 4294967295.0)
            return false;
        out = static_cast<uint32_t>(v->number);
        return true;
    }
    if (v->type == Value::Str)
    {
        char *stop = nullptr;
        unsigned long n = std::strtoul(v->str.c_str(), &stop, 10);
        if (!stop || *stop != '\0' || n > 0xFFFFFFFFul)
            return false;
        out = static_cast<uint32_t>(n);
        return true;
    }
    return false;
}

inline std::string asString(const Value *v)
{
    if (!v)
        return {};
    if (v->type == Value::Str)
        return v->str;
    if (v->type == Value::Num)
        return v->str;
    return {};
}

} // namespace json

bool parseSetJson(const std::string &text, std::vector<Entry> &out, uint32_t &skipped, std::string &err)
{
    out.clear();
    skipped = 0;
    json::Value root;
    json::Parser parser(text);
    if (!parser.run(root) || root.type != json::Value::Obj)
    {
        err = parser.err.empty() ? "want top-level object" : parser.err;
        return false;
    }
    const json::Value *achs = json::field(root, "Achievements", "achievements");
    if (!achs || (achs->type != json::Value::Arr && achs->type != json::Value::Obj))
    {
        err = "no Achievements array/object";
        return false;
    }
    auto one = [&](const json::Value &a) {
        uint32_t id = 0, flags = 0, points = 0;
        if (!json::asUint(json::field(a, "ID", "id"), id) ||
            !json::asUint(json::field(a, "Flags", "flags"), flags))
        {
            ++skipped;
            return;
        }
        Entry e;
        e.id = id;
        e.points = json::asUint(json::field(a, "Points", "points"), points) ? points : 0u;
        e.title = json::asString(json::field(a, "Title", "title"));
        e.desc = json::asString(json::field(a, "Description", "description"));
        e.memaddr = json::asString(json::field(a, "MemAddr", "memaddr"));
        // Flags 3 (core) only; IDs >= 101000000 are server-injected warnings
        // (101000001 'Warning: Unknown Emulator' is itself Flags 3).
        if (flags != 3u || id >= 101000000u || e.memaddr.empty())
        {
            ++skipped;
            return;
        }
        out.push_back(std::move(e));
    };
    if (achs->type == json::Value::Arr)
    {
        for (const auto &a : achs->arr)
            one(a);
    }
    else
    {
        for (const auto &kv : achs->obj)
            one(kv.second);
    }
    return true;
}

std::string renderStateJson(const UnlockMap &m)
{
    std::string out("{\"unlocked\":{");
    bool first = true;
    for (const auto &[id, ts] : m)
    {
        if (!first)
            out.push_back(',');
        first = false;
        out.push_back('"');
        out += std::to_string(id);
        out += "\":";
        out += std::to_string(ts);
    }
    out += "}}";
    return out;
}

bool parseStateJson(const std::string &text, UnlockMap &out)
{
    out.clear();
    json::Value root;
    json::Parser parser(text);
    if (!parser.run(root) || root.type != json::Value::Obj)
        return false;
    const json::Value *un = json::field(root, "Unlocked", "unlocked");
    if (!un)
        return true; // no unlocks section = empty
    if (un->type != json::Value::Obj)
        return false;
    for (const auto &kv : un->obj)
    {
        char *stop = nullptr;
        unsigned long id = std::strtoul(kv.first.c_str(), &stop, 10);
        uint32_t ts = 0;
        if (!stop || *stop != '\0' || id == 0ul || id > 0xFFFFFFFFul ||
            !json::asUint(&kv.second, ts))
            return false;
        out[static_cast<uint32_t>(id)] = ts;
    }
    return true;
}

bool loadStateFile(const std::string &path, UnlockMap &out)
{
    out.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        return true; // missing = no unlocks yet
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    if (text.empty())
        return true;
    return parseStateJson(text, out);
}

bool saveStateFile(const std::string &path, const UnlockMap &m)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open())
        return false;
    f << renderStateJson(m) << "\n";
    f.flush();
    return static_cast<bool>(f);
}

std::string defaultSetPath(const std::string &mcRoot, const std::string &elfDir)
{
    namespace fs = std::filesystem;
    fs::path dir;
    if (!mcRoot.empty())
        dir = fs::path(mcRoot).parent_path();
    else if (!elfDir.empty())
        dir = fs::path(elfDir);
    else
        dir = fs::current_path();
    return (dir / kSetFileName).string();
}

std::string setPathFromEnv(const std::string &mcRoot, const std::string &elfDir)
{
    const char *v = std::getenv("PS2X_ACH_SET");
    if (v && v[0] != '\0')
        return v;
    return defaultSetPath(mcRoot, elfDir);
}

std::string statePathForSet(const std::string &setPath)
{
    namespace fs = std::filesystem;
    fs::path parent = fs::path(setPath).parent_path();
    if (parent.empty())
        return kStateFileName;
    return (parent / kStateFileName).string();
}

// ---- Runtime --------------------------------------------------------------
namespace
{
rc_runtime_t g_rt;
bool g_rtInit = false;
StockGate g_gate;
MemView g_view;
uint64_t g_tick = 0;
std::vector<Entry> g_entries;
std::map<uint32_t, size_t> g_index;
std::vector<uint32_t> g_pendingDeact;
UnlockMap g_unlocked;
std::string g_statePath;
bool g_initAttempted = false;
bool g_ready = false;
bool g_trickyLogged = false;
bool g_nullMemLogged = false;
uint64_t g_vblanks = 0;
uint64_t g_evals = 0;
uint64_t g_unlocks = 0;
ToastQueue g_toasts; // ACH4: EE thread only (push in eventHandler, pump at VBlank)

uint32_t RC_CCONV readCb(uint32_t address, uint8_t *buffer, uint32_t numBytes, void *ud)
{
    (void)ud;
    return readGuestMemory(address, buffer, numBytes, g_view);
}

void toastUnlock(const Entry &e)
{
    // ACH4: queued, not shown here; pumpToasts() hands them to the shared
    // overlay toast (Android mirrors to the Java Toast) one at a time, so a
    // burst is not collapsed by its latest-wins slot. The [ach] unlocked log
    // line stays as the record.
    g_toasts.push(e.title, e.desc);
}

uint64_t nowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// Once per VBlank (knob on): idle cost is one clock read and two compares
// (the shared toast's mutex is only taken when the next toast is due).
// Another user's live toast (quick save/load) defers the next unlock toast
// until it expires; ours is recognised by its text.
void pumpToasts()
{
    const uint64_t now = nowMs();
    if (!g_toasts.due(now))
        return;
    std::string live;
    const bool otherBusy = ps2x::ui::pollToast(live) && live != g_toasts.lastShown();
    std::string text;
    if (g_toasts.poll(now, otherBusy, text))
        ps2x::ui::toast(text, static_cast<float>(ToastQueue::kShowMs) / 1000.0f);
}

void RC_CCONV eventHandler(const rc_runtime_event_t *e)
{
    if (!e || e->type != RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED)
        return;
    auto it = g_index.find(e->id);
    if (it == g_index.end())
        return;
    const Entry &en = g_entries[it->second];
    if (g_unlocked.count(en.id))
        return; // belt & braces (already-unlocked ids never activate)
    g_unlocked[en.id] = static_cast<uint64_t>(std::time(nullptr));
    ++g_unlocks;
    if (!saveStateFile(g_statePath, g_unlocked))
        std::fprintf(stderr, "[ach] state write failed: %s\n", g_statePath.c_str());
    std::fprintf(stderr, "[ach] unlocked %u \"%s\" tick=%llu\n", en.id, en.title.c_str(),
                 static_cast<unsigned long long>(g_tick));
    toastUnlock(en);
    // Deactivation after the frame (never inside the do_frame iteration).
    g_pendingDeact.push_back(en.id);
}

void logDiscHash()
{
    const char *img = std::getenv("PS2X_CD_IMAGE");
    if (!img || img[0] == '\0')
        return;
    rc_hash_init_custom_filereader(nullptr);
    rc_hash_iterator_t it;
    rc_hash_initialize_iterator(&it, img, nullptr, 0);
    char hash[33] = {};
    const int ok = rc_hash_generate(hash, RC_CONSOLE_PLAYSTATION_2, &it);
    rc_hash_destroy_iterator(&it);
    if (ok)
        std::fprintf(stderr, "[ach] disc hash %s\n", hash);
    else
        std::fprintf(stderr, "[ach] disc hash failed for %s\n", img);
}

void doInit()
{
    const PS2Runtime::IoPaths &io = PS2Runtime::getIoPaths();
    const std::string setPath = setPathFromEnv(io.mcRoot.string(), io.elfDirectory.string());
    g_statePath = statePathForSet(setPath);
    std::ifstream f(setPath, std::ios::binary);
    if (!f.is_open())
    {
        std::fprintf(stderr, "[ach] no set at %s; idle\n", setPath.c_str());
        return;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::vector<Entry> parsed;
    uint32_t skipped = 0;
    std::string err;
    if (!parseSetJson(ss.str(), parsed, skipped, err))
    {
        std::fprintf(stderr, "[ach] set parse failed %s: %s\n", setPath.c_str(), err.c_str());
        return;
    }
    if (!loadStateFile(g_statePath, g_unlocked))
    {
        std::fprintf(stderr, "[ach] state parse failed %s; starting empty\n", g_statePath.c_str());
        g_unlocked.clear();
    }
    rc_runtime_init(&g_rt);
    g_rtInit = true;
    uint32_t already = 0, failed = 0;
    for (const Entry &e : parsed)
    {
        if (g_unlocked.count(e.id))
        {
            ++already;
            continue;
        }
        const int rc = rc_runtime_activate_achievement(&g_rt, e.id, e.memaddr.c_str(), nullptr, 0);
        if (rc != RC_OK)
        {
            ++failed;
            std::fprintf(stderr, "[ach] activate %u failed: %s\n", e.id, rc_error_str(rc));
            continue;
        }
        g_index[e.id] = g_entries.size();
        g_entries.push_back(e);
    }
    std::fprintf(stderr,
                 "[ach] set %s: parsed=%u skipped=%u already=%u failed=%u active=%u state=%s\n",
                 setPath.c_str(), static_cast<unsigned>(parsed.size()), skipped, already, failed,
                 static_cast<unsigned>(g_entries.size()), g_statePath.c_str());
    logDiscHash();
    g_ready = true;
}

} // namespace

void onVBlankTick(uint64_t tick, const uint8_t *rdram, const uint8_t *scratch)
{
    ++g_vblanks;
    if (!knobOn())
        return;
    pumpToasts();
    if (trickyActive())
    {
        if (!g_trickyLogged)
        {
            g_trickyLogged = true;
            std::fprintf(stderr, "[ach] disabled: tricky course active\n");
        }
        return;
    }
    if (!rdram || !scratch)
    {
        if (!g_nullMemLogged)
        {
            g_nullMemLogged = true;
            std::fprintf(stderr, "[ach] no guest memory view; idle\n");
        }
        return;
    }
    if (!g_initAttempted)
    {
        g_initAttempted = true;
        doInit();
    }
    if (!g_ready)
        return;
    const bool useHalf = ps2_fh1::eventsMode();
    const uint32_t div = ps2_fh1::vblankDivisor();
    const uint64_t half = ps2_fh1::g_stockHalfExact.load(std::memory_order_relaxed);
    if (!g_gate.due(frameForTick(tick, div, half, useHalf)))
        return;
    g_view = MemView{rdram, scratch};
    g_tick = tick;
    g_pendingDeact.clear();
    rc_runtime_do_frame(&g_rt, eventHandler, readCb, nullptr, nullptr);
    for (uint32_t id : g_pendingDeact)
        rc_runtime_deactivate_achievement(&g_rt, id);
    if (!g_pendingDeact.empty())
        pumpToasts(); // first unlock of a burst shows this VBlank
    ++g_evals;
    if ((g_evals % 600u) == 0u)
        std::fprintf(stderr, "[ach] status evals=%llu unlocks=%llu active=%u tick=%llu\n",
                     static_cast<unsigned long long>(g_evals),
                     static_cast<unsigned long long>(g_unlocks), activeCount(),
                     static_cast<unsigned long long>(tick));
}

uint64_t evalsTotal() { return g_evals; }
uint64_t vblanksSeen() { return g_vblanks; }
uint64_t unlocksTotal() { return g_unlocks; }
uint32_t activeCount() { return static_cast<uint32_t>(g_entries.size() - g_unlocks); }
size_t toastsPending() { return g_toasts.pending(); }
bool trickyLatched() { return g_trickyLogged; }

void resetForTest()
{
    if (g_rtInit)
    {
        rc_runtime_destroy(&g_rt);
        g_rtInit = false;
    }
    g_gate.reset();
    g_view = MemView{};
    g_tick = 0;
    g_entries.clear();
    g_index.clear();
    g_pendingDeact.clear();
    g_unlocked.clear();
    g_statePath.clear();
    g_initAttempted = false;
    g_ready = false;
    g_trickyLogged = false;
    g_nullMemLogged = false;
    g_vblanks = 0;
    g_evals = 0;
    g_unlocks = 0;
    g_toasts.clear();
}

} // namespace ps2_ach
