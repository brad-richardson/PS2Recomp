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
//
// TK10: composite files. A host file whose first bytes are "PS2XCMP1\n" is
// a descriptor, not data. Its first sector is text:
//
//   PS2XCMP1
//   size <bytes>             size of the file the game sees
//   image <sectors>          the disc it was made for (refused otherwise)
//   iso <lbn> <sectors>      served from the disc image
//   self <offset> <sectors>  served from this host file (offset >= 2048)
//   end
//
// The segments, in order, cover the file's sectors exactly. A replace-world
// is then its changed pieces (BIG directory, SDB, the rebuilt group) plus
// ranges of the stock BAM.BIG on the disc, a few MB instead of ~113 MB.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ps2_cd_overlay
{

inline constexpr uint32_t kSectorSize = 2048u;
inline constexpr uint32_t kPvdLbn = 16u;
inline constexpr uint32_t kLbnAlign = 16u;
inline constexpr char kCompositeMagic[] = "PS2XCMP1\n";

struct Segment
{
    bool image = false;  // true: disc LBN range; false: bytes of the host file
    uint64_t source = 0; // image: first LBN; host: byte offset
    uint32_t first = 0;  // first sector of this segment within the file
    uint32_t sectors = 0;
};

struct OverlayFile
{
    std::string isoPath;          // "/DATA/WORLDS/GARI.BIG;1"
    std::filesystem::path host;
    uint32_t lbn = 0;
    uint32_t sectors = 0;
    uint64_t sizeBytes = 0;
    std::vector<Segment> segments; // TK10 composite; empty = plain host file
};

// One contiguous source range of a read (TK10 composite files).
struct Piece
{
    enum Kind { Image, Host, Zero } kind = Zero;
    uint64_t offset = 0; // byte offset in the image or the host file
    uint64_t bytes = 0;
};

// Splits bytes [offset, offset + count) of f into source ranges, in order.
// A plain file is one Host range; bytes past the segments read as zeros.
inline std::vector<Piece> resolve(const OverlayFile &f, uint64_t offset, uint64_t count)
{
    std::vector<Piece> out;
    if (f.segments.empty())
    {
        out.push_back({Piece::Host, offset, count});
        return out;
    }
    while (count > 0)
    {
        const uint64_t sector = offset / kSectorSize;
        const Segment *seg = nullptr;
        for (const Segment &s : f.segments)
        {
            if (sector >= s.first && sector < uint64_t(s.first) + s.sectors)
            {
                seg = &s;
                break;
            }
        }
        if (!seg)
        {
            out.push_back({Piece::Zero, 0, count});
            break;
        }
        const uint64_t within = offset - uint64_t(seg->first) * kSectorSize;
        const uint64_t n = std::min<uint64_t>(count, uint64_t(seg->sectors) * kSectorSize - within);
        const uint64_t base = seg->image ? seg->source * kSectorSize : seg->source;
        if (!out.empty() && out.back().kind == (seg->image ? Piece::Image : Piece::Host) &&
            out.back().offset + out.back().bytes == base + within)
            out.back().bytes += n;
        else
            out.push_back({seg->image ? Piece::Image : Piece::Host, base + within, n});
        offset += n;
        count -= n;
    }
    return out;
}

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

// Parses a composite descriptor (the text of its first sector) into f.
inline bool parseComposite(const std::string &text, uint64_t hostBytes, uint64_t imageSectors, OverlayFile &f, std::string &err)
{
    uint64_t size = 0, image = 0;
    bool haveSize = false, haveImage = false, ended = false;
    uint32_t next = 0;
    size_t pos = 0;
    while (pos < text.size() && !ended)
    {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        char word[16] = {};
        unsigned long long a = 0, b = 0;
        const int n = std::sscanf(line.c_str(), "%15s %llu %llu", word, &a, &b);
        const std::string w = n >= 1 ? word : "";
        if (w.empty() || w == "PS2XCMP1")
            continue;
        if (w == "end")
            ended = true;
        else if (w == "size" && n == 2)
            size = a, haveSize = true;
        else if (w == "image" && n == 2)
            image = a, haveImage = true;
        else if ((w == "iso" || w == "self") && n == 3 && b > 0 && b <= 0xFFFFFFFFull - next)
        {
            const bool isImage = w == "iso";
            if (isImage ? a + b > imageSectors : (a < kSectorSize || a + (b - 1) * kSectorSize >= hostBytes))
            {
                err = "composite segment outside the " + std::string(isImage ? "image" : "host file") + ": " + line;
                return false;
            }
            f.segments.push_back({isImage, a, next, static_cast<uint32_t>(b)});
            next += static_cast<uint32_t>(b);
        }
        else
        {
            err = "bad composite line: " + line;
            return false;
        }
    }
    if (!ended || !haveSize || !haveImage || size == 0 || size > 0xFFFFFFFFull)
    {
        err = "composite needs size, image and end";
        return false;
    }
    if (image != imageSectors)
    {
        err = "composite made for a " + std::to_string(image) + "-sector image, this one has " + std::to_string(imageSectors);
        return false;
    }
    const uint64_t need = (size + kSectorSize - 1) / kSectorSize;
    if (next != need)
    {
        err = "composite segments cover " + std::to_string(next) + " sectors, size needs " + std::to_string(need);
        return false;
    }
    f.sizeBytes = size;
    f.sectors = static_cast<uint32_t>(need);
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
        {
            std::string head(kSectorSize, '\0');
            std::ifstream in(host, std::ios::binary);
            in.read(head.data(), kSectorSize);
            head.resize(static_cast<size_t>(std::max<std::streamsize>(0, in.gcount())));
            if (head.compare(0, sizeof kCompositeMagic - 1, kCompositeMagic) == 0)
            {
                head.resize(std::min(head.find('\0'), head.size()));
                if (!parseComposite(head, bytes, imageSectors, f, err))
                {
                    err = rel.string() + ": " + err;
                    return false;
                }
            }
        }
        next = (f.lbn + f.sectors + kLbnAlign - 1) / kLbnAlign * kLbnAlign;
        dit->second.records.push_back(makeRecord(leaf + ";1", f.lbn, static_cast<uint32_t>(f.sizeBytes), dit->second.records[0]));
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
        std::string line = std::string(b) + " host " + f.host.string();
        if (!f.segments.empty())
        {
            uint64_t fromImage = 0;
            for (const Segment &s : f.segments)
                fromImage += s.image ? s.sectors : 0;
            line += " composite " + std::to_string(f.segments.size()) + " segments, " + std::to_string(fromImage) +
                    " sectors from the image";
        }
        out.log.push_back(line);
    }
    return true;
}

// ---- TK25c: mode-scoped CD alias (TKP4b: several per mode) -------------------
// A manifest mode block may carry `alias = <disc>:<host composite>` lines
// (e.g. SPEECH.BIG + MUSIC.BIG + MUSIC2.BIG). While that mode is on,
// readCdSectors serves each disc file's LBN range from its host file (a TK10
// PS2XCMP1 composite: `iso` segments for unchanged ranges, `self` for the
// replaced members). Same size is required: a size mismatch is refused at
// load and the disc is served unchanged. No alias (mode off or Stock) =
// byte-identical reads to today; the knob is the manifest (default off
// everywhere).
//
// The aliases are set by the course-mode switch (L3+R3 chord) and consumed
// lazily by the CD layer, which owns the image reader: setModeAliases stores
// the strings, and resolveActiveAlias loads + validates on the next read
// (once per mode activation; failures are logged once and serve the disc).

inline constexpr size_t kAliasDiscMaxLen = 127u;
inline constexpr size_t kAliasHostMaxLen = 511u;

struct DiscAlias
{
    std::string discPath;      // as in the manifest, e.g. /DATA/AUDIO/SPEECH.BIG
    std::filesystem::path host; // the composite host file
    uint32_t discLbn = 0;      // the file's extent on the disc
    uint32_t discSectors = 0;
    uint64_t discSize = 0; // bytes (== composite.sizeBytes)
    OverlayFile composite; // the parsed composite (host = the composite file)
    bool loaded = false;
    std::string err;
};

// Walks the image's ISO9660 tree for an absolute disc path (case-insensitive,
// like the game's own walker). Returns the file's extent LBN and byte size.
inline bool findDiscFile(uint64_t imageSectors, const SectorReader &read, const std::string &discPath,
                         uint32_t &lbnOut, uint64_t &sizeOut, std::string &err)
{
    using namespace detail;
    if (discPath.empty() || discPath.front() != '/')
    {
        err = "disc path must start with '/': " + discPath;
        return false;
    }
    std::vector<std::string> parts;
    std::string cur;
    for (char c : discPath)
    {
        if (c == '/')
        {
            if (!cur.empty())
            {
                parts.push_back(upper(cur));
                cur.clear();
            }
        }
        else
        {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        parts.push_back(upper(cur));
    if (parts.empty())
    {
        err = "disc path names no file: " + discPath;
        return false;
    }
    // Tolerate an ISO9660 version suffix on the query (SPEECH.BIG;1):
    // record stems never carry one (detail::stem strips it).
    for (std::string &part : parts)
    {
        const size_t semi = part.find(';');
        if (semi != std::string::npos)
            part.erase(semi);
    }
    std::vector<uint8_t> pvd(kSectorSize);
    if (!read(kPvdLbn, pvd.data()) || pvd[0] != 1 || std::memcmp(&pvd[1], "CD001", 5) != 0)
    {
        err = "no ISO9660 primary volume descriptor at lbn 16";
        return false;
    }
    uint32_t lbn = le32(&pvd[156 + 2]);
    uint32_t size = le32(&pvd[156 + 10]);
    for (size_t i = 0; i < parts.size(); ++i)
    {
        const bool last = i + 1 == parts.size();
        Dir d;
        if (!readDir(read, lbn, size, d, err))
        {
            err = discPath + ": " + err;
            return false;
        }
        bool found = false;
        for (size_t r = 2; r < d.records.size(); ++r)
        {
            const auto &rec = d.records[r];
            const bool isDir = (rec[25] & 2) != 0;
            if (isDir != !last)
                continue;
            if (upper(stem(rec)) != parts[i])
                continue;
            lbn = le32(&rec[2]);
            size = le32(&rec[10]);
            found = true;
            break;
        }
        if (!found)
        {
            err = discPath + ": '" + parts[i] + "' not on the disc";
            return false;
        }
    }
    if (uint64_t(lbn) + (size + kSectorSize - 1) / kSectorSize > imageSectors)
    {
        err = discPath + ": extent past the image end";
        return false;
    }
    lbnOut = lbn;
    sizeOut = size;
    return true;
}

// Loads + validates the alias: the disc file must exist, the host file must
// be a PS2XCMP1 composite for this image, and its size must equal the disc
// file's size (so the guest's directory and the boot-loaded .hdr tables stay
// valid). Every `iso` segment must lie inside the aliased file's own range.
inline bool loadDiscAlias(const std::string &discPath, const std::filesystem::path &host, uint64_t imageSectors,
                          const SectorReader &read, DiscAlias &out, std::string &err)
{
    using namespace detail;
    out = DiscAlias{};
    uint32_t discLbn = 0;
    uint64_t discSize = 0;
    if (!findDiscFile(imageSectors, read, discPath, discLbn, discSize, err))
    {
        err = "alias " + discPath + ": " + err;
        return false;
    }
    std::error_code ec;
    const uint64_t hostBytes = std::filesystem::file_size(host, ec);
    if (ec)
    {
        err = "alias " + discPath + ": cannot size " + host.string();
        return false;
    }
    std::string head(kSectorSize, '\0');
    std::ifstream in(host, std::ios::binary);
    if (!in)
    {
        err = "alias " + discPath + ": cannot open " + host.string();
        return false;
    }
    in.read(head.data(), kSectorSize);
    head.resize(static_cast<size_t>(std::max<std::streamsize>(0, in.gcount())));
    if (head.compare(0, sizeof kCompositeMagic - 1, kCompositeMagic) != 0)
    {
        err = "alias " + discPath + ": " + host.string() + " is not a PS2XCMP1 composite";
        return false;
    }
    head.resize(std::min(head.find('\0'), head.size()));
    OverlayFile f;
    f.host = host;
    if (!parseComposite(head, hostBytes, imageSectors, f, err))
    {
        err = "alias " + discPath + ": " + host.string() + ": " + err;
        return false;
    }
    if (f.sizeBytes != discSize)
    {
        char b[192];
        std::snprintf(b, sizeof b, "alias %s: size mismatch (composite %llu, disc %llu); refused",
                      discPath.c_str(), static_cast<unsigned long long>(f.sizeBytes),
                      static_cast<unsigned long long>(discSize));
        err = b;
        return false;
    }
    const uint32_t discSectors = static_cast<uint32_t>((discSize + kSectorSize - 1) / kSectorSize);
    for (const Segment &s : f.segments)
    {
        if (s.image && (s.source < discLbn || s.source + s.sectors > uint64_t(discLbn) + discSectors))
        {
            char b[192];
            std::snprintf(b, sizeof b, "alias %s: iso segment lbn %llu + %u outside the file's range",
                          discPath.c_str(), static_cast<unsigned long long>(s.source), s.sectors);
            err = b;
            return false;
        }
    }
    out.discPath = discPath;
    out.host = host;
    out.discLbn = discLbn;
    out.discSectors = discSectors;
    out.discSize = discSize;
    out.composite = f;
    out.loaded = true;
    return true;
}

namespace alias_detail
{
struct State
{
    std::mutex mu;
    // TKP4b: pending aliases from the last mode switch (empty = off).
    std::vector<std::pair<std::string, std::string>> pending;
    uint64_t generation = 0;
    std::vector<DiscAlias> loaded; // this generation's successes, valid when loadedGen == generation
    uint64_t loadedGen = 0;
};

inline State &state()
{
    static State s;
    return s;
}
} // namespace alias_detail

// Called by the course-mode switch. An empty list = off.
inline void setModeAliases(const std::vector<std::pair<std::string, std::string>> &list)
{
    alias_detail::State &s = alias_detail::state();
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.pending == list)
        return;
    s.pending = list;
    ++s.generation;
}

// Single-alias form (TK25c): both empty = off.
inline void setModeAlias(const std::string &disc, const std::string &host)
{
    if (disc.empty() && host.empty())
        setModeAliases({});
    else
        setModeAliases({{disc, host}});
}

inline void clearModeAlias() { setModeAliases({}); }

// Test/observer hooks: the pending list (empty = off); the single form
// returns the first pending pair (empty pair = off).
inline std::vector<std::pair<std::string, std::string>> pendingAliases()
{
    alias_detail::State &s = alias_detail::state();
    std::lock_guard<std::mutex> lock(s.mu);
    return s.pending;
}

inline std::pair<std::string, std::string> pendingAlias()
{
    alias_detail::State &s = alias_detail::state();
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.pending.empty())
        return {};
    return s.pending.front();
}

// The loaded alias serving `lbn`, loading + validating on the first read of
// a mode activation. Returns false (serve the disc as today) when off, when
// `lbn` is outside every aliased range, or when the load was refused.
inline bool resolveActiveAlias(uint64_t imageSectors, const SectorReader &read, uint32_t lbn, DiscAlias &out)
{
    alias_detail::State &s = alias_detail::state();
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.pending.empty())
        return false;
    if (s.loadedGen != s.generation)
    {
        s.loaded.clear();
        for (const auto &[disc, host] : s.pending)
        {
            if (disc.empty() || host.empty())
                continue; // a half-empty pair is off, silently (TK25c)
            DiscAlias a;
            std::string err;
            if (loadDiscAlias(disc, std::filesystem::path(host), imageSectors, read, a, err))
            {
                uint64_t fromImage = 0;
                for (const Segment &seg : a.composite.segments)
                    fromImage += seg.image ? seg.sectors : 0;
                char b[256];
                std::snprintf(b, sizeof b, "[cd-alias] %s -> %s (%u sectors, %llu from the image)",
                              a.discPath.c_str(), a.host.string().c_str(), a.discSectors,
                              static_cast<unsigned long long>(fromImage));
                std::cerr << b << std::endl;
                s.loaded.push_back(a);
            }
            else
            {
                std::cerr << "[cd-alias] REFUSED: " << err << " (serving the disc unchanged)" << std::endl;
            }
        }
        s.loadedGen = s.generation;
    }
    for (const DiscAlias &a : s.loaded)
    {
        if (lbn < a.discLbn || lbn >= a.discLbn + a.discSectors)
            continue;
        out = a;
        return true;
    }
    return false;
}

} // namespace ps2_cd_overlay
