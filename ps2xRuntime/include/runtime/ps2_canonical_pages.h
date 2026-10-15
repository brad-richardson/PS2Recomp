#pragma once

#include "runtime/ps2_savestate.h"
#include <limits>

namespace ps2_savestate
{
// np-checksum-1: diagnostic schema 2. Content comparisons cover ALL writers,
// including direct host pointers. The caller owns a quiescent boundary for the
// entire begin/append/finish sequence; this is not a concurrent snapshot API.
// FNV64 is for accidental divergence diagnostics, not hostile attestation.
class CanonicalPages
{
public:
    static constexpr size_t pageSize = 4096;
    struct Section
    {
        uint32_t version = 0;
        std::vector<uint8_t> bytes;
        std::vector<uint64_t> hashes;
        std::vector<bool> dirty;
        size_t cursor = 0, previousSize = 0;
        bool seen = false;
    };
    void begin()
    {
        changedPages = totalPages = totalBytes = 0;
        active = nullptr;
        for (auto &[key, s] : sections) s.seen = false;
    }
    void section(const std::string &key, uint32_t version)
    {
        finishSection();
        auto &s = sections[key];
        if (s.seen) throw std::logic_error("duplicate canonical section");
        s.seen = true;
        s.version = version;
        s.cursor = 0;
        s.previousSize = s.bytes.size();
        s.dirty.assign(s.hashes.size(), false);
        active = &s;
    }
    void append(const uint8_t *p, size_t n)
    {
        if (!active) throw std::logic_error("canonical bytes outside section");
        auto &s = *active;
        if (n > (128u << 20) || s.cursor > (128u << 20) - n)
            throw std::length_error("canonical section exceeds 128 MiB");
        const size_t end = s.cursor + n;
        if (end > s.bytes.size()) s.bytes.resize(end);
        const size_t pages = (end + pageSize - 1) / pageSize;
        s.dirty.resize(pages, false);
        while (n)
        {
            const size_t index = s.cursor / pageSize;
            const size_t chunk = std::min(n, pageSize - s.cursor % pageSize);
            if (s.cursor + chunk > s.previousSize ||
                std::memcmp(s.bytes.data() + s.cursor, p, chunk) != 0)
            {
                s.dirty[index] = true;
                std::memcpy(s.bytes.data() + s.cursor, p, chunk);
            }
            s.cursor += chunk;
            p += chunk;
            n -= chunk;
        }
    }
    uint64_t finish()
    {
        finishSection();
        Writer root;
        root.str("ps2x-canonical-pages-v2-fnv64");
        size_t count = 0;
        for (const auto &[key, s] : sections) if (s.seen) ++count;
        root.u64(count);
        for (auto it = sections.begin(); it != sections.end(); )
        {
            auto &[key, s] = *it;
            if (!s.seen) { it = sections.erase(it); continue; }
            root.str(key);
            root.u32(s.version);
            root.u64(s.bytes.size());
            root.u64(s.hashes.size());
            for (uint64_t hash : s.hashes) root.u64(hash);
            ++it;
        }
        return sectionDigest(root.buf.data(), root.buf.size());
    }
    const std::map<std::string, Section> &data() const { return sections; }
    size_t changedPages = 0, totalPages = 0, totalBytes = 0;

private:
    void finishSection()
    {
        if (!active) return;
        auto &s = *active;
        s.bytes.resize(s.cursor);
        const size_t count = (s.cursor + pageSize - 1) / pageSize;
        s.hashes.resize(count);
        s.dirty.resize(count, false);
        // A shrinking final page changes its framed length even if its
        // remaining bytes match. Removed pages leave the root entirely.
        if (count && s.cursor != s.previousSize) s.dirty.back() = true;
        for (size_t i = 0; i < count; ++i)
        {
            if (!s.dirty[i]) continue;
            const size_t len = std::min(pageSize, s.bytes.size() - i * pageSize);
            Writer leaf;
            leaf.u8(0); // leaf domain, distinct from section/root framing
            leaf.u64(i);
            leaf.u64(len);
            leaf.bytes(s.bytes.data() + i * pageSize, len);
            s.hashes[i] = sectionDigest(leaf.buf.data(), leaf.buf.size());
            ++changedPages;
        }
        totalPages += count;
        totalBytes += s.bytes.size();
        active = nullptr;
    }
    std::map<std::string, Section> sections;
    Section *active = nullptr;
};
}
