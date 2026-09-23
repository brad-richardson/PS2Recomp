// E51 DEV-ONLY raw GIF packet dump behind PS2X_GIF_DUMP=<file>.
//
// Every packet entering GS::processGIFPacket is appended as one record:
//   u32 magic 0x50464947 ('GIFP'), u32 vsync, u32 path (1..3),
//   u32 vu1StartPc (latest VIF MSCAL startPC, ~0u when unknown),
//   u32 size, size bytes of packet data.
// Windows (inclusive guest vsyncs):
//   PS2X_GIF_DUMP_FROM / _TO    all paths (default 0..2^64-1)
//   PS2X_GIF_DUMP_IMG_FROM      PATH2/PATH3 packets are also kept from this
//                               vsync up to _TO (texture uploads before the
//                               window); default = _FROM
// Hard cap 256 MiB, then the file goes quiet. Unset (default) = one relaxed
// atomic check per packet; no I/O.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace ps2_e51_gifdump
{

inline constexpr uint32_t kMagic = 0x50464947u;
inline constexpr uint64_t kMaxBytes = 256ull * 1024ull * 1024ull;

namespace detail
{
    struct State
    {
        std::mutex mutex;
        std::string path;
        std::FILE *out = nullptr;
        uint64_t from = 0u;
        uint64_t to = ~0ull;
        uint64_t imgFrom = 0u;
        uint64_t bytes = 0u;
        bool capped = false;
    };

    inline State &state()
    {
        static State s;
        return s;
    }

    inline std::atomic<int> &flag()
    {
        static std::atomic<int> f{-1};
        return f;
    }

    inline bool parseU64(const char *t, uint64_t &out)
    {
        if (!t || !*t)
            return false;
        uint64_t v = 0u;
        for (const char *p = t; *p; ++p)
        {
            if (*p < '0' || *p > '9')
                return false;
            v = v * 10u + static_cast<uint64_t>(*p - '0');
        }
        out = v;
        return true;
    }

    inline void init()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (flag().load(std::memory_order_relaxed) >= 0)
            return;
        const char *file = std::getenv("PS2X_GIF_DUMP");
        int on = 0;
        if (file && *file)
        {
            s.path = file;
            uint64_t v = 0u;
            if (parseU64(std::getenv("PS2X_GIF_DUMP_FROM"), v))
                s.from = v;
            if (parseU64(std::getenv("PS2X_GIF_DUMP_TO"), v))
                s.to = v;
            s.imgFrom = s.from;
            if (parseU64(std::getenv("PS2X_GIF_DUMP_IMG_FROM"), v))
                s.imgFrom = v;
            on = 1;
        }
        flag().store(on, std::memory_order_release);
    }

    inline void put32(State &s, uint32_t v)
    {
        std::fwrite(&v, sizeof(v), 1, s.out);
    }
} // namespace detail

inline bool enabled()
{
    int f = detail::flag().load(std::memory_order_relaxed);
    if (f < 0)
    {
        detail::init();
        f = detail::flag().load(std::memory_order_relaxed);
    }
    return f > 0;
}

// path: 1..3. Pure window predicate (unit-tested).
inline bool keep(uint64_t vsync, uint32_t path, uint64_t from, uint64_t to, uint64_t imgFrom)
{
    if (vsync > to)
        return false;
    if (vsync >= from)
        return true;
    return path >= 2u && vsync >= imgFrom;
}

inline void notePacket(uint64_t vsync, uint32_t path, uint32_t vu1Pc, const uint8_t *data, uint32_t size)
{
    if (!enabled() || !data || size == 0u)
        return;
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.capped || !keep(vsync, path, s.from, s.to, s.imgFrom))
        return;
    if (!s.out)
    {
        s.out = std::fopen(s.path.c_str(), "wb");
        if (!s.out)
        {
            s.capped = true;
            return;
        }
    }
    if (s.bytes + 20u + size > kMaxBytes)
    {
        s.capped = true;
        std::fflush(s.out);
        return;
    }
    detail::put32(s, kMagic);
    detail::put32(s, static_cast<uint32_t>(vsync));
    detail::put32(s, path);
    detail::put32(s, vu1Pc);
    detail::put32(s, size);
    std::fwrite(data, 1, size, s.out);
    s.bytes += 20u + size;
    if (vsync >= s.to)
        std::fflush(s.out);
}

// Test hooks.
inline void configureForTest(const char *path, uint64_t from, uint64_t to, uint64_t imgFrom)
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.out)
    {
        std::fclose(s.out);
        s.out = nullptr;
    }
    s.path = path;
    s.from = from;
    s.to = to;
    s.imgFrom = imgFrom;
    s.bytes = 0u;
    s.capped = false;
    detail::flag().store(1, std::memory_order_release);
}

inline void clearForTest()
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.out)
    {
        std::fclose(s.out);
        s.out = nullptr;
    }
    s.path.clear();
    s.bytes = 0u;
    s.capped = false;
    detail::flag().store(0, std::memory_order_release);
}

} // namespace ps2_e51_gifdump
