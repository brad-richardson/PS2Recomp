// SPDX-License-Identifier: GPL-3.0-or-later OR MIT
#pragma once
// F1 (appsec-fix-1): credential-adjacent key detection, shared by the iOS
// loader (keys-only logging) and the knob dump (value redaction).
//
// Deliberately its own leaf header: ps2_env_file.h and ps2_android_env.h
// both define parseEnvFileContent, so neither can be included next to the
// other (see ps2_record_env.h) — this header defines no parser and can be
// included next to either.

#include <cctype>
#include <string>

namespace ps2x
{

// A key is sensitive when it names a credential, token, secret or network
// identity. Case-insensitive substring match over the upper-cased key.
inline std::string envKeyUpper(const std::string &key)
{
    std::string out;
    out.reserve(key.size());
    for (char c : key)
    {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

inline bool envKeyIsSensitive(const std::string &key)
{
    const std::string upper = envKeyUpper(key);
    static const char *const kTokens[] = {"NET", "TOKEN", "PASS", "KEY", "SECRET", "LOGIN"};
    for (const char *token : kTokens)
    {
        if (upper.find(token) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

} // namespace ps2x
