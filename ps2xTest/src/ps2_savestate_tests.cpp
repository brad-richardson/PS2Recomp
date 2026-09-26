#include "MiniTest.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_savestate.h"
#include "../../ps2xRuntime/src/lib/ps2_savestate_internal.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
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
} // namespace

void register_ps2_savestate_tests()
{
    MiniTest::Case("Ps2Savestate", [](TestCase &tc)
    {
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
    });
}
