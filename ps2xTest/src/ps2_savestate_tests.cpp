#include "MiniTest.h"
#include "ps2_microvu.h"
#include "ps2_runtime.h"
#include "ps2_ssx3_patch_grow_state.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_savestate.h"
#include "runtime/ps2_vfs.h"
#include "runtime/ps2_rom_device.h"
#include "../../ps2xRuntime/src/lib/Kernel/Stubs/MemoryCard.h"
#include "../../ps2xRuntime/src/lib/Kernel/Stubs/SIF.h"
#include "../../ps2xRuntime/src/lib/ps2_savestate_internal.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using ps2_savestate::Reader;
    using ps2_savestate::Writer;

    template <typename Map>
    std::vector<typename Map::key_type> order(const Map &m)
    {
        std::vector<typename Map::key_type> keys;
        for (const auto &e : m)
        {
            if constexpr (std::is_same_v<typename Map::key_type, typename Map::value_type>)
                keys.push_back(e);
            else
                keys.push_back(e.first);
        }
        return keys;
    }

    std::vector<uint8_t> saveScheduler(const EeScheduler &ee)
    {
        Writer w;
        EeSchedulerSavestate::save(ee, w);
        return w.buf;
    }

    std::string tmpPath(const char *name)
    {
        return (std::filesystem::temp_directory_path() / name).string();
    }

    void writeFile(const std::string &path, const std::vector<uint8_t> &bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // S4: point the card stubs at a scratch root, restored on scope exit.
    struct Ss4IoPathsGuard
    {
        PS2Runtime::IoPaths saved;
        explicit Ss4IoPathsGuard(const std::filesystem::path &mcRoot) : saved(PS2Runtime::getIoPaths())
        {
            PS2Runtime::IoPaths io;
            io.mcRoot = mcRoot;
            PS2Runtime::setIoPaths(io);
        }
        ~Ss4IoPathsGuard() { PS2Runtime::setIoPaths(saved); }
    };

    void ss4SetReg(R5900Context &ctx, int reg, uint32_t v)
    {
        std::memcpy(&ctx.r[reg], &v, sizeof(v)); // low word, as getRegU32 reads
    }

    // Runs the real sceMcGetDir and returns the raw dir table (maxEnt x 64B).
    std::vector<uint8_t> ss4GetDir(PS2Runtime &rt, const std::string &guestPath, int maxEnt)
    {
        std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
        R5900Context ctx{};
        constexpr uint32_t kPathAddr = 0x1000, kTableAddr = 0x2000;
        std::memcpy(rdram.data() + kPathAddr, guestPath.c_str(), guestPath.size() + 1);
        ss4SetReg(ctx, 4, 0); // port 0
        ss4SetReg(ctx, 5, 0); // slot 0
        ss4SetReg(ctx, 6, kPathAddr); // path
        ss4SetReg(ctx, 8, static_cast<uint32_t>(maxEnt)); // max entries
        ss4SetReg(ctx, 9, kTableAddr); // table
        ps2_stubs::sceMcGetDir(rdram.data(), &ctx, &rt);
        return std::vector<uint8_t>(rdram.begin() + kTableAddr, rdram.begin() + kTableAddr + maxEnt * 64);
    }

    std::filesystem::file_time_type ss4FileTime(int64_t sec, int64_t ns)
    {
        namespace fs = std::filesystem;
        return fs::file_time_type{} + std::chrono::seconds(sec) + std::chrono::nanoseconds(ns);
    }

} // namespace

void register_ps2_savestate_tests()
{
    MiniTest::Case("Ps2Savestate", [](TestCase &tc)
    {
        tc.Run("HMS2: TK34 grow map survives cold restore and pre-grow rewind", [](TestCase &t)
        {
            PS2Runtime runtime;
            Writer before, grown;
            PS2RuntimeSavestate::saveKernel(runtime, before);

            ps2_ssx3_patch_grow::State state;
            state.active = true;
            state.cache = 0x00560000u;
            state.moved = {{0x01F33400u, 0x00570000u}, {0x01F34000u, 0x00571000u}};
            t.IsTrue(ps2_ssx3_patch_grow::restore(state), "test grow map accepted");
            PS2RuntimeSavestate::saveKernel(runtime, grown);

            ps2_ssx3_patch_grow::reset(); // cold process has no map
            Reader cold(grown.buf.data(), grown.buf.size());
            t.IsTrue(PS2RuntimeSavestate::loadKernel(runtime, cold), "cold load restores grow map");
            const auto restored = ps2_ssx3_patch_grow::snapshot();
            t.IsTrue(restored.active && restored.cache == state.cache && restored.moved == state.moved,
                     "course exit can translate both relocated frees");

            Reader rewind(before.buf.data(), before.buf.size());
            t.IsTrue(PS2RuntimeSavestate::loadKernel(runtime, rewind), "pre-grow rewind loads");
            t.IsTrue(!ps2_ssx3_patch_grow::snapshot().active &&
                     ps2_ssx3_patch_grow::snapshot().moved.empty(), "rewind removes future map");
            t.IsTrue(ps2_ssx3_patch_grow::restore(state), "later grow can own the same relocation anew");
            ps2_ssx3_patch_grow::State refused;
            refused.refused = true;
            t.IsTrue(ps2_ssx3_patch_grow::restore(refused), "refused state accepted");
            Writer refusedBytes;
            PS2RuntimeSavestate::saveKernel(runtime, refusedBytes);
            ps2_ssx3_patch_grow::reset();
            Reader refusedLoad(refusedBytes.buf.data(), refusedBytes.buf.size());
            t.IsTrue(PS2RuntimeSavestate::loadKernel(runtime, refusedLoad) &&
                     ps2_ssx3_patch_grow::snapshot().refused, "refusal survives a cold load");
            ps2_ssx3_patch_grow::reset();
        });

        tc.Run("writer/reader round trip and bounds", [](TestCase &t)
        {
            Writer w;
            size_t mark = w.beginSection("alpha", 3u);
            w.u32(0xDEADBEEFu);
            w.str("hello");
            w.blob({1, 2, 3});
            w.b(true);
            w.endSection(mark);
            mark = w.beginSection("beta", 1u);
            w.u64(42u);
            w.endSection(mark);

            Reader r(w.buf.data(), w.buf.size());
            std::string key;
            uint32_t version = 0;
            t.IsTrue(r.beginSection(key, version), "first section frame");
            t.Equals(key, std::string("alpha"), "key");
            t.Equals(version, 3u, "version");
            t.Equals(r.u32(), 0xDEADBEEFu, "u32");
            t.Equals(r.str(), std::string("hello"), "str");
            t.Equals(r.blob().size(), size_t{3}, "blob");
            t.IsTrue(r.b(), "bool");
            t.IsTrue(r.endSection(key), "section consumed");
            t.IsTrue(r.beginSection(key, version), "second section frame");
            t.IsTrue(!r.endSection(key), "unconsumed section is refused");

            Reader bounded(w.buf.data(), w.buf.size());
            bounded.beginSection(key, version);
            bounded.u32();
            bounded.str();
            bounded.blob();
            bounded.b();
            (void)bounded.u64(); // past the section end
            t.IsTrue(!bounded.ok(), "a read past the section end fails");

            Reader truncated(w.buf.data(), 6);
            truncated.beginSection(key, version);
            t.IsTrue(!truncated.ok(), "truncated frame fails");
        });

        tc.Run("unordered_map round trip keeps iteration order and later inserts", [](TestCase &t)
        {
            std::mt19937 rng(1234u);
            std::unordered_map<int, uint32_t> a;
            for (int step = 0; step < 4000; ++step)
            {
                const int key = static_cast<int>(rng() % 700u) - 350;
                if (rng() % 3u == 0u)
                    a.erase(key);
                else
                    a[key] = rng();
            }
            Writer w;
            ps2_savestate::writeOrderedPod(w, a);
            std::unordered_map<int, uint32_t> b;
            b[99999] = 1u; // restore replaces existing content
            Reader r(w.buf.data(), w.buf.size());
            t.IsTrue(ps2_savestate::readOrderedPod(r, b), "read back");
            t.Equals(b.bucket_count(), a.bucket_count(), "bucket count");
            t.IsTrue(order(a) == order(b), "iteration order identical");
            t.IsTrue(a == b, "contents identical");
            for (int step = 0; step < 3000; ++step)
            {
                const int key = static_cast<int>(rng() % 900u) - 450;
                const uint32_t value = rng();
                if (value % 4u == 0u)
                {
                    a.erase(key);
                    b.erase(key);
                }
                else
                {
                    a[key] = value;
                    b[key] = value;
                }
            }
            t.Equals(b.bucket_count(), a.bucket_count(), "bucket count after more history");
            t.IsTrue(order(a) == order(b), "iteration order identical after more history (incl. rehash)");

            std::unordered_set<uint32_t> sa;
            for (int i = 0; i < 500; ++i)
                sa.insert(rng() % 2000u);
            Writer ws;
            ps2_savestate::writeOrdered(ws, sa, [](Writer &ww, uint32_t v) { ww.u32(v); });
            std::unordered_set<uint32_t> sb;
            Reader rs(ws.buf.data(), ws.buf.size());
            t.IsTrue(ps2_savestate::readOrdered(rs, sb, [](Reader &rr, uint32_t &v) { v = rr.u32(); }), "set read back");
            t.IsTrue(order(sa) == order(sb), "set iteration order identical");
        });

        tc.Run("ordered-map round trip covers empty and string-keyed maps", [](TestCase &t)
        {
            std::unordered_map<int, uint32_t> empty;
            Writer w;
            ps2_savestate::writeOrderedPod(w, empty);
            std::unordered_map<int, uint32_t> emptyBack;
            emptyBack[1] = 2u; // restore replaces existing content
            Reader r(w.buf.data(), w.buf.size());
            t.IsTrue(ps2_savestate::readOrderedPod(r, emptyBack), "empty map reads back");
            t.IsTrue(emptyBack.empty(), "restored map is empty");

            std::unordered_map<std::string, int32_t> a;
            for (int i = 0; i < 50; ++i)
                a["mod/path_" + std::to_string(i * 7 % 50)] = i;
            Writer ws;
            ps2_savestate::writeOrdered(ws, a, [](Writer &ww, const auto &e) {
                ww.str(e.first);
                ww.pod(e.second);
            });
            std::unordered_map<std::string, int32_t> b;
            Reader rs(ws.buf.data(), ws.buf.size());
            t.IsTrue(ps2_savestate::readOrdered(rs, b, [](Reader &rr, auto &e) {
                e.first = rr.str();
                rr.pod(e.second);
            }),
                     "string-keyed map reads back");
            t.IsTrue(order(a) == order(b), "string-keyed iteration order identical");
            t.IsTrue(a == b, "string-keyed contents identical");
        });

        tc.Run("sha256 and pad-script prefix", [](TestCase &t)
        {
            const std::string abc = "abc";
            t.Equals(ps2_savestate::sha256Hex(reinterpret_cast<const uint8_t *>(abc.data()), abc.size()),
                     std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "FIPS 180-2 abc");
            const std::string empty;
            t.Equals(ps2_savestate::sha256Hex(reinterpret_cast<const uint8_t *>(empty.data()), 0),
                     std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "empty");
            // tick 1000 = 16683 ms on the vsync clock.
            const char *script = "100:start:250,16683:cross:250,16684:down:150,30000:cross:200";
            t.Equals(ps2_savestate::padScriptPrefix(script, 1000u), std::string("100:start:250,16683:cross:250"),
                     "entries at or before the save tick");
            t.Equals(ps2_savestate::padScriptPrefix(script, 0u), std::string(), "nothing before tick 0 except t=0");
            t.Equals(ps2_savestate::padScriptPrefix("100:start:250,16683:cross:250,99999:x:1", 1000u),
                     ps2_savestate::padScriptPrefix(script, 1000u), "a different route after the save tick matches");
        });

        tc.Run("scheduler round trip is byte-identical (threads, waits, objects, deadlines)", [](TestCase &t)
        {
            ps2_savestate::setSteadyNowForTests(1'000'000'000'000);
            std::vector<uint8_t> rdramA(PS2_RAM_SIZE, 0), rdramB(PS2_RAM_SIZE, 0);
            R5900Context ctxA{}, ctxB{};
            PS2Runtime rtA;
            PS2Runtime rtB;
            EeScheduler &a = rtA.eeScheduler();
            a.reset(rdramA.data(), ctxA);
            a.bindMainContextForSyscall(ctxA, rdramA.data());
            for (int i = 0; i < 6; ++i)
            {
                EeThreadCreateParams p{};
                p.entry = 0x100000u + 0x100u * static_cast<uint32_t>(i);
                p.stack = 0x200000u + 0x1000u * static_cast<uint32_t>(i);
                p.stackSize = 0x800u;
                p.priority = 10 + i;
                a.createThread(p);
            }
            const int sema = a.createSemaphore(1, 4, 0u, 0u);
            a.signalSemaphore(sema, false);
            a.createSemaphore(0, 1, 1u, 2u);
            const int flag = a.createEventFlag(0x5u, 0x2u, 0u);
            a.setEventFlag(flag, 0x30u, false);
            a.setAlarm(100u, 0x123450u, 7u, 0x8000u, 0x9000u);
            a.addIrqHandler(false, 2u, 0x130000u, true, 1u, 2u, 3u);
            a.addIrqHandler(true, 5u, 0x140000u, false, 4u, 5u, 6u);
            a.setVSyncFlag(0x1000u, 0x1010u);
            a.setGsVSyncCallback(0x150000u, 0x1111u, 0x2222u);

            const std::vector<uint8_t> first = saveScheduler(a);
            EeScheduler &b = rtB.eeScheduler();
            b.reset(rdramB.data(), ctxB);
            Reader r(first.data(), first.size());
            t.IsTrue(EeSchedulerSavestate::load(b, r, rtB), "load succeeds: " + r.error());
            t.IsTrue(r.ok() && r.pos() == first.size(), "all bytes consumed");
            const std::vector<uint8_t> second = saveScheduler(b);
            t.IsTrue(first == second, "re-save is byte-identical");
            t.Equals(b.semaphore(sema)->count, a.semaphore(sema)->count, "semaphore count");
            t.Equals(b.eventFlag(flag)->bits, a.eventFlag(flag)->bits, "event flag bits");
            t.IsTrue(EeSchedulerSavestate::ready(a).empty(), "idle scheduler is saveable");

            // An untagged wait completion defers the save; a tagged one needs
            // a registered factory.
            GuestThread *thread = a.thread(2);
            thread->wait.completion = [](R5900Context &) {};
            t.IsTrue(!EeSchedulerSavestate::ready(a).empty(), "untagged completion defers");
            thread->wait.tag.kind = ps2_savestate::kCompletionCdStRead;
            t.IsTrue(EeSchedulerSavestate::ready(a).empty(), "CD stream-read tag has a factory");
            thread->wait.tag.kind = 999u;
            t.IsTrue(!EeSchedulerSavestate::ready(a).empty(), "unknown tag kind defers");
            thread->wait = {};
            ps2_savestate::setSteadyNowForTests(0);
        });

        tc.Run("memory round trip is byte-identical", [](TestCase &t)
        {
            PS2Memory a;
            PS2Memory b;
            t.IsTrue(a.initialize() && b.initialize(), "memories initialize");
            for (uint32_t i = 0; i < 4096u; ++i)
                a.getRDRAM()[i * 997u] = static_cast<uint8_t>(i * 31u);
            a.m_ioRegisters[0x1000F010u] = 0x55u;
            a.m_ioRegisters[0x10008000u] = 0x101u;
            a.gs().csr.store(0x2008u);
            a.m_path3MaskedFifo.push_back({1, 2, 3, 4});
            a.m_eeTimers[1].count = 77u;
            Writer w;
            PS2RuntimeSavestate::saveMemory(a, w);
            Reader r(w.buf.data(), w.buf.size());
            t.IsTrue(PS2RuntimeSavestate::loadMemory(b, r), "load succeeds");
            Writer w2;
            PS2RuntimeSavestate::saveMemory(b, w2);
            t.IsTrue(w.buf == w2.buf, "re-save is byte-identical");
            t.Equals(b.gs().csr.load(), uint64_t{0x2008u}, "CSR restored");
        });

        tc.Run("every stateful owner registers a section (static-lib link)", [](TestCase &t)
        {
            (void)ps2_savestate::config(); // pulls the syscall section's object in
            const auto &sections = ps2_savestate::registeredSections();
            for (const char *key : {"syscalls", "syscalls:k1", "syscalls:deci2", "stub:cd", "stub:sif", "stub:mc",
                                    "stub:pad", "stub:audio", "stub:dma", "stub:mpeg", "stub:mcdir", "snd", "padlatch",
                                    "support:Stubs/CD.cpp"})
                t.IsTrue(sections.count(key) == 1u, std::string("section registered: ") + key);
        });

        tc.Run("memory-card dir tree round trip; refuses to overwrite a different card", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            const fs::path base = fs::temp_directory_path() / "ss1-mcdir-test";
            fs::remove_all(base);
            const fs::path src = base / "src", dst = base / "dst", other = base / "other";
            fs::create_directories(src / "BASLUS-20772");
            { std::ofstream(src / "BASLUS-20772" / "save.dat", std::ios::binary) << "profile-bytes"; }
            { std::ofstream(src / "icon.sys", std::ios::binary) << "icon"; }
            Writer w;
            ps2_savestate::writeDirTree(w, src.string());
            Reader r(w.buf.data(), w.buf.size());
            t.IsTrue(ps2_savestate::readDirTree(r, dst.string()), "restore into a missing dir");
            std::ifstream in(dst / "BASLUS-20772" / "save.dat", std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            t.Equals(text, std::string("profile-bytes"), "file bytes restored");
            Reader again(w.buf.data(), w.buf.size());
            t.IsTrue(ps2_savestate::readDirTree(again, dst.string()), "restore over an identical dir");
            fs::create_directories(other);
            { std::ofstream(other / "icon.sys", std::ios::binary) << "someone else's card"; }
            Reader clash(w.buf.data(), w.buf.size());
            t.IsTrue(!ps2_savestate::readDirTree(clash, other.string()), "a different card is never overwritten");
            std::ifstream kept(other / "icon.sys", std::ios::binary);
            std::string keptText((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>());
            t.Equals(keptText, std::string("someone else's card"), "existing card untouched");
            fs::remove_all(base);
        });

        tc.Run("load refuses bad files", [](TestCase &t)
        {
            PS2Runtime rt;
            std::string error;
            const std::string path = tmpPath("ss1-test-bad.state");
            writeFile(path, {'n', 'o', 'p', 'e', 0, 0, 0, 0, 0, 0, 0, 0});
            t.IsTrue(!ps2_savestate::load(rt, path, error), "bad magic refused");
            t.IsTrue(error.find("not a ps2x save state") != std::string::npos, "names the magic");

            Writer w;
            w.bytes(ps2_savestate::kMagic, sizeof(ps2_savestate::kMagic));
            w.u32(ps2_savestate::kFormatVersion + 1u);
            writeFile(path, w.buf);
            t.IsTrue(!ps2_savestate::load(rt, path, error), "format version refused");

            Writer h;
            h.bytes(ps2_savestate::kMagic, sizeof(ps2_savestate::kMagic));
            h.u32(ps2_savestate::kFormatVersion);
            size_t mark = h.beginSection("header", ps2_savestate::kHeaderVersion + 1u);
            h.str("format=1\n");
            h.endSection(mark);
            writeFile(path, h.buf);
            t.IsTrue(!ps2_savestate::load(rt, path, error), "header version refused");

            Writer e;
            e.bytes(ps2_savestate::kMagic, sizeof(ps2_savestate::kMagic));
            e.u32(ps2_savestate::kFormatVersion);
            mark = e.beginSection("header", ps2_savestate::kHeaderVersion);
            e.str("format=1\nelf_sha=0000\n");
            e.endSection(mark);
            writeFile(path, e.buf);
            t.IsTrue(!ps2_savestate::load(rt, path, error), "ELF pin mismatch refused");
            t.IsTrue(error.find("header mismatch") != std::string::npos, "names the header field");
            std::filesystem::remove(path);
        });

        tc.Run("det2: PS2X_DETERMINISTIC pins GetDir dates (host mtimes and . / .. now)", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            const fs::path card = fs::temp_directory_path() / "det2-mcdate-test";
            fs::remove_all(card);
            fs::create_directories(card / "SAVE");
            { std::ofstream(card / "top.dat", std::ios::binary) << "top"; }
            fs::last_write_time(card / "top.dat", ss4FileTime(1700000200, 0));
            fs::last_write_time(card / "SAVE", ss4FileTime(1600000300, 0));
            const char *prev = std::getenv("PS2X_DETERMINISTIC");
            const std::string saved = prev ? prev : "";
            PS2Runtime rt;
            {
                Ss4IoPathsGuard g(card);
                // 4 entries: . .. SAVE top.dat; dates are bytes 0..15 of each 64-byte entry.
                const uint8_t fixed[8] = {0, 56, 34, 12, 16, 7, 2004 & 0xFF, 2004 >> 8};
                ::setenv("PS2X_DETERMINISTIC", "1", 1);
                const std::vector<uint8_t> det = ss4GetDir(rt, "mc0:/", 4);
                bool allFixed = true;
                for (int e = 0; e < 4; ++e)
                    for (int half = 0; half < 2; ++half)
                        allFixed = allFixed && std::memcmp(det.data() + e * 64 + half * 8, fixed, 8) == 0;
                t.IsTrue(allFixed, "every entry's create/modify date is 2004-07-16 12:34:56");
                ::unsetenv("PS2X_DETERMINISTIC");
                const std::vector<uint8_t> host = ss4GetDir(rt, "mc0:/", 4);
                t.IsTrue(std::memcmp(host.data() + 2 * 64 + 8, host.data() + 3 * 64 + 8, 8) != 0,
                         "without det the dates follow host mtimes");
            }
            if (prev)
                ::setenv("PS2X_DETERMINISTIC", saved.c_str(), 1);
            else
                ::unsetenv("PS2X_DETERMINISTIC");
            fs::remove_all(card);
        });

        tc.Run("ss3 s4: card dirs and timestamps round-trip; GetDir equal after load", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            const fs::path base = fs::temp_directory_path() / "ss3-mcdir-test";
            fs::remove_all(base);
            PS2Runtime rt;
            // Card tree: a save dir with a file, an empty dir, a top-level file.
            const fs::path card = base / "card";
            fs::create_directories(card / "SAVE");
            fs::create_directories(card / "EMPTY");
            { std::ofstream(card / "SAVE" / "data", std::ios::binary) << "save-bytes"; }
            { std::ofstream(card / "top.dat", std::ios::binary) << "top"; }
            // Distinct mid-second times (file times first, then dirs: creating
            // files bumps their parents).
            fs::last_write_time(card / "SAVE" / "data", ss4FileTime(1700000100, 111111111));
            fs::last_write_time(card / "top.dat", ss4FileTime(1700000200, 222222222));
            fs::last_write_time(card / "SAVE", ss4FileTime(1700000300, 333333333));
            fs::last_write_time(card / "EMPTY", ss4FileTime(1700000400, 444444444));
            const fs::file_time_type dataT = fs::last_write_time(card / "SAVE" / "data");
            const fs::file_time_type topT = fs::last_write_time(card / "top.dat");
            const fs::file_time_type saveT = fs::last_write_time(card / "SAVE");
            const fs::file_time_type emptyT = fs::last_write_time(card / "EMPTY");
            {
                Ss4IoPathsGuard g(card);
                const std::vector<uint8_t> tabA1 = ss4GetDir(rt, "mc0:/", 16);
                const std::vector<uint8_t> tabA2 = ss4GetDir(rt, "mc0:/", 16);
                const auto hasName = [](const std::vector<uint8_t> &tab, const char *name) {
                    const std::string s(tab.begin(), tab.end());
                    return s.find(name) != std::string::npos;
                };
                t.IsTrue(hasName(tabA1, "SAVE") && hasName(tabA1, "top.dat"), "GetDir lists the card");
                const auto tail = [](const std::vector<uint8_t> &tab) {
                    return std::vector<uint8_t>(tab.begin() + 2 * 64, tab.end()); // past . and ..
                };
                t.IsTrue(tail(tabA1) == tail(tabA2), "GetDir table stable across calls");
                Writer w;
                ps2_savestate::writeDirTree(w, card.string());
                fs::remove_all(card);
                Reader r(w.buf.data(), w.buf.size());
                t.IsTrue(ps2_savestate::readDirTree(r, card.string()), "restore into the wiped root");
                const std::vector<uint8_t> tabB = ss4GetDir(rt, "mc0:/", 16);
                t.IsTrue(tail(tabB) == tail(tabA1), "GetDir table (incl. timestamps) equal after load");
            }
            t.IsTrue(fs::last_write_time(card / "SAVE" / "data") == dataT, "file mtime restored");
            t.IsTrue(fs::last_write_time(card / "top.dat") == topT, "top-level mtime restored");
            t.IsTrue(fs::last_write_time(card / "SAVE") == saveT, "dir mtime restored");
            t.IsTrue(fs::last_write_time(card / "EMPTY") == emptyT, "empty-dir mtime restored");
            // The brief's mkdir case: an empty mkdir'd dir survives, and a file
            // can be created in it afterwards.
            const fs::path card2 = base / "card2";
            fs::create_directories(card2 / "NEW");
            Writer w2;
            ps2_savestate::writeDirTree(w2, card2.string());
            fs::remove_all(card2);
            Reader r2(w2.buf.data(), w2.buf.size());
            t.IsTrue(ps2_savestate::readDirTree(r2, card2.string()), "restore with the empty dir");
            t.IsTrue(fs::is_directory(card2 / "NEW"), "empty mkdir'd dir survives");
            { std::ofstream(card2 / "NEW" / "data", std::ios::binary) << "x"; }
            t.IsTrue(fs::exists(card2 / "NEW" / "data"), "file created in the restored dir");
            fs::remove_all(base);
        });

        tc.Run("ss3 s4: card restore validates the destination and never rewrites", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            const fs::path base = fs::temp_directory_path() / "ss3-mcdir-test2";
            fs::remove_all(base);
            const fs::path src = base / "src";
            fs::create_directories(src / "D");
            { std::ofstream(src / "D" / "f.dat", std::ios::binary) << "v"; }
            { std::ofstream(src / "g.dat", std::ios::binary) << "w"; }
            Writer w;
            ps2_savestate::writeDirTree(w, src.string());
            const auto tryLoad = [&w](const std::string &root) {
                Reader r(w.buf.data(), w.buf.size());
                return ps2_savestate::readDirTree(r, root);
            };
            const fs::path dst1 = base / "dst1"; // extra empty dir
            fs::create_directories(dst1 / "D");
            { std::ofstream(dst1 / "D" / "f.dat", std::ios::binary) << "v"; }
            { std::ofstream(dst1 / "g.dat", std::ios::binary) << "w"; }
            fs::create_directories(dst1 / "EXTRA");
            t.IsTrue(!tryLoad(dst1.string()), "an extra empty dir refuses");
            const fs::path dst2 = base / "dst2"; // extra file
            fs::create_directories(dst2 / "D");
            { std::ofstream(dst2 / "D" / "f.dat", std::ios::binary) << "v"; }
            { std::ofstream(dst2 / "g.dat", std::ios::binary) << "w"; }
            { std::ofstream(dst2 / "extra.dat", std::ios::binary) << "x"; }
            t.IsTrue(!tryLoad(dst2.string()), "an extra file refuses");
            const fs::path dst3 = base / "dst3"; // different bytes
            fs::create_directories(dst3 / "D");
            { std::ofstream(dst3 / "D" / "f.dat", std::ios::binary) << "someone else"; }
            { std::ofstream(dst3 / "g.dat", std::ios::binary) << "w"; }
            t.IsTrue(!tryLoad(dst3.string()), "different bytes refuse");
            const fs::path dst4 = base / "dst4"; // file where a dir belongs
            fs::create_directories(dst4);
            { std::ofstream(dst4 / "D", std::ios::binary) << "x"; }
            { std::ofstream(dst4 / "g.dat", std::ios::binary) << "w"; }
            t.IsTrue(!tryLoad(dst4.string()), "a file where a dir belongs refuses");
            const fs::path dst5 = base / "dst5"; // dir where a file belongs
            fs::create_directories(dst5 / "D");
            { std::ofstream(dst5 / "D" / "f.dat", std::ios::binary) << "v"; }
            fs::create_directories(dst5 / "g.dat");
            t.IsTrue(!tryLoad(dst5.string()), "a dir where a file belongs refuses");
            // Identical read-only files with correct times load without any
            // rewrite (a rewrite would fail on the read-only mode).
            const fs::path dst6 = base / "dst6";
            fs::copy(src, dst6, fs::copy_options::recursive);
            t.IsTrue(tryLoad(dst6.string()), "first load normalizes times");
            fs::permissions(dst6 / "D" / "f.dat", fs::perms::owner_read);
            fs::permissions(dst6 / "g.dat", fs::perms::owner_read);
            t.IsTrue(tryLoad(dst6.string()), "identical read-only files load (never rewritten)");
            fs::permissions(dst6 / "D" / "f.dat", fs::perms::owner_read | fs::perms::owner_write);
            fs::permissions(dst6 / "g.dat", fs::perms::owner_read | fs::perms::owner_write);
            // Identical bytes with wrong times load and take the saved times.
            const fs::path dst7 = base / "dst7";
            fs::copy(src, dst7, fs::copy_options::recursive);
            fs::last_write_time(dst7 / "D" / "f.dat", ss4FileTime(1500000000, 0));
            t.IsTrue(tryLoad(dst7.string()), "wrong-time files load");
            t.IsTrue(fs::last_write_time(dst7 / "D" / "f.dat") == fs::last_write_time(src / "D" / "f.dat"),
                     "wrong-time files take the saved time");
            fs::remove_all(base);
        });

        tc.Run("ss3 s4: card traversal errors abort the save", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            const fs::path base = fs::temp_directory_path() / "ss3-mcdir-test3";
            fs::remove_all(base);
            const fs::path src = base / "src";
            fs::create_directories(src);
            { std::ofstream(src / "ok.dat", std::ios::binary) << "ok"; }
            { std::ofstream(src / "noperm.dat", std::ios::binary) << "shh"; }
            fs::permissions(src / "noperm.dat", fs::perms::none);
            bool threw = false;
            try
            {
                Writer w;
                ps2_savestate::writeDirTree(w, src.string());
            }
            catch (const ps2_savestate::dir_tree_error &)
            {
                threw = true;
            }
            t.IsTrue(threw, "an unreadable file throws instead of saving short");
            fs::permissions(src / "noperm.dat", fs::perms::owner_read | fs::perms::owner_write);
            const fs::path src2 = base / "src2";
            fs::create_directories(src2);
            fs::create_symlink("/nonexistent-ss3-target", src2 / "dangling");
            threw = false;
            try
            {
                Writer w;
                ps2_savestate::writeDirTree(w, src2.string());
            }
            catch (const ps2_savestate::dir_tree_error &)
            {
                threw = true;
            }
            t.IsTrue(threw, "a dangling symlink throws");
            const fs::path fileRoot = base / "fileRoot";
            { std::ofstream(fileRoot, std::ios::binary) << "x"; }
            threw = false;
            try
            {
                Writer w;
                ps2_savestate::writeDirTree(w, fileRoot.string());
            }
            catch (const ps2_savestate::dir_tree_error &)
            {
                threw = true;
            }
            t.IsTrue(threw, "a file where the root belongs throws");
            // A missing root still saves an empty tree (existing behavior).
            Writer w;
            ps2_savestate::writeDirTree(w, (base / "missing").string());
            t.Equals(w.buf.size(), size_t{16}, "missing root saves two empty counts");
            Reader r(w.buf.data(), w.buf.size());
            t.IsTrue(ps2_savestate::readDirTree(r, (base / "restored").string()), "empty tree restores");
            t.IsTrue(fs::is_directory(base / "restored"), "restore creates the root");
            fs::remove_all(base);
        });

        tc.Run("ss3 s5: runner identity is the loaded runtime module", [](TestCase &t)
        {
            const std::string mod = ps2_savestate::runtimeModulePath();
#if defined(__APPLE__) || defined(__linux__)
            t.IsTrue(!mod.empty(), "module path obtained");
            if (mod.empty())
                return;
            t.IsTrue(std::filesystem::exists(mod), "module path exists");
            // The same module serves any function in this library ...
            Dl_info info{};
            const bool ok =
                dladdr(reinterpret_cast<const void *>(&ps2_savestate::sha256Hex), &info) && info.dli_fname;
            t.IsTrue(ok, "dladdr works on the runtime library");
            if (ok)
                t.Equals(mod, std::string(info.dli_fname), "helper names the runtime's own module");
            // ... and on desktop static builds that is the executable itself,
            // so the identity (and its hash) is unchanged from before S5.
            std::string exe;
#if defined(__APPLE__)
            char buf[4096];
            uint32_t size = sizeof(buf);
            if (_NSGetExecutablePath(buf, &size) == 0)
                exe = buf;
#else
            std::error_code ec;
            exe = std::filesystem::read_symlink("/proc/self/exe", ec).string();
#endif
            t.IsTrue(!exe.empty(), "executable path obtained");
            if (!exe.empty())
                t.IsTrue(std::filesystem::equivalent(mod, exe), "module is the test executable");
            std::string hex;
            t.IsTrue(ps2_savestate::sha256File(mod, hex) && hex.size() == 64u, "module hashes");
#else
            t.Equals(mod, std::string(), "no module query off Apple/Linux");
#endif
        });

        tc.Run("ss3 s5: strict refuses unknown runner identity", [](TestCase &t)
        {
            using ps2_savestate::RunnerShaVerdict;
            using ps2_savestate::checkRunnerSha;
            t.IsTrue(checkRunnerSha("aaa", "aaa", true) == RunnerShaVerdict::Accept, "strict match accepts");
            t.IsTrue(checkRunnerSha("aaa", "bbb", true) == RunnerShaVerdict::Refuse, "strict mismatch refuses");
            t.IsTrue(checkRunnerSha("unknown", "unknown", true) == RunnerShaVerdict::Refuse,
                     "strict unknown-vs-unknown refuses (warned past it pre-S5)");
            t.IsTrue(checkRunnerSha("unknown", "bbb", true) == RunnerShaVerdict::Refuse,
                     "strict unknown-saved refuses");
            t.IsTrue(checkRunnerSha("aaa", "unknown", true) == RunnerShaVerdict::Refuse,
                     "strict unknown-current refuses");
            t.IsTrue(checkRunnerSha("aaa", "aaa", false) == RunnerShaVerdict::Accept, "lax match accepts");
            t.IsTrue(checkRunnerSha("aaa", "bbb", false) == RunnerShaVerdict::Warn, "lax mismatch warns");
            t.IsTrue(checkRunnerSha("unknown", "unknown", false) == RunnerShaVerdict::Accept,
                     "lax unknown-vs-unknown keeps the legacy silent match");
        });

        tc.Run("ss4: microvu saveReady gates on D/T stops", [](TestCase &t)
        {
            VuState clean{};
            t.IsTrue(ps2_microvu::saveReady(clean).empty(), "E-bit-idle state is saveable");
            VuState dStop{};
            dStop.stoppedByD = true;
            t.IsTrue(!ps2_microvu::saveReady(dStop).empty(), "D stop awaiting MSCNT defers");
            VuState tStop{};
            tStop.stoppedByT = true;
            t.IsTrue(!ps2_microvu::saveReady(tStop).empty(), "T stop awaiting MSCNT defers");
        });

        tc.Run("ds1: one quick-save slot per memory-card root", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            t.Equals(ps2_savestate::quickSlotPath("/app/files", "/app/files/mc0"),
                     (fs::path("/app/files") / "states" / "quicksave-mc0.state").string(), "play mc0 slot");
            t.Equals(ps2_savestate::quickSlotPath("/app/files", "/app/files/mc0-allpeak"),
                     (fs::path("/app/files") / "states" / "quicksave-mc0-allpeak.state").string(),
                     "P3R root keeps a separate slot");
            t.Equals(ps2_savestate::quickSlotPath("/app/files", ""),
                     (fs::path("/app/files") / "states" / "quicksave-mc0.state").string(), "empty root is mc0");
            const std::string weird = ps2_savestate::quickSlotPath("/app/files", "/x/weird/root*name?");
            t.IsTrue(weird.find("quicksave-root_name_.state") != std::string::npos, "leaf sanitized: " + weird);
            t.IsTrue(weird.size() >= 6u && weird.compare(weird.size() - 6u, 6u, ".state") == 0,
                     "slot keeps the .state suffix");
        });

        tc.Run("ds1: quick requests are single-flight with peek/take semantics", [](TestCase &t)
        {
            using ps2_savestate::clearPendingQuickSave;
            using ps2_savestate::pendingQuickSavePath;
            using ps2_savestate::quickSavePending;
            using ps2_savestate::requestQuickLoad;
            using ps2_savestate::requestQuickSave;
            using ps2_savestate::takePendingQuickLoad;
            namespace fs = std::filesystem;
            const fs::path base = fs::temp_directory_path() / "ds1-pending-test";
            fs::remove_all(base);
            const std::string slot = (base / "states" / "quicksave-mc0.state").string();
            clearPendingQuickSave();
            std::string drop;
            takePendingQuickLoad(drop);
            t.IsTrue(!quickSavePending(), "nothing pending at start");
            t.IsTrue(requestQuickSave(slot), "save request accepted");
            t.IsTrue(fs::exists(base / "states"), "save request creates the slot dir");
            t.IsTrue(quickSavePending(), "save shows pending");
            t.IsTrue(!requestQuickSave(slot), "second save rejected while one waits");
            t.IsTrue(!requestQuickLoad(slot), "load rejected while a save waits");
            std::string peek;
            t.IsTrue(pendingQuickSavePath(peek) && peek == slot, "save peek returns the path");
            t.IsTrue(quickSavePending(), "save peek does not consume (deferrals retry)");
            clearPendingQuickSave();
            t.IsTrue(!quickSavePending(), "clear drops the save");
            t.IsTrue(requestQuickLoad(slot), "load request accepted");
            t.IsTrue(!requestQuickSave(slot), "save rejected while a load waits");
            std::string took;
            t.IsTrue(takePendingQuickLoad(took) && took == slot, "load take returns the path");
            t.IsTrue(!takePendingQuickLoad(took), "load take is one-shot");
            fs::remove_all(base);
        });

        tc.Run("ds1: quick status carries the last result with an age", [](TestCase &t)
        {
            ps2_savestate::noteQuickStatus("saved at tick 1714");
            std::string message;
            uint64_t ageMs = 0u;
            t.IsTrue(ps2_savestate::quickStatus(message, ageMs), "status present");
            t.Equals(message, std::string("saved at tick 1714"), "message");
            t.IsTrue(ageMs < 60000u, "age is fresh");
        });

        tc.Run("ds1: lenient card restore keeps the disk on any difference", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            const fs::path base = fs::temp_directory_path() / "ds1-mcdir-test";
            fs::remove_all(base);
            const fs::path src = base / "src", same = base / "same", changed = base / "changed",
                           extra = base / "extra", missing = base / "missing";
            fs::create_directories(src / "BASLUS-20772");
            { std::ofstream(src / "BASLUS-20772" / "save.dat", std::ios::binary) << "profile-bytes"; }
            { std::ofstream(src / "icon.sys", std::ios::binary) << "icon"; }
            Writer w;
            ps2_savestate::writeDirTree(w, src.string());
            std::string note;
            { // identical trees: true, clean note, times restored
                fs::create_directories(same / "BASLUS-20772");
                { std::ofstream(same / "BASLUS-20772" / "save.dat", std::ios::binary) << "profile-bytes"; }
                { std::ofstream(same / "icon.sys", std::ios::binary) << "icon"; }
                Reader r(w.buf.data(), w.buf.size());
                t.IsTrue(ps2_savestate::readDirTreeLenient(r, same.string(), note), "identical restores");
                t.IsTrue(note.empty(), "identical note is clean");
            }
            { // different bytes: true, loud note, disk untouched
                fs::create_directories(changed / "BASLUS-20772");
                { std::ofstream(changed / "BASLUS-20772" / "save.dat", std::ios::binary) << "newer-progress"; }
                { std::ofstream(changed / "icon.sys", std::ios::binary) << "icon"; }
                Reader r(w.buf.data(), w.buf.size());
                t.IsTrue(ps2_savestate::readDirTreeLenient(r, changed.string(), note), "different keeps disk");
                t.IsTrue(note.find("different") != std::string::npos, "note names it: " + note);
                std::ifstream kept(changed / "BASLUS-20772" / "save.dat", std::ios::binary);
                std::string keptText((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>());
                t.Equals(keptText, std::string("newer-progress"), "newer bytes survive");
            }
            { // extra file on disk (the game's own later save): true, disk untouched
                fs::create_directories(extra / "BASLUS-20772");
                { std::ofstream(extra / "BASLUS-20772" / "save.dat", std::ios::binary) << "profile-bytes"; }
                { std::ofstream(extra / "icon.sys", std::ios::binary) << "icon"; }
                { std::ofstream(extra / "BASLUS-20772" / "later.dat", std::ios::binary) << "later"; }
                Reader r(w.buf.data(), w.buf.size());
                t.IsTrue(ps2_savestate::readDirTreeLenient(r, extra.string(), note), "extra keeps disk");
                t.IsTrue(note.find("extra") != std::string::npos, "note names it: " + note);
                t.IsTrue(fs::exists(extra / "BASLUS-20772" / "later.dat"), "later file survives");
            }
            { // file deleted on disk: true, never recreated
                fs::create_directories(missing / "BASLUS-20772");
                { std::ofstream(missing / "BASLUS-20772" / "save.dat", std::ios::binary) << "profile-bytes"; }
                Reader r(w.buf.data(), w.buf.size());
                t.IsTrue(ps2_savestate::readDirTreeLenient(r, missing.string(), note), "missing keeps disk");
                t.IsTrue(note.find("deleted on disk") != std::string::npos, "note names it: " + note);
                t.IsTrue(!fs::exists(missing / "icon.sys"), "deleted file stays deleted");
            }
            { // corrupt payload still fails (validation is not lenient)
                std::vector<uint8_t> bad = w.buf;
                bad.resize(12u);
                Reader r(bad.data(), bad.size());
                t.IsTrue(!ps2_savestate::readDirTreeLenient(r, same.string(), note), "truncation refused");
            }
            fs::remove_all(base);
        });

        tc.Run("RBF1: stub:sif v1 handlers migrate with argument 0, v2 keeps its argument", [](TestCase &t)
        {
            // v1 layout (pre-rebase, tag ssx3-pre-rebase-1002): u32 transfer
            // id, regs map, sregs map, handlers as map<u32,u32>, heap allocs,
            // heap bytes, cmd/s-cmd buffers, initialized flag.
            ps2_stubs::resetSifState();
            const auto &hooks = ps2_savestate::registeredSections().at("stub:sif");
            t.Equals(hooks.version, 2u, "stub:sif still saves v2");
            t.Equals(hooks.minLoadVersion, 1u, "stub:sif accepts v1 and v2");
            const size_t kHeapBytes = 0x500000u; // kIopHeapLimit - kIopHeapBase (Stubs/Helpers/Support.h)

            auto craftPayload = [](bool v1, uint32_t cid, uint32_t function, uint32_t argument) {
                Writer w;
                w.u32(9u);
                const std::unordered_map<uint32_t, uint32_t> empty;
                ps2_savestate::writeOrderedPod(w, empty);
                ps2_savestate::writeOrderedPod(w, empty);
                if (v1)
                {
                    std::unordered_map<uint32_t, uint32_t> handlers;
                    handlers[cid] = function;
                    ps2_savestate::writeOrderedPod(w, handlers);
                }
                else
                {
                    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> handlers;
                    handlers[cid] = {function, argument};
                    ps2_savestate::writeOrdered(w, handlers, [](Writer &ww, const auto &e) {
                        ww.pod(e.first);
                        ww.u32(e.second.first);
                        ww.u32(e.second.second);
                    });
                }
                w.u64(0u);
                const std::vector<uint8_t> heap(kHeapBytes, 0u);
                w.bytes(heap.data(), heap.size());
                w.u32(0u);
                w.u32(0u);
                w.b(false);
                return w.buf;
            };

            { // v1: function-only handler runs with argument 0
                const std::vector<uint8_t> payload = craftPayload(true, 7u, 0x00100400u, 0u);
                Reader r(payload.data(), payload.size());
                ps2_savestate::setLoadingSectionVersion(1u);
                const bool ok = hooks.load(r);
                ps2_savestate::setLoadingSectionVersion(0u);
                t.IsTrue(ok && r.ok(), std::string("v1 payload loads: ") + (r.ok() ? "ok-flag" : r.error()));
                t.IsTrue(r.pos() == r.size(), "v1 payload fully consumed");
                uint32_t function = 0u, argument = 0u;
                t.IsTrue(ps2_stubs::sifCmdHandlerForTest(7u, function, argument), "handler present");
                t.Equals(function, 0x00100400u, "function address preserved");
                t.Equals(argument, 0u, "migrated handler runs with argument 0 (the old code passed none)");
            }
            { // v2: {function, argument} round trips
                const std::vector<uint8_t> payload = craftPayload(false, 11u, 0x00200800u, 0x42u);
                Reader r(payload.data(), payload.size());
                ps2_savestate::setLoadingSectionVersion(2u);
                const bool ok = hooks.load(r);
                ps2_savestate::setLoadingSectionVersion(0u);
                t.IsTrue(ok && r.ok(), "v2 payload loads");
                uint32_t function = 0u, argument = 0u;
                t.IsTrue(ps2_stubs::sifCmdHandlerForTest(11u, function, argument), "handler present");
                t.Equals(function, 0x00200800u, "v2 function preserved");
                t.Equals(argument, 0x42u, "v2 argument preserved");
            }
            ps2_stubs::resetSifState();
        });

        tc.Run("RBF1: syscalls v1 resumes the VFS counter, v2 leaves it, v3 carries it", [](TestCase &t)
        {
            const auto &hooks = ps2_savestate::registeredSections().at("syscalls");
            t.Equals(hooks.version, 3u, "syscalls now saves v3");
            t.Equals(hooks.minLoadVersion, 1u, "syscalls accepts v1..v3");

            PS2Vfs vfs;
            PS2Vfs::setSavestateBinding(&vfs);
            vfs.setNextDescriptorForSavestate(5u);
            Writer w;
            hooks.save(w); // v3 payload (v2 fields + trailing counter)
            const std::vector<uint8_t> v3 = w.buf;
            t.IsTrue(v3.size() >= 4u, "v3 payload non-empty");

            // v2 = v3 without the trailing counter; v1 = v2 with the old
            // leading g_nextFd word (pre-rebase layout: u32 + v2 fields).
            const std::vector<uint8_t> v2(v3.begin(), v3.end() - 4u);
            Writer pre;
            pre.u32(7u); // the old g_nextFd value the v1 file carries
            std::vector<uint8_t> v1 = pre.buf;
            v1.insert(v1.end(), v2.begin(), v2.end());

            { // v1: the old allocator word resumes the VFS counter
                vfs.setNextDescriptorForSavestate(3u);
                Reader r(v1.data(), v1.size());
                ps2_savestate::setLoadingSectionVersion(1u);
                const bool ok = hooks.load(r);
                ps2_savestate::setLoadingSectionVersion(0u);
                t.IsTrue(ok && r.ok(), "v1 payload loads");
                t.IsTrue(r.pos() == r.size(), "v1 payload fully consumed");
                t.Equals(vfs.nextDescriptorForSavestate(), 7u, "old g_nextFd resumes the VFS counter");
            }
            { // v2: no counter word, the VFS is untouched
                vfs.setNextDescriptorForSavestate(9u);
                Reader r(v2.data(), v2.size());
                ps2_savestate::setLoadingSectionVersion(2u);
                const bool ok = hooks.load(r);
                ps2_savestate::setLoadingSectionVersion(0u);
                t.IsTrue(ok && r.ok(), "v2 payload loads");
                t.Equals(vfs.nextDescriptorForSavestate(), 9u, "v2 leaves the VFS counter alone");
            }
            { // v3: the trailing counter restores
                vfs.setNextDescriptorForSavestate(3u);
                Reader r(v3.data(), v3.size());
                ps2_savestate::setLoadingSectionVersion(3u);
                const bool ok = hooks.load(r);
                ps2_savestate::setLoadingSectionVersion(0u);
                t.IsTrue(ok && r.ok(), "v3 payload loads");
                t.Equals(vfs.nextDescriptorForSavestate(), 5u, "v3 restores the saved counter");
            }
            PS2Vfs::setSavestateBinding(nullptr);
        });

        tc.Run("RBF1: VFS open/close/open allocates on, save/restore resumes the counter", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            const fs::path base = fs::temp_directory_path() / "rbf1-vfs-test";
            fs::remove_all(base);
            fs::create_directories(base);
            { std::ofstream(base / "f.txt", std::ios::binary) << "bytes"; }
            PS2VfsMounts mounts;
            mounts.hostRoot = base;
            const PS2RomDevice rom;

            PS2Vfs vfs;
            t.Equals(vfs.open("host:f.txt", PS2_FIO_O_RDONLY, mounts, rom), 3, "first descriptor is 3");
            t.Equals(vfs.close(3), 0, "close succeeds");
            t.Equals(vfs.open("host:f.txt", PS2_FIO_O_RDONLY, mounts, rom), 4, "counter advances across a close");
            t.Equals(vfs.close(4), 0, "close succeeds");
            t.Equals(vfs.nextDescriptorForSavestate(), 5u, "two open/close cycles leave the counter at 5");

            // Save with no files open, then restore into a fresh counter: the
            // next open must match uninterrupted execution (fd 5).
            const auto &hooks = ps2_savestate::registeredSections().at("syscalls");
            PS2Vfs::setSavestateBinding(&vfs);
            Writer w;
            hooks.save(w);
            PS2Vfs::setSavestateBinding(nullptr);
            vfs.setNextDescriptorForSavestate(3u); // fresh-process restore
            PS2Vfs::setSavestateBinding(&vfs);
            Reader r(w.buf.data(), w.buf.size());
            ps2_savestate::setLoadingSectionVersion(3u);
            const bool ok = hooks.load(r);
            ps2_savestate::setLoadingSectionVersion(0u);
            PS2Vfs::setSavestateBinding(nullptr);
            t.IsTrue(ok && r.ok(), "section reloads");
            t.Equals(vfs.open("host:f.txt", PS2_FIO_O_RDONLY, mounts, rom), 5, "reopen matches uninterrupted fd");
            t.Equals(vfs.close(5), 0, "close succeeds");
            fs::remove_all(base);
        });

        tc.Run("qsr1: completion toasts fire for the touch menu's slots only", [](TestCase &t)
               {
            using ps2_savestate::QuickSlotKind;
            using ps2_savestate::quickSlotKind;
            t.IsTrue(quickSlotKind("/docs/states/quick-manual-mc0.state") == QuickSlotKind::Qsr1Manual,
                     "manual slot");
            t.IsTrue(quickSlotKind("/docs/states/quick-auto-mc0.state") == QuickSlotKind::Qsr1Auto,
                     "auto slot");
            t.IsTrue(quickSlotKind("/run/states/quicksave-mc0.state") == QuickSlotKind::Other,
                     "DS1 chord slot stays silent");
            t.IsTrue(quickSlotKind("/run/save-2500.state") == QuickSlotKind::Other, "env save stays silent");
            t.IsTrue(quickSlotKind("quick-auto-mc0.state") == QuickSlotKind::Qsr1Auto, "bare filename");
            t.IsTrue(quickSlotKind("") == QuickSlotKind::Other, "empty path");
        });
    });
}
