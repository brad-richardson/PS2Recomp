#pragma once
// preserve-fix-1: Tricky files do not participate in the guest's ISO boot
// walk. Publish them into free slots after the fixed stock sorted prefix only at
// the menu's mode-on safe point; mode-off restores the pristine bytes.
// Addresses/layout: sub_003E4040, sub_003E39A8, sub_003E4508 (USA ELF).
#include "ps2_cd_overlay.h"

namespace ps2_ssx3_file_table
{
inline constexpr uint32_t kCount = 0x519c44u, kSeed = 0x519c58u;
inline constexpr uint32_t kBase = 0x519c78u, kNameBytes = 0x450c5cu;
inline constexpr uint32_t kCapacity = 500u;

inline uint32_t read32(const uint8_t *p)
{
    return ps2_cd_overlay::detail::le32(p);
}
inline void write32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8));
}
inline std::string normalized(std::string name)
{
    if (!name.empty() && (name[0] == '/' || name[0] == '\\')) name.erase(0, 1);
    const auto semi = name.find(';');
    if (semi != std::string::npos) name.erase(semi);
    for (char &c : name)
    {
        if (c == '\\') c = '/';
        else if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    }
    return "/" + name; // 0x3e4538 writes a leading slash before copying the query.
}
inline uint32_t hash(const std::string &name, uint32_t seed)
{
    for (auto i = name.rbegin(); i != name.rend(); ++i)
        seed = seed * 33u + static_cast<uint32_t>(static_cast<int8_t>(*i));
    return seed;
}

struct State
{
    bool enabled = false, active = false;
    uint32_t base = 0, stride = 0, seed = 0, count = 0;
    std::vector<uint8_t> stock;
    std::vector<std::string> names; // appended IDs: count + index + 1
};
inline State &state() { static State s; return s; }

inline void reset(bool enabled)
{
    state() = State{};
    state().enabled = enabled;
    ps2_cd_overlay::setDirectoryDormant(enabled);
}

inline bool transition(uint8_t *ram, size_t size, bool on, std::string &msg)
{
    auto &s = state();
    if (!s.enabled) return true;
    if (!ram || size < kBase + 4u) { msg = "file table: RAM unavailable"; return false; }
    const uint32_t base = read32(ram + kBase) & 0x1fffffffu;
    const uint32_t count = read32(ram + kCount);
    const uint32_t seed = read32(ram + kSeed);
    const uint32_t nameBytes = read32(ram + kNameBytes);
    // This ELF allocates 500 * (256 + 12) bytes for the sorted table.
    // Refuse another layout rather than assume its allocation capacity.
    const uint32_t stride = nameBytes + 12u;
    if (nameBytes != 256u || base < 0x100000u || base % 4u || !count || count > kCapacity ||
        uint64_t(base) + uint64_t(kCapacity) * stride > size)
    { msg = "file table: invalid sorted-table layout"; return false; }
    if (!s.stock.empty() && (s.base != base || s.stride != stride || s.seed != seed))
    { msg = "file table: layout changed since capture"; return false; }
    if (!on)
    {
        if (!s.active) return true;
        std::memcpy(ram + base, s.stock.data(), s.stock.size());
        write32(ram + kCount, s.count);
        s.active = false;
        std::fprintf(stderr, "[ssx3-files] restored base=0x%x count=%u stride=%u seed=0x%x\n",
                     base, s.count, stride, seed);
        return true;
    }
    if (s.active) return true;
    const auto *ov = ps2_cd_overlay::runtimeOverlay();
    if (!ov) { msg = "file table: overlay unavailable"; return false; }
    if (ov->files.size() > kCapacity - count)
    { msg = "file table: no free slots"; return false; }
    // Every refusal below happens before guest writes or snapshot ownership.
    std::vector<std::vector<uint8_t>> rows;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t *row = ram + base + i * stride;
        if (i && read32(row - stride) >= read32(row))
        { msg = "file table: stock keys not unique/unsigned-sorted"; return false; }
        rows.emplace_back(row, row + stride);
    }
    for (const auto &f : ov->files)
    {
        const auto name = normalized(f.isoPath);
        if (name.empty() || name.size() >= nameBytes || f.sizeBytes > 0xffffffffull)
        { msg = "file table: overlay name/size out of range"; return false; }
        const uint32_t key = hash(name, seed);
        for (const auto &row : rows)
            if (read32(row.data()) == key)
            { msg = "file table: key collision for " + name; return false; }
        std::vector<uint8_t> row(stride, 0);
        write32(row.data(), key);
        write32(row.data() + 4, f.lbn);
        write32(row.data() + 8, static_cast<uint32_t>(f.sizeBytes));
        std::memcpy(row.data() + 12, name.data(), name.size());
        rows.push_back(std::move(row));
    }
    // Capture all allocated slots, including the unused tail. Repeated
    // visits always restore this true stock image, never an in-mode table.
    if (s.stock.empty())
    {
        s.stock.assign(ram + base, ram + base + kCapacity * stride);
        s.base = base; s.count = count; s.stride = stride; s.seed = seed;
    }
    else if (count != s.count || std::memcmp(ram + base, s.stock.data(), s.stock.size()) != 0)
    { msg = "file table: stock bytes changed since capture"; return false; }
    // Guest CD handles cache 1-based row indices. Never move the stock
    // prefix or change the bsearch count: only overlay lookup uses the tail.
    s.names.clear();
    for (size_t i = count; i < rows.size(); ++i)
    {
        std::memcpy(ram + base + i * stride, rows[i].data(), stride);
        s.names.emplace_back(reinterpret_cast<const char *>(rows[i].data() + 12));
    }
    s.active = true;
    std::fprintf(stderr, "[ssx3-files] appended base=0x%x stock-count=%u overlay=%zu stride=%u seed=0x%x\n",
                 base, count, ov->files.size(), stride, seed);
    return true;
}
// 0x3e4508 ABI: a0=query, a2=out 1-based ID, v0=found. A miss
// delegates to the original guest lookup, including all stock queries.
inline bool lookup(uint8_t *ram, size_t size, uint32_t query, uint32_t out)
{
    const auto &s = state();
    if (!s.enabled || !s.active || !ram) return false;
    query &= 0x1fffffffu; out &= 0x1fffffffu;
    if (!query || query >= size || !out || uint64_t(out) + 4 > size) return false;
    const size_t limit = std::min<size_t>(256, size - query);
    const auto *end = static_cast<const uint8_t *>(std::memchr(ram + query, 0, limit));
    if (!end) return false;
    const auto name = normalized(std::string(reinterpret_cast<const char *>(ram + query), end - ram - query));
    for (size_t i = 0; i < s.names.size(); ++i)
        if (s.names[i] == name)
        {
            write32(ram + out, s.count + static_cast<uint32_t>(i) + 1);
            return true;
        }
    return false;
}
} // namespace ps2_ssx3_file_table
