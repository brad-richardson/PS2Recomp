#pragma once
// N-lane Android env-file shim: parser + path helpers.
//
// Platform-neutral on purpose so the host unit test compiles this header.
// The Android-only loader (file read + setenv + logcat) lives in
// ps2xRuntime/src/lib/ps2_android_runtime.cpp and runs before main() reads
// anything. NativeActivity launches set no environment, so env-driven
// config (PS2X_CD_IMAGE, PS2X_SKIP_MOVIE, PS2X_FRAME_DUMP_DIR, ...) is read
// from <files dir>/ps2x.env instead.

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace ps2x
{

namespace android_env_detail
{
inline std::string trimAscii(const std::string &s)
{
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])))
    {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
    {
        --end;
    }
    return s.substr(begin, end - begin);
}
} // namespace android_env_detail

// Parse KEY=VALUE lines. '#' starts a comment line (after optional
// whitespace); blank lines are skipped. Lines are split at the FIRST '=',
// so values may contain '='. No quote processing: values are taken
// literally after trimming surrounding ASCII whitespace. Lines without '='
// and entries with an empty key are ignored.
inline std::vector<std::pair<std::string, std::string>> parseEnvFileContent(const std::string &content)
{
    std::vector<std::pair<std::string, std::string>> out;
    size_t pos = 0;
    while (pos <= content.size())
    {
        size_t eol = content.find_first_of("\r\n", pos);
        if (eol == std::string::npos)
        {
            eol = content.size();
        }
        const std::string line = android_env_detail::trimAscii(content.substr(pos, eol - pos));
        if (!line.empty() && line[0] != '#')
        {
            const size_t eq = line.find('=');
            if (eq != std::string::npos)
            {
                const std::string key = android_env_detail::trimAscii(line.substr(0, eq));
                const std::string value = android_env_detail::trimAscii(line.substr(eq + 1));
                if (!key.empty())
                {
                    out.emplace_back(key, value);
                }
            }
        }
        if (eol == content.size())
        {
            break;
        }
        pos = (content[eol] == '\r' && eol + 1 < content.size() && content[eol + 1] == '\n') ? eol + 2 : eol + 1;
    }
    return out;
}

// <bootElfDir>/ps2x.env — the shim reads the env file from the same
// directory PS2X_DEFAULT_BOOT_ELF points into.
inline std::string envFilePathForBootElf(const char *bootElf)
{
    if (bootElf == nullptr || bootElf[0] == '\0')
    {
        return std::string();
    }
    const std::string path(bootElf);
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
    {
        return std::string("ps2x.env");
    }
    return path.substr(0, slash) + "/ps2x.env";
}

} // namespace ps2x
