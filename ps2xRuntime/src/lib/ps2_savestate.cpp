// SS1 (DEV, default off): save-state file, header, orchestration and the
// PS2Memory / PS2Runtime / VU / GS / SND sections. See ps2_savestate.h.

#include "runtime/ps2_savestate.h"
#include "ps2_mtvu.h"
#include "ps2_savestate_internal.h"

#include "ps2_runtime.h"
#include "ps2_iop_host.h"
#include "ps2_pad_latch.h"
#include "ps2_snd_spike.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

void ps2_savestate_linkSyscallSection(); // Kernel/Syscalls/Savestate.cpp

namespace ps2_savestate
{
    // ---------------------------------------------------------------- registry
    namespace
    {
        std::map<std::string, SectionHooks> &sectionRegistry()
        {
            static std::map<std::string, SectionHooks> registry;
            return registry;
        }
        std::map<uint32_t, CompletionFactory> &completionRegistry()
        {
            static std::map<uint32_t, CompletionFactory> registry;
            return registry;
        }
        std::atomic<bool> g_hostRandUsed{false};
        std::atomic<bool> g_resumeSkip{false};
        std::string g_elfPath;
    } // namespace

    bool registerSection(const std::string &key, SectionHooks hooks)
    {
        return sectionRegistry().emplace(key, hooks).second;
    }
    const std::map<std::string, SectionHooks> &registeredSections() { return sectionRegistry(); }

    bool registerCompletionFactory(uint32_t kind, CompletionFactory factory)
    {
        return completionRegistry().emplace(kind, factory).second;
    }
    CompletionFactory completionFactory(uint32_t kind)
    {
        auto it = completionRegistry().find(kind);
        return it == completionRegistry().end() ? nullptr : it->second;
    }

    void noteHostRandUsed() { g_hostRandUsed.store(true, std::memory_order_relaxed); }
    bool hostRandUsed() { return g_hostRandUsed.load(std::memory_order_relaxed); }
    void setResumeSkip() { g_resumeSkip.store(true, std::memory_order_release); }
    bool takeResumeSkip() { return g_resumeSkip.exchange(false, std::memory_order_acq_rel); }
    void setElfPath(const std::string &path) { g_elfPath = path; }

    const Config &config()
    {
        static const Config cfg = [] {
            ps2_savestate_linkSyscallSection();
            Config c;
            auto env = [](const char *k) -> std::string {
                const char *v = std::getenv(k);
                return v ? std::string(v) : std::string();
            };
            const std::string at = env("PS2X_SAVESTATE_SAVE_AT");
            if (!at.empty())
                c.saveAt = std::strtoull(at.c_str(), nullptr, 10);
            c.savePath = env("PS2X_SAVESTATE_PATH");
            c.loadPath = env("PS2X_SAVESTATE_LOAD");
            c.exitAfterSave = env("PS2X_SAVESTATE_EXIT_AFTER_SAVE") == "1";
            c.strict = env("PS2X_SAVESTATE_STRICT") == "1";
            c.deterministic = env("PS2X_DETERMINISTIC") == "1";
            if (c.saveAt != 0u && c.savePath.empty())
            {
                std::fprintf(stderr, "[savestate] PS2X_SAVESTATE_SAVE_AT without PS2X_SAVESTATE_PATH; save off\n");
                c.saveAt = 0u;
            }
            if ((c.saveAt != 0u || !c.loadPath.empty()) && !c.deterministic)
            {
                std::fprintf(stderr, "[savestate] needs PS2X_DETERMINISTIC=1; save/load off\n");
                c.saveAt = 0u;
                c.loadPath.clear();
            }
            return c;
        }();
        return cfg;
    }

    namespace
    {
        std::atomic<int64_t> g_steadyNowOverride{0};
    }
    void setSteadyNowForTests(int64_t ns) { g_steadyNowOverride.store(ns); }

    int64_t steadyNowNs()
    {
        if (const int64_t pinned = g_steadyNowOverride.load(); pinned != 0)
            return pinned;
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // ---------------------------------------------------------------- SHA-256
    namespace
    {
        struct Sha256
        {
            uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
            uint8_t block[64];
            size_t blockLen = 0;
            uint64_t total = 0;

            static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
            void compress(const uint8_t *p)
            {
                static const uint32_t k[64] = {
                    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
                    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
                    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
                    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
                    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
                    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
                    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
                uint32_t w[64];
                for (int i = 0; i < 16; ++i)
                    w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
                for (int i = 16; i < 64; ++i)
                {
                    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }
                uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
                for (int i = 0; i < 64; ++i)
                {
                    const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                    const uint32_t ch = (e & f) ^ (~e & g);
                    const uint32_t t1 = hh + S1 + ch + k[i] + w[i];
                    const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                    const uint32_t t2 = S0 + maj;
                    hh = g;
                    g = f;
                    f = e;
                    e = d + t1;
                    d = c;
                    c = b;
                    b = a;
                    a = t1 + t2;
                }
                h[0] += a;
                h[1] += b;
                h[2] += c;
                h[3] += d;
                h[4] += e;
                h[5] += f;
                h[6] += g;
                h[7] += hh;
            }
            void update(const uint8_t *p, size_t n)
            {
                total += n;
                while (n)
                {
                    if (blockLen == 0 && n >= 64)
                    {
                        compress(p);
                        p += 64;
                        n -= 64;
                        continue;
                    }
                    const size_t take = std::min(n, 64 - blockLen);
                    std::memcpy(block + blockLen, p, take);
                    blockLen += take;
                    p += take;
                    n -= take;
                    if (blockLen == 64)
                    {
                        compress(block);
                        blockLen = 0;
                    }
                }
            }
            std::string hex()
            {
                const uint64_t bits = total * 8u;
                const uint8_t pad = 0x80u;
                const uint8_t zero = 0u;
                update(&pad, 1);
                while (blockLen != 56)
                    update(&zero, 1);
                uint8_t len[8];
                for (int i = 0; i < 8; ++i)
                    len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
                update(len, 8);
                char out[65];
                for (int i = 0; i < 8; ++i)
                    std::snprintf(out + 8 * i, 9, "%08x", h[i]);
                return std::string(out, 64);
            }
        };

        std::string runnerPath()
        {
#if defined(__APPLE__)
            char buf[4096];
            uint32_t size = sizeof(buf);
            if (_NSGetExecutablePath(buf, &size) == 0)
                return std::string(buf);
            return {};
#else
            std::error_code ec;
            auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
            return ec ? std::string() : p.string();
#endif
        }

        // Cheap ISO identity: size + SHA-256 of the first and last MiB.
        std::string isoIdentity(const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return "missing";
            in.seekg(0, std::ios::end);
            const uint64_t size = static_cast<uint64_t>(in.tellg());
            const uint64_t chunk = std::min<uint64_t>(size, 1u << 20);
            std::vector<uint8_t> buf(static_cast<size_t>(chunk));
            Sha256 sha;
            in.seekg(0);
            in.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(chunk));
            sha.update(buf.data(), buf.size());
            in.seekg(static_cast<std::streamoff>(size - chunk));
            in.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(chunk));
            sha.update(buf.data(), buf.size());
            return std::to_string(size) + ":" + sha.hex();
        }
    } // namespace

    std::string sha256Hex(const uint8_t *data, size_t size)
    {
        Sha256 sha;
        sha.update(data, size);
        return sha.hex();
    }

    bool sha256File(const std::string &path, std::string &hex)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        Sha256 sha;
        std::vector<uint8_t> buf(1u << 20);
        while (in)
        {
            in.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
            const std::streamsize got = in.gcount();
            if (got > 0)
                sha.update(buf.data(), static_cast<size_t>(got));
        }
        hex = sha.hex();
        return true;
    }

    // Entries "t_ms:spec:hold_ms" that start at or before the save tick, in
    // script order (the route the saved machine has already seen). A
    // candidate may change the route after the save tick.
    std::string padScriptPrefix(const char *script, uint64_t vsyncTick)
    {
        if (!script)
            return {};
        const uint64_t nowMs = (vsyncTick * 100000ull) / 5994ull; // Pad.cpp padScriptVsyncTickToMs
        std::string out;
        std::stringstream ss(script);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            if (token.empty())
                continue;
            const uint64_t at = std::strtoull(token.c_str(), nullptr, 10);
            if (at <= nowMs)
            {
                if (!out.empty())
                    out += ',';
                out += token;
            }
        }
        return out;
    }

    // ---------------------------------------------------------------- dir tree
    namespace
    {
        namespace fs = std::filesystem;
        // Nanoseconds since the file-clock epoch (i64 covers +/-292 years;
        // the rep itself is __int128 on Apple libc++, so it is narrowed
        // through duration_cast; no filesystem offers sub-nanoseconds).
        int64_t fileTimeToRep(fs::file_time_type t)
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
        }
        fs::file_time_type repToFileTime(int64_t v)
        {
            return fs::file_time_type(
                std::chrono::duration_cast<fs::file_time_type::duration>(std::chrono::nanoseconds(v)));
        }

        bool validTreeRel(const std::string &rel)
        {
            return !rel.empty() && rel[0] != '/' && rel.find("..") == std::string::npos;
        }
    } // namespace
    void writeDirTree(Writer &w, const std::string &root)
    {
        namespace fs = std::filesystem;
        std::vector<std::pair<std::string, int64_t>> dirs;
        struct SavedFile
        {
            std::string rel;
            int64_t mtime = 0;
            std::vector<uint8_t> bytes;
        };
        std::vector<SavedFile> files;
        std::error_code ec;
        const bool exists = fs::exists(root, ec);
        if (ec)
            throw dir_tree_error("cannot stat " + root + ": " + ec.message());
        if (exists)
        {
            if (fs::symlink_status(root, ec).type() != fs::file_type::directory)
                throw dir_tree_error(root + " is not a directory" + (ec ? ": " + ec.message() : ""));
            for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator();
                 it.increment(ec))
            {
                if (ec)
                    throw dir_tree_error("cannot traverse " + root + ": " + ec.message());
                const fs::file_type ft = it->symlink_status(ec).type();
                if (ec)
                    throw dir_tree_error("cannot stat " + it->path().string() + ": " + ec.message());
                const std::string rel = fs::relative(it->path(), root, ec).generic_string();
                if (ec || rel.empty())
                    throw dir_tree_error("cannot relativize " + it->path().string());
                const fs::file_time_type mtime = fs::last_write_time(it->path(), ec);
                if (ec)
                    throw dir_tree_error("cannot stat time of " + rel + ": " + ec.message());
                if (ft == fs::file_type::directory)
                {
                    dirs.emplace_back(rel, fileTimeToRep(mtime));
                }
                else if (ft == fs::file_type::regular)
                {
                    std::ifstream in(it->path(), std::ios::binary);
                    if (!in)
                        throw dir_tree_error("cannot read " + rel);
                    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    if (in.bad())
                        throw dir_tree_error("cannot read " + rel);
                    files.push_back({rel, fileTimeToRep(mtime), std::move(bytes)});
                }
                else
                {
                    throw dir_tree_error("not a file or directory: " + rel);
                }
            }
            if (ec)
                throw dir_tree_error("cannot traverse " + root + ": " + ec.message());
        }
        std::sort(dirs.begin(), dirs.end());
        std::sort(files.begin(), files.end(),
                  [](const SavedFile &a, const SavedFile &b) { return a.rel < b.rel; });
        w.u64(dirs.size());
        for (const auto &[rel, mtime] : dirs)
        {
            w.str(rel);
            w.pod(mtime);
        }
        w.u64(files.size());
        for (const auto &f : files)
        {
            w.str(f.rel);
            w.pod(f.mtime);
            w.blob(f.bytes);
        }
    }

    bool readDirTree(Reader &r, const std::string &root)
    {
        namespace fs = std::filesystem;
        const uint64_t nDirs = r.count(1u << 16);
        std::vector<std::pair<std::string, int64_t>> dirs;
        for (uint64_t i = 0; i < nDirs && r.ok(); ++i)
        {
            std::string rel = r.str();
            int64_t mtime = 0;
            r.pod(mtime);
            if (!validTreeRel(rel))
                return r.fail("bad path in dir tree: " + rel);
            dirs.emplace_back(std::move(rel), mtime);
        }
        const uint64_t nFiles = r.count(1u << 16);
        struct WantFile
        {
            int64_t mtime = 0;
            std::vector<uint8_t> bytes;
        };
        std::map<std::string, WantFile> want;
        for (uint64_t i = 0; i < nFiles && r.ok(); ++i)
        {
            std::string rel = r.str();
            int64_t mtime = 0;
            r.pod(mtime);
            std::vector<uint8_t> bytes = r.blob();
            if (!validTreeRel(rel))
                return r.fail("bad path in dir tree: " + rel);
            want[std::move(rel)] = WantFile{mtime, std::move(bytes)};
        }
        if (!r.ok())
            return false;
        std::error_code ec;
        const bool rootExists = fs::exists(root, ec);
        if (ec)
            return r.fail("cannot stat " + root + ": " + ec.message());
        if (rootExists && fs::symlink_status(root, ec).type() != fs::file_type::directory)
            return r.fail(root + " is not a directory");
        if (ec)
            return r.fail("cannot stat " + root + ": " + ec.message());
        // Directories the restore itself creates (wanted dirs plus every
        // parent prefix of a wanted path) may already exist; anything else
        // in the destination refuses the load.
        std::set<std::string> okDirs;
        for (const auto &[rel, mtime] : dirs)
        {
            (void)mtime;
            okDirs.insert(rel);
        }
        const auto addParents = [&okDirs](const std::string &rel) {
            std::string prefix;
            for (size_t i = 0; i < rel.size(); ++i)
            {
                if (rel[i] == '/')
                    okDirs.insert(prefix);
                prefix.push_back(rel[i]);
            }
        };
        for (const auto &[rel, mtime] : dirs)
        {
            (void)mtime;
            addParents(rel);
        }
        for (const auto &[rel, f] : want)
        {
            (void)f;
            addParents(rel);
        }
        if (rootExists)
        {
            for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator();
                 it.increment(ec))
            {
                if (ec)
                    return r.fail("cannot traverse " + root + ": " + ec.message());
                const fs::file_type ft = it->symlink_status(ec).type();
                if (ec)
                    return r.fail("cannot stat " + it->path().string() + ": " + ec.message());
                const std::string rel = fs::relative(it->path(), root, ec).generic_string();
                if (ec || rel.empty())
                    return r.fail("cannot relativize " + it->path().string());
                if (ft == fs::file_type::directory)
                {
                    if (!okDirs.count(rel))
                        return r.fail("memory-card dir " + root + " holds an extra directory " + rel +
                                      "; load into an empty card dir");
                }
                else if (ft == fs::file_type::regular)
                {
                    auto found = want.find(rel);
                    if (found == want.end())
                        return r.fail("memory-card dir " + root + " holds an extra file " + rel +
                                      "; load into an empty card dir");
                    std::ifstream in(it->path(), std::ios::binary);
                    if (!in)
                        return r.fail("cannot read " + rel);
                    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    if (in.bad())
                        return r.fail("cannot read " + rel);
                    if (found->second.bytes != bytes)
                        return r.fail("memory-card dir " + root + " holds a different " + rel +
                                      "; load into an empty card dir");
                }
                else
                {
                    return r.fail("memory-card dir " + root + " holds a non-file " + rel +
                                  "; load into an empty card dir");
                }
            }
            if (ec)
                return r.fail("cannot traverse " + root + ": " + ec.message());
        }
        // Restore: files first (identical bytes keep their data; times are
        // restored), then directory times once nothing else is created.
        fs::create_directories(root, ec);
        if (ec)
            return r.fail("cannot create " + root + ": " + ec.message());
        for (const auto &[rel, f] : want)
        {
            const fs::path dst = fs::path(root) / rel;
            fs::create_directories(dst.parent_path(), ec);
            if (ec)
                return r.fail("cannot create " + dst.parent_path().string() + ": " + ec.message());
            const fs::file_time_type wantTime = repToFileTime(f.mtime);
            // (is_regular_file reports missing files via ec, so existence is
            // checked first.)
            const bool dstExists = fs::exists(dst, ec);
            if (ec)
                return r.fail("cannot stat " + rel + ": " + ec.message());
            if (dstExists)
            {
                if (!fs::is_regular_file(dst, ec) || ec)
                    return r.fail("memory-card dir " + root + " holds a non-file " + rel +
                                  "; load into an empty card dir");
                // Validated byte-identical above: never rewritten.
                const fs::file_time_type curTime = fs::last_write_time(dst, ec);
                if (ec)
                    return r.fail("cannot stat time of " + rel + ": " + ec.message());
                if (curTime != wantTime)
                {
                    fs::last_write_time(dst, wantTime, ec);
                    if (ec)
                        return r.fail("cannot set time of " + rel + ": " + ec.message());
                }
            }
            else
            {
                std::ofstream out(dst, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char *>(f.bytes.data()), static_cast<std::streamsize>(f.bytes.size()));
                out.close();
                if (!out)
                    return r.fail("cannot write " + dst.string());
                fs::last_write_time(dst, wantTime, ec);
                if (ec)
                    return r.fail("cannot set time of " + rel + ": " + ec.message());
            }
        }
        for (const auto &[rel, mtime] : dirs)
        {
            const fs::path dst = fs::path(root) / rel;
            fs::create_directories(dst, ec);
            if (ec)
                return r.fail("cannot create " + dst.string() + ": " + ec.message());
        }
        for (const auto &[rel, mtime] : dirs)
        {
            const fs::path dst = fs::path(root) / rel;
            const fs::file_time_type wantTime = repToFileTime(mtime);
            const fs::file_time_type curTime = fs::last_write_time(dst, ec);
            if (ec)
                return r.fail("cannot stat time of " + rel + ": " + ec.message());
            if (curTime != wantTime)
            {
                fs::last_write_time(dst, wantTime, ec);
                if (ec)
                    return r.fail("cannot set time of " + rel + ": " + ec.message());
            }
        }
        return true;
    }

    // ---------------------------------------------------------------- header
    namespace
    {
        struct HeaderLine
        {
            std::string key;
            std::string value;
            bool refuse; // mismatch refuses the load (else warn)
        };

        std::vector<HeaderLine> makeHeader(uint64_t vsyncTick, bool withRunnerSha)
        {
            const char *env = nullptr;
            std::vector<HeaderLine> h;
            h.push_back({"format", std::to_string(kFormatVersion), true});
            std::string runnerSha = "unknown";
            if (withRunnerSha)
                sha256File(runnerPath(), runnerSha);
            h.push_back({"runner_sha", runnerSha, config().strict});
            std::string elfSha = "unknown";
            sha256File(g_elfPath, elfSha);
            h.push_back({"elf_sha", elfSha, true});
            env = std::getenv("PS2X_CD_IMAGE");
            h.push_back({"iso", env ? isoIdentity(env) : std::string("none"), true});
            h.push_back({"deterministic", config().deterministic ? "1" : "0", true});
            env = std::getenv("PS2X_PAD_SCRIPT_CLOCK");
            h.push_back({"pad_clock", env ? env : "", true});
            const std::string prefix = padScriptPrefix(std::getenv("PS2X_PAD_SCRIPT"), vsyncTick);
            h.push_back({"pad_prefix_sha", sha256Hex(reinterpret_cast<const uint8_t *>(prefix.data()), prefix.size()), true});
            env = std::getenv("PS2X_GS_BACKEND");
            h.push_back({"gs_backend", env ? env : "cpu", true});
            env = std::getenv("PS2X_SKIP_MOVIE");
            h.push_back({"skip_movie", env ? env : "", false});
            // Host render settings: SSAA planes are not saved (a load starts
            // them clean), so a different SSAA/hi-res only warns.
            env = std::getenv("PS2X_PGS_SSAA");
            h.push_back({"pgs_ssaa", env ? env : "", false});
            env = std::getenv("PS2X_PGS_HIRES_SCANOUT");
            h.push_back({"pgs_hires", env ? env : "", false});
            return h;
        }

        std::string headerText(const std::vector<HeaderLine> &h, uint64_t tick, uint64_t eeCycle)
        {
            std::string out;
            for (const HeaderLine &l : h)
                out += l.key + "=" + l.value + "\n";
            out += "save_tick=" + std::to_string(tick) + "\n";
            out += "ee_cycle=" + std::to_string(eeCycle) + "\n";
            return out;
        }

        std::map<std::string, std::string> parseHeader(const std::string &text)
        {
            std::map<std::string, std::string> out;
            std::stringstream ss(text);
            std::string line;
            while (std::getline(ss, line))
            {
                const size_t eq = line.find('=');
                if (eq != std::string::npos)
                    out[line.substr(0, eq)] = line.substr(eq + 1);
            }
            return out;
        }
    } // namespace

    // ---------------------------------------------------------------- save
    namespace
    {
        uint64_t g_saveCount = 0;
    }

    bool trySave(PS2Runtime &runtime, uint64_t vsyncTick, std::string &why)
    {
        EeScheduler &sched = runtime.eeScheduler();
        why = EeSchedulerSavestate::ready(sched);
        if (why.empty())
            why = PS2RuntimeSavestate::ready(runtime);
        if (why.empty() && hostRandUsed())
            why = "host std::rand used";
        if (why.empty())
        {
            for (const auto &[key, hooks] : registeredSections())
            {
                if (hooks.ready)
                {
                    why = hooks.ready();
                    if (!why.empty())
                    {
                        why = key + ": " + why;
                        break;
                    }
                }
            }
        }
        if (!why.empty())
            return false;

        // GS last (it drains the worker): frontend + backend on the worker.
        GS &gs = runtime.gs();
        gs.drainQueue();
        Writer gsw;
        std::string gsWhy;
        gs.privWrite([&gs, &gsw, &gsWhy]() {
            gsWhy = GSSavestate::ready(gs);
            if (gsWhy.empty())
                GSSavestate::save(gs, gsw);
        });
        gs.drainQueue();
        if (!gsWhy.empty())
        {
            why = "gs: " + gsWhy;
            return false;
        }

        const auto t0 = std::chrono::steady_clock::now();
        Writer w;
        w.bytes(kMagic, sizeof(kMagic));
        w.u32(kFormatVersion);
        size_t mark = w.beginSection("header", kHeaderVersion);
        w.str(headerText(makeHeader(vsyncTick, true), vsyncTick, sched.currentEeCycle()));
        w.endSection(mark);

        mark = w.beginSection("memory", kMemoryVersion);
        PS2RuntimeSavestate::saveMemory(runtime.memory(), w);
        w.endSection(mark);
        mark = w.beginSection("kernel", kRuntimeVersion);
        PS2RuntimeSavestate::saveKernel(runtime, w);
        w.endSection(mark);
        mark = w.beginSection("scheduler", kSchedulerVersion);
        EeSchedulerSavestate::save(sched, w);
        w.endSection(mark);
        mark = w.beginSection("vu0", kVuVersion);
        VU1InterpreterSavestate::save(runtime.vu0(), w);
        w.endSection(mark);
        mark = w.beginSection("vu1", kVuVersion);
        VU1InterpreterSavestate::save(runtime.vu1(), w);
        w.endSection(mark);
        mark = w.beginSection("gs", kGsVersion);
        w.bytes(gsw.buf.data(), gsw.buf.size());
        w.endSection(mark);
        for (const auto &[key, hooks] : registeredSections())
        {
            mark = w.beginSection(key, hooks.version);
            try
            {
                hooks.save(w);
            }
            catch (const dir_tree_error &e)
            {
                // A card tree that cannot be traversed is never saved
                // partially; the save waits for the next tick like any other
                // deferral. (Only dir_tree_error is caught: anything else is
                // a bug and must stay loud.)
                why = key + ": " + e.what();
                return false;
            }
            w.endSection(mark);
        }

        const std::string &path = config().savePath;
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(w.buf.data()), static_cast<std::streamsize>(w.buf.size()));
            if (!out)
            {
                why = "write failed: " + tmp;
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec)
        {
            why = "rename failed: " + ec.message();
            return false;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++g_saveCount;
        std::fprintf(stderr, "[savestate] saved tick=%" PRIu64 " eeCycle=%" PRIu64 " bytes=%zu sections=%zu ms=%.1f path=%s\n",
                     vsyncTick, sched.currentEeCycle(), w.buf.size(), registeredSections().size() + 7u, ms, path.c_str());
        return true;
    }

    // ---------------------------------------------------------------- load
    bool load(PS2Runtime &runtime, const std::string &path, std::string &error)
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<uint8_t> data;
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                error = "cannot open " + path;
                return false;
            }
            in.seekg(0, std::ios::end);
            data.resize(static_cast<size_t>(in.tellg()));
            in.seekg(0);
            in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        Reader r(data.data(), data.size());
        char magic[8];
        r.bytes(magic, sizeof(magic));
        const uint32_t format = r.u32();
        if (!r.ok() || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
        {
            error = "not a ps2x save state";
            return false;
        }
        if (format != kFormatVersion)
        {
            error = "format version " + std::to_string(format) + " != " + std::to_string(kFormatVersion);
            return false;
        }

        std::string key;
        uint32_t version = 0;
        if (!r.beginSection(key, version) || key != "header" || version != kHeaderVersion)
        {
            error = "missing or mismatched header section";
            return false;
        }
        const std::map<std::string, std::string> saved = parseHeader(r.str());
        r.endSection(key);
        const uint64_t savedTick = std::strtoull(saved.count("save_tick") ? saved.at("save_tick").c_str() : "0", nullptr, 10);
        for (const HeaderLine &line : makeHeader(savedTick, true))
        {
            auto it = saved.find(line.key);
            const std::string have = it == saved.end() ? std::string("<missing>") : it->second;
            if (have == line.value)
                continue;
            if (line.refuse)
            {
                error = "header mismatch " + line.key + ": file=" + have + " this=" + line.value;
                return false;
            }
            std::fprintf(stderr, "[savestate] warning: %s differs (file=%s this=%s); loading anyway\n",
                         line.key.c_str(), have.c_str(), line.value.c_str());
        }

        // Every section this build knows must be present exactly once, at
        // this build's version.
        std::map<std::string, uint32_t> expected = {
            {"memory", kMemoryVersion}, {"kernel", kRuntimeVersion}, {"scheduler", kSchedulerVersion},
            {"vu0", kVuVersion}, {"vu1", kVuVersion}, {"gs", kGsVersion}};
        for (const auto &[k, hooks] : registeredSections())
            expected[k] = hooks.version;

        // Pass 1: validate every section frame before any state is touched,
        // so a refused file leaves the fresh machine as it was.
        {
            Reader scan = r;
            std::map<std::string, bool> seen;
            while (scan.ok() && scan.pos() < scan.size())
            {
                if (!scan.beginSection(key, version))
                    break;
                auto want = expected.find(key);
                if (want == expected.end())
                {
                    error = "unknown section " + key + " (built without its owner?)";
                    return false;
                }
                if (want->second != version)
                {
                    error = "section " + key + " version " + std::to_string(version) + " != " + std::to_string(want->second);
                    return false;
                }
                if (seen[key])
                {
                    error = "duplicate section " + key;
                    return false;
                }
                seen[key] = true;
                scan.skipSection();
            }
            if (!scan.ok())
            {
                error = scan.error();
                return false;
            }
            for (const auto &[k, v] : expected)
            {
                (void)v;
                if (!seen[k])
                {
                    error = "missing section " + k;
                    return false;
                }
            }
        }

        // Pass 2: apply.
        std::map<std::string, bool> loaded;
        while (r.ok() && r.pos() < r.size())
        {
            if (!r.beginSection(key, version))
                break;
            auto want = expected.find(key);
            if (want == expected.end())
            {
                error = "unknown section " + key + " (built without its owner?)";
                return false;
            }
            if (want->second != version)
            {
                error = "section " + key + " version " + std::to_string(version) + " != " + std::to_string(want->second);
                return false;
            }
            if (loaded[key])
            {
                error = "duplicate section " + key;
                return false;
            }
            bool ok = false;
            if (key == "memory")
                ok = PS2RuntimeSavestate::loadMemory(runtime.memory(), r);
            else if (key == "kernel")
                ok = PS2RuntimeSavestate::loadKernel(runtime, r);
            else if (key == "scheduler")
                ok = EeSchedulerSavestate::load(runtime.eeScheduler(), r, runtime);
            else if (key == "vu0")
                ok = VU1InterpreterSavestate::load(runtime.vu0(), r);
            else if (key == "vu1")
                ok = VU1InterpreterSavestate::load(runtime.vu1(), r);
            else if (key == "gs")
            {
                GS &gs = runtime.gs();
                gs.drainQueue();
                gs.privWrite([&gs, &r, &ok]() { ok = GSSavestate::load(gs, r); });
                gs.drainQueue();
            }
            else
                ok = registeredSections().at(key).load(r);
            if (!ok || !r.endSection(key))
            {
                error = "section " + key + ": " + (r.ok() ? std::string("load failed") : r.error());
                return false;
            }
            loaded[key] = true;
        }
        if (!r.ok())
        {
            error = r.error();
            return false;
        }
        for (const auto &[k, v] : expected)
        {
            (void)v;
            if (!loaded[k])
            {
                error = "missing section " + k;
                return false;
            }
        }
        setResumeSkip();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "[savestate] loaded tick=%" PRIu64 " eeCycle=%s bytes=%zu sections=%zu ms=%.1f path=%s\n",
                     savedTick, saved.count("ee_cycle") ? saved.at("ee_cycle").c_str() : "?", data.size(), loaded.size(), ms,
                     path.c_str());
        return true;
    }
} // namespace ps2_savestate

using namespace ps2_savestate;

// ==================================================================== memory
std::string PS2RuntimeSavestate::ready(const PS2Runtime &rt)
{
    const PS2Memory &m = rt.m_memory;
    if (!m.m_pendingGifTransfers.empty() || !m.m_pendingVif0Transfers.empty() || !m.m_pendingVif1Transfers.empty())
        return "pending GIF/VIF transfers";
    if (!m.m_completedDmacCauses.empty())
        return "completed DMAC causes not drained";
    if (!rt.m_gifArbiter.empty())
        return "GIF arbiter queue not empty";
    if (rt.m_iopHost && rt.m_iopHost->savestateOpenHostFiles() != 0u)
        return "IOP host files open";
    return {};
}

void PS2RuntimeSavestate::saveMemory(const PS2Memory &m, Writer &w)
{
    ps2_mtvu::sync(ps2_mtvu::Reason::SaveState); // MT1: no unit job spans a state
    w.bytes(m.m_rdram, PS2_RAM_SIZE);
    w.bytes(m.m_scratchpad, PS2_SCRATCHPAD_SIZE);
    w.bytes(m.iop_ram, 2u * 1024u * 1024u);
    w.bytes(m.m_gsVRAM, PS2_GS_VRAM_SIZE);
    w.bytes(m.m_vu0Code, PS2_VU0_CODE_SIZE);
    w.bytes(m.m_vu0Data, PS2_VU0_DATA_SIZE);
    w.bytes(m.m_vu1Code, PS2_VU1_CODE_SIZE);
    w.bytes(m.m_vu1Data, PS2_VU1_DATA_SIZE);
    w.b(m.m_seenGifCopy);
    writeOrderedPod(w, m.m_ioRegisters);
    const GSRegisters &g = m.gs_regs;
    for (uint64_t v : {g.pmode, g.smode1, g.smode2, g.srfsh, g.synch1, g.synch2, g.syncv, g.dispfb1, g.display1,
                       g.dispfb2, g.display2, g.extbuf, g.extdata, g.extwrite, g.bgcolor})
        w.u64(v);
    w.u64(g.csr.load());
    w.u64(g.vsyncTick.load());
    w.u64(g.imr);
    w.u64(g.busdir);
    w.u64(g.siglblid.load());
    w.pod(m.vif0_regs);
    w.pod(m.vif1_regs);
    w.pod(m.dma_regs);
    w.u64(m.m_tlbEntries.size());
    for (const auto &e : m.m_tlbEntries)
    {
        w.u32(e.vpn);
        w.u32(e.pfn);
        w.u32(e.mask);
        w.b(e.valid);
    }
    w.b(m.m_path3Masked);
    w.u32(m.m_vif1PendingPath2ImageQwc);
    w.b(m.m_vif1PendingPath2DirectHl);
    w.u64(m.m_path3MaskedFifo.size());
    for (const auto &pkt : m.m_path3MaskedFifo)
        w.blob(pkt);
    w.u64(m.m_codeRegions.size());
    for (const auto &region : m.m_codeRegions)
    {
        w.u32(region.start);
        w.u32(region.end);
        w.u64(region.modified.size());
        for (bool bit : region.modified)
            w.b(bit);
    }
    for (const auto &t : m.m_eeTimers)
    {
        w.u32(t.count);
        w.u32(t.mode);
        w.u32(t.compare);
        w.u32(t.hold);
        w.u64(t.clockRemainder);
    }
}

bool PS2RuntimeSavestate::loadMemory(PS2Memory &m, Reader &r)
{
    ps2_mtvu::sync(ps2_mtvu::Reason::SaveState); // MT1: no unit job spans a state
    r.bytes(m.m_rdram, PS2_RAM_SIZE);
    r.bytes(m.m_scratchpad, PS2_SCRATCHPAD_SIZE);
    r.bytes(m.iop_ram, 2u * 1024u * 1024u);
    r.bytes(m.m_gsVRAM, PS2_GS_VRAM_SIZE);
    r.bytes(m.m_vu0Code, PS2_VU0_CODE_SIZE);
    r.bytes(m.m_vu0Data, PS2_VU0_DATA_SIZE);
    r.bytes(m.m_vu1Code, PS2_VU1_CODE_SIZE);
    r.bytes(m.m_vu1Data, PS2_VU1_DATA_SIZE);
    m.m_seenGifCopy = r.b();
    readOrderedPod(r, m.m_ioRegisters);
    GSRegisters &g = m.gs_regs;
    for (uint64_t *v : {&g.pmode, &g.smode1, &g.smode2, &g.srfsh, &g.synch1, &g.synch2, &g.syncv, &g.dispfb1,
                        &g.display1, &g.dispfb2, &g.display2, &g.extbuf, &g.extdata, &g.extwrite, &g.bgcolor})
        *v = r.u64();
    g.csr.store(r.u64());
    g.vsyncTick.store(r.u64());
    g.imr = r.u64();
    g.busdir = r.u64();
    g.siglblid.store(r.u64());
    r.pod(m.vif0_regs);
    r.pod(m.vif1_regs);
    r.pod(m.dma_regs);
    const uint64_t tlb = r.count(256);
    m.m_tlbEntries.assign(static_cast<size_t>(tlb), PS2Memory::TLBEntry{0, 0, 0, false});
    for (auto &e : m.m_tlbEntries)
    {
        e.vpn = r.u32();
        e.pfn = r.u32();
        e.mask = r.u32();
        e.valid = r.b();
    }
    m.m_path3Masked = r.b();
    m.m_vif1PendingPath2ImageQwc = r.u32();
    m.m_vif1PendingPath2DirectHl = r.b();
    m.m_path3MaskedFifo.resize(static_cast<size_t>(r.count(1u << 20)));
    for (auto &pkt : m.m_path3MaskedFifo)
        pkt = r.blob();
    m.m_codeRegions.resize(static_cast<size_t>(r.count(1u << 20)));
    for (auto &region : m.m_codeRegions)
    {
        region.start = r.u32();
        region.end = r.u32();
        region.modified.assign(static_cast<size_t>(r.count(1u << 26)), false);
        for (size_t i = 0; i < region.modified.size(); ++i)
            region.modified[i] = r.b();
    }
    for (auto &t : m.m_eeTimers)
    {
        t.count = r.u32();
        t.mode = r.u32();
        t.compare = r.u32();
        t.hold = r.u32();
        t.clockRemainder = r.u64();
    }
    m.m_pendingGifTransfers.clear();
    m.m_pendingVif0Transfers.clear();
    m.m_pendingVif1Transfers.clear();
    m.m_completedDmacCauses.clear();
    // VU decode/recomp caches key on the code generation.
    m.markVU0CodeModified();
    m.markVU1CodeModified();
    return r.ok();
}

// ==================================================================== kernel
void PS2RuntimeSavestate::saveKernel(const PS2Runtime &rt, Writer &w)
{
    w.pod(rt.m_cpuContext);
    writeOrdered(w, rt.m_eeExitHandlers, [](Writer &ww, const auto &e) {
        ww.pod(e.first);
        ww.u64(e.second.size());
        for (const auto &h : e.second)
        {
            ww.u32(h.function);
            ww.u32(h.argument);
        }
    });
    writeOrderedPod(w, rt.m_eeSyscallOverrides);
    writeOrdered(w, rt.m_eeSyscallMirrorAddresses, [](Writer &ww, uint32_t v) { ww.u32(v); });
    w.u64(rt.m_guestHeapBlocks.size());
    for (const auto &b : rt.m_guestHeapBlocks)
    {
        w.u32(b.addr);
        w.u32(b.size);
        w.b(b.free);
    }
    w.u32(rt.m_guestHeapBase);
    w.u32(rt.m_guestHeapEnd);
    w.u32(rt.m_guestHeapLimit);
    w.u32(rt.m_guestHeapSuggestedBase);
    w.b(rt.m_guestHeapConfigured);
    w.u32(rt.m_asyncCallbackStackFloor);
    w.u32(rt.m_asyncCallbackStackTop);
    w.u64(rt.m_loadedModules.size());
    for (const auto &mod : rt.m_loadedModules)
    {
        w.str(mod.name);
        w.u32(mod.baseAddress);
        w.u64(mod.size);
        w.b(mod.active);
    }
    uint64_t nextToken = 0u, nextHandle = 0u;
    if (rt.m_iopHost)
        rt.m_iopHost->savestateCounters(nextToken, nextHandle);
    w.b(static_cast<bool>(rt.m_iopHost));
    w.u64(nextToken);
    w.u64(nextHandle);
}

bool PS2RuntimeSavestate::loadKernel(PS2Runtime &rt, Reader &r)
{
    r.pod(rt.m_cpuContext);
    readOrdered(r, rt.m_eeExitHandlers, [](Reader &rr, auto &e) {
        rr.pod(e.first);
        e.second.resize(static_cast<size_t>(rr.count(1u << 16)));
        for (auto &h : e.second)
        {
            h.function = rr.u32();
            h.argument = rr.u32();
        }
    });
    readOrderedPod(r, rt.m_eeSyscallOverrides);
    readOrdered(r, rt.m_eeSyscallMirrorAddresses, [](Reader &rr, uint32_t &v) { v = rr.u32(); });
    rt.m_guestHeapBlocks.resize(static_cast<size_t>(r.count(1u << 24)));
    for (auto &b : rt.m_guestHeapBlocks)
    {
        b.addr = r.u32();
        b.size = r.u32();
        b.free = r.b();
    }
    rt.m_guestHeapBase = r.u32();
    rt.m_guestHeapEnd = r.u32();
    rt.m_guestHeapLimit = r.u32();
    rt.m_guestHeapSuggestedBase = r.u32();
    rt.m_guestHeapConfigured = r.b();
    rt.m_asyncCallbackStackFloor = r.u32();
    rt.m_asyncCallbackStackTop = r.u32();
    rt.m_loadedModules.resize(static_cast<size_t>(r.count(1u << 16)));
    for (auto &mod : rt.m_loadedModules)
    {
        mod.name = r.str();
        mod.baseAddress = r.u32();
        mod.size = static_cast<size_t>(r.u64());
        mod.active = r.b();
    }
    const bool hadIopHost = r.b();
    const uint64_t nextToken = r.u64();
    const uint64_t nextHandle = r.u64();
    if (hadIopHost != static_cast<bool>(rt.m_iopHost))
        return r.fail("IOP host adapter presence differs");
    if (rt.m_iopHost)
        rt.m_iopHost->savestateSetCounters(nextToken, nextHandle);
    return r.ok();
}

// ==================================================================== VU
void VU1InterpreterSavestate::save(const VU1Interpreter &vu, Writer &w)
{
    w.pod(vu.m_state);
#if PS2X_ENABLE_DET_HASH_TAP
    w.u64(vu.m_programStartCount);
#else
    w.u64(0u);
#endif
    w.pod(vu.m_flagPipeline);
    w.pod(vu.m_fdiv);
    w.pod(vu.m_efu);
    w.pod(vu.m_storePipeline);
    w.pod(vu.m_vfWritePipeline);
    w.pod(vu.m_viWritePipeline);
    w.pod(vu.m_accWritePipeline);
    w.pod(vu.m_xgkick);
    w.pod(vu.m_vfReady);
    w.pod(vu.m_viReady);
    w.pod(vu.m_accReady);
    for (uint32_t mask : {vu.m_flagValidMask, vu.m_storeValidMask, vu.m_vfWriteValidMask, vu.m_viWriteValidMask,
                          vu.m_accWriteValidMask})
        w.u32(mask);
    w.u64(vu.m_nextCommitCycle);
    w.pod(vu.m_vfLatestWrite);
    w.pod(vu.m_viLatestWrite);
    w.pod(vu.m_accLatestWrite);
    w.u64(vu.m_cycle);
    w.u64(vu.m_nextWriteSequence);
    w.u64(vu.m_efuResourceReady);
    w.u32(vu.m_workingClip);
    w.u32(vu.m_currentUpperInstruction);
    w.pod(vu.m_viBranchBackupValue);
    w.u8(vu.m_viBranchBackupReg);
    w.b(vu.m_viBranchBackupValid);
    w.b(vu.m_stopRequested);
    w.b(vu.m_pendingHaltD);
    w.b(vu.m_pendingHaltT);
    w.b(vu.m_directStores);
    w.b(vu.m_directFlags);
    w.u64(vu.m_directPendingUntil);
    w.pod(vu.m_storeScratch);
}

bool VU1InterpreterSavestate::load(VU1Interpreter &vu, Reader &r)
{
    r.pod(vu.m_state);
#if PS2X_ENABLE_DET_HASH_TAP
    vu.m_programStartCount = r.u64();
#else
    (void)r.u64();
#endif
    r.pod(vu.m_flagPipeline);
    r.pod(vu.m_fdiv);
    r.pod(vu.m_efu);
    r.pod(vu.m_storePipeline);
    r.pod(vu.m_vfWritePipeline);
    r.pod(vu.m_viWritePipeline);
    r.pod(vu.m_accWritePipeline);
    r.pod(vu.m_xgkick);
    r.pod(vu.m_vfReady);
    r.pod(vu.m_viReady);
    r.pod(vu.m_accReady);
    for (uint32_t *mask : {&vu.m_flagValidMask, &vu.m_storeValidMask, &vu.m_vfWriteValidMask, &vu.m_viWriteValidMask,
                           &vu.m_accWriteValidMask})
        *mask = r.u32();
    vu.m_nextCommitCycle = r.u64();
    r.pod(vu.m_vfLatestWrite);
    r.pod(vu.m_viLatestWrite);
    r.pod(vu.m_accLatestWrite);
    vu.m_cycle = r.u64();
    vu.m_nextWriteSequence = r.u64();
    vu.m_efuResourceReady = r.u64();
    vu.m_workingClip = r.u32();
    vu.m_currentUpperInstruction = r.u32();
    r.pod(vu.m_viBranchBackupValue);
    vu.m_viBranchBackupReg = r.u8();
    vu.m_viBranchBackupValid = r.b();
    vu.m_stopRequested = r.b();
    vu.m_pendingHaltD = r.b();
    vu.m_pendingHaltT = r.b();
    vu.m_directStores = r.b();
    vu.m_directFlags = r.b();
    vu.m_directPendingUntil = r.u64();
    r.pod(vu.m_storeScratch);
    // Caches keyed by code generation / content (markVU*CodeModified bumps
    // the generation in the memory section).
    vu.m_decodedCodeCacheValid = false;
    vu.m_cachedVuCode = nullptr;
    vu.m_cachedMemory = nullptr;
    vu.m_recompValid = false;
    vu.m_recompProgram = nullptr;
    vu.m_recompCode = nullptr;
    return r.ok();
}

// ==================================================================== GS
std::string GSSavestate::ready(const GS &gs)
{
    // The frontend vertex queue and (SS3) paraLLEl's retained strip/fan
    // vertices are saved; an in-flight host->local transfer is not, so the
    // save defers while one is live.
    if (gs.m_backend && !gs.m_backend->SavestateIdle())
    {
        const std::string reason = gs.m_backend->SavestateBusyReason();
        return reason.empty() ? "GS backend transfer active" : reason;
    }
    return {};
}

void GSSavestate::save(GS &gs, Writer &w)
{
    ps2_mtvu::sync(ps2_mtvu::Reason::SaveState); // MT1: no unit job spans a state
    std::lock_guard<std::recursive_mutex> lock(gs.m_stateMutex);
    w.pod(gs.m_ctx);
    w.pod(gs.m_prim);
    w.pod(gs.m_curGifPath);
    w.pod(gs.m_primRegister);
    w.pod(gs.m_prmodeRegister);
    for (uint8_t v : {gs.m_curR, gs.m_curG, gs.m_curB, gs.m_curA})
        w.u8(v);
    w.pod(gs.m_curQ);
    w.pod(gs.m_curS);
    w.pod(gs.m_curT);
    w.pod(gs.m_curU);
    w.pod(gs.m_curV);
    for (uint8_t v : {gs.m_curFog, gs.m_fogR, gs.m_fogG, gs.m_fogB})
        w.u8(v);
    w.b(gs.m_prmodecont);
    w.b(gs.m_pabe);
    for (uint64_t v : {gs.m_scanmsk, gs.m_dimx, gs.m_dthe, gs.m_colclamp})
        w.u64(v);
    w.pod(gs.m_texa);
    w.pod(gs.m_texclut);
    w.pod(gs.m_bitbltbuf);
    w.pod(gs.m_trxpos);
    w.pod(gs.m_trxreg);
    w.u32(gs.m_trxdir);
    w.pod(gs.m_vtxQueue);
    w.pod(gs.m_vtxCount);
    w.pod(gs.m_vtxIndex);
    w.pod(gs.m_preferredDisplaySourceFrame);
    w.u32(gs.m_preferredDisplayDestFbp);
    w.b(gs.m_hasPreferredDisplaySource);
    std::vector<uint8_t> backendState;
    if (gs.m_backend)
        gs.m_backend->SavestateSave(backendState);
    w.blob(backendState);
}

bool GSSavestate::load(GS &gs, Reader &r)
{
    ps2_mtvu::sync(ps2_mtvu::Reason::SaveState); // MT1: no unit job spans a state
    std::lock_guard<std::recursive_mutex> lock(gs.m_stateMutex);
    r.pod(gs.m_ctx);
    r.pod(gs.m_prim);
    r.pod(gs.m_curGifPath);
    r.pod(gs.m_primRegister);
    r.pod(gs.m_prmodeRegister);
    for (uint8_t *v : {&gs.m_curR, &gs.m_curG, &gs.m_curB, &gs.m_curA})
        *v = r.u8();
    r.pod(gs.m_curQ);
    r.pod(gs.m_curS);
    r.pod(gs.m_curT);
    r.pod(gs.m_curU);
    r.pod(gs.m_curV);
    for (uint8_t *v : {&gs.m_curFog, &gs.m_fogR, &gs.m_fogG, &gs.m_fogB})
        *v = r.u8();
    gs.m_prmodecont = r.b();
    gs.m_pabe = r.b();
    for (uint64_t *v : {&gs.m_scanmsk, &gs.m_dimx, &gs.m_dthe, &gs.m_colclamp})
        *v = r.u64();
    r.pod(gs.m_texa);
    r.pod(gs.m_texclut);
    r.pod(gs.m_bitbltbuf);
    r.pod(gs.m_trxpos);
    r.pod(gs.m_trxreg);
    gs.m_trxdir = r.u32();
    r.pod(gs.m_vtxQueue);
    r.pod(gs.m_vtxCount);
    r.pod(gs.m_vtxIndex);
    r.pod(gs.m_preferredDisplaySourceFrame);
    gs.m_preferredDisplayDestFbp = r.u32();
    gs.m_hasPreferredDisplaySource = r.b();
    const std::vector<uint8_t> backendState = r.blob();
    if (!r.ok())
        return false;
    if (gs.m_backend && !gs.m_backend->SavestateLoad(backendState.data(), backendState.size()))
        return r.fail("GS backend refused its state");
    return true;
}

// ==================================================================== SND
struct SndSavestate
{
    static void save(ps2_savestate::Writer &w)
    {
        ps2_snd_spike::State &s = ps2_snd_spike::state();
        std::lock_guard<std::mutex> lock(s.mutex);
        for (uint32_t v : {s.handler, s.handlerData, s.handlerGp, s.statusAddr, s.serial})
            w.u32(v);
        for (uint64_t v : {s.ticks, s.cid0, s.dmq, s.done, s.setdma, s.tagbufs})
            w.u64(v);
        w.u32(s.doneRing);
        w.u64(s.iopMem.size());
        for (const auto &[addr, bytes] : s.iopMem)
        {
            w.u32(addr);
            w.blob(bytes);
        }
        w.blob(s.spu.m_ram);
        w.pod(s.spu.m_voices);
        w.u64(s.spu.m_uploads);
        w.u64(s.spu.m_uploadBytes);
        w.pod(s.driver.m_word);
        w.pod(s.driver.m_statusWord);
        w.pod(s.driver.m_statusNax);
        w.pod(s.driver.m_statusAddr);
        w.pod(s.driver.m_statusTriggered);
        w.pod(s.driver.m_pitch);
        w.pod(s.driver.m_vol);
        w.u64(s.driver.m_keyOns);
        w.pod(s.upLeft.m_history);
        w.pod(s.upRight.m_history);
    }
    static bool load(ps2_savestate::Reader &r)
    {
        ps2_snd_spike::State &s = ps2_snd_spike::state();
        std::lock_guard<std::mutex> lock(s.mutex);
        for (uint32_t *v : {&s.handler, &s.handlerData, &s.handlerGp, &s.statusAddr, &s.serial})
            *v = r.u32();
        for (uint64_t *v : {&s.ticks, &s.cid0, &s.dmq, &s.done, &s.setdma, &s.tagbufs})
            *v = r.u64();
        s.doneRing = r.u32();
        s.iopMem.clear();
        const uint64_t n = r.count(1u << 20);
        for (uint64_t i = 0; i < n && r.ok(); ++i)
        {
            const uint32_t addr = r.u32();
            s.iopMem[addr] = r.blob();
        }
        std::vector<uint8_t> ram = r.blob();
        if (ram.size() != s.spu.m_ram.size())
            return r.fail("SPU RAM size mismatch");
        s.spu.m_ram = std::move(ram);
        r.pod(s.spu.m_voices);
        s.spu.m_uploads = r.u64();
        s.spu.m_uploadBytes = r.u64();
        r.pod(s.driver.m_word);
        r.pod(s.driver.m_statusWord);
        r.pod(s.driver.m_statusNax);
        r.pod(s.driver.m_statusAddr);
        r.pod(s.driver.m_statusTriggered);
        r.pod(s.driver.m_pitch);
        r.pod(s.driver.m_vol);
        s.driver.m_keyOns = r.u64();
        r.pod(s.upLeft.m_history);
        r.pod(s.upRight.m_history);
        return r.ok();
    }
};

namespace
{
    void padLatchSave(ps2_savestate::Writer &w) { w.pod(ps2x::padlatch::sharedLatch().savestateGet()); }
    bool padLatchLoad(ps2_savestate::Reader &r)
    {
        ps2x::padlatch::Latch latch{};
        if (!r.pod(latch))
            return false;
        ps2x::padlatch::sharedLatch().savestateSet(latch);
        return true;
    }
    std::string padLatchReady()
    {
        const ps2x::padlatch::Latch l = ps2x::padlatch::sharedLatch().savestateGet();
        // Host input would leak into a restored run; save with the pad idle.
        if (l.live || l.pendDown || l.pendUp || l.owedDown || l.owedUp)
            return "host pad input pending";
        return {};
    }

    const bool kSndRegistered = ps2_savestate::registerSection(
        "snd", {ps2_savestate::kSndVersion, &SndSavestate::save, &SndSavestate::load, nullptr});
    const bool kPadLatchRegistered =
        ps2_savestate::registerSection("padlatch", {1u, &padLatchSave, &padLatchLoad, &padLatchReady});
} // namespace
