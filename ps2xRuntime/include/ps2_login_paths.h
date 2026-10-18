#pragma once
// F2 (appsec-fix-1): online-secret file locations + one-time migration.
//
// The Quick Play login (name:pass), its persistent install-ID sibling
// (.device-id) and the first-online notice flag (.notice-v1) must live in
// non-shared app storage, never in the shared Documents / external-files
// tree that users copy for support. iOS uses Application Support
// (~/Library/Application Support); Android uses the internal files dir
// (Context.getFilesDir, published as PS2X_ANDROID_INTERNAL_DIR by
// RunnerApplication before native load); desktop keeps ~/.config.
//
// migrateLoginFiles(oldDir, newDir) moves the three files when the new side
// is missing and the old side exists. It touches only these three leaf
// names: cards and every other file are never deleted, moved or overwritten
// (an existing new-side file always wins; missing old-side files are skipped).

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace ps2x::loginpaths
{

inline constexpr char kLoginFileName[] = "quickplay.login";

inline const std::array<std::string, 3> &loginSuffixes()
{
    static const std::array<std::string, 3> kSuffixes = {"", ".device-id", ".notice-v1"};
    return kSuffixes;
}

inline std::string loginPathForDir(const std::string &dir)
{
    if (dir.empty())
    {
        return std::string();
    }
    return dir + "/" + kLoginFileName;
}

// Moves oldDir/quickplay.login{, .device-id, .notice-v1} to newDir when the
// new-side file is absent and the old-side file exists. Creates newDir.
// Returns the number of files moved. Never overwrites, never deletes cards.
inline size_t migrateLoginFiles(const std::string &oldDir, const std::string &newDir)
{
    if (oldDir.empty() || newDir.empty() || oldDir == newDir)
    {
        return 0;
    }
    std::error_code ec;
    std::filesystem::create_directories(newDir, ec);
    ec.clear();
    size_t moved = 0;
    for (const std::string &suffix : loginSuffixes())
    {
        const std::filesystem::path oldPath = std::filesystem::path(oldDir) / (kLoginFileName + suffix);
        const std::filesystem::path newPath = std::filesystem::path(newDir) / (kLoginFileName + suffix);
        std::error_code probe;
        if (std::filesystem::exists(newPath, probe) || probe)
        {
            continue;
        }
        if (!std::filesystem::is_regular_file(oldPath, probe) || probe)
        {
            continue;
        }
        std::error_code mv;
        std::filesystem::rename(oldPath, newPath, mv);
        if (!mv)
        {
            ++moved;
        }
        else
        {
            std::fprintf(stderr, "[login] migrate %s -> %s failed: %s\n", oldPath.c_str(), newPath.c_str(),
                         mv.message().c_str());
        }
    }
    return moved;
}

} // namespace ps2x::loginpaths
