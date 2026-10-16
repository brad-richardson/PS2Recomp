#include "MiniTest.h"
#include "ps2_runtime.h"
#include "../../ps2xRuntime/src/lib/Kernel/Stubs/Helpers/Support.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// slim-dirpatch-1: an overlay with synthetic dir entries is visible through
// a slim-mode read of the directory sector. On the pre-fix code the slim
// branch returned before patchDirSectors, so the extra record is missing.
namespace
{
namespace fs = std::filesystem;
constexpr uint32_t kSec = 2048;
constexpr uint32_t kSectors = 40;
constexpr uint32_t kDirLbn = 22;

void both32(uint8_t *p, uint32_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

size_t putRecord(std::vector<uint8_t> &img, size_t at, const std::string &name, uint32_t lbn, uint32_t size)
{
    const size_t len = 33 + name.size() + ((name.size() % 2) == 0 ? 1 : 0);
    img[at] = static_cast<uint8_t>(len);
    both32(&img[at + 2], lbn);
    both32(&img[at + 10], size);
    img[at + 25] = 0;
    img[at + 32] = static_cast<uint8_t>(name.size());
    std::memcpy(&img[at + 33], name.data(), name.size());
    return at + len;
}

size_t putDots(std::vector<uint8_t> &img, uint32_t self, uint32_t parent)
{
    size_t o = putRecord(img, self * kSec, std::string(1, '\0'), self, kSec);
    return putRecord(img, o, std::string(1, '\1'), parent, kSec);
}

// 40-sector ISO: PVD@16 (root@20), root {., .., DATA@21},
// DATA {., .., WORLDS@22}, WORLDS@22 {., .., STOCK.BIG@30 size 4096}.
std::vector<uint8_t> makeSlimPayload()
{
    std::vector<uint8_t> img(kSectors * kSec, 0);
    uint8_t *pvd = &img[16 * kSec];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    putRecord(img, 16 * kSec + 156, std::string(1, '\0'), 20, kSec);
    size_t o = putDots(img, 20, 20);
    putRecord(img, o, "DATA", 21, kSec);
    o = putDots(img, 21, 20);
    putRecord(img, o, "WORLDS", 22, kSec);
    o = putDots(img, 22, 21);
    putRecord(img, o, "STOCK.BIG;1", 30, 4096);
    std::memset(&img[30 * kSec], 0xA0, kSec);
    std::memset(&img[31 * kSec], 0xA1, kSec);
    return img;
}

void writeSidecar(const fs::path &image)
{
    // One pad hole over sectors 32-39; payload keeps raw sectors 0-31.
    std::string b("PS2XSLM2", 8);
    b += ps2x::slim_cd::PinnedSourceSha;
    const uint64_t logical = kSectors;
    for (int i = 0; i < 8; ++i)
        b.push_back(char((logical >> (8 * i)) & 0xFF));
    const uint32_t count = 1, first = 32, n = 8, kind = 1;
    for (uint32_t v : {count, first, n, kind})
        for (int i = 0; i < 4; ++i)
            b.push_back(char((v >> (8 * i)) & 0xFF));
    std::ofstream(image.string() + ".holes", std::ios::binary) << b;
}

bool hasRecord(const std::vector<uint8_t> &sec, const std::string &name, uint32_t &extentOut)
{
    for (uint32_t o = 0; o < kSec && sec[o] != 0; o += sec[o])
    {
        if (sec[o + 32] == name.size() && std::memcmp(&sec[o + 33], name.data(), name.size()) == 0)
        {
            extentOut = uint32_t(sec[o + 2]) | (uint32_t(sec[o + 3]) << 8) |
                        (uint32_t(sec[o + 4]) << 16) | (uint32_t(sec[o + 5]) << 24);
            return true;
        }
    }
    return false;
}
} // namespace

void register_ps2_cd_dirpatch_tests()
{
    MiniTest::Case("Ps2CdDirpatch", [](TestCase &tc)
                   {
        tc.Run("slim read of a directory sector shows overlay entries", [](TestCase &t)
               {
            const fs::path root = fs::temp_directory_path() / "ps2x-dirpatch-1";
            fs::remove_all(root);
            fs::create_directories(root / "ov/DATA/WORLDS");
            const auto payload = makeSlimPayload();
            const fs::path image = root / "img.iso";
            // Payload keeps raw sectors 0-31 (PVD, dirs, STOCK data).
            std::ofstream(image, std::ios::binary)
                .write(reinterpret_cast<const char *>(payload.data()), 32 * kSec);
            writeSidecar(image);
            std::ofstream(root / "ov/DATA/WORLDS/EXTRA.BIG", std::ios::binary) << std::string(100, 'e');

            PS2Runtime::IoPaths paths;
            paths.cdImage = image;
            PS2Runtime::setIoPaths(paths);
            ::setenv("PS2X_CD_OVERLAY", (root / "ov").string().c_str(), 1);

            std::vector<uint8_t> dir(kSec, 0);
            t.IsTrue(readCdSectors(kDirLbn, 1, dir.data(), kSec), "dir read ok");
            uint32_t extent = 0;
            t.IsTrue(hasRecord(dir, "EXTRA.BIG;1", extent), "overlay entry visible in slim mode");
            t.IsTrue(extent >= kSectors, "overlay extent is a pseudo-LBN");

            std::vector<uint8_t> stock(kSec, 0);
            t.IsTrue(hasRecord(dir, "STOCK.BIG;1", extent), "stock entry intact");
            t.Equals(extent, 30u, "stock extent unchanged");

            std::vector<uint8_t> data(kSec, 0);
            t.IsTrue(readCdSectors(30, 1, data.data(), kSec), "data read ok");
            t.Equals(data[0], uint8_t(0xA0), "slim data byte intact");
            fs::remove_all(root); }); });
}
