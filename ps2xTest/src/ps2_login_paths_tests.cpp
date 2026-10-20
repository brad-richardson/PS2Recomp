#include "MiniTest.h"
#include "ps2_login_paths.h"
#include "ps2_quickplay_bootstrap.h"

#include <filesystem>
#include <fstream>
#include <string>

// F2 (appsec-fix-1): secrets live in non-shared storage; migration moves
// only the three login files, never cards.
void register_ps2_login_paths_tests()
{
    MiniTest::Case("Ps2LoginPaths", [](TestCase &tc)
                   {
        tc.Run("bootstrap fields are well formed and cannot identify a lobby account", [](TestCase &t) {
            const auto &record = ps2x::kInertQuickplayLogin;
            const char mask[] = "Copyright Electro";
            auto u32 = [](const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
                uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; };
            for (size_t field = 0; field < 2; ++field) {
                uint8_t header[12] = {}, value[17] = {};
                for (size_t i = 0; i < 12; ++i) header[i] = record[field * 29 + i] ^ uint8_t(mask[i]);
                for (size_t i = 0; i < 17; ++i) value[i] = record[field * 29 + 12 + i] ^ uint8_t(mask[i]);
                uint32_t crc = ~0u;
                for (uint8_t byte : value) {
                    crc ^= byte;
                    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
                }
                t.IsTrue(u32(header) == (field ? 0x50415353u : 0x41434354u), "field magic");
                t.IsTrue(u32(header + 4) == 17u, "field length");
                t.IsTrue(u32(header + 8) == ~crc, "field checksum");
                t.IsTrue(value[0] != 0u && value[16] == 0u, "nonempty terminated field");
                if (field == 0) {
                    bool rejected = false;
                    for (uint8_t c : value) {
                        if (!c) break;
                        rejected |= !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                      (c >= '0' && c <= '9') || c == '-' || c == '_');
                    }
                    t.IsTrue(rejected, "lobby name grammar rejects bootstrap identity");
                }
            }
        });

        tc.Run("login path joins the directory", [](TestCase &t)
               {
            t.Equals(ps2x::loginpaths::loginPathForDir("/support"),
                     std::string("/support/quickplay.login"), "joined");
            t.Equals(ps2x::loginpaths::loginPathForDir(""), std::string(), "empty dir empty path"); });

        tc.Run("migration moves the three login files, keeps cards", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            const fs::path base = fs::temp_directory_path() / "ps2x-login-migrate-test";
            std::error_code ec;
            fs::remove_all(base, ec);
            const fs::path oldDir = base / "old";
            const fs::path newDir = base / "new";
            fs::create_directories(oldDir, ec);
            auto write = [&](const fs::path &p, const std::string &body) {
                std::ofstream out(p, std::ios::binary);
                out << body;
            };
            write(oldDir / "quickplay.login", "name:pass\n");
            write(oldDir / "quickplay.login.device-id", "0123456789abcdef0123456789abcdef");
            write(oldDir / "quickplay.login.notice-v1", "");
            write(oldDir / "mc0-fake-card.bin", "card-bytes");
            const size_t moved =
                ps2x::loginpaths::migrateLoginFiles(oldDir.string(), newDir.string());
            t.Equals(moved, static_cast<size_t>(3), "three files moved");
            t.IsTrue(fs::is_regular_file(newDir / "quickplay.login"), "login in new dir");
            t.IsTrue(fs::is_regular_file(newDir / "quickplay.login.device-id"), "device id in new dir");
            t.IsTrue(fs::is_regular_file(newDir / "quickplay.login.notice-v1"), "notice in new dir");
            t.IsTrue(!fs::exists(oldDir / "quickplay.login"), "old login gone");
            t.IsTrue(fs::is_regular_file(oldDir / "mc0-fake-card.bin"), "card untouched in old dir");
            t.IsTrue(!fs::exists(newDir / "mc0-fake-card.bin"), "card not copied");
            fs::remove_all(base, ec); });

        tc.Run("migration never overwrites an existing new-side file", [](TestCase &t)
               {
            namespace fs = std::filesystem;
            const fs::path base = fs::temp_directory_path() / "ps2x-login-nooverwrite-test";
            std::error_code ec;
            fs::remove_all(base, ec);
            const fs::path oldDir = base / "old";
            const fs::path newDir = base / "new";
            fs::create_directories(oldDir, ec);
            fs::create_directories(newDir, ec);
            {
                std::ofstream out(oldDir / "quickplay.login", std::ios::binary);
                out << "old:creds\n";
            }
            {
                std::ofstream out(newDir / "quickplay.login", std::ios::binary);
                out << "new:creds\n";
            }
            const size_t moved =
                ps2x::loginpaths::migrateLoginFiles(oldDir.string(), newDir.string());
            t.Equals(moved, static_cast<size_t>(0), "nothing moved");
            std::ifstream in(newDir / "quickplay.login", std::ios::binary);
            std::string body;
            std::getline(in, body);
            t.Equals(body, std::string("new:creds"), "new-side file wins");
            fs::remove_all(base, ec); }); });}
