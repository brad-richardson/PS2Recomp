#include "MiniTest.h"
#include "ps2_ssx3_anim_pick_guard.h"

#include <vector>

// HNG2: the zero-weight animation-pick guard on a crafted picker table.
namespace
{
    using namespace ps2_ssx3_anim_pick;
    constexpr uint32_t kGp = 0x4a30f0u;
    constexpr uint32_t kEventTable = 0x00b00000u;
    constexpr uint32_t kHandles = 0x00a00000u;
    constexpr uint32_t kRa = 0x104d28u;

    void w32(std::vector<uint8_t> &ram, uint32_t a, uint32_t v) { std::memcpy(ram.data() + a, &v, 4u); }
    void entry(std::vector<uint8_t> &ram, uint32_t event, int16_t start, int16_t count)
    {
        w32(ram, kEventTable + event * 4u, static_cast<uint16_t>(start) | (static_cast<uint32_t>(static_cast<uint16_t>(count)) << 16));
    }
    void record(std::vector<uint8_t> &ram, uint32_t i, uint32_t anim, uint32_t weight, uint32_t flags)
    {
        const uint32_t a = kRecords + i * kRecordSize;
        w32(ram, a, anim);
        w32(ram, a + 4u, weight);
        w32(ram, a + 8u, flags);
    }
    void setReg(R5900Context &ctx, int reg, uint32_t v) { ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(v))); }

    std::vector<uint8_t> world()
    {
        std::vector<uint8_t> ram(PS2_RAM_SIZE);
        w32(ram, kGp + kHandleTableGpOff, kHandles);
        w32(ram, kHandles + kHandleTableOff, 0x2000u); // anim 0: bank 0, resident
        w32(ram, kGp + kNoAnimKeyGpOff, 0xffffffffu);
        entry(ram, 5, 10, 3);      // three variants, one matches mask 2
        record(ram, 10, 40, 100, 0x1u);
        record(ram, 11, 41, 100, 0x2u);
        record(ram, 12, 42, 100, 0x4u);
        entry(ram, 7, 20, 1);      // single variant: returned directly
        entry(ram, 8, 30, 2);      // two variants, none for mask 2
        record(ram, 30, 50, 100, 0x1u);
        record(ram, 31, 51, 100, 0x4u);
        entry(ram, 432, 0x1234, 0); // crafted count-0 event (stock 432/433 have no records)
        return ram;
    }

    R5900Context call(uint32_t event, uint32_t mask, uint32_t pc = kPicker)
    {
        R5900Context ctx{};
        setReg(ctx, 4, kEventTable);
        setReg(ctx, 5, event);
        setReg(ctx, 6, mask);
        setReg(ctx, 28, kGp);
        setReg(ctx, 31, kRa);
        setReg(ctx, 2, 0xdeadbeefu);
        ctx.pc = pc;
        return ctx;
    }
}

void register_ps2_ssx3_anim_pick_guard_tests()
{
    MiniTest::Case("Ps2Ssx3AnimPickGuard", [](TestCase &tc)
                   {
        tc.Run("knob: default on, 0 restores the stock trap", [](TestCase &t)
               {
            t.IsTrue(parseEnabled(nullptr), "unset");
            t.IsTrue(parseEnabled(""), "empty");
            t.IsTrue(parseEnabled("1"), "1");
            t.IsFalse(parseEnabled("0"), "0"); });
        tc.Run("count-0 event at the picker entry returns anim 0 to ra", [](TestCase &t)
               {
            auto ram = world();
            auto ctx = call(432, 2);
            Decision d = Decision::Stock;
            t.IsTrue(guardEntry(ram.data(), &ctx, &d), "guarded");
            t.IsTrue(d == Decision::Guard, "decision");
            t.Equals(getRegU32(&ctx, 2), kSafeAnim, "v0 = anim 0");
            t.Equals(ctx.pc, kRa, "returns to ra");
            t.Equals(getRegU32(&ctx, 4), kEventTable, "a0 untouched"); });
        tc.Run("no variant for the mask (sum 0) is guarded too", [](TestCase &t)
               {
            auto ram = world();
            auto ctx = call(8, 2);
            t.IsTrue(guardEntry(ram.data(), &ctx), "guarded");
            t.Equals(getRegU32(&ctx, 2), kSafeAnim, "v0"); });
        tc.Run("normal picks run the original", [](TestCase &t)
               {
            auto ram = world();
            for (uint32_t ev : {5u, 7u})
            {
                auto ctx = call(ev, 2);
                Decision d = Decision::Guard;
                t.IsFalse(guardEntry(ram.data(), &ctx, &d), "original runs");
                t.IsTrue(d == Decision::Stock, "stock");
                t.Equals(ctx.pc, kPicker, "pc untouched");
                t.Equals(getRegU32(&ctx, 2), 0xdeadbeefu, "v0 untouched");
            }
            auto ctx = call(8, 1); // mask 1 matches record 30
            t.IsFalse(guardEntry(ram.data(), &ctx), "mask 1 has weight"); });
        tc.Run("checkpoint resume inside the picker runs the original", [](TestCase &t)
               {
            auto ram = world();
            auto ctx = call(432, 2, 0x3117b8u);
            t.IsFalse(guardEntry(ram.data(), &ctx), "resume label");
            t.Equals(ctx.pc, 0x3117b8u, "pc untouched"); });
        tc.Run("anim 0 not resident: left to the stock path", [](TestCase &t)
               {
            auto ram = world();
            w32(ram, kHandles + kHandleTableOff, 0xffffffffu);
            auto ctx = call(432, 2);
            Decision d = Decision::Stock;
            t.IsFalse(guardEntry(ram.data(), &ctx, &d), "not guarded");
            t.IsTrue(d == Decision::Unsafe, "unsafe");
            w32(ram, kGp + kHandleTableGpOff, 0u);
            t.IsTrue(decide(ram.data(), kEventTable, 432, 2, kGp) == Decision::Unsafe, "no handle table"); });
        tc.Run("unreadable table or a garbage span stays stock", [](TestCase &t)
               {
            auto ram = world();
            t.IsTrue(decide(ram.data(), 0x01fffffeu, 0, 2, kGp) == Decision::Stock, "entry past RAM");
            entry(ram, 9, 0, 0x7fff);
            t.IsTrue(decide(ram.data(), kEventTable, 9, 2, kGp) == Decision::Stock, "span over the cap"); }); });
}
