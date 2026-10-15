#include "MiniTest.h"
#include "ps2_np_rider.h"
#include "ps2_mtvu.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_headless_backend.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_savestate.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace
{
// Canonical-transcript scratch space. The parent test creates a unique
// directory per test under the platform temp root (overridable with
// PS2X_NP_FIX_WORKROOT) and publishes it to re-exec children through the
// environment, since fork+exec children cannot share test-local state.
// Nothing under a lane or checkout directory is ever touched.
std::string workRoot()
{
    if (const char *v = std::getenv("PS2X_NP_FIX_WORKROOT"))
        return std::string(v);
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec)
        base = std::filesystem::path("/tmp");
    return (base / "ps2x-np-fix").string();
}

#if defined(__unix__) || defined(__APPLE__)
std::string makeUniqueWorkRoot()
{
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec)
        base = std::filesystem::path("/tmp");
    std::string tmpl = (base / "ps2x-np-fix-XXXXXX").string();
    if (::mkdtemp(tmpl.data()) == nullptr)
        return (base / "ps2x-np-fix-fallback").string();
    return tmpl;
}
#endif

struct EnvGuard
{
    std::string name, old;
    bool had = false;
    explicit EnvGuard(const char *n) : name(n)
    {
        if (const char *v = std::getenv(n))
        {
            old = v;
            had = true;
        }
    }
    void set(const char *v) { ::setenv(name.c_str(), v, 1); }
    void unset() { ::unsetenv(name.c_str()); }
    ~EnvGuard()
    {
        if (had)
            ::setenv(name.c_str(), old.c_str(), 1);
        else
            ::unsetenv(name.c_str());
    }
};

#if defined(__unix__) || defined(__APPLE__)
// fork+exec isolation: CANON statics (enabled cache, trace file) are
// process-global and order-sensitive, so CANON tests re-exec the suite
// binary with --np-fix-child <name> for truly fresh state. This also avoids
// fork-with-threads hazards (exec clears the thread list).
std::string selfExePath()
{
#if defined(__APPLE__)
    uint32_t size = 0u;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0)
    {
        buf.resize(std::strlen(buf.c_str()));
        return buf;
    }
    return {};
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1u);
    if (n <= 0)
        return {};
    buf[n] = '\0';
    return std::string(buf);
#endif
}

int runChild(const char *name)
{
    const std::string self = selfExePath();
    if (self.empty())
        return 1002;
    pid_t pid = ::fork();
    if (pid < 0)
        return 1000;
    if (pid == 0)
    {
        ::execl(self.c_str(), self.c_str(), "--np-fix-child", name, nullptr);
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
    if (!WIFEXITED(status))
        return 1001;
    return WEXITSTATUS(status);
}
#endif

std::string readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> splitLines(const std::string &s)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < s.size())
    {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos)
        {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

std::vector<std::string> splitTabs(const std::string &s)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (true)
    {
        size_t t = s.find('\t', pos);
        if (t == std::string::npos)
        {
            out.push_back(s.substr(pos));
            return out;
        }
        out.push_back(s.substr(pos, t - pos));
        pos = t + 1;
    }
}

void appendU64(std::vector<uint8_t> &dst, uint64_t v)
{
    const size_t pos = dst.size();
    dst.resize(pos + 8u);
    std::memcpy(dst.data() + pos, &v, 8u);
}

void appendGifTag(std::vector<uint8_t> &dst, uint16_t nloop, uint8_t flg, uint8_t nreg, uint64_t regs)
{
    uint64_t tag = static_cast<uint64_t>(nloop & 0x7FFFu) | (1ull << 15);
    tag |= (static_cast<uint64_t>(flg & 0x3u) << 58);
    tag |= (static_cast<uint64_t>(nreg & 0xFu) << 60);
    appendU64(dst, tag);
    appendU64(dst, regs);
}

void appendGifAd(std::vector<uint8_t> &dst, uint64_t value, uint64_t reg)
{
    appendU64(dst, value);
    appendU64(dst, reg);
}

// ---------------------------------------------------------------- F2 child
#if defined(__unix__) || defined(__APPLE__)
int f2Child()
{
    ::setenv("PS2X_NETPLAY_CANON", "1", 1);
    ::setenv("PS2X_NETPLAY_CANON_DIR", (workRoot() + "/f2").c_str(), 1);
    auto be = ps2x_gs_headless::create();
    if (!be)
        return 20;
    uint8_t vram[64] = {};
    be->Initialize(vram, sizeof(vram));
    uint8_t dst[64];
    std::memset(dst, 0xA5, sizeof(dst));
    const uint32_t n = be->ConsumeLocalToHostBytes(dst, sizeof(dst));
    if (n != sizeof(dst))
        return 10; // B0: headless serves 0 bytes (success-with-0 at the HLE layer)
    for (uint8_t b : dst)
        if (b != 0xFFu)
            return 11;
    return 0;
}
#endif

// ---------------------------------------------------------------- F12 children
#if defined(__unix__) || defined(__APPLE__)
int f12EventChild()
{
    ::setenv("PS2X_NETPLAY_CANON", "1", 1);
    ::setenv("PS2X_NETPLAY_CANON_DIR", (workRoot() + "/e").c_str(), 1);
    ps2_savestate::canonicalRecordEvent(120u, 999u, 3u, 7u, 555u, 4242u, 5u);
    return 0;
}

int f12RiderChild()
{
    ::setenv("PS2X_NETPLAY_CANON", "1", 1);
    ::setenv("PS2X_NETPLAY_CANON_DIR", (workRoot() + "/d").c_str(), 1);
    ps2_savestate::canonicalRecordRider(120u, 1u, 0u, 5u, "applied", true);
    ps2_savestate::canonicalRecordRider(120u, 1u, 1u, 5u, "applied", true);
    return 0;
}

int f12SessionChild()
{
    ::setenv("PS2X_NETPLAY_CANON", "1", 1);
    ::setenv("PS2X_NETPLAY_CANON_DIR", (workRoot() + "/s1").c_str(), 1);
    ps2_savestate::canonicalRecordReadback(100u, ps2_savestate::canonicalNextReadbackTicket(),
                                            0x10u, 4u, 0x1234u);
    ps2_savestate::canonicalNewSession();
    ::setenv("PS2X_NETPLAY_CANON_DIR", (workRoot() + "/s2").c_str(), 1);
    ps2_savestate::canonicalRecordReadback(100u, ps2_savestate::canonicalNextReadbackTicket(),
                                            0x10u, 4u, 0x1234u);
    return 0;
}
#endif

void register_np_fix_tests_body(TestCase &tc)
{
    // F2: headless must never succeed with 0 bytes under CANON.
    tc.Run("F2 headless consume serves canonical bytes under CANON", [](TestCase &t)
           {
#if defined(__unix__) || defined(__APPLE__)
               EnvGuard root("PS2X_NP_FIX_WORKROOT");
               root.set(makeUniqueWorkRoot().c_str());
               const int rc = runChild("f2");
               t.Equals(rc, 0, "headless consume returns want-bytes of 0xff under CANON");
               std::error_code ecF2;
               std::filesystem::remove_all(workRoot(), ecF2);
#else
            std::puts("[np-fix] SKIP F2: fork unavailable on this platform");
#endif
           });

    // F4: CANON establishes its own input path (fresh env reads, no globals).
    tc.Run("F4 wall-clock pad script is rejected under CANON", [](TestCase &t)
           {
               EnvGuard canon("PS2X_NETPLAY_CANON"), script("PS2X_PAD_SCRIPT"), clock("PS2X_PAD_SCRIPT_CLOCK");
               canon.set("1");
               script.set("tick 100 cross");
               clock.unset();
               std::string why;
               t.IsFalse(ps2_savestate::canonicalInputProfileOk(why), "wall-clock script must not validate");
               t.IsTrue(why.find("CLOCK") != std::string::npos || why.find("vsync") != std::string::npos,
                        "rejection names the clock");
               clock.set("vsync");
               std::string why2;
               t.IsTrue(ps2_savestate::canonicalInputProfileOk(why2), "vsync script validates");
           });

    // F3: SIGNAL W1C clear is ordered after GS-worker decode under threaded MTVU.
    tc.Run("F3 threaded CSR W1C clears a queued SIGNAL deterministically", [](TestCase &t)
           {
               ps2_mtvu::setModeForTest(ps2_mtvu::Mode::Threaded, false, 0u);
               PS2Memory mem;
               t.IsTrue(mem.initialize(), "memory initializes");
               std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
               GS gs;
               gs.init(vram.data(), static_cast<uint32_t>(vram.size()), &mem.gs());
               gs.setQueueEnabled(true);
               mem.setGsFrontend(&gs);
               // Flood the worker with safe 128x64 uploads, then the SIGNAL.
               for (int i = 0; i < 64; ++i)
               {
                   std::vector<uint8_t> pkt;
                   const uint64_t bitbltbuf = (static_cast<uint64_t>(0u) << 32) |
                                              (static_cast<uint64_t>(1u) << 48) |
                                              (static_cast<uint64_t>(GS_PSM_CT32) << 56);
                   appendGifTag(pkt, 4u, 0u, 1u, 0xEull);
                   appendGifAd(pkt, bitbltbuf, GS_REG_BITBLTBUF);
                   appendGifAd(pkt, 0ull, GS_REG_TRXPOS);
                   appendGifAd(pkt, static_cast<uint64_t>(128u) | (static_cast<uint64_t>(64u) << 32),
                               GS_REG_TRXREG);
                   appendGifAd(pkt, 0ull, GS_REG_TRXDIR);
                   const uint32_t payload = 128u * 64u * 4u;
                   appendGifTag(pkt, static_cast<uint16_t>(payload / 16u), 2u, 0u, 0ull);
                   const size_t base = pkt.size();
                   pkt.resize(base + payload, 0x51u);
                   gs.noteGifPath(GifPathId::Path3);
                   gs.processGIFPacket(pkt.data(), static_cast<uint32_t>(pkt.size()));
               }
               std::vector<uint8_t> sig;
               appendGifTag(sig, 1u, 0u, 1u, 0xEull);
               appendGifAd(sig, 0x11ull | (0xFFull << 32), GS_REG_SIGNAL);
               gs.noteGifPath(GifPathId::Path2);
               gs.processGIFPacket(sig.data(), static_cast<uint32_t>(sig.size()));
               // EE clears SIGNAL immediately (worker still on the backlog).
               mem.write32(0x12001000u, 1u, 0u);
               gs.drainQueue(); // settle the worker so the assert is deterministic
               const uint64_t csr = mem.gs().csr.load(std::memory_order_relaxed);
               t.Equals(csr & 1u, 0u, "SIGNAL bit is clear after ordered W1C");
               ps2_mtvu::setModeForTest(ps2_mtvu::Mode::Off, false, 0u);
           });

    // F7: rider-script strictness.
    tc.Run("F7 malformed lines are reported, not silently skipped", [](TestCase &t)
           {
               std::vector<ps2_np_rider::Entry> entries;
               std::string error;
               ps2_np_rider::parseScriptText("100 0 11111111 22222222\nTHIS IS NOT A SCRIPT LINE\n",
                                             entries, error);
               t.IsTrue(!error.empty(), "malformed line must produce an error");
           });

    tc.Run("F7 conflicting duplicate ticks are rejected", [](TestCase &t)
           {
               std::vector<ps2_np_rider::Entry> entries;
               std::string error;
               ps2_np_rider::parseScriptText("100 0 aaaaaaaa bbbbbbbb\n100 1 cccccccc dddddddd\n",
                                             entries, error);
               t.IsTrue(!error.empty(), "conflicting duplicate tick must produce an error");
           });

    tc.Run("F7 overlong lines are rejected", [](TestCase &t)
           {
               std::vector<ps2_np_rider::Entry> entries;
               std::string error;
               ps2_np_rider::parseScriptText(std::string(300, 'A'), entries, error);
               t.IsTrue(!error.empty(), "overlong line must produce an error");
           });

    tc.Run("F7 a load error refuses every apply", [](TestCase &t)
           {
               ps2_np_rider::Config cfg;
               cfg.enabled = true;
               cfg.slot = 1;
               cfg.loadError = true;
               cfg.script.push_back(ps2_np_rider::Entry{100u, 0u, 0x11111111u, 0x22222222u});
               ps2_np_rider::Session s;
               const ps2_np_rider::Decision d =
                   ps2_np_rider::decide(s, cfg, 100u, true, 0x1000u, 0u, true, 0u, 0u);
               t.IsFalse(d.applied, "load error must refuse applies");
           });

    // F8: per-actor binding + per-call consumption contract.
    tc.Run("F8 a foreign actor in the same session is refused", [](TestCase &t)
           {
               ps2_np_rider::Config cfg;
               cfg.enabled = true;
               cfg.slot = 1;
               cfg.script.push_back(ps2_np_rider::Entry{100u, 0u, 0x11111111u, 0x22222222u});
               ps2_np_rider::Session s;
               const ps2_np_rider::Decision d1 =
                   ps2_np_rider::decide(s, cfg, 100u, true, 0x1000u, 0u, true, 0u, 0u);
               t.IsTrue(d1.applied, "bound actor applies");
               const ps2_np_rider::Decision d2 =
                   ps2_np_rider::decide(s, cfg, 100u, true, 0x2000u, 0u, true, 0u, 0u);
               t.IsFalse(d2.applied, "foreign actor refused in the same session");
           });

    tc.Run("F8 a backward tick re-derives the cursor", [](TestCase &t)
           {
               ps2_np_rider::Config cfg;
               cfg.enabled = true;
               cfg.slot = 1;
               cfg.script.push_back(ps2_np_rider::Entry{100u, 0u, 0x11111111u, 0x22222222u});
               cfg.script.push_back(ps2_np_rider::Entry{200u, 0u, 0x33333333u, 0x44444444u});
               ps2_np_rider::Session s;
               const ps2_np_rider::Decision d1 =
                   ps2_np_rider::decide(s, cfg, 200u, true, 0x1000u, 0u, true, 0u, 0u);
               t.IsTrue(d1.applied, "later tick applies");
               const ps2_np_rider::Decision d2 =
                   ps2_np_rider::decide(s, cfg, 100u, true, 0x1000u, 0u, true, 0u, 0u);
               t.IsTrue(d2.applied, "backward tick re-resolves its entry");
               t.Equals(d2.outW0, 0x11111111u, "rewound entry carries its own words");
           });

    tc.Run("F8 same-tick calls share words with distinct ordinals", [](TestCase &t)
           {
               ps2_np_rider::Config cfg;
               cfg.enabled = true;
               cfg.slot = 1;
               cfg.script.push_back(ps2_np_rider::Entry{100u, 0u, 0x11111111u, 0x22222222u});
               ps2_np_rider::Session s;
               const ps2_np_rider::Decision d1 =
                   ps2_np_rider::decide(s, cfg, 100u, true, 0x1000u, 0u, true, 0u, 0u);
               const ps2_np_rider::Decision d2 =
                   ps2_np_rider::decide(s, cfg, 100u, true, 0x1000u, 0u, true, 0u, 0u);
               t.IsTrue(d1.applied && d2.applied, "same-tick re-apply is idempotent");
               t.Equals(d1.outW0, d2.outW0, "same words on the repeated call");
               t.Equals(d1.ordinal, 0u, "first call ordinal 0");
               t.Equals(d2.ordinal, 1u, "second call ordinal 1");
           });

    // F12: fail-closed transcript.
    tc.Run("F12 event rows carry the event value", [](TestCase &t)
           {
#if defined(__unix__) || defined(__APPLE__)
               EnvGuard root("PS2X_NP_FIX_WORKROOT");
               root.set(makeUniqueWorkRoot().c_str());
               std::error_code ec;
               std::filesystem::remove_all(workRoot() + "/e", ec);
               const int rc = runChild("e");
               t.Equals(rc, 0, "event child runs");
               bool found = false;
               for (const std::string &line : splitLines(readFile(workRoot() + "/e/transcript.tsv")))
               {
                   if (line.rfind("E\t", 0) != 0)
                       continue;
                   found = true;
                   // E tick ee type id value deadline seq (value appended).
                   t.Equals(splitTabs(line).size(), size_t{8}, "event row has the value column");
               }
               t.IsTrue(found, "an event row was recorded");
               std::filesystem::remove_all(workRoot(), ec);
#else
            std::puts("[np-fix] SKIP F12 event rows: fork unavailable on this platform");
#endif
           });

    tc.Run("F12 rider decisions are recorded in the transcript", [](TestCase &t)
           {
#if defined(__unix__) || defined(__APPLE__)
               EnvGuard root("PS2X_NP_FIX_WORKROOT");
               root.set(makeUniqueWorkRoot().c_str());
               std::error_code ec;
               std::filesystem::remove_all(workRoot() + "/d", ec);
               const int rc = runChild("d");
               t.Equals(rc, 0, "rider child runs");
               size_t decisions = 0;
               for (const std::string &line : splitLines(readFile(workRoot() + "/d/transcript.tsv")))
                   if (line.rfind("D\t", 0) == 0)
                       ++decisions;
               t.Equals(decisions, size_t{2}, "both rider decisions recorded");
               std::filesystem::remove_all(workRoot(), ec);
#else
            std::puts("[np-fix] SKIP F12 rider decisions: fork unavailable on this platform");
#endif
           });

    tc.Run("F12 a new session retargets the trace and restarts tickets", [](TestCase &t)
           {
#if defined(__unix__) || defined(__APPLE__)
               EnvGuard root("PS2X_NP_FIX_WORKROOT");
               root.set(makeUniqueWorkRoot().c_str());
               std::error_code ec;
               std::filesystem::remove_all(workRoot() + "/s1", ec);
               std::filesystem::remove_all(workRoot() + "/s2", ec);
               const int rc = runChild("s");
               t.Equals(rc, 0, "session child runs");
               const std::string s2 = readFile(workRoot() + "/s2/transcript.tsv");
               bool found = false;
               for (const std::string &line : splitLines(s2))
               {
                   if (line.rfind("R\t", 0) != 0)
                       continue;
                   found = true;
                   const std::vector<std::string> f = splitTabs(line);
                   t.Equals(f.size(), size_t{6}, "readback row shape");
                   if (f.size() == 6u)
                       t.Equals(f[2], std::string("0"), "ticket restarts in the new session");
               }
               t.IsTrue(found, "second session wrote its own transcript");
               std::filesystem::remove_all(workRoot(), ec);
#else
            std::puts("[np-fix] SKIP F12 session retarget: fork unavailable on this platform");
#endif
           });
}
} // namespace

void register_np_fix_tests()
{
    MiniTest::Case("ZzNpFix", register_np_fix_tests_body);
}

int np_fix_child_main(const std::string &name)
{
#if defined(__unix__) || defined(__APPLE__)
    int rc = 99;
    if (name == "f2")
        rc = f2Child();
    else if (name == "e")
        rc = f12EventChild();
    else if (name == "d")
        rc = f12RiderChild();
    else if (name == "s")
        rc = f12SessionChild();
    std::fflush(nullptr);
    return rc;
#else
    (void)name;
    return 99;
#endif
}
