// ONL3: host-socket network layer for SSX 3's EA DirtySock stack.
//
// Protocol facts (from DRTYSCKF.IRX disassembly + EE codegen):
// - EE->IOP requests are SIFCMD cid=1 (DirtySock registers RpcWakeup for
//   cid 1 via sceSifAddCmdHandler; it registers no SIF RPC server).
// - The version poll sends a 16-byte packet {psize=0x10, 0, 0,
//   eeAddr|1}; the IOP DMAs 16 bytes ("RPC IOP Ver 1.0 ") to eeAddr.
//   EE helper: sub_003F4F40; IOP side: RpcWakeup -> _TransferData.
// - Bulk requests go through SifSetDma + cid=1 + sema waits (EE helper:
//   sub_003F4528). Formats are decoded incrementally as the game proceeds.
#include "ps2_net_dirtysock.h"

#include "game_overrides.h"
#include "ps2_e41_trace.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <map>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <string>

#if defined(__APPLE__) || defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#define PS2_NET_HAVE_POSIX_SOCKETS 1
#else
#define PS2_NET_HAVE_POSIX_SOCKETS 0
#endif

#if defined(__ANDROID__)
#include "ps2_android_env.h" // ONL11: files-dir login path accessor
#endif

namespace ps2_net
{
    uint32_t readEeU32(const uint8_t *rdram, uint32_t addr);

    namespace
    {
        // "RPC IOP Ver 1.0 " (DRTYSCKF.IRX .data+0x70, the RpcWakeup DMA
        // source; verified bytes, not a guess).
        constexpr char kIopVersion[16] = {
            'R', 'P', 'C', ' ', 'I', 'O', 'P', ' ',
            'V', 'e', 'r', ' ', '1', '.', '0', ' '};

        constexpr uint32_t kDirtySockCid = 1u;
        // SLUS_207.72 sites (ONL1 §3; fileoff+0xFF000=vaddr).
        constexpr uint32_t kServerStringAddr = 0x4807D8u;
        constexpr char kServerStringOrig[] = "ps2ssx04.ea.com";
        constexpr uint32_t kDnasDispatchAddr = 0x3FD100u;

        std::mutex g_captureMutex;
        std::string g_capturePath;
        bool g_captureResolved = false;

        const std::string &capturePath()
        {
            std::lock_guard<std::mutex> lock(g_captureMutex);
            if (!g_captureResolved)
            {
                if (const char *env = std::getenv("PS2X_NET_CAPTURE"))
                {
                    g_capturePath = env;
                }
                g_captureResolved = true;
            }
            return g_capturePath;
        }

        bool g_patchesApplied = false;

        // Link state: the interface reports down until netw/star runs,
        // then up (matches the RpcNetworkStatus [0x1FE60] lifecycle).
        bool g_linkUp = false;
        // DNS state: name/look resolves instantly; name/done reports
        // complete once a lookup has been issued.
        bool g_dnsResolved = false;
        // Unified session-handle namespace (all */crea methods; the EE
        // lib passes status back as the handle, e.g. sess/disc(a0=1)).
        uint32_t g_nextHandle = 0u;
        // Host TCP sockets by session handle (sess/conn fills, sess/disc
        // drains). Guarded; the EE thread is the only writer today.
        std::mutex g_sockMutex;
        std::map<uint32_t, int> g_sockets;
        // recv lag (v8.3): loopback is faster than the app (data
        // consumed before the alarm is set, leaving it stale and
        // the pump skipping the wake). Hold first-seen data until
        // readyTick (firstTick + kRecvLagTicks), then deliver.
        // p2am probe: variable per-call lag (race simulation). The
        // guest races app-vs-IOP; fixed lag always hangs (hang =
        // data+1 invariant), so vary the hold per delivery.
        uint32_t recvLagFor(uint32_t handle, uint32_t tick)
        {
            return (handle * 7u + tick * 13u) % 17u;
        }
        // SEC1: bound per-handle held bytes. The hold accumulates socket
        // bytes across polls; a malicious server could otherwise grow it
        // without bound by streaming. Legit traffic is one <=64 KB
        // message per delivery and drains every poll, so this never
        // triggers legitimately. When full we stop reading and let TCP
        // backpressure hold the sender instead of dropping stream bytes.
        constexpr size_t kRecvHoldCap = 256u * 1024u;
        std::map<uint32_t, std::vector<uint8_t>> g_recvHold;
        std::atomic<bool> g_lockstepLink{false};  // any gmlk link up (ONL5-P3)
        std::map<uint32_t, uint32_t> g_recvReady;

        struct PingReq
        {
            std::vector<uint8_t> payload;
            uint64_t tick = 0u;
            uint32_t target = 0u;
        };
        std::map<uint32_t, PingReq> g_pingReq; // handle -> outstanding requ
        // ONL4-P2: newest sess/conn handle (the game-port session the
        // synthetic auth goes on) + the handle it was sent on.
        uint32_t g_loginConn = 0u;
        uint32_t g_loginSentConn = 0u;
        bool g_gameAuthSeen = false;
        uint64_t g_submitTick = 0u;
        // ONL10: navigator milestones. First-observed guest tick per
        // event (0 = unseen); set only when the NAV knob wants them
        // (qpNavNote gate below), so the knob-off path keeps one
        // cached branch per handler site and zero guest-visible change.
        std::atomic<uint64_t> g_qpNetwCrea{0u};
        // ONL11: Online-entry count (incremented per netw/crea call, only
        // behind the qpNavNote gate). The online arming mode watches for
        // advances: each new entry re-arms the navigator after a back-out.
        std::atomic<uint64_t> g_qpNetwCreaCount{0u};
        std::atomic<uint64_t> g_qpNetwList{0u};
        std::atomic<uint64_t> g_qpNetwStar{0u};
        std::atomic<uint64_t> g_qpSessConnOk{0u};
        std::atomic<uint32_t> g_qpSessConnPort{0u};
        std::atomic<uint64_t> g_qpSessConnFail{0u};
        std::atomic<uint64_t> g_qpGameAuth{0u};
        std::atomic<uint64_t> g_qpPers{0u};
        std::atomic<uint64_t> g_qpOnln{0u};
        std::atomic<uint64_t> g_qpNews{0u};
        std::atomic<uint64_t> g_qpPset{0u};
        std::atomic<uint64_t> g_qpRget{0u};
        std::atomic<uint64_t> g_qpQuik{0u};
        std::atomic<uint64_t> g_qpQuikCount{0u};
        // ONL11 addendum: first sess/disc tick (0 = none), behind the
        // qpNavNote gate. The abort-time check (pers sent, disc after
        // pers, abort while navigating) isolates the name-collision
        // signature (ONL9 Q2): a lone waiter never drops, and post-pairing
        // link loss lands after the navigator is done.
        std::atomic<uint64_t> g_qpSessDisc{0u};
        // ONL11: navigator arming mode. 0 = off (NAV unset or any other
        // value, or QUICKPLAY off), 1 = launch-flag (arm at boot, ONL10),
        // 2 = online (dormant until the player enters Online, ONL11).
        int qpNavModeImpl()
        {
            static const int mode = [] {
                const char *qp = std::getenv("PS2X_NET_QUICKPLAY");
                if (!qp || std::string(qp) != "1")
                {
                    return 0;
                }
                const char *nav = std::getenv("PS2X_NET_QUICKPLAY_NAV");
                if (!nav)
                {
                    return 0;
                }
                const std::string v(nav);
                if (v == "1")
                {
                    return 1;
                }
                if (v == "online")
                {
                    return 2;
                }
                return 0;
            }();
            return mode;
        }
        bool qpNavNote()
        {
            return qpNavModeImpl() != 0;
        }
        void qpSetFirst(std::atomic<uint64_t> &slot, uint64_t tick)
        {
            uint64_t z = 0u;
            slot.compare_exchange_strong(z, tick, std::memory_order_relaxed);
        }
        // ONL4-P2 PS2X_NET_LOGIN creds (parsed once): auto generates
        // rider-<4hex> from the hostname; name:pass uses both.
        bool g_loginParsed = false;
        bool g_loginOn = false;
        std::string g_loginName;
        std::string g_loginPass;

        // ONL9: one-tap Quick Play prefill (knob PS2X_NET_QUICKPLAY=1,
        // default off, class net). When on and PS2X_NET_LOGIN is unset,
        // credentials come from a local, gitignored file
        // (PS2X_NET_QUICKPLAY_FILE, or ~/.config/ssx3-quickplay-login;
        // first line "name:pass"). Inert when the knob is off: no file
        // read, no log, no guest-visible change.
        bool quickplayOn()
        {
            static const bool on = [] {
                const char *env = std::getenv("PS2X_NET_QUICKPLAY");
                return env && std::string(env) == "1";
            }();
            return on;
        }

        // ONL11: login-file path. Explicit PS2X_NET_QUICKPLAY_FILE wins;
        // otherwise the app's own data dir, resolved like other app-dir
        // paths (Android files dir via the boot-ELF sibling accessor, iOS
        // Documents via PS2X_IOS_DOCUMENTS), falling back to the
        // Mac/desktop dotfile. Empty when no location resolves.
        std::string quickplayLoginPath()
        {
            if (const char *env = std::getenv("PS2X_NET_QUICKPLAY_FILE"))
            {
                if (*env)
                {
                    return std::string(env);
                }
            }
#if defined(__ANDROID__)
            return ps2x::quickplayLoginFilePath();
#else
            if (const char *docs = std::getenv("PS2X_IOS_DOCUMENTS"))
            {
                if (*docs)
                {
                    return std::string(docs) + "/quickplay.login";
                }
            }
            if (const char *home = std::getenv("HOME"))
            {
                if (*home)
                {
                    return std::string(home) + "/.config/ssx3-quickplay-login";
                }
            }
            return std::string();
#endif
        }

        bool readQuickplayLoginFile()
        {
            const std::string path = quickplayLoginPath();
            if (path.empty())
            {
                return false;
            }
            std::ifstream in(path);
            if (!in)
            {
                return false;
            }
            std::string line;
            if (!std::getline(in, line) || line.size() > 256u)
            {
                return false;
            }
            while (!line.empty() &&
                   (line.back() == '\r' || line.back() == '\n' ||
                    line.back() == ' ' || line.back() == '\t'))
            {
                line.pop_back();
            }
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            {
                line.erase(line.begin());
            }
            const size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0u ||
                colon + 1u >= line.size() || colon > 64u ||
                line.size() - colon - 1u > 64u || line.size() - colon - 1u == 0u)
            {
                std::cerr << "[net] quickplay login file malformed (want name:pass)"
                          << std::endl;
                return false;
            }
            g_loginName = line.substr(0u, colon);
            g_loginPass = line.substr(colon + 1u);
            return true;
        }

        // ONL11 addendum: generated identity word lists (~100 clean words
        // each, lowercase ASCII, game-font-safe). Persona = adjective +
        // "-" + noun (e.g. swift-otter); "-" renders in the game font
        // (challenge strings use it) and rides the lobby NAME field.
        const char *kQpAdjectives[100] = {
            "swift", "brave", "calm", "clever", "bright", "bold", "brisk",
            "cedar", "cheerful", "cobalt", "copper", "coral", "cosmic",
            "cozy", "crimson", "crisp", "daring", "dawn", "deft",
            "ebony", "eager", "early", "ember", "fair", "frosty", "gentle",
            "glad", "golden", "grand", "happy", "hasty", "hidden", "honest",
            "humble", "icy", "iron", "ivory", "jade", "jolly", "keen",
            "kind", "lively", "loyal", "lucky", "lunar", "merry", "misty",
            "modest", "noble", "nimble", "north", "ocean", "onyx", "patient",
            "peppy", "plucky", "polar", "proud", "quick", "quiet", "rapid",
            "ready", "rough", "royal", "rustic", "shiny", "silent", "silver",
            "sleek", "smart", "smooth", "snowy", "solar", "solid", "spry",
            "steady", "sturdy", "sunny", "tidal", "timber", "topaz", "tranquil",
            "vivid", "warm", "witty", "wooden", "zesty", "zippy",
            "agile", "amber", "arctic", "ashen", "candid", "cinder",
            "civic", "docile", "earnest", "even", "fluent", "frugal",
        };
        const char *kQpNouns[100] = {
            "otter", "fox", "bear", "wolf", "eagle", "shark", "falcon",
            "raven", "badger", "moose", "bison", "cobra", "panda", "gecko",
            "finch", "heron", "trout", "wren", "lark", "robin", "jay",
            "owl", "hawk", "dove", "salmon", "pike", "bass", "cod", "crab",
            "shrimp", "clam", "pine", "spruce", "maple", "oak",
            "willow", "stone", "pebble", "boulder", "ridge", "peak", "slope",
            "drift", "flake", "frost", "glacier", "powder", "comet", "rocket",
            "sail", "anchor", "beacon", "lantern", "compass", "paddle",
            "summit", "trail", "cabin", "meadow", "brook", "grove", "canyon",
            "mesa", "butte", "dune", "reef", "lagoon", "harbor", "island",
            "fjord", "tundra", "savanna", "prairie", "orchard", "vineyard",
            "sparrow", "oyster", "mussel", "kelp",
            "atoll", "strait", "inlet", "cove", "bay", "port", "dock",
            "ferry", "canoe", "kayak", "sled", "skis", "board", "boots",
            "goggles", "helmet", "mittens", "parka", "lynx", "newt", "vole",
        };
        // Whether the identity came from PS2X_NET_LOGIN (fixed: never
        // renamed) or the login file (ours: collision-suffix allowed).
        bool g_loginFromEnv = false;

        bool writeQuickplayLoginFile(const std::string &path,
                                     const std::string &name,
                                     const std::string &pass)
        {
#if !PS2_NET_HAVE_POSIX_SOCKETS
            (void)path;
            (void)name;
            (void)pass;
            return false;
#else
            const std::string tmp = path + ".tmp";
            const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0)
            {
                return false;
            }
            ::fchmod(fd, 0600);
            const std::string line = name + ":" + pass + "\n";
            size_t done = 0u;
            while (done < line.size())
            {
                const ssize_t w =
                    ::write(fd, line.data() + done, line.size() - done);
                if (w <= 0)
                {
                    break;
                }
                done += static_cast<size_t>(w);
            }
            ::close(fd);
            if (done != line.size())
            {
                return false;
            }
            // Atomic publish (tmp + rename); failure keeps the old file.
            if (::rename(tmp.c_str(), path.c_str()) != 0)
            {
                return false;
            }
            return true;
#endif
        }

        std::string genPersonaName(std::mt19937 &gen)
        {
            std::uniform_int_distribution<size_t> adj(0u, 99u);
            std::uniform_int_distribution<size_t> noun(0u, 99u);
            return std::string(kQpAdjectives[adj(gen)]) + "-" +
                   kQpNouns[noun(gen)];
        }

        std::string genPassword(std::mt19937 &gen)
        {
            static const char kPassChars[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
            std::uniform_int_distribution<size_t> pick(0u, sizeof(kPassChars) - 2u);
            std::string pass;
            pass.reserve(16u);
            for (size_t i = 0u; i < 16u; ++i)
            {
                pass += kPassChars[pick(gen)];
            }
            return pass;
        }

        // ONL11: first-Online-entry login creation. When quickplay is on
        // but no login is configured (no PS2X_NET_LOGIN and no stored file
        // at boot), the first observed Online entry generates an
        // adjective-noun persona with a random password, persists it
        // (mode 600), and arms the login path. Runs on the netw/crea
        // handler, well before the submit, so the preload and the
        // submit-fill carry it. The lobby auto-registers unknown accounts
        // (Rust lobby main.rs "auth": open mode accepts any non-empty NAME;
        // password mode creates on first connect; only an empty/oversize
        // NAME, a known-account password mismatch, or the account cap
        // closes). Secrets are never logged: only the path.
        void ensureQuickplayLogin(uint64_t tick)
        {
            (void)tick;
            if (!quickplayOn() || g_loginOn)
            {
                return;
            }
#if PS2_NET_HAVE_POSIX_SOCKETS
            const std::string path = quickplayLoginPath();
            if (path.empty())
            {
                return;
            }
            // Appeared since boot (or a second entry after one created it):
            // just read it.
            if (readQuickplayLoginFile())
            {
                g_loginOn = true;
                std::cerr << "[net] quickplay prefill login name='" << g_loginName
                          << "' src=file (late)" << std::endl;
                return;
            }
            std::random_device rd;
            std::mt19937 gen(rd());
            const std::string name = genPersonaName(gen);
            const std::string pass = genPassword(gen);
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0)
            {
                if (errno == EEXIST && readQuickplayLoginFile())
                {
                    g_loginOn = true;
                    std::cerr << "[net] quickplay prefill login name='" << g_loginName
                              << "' src=file (late)" << std::endl;
                    return;
                }
                std::cerr << "[net] quickplay: cannot create login file at '" << path
                          << "' (" << std::strerror(errno) << ")" << std::endl;
                return;
            }
            ::fchmod(fd, 0600);
            const std::string line = name + ":" + pass + "\n";
            size_t done = 0u;
            while (done < line.size())
            {
                const ssize_t w =
                    ::write(fd, line.data() + done, line.size() - done);
                if (w <= 0)
                {
                    break;
                }
                done += static_cast<size_t>(w);
            }
            ::close(fd);
            if (done != line.size() || !readQuickplayLoginFile())
            {
                std::cerr << "[net] quickplay: login file write failed at '" << path
                          << "'" << std::endl;
                return;
            }
            g_loginOn = true;
            std::cerr << "[net] quickplay created login file at '" << path << "'"
                      << std::endl;
#else
            return;
#endif
        }

        // ONL11 addendum: the game's own EA Login save. fYes proved the
        // filename empirically: card dir `PSCD10088slogin` holding the
        // data file `PSCD10088slogin` (58-byte obfuscated blob, EA
        // "Electro" SDK format — opaque to us, never parsed), `view.ico`
        // and `icon.sys` (title "EA Login"). Writer chain (read-only):
        // sub_00265E80 -> sub_003F7F48 -> sub_003F8258 -> sub_003FA380
        // with the "EA Login" title record (ELF vaddr 0x497390). The card
        // path is detected (never modified) here; the navigator answers
        // the save prompt from it. NEVER delete or overwrite: only the
        // game's own Yes path writes, and only when no save exists.
        bool qpCardHasEaLoginImpl()
        {
#if PS2_NET_HAVE_POSIX_SOCKETS
            const char *mc = std::getenv("PS2X_MC_ROOT");
            if (!mc || !*mc)
            {
                return false;
            }
            const std::string data =
                std::string(mc) + "/PSCD10088slogin/PSCD10088slogin";
            struct stat st{};
            if (::stat(data.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            {
                return false;
            }
            return true;
#else
            return false;
#endif
        }

        // ONL11 addendum: collision recovery. Called by the navigator when
        // it aborts while navigating. Renames ONLY on the collision
        // signature: pers sent, then a login-socket sess/disc, never
        // paired (ONL9 Q2). A lone waiter (no drop) and a post-pairing
        // link loss (navigator already done) never reach this. The
        // 2-digit suffix keeps the adjective-noun stem readable.
        // Env-fixed identities are never renamed (logged instead).
        void qpNoteNavAbortImpl()
        {
            if (!quickplayOn() || !g_loginOn || g_loginFromEnv)
            {
                return;
            }
            const uint64_t pers = g_qpPers.load(std::memory_order_relaxed);
            const uint64_t disc = g_qpSessDisc.load(std::memory_order_relaxed);
            if (pers == 0u || disc <= pers)
            {
                return;
            }
            const std::string path = quickplayLoginPath();
            if (path.empty())
            {
                return;
            }
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<unsigned> digits(0u, 99u);
            char suffix[4] = {};
            std::snprintf(suffix, sizeof(suffix), "%02u",
                          digits(gen));
            std::string stem = g_loginName;
            if (stem.size() + 2u > 32u)
            {
                stem.resize(30u);
            }
            const std::string renamed = stem + suffix;
            if (!writeQuickplayLoginFile(path, renamed, g_loginPass))
            {
                std::cerr << "[net] quickplay: collision-suspect '" << g_loginName
                          << "' but file rewrite failed; keeping it" << std::endl;
                return;
            }
            g_loginName = renamed;
            std::cerr << "[net] quickplay: collision-suspect login renamed to '"
                      << renamed << "' for the next Online entry" << std::endl;
        }

        void parseLoginKnob()
        {
            if (g_loginParsed)
            {
                return;
            }
            g_loginParsed = true;
            const char *env = std::getenv("PS2X_NET_LOGIN");
            g_loginFromEnv = (env && *env && *env != '0');
            if (!env || !*env || *env == '0')
            {
                if (!quickplayOn())
                {
                    return;
                }
                if (!readQuickplayLoginFile())
                {
                    std::cerr << "[net] quickplay=1 on, no stored login file "
                                 "(set PS2X_NET_LOGIN or PS2X_NET_QUICKPLAY_FILE "
                                 "to fix the identity); a fresh login is created "
                                 "on first Online entry"
                              << std::endl;
                    return;
                }
                g_loginOn = true;
                std::cerr << "[net] quickplay prefill login name='" << g_loginName
                          << "' src=file" << std::endl;
                return;
            }
            std::string v(env);
            if (v == "auto")
            {
                char host[256] = {};
#if PS2_NET_HAVE_POSIX_SOCKETS
                if (::gethostname(host, sizeof(host) - 1u) != 0)
                {
                    host[0] = 0;
                }
#endif
                uint32_t h = 2166136261u;
                for (const char *p = host; *p; ++p)
                {
                    h ^= static_cast<uint8_t>(*p);
                    h *= 16777619u;
                }
                char name[16] = {};
                std::snprintf(name, sizeof(name), "rider-%04x", h & 0xFFFFu);
                g_loginName = name;
                g_loginPass = name;
            }
            else
            {
                const size_t colon = v.find(':');
                if (colon == std::string::npos || colon == 0u ||
                    colon + 1u >= v.size())
                {
                    std::cerr << "[net] PS2X_NET_LOGIN malformed (want auto or name:pass)"
                              << std::endl;
                    return;
                }
                g_loginName = v.substr(0u, colon);
                g_loginPass = v.substr(colon + 1u);
            }
            g_loginOn = true;
            std::cerr << "[net] PS2X_NET_LOGIN name='" << g_loginName << "'"
                      << (quickplayOn() ? " quickplay=1" : "") << std::endl;
        }

        void closeSocket(uint32_t handle)
        {
#if PS2_NET_HAVE_POSIX_SOCKETS
            std::lock_guard<std::mutex> lock(g_sockMutex);
            auto it = g_sockets.find(handle);
            if (it != g_sockets.end())
            {
                ::close(it->second);
                g_sockets.erase(it);
            }
#else
            (void)handle;
#endif
        }

        int getSocket(uint32_t handle)
        {
#if PS2_NET_HAVE_POSIX_SOCKETS
            std::lock_guard<std::mutex> lock(g_sockMutex);
            auto it = g_sockets.find(handle);
            return (it != g_sockets.end()) ? it->second : -1;
#else
            (void)handle;
            return -1;
#endif
        }

        void setSocket(uint32_t handle, int fd)
        {
#if PS2_NET_HAVE_POSIX_SOCKETS
            std::lock_guard<std::mutex> lock(g_sockMutex);
            auto it = g_sockets.find(handle);
            if (it != g_sockets.end())
            {
                ::close(it->second);
            }
            g_sockets[handle] = fd;
#else
            (void)handle;
            (void)fd;
#endif
        }

        // EE->IOP payloads by IOP destination (sifbuf window only).
        std::mutex g_dmaMutex;
        std::map<uint32_t, std::vector<uint8_t>> g_dmaPayloads;

        void writeReply(uint8_t *rdram, uint32_t replyBase, uint32_t i,
                        uint32_t w2, uint32_t outlen, uint32_t status, uint32_t w7,
                        uint32_t r0, uint32_t r1)
        {
            if (replyBase == 0u)
            {
                return;
            }
            uint8_t *reply = getMemPtr(rdram, replyBase + i * 32u);
            if (!reply)
            {
                return;
            }
            std::memset(reply, 0, 32u);
            // v8.6: REPLY[0]/REPLY[4] mimic the real IOP consume
            // pointers (guest-state-driven, advancing) instead of
            // zero. Zero leaves CONSUMED negative, FREE wild, and
            // the queue rejecting late calls (phantom hang).
            std::memcpy(reply + 0u, &r0, 4u);
            std::memcpy(reply + 4u, &r1, 4u);
            std::memcpy(reply + 8u, &w2, 4u);
            std::memcpy(reply + 12u, &outlen, 4u);
            std::memcpy(reply + 20u, &status, 4u);
            std::memcpy(reply + 28u, &w7, 4u);
        }

        // One fake broadband config (256 B entry). Layout from
        // _BuildConfigList: +0 index (>0), +4 name, +66 name, +128 path.
        // All four must be non-empty/nonzero or the IOP drops the entry.
        uint32_t writeFakeConfig(uint8_t *rdram, uint32_t eeBuf, uint32_t maxBytes)
        {
            if (maxBytes < 256u)
            {
                return 0u;
            }
            uint8_t *dst = getMemPtr(rdram, eeBuf);
            if (!dst)
            {
                return 0u;
            }
            std::memset(dst, 0, 256u);
            const uint32_t index = 1u;
            std::memcpy(dst + 0u, &index, 4u);
            const char name[] = "Broadband";
            std::memcpy(dst + 4u, name, sizeof(name));
            std::memcpy(dst + 66u, name, sizeof(name));
            const char path[] = "mc0:Broadband#0";
            std::memcpy(dst + 128u, path, sizeof(path));
            return 1u;
        }

        void dnasReturnZero(uint8_t * /*rdram*/, R5900Context *ctx, PS2Runtime * /*runtime*/)
        {
            // Equivalent to the community DNAS pnach (0x2565FC jal ->
            // lui $v0,0): func_3FD100 has exactly one caller, the DNAS
            // check in sub_002565E0, which branches on $v0 != 0.
            static int fires = 0;
            std::cerr << "[net] DNAS bypass FIRED count=" << ++fires << std::endl;
            setReturnS32(ctx, 0);
        }

        // ONL5: the recompiler folded four net-reply callback thunks
        // (0x268A90.. block, one per reply type) into the previous
        // thunk's function: 0x268C70 sits inside sub_00268C48, whose
        // entry switch has no 0x268C70 label, so a jalr to 0x268C70
        // (the auth reply callback queued by 25C610 via 25B608)
        // runs 0x268C48's thunk (jal 25C4F0) instead of 25C8A8.
        // This entry replays the thunk exactly:
        //   lui v0,0x4A; addiu sp,-16; lh a0,kSlot; sd ra,0(sp);
        //   jal kTarget (delay: addu a0,a2,a0); ld ra,0(sp);
        //   jr ra (delay: addiu sp,16)
        template <uint32_t kThunk, uint32_t kSlot, uint32_t kTarget>
        void foldedThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            SET_GPR_S32(ctx, 2, (int32_t)(0x4Au << 16));
            SET_GPR_S32(ctx, 29, (int32_t)ADD32(GPR_U32(ctx, 29), 0xFFFFFFF0u));
            SET_GPR_S32(ctx, 4, (int16_t)FAST_READ16(kSlot));
            WRITE64(ADD32(GPR_U32(ctx, 29), 0), GPR_U64(ctx, 31));
            SET_GPR_U32(ctx, 31, kThunk + 0x18u);
            SET_GPR_S32(ctx, 4, (int32_t)ADD32(GPR_U32(ctx, 6), GPR_U32(ctx, 4)));
            ctx->pc = kTarget;
            if (!runtime->dispatchGuestBranch(rdram, ctx, kTarget, kThunk + 0x10u, kThunk + 0x18u,
                                              PS2Runtime::GuestBranchKind::DirectCall, "JAL"))
            {
                return;
            }
            SET_GPR_U64(ctx, 31, READ64(ADD32(GPR_U32(ctx, 29), 0)));
            SET_GPR_S32(ctx, 29, (int32_t)ADD32(GPR_U32(ctx, 29), 16u));
            ctx->pc = GPR_U32(ctx, 31);
        }

        void applyOneTimePatches(uint8_t *rdram, PS2Runtime *runtime)
        {
            if (g_patchesApplied)
            {
                return;
            }
            g_patchesApplied = true;

            // Explicit self-hosted server override; DNS also returns lobbyAddress().
            const char *server = std::getenv("PS2X_NET_SERVER");
            const std::string serverValue = server; // enabled() validated this IPv4 value
            uint8_t *strPtr = getMemPtr(rdram, kServerStringAddr);
            if (strPtr && std::memcmp(strPtr, kServerStringOrig, sizeof(kServerStringOrig) - 1u) == 0 &&
                serverValue.size() < sizeof(kServerStringOrig))
            {
                std::memset(strPtr, 0, sizeof(kServerStringOrig));
                std::memcpy(strPtr, serverValue.data(), serverValue.size());
                std::cerr << "[net] server override: '" << kServerStringOrig << "' -> '"
                          << serverValue << "'" << std::endl;
            }
            else
            {
                std::cerr << "[net] server override SKIPPED (verify failed or value too long)"
                          << std::endl;
            }

            // DNAS bypass (recompiler equivalent of the pnach).
            if (runtime && runtime->hasFunction(kDnasDispatchAddr))
            {
                runtime->replaceFunction(kDnasDispatchAddr, &dnasReturnZero);
                std::cerr << "[net] DNAS dispatch bypass armed" << std::endl;
            }
            else
            {
                std::cerr << "[net] DNAS bypass SKIPPED (no table entry)" << std::endl;
            }

            // Exact reply-callback entries are required for typed and preset login.
            parseLoginKnob();
            if (runtime)
            {
                // ONL5 fix #2: exact entries for the four folded
                // reply-callback thunks (see foldedThunk).
                const struct
                {
                    uint32_t addr;
                    PS2Runtime::RecompiledFunction fn;
                } folded[] = {
                    {0x268C70u, &foldedThunk<0x268C70u, 0x4A30E8u, 0x25C8A8u>},
                    {0x268E28u, &foldedThunk<0x268E28u, 0x4A31B8u, 0x25E7F0u>},
                    {0x268EA0u, &foldedThunk<0x268EA0u, 0x4A3248u, 0x25F8A0u>},
                    {0x268EF0u, &foldedThunk<0x268EF0u, 0x4A3318u, 0x260C68u>},
                };
                for (const auto &f : folded)
                {
                    std::cerr << "[net] login: folded thunk 0x" << std::hex << f.addr << std::dec
                              << (runtime->replaceFunction(f.addr, f.fn) ? " entry armed"
                                                                          : " entry SKIPPED")
                              << std::endl;
                }
            }
        }

        uint32_t readU32(const uint8_t *rdram, uint32_t addr, bool &ok)
        {
            ok = false;
            const uint8_t *ptr = getConstMemPtr(rdram, addr);
            if (!ptr)
            {
                return 0u;
            }
            uint32_t value = 0u;
            std::memcpy(&value, ptr, sizeof(value));
            ok = true;
            return value;
        }

        // ONL4: slot fingerprint dedup, shared by the ring path
        // (processDoorbell) and the DMA path (noteDma sweep). A slot is
        // dispatched exactly once per unique 8-word content, whichever
        // path sees it first; ring reuse with identical bytes is
        // skipped; any byte change re-arms the slot.
        uint32_t g_slotLastW[64][8] = {};
        bool g_slotLastValid[64] = {};

        // ONL4: PS2X_NET_QUIET_POLLS=1 drops the log lines for idle
        // sess/recv + ping/resp polls (delivery/first-seen still log)
        // so long runs fit the 16 MB harness log cap. Replies and
        // captures are unaffected.
        bool quietPolls()
        {
            static int cached = -1;
            if (cached < 0)
            {
                const char *env = std::getenv("PS2X_NET_QUIET_POLLS");
                cached = (env && *env && env[0] != '0') ? 1 : 0;
            }
            return cached == 1;
        }

        // LOB1: PS2X_NET_RELAY_REGISTER=1 (default ON) makes the
        // listen (0x101) side emit a NON-game registration datagram
        // ('SSX3RLY1' + the +ses session token) to the +ses peer until
        // it learns the peer, so a relay can learn the listen endpoint
        // even though the stock game handshake is connect-driven.
        // Bounded: every 250 wall ms for at most 30 wall seconds per
        // util. =0 restores stock (no registrations). The peer HLE
        // discards unknown datagram tags silently, and the relay drops
        // registrations without forwarding, so the game handshake stays
        // byte-identical to stock either way.
        bool relayRegister()
        {
            static int cached = -1;
            if (cached < 0)
            {
                const char *env = std::getenv("PS2X_NET_RELAY_REGISTER");
                cached = (!env || !*env || env[0] != '0') ? 1 : 0;
            }
            return cached == 1;
        }

        // Non-consuming check for pending socket bytes (MSG_PEEK), so
        // the quiet-poll decision can see first-seen data.
        bool socketHasData(uint32_t handle)
        {
#if PS2_NET_HAVE_POSIX_SOCKETS
            const int fd = getSocket(handle);
            if (fd < 0)
            {
                return false;
            }
            uint8_t byte = 0u;
            const ssize_t r = ::recv(fd, &byte, 1u, MSG_PEEK);
            return r > 0;
#else
            (void)handle;
            return false;
#endif
        }
    } // namespace

    bool lockstepLinkActive()
    {
#if PS2_NET_HAVE_POSIX_SOCKETS
        return g_lockstepLink.load();
#else
        return false;
#endif
    }

    // ONL10: navigator milestone snapshot + knob reads (see the header).
    // ONL11: qpNavMode 0/1/2 (off/launch/online); wanted = mode != 0.
    bool qpNavQuickplayOn()
    {
        return quickplayOn();
    }

    bool qpNavWanted()
    {
        return qpNavModeImpl() != 0;
    }

    int qpNavMode()
    {
        return qpNavModeImpl();
    }

    bool qpCardHasEaLogin()
    {
        return qpCardHasEaLoginImpl();
    }

    void qpNoteNavAbort()
    {
        qpNoteNavAbortImpl();
    }

    QpNavMilestones qpNavMilestones()
    {
        QpNavMilestones m;
        m.netwCrea = g_qpNetwCrea.load(std::memory_order_relaxed);
        m.creaCount = g_qpNetwCreaCount.load(std::memory_order_relaxed);
        m.pers = g_qpPers.load(std::memory_order_relaxed);
        m.sessDisc = g_qpSessDisc.load(std::memory_order_relaxed);
        m.netwList = g_qpNetwList.load(std::memory_order_relaxed);
        m.netwStar = g_qpNetwStar.load(std::memory_order_relaxed);
        m.sessConnOk = g_qpSessConnOk.load(std::memory_order_relaxed);
        m.sessConnPort = g_qpSessConnPort.load(std::memory_order_relaxed);
        m.sessConnFail = g_qpSessConnFail.load(std::memory_order_relaxed);
        m.gameAuth = g_qpGameAuth.load(std::memory_order_relaxed);
        m.pers = g_qpPers.load(std::memory_order_relaxed);
        m.onln = g_qpOnln.load(std::memory_order_relaxed);
        m.news = g_qpNews.load(std::memory_order_relaxed);
        m.pset = g_qpPset.load(std::memory_order_relaxed);
        m.rget = g_qpRget.load(std::memory_order_relaxed);
        m.quik = g_qpQuik.load(std::memory_order_relaxed);
        m.quikCount = g_qpQuikCount.load(std::memory_order_relaxed);
        return m;
    }

    uint32_t lobbyAddress()
    {
        static const uint32_t address = [] {
#if PS2_NET_HAVE_POSIX_SOCKETS
            const char *server = std::getenv("PS2X_NET_SERVER");
            in_addr parsed{};
            if (server && ::inet_pton(AF_INET, server, &parsed) == 1)
                return ntohl(parsed.s_addr);
#endif
            return uint32_t{0};
        }();
        return address;
    }

    bool enabled()
    {
        static const bool on = [] {
            const char *env = std::getenv("PS2X_NET");
            return env && *env && env[0] != '0' && lobbyAddress() != 0;
        }();
        return on;
    }

    void capture(const char *direction, uint64_t hostTick, uint64_t guestTick,
                 const uint8_t *data, uint32_t length)
    {
        const std::string &path = capturePath();
        if (path.empty() || !direction)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(g_captureMutex);
        std::ofstream out(path, std::ios::app | std::ios::binary);
        if (!out)
        {
            return;
        }
        out << direction << " host=" << hostTick << " guest=" << guestTick
            << " len=" << length << " ";
        char item[4] = {};
        for (uint32_t i = 0; i < length; ++i)
        {
            std::snprintf(item, sizeof(item), "%02X", data ? data[i] : 0);
            out << item;
        }
        out << "\n";
    }

    // DirtySock member table (from DRTYSCKF.IRX RpcRegisterMember call
    // sites; FOURCC (member, sub) -> handler). The EE looks handlers up by
    // FOURCC scan (sub_003F4C98), so order is informational; cookies
    // (0x100+i) stand in for IOP function addresses and are dispatched
    // back in processDoorbell.
    struct MemberEntry
    {
        uint32_t member;
        uint32_t sub;
        const char *name;
    };
    constexpr MemberEntry kMembers[] = {
        {0x6e657477u, 0x63726561u, "netw/crea"}, // RpcNetworkCreate
        {0x6e657477u, 0x64657374u, "netw/dest"}, // RpcNetworkDestroy
        {0x6e657477u, 0x73746172u, "netw/star"}, // RpcNetworkStartup
        {0x6e657477u, 0x73687574u, "netw/shut"}, // RpcNetworkShutdown
        {0x6e657477u, 0x73746174u, "netw/stat"}, // RpcNetworkStatus
        {0x6e657477u, 0x6c697374u, "netw/list"}, // RpcNetworkList
        {0x68747470u, 0x63726561u, "http/crea"},
        {0x68747470u, 0x64657374u, "http/dest"},
        {0x68747470u, 0x67657420u, "http/get "},
        {0x68747470u, 0x696e666fu, "http/info"},
        {0x68747470u, 0x64617461u, "http/data"},
        {0x73657373u, 0x63726561u, "sess/crea"},
        {0x73657373u, 0x64657374u, "sess/dest"},
        {0x73657373u, 0x73746174u, "sess/stat"},
        {0x73657373u, 0x636f6e6eu, "sess/conn"},
        {0x73657373u, 0x64697363u, "sess/disc"},
        {0x73657373u, 0x73656e64u, "sess/send"},
        {0x73657373u, 0x72656376u, "sess/recv"},
        {0x70696e67u, 0x63726561u, "ping/crea"},
        {0x70696e67u, 0x64657374u, "ping/dest"},
        {0x70696e67u, 0x72657175u, "ping/requ"},
        {0x70696e67u, 0x72657370u, "ping/resp"},
        {0x6e616d65u, 0x6c6f6f6bu, "name/look"},
        {0x6e616d65u, 0x646f6e65u, "name/done"},
        {0x6e616d65u, 0x66726565u, "name/free"},
        {0x6e616d65u, 0x61646472u, "name/addr"},
        {0x6d6e676cu, 0x63726561u, "mngl/crea"},
        {0x6d6e676cu, 0x64657374u, "mngl/dest"},
        {0x6d6e676cu, 0x636f6e6eu, "mngl/conn"},
        {0x6d6e676cu, 0x636d706cu, "mngl/cmpl"},
        {0x676d6c6bu, 0x63726561u, "gmlk/crea"},
        {0x676d6c6bu, 0x64657374u, "gmlk/dest"},
        {0x676d6c6bu, 0x73746174u, "gmlk/stat"},
        {0x676d6c6bu, 0x73656e64u, "gmlk/send"},
        {0x676d6c6bu, 0x72656376u, "gmlk/recv"},
        {0x676d6c6bu, 0x6374726cu, "gmlk/ctrl"},
        {0x676d7574u, 0x63726561u, "gmut/crea"},
        {0x676d7574u, 0x64657374u, "gmut/dest"},
        {0x676d7574u, 0x72736574u, "gmut/rset"},
        {0x676d7574u, 0x636f6e6eu, "gmut/conn"},
        {0x676d7574u, 0x636f6d70u, "gmut/comp"},
        {0x676d7574u, 0x73746174u, "gmut/stat"},
        {0x676d7574u, 0x7461626cu, "gmut/tabl"},
        {0x676d7574u, 0x61647674u, "gmut/advt"},
        {0x676d7574u, 0x77697468u, "gmut/with"},
        {0x676d7574u, 0x6c6f6361u, "gmut/loca"},
        {0x676d7574u, 0x71756572u, "gmut/quer"},
    };
    constexpr uint32_t kMemberCount = sizeof(kMembers) / sizeof(kMembers[0]);
    constexpr uint32_t kCookieBase = 0x100u;

    const char *memberName(uint32_t cookie)
    {
        const uint32_t i = (cookie >= kCookieBase) ? (cookie - kCookieBase) : 0xFFFFFFFFu;
        return (i < kMemberCount) ? kMembers[i].name : "?";
    }

    // EE DirtySock state struct base (fixed; both EE helpers use it).
    constexpr uint32_t kEeStateAddr = 0x51F0C0u;
    // EE struct field offsets (from sub_003F5000/sub_003F4528).
    constexpr uint32_t kFSlotBase = 0x4D94u;   // u32 EE slot-area base
    constexpr uint32_t kFReplyBase = 0x4DA0u;  // u32 EE reply-area base
    constexpr uint32_t kFTableBase = 0x4DB8u;  // u32 completion-flag array
    constexpr uint32_t kFIopBase = 0x4DC0u;    // u32 IOP sifbuf base
    constexpr uint32_t kFIdxA = 0x4DD0u;       // ring indices
    constexpr uint32_t kFIdxB = 0x4DD4u;
    constexpr uint32_t kFRingN = 0x4DD8u;
    constexpr uint32_t kFSema = 0x4E08u;

    uint64_t hostMillis();

    uint32_t readEeU32(const uint8_t *rdram, uint32_t addr)
    {
        bool ok = false;
        return readU32(rdram, addr, ok);
    }

    // ONL4-P2: send one synthetic auth on the newest lobby socket.
    // Framing mirrors the scripted client (type + empty sub + BE
    // total size + KEY=value\n pairs + NUL); keys mirror the game's
    // own auth sender (sub_0025C610: NAME/PASS/TOS/MID/PID/HWFLAG/
    // HWMASK/PROD/VERS/LANG/SLUS).
    void synthLoginAuth(uint64_t tick)
    {
        parseLoginKnob();
        if (!g_loginOn || g_loginConn == 0u || g_loginSentConn == g_loginConn)
        {
            return;
        }
#if PS2_NET_HAVE_POSIX_SOCKETS
        const int fd = getSocket(g_loginConn);
        if (fd < 0)
        {
            return;
        }
        std::string body;
        body += "NAME=" + g_loginName + "\n";
        body += "PASS=" + g_loginPass + "\n";
        body += "TOS=1\nMID=1\nPID=1\nHWFLAG=0\nHWMASK=0\n";
        body += "PROD=SSX-PS2-2004\nVERS=PS2/Beta 1.04-Sep 17 2003\nLANG=en\nSLUS=SLUS-20772\n";
        std::vector<uint8_t> msg;
        msg.insert(msg.end(), {'a', 'u', 't', 'h'});
        msg.insert(msg.end(), {0u, 0u, 0u, 0u});
        const uint32_t total = 12u + static_cast<uint32_t>(body.size()) + 1u;
        msg.push_back((total >> 24) & 0xFFu);
        msg.push_back((total >> 16) & 0xFFu);
        msg.push_back((total >> 8) & 0xFFu);
        msg.push_back(total & 0xFFu);
        msg.insert(msg.end(), body.begin(), body.end());
        msg.push_back(0u);
        uint32_t sent = 0u;
        while (sent < msg.size())
        {
            const ssize_t r = ::send(fd, msg.data() + sent, msg.size() - sent, 0);
            if (r <= 0)
            {
                break;
            }
            sent += static_cast<uint32_t>(r);
        }
        capture("tcp-tx", hostMillis(), tick, msg.data(),
                static_cast<uint32_t>(msg.size()));
        std::cerr << "[net] login: synth auth h=" << g_loginConn
                  << " name='" << g_loginName << "' sent=" << sent
                  << "/" << msg.size() << " tick=" << tick << std::endl;
        g_loginSentConn = g_loginConn;
#else
        (void)tick;
#endif
    }

    // ONL4-P3: persona preload. The game's own auth sender
    // (sub_0025C610) reads NAME/PASS from the typed-login struct
    // at 0x54332C (name at +0, Part 2 watch target); when the
    // player never types, pre-fill it with the auto persona so a
    // fired pump sends a valid auth. Best-effort NUL-terminated
    // copies, inert when the knob is off.
    void preloadPersona(uint8_t *rdram, uint64_t tick)
    {
        (void)tick;
        const size_t nameLen = g_loginName.size() + 1u;
        const size_t passLen = g_loginPass.size() + 1u;
        if (nameLen > 64u || passLen > 64u)
        {
            return;
        }
        for (size_t i = 0u; i < nameLen; i++)
        {
            rdram[0x54332Cu + i] = static_cast<uint8_t>(g_loginName[i]);
        }
        for (size_t i = 0u; i < passLen; i++)
        {
            rdram[0x54332Cu + 64u + i] = static_cast<uint8_t>(g_loginPass[i]);
        }
    }

    // ONL4-P2: Verifying-wait detector. The game fills s2+0x48/+0x4C
    // (auth struct = s2, slot [0x4A3028]) on submit and shows
    // Verifying, but never sends auth. ONL4-P3: first preload the
    // persona and give the game's own pump ~10 s to fire (the
    // ghost's reply is unmatched when the game never sent, so the
    // ghost is now a delayed fallback, suppressed entirely once
    // the game sends its own auth).
    void maybeLogin(uint8_t *rdram, uint64_t tick)
    {
        parseLoginKnob();
        if (!enabled() || !g_loginOn || tick < 100u)
            return;
        // Preload runs before any phase gate: the OK submit
        // copies 0x54332C into the auth struct, so the persona
        // must be in place before the player (or script) presses
        // OK, not after phase 3 appears. p3f: not before tick
        // 4200 — p3e proved writes to 0x54332C from tick 100
        // derail the menu flow (no net at all); the dialog
        // opens ~tick 3930, submit lands ~7110.
        if (tick >= 4200u)
        {
            preloadPersona(rdram, tick);
        }
        if (g_loginConn == 0u)
        {
            return;
        }
        const uint32_t phase = readEeU32(rdram, 0xB82E10u);
        if (phase != 3u)
        {
            return;
        }
        const uint32_t authObj = readEeU32(rdram, 0x4A3028u);
        if (authObj == 0u)
        {
            return;
        }
        // ONL9-P2: submit-window authoritative fill (STATE-GATED, not a
        // tick delay). The submit copy (~tick 7104) races the
        // traffic-driven preload: when the HLE observes phase 3, (re)fill
        // the field AND the struct's pointed-to name/pass buffers —
        // whichever the submit installed (alias at 0x54332C, or a dup of
        // the dialog buffer). The game's own auth send (~7126) reads
        // through the struct pointers, so it carries the prefill
        // regardless of which write landed last at submit. Login-gated
        // (all login sources share the race); inert when off. A target is
        // only overwritten when it maps and currently holds name-like
        // bytes (printable, NUL-terminated within 64), never blind.
        {
            const size_t nameLen = g_loginName.size() + 1u;
            const size_t passLen = g_loginPass.size() + 1u;
            if (nameLen <= 64u && passLen <= 64u)
            {
                preloadPersona(rdram, tick);
                const uint32_t targets[2] = {
                    readEeU32(rdram, authObj + 0x48u),
                    readEeU32(rdram, authObj + 0x4Cu)};
                const char *wanted[2] = {g_loginName.c_str(),
                                         g_loginPass.c_str()};
                const size_t wantLen[2] = {nameLen, passLen};
                for (int i = 0; i < 2; ++i)
                {
                    const uint32_t fieldAddr = 0x54332Cu + (i ? 64u : 0u);
                    if (targets[i] == 0u || targets[i] == fieldAddr)
                    {
                        continue;
                    }
                    uint8_t *dst = getMemPtr(rdram, targets[i]);
                    if (!dst)
                    {
                        continue;
                    }
                    size_t cur = 0u;
                    bool nameLike = true;
                    while (cur < 64u && dst[cur])
                    {
                        const char c = static_cast<char>(dst[cur]);
                        if (c < 32 || c >= 127)
                        {
                            nameLike = false;
                            break;
                        }
                        ++cur;
                    }
                    if (!nameLike || cur == 64u)
                    {
                        continue;
                    }
                    for (size_t k = 0u; k < wantLen[i]; ++k)
                    {
                        dst[k] = static_cast<uint8_t>(wanted[i][k]);
                    }
                    static bool logged = false;
                    if (!logged)
                    {
                        logged = true;
                        std::cerr << "[net] quickplay submit-fill target=0x"
                                  << std::hex << targets[i] << std::dec
                                  << " slot=" << (i ? "pass" : "name")
                                  << " tick=" << tick << std::endl;
                    }
                }
            }
        }
        if (g_submitTick == 0u)
        {
            g_submitTick = tick;
        }
        if (g_loginSentConn == g_loginConn || g_gameAuthSeen)
        {
            return;
        }
        if (tick - g_submitTick < 600u)
        {
            return;
        }
        synthLoginAuth(tick);
    }

    // Scan the EE slot ring for new fingerprints and dispatch them.
    // Runs on the ring path (processDoorbell) and on the DMA path
    // (noteDma sweep). The DMA path exists because the EE wrapper
    // (sub_003F4528) queues some requests WITHOUT ringing: it skips
    // the SIFCMD send when its flags bit0 == 0 and sleeps for a
    // completion, relying on the IOP draining the queue
    // asynchronously (the first such call is sess/stat right after
    // the @dir reply, which is ONL3's data+1 stall: the pump stops
    // at the unanswered slot and the waiter sleeps forever). Payload
    // DMAs always precede their slot DMA (same SifSetDma chain, in
    // order), so dispatching at DMA time sees complete payloads, and
    // the shared fingerprint dedup keeps ring-time dispatch from
    // re-running the handler. Returns true when any reply was written.
#if PS2_NET_HAVE_POSIX_SOCKETS
    // ONL5-P2: P2P race channel (gmut/gmlk) over host UDP.
    //
    // The EE drives DirtySock NetGameUtil + NetGameLink in the IRX:
    //   gmut/conn(h, 0x101 listen | 0x102 connect, "addr:lport:rport")
    //   gmut/comp(h) -> CommRef (nonzero) once the peer is connected
    //   gmlk/crea(ref, bufsize, payload = EE status-area address)
    //   gmlk/send(link, 0, packet, len): one NetGame packet; the IRX
    //     then DMAs its 16-byte link header (word 0 = cumulative bytes
    //     accepted) to the status area; the EE's send flow control is
    //     free = bufsize - (written - status[0])
    //   gmlk/recv(link, eeBuf): arms one record; when packets arrive
    //     the IRX (RpcPush) DMAs {end, 0, seq, len} + packets + trailer
    //     {eeBuf, 0, seq, len} to eeBuf; seq counts recv requests
    //   gmlk/stat(link) -> 0x150 B NetGameLinkStat (+0 tick ms,
    //     +0x0C latency, +0x20 last-receive tick, +0x44 isconn,
    //     +0x45 isopen); the EE (0x256AA8) drops the link when
    //     tick - lastRecv >= 30000 or isopen == 0
    // The guest sees its own addr/ports. Host ports: own bind port =
    // PS2X_NET_PORT_BASE (default lport), peer port =
    // PS2X_NET_PEER_PORT (default rport), so two instances on one
    // host behave like two consoles on a LAN. Wire between HLEs:
    // "O5HI" hello, "O5OK" ack, "O5KA" keep-alive, "O5PK"+packet.
    struct P2pUtil
    {
        int fd = -1;
        uint32_t mode = 0u;
        bool peerKnown = false;
        bool connected = false;
        struct sockaddr_in peer;
        uint64_t lastHello = 0u;
        uint64_t connMs = 0u;  // LOB1: wall clock at gmut/conn (registration 30 s bound)
        uint64_t lastRegMs = 0u;  // LOB1: wall clock of last relay registration
        bool regLogged = false;  // LOB1: first-registration log already emitted
        uint32_t lastRxMs = 0u;  // stat +0x20 (last receive, ms)
        // Packets that arrive after gmut/conn but before gmlk/crea (the
        // peer's first NetGame packet can beat our link): CommUDP buffers
        // once connected and the link picks them up; dropping them lost the
        // Character Matchup hello (ONL5-P3 p33).
        std::vector<std::vector<uint8_t>> pendq;
    };
    struct P2pLink
    {
        uint32_t util = 0u;      // gmut handle
        uint32_t statusAddr = 0u;
        uint32_t bufSize = 0u;
        uint32_t cumSent = 0u;
        uint32_t recvAddr = 0u;  // armed EE record buffer (0 = none)
        uint32_t seq = 0u;
        std::vector<std::vector<uint8_t>> rxq;
        uint32_t txPkts = 0u, rxPkts = 0u;
    };
    // LOB1: the +ses session token (if the server sent one), parsed
    // from the +ses message on the game-TCP socket. Included in relay
    // registrations so the relay can verify them. Process-lifetime:
    // one game session per process. Never logged.
    std::string g_sesToken;
    std::map<uint32_t, P2pUtil> g_p2pUtil;  // gmut handle
    std::map<uint32_t, P2pLink> g_p2pLink;  // gmlk handle
    constexpr uint32_t kCommRefBase = 0x00C00000u;
    // SEC1: bound queued P2P packets. A flooder that can reach the UDP
    // port could otherwise grow rxq/pendq without bound (each datagram
    // <= ~2 KB, 64 per pump, no drain without an armed guest record).
    // Legit traffic drains every pump, so the cap never triggers
    // legitimately; newest packets win (oldest dropped first).
    constexpr size_t kP2pRxCap = 512u;

    uint16_t envPort(const char *name, uint16_t dflt)
    {
        const char *e = std::getenv(name);
        if (!e || !*e)
        {
            return dflt;
        }
        const long v = std::strtol(e, nullptr, 10);
        return (v > 0 && v < 65536) ? static_cast<uint16_t>(v) : dflt;
    }

    void p2pSendRaw(P2pUtil &u, const char *tag, const uint8_t *data, size_t n)
    {
        if (u.fd < 0 || !u.peerKnown)
        {
            return;
        }
        std::vector<uint8_t> d(tag, tag + 4);
        if (data && n)
        {
            d.insert(d.end(), data, data + n);
        }
        ::sendto(u.fd, d.data(), d.size(), 0,
                 reinterpret_cast<const struct sockaddr *>(&u.peer), sizeof(u.peer));
    }

    // LOB1: raw datagram send for relay registrations (payload is not
    // a 4-byte game tag). force bypasses the peerKnown gate: the
    // registration goes to the spec peer precisely because the peer is
    // not learned yet. Uncaptured, like game hellos.
    void p2pSendBytes(P2pUtil &u, const uint8_t *data, size_t n, bool force = false)
    {
        if (u.fd < 0 || n == 0u || (!u.peerKnown && !force))
        {
            return;
        }
        ::sendto(u.fd, data, n, 0,
                 reinterpret_cast<const struct sockaddr *>(&u.peer), sizeof(u.peer));
    }

    // Deliver queued packets into an armed EE record (IRX RpcPush layout).
    void p2pDeliver(uint8_t *rdram, uint32_t linkH, P2pLink &l, uint64_t tick)
    {
        if (l.recvAddr == 0u || l.rxq.empty())
        {
            return;
        }
        const uint32_t cap = (l.bufSize > 0x40u) ? l.bufSize - 0x20u : 0x100u;
        std::vector<uint8_t> body;
        size_t taken = 0u;
        while (taken < l.rxq.size() && body.size() + l.rxq[taken].size() <= cap)
        {
            body.insert(body.end(), l.rxq[taken].begin(), l.rxq[taken].end());
            ++taken;
        }
        if (taken == 0u)
        {
            return;
        }
        l.rxq.erase(l.rxq.begin(), l.rxq.begin() + static_cast<long>(taken));
        const uint32_t ee = l.recvAddr & 0x1FFFFFFFu;
        const uint32_t len = static_cast<uint32_t>(body.size());
        const uint32_t padded = (len + 15u) & ~15u;
        uint8_t *dst = getMemPtr(rdram, ee);
        uint8_t *end = getMemPtr(rdram, ee + 0x10u + padded + 0x0Fu);
        if (!dst || !end)
        {
            return;
        }
        const uint32_t hdr[4] = {l.recvAddr + padded + 0x10u, 0u, l.seq, len};
        std::memset(dst, 0, 0x10u + padded + 0x10u);
        std::memcpy(dst + 0x10u, body.data(), len);
        const uint32_t trl[4] = {l.recvAddr, 0u, l.seq, len};
        std::memcpy(dst + 0x10u + padded, trl, 16u);
        std::memcpy(dst, hdr, 16u);
        l.recvAddr = 0u;
        capture("udp-rx", hostMillis(), tick, body.data(), len);
        if (l.rxPkts < 8u || (l.rxPkts % 600u) == 0u)
        {
            std::cerr << "[net] p2p: deliver link=" << linkH << " seq=" << l.seq
                      << " pkts=" << taken << " len=" << len << " tick=" << tick
                      << std::endl;
        }
    }

    void p2pPump(uint8_t *rdram, uint64_t tick)
    {
        for (auto &kv : g_p2pUtil)
        {
            P2pUtil &u = kv.second;
            if (u.fd < 0)
            {
                continue;
            }
            // Stock hello burst: the connect side hellos until
            // connected. The listen side stays silent here (its game
            // handshake traffic starts at crea); LOB1 registration
            // below is a non-game datagram and never touches O5.
            if (!u.connected && (u.mode & 2u) && tick >= u.lastHello + 15u)
            {
                u.lastHello = tick;
                p2pSendRaw(u, "O5HI", nullptr, 0u);
            }
            // LOB1: relay registration (PS2X_NET_RELAY_REGISTER,
            // default ON). The listen side emits 'SSX3RLY1' + the +ses
            // session token to the spec peer until it learns the peer,
            // so a relay can learn the listen endpoint. The peer HLE
            // discards unknown tags silently and the relay drops
            // registrations without forwarding, so game traffic stays
            // byte-identical to stock. Bounded: every 250 wall ms for
            // at most 30 wall s per util.
            if ((u.mode & 1u) && !u.peerKnown && relayRegister())
            {
                const uint64_t nowMs = hostMillis();
                if (nowMs - u.connMs < 30000u && nowMs - u.lastRegMs >= 250u)
                {
                    u.lastRegMs = nowMs;
                    std::string reg("SSX3RLY1");
                    reg += g_sesToken;
                    p2pSendBytes(u,
                                 reinterpret_cast<const uint8_t *>(reg.data()),
                                 reg.size(), true);
                    if (!u.regLogged)
                    {
                        u.regLogged = true;
                        std::cerr << "[net] p2p: util=" << kv.first
                                  << " relay-reg tx token_len=" << g_sesToken.size()
                                  << " tick=" << tick << std::endl;
                    }
                }
            }
            // Link keep-alive: the IRX NetGameLink sends its own sync
            // packets while idle (_NetGameLinkProcess ->
            // _NetGameLinkSendPacket), so a quiet game screen (e.g.
            // Character Matchup) never trips the EE's 30 s
            // last-receive check. Once a second while connected.
            if (u.connected && tick >= u.lastHello + 60u)
            {
                u.lastHello = tick;
                p2pSendRaw(u, "O5KA", nullptr, 0u);
            }
            uint8_t buf[2048];
            for (int n = 0; n < 64; ++n)
            {
                struct sockaddr_in from;
                socklen_t fl = sizeof(from);
                const ssize_t r = ::recvfrom(u.fd, buf, sizeof(buf), 0,
                                             reinterpret_cast<struct sockaddr *>(&from), &fl);
                if (r < 4)
                {
                    break;
                }
                // SEC1: learn-then-enforce the peer source address. Connect
                // mode pins the spec address at conn time; listen mode
                // learns it from the first hello (the connect side hellos
                // until acked, and no race packet flows before the
                // hello/ack exchange, so this cannot deadlock legit play).
                // Afterwards datagrams from any other source address are
                // dropped: without this any host that can reach the UDP
                // port can inject race packets, force the link connected,
                // or keep a dead link's 30 s last-receive check fresh.
                // Only the address is checked, never the port, so NAT
                // source-port remapping still works. Residual: a blind
                // off-path spoofer forging the peer's source address;
                // closing that needs a lobby-issued session token (SEC1).
                const bool isListen = (u.mode & 1u) != 0u;
                if (!u.peerKnown && !isListen)
                {
                    u.peerKnown = true;
                }
                if (isListen && !u.peerKnown)
                {
                    if (std::memcmp(buf, "O5HI", 4) != 0)
                    {
                        continue;
                    }
                    u.peer = from;
                    u.peerKnown = true;
                }
                if (u.peerKnown &&
                    from.sin_addr.s_addr != u.peer.sin_addr.s_addr)
                {
                    continue;
                }
                u.lastRxMs = static_cast<uint32_t>(hostMillis());
                if (std::memcmp(buf, "O5HI", 4) == 0)
                {
                    p2pSendRaw(u, "O5OK", nullptr, 0u);
                    if (!u.connected)
                    {
                        std::cerr << "[net] p2p: util=" << kv.first << " hello rx -> connected tick="
                                  << tick << std::endl;
                    }
                    u.connected = true;
                }
                else if (std::memcmp(buf, "O5OK", 4) == 0)
                {
                    if (!u.connected)
                    {
                        std::cerr << "[net] p2p: util=" << kv.first << " ack rx -> connected tick="
                                  << tick << std::endl;
                    }
                    u.connected = true;
                }
                else if (std::memcmp(buf, "O5PK", 4) == 0 && r > 4)
                {
                    u.connected = true;
                    bool queued = false;
                    for (auto &lk : g_p2pLink)
                    {
                        if (lk.second.util == kv.first)
                        {
                            // SEC1: drop oldest at the cap (see kP2pRxCap).
                            if (lk.second.rxq.size() >= kP2pRxCap)
                            {
                                lk.second.rxq.erase(lk.second.rxq.begin());
                            }
                            lk.second.rxq.emplace_back(buf + 4, buf + r);
                            lk.second.rxPkts++;
                            queued = true;
                        }
                    }
                    if (!queued)
                    {
                        if (u.pendq.size() >= kP2pRxCap)
                        {
                            u.pendq.erase(u.pendq.begin());
                        }
                        u.pendq.emplace_back(buf + 4, buf + r);
                    }
                }
            }
        }
        for (auto &lk : g_p2pLink)
        {
            p2pDeliver(rdram, lk.first, lk.second, tick);
        }
    }

#endif

    bool scanAndDispatch(uint8_t *rdram, uint64_t tick, bool fromRing)
    {
        maybeLogin(rdram, tick);
#if PS2_NET_HAVE_POSIX_SOCKETS
        if (!g_p2pUtil.empty())
        {
            p2pPump(rdram, tick);
        }
#endif

        const uint32_t slotBase = readEeU32(rdram, kEeStateAddr + kFSlotBase) & 0x1FFFFFFFu;
        const uint32_t replyBase = readEeU32(rdram, kEeStateAddr + kFReplyBase) & 0x1FFFFFFFu;
        const uint32_t tableBase = readEeU32(rdram, kEeStateAddr + kFTableBase) & 0x1FFFFFFFu;
        const uint32_t ringN = readEeU32(rdram, kEeStateAddr + kFRingN);
        const uint32_t sema = readEeU32(rdram, kEeStateAddr + kFSema);
        (void)sema;

        bool replied = false;
        const uint32_t n = (ringN > 0u && ringN <= 64u) ? ringN : 16u;
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t slot = slotBase + i * 32u;
            const uint32_t w0 = readEeU32(rdram, slot + 0u);
            const uint32_t w1 = readEeU32(rdram, slot + 4u);
            const uint32_t w2 = readEeU32(rdram, slot + 8u);
            const uint32_t w3 = readEeU32(rdram, slot + 12u);
            const uint32_t w4 = readEeU32(rdram, slot + 16u);
            const uint32_t w5 = readEeU32(rdram, slot + 20u);
            const uint32_t w6 = readEeU32(rdram, slot + 24u);
            const uint32_t w7 = readEeU32(rdram, slot + 28u);
            if (w0 == 0u && w1 == 0u && w2 == 0u && w3 == 0u &&
                w4 == 0u && w5 == 0u && w6 == 0u && w7 == 0u)
            {
                continue;
            }
            const uint32_t cur[8] = {w0, w1, w2, w3, w4, w5, w6, w7};
            bool same = g_slotLastValid[i];
            for (int k = 0; same && k < 8; ++k)
            {
                same = (g_slotLastW[i][k] == cur[k]);
            }
            if (same)
            {
                continue;
            }
            for (int k = 0; k < 8; ++k)
            {
                g_slotLastW[i][k] = cur[k];
            }
            g_slotLastValid[i] = true;
            // Handler 1 = RpcReturnTable: copy the member table to w2
            // (EE addr, w3 max bytes). Reply slot mirrors the IOP dispatch
            // (RpcThread 0x1C9C): the alarm handler (sub_003F4448) wakes
            // when reply[0x1C] ECHOES request[0x1C].
            if (w6 == 1u)
            {
                const uint32_t eeBuf = w2 & 0x1FFFFFFFu;
                const uint32_t maxBytes = w3;
                uint8_t *dst = getMemPtr(rdram, eeBuf);
                uint32_t outlen = 0u;
                if (dst && maxBytes >= kMemberCount * 16u)
                {
                    for (uint32_t m = 0; m < kMemberCount; ++m)
                    {
                        const uint32_t index = m;
                        const uint32_t cookie = kCookieBase + m;
                        std::memcpy(dst + m * 16u + 0u, &index, 4u);
                        std::memcpy(dst + m * 16u + 4u, &kMembers[m].member, 4u);
                        std::memcpy(dst + m * 16u + 8u, &kMembers[m].sub, 4u);
                        std::memcpy(dst + m * 16u + 12u, &cookie, 4u);
                    }
                    outlen = kMemberCount * 16u;
                }
                if (replyBase != 0u)
                {
                    uint8_t *reply = getMemPtr(rdram, replyBase + i * 32u);
                    if (reply)
                    {
                        std::memset(reply, 0, 32u);
                        std::memcpy(reply + 8u, &w2, 4u);
                        std::memcpy(reply + 12u, &outlen, 4u);
                        const uint32_t status = static_cast<uint32_t>(kMemberCount);
                        std::memcpy(reply + 20u, &status, 4u);
                        std::memcpy(reply + 28u, &w7, 4u);
                    }
                }
                // Completion flag for the EE lookup table (if indexed).
                if (tableBase != 0u)
                {
                    uint8_t *flag = getMemPtr(rdram, tableBase + i * 4u);
                    if (flag)
                    {
                        const uint32_t one = 1u;
                        std::memcpy(flag, &one, 4u);
                    }
                }
                std::cerr << "[net] table reply tick=" << tick << " slot=" << i
                          << " eeBuf=0x" << std::hex << eeBuf << " members=" << std::dec
                          << kMemberCount << " outlen=" << outlen << std::endl;
                replied = true;
            }
            else if (w6 >= kCookieBase && w6 < kCookieBase + kMemberCount)
            {
                // Cookie dispatch: log the call; per-handler payload
                // emulation lands incrementally (stubs: status 0/outlen 0).
                const uint32_t method = w6 - kCookieBase;
                const uint32_t st_dc = readEeU32(rdram, kEeStateAddr + 0x4DDCu);
                // Quiet-poll decision (replies/captures unaffected):
                // ping/resp is always empty (stub); sess/recv logs only
                // delivery and first-seen.
                bool loud = true;
                if (quietPolls())
                {
                    if (method == 21u)
                    {
                        loud = false;
                        auto qpi = g_pingReq.find(w4);
                        if (qpi != g_pingReq.end() &&
                            tick >= qpi->second.tick + 2u &&
                            !qpi->second.payload.empty())
                        {
                            loud = true;
                        }
                    }
                    else if (method == 17u)
                    {
                        loud = false;
                        auto qhi = g_recvHold.find(w4);
                        if (qhi != g_recvHold.end() && tick >= g_recvReady[w4])
                        {
                            loud = true;
                        }
                        else if (qhi == g_recvHold.end() && socketHasData(w4))
                        {
                            loud = true;
                        }
                    }
                }
                if (loud)
                {
                    std::cerr << "[net] call tick=" << tick << " slot=" << i << " "
                              << memberName(w6) << " a0=0x" << std::hex << w4
                              << " a1=0x" << w5 << " a2=0x" << w0 << " a3=0x" << w1
                              << " eeBuf=0x" << (w2 & 0x1FFFFFFFu) << " max=" << std::dec << w3
                              << std::dec;
                    // ONL5-P2: P2P families (mngl/gmlk/gmut): log the
                    // payload (address strings, link args) too.
                    if (method >= 26u && method <= 46u && w1 > 0u)
                    {
                        std::cerr << " pay=" << dmaHex(w0, 64u);
                    }
                }
                uint32_t status = 0u;
                uint32_t outlen = 0u;
                if (method == 5u)
                {
                    // netw/list: a2 = IOP addr of the source string
                    // ("mc0:"/"mc1:"). One fake broadband config on mc0:.
                    const std::string src = dmaString(w0);
                    std::cerr << " src='" << src << "'";
                    if (src == "mc0:")
                    {
                        const uint32_t n = writeFakeConfig(rdram, w2 & 0x1FFFFFFFu, w3);
                        status = n;
                        outlen = n * 256u;
                        if (qpNavNote())
                        {
                            qpSetFirst(g_qpNetwList, tick);
                        }
                    }
                }
#if PS2_NET_HAVE_POSIX_SOCKETS
                else if (method == 39u)
                {
                    // ONL5-P2 gmut/conn: a0 = util, a1 = mode
                    // (0x101 listen / 0x102 connect), a2/a3 =
                    // "addr:lport:rport".
                    const std::string spec = dmaString(w0);
                    std::cerr << " spec='" << spec << "'";
                    uint32_t ip = 0x7F000001u;
                    uint16_t lport = 3658u, rport = 3658u;
                    {
                        unsigned a = 0, b = 0, c = 0, d = 0, p1 = 0, p2 = 0;
                        const int got = std::sscanf(spec.c_str(), "%u.%u.%u.%u:%u:%u",
                                                    &a, &b, &c, &d, &p1, &p2);
                        if (got >= 4)
                        {
                            ip = (a << 24) | (b << 16) | (c << 8) | d;
                        }
                        if (got >= 5 && p1 > 0u && p1 < 65536u)
                        {
                            lport = static_cast<uint16_t>(p1);
                            rport = lport;
                        }
                        if (got >= 6 && p2 > 0u && p2 < 65536u)
                        {
                            rport = static_cast<uint16_t>(p2);
                        }
                    }
                    P2pUtil &u = g_p2pUtil[w4];
                    if (u.fd >= 0)
                    {
                        ::close(u.fd);
                    }
                    u = P2pUtil();
                    u.mode = w5;
                    const uint16_t bindPort = envPort("PS2X_NET_PORT_BASE", lport);
                    const uint16_t peerPort = envPort("PS2X_NET_PEER_PORT", rport);
                    u.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
                    struct sockaddr_in sin;
                    std::memset(&sin, 0, sizeof(sin));
                    sin.sin_family = AF_INET;
                    sin.sin_port = htons(bindPort);
                    sin.sin_addr.s_addr = htonl(INADDR_ANY);
                    const int one = 1;
                    ::setsockopt(u.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                    const bool bound = (u.fd >= 0) &&
                                       ::bind(u.fd, reinterpret_cast<struct sockaddr *>(&sin),
                                              sizeof(sin)) == 0;
                    if (u.fd >= 0)
                    {
                        const int fl = ::fcntl(u.fd, F_GETFL, 0);
                        ::fcntl(u.fd, F_SETFL, fl | O_NONBLOCK);
                    }
                    std::memset(&u.peer, 0, sizeof(u.peer));
                    u.peer.sin_family = AF_INET;
                    u.peer.sin_port = htons(peerPort);
                    u.peer.sin_addr.s_addr = htonl(ip);
                    u.peerKnown = (w5 & 2u) != 0u;
                    u.connMs = hostMillis();
                    std::cerr << " [net] p2p: util=" << w4 << " mode=0x" << std::hex << w5
                              << std::dec << " bind=" << bindPort << (bound ? "" : "(FAILED)")
                              << " peer=" << peerPort << " tick=" << tick;
                    if ((w5 & 1u) && relayRegister())
                    {
                        std::cerr << " relay-reg=on";
                    }
                    status = 0u;
                }
                else if (method == 40u)
                {
                    // ONL5-P2 gmut/comp: CommRef once connected, else 0.
                    auto ui = g_p2pUtil.find(w4);
                    status = (ui != g_p2pUtil.end() && ui->second.connected)
                                 ? (kCommRefBase | w4)
                                 : 0u;
                    if (status != 0u)
                    {
                        std::cerr << " [net] p2p: comp util=" << w4 << " -> ref=0x" << std::hex
                                  << status << std::dec;
                    }
                }
                else if (method == 30u && (w4 & 0xFFFF0000u) == kCommRefBase)
                {
                    // ONL5-P2 gmlk/crea: a0 = CommRef, a1 = buffer
                    // size, a2/a3 = 4 B EE status-area address.
                    status = ++g_nextHandle;
                    P2pLink &l = g_p2pLink[status];
                    l.util = w4 & 0xFFFFu;
                    l.bufSize = (w5 + 15u) & 0x7FF0u;
                    const std::vector<uint8_t> pay = dmaBytes(w0);
                    if (pay.size() >= 4u)
                    {
                        std::memcpy(&l.statusAddr, pay.data(), 4u);
                    }
                    if (g_p2pUtil.count(l.util))
                    {
                        auto &pq = g_p2pUtil[l.util].pendq;
                        if (!pq.empty())
                        {
                            std::cerr << " [net] p2p: link takes " << pq.size()
                                      << " early packet(s)";
                            for (auto &pk : pq)
                            {
                                l.rxq.push_back(std::move(pk));
                                l.rxPkts++;
                            }
                            pq.clear();
                        }
                    }
                    g_lockstepLink.store(true);
                    std::cerr << " [net] p2p: link=" << status << " util=" << l.util
                              << " buf=0x" << std::hex << l.bufSize << " status=0x"
                              << l.statusAddr << std::dec;
                }
                else if (method == 32u && g_p2pLink.count(w4))
                {
                    // ONL5-P2 gmlk/stat: 0x150 B into eeBuf.
                    const uint32_t eeBuf = w2 & 0x1FFFFFFFu;
                    uint8_t *dst = (w3 >= 0x150u) ? getMemPtr(rdram, eeBuf) : nullptr;
                    if (dst)
                    {
                        std::memset(dst, 0, 0x150u);
                        const uint32_t now = static_cast<uint32_t>(hostMillis());
                        std::memcpy(dst, &now, 4u);
                        const P2pLink &l = g_p2pLink[w4];
                        const bool conn = g_p2pUtil.count(l.util) && g_p2pUtil[l.util].connected;
                        if (conn)
                        {
                            const uint32_t rx = g_p2pUtil[l.util].lastRxMs;
                            std::memcpy(dst + 0x20u, &rx, 4u);
                        }
                        dst[0x44] = conn ? 1u : 0u;
                        dst[0x45] = conn ? 1u : 0u;
                        outlen = 0x150u;
                    }
                    status = 0u;
                }
                else if (method == 33u && g_p2pLink.count(w4))
                {
                    // ONL5-P2 gmlk/send: one packet to the peer, then
                    // the 16 B link header to the EE status area.
                    P2pLink &l = g_p2pLink[w4];
                    const std::vector<uint8_t> bytes = dmaBytes(w0);
                    const uint32_t len = (w1 <= bytes.size()) ? w1 : static_cast<uint32_t>(bytes.size());
                    if (len > 0u && g_p2pUtil.count(l.util))
                    {
                        p2pSendRaw(g_p2pUtil[l.util], "O5PK", bytes.data(), len);
                        capture("udp-tx", hostMillis(), tick, bytes.data(), len);
                    }
                    l.cumSent += len;
                    l.txPkts++;
                    uint8_t *st = l.statusAddr ? getMemPtr(rdram, l.statusAddr & 0x1FFFFFFFu) : nullptr;
                    if (st)
                    {
                        const uint32_t hdr[4] = {l.cumSent, 0u, 0u, 0u};
                        std::memcpy(st, hdr, 16u);
                    }
                    if (l.txPkts <= 8u || (l.txPkts % 600u) == 0u)
                    {
                        std::cerr << " [net] p2p: tx link=" << w4 << " n=" << len
                                  << " cum=" << l.cumSent << " pkts=" << l.txPkts;
                    }
                    status = len;
                }
                else if (method == 34u && g_p2pLink.count(w4))
                {
                    // ONL5-P2 gmlk/recv: arm one record at a1.
                    P2pLink &l = g_p2pLink[w4];
                    l.seq++;
                    l.recvAddr = w5;
                    p2pDeliver(rdram, w4, l, tick);
                    status = 0u;
                }
                else if ((method == 31u && g_p2pLink.count(w4)) ||
                         (method == 37u && g_p2pUtil.count(w4)))
                {
                    // ONL5-P2 gmlk/dest, gmut/dest.
                    if (method == 31u)
                    {
                        g_p2pLink.erase(w4);
                        g_lockstepLink.store(!g_p2pLink.empty());
                    }
                    else
                    {
                        if (g_p2pUtil[w4].fd >= 0)
                        {
                            ::close(g_p2pUtil[w4].fd);
                        }
                        g_p2pUtil.erase(w4);
                    }
                    std::cerr << " [net] p2p: dest h=" << w4;
                    status = 0u;
                }
#endif
                else if (method == 0u)
                {
                    // netw/crea: a2 = IOP addr of the domain ("www.ea.com").
                    // Observed when the player enters Online from the main
                    // menu (ONL10 D1: first guest net call of the Online
                    // flow; the SP dance is net-silent). ONL11: arms the
                    // online navigator mode and the first-Online-entry
                    // login creation.
                    std::cerr << " domain='" << dmaString(w0) << "'";
                    status = 0u;
                    if (qpNavNote())
                    {
                        qpSetFirst(g_qpNetwCrea, tick);
                        g_qpNetwCreaCount.fetch_add(1u, std::memory_order_relaxed);
                    }
                    ensureQuickplayLogin(tick);
                }
                else if (method == 2u)
                {
                    // netw/star: a2 = IOP addr of the selected config path.
                    std::cerr << " path='" << dmaString(w0) << "'";
                    g_linkUp = true;
                    status = 0u;
                    if (qpNavNote())
                    {
                        qpSetFirst(g_qpNetwStar, tick);
                    }
                }
                else if (method == 4u)
                {
                    // netw/stat: a1 = sub-query FOURCC, pure status code
                    // (no out buffer, except mach). Report link-up.
                    const char *q = "?";
                    if (w5 == 0x686f7374u) { q = "host"; status = g_linkUp ? 2u : 0u; }
                    else if (w5 == 0x636f6e6eu) { q = "conn"; status = g_linkUp ? 1u : 0u; }
                    else if (w5 == 0x6f6e6c6eu) { q = "onln"; status = g_linkUp ? 1u : 0u; }
                    else if (w5 == 0x61646472u) { q = "addr"; status = g_linkUp ? 0x7F000001u : 0u; }
                    else if (w5 == 0x74797065u) { q = "type"; status = 0u; }
                    else if (w5 == 0x6d616378u)
                    {
                        // mach: guest passes an out buffer (max=16);
                        // fill a fixed locally-administered MAC.
                        q = "mach";
                        const uint32_t eeBuf = w2 & 0x1FFFFFFFu;
                        uint8_t *dst = (w3 >= 6u) ? getMemPtr(rdram, eeBuf) : nullptr;
                        if (dst)
                        {
                            const uint8_t mac[6] = {0x02u, 0x0Cu, 0x29u, 0x4Fu, 0x4Eu, 0x4Cu};
                            std::memcpy(dst, mac, sizeof(mac));
                            outlen = 6u;
                        }
                        status = 0u;
                    }
                    std::cerr << " q=" << q;
                }
                else if (method == 22u)
                {
                    // name/look: a2 = IOP addr of the hostname (a3 = len
                    // incl NUL). Resolve only through the configured lobby override.
                    std::cerr << " host='" << dmaString(w0) << "' hex=" << dmaHex(w0, 64u);
                    g_dnsResolved = true;
                    status = 0u;
                }
                else if (method == 23u)
                {
                    // name/done: completion poll; 1 once resolved.
                    status = g_dnsResolved ? 1u : 0u;
                }
                else if (method == 25u)
                {
                    // name/addr: the resolved address comes back in STATUS
                    // (eeBuf=0/max=0 in practice; the EE lib builds the
                    // sockaddr itself). Same convention as netw/stat addr.
                    status = lobbyAddress();
                    const uint32_t eeBuf = w2 & 0x1FFFFFFFu;
                    uint8_t *dst = (w3 >= 4u) ? getMemPtr(rdram, eeBuf) : nullptr;
                    if (dst)
                    {
                        std::memcpy(dst, &status, 4u);
                        outlen = 4u;
                    }
                }
                else if (method == 11u || method == 18u || method == 6u ||
                         method == 26u || method == 30u || method == 36u)
                {
                    // */crea: hand out a host handle in status.
                    status = ++g_nextHandle;
                    std::cerr << " h=" << status << " hex=" << dmaHex(w0, 64u);
                }
                else if (method == 13u)
                {
                    // sess/stat: a0 = handle; eeBuf/max = 32 B out =
                    // remote sockaddr (16 B) + local sockaddr (16 B).
                    // The game reads remote port @+2 / addr @+4 and
                    // local port @+18 / addr @+20 (all BE) via the
                    // 'port'/'addr'/'lprt'/'ladr' queries
                    // (sub_003F57C8); 'ladr'/'lprt' feed the addr
                    // message and the login state machine, so zeros
                    // here stall the login at "Verifying" (~30 s then
                    // teardown). Fill from the live TCP socket.
                    const uint32_t eeBuf = w2 & 0x1FFFFFFFu;
                    uint8_t *statDst = (w3 >= 32u) ? getMemPtr(rdram, eeBuf) : nullptr;
                    if (statDst)
                    {
                        std::memset(statDst, 0, 32u);
#if PS2_NET_HAVE_POSIX_SOCKETS
                        const int statFd = getSocket(w4);
                        if (statFd >= 0)
                        {
                            struct sockaddr_in peer;
                            struct sockaddr_in self;
                            socklen_t peerLen = sizeof(peer);
                            socklen_t selfLen = sizeof(self);
                            if (::getpeername(statFd, reinterpret_cast<struct sockaddr *>(&peer),
                                              &peerLen) == 0 &&
                                peerLen >= 8u)
                            {
                                // Family AF_INET as BE u16 @+0 (the
                                // queries never read it; the IOP is BE),
                                // port (BE) @+2, addr (BE) @+4.
                                statDst[0u] = 0x00u;
                                statDst[1u] = 0x02u;
                                std::memcpy(statDst + 2u, &peer.sin_port, 2u);
                                std::memcpy(statDst + 4u, &peer.sin_addr.s_addr, 4u);
                            }
                            if (::getsockname(statFd, reinterpret_cast<struct sockaddr *>(&self),
                                              &selfLen) == 0 &&
                                selfLen >= 8u)
                            {
                                statDst[16u] = 0x00u;
                                statDst[17u] = 0x02u;
                                std::memcpy(statDst + 18u, &self.sin_port, 2u);
                                std::memcpy(statDst + 20u, &self.sin_addr.s_addr, 4u);
                            }
                        }
#endif
                        outlen = 32u;
                        if (loud)
                        {
                            std::cerr << " h=" << w4 << " addrs=live";
                        }
                    }
                    status = 0u;
                }
                else if (method == 14u)
                {
                    // sess/conn: a0 = handle, a2/a3 = sockaddr (16 B:
                    // family LE, port BE, addr BE). Blocking TCP connect;
                    // loopback is instant. Non-blocking afterwards: the
                    // game polls sess/recv.
                    const std::vector<uint8_t> sa = dmaBytes(w0);
                    uint16_t port = 0u;
                    uint32_t addr = 0u;
                    if (sa.size() >= 8u)
                    {
                        port = static_cast<uint16_t>((sa[2] << 8) | sa[3]);
                        addr = (static_cast<uint32_t>(sa[4]) << 24) |
                               (static_cast<uint32_t>(sa[5]) << 16) |
                               (static_cast<uint32_t>(sa[6]) << 8) | sa[7];
                    }
                    std::cerr << " h=" << w4 << " port=" << port << " addr=0x"
                              << std::hex << addr << std::dec
                              << " hex=" << dmaHex(w0, 16u);
#if PS2_NET_HAVE_POSIX_SOCKETS
                    closeSocket(w4);
                    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                    bool ok = (fd >= 0);
#ifdef SO_NOSIGPIPE
                    // ONL5-P2: a send on a socket the server reset must
                    // fail (as on the IRX), not SIGPIPE the runner (o7a).
                    if (ok)
                    {
                        const int one = 1;
                        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
                    }
#endif
                    if (ok)
                    {
                        struct sockaddr_in sin;
                        std::memset(&sin, 0, sizeof(sin));
                        sin.sin_family = AF_INET;
                        sin.sin_port = htons(port);
                        sin.sin_addr.s_addr = htonl(addr);
                        ok = (::connect(fd, reinterpret_cast<struct sockaddr *>(&sin),
                                        sizeof(sin)) == 0);
                    }
                    if (ok)
                    {
                        const int flags = ::fcntl(fd, F_GETFL, 0);
                        if (flags >= 0)
                        {
                            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                        }
                        setSocket(w4, fd);
                        g_loginConn = w4;
                        std::cerr << " connected";
                        if (qpNavNote())
                        {
                            qpSetFirst(g_qpSessConnOk, tick);
                            g_qpSessConnPort.store(port, std::memory_order_relaxed);
                        }
                        status = 0u;
                        // ONL5: the real IRX connects asynchronously
                        // (RpcProtoAriesUpdate, DRTYSCKF .text 0x5738):
                        // on 'peer' success it resets the input buffer
                        // to one header-only frame type=-1 sub=-1
                        // size=12. The EE processor (0x3E9944) answers
                        // it by sending 'skey' and moving the conn tag
                        // conn -> skey (-> idle on the server's skey),
                        // the tag the request sender 0x3E8B10 needs.
                        // In tag rdir the frame is ignored (0x3E979C).
                        {
                            g_recvHold[w4] = std::vector<uint8_t>{
                                0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
                                0xFFu, 0xFFu, 0x00u, 0x00u, 0x00u, 0x0Cu};
                            g_recvReady[w4] = static_cast<uint32_t>(tick);
                            std::cerr << " [net] login: connect frame queued h=" << w4
                                      << " tick=" << tick;
                        }
                    }
                    else
                    {
                        if (fd >= 0)
                        {
                            ::close(fd);
                        }
                        std::cerr << " CONNECT-FAILED";
                        status = 1u;
                        if (qpNavNote())
                        {
                            qpSetFirst(g_qpSessConnFail, tick);
                        }
                    }
#else
                    std::cerr << " NO-SOCKETS";
                    status = 1u;
#endif
                }
                else if (method == 15u)
                {
                    // sess/disc: a0 = handle; close (no-op when fresh).
                    // ONL11 addendum: first-drop milestone for the
                    // collision signature (checked at navigator aborts).
                    closeSocket(w4);
                    std::cerr << " h=" << w4;
                    status = 0u;
                    if (qpNavNote())
                    {
                        qpSetFirst(g_qpSessDisc, tick);
                    }
                }
                else if (method == 16u)
                {
                    // sess/send: a0 = handle, a2/a3 = bytes (a3 = length;
                    // the DMA is padded). Send exactly a3, capture, echo
                    // the byte count in status.
                    const std::vector<uint8_t> bytes = dmaBytes(w0);
                    const uint32_t len = (w1 <= bytes.size()) ? w1 : static_cast<uint32_t>(bytes.size());
                    std::cerr << " h=" << w4 << " n=" << len << " hex=" << dmaHex(w0, 64u);
                    status = 0u;
                    if (len > 0u)
                    {
                        capture("tcp-tx", hostMillis(), tick, bytes.data(), len);
                        // ONL4-P3: own-auth detector. A game-sent
                        // 'auth' message (the sub_0025C610 sender)
                        // means the pump fired; suppress the ghost
                        // so the reply matches the game's queue.
                        if (len >= 12u && bytes[0] == 'a' && bytes[1] == 'u' &&
                            bytes[2] == 't' && bytes[3] == 'h' && !g_gameAuthSeen)
                        {
                            g_gameAuthSeen = true;
                            std::cerr << " [net] login: GAME AUTH seen n=" << len
                                      << " tick=" << tick << std::endl;
                            if (qpNavNote())
                            {
                                qpSetFirst(g_qpGameAuth, tick);
                            }
                        }
                        // ONL10: navigator lobby milestones. sess/send
                        // carries every client->lobby message with a
                        // 4-char type in bytes[0..3]; record first-seen
                        // ticks for the persona/online-main/filter gates
                        // (pers sent only after the persona-screen Cross,
                        // quik only on the filter confirm).
                        if (qpNavNote() && len >= 4u)
                        {
                            const uint32_t t4 = (static_cast<uint32_t>(bytes[0]) << 24) |
                                                (static_cast<uint32_t>(bytes[1]) << 16) |
                                                (static_cast<uint32_t>(bytes[2]) << 8) |
                                                static_cast<uint32_t>(bytes[3]);
                            switch (t4)
                            {
                            case 0x70657273u: // pers
                                qpSetFirst(g_qpPers, tick);
                                break;
                            case 0x6F6E6C6Eu: // onln
                                qpSetFirst(g_qpOnln, tick);
                                break;
                            case 0x6E657773u: // news
                                qpSetFirst(g_qpNews, tick);
                                break;
                            case 0x50534554u: // PSET
                                qpSetFirst(g_qpPset, tick);
                                break;
                            case 0x52474554u: // RGET
                                qpSetFirst(g_qpRget, tick);
                                break;
                            case 0x7175696Bu: // quik
                                qpSetFirst(g_qpQuik, tick);
                                g_qpQuikCount.fetch_add(1u, std::memory_order_relaxed);
                                break;
                            default:
                                break;
                            }
                        }
#if PS2_NET_HAVE_POSIX_SOCKETS
                        const int fd = getSocket(w4);
                        if (fd >= 0)
                        {
                            uint32_t sent = 0u;
                            while (sent < len)
                            {
                                const ssize_t r = ::send(fd, bytes.data() + sent, len - sent, 0);
                                if (r <= 0)
                                {
                                    break;
                                }
                                sent += static_cast<uint32_t>(r);
                            }
                            status = sent;
                        }
#else
                        status = len;
#endif
                    }
                }
                else if (method == 17u)
                {
                    // sess/recv: a0 = handle, eeBuf/max = out. Hold
                    // first-seen data for a short guest-tick lag (loopback
                    // is faster than the app; instant delivery
                    // leaves the alarm stale and the pump skips
                    // the wake). v8.4 shape: status=0 (error code,
                    // 0 = success) + outlen=N (bytes); v8's
                    // status=N reads as fatal error 93.
                    // ONL4-P2: one EA message per delivery. The game
                    // accepts single-message deliveries (@dir-reply,
                    // addr-echo) but ignores a coalesced blob
                    // (auth-ok + rooms in one recv). Accumulate
                    // socket bytes; deliver the first complete
                    // message (12 B header + BE size) per call and
                    // hold the remainder for later polls. Partial
                    // messages wait for more bytes; a corrupt size
                    // falls back to whole-blob delivery (old shape).
                    const uint32_t eeBuf = w2 & 0x1FFFFFFFu;
                    status = 0u;
                    outlen = 0u;
#if PS2_NET_HAVE_POSIX_SOCKETS
                    {
                        auto hn = g_recvHold.find(w4);
                        const bool fresh = (hn == g_recvHold.end());
                        const size_t held = fresh ? 0u : hn->second.size();
                        const int fd = getSocket(w4);
                        // SEC1: stop reading at the cap; TCP backpressure
                        // (kernel-bounded socket buffers) holds the sender
                        // while the guest drains the hold via deliveries.
                        if (fd >= 0 && held < kRecvHoldCap)
                        {
                            uint8_t tmp[65536];
                            const ssize_t r = ::recv(fd, tmp, sizeof(tmp), 0);
                            if (r > 0)
                            {
                                if (fresh)
                                {
                                    g_recvHold[w4] = std::vector<uint8_t>(tmp, tmp + r);
                                    g_recvReady[w4] = tick + recvLagFor(w4, tick);
                                    std::cerr << " h=" << w4 << " n=" << r << " held"
                                              << " lag=" << recvLagFor(w4, tick);
                                }
                                else
                                {
                                    hn->second.insert(hn->second.end(), tmp, tmp + r);
                                }
                            }
                        }
                    }
                    auto hi = g_recvHold.find(w4);
                    if (hi != g_recvHold.end() && tick >= g_recvReady[w4] &&
                        hi->second.size() >= 12u)
                    {
                        const uint8_t *hd = hi->second.data();
                        const uint32_t size = (static_cast<uint32_t>(hd[8]) << 24) |
                                              (static_cast<uint32_t>(hd[9]) << 16) |
                                              (static_cast<uint32_t>(hd[10]) << 8) |
                                              static_cast<uint32_t>(hd[11]);
                        uint32_t n = 0u;
                        if (size >= 12u && size <= 65536u &&
                            hi->second.size() >= size)
                        {
                            n = size;
                        }
                        else if (size < 12u || size > 65536u)
                        {
                            n = static_cast<uint32_t>(hi->second.size());
                        }
                        if (n > 0u)
                        {
                            uint8_t *dst = (w3 > 0u) ? getMemPtr(rdram, eeBuf) : nullptr;
                            if (dst && n <= w3)
                            {
                                std::memcpy(dst, hd, n);
                                capture("tcp-rx", hostMillis(), tick, dst, n);
                                std::cerr << " h=" << w4 << " n=" << n << " lagged";
                                status = 0u;
                                outlen = n;
                                // LOB1: stash the +ses session token
                                // (if the server sent one) for relay
                                // registrations. The game ignores
                                // unknown +ses keys, so this changes no
                                // game-visible bytes. The token itself
                                // is never logged.
                                if (n > 20u && std::memcmp(dst, "+ses", 4) == 0 &&
                                    g_sesToken.empty())
                                {
                                    // Our lobby names the session token
                                    // SESSTOK (TOKEN accepted as a fallback).
                                    const char *body =
                                        reinterpret_cast<const char *>(dst + 12);
                                    const size_t bodyLen = static_cast<size_t>(n) - 12u;
                                    const char *keys[] = {"SESSTOK=", "TOKEN="};
                                    for (const char *key : keys)
                                    {
                                        const size_t klen = std::strlen(key);
                                        bool found = false;
                                        for (size_t i = 0u; i + klen + 1u <= bodyLen; ++i)
                                        {
                                            if ((i == 0u || body[i - 1u] == '\n') &&
                                                std::memcmp(body + i, key, klen) == 0)
                                            {
                                                size_t j = i + klen;
                                                while (j < bodyLen && body[j] != '\n' &&
                                                       body[j] != '\0' && (j - i) < 140u)
                                                {
                                                    ++j;
                                                }
                                                g_sesToken.assign(body + i + klen, j - i - klen);
                                                std::cerr << " [net] login: +ses token seen len="
                                                          << g_sesToken.size() << " tick=" << tick
                                                          << std::endl;
                                                found = true;
                                                break;
                                            }
                                        }
                                        if (found)
                                        {
                                            break;
                                        }
                                    }
                                }
                            }
                            hi->second.erase(hi->second.begin(), hi->second.begin() + n);
                        }
                        else if (loud)
                        {
                            std::cerr << " h=" << w4 << " holding";
                        }
                        if (hi->second.empty())
                        {
                            g_recvHold.erase(hi);
                            g_recvReady.erase(w4);
                        }
                    }
                    else if (loud)
                    {
                        std::cerr << " h=" << w4 << " holding";
                    }
#endif
                }
                else if (method == 33u)
                {
                    // gmlk/send: P2P/race bytes out; capture them.
                    const std::vector<uint8_t> bytes = dmaBytes(w0);
                    std::cerr << " n=" << bytes.size() << " hex=" << dmaHex(w0, 64u);
                    if (!bytes.empty())
                    {
                        capture("udp-tx", hostMillis(), tick, bytes.data(),
                                static_cast<uint32_t>(bytes.size()));
                    }
                    status = 0u;
                }
                else if (method == 34u)
                {
                    // gmlk/recv: no peer data yet.
                    status = 0u;
                }
                else if (method == 20u)
                {
                    // ping/requ: a0 = handle, a1 = target IP, a2 =
                    // payload addr (IOP), a3 = len. Record the
                    // request; ping/resp answers it 2 ticks later.
                    // (ONL4 s13: PONGs alone do NOT unblock the
                    // login, but ping-ok may still be necessary;
                    // answering a real outstanding requ is the
                    // case v8.2 never tested, so keep it.)
                    const std::vector<uint8_t> payload = dmaBytes(w0);
                    PingReq req;
                    req.payload = payload;
                    req.tick = tick;
                    req.target = w5;
                    g_pingReq[w4] = std::move(req);
                    if (loud)
                    {
                        std::cerr << " h=" << w4 << " target=0x" << std::hex << w5
                                  << std::dec << " n=" << payload.size();
                    }
                    if (!payload.empty())
                    {
                        capture("udp-ping-tx", hostMillis(), tick, payload.data(),
                                static_cast<uint32_t>(payload.size()));
                    }
                    status = 0u;
                }
                else if (method == 21u)
                {
                    // ping/resp: answer an outstanding requ (verbatim
                    // echo of the requ payload, so any cookie/seq the
                    // game planted comes back intact). No requ, or
                    // requ too recent (< 2 ticks, nonzero RTT): empty.
                    status = 0u;
                    auto pi = g_pingReq.find(w4);
                    if (pi != g_pingReq.end() && tick >= pi->second.tick + 2u &&
                        !pi->second.payload.empty())
                    {
                        const uint32_t eeBuf = w2 & 0x1FFFFFFFu;
                        uint8_t *pongDst = getMemPtr(rdram, eeBuf);
                        if (pongDst)
                        {
                            size_t n = pi->second.payload.size();
                            if (n > w3)
                            {
                                n = w3;
                            }
                            if (n > 0u)
                            {
                                std::memcpy(pongDst, pi->second.payload.data(), n);
                                outlen = static_cast<uint32_t>(n);
                            }
                        }
                        if (loud)
                        {
                            std::cerr << " h=" << w4 << " PONG n=" << outlen;
                        }
                        capture("udp-ping-rx", hostMillis(), tick,
                                pi->second.payload.data(),
                                static_cast<uint32_t>(pi->second.payload.size()));
                        g_pingReq.erase(pi);
                    }
                }
                if (method == 3u)
                {
                    g_linkUp = false;
                }
                if (loud)
                {
                    std::cerr << " -> status=" << status << " outlen=" << outlen
                              << " via=" << (fromRing ? "ring" : "dma") << std::endl;
                }
                writeReply(rdram, replyBase, i, w2, outlen, status, w7, st_dc, outlen);
                replied = true;
            }
            else if (w6 != 0u)
            {
                std::cerr << "[net] UNKNOWN handler tick=" << tick << " slot=" << i
                          << " h=0x" << std::hex << w6 << std::dec << std::endl;
            }
        }
        return replied;
    }

    // Process one doorbell (flags = sifbuf addr or 0xFFFFFFFF). Logs the
    // EE struct, then scans; with the DMA sweep most slots are already
    // dispatched and show as repeats. Returns true when any reply was
    // written (by either path this scan).
    bool processDoorbell(uint8_t *rdram, uint32_t flags, uint64_t tick)
    {
        (void)flags;
        return scanAndDispatch(rdram, tick, true);
    }

    void noteDma(uint32_t src, uint32_t dst, uint32_t size, uint8_t *rdram)
    {
        if (!enabled() || !rdram)
        {
            return;
        }
        if (dst < 0xA4000000u || dst >= 0xA5000000u || size == 0u || size > 65536u)
        {
            return;
        }
        std::vector<uint8_t> bytes;
        bytes.reserve(size);
        for (uint32_t i = 0; i < size; ++i)
        {
            const uint8_t *ptr = getConstMemPtr(rdram, src + i);
            if (!ptr)
            {
                break;
            }
            bytes.push_back(*ptr);
        }
        {
            std::lock_guard<std::mutex> lock(g_dmaMutex);
            if (g_dmaPayloads.size() > 256u)
            {
                g_dmaPayloads.clear();
            }
            g_dmaPayloads[dst] = std::move(bytes);
        }
        // ONL4 sweep: dispatch newly queued slots at DMA time. The
        // payload is recorded first (above), so handlers see complete
        // payloads; the shared fingerprint dedup keeps the later ring
        // from re-running them. Pre-init (slotBase 0) sweeps no-op.
        const uint32_t slotBase = readEeU32(rdram, kEeStateAddr + kFSlotBase) & 0x1FFFFFFFu;
        if (slotBase != 0u)
        {
            scanAndDispatch(rdram, ps2_e41_trace::lastVsyncTick(), false);
        }
    }

    std::string dmaString(uint32_t dst)
    {
        std::lock_guard<std::mutex> lock(g_dmaMutex);
        auto it = g_dmaPayloads.find(dst);
        if (it == g_dmaPayloads.end() || it->second.empty())
        {
            return std::string();
        }
        const uint8_t *data = it->second.data();
        size_t len = 0;
        while (len < it->second.size() && data[len] != 0u)
        {
            ++len;
        }
        return std::string(reinterpret_cast<const char *>(data), len);
    }

    std::string dmaHex(uint32_t dst, uint32_t maxBytes)
    {
        std::lock_guard<std::mutex> lock(g_dmaMutex);
        auto it = g_dmaPayloads.find(dst);
        if (it == g_dmaPayloads.end() || it->second.empty())
        {
            return std::string();
        }
        std::string out;
        char item[4] = {};
        const size_t n = (it->second.size() < maxBytes) ? it->second.size() : maxBytes;
        for (size_t i = 0; i < n; ++i)
        {
            std::snprintf(item, sizeof(item), "%02X", it->second[i]);
            if (!out.empty())
            {
                out.push_back(' ');
            }
            out += item;
        }
        if (it->second.size() > maxBytes)
        {
            out += " ...";
        }
        return out;
    }

    std::vector<uint8_t> dmaBytes(uint32_t dst)
    {
        std::lock_guard<std::mutex> lock(g_dmaMutex);
        auto it = g_dmaPayloads.find(dst);
        if (it == g_dmaPayloads.end())
        {
            return {};
        }
        return it->second;
    }

    uint64_t hostMillis()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }

    bool onSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                   uint32_t cid, uint32_t packetAddr, uint32_t packetSize,
                   uint32_t srcExtra, uint32_t dstExtra, uint32_t sizeExtra,
                   uint64_t tick)
    {
        (void)srcExtra;
        (void)dstExtra;
        (void)sizeExtra;
        if (!enabled() || cid != kDirtySockCid || !rdram)
        {
            return false;
        }
        applyOneTimePatches(rdram, runtime);

        if (packetSize < 16u || packetAddr == 0u)
        {
            std::cerr << "[net] cid=1 short packet size=" << packetSize << std::endl;
            return false;
        }
        bool ok = false;
        const uint32_t flags = readU32(rdram, packetAddr + 12u, ok);
        if (!ok)
        {
            std::cerr << "[net] cid=1 unreadable packet" << std::endl;
            return false;
        }

        // Doorbell: flags = sifbuf addr (process the queue) or 0xFFFFFFFF
        // (wake; same handling: the queue is the source of truth).
        // RpcWakeup checks -1 FIRST, so -1 is a doorbell even though bit0
        // is set.
        if (flags == 0xFFFFFFFFu || (flags & 1u) == 0u)
        {
            return processDoorbell(rdram, flags, tick);
        }

        // Version poll: IOP DMAs "RPC IOP Ver 1.0 " to flags&~1. Only EE
        // RAM targets are honored (RpcWakeup would DMA anywhere, but the
        // game only polls RAM; anything else is a protocol error).
        {
            const uint32_t eeAddr = (flags & ~1u) & 0x1FFFFFFFu;
            if (eeAddr >= 32u * 1024u * 1024u)
            {
                std::cerr << "[net] poll reply addr out of range 0x" << std::hex << flags
                          << std::dec << std::endl;
                return false;
            }
            uint8_t *dst = getMemPtr(rdram, eeAddr);
            if (!dst)
            {
                std::cerr << "[net] poll reply addr unmapped 0x" << std::hex << eeAddr
                          << std::dec << std::endl;
                return false;
            }
            std::memcpy(dst, kIopVersion, sizeof(kIopVersion));
            std::cerr << "[net] poll reply tick=" << tick << " -> 0x" << std::hex << eeAddr
                      << std::dec << std::endl;
            return true;
        }
    }
} // namespace ps2_net
