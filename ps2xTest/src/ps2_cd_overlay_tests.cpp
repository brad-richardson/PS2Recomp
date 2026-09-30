#include "MiniTest.h"
#include "ps2_cd_overlay.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// TK3: runtime virtual CD directory. A synthetic ISO9660 image in memory:
// PVD at 16, root at 20, /DATA at 21, /DATA/WORLDS at 22 (BAM.BIG, IRR.DAT).
namespace
{
namespace fs = std::filesystem;
constexpr uint32_t kSec = ps2_cd_overlay::kSectorSize;
constexpr uint32_t kImageSectors = 40;

void both32(uint8_t *p, uint32_t v) { ps2_cd_overlay::detail::both32(p, v); }

uint32_t le32(const uint8_t *p) { return ps2_cd_overlay::detail::le32(p); }

size_t putRecord(std::vector<uint8_t> &img, size_t at, const std::string &name, uint32_t lbn, uint32_t size, bool dir)
{
    const size_t len = 33 + name.size() + ((name.size() % 2) == 0 ? 1 : 0);
    img[at] = static_cast<uint8_t>(len);
    both32(&img[at + 2], lbn);
    both32(&img[at + 10], size);
    img[at + 18] = 103; // 2003
    img[at + 25] = dir ? 2 : 0;
    img[at + 32] = static_cast<uint8_t>(name.size());
    std::memcpy(&img[at + 33], name.data(), name.size());
    return at + len;
}

size_t putDots(std::vector<uint8_t> &img, uint32_t self, uint32_t parent)
{
    size_t o = putRecord(img, self * kSec, std::string(1, '\0'), self, kSec, true);
    return putRecord(img, o, std::string(1, '\1'), parent, kSec, true);
}

std::vector<uint8_t> makeImage(int extraWorldRecords = 0)
{
    std::vector<uint8_t> img(kImageSectors * kSec, 0);
    uint8_t *pvd = &img[16 * kSec];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    putRecord(img, 16 * kSec + 156, std::string(1, '\0'), 20, kSec, true);
    size_t o = putDots(img, 20, 20);
    putRecord(img, o, "DATA", 21, kSec, true);
    o = putDots(img, 21, 20);
    putRecord(img, o, "WORLDS", 22, kSec, true);
    o = putDots(img, 22, 21);
    o = putRecord(img, o, "BAM.BIG;1", 30, 4096, false);
    o = putRecord(img, o, "IRR.DAT;1", 32, 100, false);
    for (int i = 0; i < extraWorldRecords; ++i)
        o = putRecord(img, o, "Z" + std::to_string(1000 + i) + ".DAT;1", 33, 1, false);
    return img;
}

ps2_cd_overlay::SectorReader reader(const std::vector<uint8_t> &img)
{
    return [&img](uint32_t lbn, uint8_t *dst)
    {
        if (lbn >= img.size() / kSec)
            return false;
        std::memcpy(dst, &img[lbn * kSec], kSec);
        return true;
    };
}

fs::path makeOverlayDir(const std::string &tag, const std::vector<std::pair<std::string, size_t>> &files)
{
    const fs::path root = fs::temp_directory_path() / ("ps2x-tk3-" + tag);
    fs::remove_all(root);
    for (const auto &[rel, bytes] : files)
    {
        fs::create_directories((root / rel).parent_path());
        std::ofstream(root / rel, std::ios::binary) << std::string(bytes, 'x');
    }
    return root;
}

std::vector<std::string> names(const std::vector<uint8_t> &sector)
{
    std::vector<std::string> out;
    for (uint32_t o = 0; o < kSec && sector[o] != 0; o += sector[o])
        out.emplace_back(reinterpret_cast<const char *>(&sector[o + 33]), sector[o + 32]);
    return out;
}
} // namespace

void register_ps2_cd_overlay_tests()
{
    MiniTest::Case("Ps2CdOverlay", [](TestCase &tc)
                   {
        tc.Run("adds a sorted record past the image end", [](TestCase &t)
               {
            const auto img = makeImage();
            const fs::path dir = makeOverlayDir("add", {{"data/worlds/gari.big", 5000}});
            ps2_cd_overlay::Overlay ov;
            std::string err;
            t.IsTrue(ps2_cd_overlay::build(dir, kImageSectors, reader(img), ov, err), err.c_str());
            t.Equals(ov.files.size(), static_cast<size_t>(1), "one file");
            t.Equals(ov.files[0].isoPath, std::string("/DATA/WORLDS/GARI.BIG;1"), "iso path");
            t.Equals(ov.files[0].lbn, 48u, "first lbn aligned past the image");
            t.Equals(ov.files[0].sectors, 3u, "sectors");
            t.IsTrue(ov.contains(48) && ov.contains(50) && !ov.contains(47), "range");
            t.IsTrue(ov.fileFor(49) == &ov.files[0], "fileFor");
            t.Equals(ov.dirSectors.size(), static_cast<size_t>(1), "one directory sector");
            const auto &sec = ov.dirSectors.at(22);
            const auto n = names(sec);
            t.Equals(n.size(), static_cast<size_t>(5), "five records");
            t.Equals(n[2], std::string("BAM.BIG;1"), "sorted 1");
            t.Equals(n[3], std::string("GARI.BIG;1"), "sorted 2");
            t.Equals(n[4], std::string("IRR.DAT;1"), "sorted 3");
            uint32_t o = 0;
            for (int i = 0; i < 3; ++i)
                o += sec[o];
            t.Equals(le32(&sec[o + 2]), 48u, "extent");
            t.Equals(le32(&sec[o + 10]), 5000u, "size");
            t.Equals(sec[o + 9], static_cast<uint8_t>(48), "big-endian extent low byte");
            t.Equals(sec[o + 18], static_cast<uint8_t>(103), "date copied from .");

            std::vector<uint8_t> buf(3 * kSec, 0xAA);
            ov.patchDirSectors(21, 3, buf.data(), buf.size());
            t.IsTrue(std::memcmp(&buf[kSec], sec.data(), kSec) == 0, "patched in the middle of a read");
            t.Equals(buf[0], static_cast<uint8_t>(0xAA), "other sectors untouched");
            fs::remove_all(dir); });

        tc.Run("refuses loudly", [](TestCase &t)
               {
            const auto img = makeImage();
            ps2_cd_overlay::Overlay ov;
            std::string err;
            struct Case { const char *tag; const char *rel; };
            for (const Case c : {Case{"exists", "DATA/WORLDS/BAM.BIG"}, Case{"nodir", "DATA/NEWDIR/X.BIG"},
                                 Case{"badname", "DATA/WORLDS/GARI-2.BIG"}, Case{"noext", "DATA/WORLDS/GARI"}})
            {
                const fs::path dir = makeOverlayDir(c.tag, {{c.rel, 10}});
                t.IsFalse(ps2_cd_overlay::build(dir, kImageSectors, reader(img), ov, err), c.tag);
                t.IsTrue(ov.files.empty() && !ov.contains(48), "nothing kept");
                fs::remove_all(dir);
            }
            const auto full = makeImage(43); // 43 x 44 B more: the new record no longer fits
            const fs::path dir = makeOverlayDir("full", {{"DATA/WORLDS/GARI.BIG", 10}});
            t.IsFalse(ps2_cd_overlay::build(dir, kImageSectors, reader(full), ov, err), "sector overflow");
            t.IsTrue(err.find("would need 2 sectors") != std::string::npos, err.c_str());
            fs::remove_all(dir);
            t.IsFalse(ps2_cd_overlay::build("/nonexistent-tk3", kImageSectors, reader(img), ov, err), "missing dir"); });
    });
}
