// E7 bounded observation of the SSX3 fixed-buffer copy join.
// PS2X_E7_DIR gates every tap; no guest state, packet, or scheduling writes.
// Separate byte budgets reserve the 599..603 boundary even if boot is noisy.
#pragma once
#include <atomic>
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
// E15 opt-in alignment uses only diagnostic atomics; no guest writes.
inline bool aligned() { static const bool yes = [] { const char *p=std::getenv("PS2X_E15_ALIGN"); return p && std::strcmp(p,"1")==0; }(); return yes; }
inline std::atomic<uint64_t> &alignedArm() { static std::atomic<uint64_t> value{UINT64_MAX}; return value; }
inline bool window(uint64_t tick) {
    const uint64_t a=alignedArm().load();
    return aligned() ? (a!=UINT64_MAX && tick>=a && tick<=a+1u) : (tick>=599u && tick<=603u);
}
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
    bool opened = false, finished = false, closed = false, packetTruncated = false;
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
        if (s.file) std::fprintf(s.file, "# E7 observation only; ticks 0..603; boundary 599..603 unless E15 aligned trigger; byte caps boot=%llu boundary=%llu packets=%llu\n",
            static_cast<unsigned long long>(kBootBytes), static_cast<unsigned long long>(kWindowBytes), static_cast<unsigned long long>(kPacketBytes));
    }
    if (!s.file || s.finished || s.closed) return;
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
inline void shutdown(uint64_t tick)
{
    if (!enabled()) return;
    Sink &s=sink(); std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.file || s.closed) return;
    std::fprintf(s.file,"# E7 SHUTDOWN tick=%llu events=%llu bootBytes=%llu boundaryBytes=%llu packetBytes=%llu packetFiles=%llu bootTruncated=%d boundaryTruncated=%d packetTruncated=%d windowComplete=%d aligned=%d arm=%llu\n",
        static_cast<unsigned long long>(tick),static_cast<unsigned long long>(s.seq),
        static_cast<unsigned long long>(s.budget.boot),static_cast<unsigned long long>(s.budget.boundary),
        static_cast<unsigned long long>(s.packetBytes),static_cast<unsigned long long>(s.packets),
        s.budget.bootTruncated,s.budget.boundaryTruncated,s.packetTruncated,s.finished,aligned(),
        static_cast<unsigned long long>(alignedArm().load()));
    std::fflush(s.file); std::fclose(s.file); s.file=nullptr; s.closed=true;
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
    if (size > kPacketBytes - s.packetBytes || (aligned() && s.packets>=16u)) { s.packetTruncated = true; return; }
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
// E11 read-only card-query observations share E7's ordered sink and byte caps.
inline std::atomic<uint32_t> &cardObject() { static std::atomic<uint32_t> value{0}; return value; }
inline std::atomic<uint32_t> &cardUi() { static std::atomic<uint32_t> value{0}; return value; }
inline bool cardAddress(uint32_t a) { return a != 0 && a <= 0x02000000u - 0x500u; }
inline void cardWrite(uint64_t tick, const uint8_t *ram, uint32_t address,
                      uint32_t width, uint64_t lo, uint32_t pc, int thread)
{
    if (!enabled() || tick > 603u || !ram) return;
    if (pc == 0x2c3fc4u && width == 4u && lo == 0x486f78u && cardAddress(address))
    {
        cardObject().store(address);
        event(tick, "mc-constructor", "MC=0x%x addr=0x%x width=%u value=0x%llx pc=0x%x thread=%d",
              address,address,width,static_cast<unsigned long long>(lo),pc,thread);
    }
    const uint32_t mc = cardObject().load();
    if (pc == 0x23d5c8u && width == 4u && cardAddress(address-0x434u))
    {
        cardUi().store(address-0x434u);
        event(tick, "mc-ui-pointer", "UI=0x%x addr=0x%x value=0x%llx MC=0x%x guard=%u pc=0x%x thread=%d",
              address-0x434u,address,static_cast<unsigned long long>(lo),mc,
              lo==mc && cardAddress(mc) && word(ram,mc)==0x486f78u,pc,thread);
    }
    const uint32_t ui=cardUi().load();
    if (aligned() && ui && cardAddress(ui) && cardAddress(mc) && word(ram,mc)==0x486f78u &&
        word(ram,ui+0x434u)==mc && address==ui+0x130u && width==4u &&
        word(ram,address)==3u && lo==6u)
    {
        uint64_t unset=UINT64_MAX;
        if (alignedArm().compare_exchange_strong(unset,tick+1u))
            event(tick,"e15-align","UI=0x%x MC=0x%x old=3 new=6 pc=0x%x arm=%llu freeze=%llu guard=1",
                ui,mc,pc,static_cast<unsigned long long>(tick+1u),static_cast<unsigned long long>(tick+2u));
    }
    const uint32_t off=address-mc;
    if ((mc && (off==0u || off==4u || off==0xcu || off==0x10u || off==0x40u || off==0x4cu || off==0x184u || off==0x198u)) ||
        (ui && (address==ui+0x338u || address==ui+0x344u || address==ui+0x424u || address==ui+0x43cu || address==ui+0x440u || address==ui+0x130u || address==ui+0xb8u || address==ui+0x748u)) || (address>=0x4a3938u && address<=0x4a3944u))
        event(tick,"mc-write","MC=0x%x UI=0x%x addr=0x%x width=%u old=0x%x value=0x%llx pc=0x%x thread=%d",
              mc,ui,address,width,word(ram,address),static_cast<unsigned long long>(lo),pc,thread);
}
inline bool cardTarget(uint32_t target, uint32_t source = 0u)
{
    return target==0x2c5300u || target==0x2c5140u || target==0x2c5358u || target==0x2c4480u || target==0x2c48c0u ||
           target==0x2c4980u || target==0x2c50e0u || target==0x40a498u || target==0x40a360u ||
           target==0x2d3810u || target==0x241b20u || target==0x241cd8u || target==0x23e540u ||
           target==0x23eb50u || target==0x23cf38u || target==0x23d570u ||
           source==0x23eb68u || source==0x23e7e4u || source==0x23e800u || source==0x23e528u;
}
inline void cardCall(uint64_t tick, const char *phase, const uint8_t *ram,
                     uint32_t target, uint32_t source, uint32_t entryA0, uint32_t entryA1,
                     uint32_t pc, uint32_t v0, uint32_t a1, uint32_t a2,
                     uint32_t a3, uint32_t sp, uint32_t ra, int thread)
{
    if (!enabled() || tick > 603u || !ram || !cardTarget(target,source)) return;
    const uint32_t mc=(target==0x2c5300u || target==0x2c5140u || target==0x2c5358u || target==0x2d3810u) ? entryA0 : cardObject().load();
    const uint32_t ui=cardUi().load(); const bool safe=cardAddress(mc);
    // E13: UI flags/state and route words, guarded separately for the larger object.
    const bool uiSafe=ui && ui<=0x02000000u-0x750u;
    const uint32_t route=uiSafe?word(ram,ui+0x748u):0u;
    const bool routeSafe=route && route<=0x02000000u-0x10u;
    event(tick,phase,"target=0x%x source=0x%x a0=0x%x a1=0x%x a1Now=0x%x a2=0x%x a3=0x%x pc=0x%x v0=0x%x sp=0x%x ra=0x%x thread=%d MC=0x%x constructor=0x%x VT=0x%x state=%u outstanding=%u port=%u activePort=%u command=%u UI=0x%x pending=%u slot0=%d slot1=%d info=%u,%u,%u,%u uiFlags=0x%x uiState=%u uiMode=%u uiRoute=0x%x uiRouteTarget=0x%x",
          target,source,entryA0,entryA1,a1,a2,a3,pc,v0,sp,ra,thread,mc,cardObject().load(),
          safe?word(ram,mc):0u,safe?word(ram,mc+4u):0u,safe?word(ram,mc+0x40u):0u,
          safe?word(ram,mc+0xcu):0u,safe?word(ram,mc+0x10u):0u,safe?word(ram,mc+0x4cu):0u,
          ui,cardAddress(ui)?word(ram,ui+0x338u):0u,
          safe?static_cast<int32_t>(word(ram,mc+0x184u)):0,safe?static_cast<int32_t>(word(ram,mc+0x198u)):0,
          word(ram,0x4a3938u),word(ram,0x4a393cu),word(ram,0x4a3940u),word(ram,0x4a3944u),
          uiSafe?word(ram,ui+0x43cu):0u,uiSafe?word(ram,ui+0x130u):0u,uiSafe?word(ram,ui+0xb8u):0u,
          route,routeSafe?word(ram,route+0xcu):0u);
}

inline void fields(uint64_t tick, const uint8_t *ram, uint32_t address, uint32_t width,
                   uint64_t lo, uint64_t hi, uint32_t pc, int thread)
{
    if (!enabled() || !ram) return;
    cardWrite(tick, ram, address, width, lo, pc, thread);
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
