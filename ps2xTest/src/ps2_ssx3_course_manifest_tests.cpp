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

// TK6: free-ride peak 3 (index 2) as on the disc: slot 2 ERA5 (event 4, type 4), slots 6/7 DONOTUSE.
void freerideNodes(std::vector<uint8_t> &ram)
{
    auto node = [&](uint32_t slot, uint32_t ev, uint32_t type, const char *label, const char *path)
    {
        const uint32_t a = 0x478D38u + 2u * 0x360u + slot * 0x6Cu;
        std::memcpy(&ram[a], &ev, 4);
        std::memcpy(&ram[a + 4], &type, 4);
        std::memcpy(&ram[a + 0x14], label, std::strlen(label));
        std::memcpy(&ram[a + 0x55], path, std::strlen(path));
    };
    node(2, 4, 4, "", "era5path");
    node(6, 23, 7, "DONOTUSE", "");
    node(7, 23, 7, "DONOTUSE", "");
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
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nname\n", b, err), "no equals");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nsky = -2\n", b, err), "sky range");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\ntransp = 50\n", b, err), "transp range");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nnode = freeride:3:6:2\n", b, err), "node peak range");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nnode = freeride:2:8:2\n", b, err), "node slot range");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nnode = race:2:4:1\n", b, err), "race has 4 slots");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nnode = freeride:2:6:6\n", b, err), "template is target");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nnode = station:2:6:2\n", b, err), "table name");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nnode = freeride:2:6\n", b, err), "missing template");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nnode = freeride:2:6:2\nevent = 1\nnode = freeride:2:6:2\n", b, err),
                      "same node twice"); });

        tc.Run("sky, transp and a DONOTUSE node become a menu entry", [](TestCase &t)
               {
            auto ram = stockTables();
            freerideNodes(ram);
            std::vector<ps2_ssx3_course::Block> b;
            std::string err;
            t.IsTrue(ps2_ssx3_course::parse("event = 22\nname = Garibaldi\nsky = 44\ntransp = 43\nnode = freeride:2:6:2\n", b, err),
                     err.c_str());
            std::vector<std::string> lines;
            const int n = ps2_ssx3_course::apply(ram.data(), b, [&](const std::string &s) { lines.push_back(s); });
            t.Equals(n, 4, "four writes");
            int32_t sky = 0, transp = 0;
            std::memcpy(&sky, &ram[0x442488 + 22 * 40 + 8], 4);
            std::memcpy(&transp, &ram[0x442488 + 22 * 40 + 12], 4);
            t.Equals(sky, 44, "sky");
            t.Equals(transp, 43, "transp");
            const uint32_t dst = 0x478D38u + 2u * 0x360u + 6u * 0x6Cu;
            uint32_t ev = 0, type = 0;
            std::memcpy(&ev, &ram[dst], 4);
            std::memcpy(&type, &ram[dst + 4], 4);
            t.Equals(ev, 22u, "node event");
            t.Equals(type, 4u, "node type from the template");
            t.Equals(std::string(reinterpret_cast<const char *>(&ram[dst + 0x14])), std::string(""), "label from the template");
            t.Equals(std::string(reinterpret_cast<const char *>(&ram[dst + 0x55])), std::string("era5path"), "path from the template");
            uint32_t next = 0;
            std::memcpy(&next, &ram[dst + 0x6C], 4);
            t.Equals(next, 23u, "slot 7 stays the terminator");
            t.IsTrue(lines.back().find("\"DONOTUSE\" -> copy of slot 2 (event 4) with event 22") != std::string::npos,
                     lines.back().c_str()); });

        tc.Run("refuses a node target that is not DONOTUSE", [](TestCase &t)
               {
            auto ram = stockTables();
            freerideNodes(ram);
            std::vector<ps2_ssx3_course::Block> b;
            std::string err;
            t.IsTrue(ps2_ssx3_course::parse("event = 22\narchive = GARI\nnode = freeride:2:2:6\n", b, err), err.c_str());
            std::vector<std::string> lines;
            const int n = ps2_ssx3_course::apply(ram.data(), b, [&](const std::string &s) { lines.push_back(s); });
            t.Equals(n, -1, "refused");
            t.IsTrue(ram[0x43D950 + 22 * 100 + 68] == 0, "archive untouched");
            t.IsTrue(lines.size() == 1 && lines[0].find("not DONOTUSE") != std::string::npos, lines[0].c_str()); });

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
