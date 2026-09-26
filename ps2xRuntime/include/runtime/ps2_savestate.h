#pragma once

// SS1 (DEV, default off): whole-machine save states at a vsync boundary.
//   PS2X_SAVESTATE_SAVE_AT=<tick> + PS2X_SAVESTATE_PATH=<file> writes one file
//     at the first dispatcher point with m_vsyncTick >= tick where nothing
//     un-capturable is live (deferral reasons are logged).
//   PS2X_SAVESTATE_EXIT_AFTER_SAVE=1 stops the runner after the save.
//   PS2X_SAVESTATE_LOAD=<file> restores it in a fresh process after init.
//   PS2X_SAVESTATE_STRICT=1 also refuses a runner-SHA mismatch (default: warn).
// Requires PS2X_DETERMINISTIC=1. The file holds game RAM: keep it in scratch.
//
// File: "PS2XSAVE" + u32 format version, then sections
//   [u32 keyLen][key][u32 sectionVersion][u64 size][payload].
// Section 0 is "header" (key=value lines). A layout change bumps that
// section's version; the loader refuses unknown, missing or mismatched
// sections instead of reading garbage.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class PS2Runtime;
struct R5900Context;

namespace ps2_savestate
{
    inline constexpr char kMagic[8] = {'P', 'S', '2', 'X', 'S', 'A', 'V', 'E'};
    inline constexpr uint32_t kFormatVersion = 1u;

    class Writer
    {
    public:
        std::vector<uint8_t> buf;

        void bytes(const void *data, size_t size)
        {
            const auto *p = static_cast<const uint8_t *>(data);
            buf.insert(buf.end(), p, p + size);
        }
        template <typename T>
        void pod(const T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "pod() needs a trivially copyable type");
            bytes(&value, sizeof(T));
        }
        void u8(uint8_t v) { pod(v); }
        void u32(uint32_t v) { pod(v); }
        void u64(uint64_t v) { pod(v); }
        void b(bool v) { u8(v ? 1u : 0u); }
        void str(const std::string &s)
        {
            u64(s.size());
            bytes(s.data(), s.size());
        }
        void blob(const std::vector<uint8_t> &v)
        {
            u64(v.size());
            bytes(v.data(), v.size());
        }
        template <typename T>
        void podVec(const std::vector<T> &v)
        {
            static_assert(std::is_trivially_copyable_v<T>, "podVec() needs a trivially copyable type");
            u64(v.size());
            if (!v.empty())
                bytes(v.data(), v.size() * sizeof(T));
        }
        // Returns the offset of the size field; endSection patches it.
        size_t beginSection(const std::string &key, uint32_t version)
        {
            u32(static_cast<uint32_t>(key.size()));
            bytes(key.data(), key.size());
            u32(version);
            const size_t mark = buf.size();
            u64(0u);
            return mark;
        }
        void endSection(size_t mark)
        {
            const uint64_t size = buf.size() - mark - sizeof(uint64_t);
            std::memcpy(buf.data() + mark, &size, sizeof(size));
        }
    };

    class Reader
    {
    public:
        Reader(const uint8_t *data, size_t size) : m_data(data), m_size(size) {}

        bool ok() const { return m_ok; }
        const std::string &error() const { return m_error; }
        bool fail(const std::string &why)
        {
            if (m_ok)
            {
                m_ok = false;
                m_error = why;
            }
            return false;
        }
        size_t pos() const { return m_pos; }
        size_t size() const { return m_size; }
        bool atEnd() const { return m_pos >= m_limit(); }

        bool bytes(void *out, size_t size)
        {
            if (!m_ok)
                return false;
            if (size > m_limit() - m_pos)
                return fail("truncated read");
            std::memcpy(out, m_data + m_pos, size);
            m_pos += size;
            return true;
        }
        template <typename T>
        bool pod(T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "pod() needs a trivially copyable type");
            return bytes(&value, sizeof(T));
        }
        uint8_t u8()
        {
            uint8_t v = 0;
            pod(v);
            return v;
        }
        uint32_t u32()
        {
            uint32_t v = 0;
            pod(v);
            return v;
        }
        uint64_t u64()
        {
            uint64_t v = 0;
            pod(v);
            return v;
        }
        bool b() { return u8() != 0u; }
        // Bounded count for containers (guards against garbage sizes).
        uint64_t count(uint64_t maxCount)
        {
            const uint64_t n = u64();
            if (n > maxCount)
            {
                fail("container count out of range");
                return 0u;
            }
            return n;
        }
        std::string str()
        {
            const uint64_t n = count(m_limit() - m_pos);
            std::string s(static_cast<size_t>(n), '\0');
            if (n)
                bytes(s.data(), static_cast<size_t>(n));
            return s;
        }
        std::vector<uint8_t> blob()
        {
            const uint64_t n = count(m_limit() - m_pos);
            std::vector<uint8_t> v(static_cast<size_t>(n));
            if (n)
                bytes(v.data(), static_cast<size_t>(n));
            return v;
        }
        template <typename T>
        bool podVec(std::vector<T> &v)
        {
            static_assert(std::is_trivially_copyable_v<T>, "podVec() needs a trivially copyable type");
            const uint64_t n = count((m_limit() - m_pos) / sizeof(T));
            v.resize(static_cast<size_t>(n));
            return n == 0u || bytes(v.data(), static_cast<size_t>(n) * sizeof(T));
        }
        // Reads one section frame; the payload is then bounded to it.
        bool beginSection(std::string &key, uint32_t &version)
        {
            m_sectionEnd = 0;
            const uint32_t keyLen = u32();
            if (!m_ok || keyLen > 256u)
                return fail("bad section key");
            key.assign(keyLen, '\0');
            bytes(key.data(), keyLen);
            version = u32();
            const uint64_t size = u64();
            if (!m_ok || size > m_size - m_pos)
                return fail("bad section size for " + key);
            m_sectionEnd = m_pos + static_cast<size_t>(size);
            return true;
        }
        bool endSection(const std::string &key)
        {
            if (!m_ok)
                return false;
            if (m_pos != m_sectionEnd)
                return fail("section " + key + " not fully consumed");
            m_sectionEnd = 0;
            return true;
        }
        void skipSection()
        {
            m_pos = m_sectionEnd;
            m_sectionEnd = 0;
        }

    private:
        size_t m_limit() const { return m_sectionEnd ? m_sectionEnd : m_size; }
        const uint8_t *m_data;
        size_t m_size;
        size_t m_pos = 0;
        size_t m_sectionEnd = 0;
        bool m_ok = true;
        std::string m_error;
    };

    // unordered_map / unordered_set round trip that keeps STL iteration
    // order on the saving host (guest-visible through
    // EeScheduler::acquireInvocationThread): same bucket count, then the
    // saved order inserted in reverse. On both libc++ and libstdc++ a new
    // node goes to the head of its bucket's run and a new bucket's run to
    // the head of the list, so reverse insertion rebuilds the saved order;
    // the restore verifies it key by key. Empty maps skip the bucket
    // requirement (a fresh map's default count differs by STL and carries
    // no state). A cross-STL load still refuses at the first non-empty map.
    template <typename Map, typename WriteEntry>
    void writeOrdered(Writer &w, const Map &map, WriteEntry writeEntry)
    {
        w.u64(map.bucket_count());
        w.u64(map.size());
        for (const auto &entry : map)
            writeEntry(w, entry);
    }

    // Mutable-key entry type used while reading (value_type has a const key).
    template <typename Map, typename = void>
    struct StoredEntry
    {
        using type = typename Map::value_type; // sets
    };
    template <typename Map>
    struct StoredEntry<Map, std::void_t<typename Map::mapped_type>>
    {
        using type = std::pair<typename Map::key_type, typename Map::mapped_type>;
    };

    // Key accessor for the restore's order check (maps compare .first, sets
    // compare the value itself).
    template <typename Map, typename = void>
    struct HasMappedType : std::false_type
    {
    };
    template <typename Map>
    struct HasMappedType<Map, std::void_t<typename Map::mapped_type>> : std::true_type
    {
    };
    template <typename Map, typename Entry>
    const typename Map::key_type &orderedKey(const Entry &e)
    {
        if constexpr (HasMappedType<Map>::value)
            return e.first;
        else
            return e;
    }

    template <typename Map, typename ReadEntry>
    bool readOrdered(Reader &r, Map &map, ReadEntry readEntry)
    {
        using Stored = typename StoredEntry<Map>::type;
        const uint64_t buckets = r.u64();
        const uint64_t n = r.count(1u << 24);
        if (!r.ok() || buckets > (1u << 26)) // 0 = never-used libc++ map
            return r.fail("bad ordered-map header");
        std::vector<Stored> entries;
        entries.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n && r.ok(); ++i)
        {
            Stored e{};
            readEntry(r, e);
            entries.push_back(std::move(e));
        }
        if (!r.ok())
            return false;
        map.clear();
        if (n == 0u)
        {
            // No entries: iteration order is trivial, so the bucket count is
            // not required to reproduce. A fresh map's default differs by STL
            // (libc++ 0, libstdc++ 1) and neither rehash(0) nor rehash(1)
            // reproduces under libstdc++ (both yield 2), so requiring it
            // refuses same-STL Linux states and all cross-STL states. Rehash
            // toward the saved count on a best-effort basis (a used-then-
            // emptied map keeps its count on the same STL); a corrupt huge
            // count is not worth the allocation.
            if (map.bucket_count() != buckets && buckets <= (1u << 20))
                map.rehash(static_cast<size_t>(buckets));
            return true;
        }
        map.rehash(static_cast<size_t>(buckets));
        if (map.bucket_count() != buckets)
            return r.fail("ordered-map bucket count not reproducible");
        // Saved key order, copied before the moves below (entries with
        // string keys are moved-from by the insert loop).
        std::vector<typename Map::key_type> savedKeys;
        savedKeys.reserve(entries.size());
        for (const auto &e : entries)
            savedKeys.push_back(orderedKey<Map>(e));
        for (auto it = entries.rbegin(); it != entries.rend(); ++it)
            map.insert(std::move(*it));
        if (map.bucket_count() != buckets || map.size() != n)
            return r.fail("ordered-map rebuild changed shape");
        size_t i = 0;
        for (const auto &live : map)
        {
            if (i >= savedKeys.size() || orderedKey<Map>(live) != savedKeys[i])
                return r.fail("ordered-map iteration order not reproducible");
            ++i;
        }
        return true;
    }

    // Plain-data helpers for simple maps (key and value trivially copyable).
    template <typename Map>
    void writeOrderedPod(Writer &w, const Map &map)
    {
        writeOrdered(w, map, [](Writer &ww, const auto &e) {
            ww.pod(e.first);
            ww.pod(e.second);
        });
    }
    template <typename Map>
    bool readOrderedPod(Reader &r, Map &map)
    {
        return readOrdered(r, map, [](Reader &rr, auto &e) {
            rr.pod(e.first);
            rr.pod(e.second);
        });
    }

    template <typename T>
    void writeDequePod(Writer &w, const std::deque<T> &d)
    {
        w.u64(d.size());
        for (const T &v : d)
            w.pod(v);
    }
    template <typename T>
    bool readDequePod(Reader &r, std::deque<T> &d)
    {
        d.clear();
        const uint64_t n = r.count(1u << 24);
        for (uint64_t i = 0; i < n && r.ok(); ++i)
        {
            T v{};
            r.pod(v);
            d.push_back(v);
        }
        return r.ok();
    }

    // Stub-side sections (registered from each owning TU at static init;
    // saved after the core sections in key order).
    using SaveFn = void (*)(Writer &);
    using LoadFn = bool (*)(Reader &);
    // Returns an empty string when the owner can be saved now, else why not.
    using ReadyFn = std::string (*)();
    struct SectionHooks
    {
        uint32_t version = 1u;
        SaveFn save = nullptr;
        LoadFn load = nullptr;
        ReadyFn ready = nullptr;
    };
    bool registerSection(const std::string &key, SectionHooks hooks);
    const std::map<std::string, SectionHooks> &registeredSections();

    // Rebuilds an EE wait/resume completion from its tag (EeCompletionTag in
    // ee_scheduler.h). Factories register from the TU that owns the closure.
    using CompletionFactory = std::function<void(R5900Context &)> (*)(const uint32_t args[4], PS2Runtime *runtime);
    bool registerCompletionFactory(uint32_t kind, CompletionFactory factory);
    CompletionFactory completionFactory(uint32_t kind);
    // Tag kinds (stable; they are written into save files).
    inline constexpr uint32_t kCompletionNone = 0u;
    inline constexpr uint32_t kCompletionCdStRead = 1u;

    // Host-state flags a save must refuse on (set by the stubs that touch
    // opaque host state).
    void noteHostRandUsed();
    bool hostRandUsed();

    // Config snapshot taken once at run() start (env + ELF path).
    struct Config
    {
        uint64_t saveAt = 0u;
        std::string savePath;
        std::string loadPath;
        bool exitAfterSave = false;
        bool strict = false;
        bool deterministic = false;
    };
    const Config &config();
    void setElfPath(const std::string &path);

    // Host-time rebase for scheduled-event deadlines (steady_clock ns).
    // Tests pin it (setSteadyNowForTests(0) unpins).
    int64_t steadyNowNs();
    void setSteadyNowForTests(int64_t ns);

    // Entry points (called by EeScheduler on the executor thread).
    // save(): returns true when written; `why` names the deferral otherwise.
    bool trySave(PS2Runtime &runtime, uint64_t vsyncTick, std::string &why);
    bool load(PS2Runtime &runtime, const std::string &path, std::string &error);

    // Directory tree as a section payload (memory-card roots). Restore writes
    // into `root` and refuses when `root` already holds a file that is not
    // byte-identical to the saved one (never overwrites a real card).
    void writeDirTree(Writer &w, const std::string &root);
    bool readDirTree(Reader &r, const std::string &root);

    // Header helpers (exposed for tests).
    std::string sha256Hex(const uint8_t *data, size_t size);
    bool sha256File(const std::string &path, std::string &hex);
    std::string padScriptPrefix(const char *script, uint64_t vsyncTick);
} // namespace ps2_savestate
