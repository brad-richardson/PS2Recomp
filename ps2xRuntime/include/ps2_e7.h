// E7 bounded observation of the SSX3 fixed-buffer copy join.
// PS2X_E7_DIR gates every tap; no guest state, packet, or scheduling writes.
// Separate byte budgets reserve the 599..603 boundary even if boot is noisy.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace ps2_e7
{
inline constexpr uint64_t kBootBytes = 4u * 1024u * 1024u;
inline constexpr uint64_t kWindowBytes = 1u * 1024u * 1024u;
inline constexpr uint64_t kPacketBytes = 512u * 1024u;
inline bool window(uint64_t tick) { return tick >= 599u && tick <= 603u; }
inline const char *directory()
{
    static const char *dir = [] { const char *p = std::getenv("PS2X_E7_DIR"); return p && *p ? p : nullptr; }();
    return dir;
}
inline bool enabled() { return directory() != nullptr; }
struct Budget
{
    uint64_t boot = 0, boundary = 0;
    bool bootTruncated = false, boundaryTruncated = false;
    bool admit(uint64_t bytes, bool inWindow)
    {
        uint64_t &used = inWindow ? boundary : boot;
        bool &truncated = inWindow ? boundaryTruncated : bootTruncated;
        const uint64_t limit = inWindow ? kWindowBytes : kBootBytes;
        if (bytes > limit - used) { truncated = true; return false; }
        used += bytes;
        return true;
    }
};
struct Sink
{
    std::mutex mutex;
    FILE *file = nullptr;
    Budget budget;
    uint64_t seq = 0, packetBytes = 0, packets = 0;
    bool opened = false, finished = false, packetTruncated = false;
};
inline Sink &sink() { static Sink s; return s; }
inline uint64_t hash(const uint8_t *data, uint32_t size)
{
    uint64_t h = 14695981039346656037ull;
    for (uint32_t i = 0; i < size; ++i) { h ^= data[i]; h *= 1099511628211ull; }
    return h;
}
inline void event(uint64_t tick, const char *kind, const char *fmt, ...)
{
    if (!enabled()) return;
    Sink &s = sink();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.opened)
    {
        s.opened = true;
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/e7-events.txt", directory());
        s.file = std::fopen(path, "w");
        if (s.file) std::fprintf(s.file, "# E7 observation only; ticks 0..603; boundary 599..603; byte caps boot=%llu boundary=%llu packets=%llu\n",
            static_cast<unsigned long long>(kBootBytes), static_cast<unsigned long long>(kWindowBytes), static_cast<unsigned long long>(kPacketBytes));
    }
    if (!s.file || s.finished) return;
    if (tick > 603u)
    {
        std::fprintf(s.file, "# E7 COMPLETE tick=%llu events=%llu bootBytes=%llu boundaryBytes=%llu packetBytes=%llu bootTruncated=%d boundaryTruncated=%d packetTruncated=%d\n",
            static_cast<unsigned long long>(tick), static_cast<unsigned long long>(s.seq),
            static_cast<unsigned long long>(s.budget.boot), static_cast<unsigned long long>(s.budget.boundary),
            static_cast<unsigned long long>(s.packetBytes), s.budget.bootTruncated, s.budget.boundaryTruncated, s.packetTruncated);
        std::fflush(s.file); s.finished = true; return;
    }
    char body[768];
    va_list args; va_start(args, fmt);
    const int length = std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(body))
    {
        if (window(tick)) s.budget.boundaryTruncated = true; else s.budget.bootTruncated = true;
        return;
    }
    char row[1024];
    const int n = std::snprintf(row, sizeof(row), "seq=%llu tick=%llu kind=%s %s\n",
        static_cast<unsigned long long>(s.seq + 1u), static_cast<unsigned long long>(tick), kind, body);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(row) || !s.budget.admit(static_cast<uint64_t>(n), window(tick))) return;
    ++s.seq;
    std::fwrite(row, 1, static_cast<size_t>(n), s.file); std::fflush(s.file);
}
inline void packet(uint64_t tick, const char *kind, const uint8_t *data, uint32_t size,
                   bool masked = false, size_t queued = 0u, uint32_t source = 0u)
{
    if (!enabled() || tick > 603u || !data || size < 16u) return;
    uint64_t tag = 0;
    std::memcpy(&tag, data, sizeof(tag));
    if (std::strcmp(kind, "gs-enter") == 0 && !window(tick) &&
        tag != 0x100000000000000bull && tag != 0x1000000000008010ull) return;
    const uint64_t digest = hash(data, size);
    event(tick, kind, "bytes=%u fnv64=0x%llx tag=0x%llx mask=%u queued=%zu source=0x%x",
          size, static_cast<unsigned long long>(digest), static_cast<unsigned long long>(tag), masked, queued, source);
    if (source != 0x004ffcc0u || !window(tick)) return;
    Sink &s = sink();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (size > kPacketBytes - s.packetBytes) { s.packetTruncated = true; return; }
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/e7-copy-tick%llu-%llu.bin", directory(),
        static_cast<unsigned long long>(tick), static_cast<unsigned long long>(++s.packets));
    FILE *f = std::fopen(path, "wb");
    if (!f) { s.packetTruncated = true; return; }
    const size_t written = std::fwrite(data, 1, size, f);
    std::fclose(f); s.packetBytes += written;
    if (written != size) s.packetTruncated = true;
}
inline uint32_t word(const uint8_t *ram, uint32_t address)
{
    uint32_t value; std::memcpy(&value, ram + address, sizeof(value)); return value;
}
inline void fields(uint64_t tick, const uint8_t *ram, uint32_t address, uint32_t width,
                   uint64_t lo, uint64_t hi, uint32_t pc, int thread)
{
    if (!enabled() || !ram) return;
    const uint32_t s = word(ram, 0x4a289cu);
    if (address == 0x4a289cu)
        event(tick, "singleton", "addr=0x%x width=%u value=0x%llx pc=0x%x thread=%d oldS=0x%x",
              address, width, static_cast<unsigned long long>(lo), pc, thread, s);
    if (!s || s > 0x02000000u - 0x75e0u) return;
    const uint32_t off = address - s;
    if (off != 0xf44u && off != 0x59e8u && off != 0x5a74u && off != 0x5a78u &&
        off != 0x5a7cu && off != 0x5a84u && off != 0x5a88u) return;
    event(tick, "field-before", "S=0x%x off=0x%x width=%u lo=0x%llx hi=0x%llx pc=0x%x thread=%d C=%u G=%u M=%u A=%u B=%u P=%u D=%u",
          s, off, width, static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi), pc, thread,
          word(ram,s+0x5a74u),word(ram,s+0xf44u),word(ram,s+0x59e8u),word(ram,s+0x5a78u),
          word(ram,s+0x5a7cu),word(ram,s+0x5a84u),word(ram,s+0x5a88u));
}
} // namespace ps2_e7
