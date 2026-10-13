#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// SSX 3 v0 compact ISO sidecar. The payload keeps every retained 2048-byte
// sector in original LBA order; this table restores the original address space.
namespace ps2x::slim_cd
{
    constexpr uint64_t SectorBytes = 2048;
    constexpr char PinnedSourceSha[] = "3c2f8eb182c9c6208a6e8172a41e61c98f420abe3f42c845f6829aeb9761ebf5";
    struct Hole { uint32_t first = 0, count = 0, kind = 0; }; // kind 1 pad, 2 movie
    struct Piece { uint64_t payloadOffset = 0; uint32_t sectors = 0; bool zero = false; };
    enum class Status { Plain, Slim, Invalid };
    struct Table
    {
        Status status = Status::Plain;
        uint64_t logicalSectors = 0;
        std::vector<Hole> holes;
        std::string error;

        bool movieHole(uint32_t lba, uint32_t sectors = 1) const
        {
            for (const Hole &h : holes)
                if (h.kind == 2 && lba < uint64_t(h.first) + h.count &&
                    uint64_t(lba) + sectors > h.first)
                    return true;
            return false;
        }
        std::vector<Piece> pieces(uint32_t lba, uint32_t count) const
        {
            std::vector<Piece> out;
            uint64_t at = lba, end = at + count, omitted = 0;
            for (const Hole &h : holes)
            {
                if (at >= end) break;
                if (uint64_t(h.first) + h.count <= at) { omitted += h.count; continue; }
                if (at < h.first)
                {
                    const uint64_t n = std::min<uint64_t>(end, h.first) - at;
                    out.push_back({(at - omitted) * SectorBytes, uint32_t(n), false});
                    at += n;
                    if (at == end) break;
                }
                const uint64_t n = std::min<uint64_t>(end, uint64_t(h.first) + h.count) - at;
                if (n) { out.push_back({0, uint32_t(n), true}); at += n; }
                omitted += h.count;
            }
            if (at < end) out.push_back({(at - omitted) * SectorBytes, uint32_t(end - at), false});
            return out;
        }
    };

    inline uint32_t le32(const uint8_t *p)
    {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
    inline uint64_t le64(const uint8_t *p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }
    inline Table load(const std::filesystem::path &image)
    {
        Table t;
        const std::filesystem::path sidecar = image.string() + ".holes";
        std::error_code ec;
        if (!std::filesystem::exists(sidecar, ec))
        {
            if (ec) { t.status = Status::Invalid; t.error = "cannot stat sidecar"; }
            return t;
        }
        t.status = Status::Invalid;
        const uint64_t sidecarSize = std::filesystem::file_size(sidecar, ec);
        if (ec || sidecarSize < 84 || sidecarSize > 84 + 12 * 64)
        { t.error = "invalid sidecar size"; return t; }
        std::vector<uint8_t> b(size_t(sidecarSize), uint8_t(0));
        std::ifstream f(sidecar, std::ios::binary);
        if (!f.read(reinterpret_cast<char *>(b.data()), std::streamsize(b.size())))
        { t.error = "cannot read sidecar"; return t; }
        if (std::memcmp(b.data(), "PS2XSLM1", 8) != 0 ||
            std::memcmp(b.data() + 8, PinnedSourceSha, 64) != 0)
        { t.error = "format or pinned source SHA mismatch"; return t; }
        // Header is 8 magic + 64 ASCII SHA + 8 logical sectors + 4 count.
        if (b.size() < 84) { t.error = "short sidecar header"; return t; }
        t.logicalSectors = le64(b.data() + 72);
        const uint32_t count = le32(b.data() + 80);
        if (count == 0 || count > 64 || b.size() != 84 + size_t(count) * 12)
        { t.error = "invalid hole count"; return t; }
        uint64_t previousEnd = 0, omitted = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint8_t *p = b.data() + 84 + i * 12;
            const Hole h{le32(p), le32(p + 4), le32(p + 8)};
            if (!h.count || h.first < previousEnd || uint64_t(h.first) + h.count > t.logicalSectors ||
                (h.kind != 1 && h.kind != 2))
            { t.error = "invalid hole range"; return t; }
            previousEnd = uint64_t(h.first) + h.count;
            omitted += h.count;
            t.holes.push_back(h);
        }
        const uint64_t payloadSize = std::filesystem::file_size(image, ec);
        if (ec || t.logicalSectors < omitted || payloadSize != (t.logicalSectors - omitted) * SectorBytes)
        { t.error = "payload size mismatch"; return t; }
        t.status = Status::Slim;
        return t;
    }
}
