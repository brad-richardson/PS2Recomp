#pragma once
// TK3: runtime virtual CD directory (PS2X_CD_OVERLAY=<dir>, default off).
//
// SSX 3 never calls sceCdSearchFile: at boot sub_003E4040 walks the disc's
// own ISO9660 tree (sub_003E3758, one sceCdRead per directory sector into
// 0x519C80) and builds its file table from the records it finds (500 slots,
// 163 used by the disc). A loose file in the cd tree is therefore invisible.
//
// This overlay makes extra files visible without touching the image. Files
// under <dir> mirror the disc layout (<dir>/DATA/WORLDS/GARI.BIG). Each gets
// a pseudo-LBN range past the image end, and the sector(s) of its parent
// directory get a synthetic ISO9660 record (correct extent and size), so the
// game's own walk lists it. readCdSectors then maps those LBNs to the host
// file. It is all-or-nothing: a name that already exists on the disc, a
// missing parent directory, a bad ISO name or a directory that would need
// more sectors refuses the whole overlay loudly, and the disc is served
// unchanged.
//
// Guest-affecting when set (the game sees new directory records); unset =
// one getenv, no reads, no writes. Platform-neutral so the host unit test
// compiles it: sector reads come through a callback.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ps2_cd_overlay
{

inline constexpr uint32_t kSectorSize = 2048u;
inline constexpr uint32_t kPvdLbn = 16u;
inline constexpr uint32_t kLbnAlign = 16u;

struct OverlayFile
{
    std::string isoPath;          // "/DATA/WORLDS/GARI.BIG;1"
    std::filesystem::path host;
    uint32_t lbn = 0;
    uint32_t sectors = 0;
    uint64_t sizeBytes = 0;
};

struct Overlay
{
    std::vector<OverlayFile> files;                     // ascending lbn
    std::map<uint32_t, std::vector<uint8_t>> dirSectors; // lbn -> 2048 B
    uint32_t firstLbn = 0;
    uint32_t endLbn = 0;
    std::vector<std::string> log;

    bool contains(uint32_t lbn) const { return !files.empty() && lbn >= firstLbn && lbn < endLbn; }

    // Overlay file holding lbn, or nullptr (a gap between files or outside).
    const OverlayFile *fileFor(uint32_t lbn) const
    {
        for (const OverlayFile &f : files)
        {
            if (lbn >= f.lbn && lbn < f.lbn + f.sectors)
                return &f;
        }
        return nullptr;
    }

    // Copy synthetic directory sectors over [lbn, lbn+sectors) of dst.
    void patchDirSectors(uint32_t lbn, uint32_t sectors, uint8_t *dst, size_t byteCount) const
    {
        if (!dst)
            return;
        for (const auto &[dirLbn, bytes] : dirSectors)
        {
            if (dirLbn < lbn || dirLbn - lbn >= sectors)
                continue;
            const uint64_t off = static_cast<uint64_t>(dirLbn - lbn) * kSectorSize;
            if (off >= byteCount)
                continue;
            const size_t n = static_cast<size_t>(std::min<uint64_t>(kSectorSize, byteCount - off));
            std::memcpy(dst + off, bytes.data(), n);
        }
    }
};

using SectorReader = std::function<bool(uint32_t lbn, uint8_t *dst)>;

namespace detail
{
inline uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

inline void both32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        p[i] = static_cast<uint8_t>(v >> (8 * i));
        p[7 - i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

inline std::string upper(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Record identifier without the ";1" version (directories have none).
inline std::string stem(const std::vector<uint8_t> &rec)
{
    std::string id(reinterpret_cast<const char *>(rec.data() + 33), rec[32]);
    const size_t semi = id.find(';');
    return semi == std::string::npos ? id : id.substr(0, semi);
}

// ISO 9660 d-characters plus one dot; 8.3-style lengths are not enforced
// (the game's walker copies up to 255 bytes) but stay under 31.
inline bool validIsoLeaf(const std::string &leaf)
{
    if (leaf.empty() || leaf.size() > 30 || std::count(leaf.begin(), leaf.end(), '.') != 1 || leaf.front() == '.')
        return false;
    for (char c : leaf)
    {
        if (!(std::isupper(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_' || c == '.'))
            return false;
    }
    return true;
}

struct Dir
{
    uint32_t lbn = 0;
    uint32_t sizeBytes = 0;
    std::vector<std::vector<uint8_t>> records; // in disc order, "." and ".." first
};

inline bool readDir(const SectorReader &read, uint32_t lbn, uint32_t sizeBytes, Dir &out, std::string &err)
{
    out = Dir{};
    out.lbn = lbn;
    out.sizeBytes = sizeBytes;
    const uint32_t n = (sizeBytes + kSectorSize - 1) / kSectorSize;
    std::vector<uint8_t> sec(kSectorSize);
    for (uint32_t s = 0; s < n; ++s)
    {
        if (!read(lbn + s, sec.data()))
        {
            err = "cannot read directory sector " + std::to_string(lbn + s);
            return false;
        }
        for (uint32_t o = 0; o < kSectorSize;)
        {
            const uint8_t len = sec[o];
            if (len == 0)
                break;
            if (len < 34 || o + len > kSectorSize || 33u + sec[o + 32] > len)
            {
                err = "malformed directory record at lbn " + std::to_string(lbn + s);
                return false;
            }
            out.records.emplace_back(sec.begin() + o, sec.begin() + o + len);
            o += len;
        }
    }
    return true;
}

inline std::vector<uint8_t> makeRecord(const std::string &isoName, uint32_t lbn, uint32_t sizeBytes, const std::vector<uint8_t> &dateFrom)
{
    const size_t nameLen = isoName.size();
    const size_t len = 33 + nameLen + ((nameLen % 2) == 0 ? 1 : 0);
    std::vector<uint8_t> r(len, 0);
    r[0] = static_cast<uint8_t>(len);
    both32(&r[2], lbn);
    both32(&r[10], sizeBytes);
    std::memcpy(&r[18], &dateFrom[18], 7);
    r[25] = 0; // plain file
    r[28] = 1; // volume sequence number 1, both-endian 16-bit
    r[31] = 1;
    r[32] = static_cast<uint8_t>(nameLen);
    std::memcpy(&r[33], isoName.data(), nameLen);
    return r;
}
} // namespace detail

// Builds the overlay from files under dir. imageSectors = image size / 2048.
// Returns false with err set on any refusal (out left empty).
inline bool build(const std::filesystem::path &dir, uint64_t imageSectors, const SectorReader &read, Overlay &out, std::string &err)
{
    using namespace detail;
    out = Overlay{};
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
    {
        err = "not a directory: " + dir.string();
        return false;
    }

    std::vector<uint8_t> pvd(kSectorSize);
    if (!read(kPvdLbn, pvd.data()) || pvd[0] != 1 || std::memcmp(&pvd[1], "CD001", 5) != 0)
    {
        err = "no ISO9660 primary volume descriptor at lbn 16";
        return false;
    }
    const uint32_t rootLbn = le32(&pvd[156 + 2]);
    const uint32_t rootSize = le32(&pvd[156 + 10]);

    std::vector<std::filesystem::path> hostFiles;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (it->is_regular_file(ec) && it->path().filename().string().rfind("._", 0) != 0 && it->path().filename() != ".DS_Store")
            hostFiles.push_back(it->path());
    }
    if (ec)
    {
        err = "cannot list " + dir.string() + ": " + ec.message();
        return false;
    }
    if (hostFiles.empty())
    {
        err = "no files under " + dir.string();
        return false;
    }
    std::sort(hostFiles.begin(), hostFiles.end());

    std::map<uint32_t, Dir> dirs; // parent extent lbn -> records (plus additions)
    uint32_t next = static_cast<uint32_t>((imageSectors + kLbnAlign - 1) / kLbnAlign * kLbnAlign);
    out.firstLbn = next;

    for (const std::filesystem::path &host : hostFiles)
    {
        const std::filesystem::path rel = host.lexically_relative(dir);
        std::vector<std::string> parts;
        for (const auto &p : rel)
            parts.push_back(upper(p.string()));
        const std::string leaf = parts.back();
        parts.pop_back();
        if (!validIsoLeaf(leaf))
        {
            err = "not an ISO9660 file name (A-Z 0-9 _ and one dot, <= 30): " + rel.string();
            return false;
        }

        uint32_t lbn = rootLbn, size = rootSize;
        std::string isoPath;
        for (const std::string &part : parts)
        {
            Dir d;
            if (!readDir(read, lbn, size, d, err))
                return false;
            bool found = false;
            for (size_t i = 2; i < d.records.size(); ++i)
            {
                const auto &r = d.records[i];
                if ((r[25] & 2) && upper(stem(r)) == part)
                {
                    lbn = le32(&r[2]);
                    size = le32(&r[10]);
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                err = "no directory /" + (isoPath.empty() ? "" : isoPath.substr(1) + "/") + part + " on the disc (the overlay adds files, not directories): " + rel.string();
                return false;
            }
            isoPath += "/" + part;
        }

        auto dit = dirs.find(lbn);
        if (dit == dirs.end())
        {
            Dir d;
            if (!readDir(read, lbn, size, d, err))
                return false;
            if (d.records.size() < 2)
            {
                err = "directory without . and .. at lbn " + std::to_string(lbn);
                return false;
            }
            dit = dirs.emplace(lbn, std::move(d)).first;
        }
        for (size_t i = 2; i < dit->second.records.size(); ++i)
        {
            if (upper(stem(dit->second.records[i])) == leaf)
            {
                err = isoPath + "/" + leaf + " already exists (on the disc or twice in the overlay)";
                return false;
            }
        }

        const uint64_t bytes = std::filesystem::file_size(host, ec);
        if (ec || bytes > 0xFFFFFFFFull)
        {
            err = "cannot size (or > 4 GiB) " + host.string();
            return false;
        }
        OverlayFile f;
        f.isoPath = isoPath + "/" + leaf + ";1";
        f.host = host;
        f.lbn = next;
        f.sectors = std::max<uint32_t>(1u, static_cast<uint32_t>((bytes + kSectorSize - 1) / kSectorSize));
        f.sizeBytes = bytes;
        next = (f.lbn + f.sectors + kLbnAlign - 1) / kLbnAlign * kLbnAlign;
        dit->second.records.push_back(makeRecord(leaf + ";1", f.lbn, static_cast<uint32_t>(bytes), dit->second.records[0]));
        out.files.push_back(f);
    }
    out.endLbn = next;

    // Re-lay each touched directory: "." and "..", then identifiers in
    // ISO9660 order; records never cross a sector boundary.
    for (auto &[lbn, d] : dirs)
    {
        std::sort(d.records.begin() + 2, d.records.end(), [](const auto &a, const auto &b)
                  { return std::string(reinterpret_cast<const char *>(a.data() + 33), a[32]) <
                           std::string(reinterpret_cast<const char *>(b.data() + 33), b[32]); });
        const uint32_t nSectors = (d.sizeBytes + kSectorSize - 1) / kSectorSize;
        std::vector<std::vector<uint8_t>> secs(1, std::vector<uint8_t>(kSectorSize, 0));
        uint32_t o = 0;
        for (const auto &r : d.records)
        {
            if (o + r.size() > kSectorSize)
            {
                secs.emplace_back(kSectorSize, 0);
                o = 0;
            }
            std::memcpy(secs.back().data() + o, r.data(), r.size());
            o += static_cast<uint32_t>(r.size());
        }
        if (secs.size() > nSectors)
        {
            err = "directory at lbn " + std::to_string(lbn) + " would need " + std::to_string(secs.size()) + " sectors, has " + std::to_string(nSectors);
            return false;
        }
        const size_t usedSectors = secs.size();
        secs.resize(nSectors, std::vector<uint8_t>(kSectorSize, 0)); // no stale records past the re-lay
        for (uint32_t s = 0; s < nSectors; ++s)
            out.dirSectors.emplace(lbn + s, std::move(secs[s]));
        char b[128];
        std::snprintf(b, sizeof b, "dir lbn 0x%x: %zu records in %zu/%u sectors, last used %u/2048", lbn,
                      d.records.size(), usedSectors, nSectors, o);
        out.log.push_back(b);
    }
    for (const OverlayFile &f : out.files)
    {
        char b[160];
        std::snprintf(b, sizeof b, "%s -> lbn 0x%x sectors %u size %llu", f.isoPath.c_str(), f.lbn, f.sectors,
                      static_cast<unsigned long long>(f.sizeBytes));
        out.log.push_back(std::string(b) + " host " + f.host.string());
    }
    return true;
}

} // namespace ps2_cd_overlay
