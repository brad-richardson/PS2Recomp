#include "MiniTest.h"
#include "ps2_ts2_split60.h"

#include <cstring>
#include <vector>

// SPL1: the split120 half-word sets (TK34 crash body, SPL1 bounce). Pure
// helper tests on a fake RDRAM; the knobs' env parse is knobOn().
namespace
{
uint32_t rd(const std::vector<uint8_t> &ram, uint32_t a)
{
    uint32_t v = 0;
    std::memcpy(&v, ram.data() + a, 4);
    return v;
}
void wr(std::vector<uint8_t> &ram, uint32_t a, uint32_t v)
{
    std::memcpy(ram.data() + a, &v, 4);
}
std::vector<uint8_t> stockRam()
{
    std::vector<uint8_t> ram(0x500000u, 0u);
    for (uint32_t a : ps2_ts2_split60::kCrashWords)
        wr(ram, a, ps2_ts2_split60::kCrashBodyStock);
    wr(ram, 0x49c13cu, ps2_ts2_split60::kBounceStock);
    return ram;
}
} // namespace

void register_ps2_ts2_split60_tests()
{
    using namespace ps2_ts2_split60;
    MiniTest::Case("Ps2Ts2Split60HalfWords", [](TestCase &tc)
                   {
        tc.Run("constants are the FH12 rows (2.65 / 1.325, 1/60 / 1/120)", [](TestCase &t)
               {
            float f = 0.0f;
            std::memcpy(&f, &kBounceStock, 4);
            t.IsTrue(f == 2.65f, "bounce stock 2.65");
            std::memcpy(&f, &kBounceHalf, 4);
            t.IsTrue(f == 1.325f, "bounce half 1.325");
            float s = 0.0f, h = 0.0f;
            std::memcpy(&s, &kCrashBodyStock, 4);
            std::memcpy(&h, &kCrashBodyHalf, 4);
            t.IsTrue(s > 0.016666f && s < 0.016667f, "crash stock 1/60");
            t.IsTrue(h > 0.0083333f && h < 0.0083334f, "crash half 1/120"); });
        tc.Run("knobOn accepts exactly 1", [](TestCase &t)
               {
            t.IsTrue(knobOn("1"), "1");
            t.IsFalse(knobOn(nullptr), "unset");
            t.IsFalse(knobOn(""), "empty");
            t.IsFalse(knobOn("0"), "0");
            t.IsFalse(knobOn("10"), "10");
            t.IsFalse(knobOn("on"), "on"); });
        tc.Run("begin/finish pair: half then stock, every word", [](TestCase &t)
               {
            std::vector<uint8_t> ram = stockRam();
            HalfWords crash{"t-crash", kCrashWords, 4u, kCrashBodyStock, kCrashBodyHalf};
            HalfWords bounce{"t-bounce", kBounceWords, 1u, kBounceStock, kBounceHalf};
            t.IsTrue(halfWordsWrite(ram.data(), crash, kCrashBodyHalf), "crash begin");
            t.IsTrue(halfWordsWrite(ram.data(), bounce, kBounceHalf), "bounce begin");
            for (uint32_t a : kCrashWords)
                t.Equals(rd(ram, a), kCrashBodyHalf, "crash word half");
            t.Equals(rd(ram, 0x49c13cu), kBounceHalf, "bounce half");
            t.IsTrue(halfWordsWrite(ram.data(), crash, kCrashBodyStock), "crash finish");
            t.IsTrue(halfWordsWrite(ram.data(), bounce, kBounceStock), "bounce finish");
            for (uint32_t a : kCrashWords)
                t.Equals(rd(ram, a), kCrashBodyStock, "crash word stock");
            t.Equals(rd(ram, 0x49c13cu), kBounceStock, "bounce stock"); });
        tc.Run("a savestate taken mid-half (half value at begin) is accepted", [](TestCase &t)
               {
            std::vector<uint8_t> ram = stockRam();
            wr(ram, 0x49c13cu, kBounceHalf);
            HalfWords bounce{"t-bounce", kBounceWords, 1u, kBounceStock, kBounceHalf};
            t.IsTrue(halfWordsWrite(ram.data(), bounce, kBounceHalf), "begin on half");
            t.IsTrue(halfWordsWrite(ram.data(), bounce, kBounceStock), "finish");
            t.Equals(rd(ram, 0x49c13cu), kBounceStock, "restored");
            t.IsFalse(bounce.refused, "not refused"); });
        tc.Run("a foreign value refuses the set and leaves RAM untouched", [](TestCase &t)
               {
            std::vector<uint8_t> ram = stockRam();
            wr(ram, 0x49bef4u, 0x3f800000u); // 1.0: not this game's word
            HalfWords crash{"t-crash", kCrashWords, 4u, kCrashBodyStock, kCrashBodyHalf};
            t.IsFalse(halfWordsWrite(ram.data(), crash, kCrashBodyHalf), "refused");
            t.IsTrue(crash.refused, "set disabled");
            t.Equals(rd(ram, 0x49be38u), kCrashBodyStock, "first word untouched");
            t.Equals(rd(ram, 0x49bef4u), 0x3f800000u, "foreign word untouched");
            wr(ram, 0x49bef4u, kCrashBodyStock);
            t.IsFalse(halfWordsWrite(ram.data(), crash, kCrashBodyHalf), "stays disabled");
            t.Equals(rd(ram, 0x49be38u), kCrashBodyStock, "still stock"); });
        tc.Run("sets are independent; null RAM is a no-op", [](TestCase &t)
               {
            std::vector<uint8_t> ram = stockRam();
            wr(ram, 0x49be78u, 0u);
            HalfWords crash{"t-crash", kCrashWords, 4u, kCrashBodyStock, kCrashBodyHalf};
            HalfWords bounce{"t-bounce", kBounceWords, 1u, kBounceStock, kBounceHalf};
            t.IsFalse(halfWordsWrite(ram.data(), crash, kCrashBodyHalf), "crash refused");
            t.IsTrue(halfWordsWrite(ram.data(), bounce, kBounceHalf), "bounce still works");
            t.IsFalse(halfWordsWrite(nullptr, bounce, kBounceStock), "null ram");
            t.Equals(rd(ram, 0x49c13cu), kBounceHalf, "unchanged by null call"); }); });
}
