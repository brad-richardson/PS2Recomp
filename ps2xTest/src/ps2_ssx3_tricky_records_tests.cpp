#include "MiniTest.h"
#include "ps2_ssx3_tricky_records.h"

#include <cstring>
#include <string>
#include <vector>

// tricky-records-1 v3: McWrite-time image patch. Vectors are the real
// save9 (clean) vs boardsave (PLAYER1-boarded) card bytes at the 16 runs
// (see lane REPORT for provenance); the patch must turn boarded into clean
// exactly, since the runs are the only differences between the two cards.
namespace
{
using namespace ps2_ssx3_tricky_records;

struct Vector
{
    std::uint32_t off;
    const char *cleanHex;
    const char *dirtyHex;
};

void hexTo(const char *hex, std::uint8_t *out)
{
    for (std::size_t i = 0; hex[i] && hex[i + 1]; i += 2)
        out[i / 2] = static_cast<std::uint8_t>(std::stoul(std::string(hex + i, 2), nullptr, 16));
}

const Vector kVectors[] = {
    {1348u, "b8f902", "000000"},
    {1356u, "424f54524f530000", "504c415945522031"},
    {1368u, "f04902", "000000"},
    {1372u, "06", "04"},
    {1376u, "4a554e000000", "424f54524f53"},
    {1388u, "289a01", "f04902"},
    {1392u, "07", "06"},
    {1397u, "414d4553", "554e0000"},
    {1408u, "d07e", "289a"},
    {1412u, "08", "07"},
    {1416u, "4b454e0000", "4a414d4553"},
    {1428u, "9834", "d07e"},
    {1432u, "09", "08"},
    {1436u, "4d494b45", "4b454e00"},
    {1568u, "b8f902", "000000"},
    {3272u, "0e13", "f722"},
};

void fillRuns(std::vector<std::uint8_t> &img, bool dirty)
{
    for (const Vector &v : kVectors)
    {
        std::uint8_t buf[8] = {0};
        hexTo(dirty ? v.dirtyHex : v.cleanHex, buf);
        std::memcpy(img.data() + v.off, buf, std::strlen(v.cleanHex) / 2);
    }
}

bool runsAre(const std::vector<std::uint8_t> &img, bool dirty)
{
    for (const Vector &v : kVectors)
    {
        std::uint8_t buf[8] = {0};
        hexTo(dirty ? v.dirtyHex : v.cleanHex, buf);
        if (std::memcmp(img.data() + v.off, buf, std::strlen(v.cleanHex) / 2) != 0)
            return false;
    }
    return true;
}
} // namespace

void register_ps2_ssx3_tricky_records_tests()
{
    using namespace ps2_ssx3_tricky_records;
    MiniTest::Case("SSX3 Tricky records patch", [](TestCase &tc) {
        tc.Run("boarded image patches to clean exactly", [](TestCase &t) {
            std::vector<std::uint8_t> img(kImageSize, 0xA5);
            fillRuns(img, true);
            patchImage(img.data(), img.size());
            t.IsTrue(runsAre(img, false), "all runs carry clean bytes");
            for (std::size_t i = 0; i < img.size(); ++i)
            {
                bool inRun = false;
                for (const Vector &v : kVectors)
                {
                    const std::size_t len = std::strlen(v.cleanHex) / 2;
                    if (i >= v.off && i < v.off + len)
                    {
                        inRun = true;
                        break;
                    }
                }
                if (!inRun)
                    t.Equals(img[i], std::uint8_t(0xA5), "non-run byte untouched");
            }
        });
        tc.Run("clean image is unchanged", [](TestCase &t) {
            std::vector<std::uint8_t> img(kImageSize, 0xA5);
            fillRuns(img, false);
            const auto before = img;
            patchImage(img.data(), img.size());
            t.IsTrue(img == before, "idempotent on clean");
        });
        tc.Run("predicate gates mapped SET0001 only", [](TestCase &t) {
            t.IsTrue(shouldPatch(true, false, kImageSize), "mapped save patches");
            t.IsTrue(!shouldPatch(false, false, kImageSize), "stock never patches");
            t.IsTrue(!shouldPatch(true, true, kImageSize), "exact path skips");
            t.IsTrue(!shouldPatch(true, false, 1024), "wrong size skips");
            t.IsTrue(!shouldPatch(true, false, 964), "icon.sys skips");
            t.IsTrue(!shouldPatch(true, false, 73144), "ssx1.ico skips");
            t.IsTrue(!shouldPatch(true, false, 39777), "GAM0001 full profile skips");
        });
        tc.Run("patch tolerates short buffers", [](TestCase &t) {
            // Runs that fit are stamped; the @1397 run (ends at 1401) and
            // everything past the end are skipped: no OOB write.
            std::vector<std::uint8_t> img(1400, 0xA5);
            img[1348] = 0x00;
            img[1349] = 0x00;
            img[1350] = 0x00; // boarded bytes at a fittable run
            patchImage(img.data(), img.size());
            for (std::size_t i = 0; i < 1348; ++i)
                t.Equals(img[i], std::uint8_t(0xA5), "pre-run bytes untouched");
            std::uint8_t buf[8] = {0};
            hexTo("b8f902", buf);
            t.IsTrue(std::memcmp(img.data() + 1348, buf, 3) == 0, "fittable run stamped");
            t.Equals(img[1399], std::uint8_t(0xA5), "no write at/past the end");
        });
    });
}
