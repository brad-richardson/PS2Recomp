#include "MiniTest.h"
#include "ps2_ssx3_course_manifest.h"

#include <cstring>
#include <string>
#include <vector>

// TK2: course manifest parser + table verification (the PS2 port of the GC
// SSX_COURSE_MANIFEST). A synthetic RAM holds only the 69 row-index words.
namespace
{
std::vector<uint8_t> stockTables()
{
    using namespace ps2_ssx3_course;
    std::vector<uint8_t> ram(kRamSize, 0);
    for (uint32_t i = 0; i < kRows; ++i)
    {
        std::memcpy(&ram[kEventBase + i * kEventStride], &i, 4);
        std::memcpy(&ram[kTopoBase + i * kTopoStride], &i, 4);
        std::memcpy(&ram[kDiscBase + i * kDiscStride], &i, 4);
    }
    std::memcpy(&ram[kEventBase + 4], "Snow Jam", 8);
    std::memcpy(&ram[kEventBase + 68], "BAM", 3);
    return ram;
}
} // namespace

void register_ps2_ssx3_course_manifest_tests()
{
    MiniTest::Case("Ps2Ssx3CourseManifest", [](TestCase &tc)
                   {
        tc.Run("parses blocks, comments and all fields", [](TestCase &t)
               {
            std::vector<ps2_ssx3_course::Block> b;
            std::string err;
            const bool ok = ps2_ssx3_course::parse(
                "# Garibaldi in Snow Jam's slot\nevent = 0\narchive = GARI\nname = Garibaldi  # label\n"
                "short = Gari\ncode = ARA1\nlocation = 0\ndiscipline = 2\n\nevent = 5\nname = X\n",
                b, err);
            t.IsTrue(ok, err.c_str());
            t.Equals(b.size(), static_cast<size_t>(2), "two blocks");
            t.Equals(b[0].str[3], std::string("GARI"), "archive");
            t.Equals(b[0].str[0], std::string("Garibaldi"), "comment stripped");
            t.IsTrue(b[0].hasLocation && b[0].location == 0, "location");
            t.IsTrue(b[0].hasDiscipline && b[0].discipline == 2, "discipline");
            t.Equals(b[1].event, 5, "second event"); });

        tc.Run("rejects malformed manifests", [](TestCase &t)
               {
            std::vector<ps2_ssx3_course::Block> b;
            std::string err;
            t.IsFalse(ps2_ssx3_course::parse("name = X\n", b, err), "key before event");
            t.IsFalse(ps2_ssx3_course::parse("event = 23\nname = X\n", b, err), "event range");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\n", b, err), "empty block");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nevent = 1\nname = X\n", b, err), "empty first block");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nname = X\nname = Y\n", b, err), "duplicate key");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nname = X\nevent = 0\nname = Y\n", b, err), "duplicate event");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\narchive = 0123456789abcdef\n", b, err), "archive too long");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nlocation = 50\n", b, err), "location range");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\ndiscipline = 0\n", b, err), "discipline range");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nmode = 2\n", b, err), "unknown key");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nname\n", b, err), "no equals"); });

        tc.Run("applies NUL-padded writes and logs old values", [](TestCase &t)
               {
            auto ram = stockTables();
            std::vector<ps2_ssx3_course::Block> b;
            std::string err;
            t.IsTrue(ps2_ssx3_course::parse("event = 0\narchive = GARI\nname = Garibaldi\nlocation = 5\ndiscipline = 3\n", b, err), err.c_str());
            std::vector<std::string> lines;
            const int n = ps2_ssx3_course::apply(ram.data(), b, [&](const std::string &s) { lines.push_back(s); });
            t.Equals(n, 4, "four writes");
            t.Equals(lines.size(), static_cast<size_t>(4), "one log line per write");
            t.Equals(std::string(reinterpret_cast<const char *>(&ram[0x43D950 + 68])), std::string("GARI"), "archive");
            t.Equals(std::string(reinterpret_cast<const char *>(&ram[0x43D950 + 4])), std::string("Garibaldi"), "name");
            t.IsTrue(ram[0x43D950 + 4 + 31] == 0, "name padded");
            int32_t loc = 0, disc = 0;
            std::memcpy(&loc, &ram[0x442488 + 16], 4);
            std::memcpy(&disc, &ram[0x442820 + 4], 4);
            t.Equals(loc, 5, "location");
            t.Equals(disc, 3, "discipline");
            t.IsTrue(lines[0].find("\"Snow Jam\" -> \"Garibaldi\"") != std::string::npos, lines[0].c_str()); });

        tc.Run("refuses when any row-index word differs", [](TestCase &t)
               {
            auto ram = stockTables();
            const uint32_t seven = 7u;
            std::memcpy(&ram[0x442820 + 22 * 8], &seven, 4); // discipline row 22
            std::vector<ps2_ssx3_course::Block> b;
            std::string err;
            t.IsTrue(ps2_ssx3_course::parse("event = 0\narchive = GARI\n", b, err), err.c_str());
            std::vector<std::string> lines;
            const int n = ps2_ssx3_course::apply(ram.data(), b, [&](const std::string &s) { lines.push_back(s); });
            t.Equals(n, -1, "refused");
            t.Equals(std::string(reinterpret_cast<const char *>(&ram[0x43D950 + 68])), std::string("BAM"), "untouched");
            t.IsTrue(lines.size() == 1 && lines[0].find("1 of 69") != std::string::npos, lines[0].c_str()); }); });
}
