#include "MiniTest.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/ps2_gs_parallel_backend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_savestate.h"
#include "../../ps2xRuntime/src/lib/Kernel/Stubs/MemoryCard.h"
#include "../../ps2xRuntime/src/lib/ps2_savestate_internal.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
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

    // SS3 GPU tests: run against the live parallel backend on machines with
    // Vulkan (the Mac), skip everywhere else. Vulkan init is slowish, so the
    // first headless failure skips the rest.
    bool g_ss3GpuSkipped = false;

    bool ss3WantParallelGpu()
    {
        if (g_ss3GpuSkipped)
        {
            std::cout << "[skip: no GPU] ";
            return false;
        }
        if (!ps2x_gs_parallel::available())
        {
            std::cout << "[skip: parallel backend not built] ";
            return false;
        }
#if defined(__APPLE__)
        if (!std::getenv("GRANITE_VULKAN_LIBRARY"))
        {
            // ssx3_boot.py's Mac recipe; harmless when absent (init fails,
            // the test skips).
            const char *mvk = "/opt/homebrew/lib/libvulkan.1.dylib";
            std::error_code ec;
            if (std::filesystem::exists(mvk, ec))
                setenv("GRANITE_VULKAN_LIBRARY", mvk, 0);
        }
#endif
        return true;
    }

    // An empty save with a failed (and never successful) init means headless.
    bool ss3HaveParallelBlob(TestCase &t, const std::vector<uint8_t> &blob)
    {
        if (!blob.empty())
            return true;
        const ps2x_gs_parallel::Stats st = ps2x_gs_parallel::stats();
        if (st.initFailed && !st.initOk)
        {
            g_ss3GpuSkipped = true;
            std::cout << "[skip: Vulkan init failed] ";
            return false;
        }
        t.Fail("parallel backend save came back empty without an init failure");
        return false;
    }

    // v3 tail parser, anchored at the footer (no paraLLEl struct sizes).
    // Pre-SS3 blobs have no footer and fail to parse, which is what the S2
    // test asserts against.
    struct Ss3Tail
    {
        bool ok = false;
        size_t tailOff = 0, tailEnd = 0;
        bool hasClut = false;
        size_t ringOff = 0;
        uint64_t ringN = 0;
        uint32_t base = 0, next = 0, iface = 0, latest = 0;
        size_t clutEnd = 0; // first byte after the CLUT part (S3: vertex part)
    };

    Ss3Tail ss3ParseTail(const std::vector<uint8_t> &blob)
    {
        Ss3Tail out;
        constexpr uint32_t kMagic = 0x33534750u; // "PGS3" LE
        constexpr size_t kFooter = sizeof(uint64_t) + sizeof(uint32_t);
        if (blob.size() < kFooter)
            return out;
        uint32_t magic = 0u;
        std::memcpy(&magic, blob.data() + blob.size() - sizeof(magic), sizeof(magic));
        if (magic != kMagic)
            return out;
        uint64_t tailLen = 0u;
        std::memcpy(&tailLen, blob.data() + blob.size() - kFooter, sizeof(tailLen));
        if (tailLen < 1u || tailLen > blob.size() - kFooter)
            return out;
        out.tailOff = blob.size() - kFooter - static_cast<size_t>(tailLen);
        out.tailEnd = blob.size() - kFooter;
        size_t t = out.tailOff;
        out.hasClut = blob[t++] != 0u;
        if (out.hasClut)
        {
            if (out.tailEnd - t < sizeof(uint64_t))
                return Ss3Tail{};
            uint64_t n = 0u;
            std::memcpy(&n, blob.data() + t, sizeof(n));
            t += sizeof(n);
            const size_t rest = out.tailEnd - t;
            if (n > rest || rest - static_cast<size_t>(n) < 4u * sizeof(uint32_t))
                return Ss3Tail{};
            out.ringOff = t;
            out.ringN = n;
            std::memcpy(&out.base, blob.data() + t + n, sizeof(out.base));
            std::memcpy(&out.next, blob.data() + t + n + 4u, sizeof(out.next));
            std::memcpy(&out.iface, blob.data() + t + n + 8u, sizeof(out.iface));
            std::memcpy(&out.latest, blob.data() + t + n + 12u, sizeof(out.latest));
            t += static_cast<size_t>(n) + 4u * sizeof(uint32_t);
        }
        out.clutEnd = t;
        out.ok = true;
        return out;
    }

    void ss3PutU32(std::vector<uint8_t> &blob, size_t off, uint32_t v)
    {
        std::memcpy(blob.data() + off, &v, sizeof(v));
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

    // S3 tests touch the backend through registers (which init lazily)
    // before their first save; confirm the init outcome for skip logic.
    bool ss3BackendInitOk(TestCase &t)
    {
        const ps2x_gs_parallel::Stats st = ps2x_gs_parallel::stats();
        if (st.initOk)
            return true;
        if (st.initFailed)
        {
            g_ss3GpuSkipped = true;
            std::cout << "[skip: Vulkan init failed] ";
            return false;
        }
        t.Fail("backend touched but init neither ok nor failed");
        return false;
    }

    uint64_t ss3F32x2(float a, float b)
    {
        uint64_t v = 0u;
        float f[2] = {a, b};
        std::memcpy(&v, f, sizeof(v));
        return v;
    }

    uint32_t ss3F32Bits(float f)
    {
        uint32_t v = 0u;
        std::memcpy(&v, &f, sizeof(v));
        return v;
    }

    // Triangle-strip setup + one ADC vertex kick (queued, never drawn, so no
    // framebuffer setup is needed). Register addresses are paraLLEl's
    // gs_register_addr.hpp.
    void ss3StripSetup(GSRasterBackend &be)
    {
        be.RawWriteRegister(0x00, 4u); // PRIM: TriangleStrip (also resets the queue)
        be.RawWriteRegister(0x02, ss3F32x2(0.5f, 0.25f)); // ST
        be.RawWriteRegister(0x01, uint64_t{0x44332211} | (uint64_t{ss3F32Bits(1.0f)} << 32)); // RGBAQ
        be.RawWriteRegister(0x03, 10u | (20u << 16)); // UV
        be.RawWriteRegister(0x0a, uint64_t{0x5a} << 56); // FOG
    }

    void ss3Kick(GSRasterBackend &be, uint32_t x, uint32_t y, uint32_t z)
    {
        be.RawWriteRegister(0x0d, uint64_t{x} | (uint64_t{y} << 16) | (uint64_t{z} << 32)); // XYZ3
    }

    // The vertex part of a v3 tail ([u32 count][count x 36B entries]); fails
    // the test and returns empty when the tail has no vertex part (pre-S3).
    std::vector<uint8_t> ss3VertexPart(TestCase &t, const std::vector<uint8_t> &blob, uint32_t &vcount)
    {
        vcount = 0u;
        const Ss3Tail tail = ss3ParseTail(blob);
        if (!tail.ok)
        {
            t.Fail("v3 tail parses (S3 vertex part)");
            return {};
        }
        if (tail.tailEnd - tail.clutEnd < sizeof(uint32_t))
        {
            t.Fail("tail has a vertex part (missing pre-S3)");
            return {};
        }
        std::memcpy(&vcount, blob.data() + tail.clutEnd, sizeof(vcount));
        if (vcount > 3u || tail.tailEnd - tail.clutEnd != sizeof(uint32_t) + size_t{vcount} * 36u)
        {
            t.Fail("vertex part has exact size for its count");
            return {};
        }
        return std::vector<uint8_t>(blob.data() + tail.clutEnd, blob.data() + tail.tailEnd);
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

        tc.Run("ss3 s2: CLUT tail carries the interface palette indices", [](TestCase &t)
        {
            if (!ss3WantParallelGpu())
                return;
            std::unique_ptr<GSRasterBackend> be = ps2x_gs_parallel::create(nullptr);
            t.IsNotNull(be.get(), "backend created");
            if (!be)
                return;
            std::vector<uint8_t> blob;
            be->SavestateSave(blob);
            if (!ss3HaveParallelBlob(t, blob))
                return;
            const Ss3Tail tail = ss3ParseTail(blob);
            t.IsTrue(tail.ok, "v3 footer+tail present (a v2 blob fails here)");
            if (!tail.ok)
                return;
            t.IsTrue(tail.hasClut, "fresh backend saves its CLUT ring");
            t.Equals(tail.ringN, uint64_t{1u << 20}, "ring is CLUTInstances x CLUTSize");
            t.Equals(tail.base, 0u, "fresh renderer base cursor");
            t.Equals(tail.next, 0u, "fresh renderer next cursor");
            t.Equals(tail.iface, 0u, "fresh interface palette index");
            t.Equals(tail.latest, 0u, "fresh interface latest index");

            // Patch a copy (ring bytes + all four cursors) and round-trip it:
            // the load must restore the patched bytes and indices. The
            // re-save flushes first, and a render-pass flush normalizes the
            // indices (latest=iface, base=next=iface), so (1,2,3,4) reads
            // back as (3,3,3,3) while the ring reads back exactly. Real
            // saves are therefore always normalized; pre-S2 the load
            // refuses the v3 shape and the re-save has no indices to read.
            std::vector<uint8_t> patched = blob;
            for (const size_t o : {size_t{0}, size_t{1000}, size_t{(1u << 20) - 1}})
                patched[tail.ringOff + o] ^= 0xFFu;
            ss3PutU32(patched, tail.ringOff + tail.ringN, 1u);
            ss3PutU32(patched, tail.ringOff + tail.ringN + 4u, 2u);
            ss3PutU32(patched, tail.ringOff + tail.ringN + 8u, 3u);
            ss3PutU32(patched, tail.ringOff + tail.ringN + 12u, 4u);
            t.IsTrue(be->SavestateLoad(patched.data(), patched.size()), "patched v3 blob loads");
            std::vector<uint8_t> blob2;
            be->SavestateSave(blob2);
            if (!ss3HaveParallelBlob(t, blob2))
                return;
            const Ss3Tail tail2 = ss3ParseTail(blob2);
            t.IsTrue(tail2.ok && tail2.hasClut, "re-saved blob parses");
            if (!tail2.ok)
                return;
            t.Equals(tail2.ringN, tail.ringN, "ring size stable");
            t.Equals(tail2.ringOff, tail.ringOff, "tail offset stable");
            t.Equals(tail2.base, 3u, "renderer base cursor rewound to the restored bank");
            t.Equals(tail2.next, 3u, "renderer next cursor rewound to the restored bank");
            t.Equals(tail2.iface, 3u, "interface palette index round-trips");
            t.Equals(tail2.latest, 3u, "interface latest index rewound to the restored bank");
            t.IsTrue(std::memcmp(blob2.data() + tail2.ringOff, patched.data() + tail.ringOff,
                                 static_cast<size_t>(tail.ringN)) == 0,
                     "ring bytes round-trip exactly");
        });

        tc.Run("ss3 s3: a split IMAGE upload defers the save", [](TestCase &t)
        {
            if (!ss3WantParallelGpu())
                return;
            std::unique_ptr<GSRasterBackend> be = ps2x_gs_parallel::create(nullptr);
            t.IsNotNull(be.get(), "backend created");
            if (!be)
                return;
            t.IsTrue(be->SavestateIdle(), "fresh backend is idle");
            t.Equals(be->SavestateBusyReason(), std::string(), "no busy reason when idle");
            // 8x8 PSMCT32 host->local transfer: 32 qwords over 16 IMAGE words.
            be->RawWriteRegister(0x50, uint64_t{1} << 48); // BITBLTBUF: DBP=0 DBW=1 DPSM=0
            be->RawWriteRegister(0x51, 0u); // TRXPOS: 0
            be->RawWriteRegister(0x52, uint64_t{8} | (uint64_t{8} << 32)); // TRXREG: 8x8
            be->RawWriteRegister(0x53, 0u); // TRXDIR: host->local
            if (!ss3BackendInitOk(t))
                return;
            t.IsTrue(!be->SavestateIdle(), "open transfer is not idle (idle pre-S3)");
            t.Equals(be->SavestateBusyReason(), std::string("gs-transfer"), "transfer names its reason");
            // First fragment: tag (NLOOP=64 IMAGE) + 4 of 16 words.
            std::vector<uint8_t> frag(16 + 4 * 16, 0);
            const uint64_t tagLo = 64u | (uint64_t{2} << 58); // NLOOP=64 FLG=IMAGE
            std::memcpy(frag.data(), &tagLo, sizeof(tagLo));
            be->RawGifPacket(1, frag.data(), static_cast<uint32_t>(frag.size()));
            t.IsTrue(!be->SavestateIdle(), "partial upload is not idle");
            t.Equals(be->SavestateBusyReason(), std::string("gs-transfer"), "partial upload keeps the reason");
            // Continuation fragments (no tag) until the guest completes it.
            const std::vector<uint8_t> chunk(4 * 16, 0);
            int chunks = 0;
            for (; chunks < 6 && !be->SavestateIdle(); ++chunks)
                be->RawGifPacket(1, chunk.data(), static_cast<uint32_t>(chunk.size()));
            t.IsTrue(be->SavestateIdle(), "completed upload is idle again");
            t.Equals(be->SavestateBusyReason(), std::string(), "reason clears on completion");
            t.Equals(chunks, 3, "8+8+8+8 qwords complete the 32-qword transfer");
            // A save/load round trip across the completion is bit-exact.
            std::vector<uint8_t> blob1, blob2;
            be->SavestateSave(blob1);
            if (!ss3HaveParallelBlob(t, blob1))
                return;
            t.IsTrue(be->SavestateLoad(blob1.data(), blob1.size()), "post-transfer blob loads");
            be->SavestateSave(blob2);
            t.IsTrue(blob1 == blob2, "save/load/save across a completed transfer is bit-exact");
        });

        tc.Run("ss3 s3: retained strip vertices survive save/load", [](TestCase &t)
        {
            if (!ss3WantParallelGpu())
                return;
            std::vector<uint8_t> vtx2;
            {
                std::unique_ptr<GSRasterBackend> be = ps2x_gs_parallel::create(nullptr);
                t.IsNotNull(be.get(), "backend A created");
                if (!be)
                    return;
                ss3StripSetup(*be);
                ss3Kick(*be, 100u, 200u, 300u);
                ss3Kick(*be, 110u, 210u, 310u);
                if (!ss3BackendInitOk(t))
                    return;
                std::vector<uint8_t> blob1;
                be->SavestateSave(blob1);
                if (!ss3HaveParallelBlob(t, blob1))
                    return;
                uint32_t vcount = 0u;
                const std::vector<uint8_t> vtx1 = ss3VertexPart(t, blob1, vcount);
                t.Equals(vcount, 2u, "two retained vertices saved");
                if (vtx1.empty())
                    return;
                t.IsTrue(be->SavestateLoad(blob1.data(), blob1.size()), "strip blob loads");
                std::vector<uint8_t> blob1b;
                be->SavestateSave(blob1b);
                uint32_t vcount1b = 0u;
                const std::vector<uint8_t> vtx1b = ss3VertexPart(t, blob1b, vcount1b);
                t.IsTrue(vtx1 == vtx1b, "retained vertices restore exactly");
                // Continue the strip after the load and save again.
                ss3Kick(*be, 120u, 220u, 320u);
                std::vector<uint8_t> blob2;
                be->SavestateSave(blob2);
                uint32_t vcount2 = 0u;
                vtx2 = ss3VertexPart(t, blob2, vcount2);
                t.Equals(vcount2, 3u, "continued strip holds three vertices");
                if (vtx2.empty())
                    return;
            }
            // Uninterrupted: the same three kicks with no save in the middle.
            std::unique_ptr<GSRasterBackend> beB = ps2x_gs_parallel::create(nullptr);
            t.IsNotNull(beB.get(), "backend B created");
            if (!beB)
                return;
            ss3StripSetup(*beB);
            ss3Kick(*beB, 100u, 200u, 300u);
            ss3Kick(*beB, 110u, 210u, 310u);
            ss3Kick(*beB, 120u, 220u, 320u);
            if (!ss3BackendInitOk(t))
                return;
            std::vector<uint8_t> blob3;
            beB->SavestateSave(blob3);
            if (!ss3HaveParallelBlob(t, blob3))
                return;
            uint32_t vcount3 = 0u;
            const std::vector<uint8_t> vtx3 = ss3VertexPart(t, blob3, vcount3);
            t.IsTrue(vtx2 == vtx3, "post-load continuation matches the uninterrupted queue");
            if (vtx3.size() != sizeof(uint32_t) + 3u * 36u)
                return;
            // Structural spot checks (entries are [x y z s t q rgba fog u v];
            // X/Y carry an unknown render-pass offset, so only differences).
            int32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
            uint32_t z0 = 0, z1 = 0, rgba = 0;
            float s = 0, q = 0, fog = 0;
            uint16_t u = 0, v = 0;
            const uint8_t *e0 = vtx3.data() + sizeof(uint32_t);
            const uint8_t *e1 = e0 + 36u;
            std::memcpy(&x0, e0, 4);
            std::memcpy(&x1, e1, 4);
            std::memcpy(&y0, e0 + 4, 4);
            std::memcpy(&y1, e1 + 4, 4);
            std::memcpy(&z0, e0 + 8, 4);
            std::memcpy(&z1, e1 + 8, 4);
            std::memcpy(&rgba, e0 + 24, 4);
            std::memcpy(&s, e0 + 12, 4);
            std::memcpy(&q, e0 + 20, 4);
            std::memcpy(&fog, e0 + 28, 4);
            std::memcpy(&u, e0 + 32, 2);
            std::memcpy(&v, e0 + 34, 2);
            t.Equals(x1 - x0, 10, "vertex X step");
            t.Equals(y1 - y0, 10, "vertex Y step");
            t.Equals(z0, 300u, "vertex Z kept");
            t.Equals(z1, 310u, "vertex Z step");
            t.Equals(rgba, 0x44332211u, "vertex RGBA kept");
            t.IsTrue(s == 0.5f && q == 1.0f, "vertex ST/Q kept");
            t.IsTrue(fog == 90.0f, "vertex fog kept");
            t.IsTrue(u == 10u && v == 20u, "vertex UV kept");
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
    });
}
