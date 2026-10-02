#include "runtime/gs/gs_stream_capture.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    constexpr uint64_t kCaptureCap = 6ull * 1024ull * 1024ull * 1024ull;
    enum : uint8_t { Packet = 1, PrivWrite = 2, Transfer = 3, VBlank = 4, NativeUpload = 5,
                     LocalToHost = 6, ClearContext = 7 };

    struct Capture
    {
        std::mutex mutex;
        FILE *file = nullptr;
        uint64_t bytes = 0;
        bool stoppedAtCap = false;

        void open()
        {
            if (file || stoppedAtCap)
                return;
            const char *path = std::getenv("PS2X_GS_CAPTURE");
            if (!path || !path[0])
            {
                stoppedAtCap = true;
                return;
            }
            file = std::fopen(path, "wb");
            if (!file)
            {
                std::fprintf(stderr, "[gs:capture] open failed path=%s\n", path);
                stoppedAtCap = true;
                return;
            }
            constexpr char magic[] = "PS2XGSC1";
            std::fwrite(magic, 1, sizeof(magic) - 1u, file);
            bytes = sizeof(magic) - 1u;
        }

        bool write(const uint8_t *payload, uint32_t payloadSize)
        {
            std::lock_guard<std::mutex> lock(mutex);
            open();
            if (!file || bytes + sizeof(payloadSize) + payloadSize > kCaptureCap)
            {
                if (file)
                {
                    std::fprintf(stderr, "[gs:capture] uncompressed byte cap reached bytes=%llu cap=%llu\n",
                                 static_cast<unsigned long long>(bytes),
                                 static_cast<unsigned long long>(kCaptureCap));
                    std::fclose(file);
                    file = nullptr;
                }
                stoppedAtCap = true;
                return false;
            }
            const bool ok = std::fwrite(&payloadSize, sizeof(payloadSize), 1, file) == 1 &&
                            std::fwrite(payload, 1, payloadSize, file) == payloadSize;
            if (!ok)
            {
                std::fprintf(stderr, "[gs:capture] write failed\n");
                std::fclose(file);
                file = nullptr;
                stoppedAtCap = true;
                return false;
            }
            bytes += sizeof(payloadSize) + payloadSize;
            return true;
        }
    };

    Capture &capture()
    {
        static Capture value;
        return value;
    }

    void put32(uint8_t *out, uint32_t value) { std::memcpy(out, &value, sizeof(value)); }
    void put64(uint8_t *out, uint64_t value) { std::memcpy(out, &value, sizeof(value)); }

    bool record(uint8_t kind, uint64_t tick, const uint8_t *body, uint32_t bodySize)
    {
        std::array<uint8_t, 9> header{};
        header[0] = kind;
        put64(header.data() + 1, tick);
        std::lock_guard<std::mutex> lock(capture().mutex);
        Capture &c = capture();
        c.open();
        if (!c.file || c.bytes + 4u + header.size() + bodySize > kCaptureCap)
        {
            if (c.file)
            {
                std::fprintf(stderr, "[gs:capture] uncompressed byte cap reached bytes=%llu cap=%llu\n",
                             static_cast<unsigned long long>(c.bytes),
                             static_cast<unsigned long long>(kCaptureCap));
                std::fclose(c.file);
                c.file = nullptr;
            }
            c.stoppedAtCap = true;
            return false;
        }
        const uint32_t payloadSize = static_cast<uint32_t>(header.size() + bodySize);
        const bool ok = std::fwrite(&payloadSize, 4, 1, c.file) == 1 &&
                        std::fwrite(header.data(), 1, header.size(), c.file) == header.size() &&
                        (bodySize == 0 || std::fwrite(body, 1, bodySize, c.file) == bodySize);
        if (!ok)
        {
            std::fprintf(stderr, "[gs:capture] write failed\n");
            std::fclose(c.file);
            c.file = nullptr;
            c.stoppedAtCap = true;
            return false;
        }
        c.bytes += 4u + payloadSize;
        return true;
    }
}

namespace ps2x_gs_capture
{
    uint64_t bisectTo()
    {
        static const uint64_t value = [] {
            const char *env = std::getenv("PS2X_GS_BISECT_TO");
            return env && *env ? std::strtoull(env, nullptr, 10) : 0ull;
        }();
        return value;
    }

    uint64_t stopAt()
    {
        static const uint64_t value = [] {
            const char *env = std::getenv("PS2X_GS_CAPTURE_STOP_TICK");
            return env && *env ? std::strtoull(env, nullptr, 10) : 0ull;
        }();
        return value;
    }

    bool enabled()
    {
        static const bool on = [] {
            const char *path = std::getenv("PS2X_GS_CAPTURE");
            return path && path[0];
        }();
        return on;
    }

    void packet(uint64_t tick, uint8_t path, const uint8_t *data, uint32_t sizeBytes)
    {
        if (!enabled() || !data || sizeBytes == 0)
            return;
        std::vector<uint8_t> body(5u + sizeBytes);
        body[0] = path;
        put32(body.data() + 1, sizeBytes);
        std::memcpy(body.data() + 5, data, sizeBytes);
        record(Packet, tick, body.data(), static_cast<uint32_t>(body.size()));
    }

    void packetDone(uint64_t tick, uint64_t index, uint8_t path,
                    const uint8_t *vram, uint32_t vramSize)
    {
        const uint64_t to = bisectTo();
        if (!enabled() || to == 0u || tick >= to || !vram || vramSize == 0u)
            return;
        uint32_t hash = 2166136261u;
        for (uint32_t i = 0; i < vramSize; ++i)
        {
            hash ^= vram[i];
            hash *= 16777619u;
        }
        std::fprintf(stderr, "[gb4:packet] idx=%llu tick=%llu path=%u vram=%08x\n",
                     static_cast<unsigned long long>(index),
                     static_cast<unsigned long long>(tick),
                     static_cast<unsigned>(path), hash);
    }

    void privWrite(uint64_t tick, uint32_t registerOffset, uint64_t value)
    {
        if (!enabled())
            return;
        uint8_t body[12];
        put32(body, registerOffset);
        put64(body + 4, value);
        record(PrivWrite, tick, body, sizeof(body));
    }

    void nativeUpload(uint64_t tick, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg,
                      uint64_t trxdir, const uint8_t *data, uint32_t sizeBytes)
    {
        if (!enabled() || !data || sizeBytes == 0)
            return;
        std::vector<uint8_t> body(36u + sizeBytes);
        put64(body.data(), bitbltbuf);
        put64(body.data() + 8, trxpos);
        put64(body.data() + 16, trxreg);
        put64(body.data() + 24, trxdir);
        put32(body.data() + 32, sizeBytes);
        std::memcpy(body.data() + 36, data, sizeBytes);
        record(NativeUpload, tick, body.data(), static_cast<uint32_t>(body.size()));
    }

    void transfer(uint64_t tick, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg,
                  uint32_t direction)
    {
        if (!enabled())
            return;
        uint8_t body[28];
        put64(body, bitbltbuf);
        put64(body + 8, trxpos);
        put64(body + 16, trxreg);
        put32(body + 24, direction);
        record(Transfer, tick, body, sizeof(body));
    }

    void localToHost(uint64_t tick, uint32_t maxBytes, const uint8_t *data, uint32_t sizeBytes)
    {
        if (!enabled() || (sizeBytes != 0u && !data))
            return;
        std::vector<uint8_t> body(8u + sizeBytes);
        put32(body.data(), maxBytes);
        put32(body.data() + 4, sizeBytes);
        if (sizeBytes != 0u)
            std::memcpy(body.data() + 8, data, sizeBytes);
        record(LocalToHost, tick, body.data(), static_cast<uint32_t>(body.size()));
    }

    void clearContext(uint64_t tick, uint32_t contextIndex, uint32_t rgba)
    {
        if (!enabled())
            return;
        uint8_t body[8];
        put32(body, contextIndex);
        put32(body + 4, rgba);
        record(ClearContext, tick, body, sizeof(body));
    }

    void vblank(uint64_t tick)
    {
        if (!enabled() || !record(VBlank, tick, nullptr, 0))
            return;
        Capture &c = capture();
        std::lock_guard<std::mutex> lock(c.mutex);
        if (c.file && std::fflush(c.file) != 0)
        {
            std::fprintf(stderr, "[gs:capture] flush failed\n");
            std::fclose(c.file);
            c.file = nullptr;
            c.stoppedAtCap = true;
        }
        else if (c.file && ((stopAt() != 0u && tick >= stopAt()) ||
                            (bisectTo() != 0u && tick >= bisectTo())))
        {
            std::fclose(c.file);
            c.file = nullptr;
            c.stoppedAtCap = true;
            std::fprintf(stderr, "[gs:capture] stopped at marker tick=%llu bytes=%llu\n",
                         static_cast<unsigned long long>(tick),
                         static_cast<unsigned long long>(c.bytes));
        }
    }

    void close()
    {
        Capture &c = capture();
        std::lock_guard<std::mutex> lock(c.mutex);
        if (c.file)
        {
            std::fflush(c.file);
            std::fclose(c.file);
            c.file = nullptr;
            std::fprintf(stderr, "[gs:capture] closed bytes=%llu\n",
                         static_cast<unsigned long long>(c.bytes));
        }
    }
}
