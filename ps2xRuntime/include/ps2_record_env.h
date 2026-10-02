#pragma once
// PL1: record-header env hash (env_sha) plumbing. Deliberately its own
// header: ps2_env_file.h and ps2_android_env.h both define
// parseEnvFileContent, so neither can be included next to the other.
//
// FNV-1a 64 over raw env-file bytes, hex-encoded. The platform env loaders
// (Android shim, iOS prepareEnvironment) stash the hash of exactly what
// they consumed at startup; the pad recorder reads it back when it arms.
// One slot per process; loaders run single-threaded before main, the
// recorder reads later.

#include <cstdint>
#include <cstdio>
#include <string>

namespace ps2x
{
inline uint64_t fnv1a64Bytes(const void *data, size_t size, uint64_t hash = 14695981039346656037ull)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

inline std::string fnv1a64Hex(const std::string &bytes)
{
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(fnv1a64Bytes(bytes.data(), bytes.size())));
    return hex;
}

inline std::string &recordedEnvFileHashSlot()
{
    static std::string s;
    return s;
}

inline void setRecordedEnvFileHash(const std::string &hex)
{
    recordedEnvFileHashSlot() = hex;
}

// "" (nothing loaded: desktop, missing file) reads back as "none".
inline const char *recordedEnvFileHash()
{
    const std::string &s = recordedEnvFileHashSlot();
    return s.empty() ? "none" : s.c_str();
}
} // namespace ps2x
