#pragma once
// I25: ps2x.env file support (parser, placeholder expansion, layering).
//
// Platform-neutral on purpose so the host unit test compiles this header.
// The iOS loader (ps2xRuntime/src/lib/ps2_ios_runtime.cpp) reads the files
// and calls setenv; home-screen launches set no environment, so env-driven
// config (PS2X_CD_IMAGE, PS2X_SKIP_MOVIE, PS2X_PAD_SCRIPT, ...) comes from
// <bundle>/ps2x.env, overridden by <Documents>/ps2x.env. Same file format
// as the Android shim on n2-android (N3, 322e55b).

#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ps2x
{

namespace env_file_detail
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
} // namespace env_file_detail

// Parse KEY=VALUE lines. '#' starts a comment line (after optional
// whitespace); blank lines are skipped. Lines are split at the FIRST '=',
// so values may contain '='. No quote processing: values are taken
// literally after trimming surrounding ASCII whitespace. Lines without '='
// and entries with an empty key are ignored. An empty value is kept (it
// clears a key set by an earlier layer, e.g. PS2X_PAD_SCRIPT=).
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
        const std::string line = env_file_detail::trimAscii(content.substr(pos, eol - pos));
        if (!line.empty() && line[0] != '#')
        {
            const size_t eq = line.find('=');
            if (eq != std::string::npos)
            {
                const std::string key = env_file_detail::trimAscii(line.substr(0, eq));
                const std::string value = env_file_detail::trimAscii(line.substr(eq + 1));
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

// Replace ${NAME} with vars[NAME]. Unknown names and unterminated "${" are
// left as written, so a typo shows up verbatim in the logged value.
inline std::string expandEnvPlaceholders(const std::string &value, const std::map<std::string, std::string> &vars)
{
    std::string out;
    size_t pos = 0;
    while (pos < value.size())
    {
        const size_t open = value.find("${", pos);
        if (open == std::string::npos)
        {
            out.append(value, pos, std::string::npos);
            break;
        }
        const size_t close = value.find('}', open + 2);
        if (close == std::string::npos)
        {
            out.append(value, pos, std::string::npos);
            break;
        }
        out.append(value, pos, open - pos);
        const auto it = vars.find(value.substr(open + 2, close - open - 2));
        if (it != vars.end())
        {
            out += it->second;
        }
        else
        {
            out.append(value, open, close - open + 1);
        }
        pos = close + 1;
    }
    return out;
}

// Merge env-file layers in order (later layers override earlier ones) and
// drop keys in `protectedKeys` (already set by the launcher, which wins).
// Returns the final KEY -> VALUE assignments, placeholders expanded.
inline std::map<std::string, std::string> mergeEnvLayers(
    const std::vector<std::vector<std::pair<std::string, std::string>>> &layers,
    const std::set<std::string> &protectedKeys,
    const std::map<std::string, std::string> &vars)
{
    std::map<std::string, std::string> out;
    for (const auto &layer : layers)
    {
        for (const auto &entry : layer)
        {
            if (protectedKeys.count(entry.first) != 0)
            {
                continue;
            }
            out[entry.first] = expandEnvPlaceholders(entry.second, vars);
        }
    }
    return out;
}

} // namespace ps2x
