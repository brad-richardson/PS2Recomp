#pragma once

#include "runtime/ps2_savestate.h"
#include <limits>

namespace ps2_savestate
{
// np-checksum-1: partitioned schema 3. Content comparisons cover ALL writers,
// including direct host pointers. The caller owns a quiescent boundary for the
// entire begin/append/finish sequence; this is not a concurrent snapshot API.
// Large spans are sampled over 120 ticks; full=true refreshes every byte.
// Ordinary roots describe a rolling window, not an instantaneous snapshot.
// FNV64 is for accidental divergence diagnostics, not hostile attestation.
class CanonicalPages
{
public:
    static constexpr size_t pageSize = 4096;
    struct Section
    {
        uint32_t version = 0;
        std::vector<uint8_t> bytes;
        std::vector<uint64_t> hashes, tree;
        size_t leaves = 0;
        std::vector<bool> dirty;
        size_t cursor = 0, previousSize = 0;
        bool seen = false;
    };
    void begin(uint64_t tick = 0, bool full = true)
    {
        phase = tick % 120; refresh = full;
        changedPages = totalPages = totalBytes = hashedBytes = comparedBytes = 0;
        active = nullptr;
        for (auto &[key, s] : sections) s.seen = false;
    }
    void section(const std::string &key, uint32_t version)
    {
        finishSection();
        auto &s = sections[key];
        if (s.seen) throw std::logic_error("duplicate canonical section");
        s.seen = true;
        sectionRefresh = refresh || s.version != version || s.bytes.empty();
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
        const bool partition = !sectionRefresh && n >= 65536;
        const size_t end = s.cursor + n;
        if (end > s.bytes.size()) s.bytes.resize(end);
        const size_t pages = (end + pageSize - 1) / pageSize;
        s.dirty.resize(pages, false);
        while (n)
        {
            const size_t index = s.cursor / pageSize;
            // Jump whole unsampled ranges, instead of visiting ~11k pages
            // just to reject 119/120 of them. Keep the old partial final page
            // in the normal path: growth must refresh its entire old prefix.
            const size_t oldWholeEnd = s.previousSize / pageSize * pageSize;
            if (partition && index % 120 != phase && s.cursor < oldWholeEnd) {
                const size_t distance = (phase + 120 - index % 120) % 120;
                const size_t skip = std::min({n, distance * pageSize - s.cursor % pageSize,
                                              oldWholeEnd - s.cursor});
                s.cursor += skip; p += skip; n -= skip;
                continue;
            }
            const size_t chunk = std::min(n, pageSize - s.cursor % pageSize);
            if (!partition || index % 120 == phase || s.cursor + chunk > s.previousSize)
            {
                comparedBytes += chunk;
                if (s.cursor + chunk > s.previousSize ||
                    std::memcmp(s.bytes.data() + s.cursor, p, chunk) != 0)
                {
                    s.dirty[index] = true;
                    std::memcpy(s.bytes.data() + s.cursor, p, chunk);
                }
            }
            s.cursor += chunk;
            p += chunk;
            n -= chunk;
        }
    }
    // A coarse partition (card tree) refreshed at full boundaries only.
    bool retain(const std::string &key, uint32_t version)
    {
        auto it = sections.find(key);
        if (refresh || it == sections.end() || it->second.version != version) return false;
        finishSection();
        auto &s = it->second;
        if (s.seen) throw std::logic_error("duplicate canonical section");
        s.seen = true;
        totalPages += s.hashes.size(); totalBytes += s.bytes.size();
        return true;
    }
    void endSection() { finishSection(); }
    uint64_t finish()
    {
        finishSection();
        Writer root;
        root.str("ps2x-canonical-partitions-v3-fnv64");
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
            root.u64(s.tree.empty() ? 0 : s.tree[1]);
            ++it;
        }
        return sectionDigest(root.buf.data(), root.buf.size());
    }
    const std::map<std::string, Section> &data() const { return sections; }
    size_t changedPages = 0, totalPages = 0, totalBytes = 0, hashedBytes = 0, comparedBytes = 0;

private:
    size_t phase = 0;
    bool refresh = true, sectionRefresh = true;
    static uint64_t node(uint64_t left, uint64_t right)
    {
        uint8_t framed[17] = {1};
        // Explicit LE framing, independent of host byte order.
        for (unsigned i = 0; i < 8; ++i) {
            framed[1+i] = uint8_t(left >> (8*i));
            framed[9+i] = uint8_t(right >> (8*i));
        }
        return sectionDigest(framed, sizeof framed);
    }
    void finishSection()
    {
        if (!active) return;
        auto &s = *active;
        s.bytes.resize(s.cursor);
        const size_t count = (s.cursor + pageSize - 1) / pageSize;
        size_t leaves = 1;
        while (leaves < count) leaves *= 2;
        const bool rebuild = leaves != s.leaves || count != s.hashes.size();
        s.hashes.resize(count);
        if (rebuild) { s.leaves = leaves; s.tree.assign(2 * leaves, 0); }
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
            if (!rebuild) {
                size_t j = s.leaves + i;
                s.tree[j] = s.hashes[i];
                while ((j /= 2) != 0) s.tree[j] = node(s.tree[2*j], s.tree[2*j+1]);
            }
            ++changedPages;
            hashedBytes += len;
        }
        if (rebuild) {
            std::copy(s.hashes.begin(), s.hashes.end(), s.tree.begin() + s.leaves);
            for (size_t j = s.leaves; --j > 0;) s.tree[j] = node(s.tree[2*j], s.tree[2*j+1]);
        }
        totalPages += count;
        totalBytes += s.bytes.size();
        active = nullptr;
    }
    std::map<std::string, Section> sections;
    Section *active = nullptr;
};
}
