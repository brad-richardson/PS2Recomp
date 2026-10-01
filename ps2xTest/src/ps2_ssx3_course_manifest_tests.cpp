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

// TK7: event 0's topology location = 0 (ARA1) as on the disc.
void stockTopology(std::vector<uint8_t> &ram)
{
    const int32_t sky = 44, transp = 43, loc = 0;
    std::memcpy(&ram[0x442488 + 8], &sky, 4);
    std::memcpy(&ram[0x442488 + 12], &transp, 4);
    std::memcpy(&ram[0x442488 + 16], &loc, 4);
}

// Pad buffer as scePadRead fills it: data[2..3] active-low buttons.
std::vector<uint8_t> padBuffer(uint16_t pressed)
{
    std::vector<uint8_t> pad(32, 0xFF);
    const uint16_t w = static_cast<uint16_t>(~pressed);
    pad[2] = static_cast<uint8_t>(w & 0xFF);
    pad[3] = static_cast<uint8_t>(w >> 8);
    return pad;
}

int32_t topoLocation(const std::vector<uint8_t> &ram, uint32_t event)
{
    int32_t v = 0;
    std::memcpy(&v, &ram[0x442488 + event * 40 + 16], 4);
    return v;
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
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nlap = 2\n", b, err), "unknown key");
            t.IsFalse(ps2_ssx3_course::parse("mode = T\nrow = 0:GARI:G\n", b, err), "mode needs the modes out-param");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nname\n", b, err), "no equals");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nsky = -2\n", b, err), "sky range");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\ntransp = 50\n", b, err), "transp range");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\nnode = freeride:2:6:2\n", b, err), "node refused (TK7)");
            t.IsTrue(err.find("not supported") != std::string::npos, err.c_str());
            t.IsFalse(ps2_ssx3_course::parse("event = 0\npicker = 49\n", b, err), "picker needs a name");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\npicker = 50:X\n", b, err), "picker location range");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\npicker = 49:\n", b, err), "picker empty name");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\npicker = 49:X\npicker = 49:Y\n", b, err), "picker location twice");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\npicker = 49:X\nevent = 1\npicker = 48:Y\n", b, err),
                      "picker in two blocks");
            t.IsFalse(ps2_ssx3_course::parse("event = 0\npicker = 1:A\npicker = 2:B\npicker = 3:C\npicker = 4:D\n"
                                             "picker = 5:E\npicker = 6:F\npicker = 7:G\npicker = 8:H\n", b, err),
                      "at most 7 picker entries"); });

        tc.Run("sky and transp write topology +8 / +12", [](TestCase &t)
               {
            auto ram = stockTables();
            std::vector<ps2_ssx3_course::Block> b;
            std::string err;
            t.IsTrue(ps2_ssx3_course::parse("event = 22\nname = Garibaldi\nsky = 44\ntransp = 43\n", b, err), err.c_str());
            std::vector<std::string> lines;
            const int n = ps2_ssx3_course::apply(ram.data(), b, [&](const std::string &s) { lines.push_back(s); });
            t.Equals(n, 3, "three writes");
            int32_t sky = 0, transp = 0;
            std::memcpy(&sky, &ram[0x442488 + 22 * 40 + 8], 4);
            std::memcpy(&transp, &ram[0x442488 + 22 * 40 + 12], 4);
            t.Equals(sky, 44, "sky");
            t.Equals(transp, 43, "transp");
            t.IsTrue(lines.back().find("transp at 0x442804: 0 -> 43") != std::string::npos, lines.back().c_str()); });

        tc.Run("picker entries write nothing at apply and arm with the row as entry 0", [](TestCase &t)
               {
            using namespace ps2_ssx3_course;
            auto ram = stockTables();
            stockTopology(ram);
            std::vector<Block> b;
            std::string err;
            t.IsTrue(parse("event = 0\narchive = GARI\npicker = 49:Garibaldi\npicker = 0:Dup\n", b, err), err.c_str());
            std::vector<std::string> lines;
            t.Equals(apply(ram.data(), b, [&](const std::string &s) { lines.push_back(s); }), 1, "archive only");
            t.Equals(topoLocation(ram, 0), 0, "location untouched at apply");
            Picker p;
            t.IsTrue(armPicker(p, ram.data(), b, [&](const std::string &s) { lines.push_back(s); }), "armed");
            t.Equals(p.entries.size(), static_cast<size_t>(2), "stock + Garibaldi (the duplicate of 0 is skipped)");
            t.Equals(p.entries[0].name, std::string("Snow Jam"), "entry 0 name from RAM");
            t.Equals(p.entries[0].location, 0, "entry 0 location from RAM");
            t.Equals(p.event, 0, "event");
            Picker none;
            std::vector<Block> nb;
            t.IsTrue(parse("event = 0\narchive = GARI\n", nb, err), err.c_str());
            t.IsFalse(armPicker(none, ram.data(), nb, [](const std::string &) {}), "no entries, not armed"); });

        tc.Run("picker cycles on the L3+R3 edge, hides the chord, and refuses during an event", [](TestCase &t)
               {
            using namespace ps2_ssx3_course;
            auto ram = stockTables();
            stockTopology(ram);
            std::vector<Block> b;
            std::string err;
            t.IsTrue(parse("event = 0\npicker = 49:Garibaldi\n", b, err), err.c_str());
            Picker &p = picker();
            t.IsTrue(armPicker(p, ram.data(), b, [](const std::string &) {}), "armed");
            std::string status;
            auto name0 = [&] { return std::string(reinterpret_cast<const char *>(&ram[0x43D950 + 4])); };

            auto idle = padBuffer(0);
            pickerOnPadRead(ram.data(), idle.data(), 1, status);
            t.Equals(topoLocation(ram, 0), 0, "no chord, no switch");

            auto l3 = padBuffer(kBtnL3);
            pickerOnPadRead(ram.data(), l3.data(), 2, status);
            t.Equals(topoLocation(ram, 0), 0, "L3 alone does nothing");
            t.Equals(l3[2], padBuffer(kBtnL3)[2], "L3 alone reaches the guest");

            auto chord = padBuffer(kBtnL3 | kBtnR3);
            pickerOnPadRead(ram.data(), chord.data(), 3, status);
            t.Equals(topoLocation(ram, 0), 49, "switched to Garibaldi");
            t.Equals(name0(), std::string("Garibaldi"), "name follows");
            t.Equals(status, std::string("Course: Garibaldi"), "status line");
            t.Equals(chord[2], static_cast<uint8_t>(0xFF), "chord hidden from the guest");
            auto held = padBuffer(kBtnL3 | kBtnR3);
            pickerOnPadRead(ram.data(), held.data(), 4, status);
            t.Equals(topoLocation(ram, 0), 49, "held chord does not repeat");

            auto withSelect = padBuffer(kBtnSelect | kBtnL3 | kBtnR3);
            pickerOnPadRead(ram.data(), idle.data(), 5, status);
            pickerOnPadRead(ram.data(), withSelect.data(), 6, status);
            t.Equals(topoLocation(ram, 0), 49, "SELECT held = DS1's chord, not ours");
            t.Equals(withSelect[2], padBuffer(kBtnSelect | kBtnL3 | kBtnR3)[2], "DS1 chord untouched");

            pickerOnPadRead(ram.data(), idle.data(), 7, status);
            auto again = padBuffer(kBtnL3 | kBtnR3);
            pickerOnPadRead(ram.data(), again.data(), 8, status);
            t.Equals(topoLocation(ram, 0), 0, "cycles back to stock");
            t.Equals(name0(), std::string("Snow Jam"), "stock name back");
            t.IsTrue(ram[0x43D950 + 4 + 8] == 0, "name NUL-padded");

            // In an event: 0x4A5B64 -> A, [A] = app, [app] = vtable, +0x34 = 0x2306B8 (KSEG0 pointers).
            const uint32_t mgr = 0x80C00000u, app = 0x00C00100u, vt = 0x00C00200u, fn = kGameUpdate;
            std::memcpy(&ram[kAppMgrPtr], &mgr, 4);
            std::memcpy(&ram[0xC00000], &app, 4);
            std::memcpy(&ram[0xC00100], &vt, 4);
            std::memcpy(&ram[0xC00200 + kAppUpdateSlot], &fn, 4);
            t.Equals(appUpdateFn(ram.data()), kGameUpdate, "pointer chain");
            pickerOnPadRead(ram.data(), idle.data(), 9, status);
            auto live = padBuffer(kBtnL3 | kBtnR3);
            pickerOnPadRead(ram.data(), live.data(), 10, status);
            t.Equals(topoLocation(ram, 0), 0, "refused while an event is live");
            t.IsTrue(status.find("an event is live") != std::string::npos, status.c_str());
            const uint32_t frontend = 0x2F0000u, zero = 0u, requested = 8u;
            std::memcpy(&ram[0xC00200 + kAppUpdateSlot], &frontend, 4);
            std::memcpy(&ram[kSlotBase + 49u * kSlotStride + 8u], &requested, 4);
            std::string msg;
            t.Equals(pickerNext(p, ram.data(), msg), -1, "refused while a location loads");
            t.IsTrue(msg.find("location 49 is loading") != std::string::npos, msg.c_str());
            std::memcpy(&ram[kSlotBase + 49u * kSlotStride + 8u], &zero, 4);
            t.Equals(pickerNext(p, ram.data(), msg), 1, "allowed again");
            p = Picker{}; // leave the process-wide picker disarmed
        });

        tc.Run("TK9 mode blocks: parse and errors", [](TestCase &t)
               {
            using namespace ps2_ssx3_course;
            std::vector<Block> b;
            std::vector<Mode> m;
            std::string err;
            t.IsTrue(parse("# Tricky\nmode = Tricky\nrow = 0:GARI:Garibaldi\nrow = 1:-:Snowdream\n"
                           "poke = 0x47BDB0:data/ui/courspic.big:data/ui/courspit.big\n"
                           "event = 2\nname = X\nmode = Other\nrow = 3:SNOW:-\n",
                           b, err, &m),
                     err.c_str());
            t.Equals(m.size(), static_cast<size_t>(2), "two modes");
            t.Equals(b.size(), static_cast<size_t>(1), "one event block between them");
            t.Equals(m[0].name, std::string("Tricky"), "name");
            t.Equals(m[0].rows.size(), static_cast<size_t>(2), "rows");
            t.Equals(m[0].rows[0].archive, std::string("GARI"), "archive");
            t.Equals(m[0].rows[1].archive, std::string(), "'-' keeps archive");
            t.Equals(m[0].rows[1].name, std::string("Snowdream"), "row name");
            t.Equals(m[0].pokes.size(), static_cast<size_t>(1), "poke");
            t.Equals(m[0].pokes[0].addr, 0x47BDB0u, "poke address");
            t.Equals(m[1].rows[0].name, std::string(), "'-' keeps name");
            t.IsTrue(parse("mode = T\nrow = 0:GARI:G\n", b, err, &m), "modes only (no event block)");
            const char *bad[] = {
                "mode = T\n",                                       // empty mode
                "mode = T\nmode = U\nrow = 0:A:B\n",                // empty first mode
                "mode = T\nrow = 0:A:B\nmode = T\nrow = 1:A:B\n",   // duplicate name
                "mode = T\nrow = 23:A:B\n",                         // event range
                "mode = T\nrow = 0:A\n",                            // two fields
                "mode = T\nrow = 0:-:-\n",                          // sets nothing
                "mode = T\nrow = 0:0123456789abcdef:-\n",           // archive too long
                "mode = T\nrow = 0:A:B\nrow = 0:C:D\n",             // event twice
                "mode = T\nname = X\n",                             // event key in a mode
                "mode = T\npoke = 47BDB0:abc:abd\n",                // no 0x
                "mode = T\npoke = 0x100:abc:abcd\n",                // length differs
                "mode = T\npoke = 0x100:abc:abc\n",                 // no change
                "mode = T\npoke = 0x100:a:b:c\n",                   // colon inside
                "mode = T\npoke = 0x1FFFFFE:abc:abd\n",             // outside RAM
                "mode = T\npoke = 0x43D960:abc:abd\n",              // over the event rows
                "mode = T\npoke = 0x100:abc:abd\npoke = 0x100:abc:abe\n", // address twice
                "mode = T\npoke = 0x100:abc:abd\nmode = U\npoke = 0x101:bc:xx\n", // overlap, other site
                "mode = T\npoke = 0x100:abc:abd\nmode = U\npoke = 0x100:abz:abd\n", // same site, other old
                "event = 0\npicker = 49:G\nmode = T\nrow = 0:A:B\n", // picker and modes
                "mode = A\nrow=0:A:-\nmode = B\nrow=0:A:-\nmode = C\nrow=0:A:-\nmode = D\nrow=0:A:-\n"
                "mode = E\nrow=0:A:-\nmode = F\nrow=0:A:-\nmode = G\nrow=0:A:-\nmode = H\nrow=0:A:-\n", // > 7
            };
            for (const char *txt : bad)
                t.IsFalse(parse(txt, b, err, &m), txt);
            t.IsTrue(parse("mode = T\npoke = 0x100:abc:abd\nmode = U\npoke = 0x100:abc:xyz\n", b, err, &m),
                     "same site in two modes"); });

        tc.Run("TK9 modes cycle Stock -> mode -> Stock, poke and undo, derive from RAM, refuse unsafe", [](TestCase &t)
               {
            using namespace ps2_ssx3_course;
            auto ram = stockTables();
            stockTopology(ram);
            std::memcpy(&ram[0x47BDB0], "data/ui/courspic.big", 21);
            std::vector<Block> b;
            std::vector<Mode> m;
            std::string err;
            t.IsTrue(parse("mode = Tricky\nrow = 0:GARI:Garibaldi\n"
                           "poke = 0x47BDB0:data/ui/courspic.big:data/ui/courspit.big\n"
                           "mode = Two\nrow = 0:-:Other\n",
                           b, err, &m),
                     err.c_str());
            std::vector<std::string> lines;
            auto log = [&](const std::string &s) { lines.push_back(s); };
            t.Equals(apply(ram.data(), b, log), 0, "no event blocks, no writes");
            Modes &ms = courseModes();
            t.IsTrue(armModes(ms, ram.data(), m, log), "armed");
            t.Equals(ms.fields.size(), static_cast<size_t>(2), "archive + name of event 0");
            t.Equals(modeCurrent(ms, ram.data()), static_cast<size_t>(0), "starts at Stock");
            auto str = [&](uint32_t a) { return std::string(reinterpret_cast<const char *>(&ram[a])); };

            std::string status;
            auto idle = padBuffer(0);
            auto chord = padBuffer(kBtnL3 | kBtnR3);
            pickerOnPadRead(ram.data(), idle.data(), 1, status);
            pickerOnPadRead(ram.data(), chord.data(), 2, status);
            t.Equals(status, std::string("Mode: Tricky"), "status");
            t.Equals(str(0x43D950 + 68), std::string("GARI"), "archive on");
            t.Equals(str(0x43D950 + 4), std::string("Garibaldi"), "name on");
            t.Equals(str(0x47BDB0), std::string("data/ui/courspit.big"), "poke on");
            t.Equals(chord[2], static_cast<uint8_t>(0xFF), "chord hidden");
            t.Equals(modeCurrent(ms, ram.data()), static_cast<size_t>(1), "derived: Tricky");

            pickerOnPadRead(ram.data(), idle.data(), 3, status);
            chord = padBuffer(kBtnL3 | kBtnR3);
            pickerOnPadRead(ram.data(), chord.data(), 4, status);
            t.Equals(status, std::string("Mode: Two"), "second mode");
            t.Equals(str(0x43D950 + 68), std::string("BAM"), "Two keeps the stock archive");
            t.Equals(str(0x43D950 + 4), std::string("Other"), "Two's name");
            t.Equals(str(0x47BDB0), std::string("data/ui/courspic.big"), "Two has no poke: undone");

            std::string msg;
            std::vector<std::string> w;
            t.Equals(modeNext(ms, ram.data(), msg, &w), 0, "back to Stock");
            t.Equals(str(0x43D950 + 4), std::string("Snow Jam"), "stock name");
            t.IsTrue(ram[0x43D950 + 4 + 8] == 0 && ram[0x43D950 + 4 + 9] == 0, "NUL-padded");
            t.Equals(w.size(), static_cast<size_t>(1), "one field changed (name)");

            // A foreign string at the poke site refuses the switch, nothing written.
            std::memcpy(&ram[0x47BDB0], "data/ui/coursXXX.big", 20);
            t.Equals(modeNext(ms, ram.data(), msg), -1, "refused");
            t.IsTrue(msg.find("neither") != std::string::npos, msg.c_str());
            t.Equals(str(0x43D950 + 68), std::string("BAM"), "nothing written");
            std::memcpy(&ram[0x47BDB0], "data/ui/courspic.big", 20);

            // Savestate-style: RAM already in Tricky -> the next press goes to Two.
            std::memcpy(&ram[0x43D950 + 68], "GARI", 5);
            std::memset(&ram[0x43D950 + 4], 0, 32);
            std::memcpy(&ram[0x43D950 + 4], "Garibaldi", 9);
            std::memcpy(&ram[0x47BDB0], "data/ui/courspit.big", 20);
            t.Equals(modeNext(ms, ram.data(), msg), 2, "derived from RAM");

            // Live event: refused.
            const uint32_t mgr = 0x80C00000u, app = 0x00C00100u, vt = 0x00C00200u, fn = kGameUpdate;
            std::memcpy(&ram[kAppMgrPtr], &mgr, 4);
            std::memcpy(&ram[0xC00000], &app, 4);
            std::memcpy(&ram[0xC00100], &vt, 4);
            std::memcpy(&ram[0xC00200 + kAppUpdateSlot], &fn, 4);
            t.Equals(modeNext(ms, ram.data(), msg), -1, "refused in an event");
            t.IsTrue(msg.find("an event is live") != std::string::npos, msg.c_str());

            // Arming refuses a mode whose poke's old bytes are not in RAM.
            Modes other;
            auto ram2 = stockTables();
            t.IsFalse(armModes(other, ram2.data(), std::vector<Mode>{m[0]}, log), "old bytes missing: refused");
            t.IsTrue(lines.back().find("refused") != std::string::npos, lines.back().c_str());
            ms = Modes{}; // leave the process-wide modes disarmed
        });

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
            t.IsTrue(lines.size() == 1 && lines[0].find("1 of 69") != std::string::npos, lines[0].c_str()); });

        tc.Run("TK25c alias: parse, errors, and the switch follows the mode", [](TestCase &t)
               {
            using namespace ps2_ssx3_course;
            std::vector<Block> b;
            std::vector<Mode> m;
            std::string err;
            t.IsTrue(parse("mode = Tricky\nrow = 0:GARI:Garibaldi\n"
                           "alias = /DATA/AUDIO/SPEECH.BIG:/host/speech-big\n",
                           b, err, &m),
                      err.c_str());
            t.Equals(m.size(), static_cast<size_t>(1), "one mode");
            t.IsTrue(m[0].hasAlias, "has alias");
            t.Equals(m[0].aliasDisc, std::string("/DATA/AUDIO/SPEECH.BIG"), "disc");
            t.Equals(m[0].aliasHost, std::string("/host/speech-big"), "host");
            const char *bad[] = {
                "alias = /D/S.BIG:/h\n",                        // before any mode
                "event = 0\nname = X\nalias = /D/S.BIG:/h\n",    // event key, not mode key
                "mode = T\nrow = 0:A:B\nalias = /D/S.BIG:/h\nalias = /D/S.BIG:/h\n", // twice
                "mode = T\nrow = 0:A:B\nalias = /D/S.BIG\n",     // no host
                "mode = T\nrow = 0:A:B\nalias = :/h\n",          // no disc
                "mode = T\nrow = 0:A:B\nalias = DATA/S.BIG:/h\n", // no leading slash
                "mode = T\nrow = 0:A:B\nalias = /D/S.BIG:/h:x\n", // colon in host
                "mode = T\nalias = /DATA/AUDIO/SPEECH.BIG:/h\n", // alias alone is no row or poke
            };
            for (const char *txt : bad)
                t.IsFalse(parse(txt, b, err, &m), txt);

            // The switch sets the pending alias on entry and clears it on exit.
            auto ram = stockTables();
            stockTopology(ram);
            std::vector<std::string> lines;
            auto log = [&](const std::string &s) { lines.push_back(s); };
            t.IsTrue(parse("mode = Tricky\nrow = 0:GARI:Garibaldi\n"
                           "alias = /DATA/AUDIO/SPEECH.BIG:/host/speech-big\n"
                           "mode = Two\nrow = 0:-:Other\n",
                           b, err, &m),
                      err.c_str());
            Modes &ms = courseModes();
            t.IsTrue(armModes(ms, ram.data(), m, log), "armed");
            auto pending = ps2_cd_overlay::pendingAlias();
            t.IsTrue(pending.first.empty(), "arming starts with the alias off");
            std::string status;
            auto idle = padBuffer(0);
            auto chord = padBuffer(kBtnL3 | kBtnR3);
            pickerOnPadRead(ram.data(), idle.data(), 1, status);
            pickerOnPadRead(ram.data(), chord.data(), 2, status);
            t.Equals(status, std::string("Mode: Tricky"), "status");
            pending = ps2_cd_overlay::pendingAlias();
            t.Equals(pending.first, std::string("/DATA/AUDIO/SPEECH.BIG"), "alias on in Tricky");
            t.Equals(pending.second, std::string("/host/speech-big"), "host on in Tricky");
            pickerOnPadRead(ram.data(), idle.data(), 3, status);
            chord = padBuffer(kBtnL3 | kBtnR3);
            pickerOnPadRead(ram.data(), chord.data(), 4, status);
            t.Equals(status, std::string("Mode: Two"), "second mode");
            pending = ps2_cd_overlay::pendingAlias();
            t.IsTrue(pending.first.empty(), "Two has no alias: off");
            ms = Modes{}; // leave the process-wide modes disarmed
            ps2_cd_overlay::clearModeAlias();
        }); });
}
