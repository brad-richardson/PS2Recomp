#include "MiniTest.h"
#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_savestate.h"
#include <new>
#include "runtime/ps2_vu0.h"
#include "runtime/ps2_vu_state.h"
#include "../../ps2xRuntime/src/lib/ps2_savestate_internal.h"
#include "vu1_recomp_fixture.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    // VZ1: fixed-seed RNG for the randomized EE-side VU0 context.
    struct LeanRnd
    {
        uint64_t s;
        uint32_t next(uint32_t n)
        {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<uint32_t>((s >> 33) % n);
        }
    };

    uint32_t leanEdgeBits(LeanRnd &rnd)
    {
        static const uint32_t kEdges[] = {
            0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u,
            0x00000001u, 0x007FFFFFu, 0x7F7FFFFFu, 0x7F800000u,
            0xFF800000u, 0x7FC00001u,
        };
        if (rnd.next(4u) == 0u)
            return kEdges[rnd.next(static_cast<uint32_t>(sizeof(kEdges) / sizeof(kEdges[0])))];
        return rnd.next(0xFFFFFFFFu);
    }

    void randomizeVu0Context(R5900Context &ctx, LeanRnd &rnd)
    {
        for (uint32_t reg = 0; reg < 32u; ++reg)
        {
            alignas(16) uint32_t words[4];
            for (uint32_t lane = 0; lane < 4u; ++lane)
                words[lane] = leanEdgeBits(rnd);
            ctx.vu0_vf[reg] = _mm_loadu_si128(reinterpret_cast<const __m128i *>(words));
        }
        for (uint32_t reg = 0; reg < 16u; ++reg)
            ctx.vi[reg] = static_cast<uint16_t>(rnd.next(0x10000u));
        alignas(16) uint32_t accWords[4];
        for (uint32_t lane = 0; lane < 4u; ++lane)
            accWords[lane] = leanEdgeBits(rnd);
        ctx.vu0_acc = _mm_loadu_si128(reinterpret_cast<const __m128i *>(accWords));
        alignas(16) uint32_t rWords[4];
        for (uint32_t lane = 0; lane < 4u; ++lane)
            rWords[lane] = leanEdgeBits(rnd);
        ctx.vu0_r = _mm_loadu_si128(reinterpret_cast<const __m128i *>(rWords));
        {
            uint32_t qBits = leanEdgeBits(rnd);
            float q = 0.0f;
            std::memcpy(&q, &qBits, sizeof(q));
            ctx.vu0_q = q;
        }
        {
            uint32_t pBits = leanEdgeBits(rnd);
            float p = 0.0f;
            std::memcpy(&p, &pBits, sizeof(p));
            ctx.vu0_p = p;
        }
        {
            uint32_t iBits = leanEdgeBits(rnd);
            float i = 0.0f;
            std::memcpy(&i, &iBits, sizeof(i));
            ctx.vu0_i = i;
        }
        ctx.vu0_pc = rnd.next(0x1000u) & ~0x7u;
        ctx.vu0_mac_flags = rnd.next(0x10000u);
        ctx.vu0_clip_flags = rnd.next(0x1000000u);
        ctx.vu0_status = static_cast<uint16_t>(rnd.next(0x10000u));
        ctx.vu0_itop = rnd.next(16u);
        ctx.vu0_fbrst = rnd.next(0x10000u);
    }

    void poisonState(VuState &state)
    {
        std::memset(&state, 0xA5, sizeof(state));
    }

    bool stateBytesEqual(const VuState &a, const VuState &b)
    {
        return std::memcmp(&a, &b, sizeof(VuState)) == 0;
    }

    // Fields beginProgram() assigns before run() reads them. The import
    // contract leaves them alone, so the 0xA5 poison survives there on the
    // lean path while the exact path zeroes them. Mask them out: the import
    // differential covers every import-assigned byte plus cycles plus all
    // padding (the exact path memsets the whole struct, so any lean residue
    // anywhere else fails the masked compare).
    void maskBeginProgramFields(VuState &s)
    {
        s.ebit = false;
        s.haltAfterDelaySlot = false;
        s.stoppedByD = false;
        s.stoppedByT = false;
        s.top = 0;
        s.branchPending = false;
        s.branchTarget = 0;
        s.branchDelay = 0;
    }

    bool importBytesEqual(VuState a, VuState b)
    {
        maskBeginProgramFields(a);
        maskBeginProgramFields(b);
        return stateBytesEqual(a, b);
    }

    void printStateDiff(const VuState &ref, const VuState &got)
    {
        for (uint32_t reg = 0; reg < 32u; ++reg)
            if (std::memcmp(ref.vf[reg], got.vf[reg], sizeof(ref.vf[reg])) != 0)
                std::fprintf(stderr, "  vf%u ref %g %g %g %g got %g %g %g %g\n", reg,
                             ref.vf[reg][0], ref.vf[reg][1], ref.vf[reg][2], ref.vf[reg][3],
                             got.vf[reg][0], got.vf[reg][1], got.vf[reg][2], got.vf[reg][3]);
        for (uint32_t reg = 0; reg < 16u; ++reg)
            if (ref.vi[reg] != got.vi[reg])
                std::fprintf(stderr, "  vi%u ref %d got %d\n", reg, ref.vi[reg], got.vi[reg]);
        std::fprintf(stderr,
                     "  mac %x/%x status %x/%x clip %x/%x q %g/%g p %g/%g i %g/%g r %x/%x cycles %llu/%llu pc %x/%x acc %d itop %x/%x d %d/%d t %d/%d\n",
                     ref.mac, got.mac, ref.status, got.status, ref.clip, got.clip, ref.q, got.q,
                     ref.p, got.p, ref.i, got.i, ref.r, got.r,
                     static_cast<unsigned long long>(ref.cycles),
                     static_cast<unsigned long long>(got.cycles), ref.pc, got.pc,
                     std::memcmp(ref.acc, got.acc, sizeof(ref.acc)) != 0, ref.itop, got.itop,
                     ref.dBitEnabled, got.dBitEnabled, ref.tBitEnabled, got.tBitEnabled);
    }

    struct LeanSnapshot
    {
        VuState state{};
        std::vector<uint8_t> data;
    };

    // Seeds a fresh interpreter from an imported state (poisoned-then-imported
    // by the caller) and runs it. Pipelines always start from execute()'s own
    // resetScheduler on both paths, so only the seeded m_state can differ.
    LeanSnapshot runLeanOnce(uint8_t *code, const VuState &imported, const std::vector<uint8_t> &data,
                             uint32_t startPc, uint32_t budget, GS &gs)
    {
        VU0Interpreter vu;
        std::memcpy(&vu.state(), &imported, sizeof(VuState));
        LeanSnapshot snap;
        snap.data = data;
        vu.execute(code, PS2_VU0_CODE_SIZE, snap.data.data(), PS2_VU0_DATA_SIZE, gs, nullptr, startPc, 0u, 0u,
                   budget);
        std::memcpy(&snap.state, &vu.state(), sizeof(VuState));
        return snap;
    }
}

void register_ps2_vu0_lean_entry_tests()
{
    MiniTest::Case("PS2VU0LeanEntry", [](TestCase &tc)
    {
        tc.Run("knob defaults on when env unset", [](TestCase &t)
        {
            if (std::getenv("PS2X_VU0_LEAN_ENTRY") == nullptr)
                t.IsTrue(PS2Runtime::vu0LeanEntryEnabled(), "PS2X_VU0_LEAN_ENTRY defaults on");
            else
                std::fprintf(stderr, "[vz1] PS2X_VU0_LEAN_ENTRY set in env; default assertion skipped\n");
        });

        tc.Run("poisoned import identical on both paths", [](TestCase &t)
        {
            std::vector<uint8_t> code(vu1_fixture::kVu0CodeSize, 0u);
            LeanRnd rnd{0x51A1u};
            uint64_t programs = 0, mismatches = 0;
            for (uint32_t image = 0; image < vu1_fixture::kVu0ImageCount; ++image)
            {
                const std::vector<vu1_fixture::Program> progs = vu1_fixture::buildVu0Image(image, code.data());
                for (const vu1_fixture::Program &prog : progs)
                {
                    (void)prog;
                    ++programs;
                    R5900Context ctx;
                    randomizeVu0Context(ctx, rnd);
                    VuState exact{}, lean{};
                    poisonState(exact);
                    poisonState(lean);
                    PS2Runtime::importVu0Context(&ctx, exact, false);
                    PS2Runtime::importVu0Context(&ctx, lean, true);
                    if (!importBytesEqual(exact, lean))
                    {
                        if (++mismatches <= 3u)
                        {
                            std::fprintf(stderr, "[vz1] import mismatch image %u\n", image);
                            printStateDiff(exact, lean);
                        }
                        continue;
                    }
                    // Anti-vacuity: the lean path must really skip the
                    // whole-struct clear, so beginProgram-owned fields still
                    // hold the poison here.
                    if (lean.top != 0xA5A5A5A5u)
                    {
                        ++mismatches;
                        std::fprintf(stderr, "[vz1] lean import cleared top (image %u)\n", image);
                    }
                    if (lean.cycles != 0u)
                    {
                        ++mismatches;
                        std::fprintf(stderr, "[vz1] lean import left cycles=%llu (image %u)\n",
                                     static_cast<unsigned long long>(lean.cycles), image);
                    }
                    // Idle-path invariants: a start with no code memory
                    // returns after the import, so cycles must be zero and
                    // every padding byte must read back as zero.
                }
            }
            std::fprintf(stderr, "[vz1] import differential: %llu programs %llu mismatches\n",
                         static_cast<unsigned long long>(programs),
                         static_cast<unsigned long long>(mismatches));
            t.Equals(mismatches, uint64_t{0}, "lean import matches the exact import on poisoned state");
            t.IsTrue(programs > 100u, "differential covered the fixture programs");
        });

        tc.Run("executed state identical at tiny and normal budgets", [](TestCase &t)
        {
            std::vector<uint8_t> code(vu1_fixture::kVu0CodeSize, 0u);
            GS gs;
            LeanRnd rnd{0x1EA907u};
            uint64_t runs = 0, mismatches = 0, programs = 0;
            for (uint32_t image = 0; image < vu1_fixture::kVu0ImageCount; ++image)
            {
                const std::vector<vu1_fixture::Program> progs = vu1_fixture::buildVu0Image(image, code.data());
                for (const vu1_fixture::Program &prog : progs)
                {
                    ++programs;
                    std::vector<uint8_t> data(PS2_VU0_DATA_SIZE, 0u);
                    for (uint32_t i = 0; i < 64u * 16u; i += 4u)
                    {
                        const float value =
                            static_cast<float>(static_cast<int32_t>(rnd.next(2001u)) - 1000) / 64.0f;
                        std::memcpy(data.data() + i, &value, sizeof(value));
                    }
                    R5900Context ctx;
                    randomizeVu0Context(ctx, rnd);
                    VuState exactImport{}, leanImport{};
                    poisonState(exactImport);
                    poisonState(leanImport);
                    PS2Runtime::importVu0Context(&ctx, exactImport, false);
                    PS2Runtime::importVu0Context(&ctx, leanImport, true);
                    for (uint32_t budget : {2u, 4096u})
                    {
                        ++runs;
                        const LeanSnapshot ref = runLeanOnce(code.data(), exactImport, data, prog.startPc, budget, gs);
                        const LeanSnapshot got = runLeanOnce(code.data(), leanImport, data, prog.startPc, budget, gs);
                        if (stateBytesEqual(ref.state, got.state) && ref.data == got.data)
                            continue;
                        if (++mismatches <= 3u)
                        {
                            std::fprintf(stderr, "[vz1] exec mismatch image %u pc 0x%x budget %u\n", image,
                                         prog.startPc, budget);
                            printStateDiff(ref.state, got.state);
                        }
                    }
                }
            }
            std::fprintf(stderr, "[vz1] exec differential: %llu programs %llu runs %llu mismatches\n",
                         static_cast<unsigned long long>(programs), static_cast<unsigned long long>(runs),
                         static_cast<unsigned long long>(mismatches));
            t.Equals(mismatches, uint64_t{0}, "lean and exact paths agree at tiny and normal budgets");
            t.IsTrue(programs > 100u, "differential covered the fixture programs");
        });

        tc.Run("savestate bytes identical; load plus fresh execute continues identically", [](TestCase &t)
        {
            std::vector<uint8_t> code(vu1_fixture::kVu0CodeSize, 0u);
            GS gs;
            LeanRnd rnd{0x5AFE0u};
            uint64_t programs = 0, mismatches = 0;
            for (uint32_t image = 0; image < vu1_fixture::kVu0ImageCount; ++image)
            {
                const std::vector<vu1_fixture::Program> progs = vu1_fixture::buildVu0Image(image, code.data());
                for (const vu1_fixture::Program &prog : progs)
                {
                    ++programs;
                    std::vector<uint8_t> data(PS2_VU0_DATA_SIZE, 0u);
                    R5900Context ctx;
                    randomizeVu0Context(ctx, rnd);
                    VuState exactImport{}, leanImport{};
                    poisonState(exactImport);
                    poisonState(leanImport);
                    PS2Runtime::importVu0Context(&ctx, exactImport, false);
                    PS2Runtime::importVu0Context(&ctx, leanImport, true);
                    // VBK1: VuSavestate writes the pipeline structs raw (w.pod),
                    // padding included, and their {} member initializers do not
                    // zero padding, so two stack-constructed units can save
                    // different padding bytes (m_fdiv: 17 bytes of fields in 24).
                    // Construct both in zeroed storage so only real state differs.
                    alignas(VU0Interpreter) unsigned char refStorage[sizeof(VU0Interpreter)] = {};
                    alignas(VU0Interpreter) unsigned char gotStorage[sizeof(VU0Interpreter)] = {};
                    VU0Interpreter &refVu = *new (refStorage) VU0Interpreter();
                    VU0Interpreter &gotVu = *new (gotStorage) VU0Interpreter();
                    struct Destroy
                    {
                        VU0Interpreter &a, &b;
                        ~Destroy() { a.~VU0Interpreter(); b.~VU0Interpreter(); }
                    } destroy{refVu, gotVu};
                    std::memcpy(&refVu.state(), &exactImport, sizeof(VuState));
                    std::memcpy(&gotVu.state(), &leanImport, sizeof(VuState));
                    std::vector<uint8_t> refData = data, gotData = data;
                    refVu.execute(code.data(), PS2_VU0_CODE_SIZE, refData.data(), PS2_VU0_DATA_SIZE, gs, nullptr,
                                  prog.startPc, 0u, 0u, 4096u);
                    gotVu.execute(code.data(), PS2_VU0_CODE_SIZE, gotData.data(), PS2_VU0_DATA_SIZE, gs, nullptr,
                                  prog.startPc, 0u, 0u, 4096u);
                    ps2_savestate::Writer refW, gotW;
                    VuSavestate::save(refVu, refW);
                    VuSavestate::save(gotVu, gotW);
                    if (refW.buf != gotW.buf)
                    {
                        if (++mismatches <= 3u)
                        {
                            size_t at = 0u;
                            while (at < refW.buf.size() && at < gotW.buf.size() && refW.buf[at] == gotW.buf[at])
                                ++at;
                            std::fprintf(stderr, "[vz1] savestate mismatch image %u pc 0x%x (%zu vs %zu bytes, first diff at %zu)\n",
                                         image, prog.startPc, refW.buf.size(), gotW.buf.size(), at);
                        }
                        continue;
                    }
                    // Continuation: the runtime's only VU0 continuation is a
                    // fresh execute() after load.
                    VU0Interpreter refCont, gotCont;
                    ps2_savestate::Reader refR(refW.buf.data(), refW.buf.size());
                    ps2_savestate::Reader gotR(gotW.buf.data(), gotW.buf.size());
                    bool okLoad = VuSavestate::load(refCont, refR) && VuSavestate::load(gotCont, gotR);
                    if (!okLoad)
                    {
                        ++mismatches;
                        std::fprintf(stderr, "[vz1] savestate load failed image %u pc 0x%x\n", image, prog.startPc);
                        continue;
                    }
                    refCont.execute(code.data(), PS2_VU0_CODE_SIZE, refData.data(), PS2_VU0_DATA_SIZE, gs, nullptr,
                                    prog.startPc, 0u, 0u, 4096u);
                    gotCont.execute(code.data(), PS2_VU0_CODE_SIZE, gotData.data(), PS2_VU0_DATA_SIZE, gs, nullptr,
                                    prog.startPc, 0u, 0u, 4096u);
                    if (!stateBytesEqual(refCont.state(), gotCont.state()) || refData != gotData)
                    {
                        if (++mismatches <= 3u)
                        {
                            std::fprintf(stderr, "[vz1] post-load continuation mismatch image %u pc 0x%x\n", image,
                                         prog.startPc);
                            printStateDiff(refCont.state(), gotCont.state());
                        }
                    }
                }
            }
            std::fprintf(stderr, "[vz1] savestate differential: %llu programs %llu mismatches\n",
                         static_cast<unsigned long long>(programs),
                         static_cast<unsigned long long>(mismatches));
            t.Equals(mismatches, uint64_t{0}, "savestates byte-stable; continuation identical");
        });
    });
}
