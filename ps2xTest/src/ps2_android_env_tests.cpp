#include "MiniTest.h"
#include "ps2_android_env.h"

#include <string>
#include <utility>
#include <vector>

void register_ps2_android_env_tests()
{
    MiniTest::Case("Ps2AndroidEnv", [](TestCase &tc)
                   {
        tc.Run("env file parser accepts keys, skips comments and blanks", [](TestCase &t)
               {
            const std::vector<std::pair<std::string, std::string>> entries = ps2x::parseEnvFileContent(
                "# ps2x env file\n"
                "\n"
                "PS2X_CD_IMAGE=/storage/emulated/0/Android/data/com.ps2x.runner/files/SSX3.iso\n"
                "   # indented comment\n"
                "PS2X_SKIP_MOVIE=1\n"
                "PS2X_FRAME_DUMP_DIR = /tmp/frames \n");
            t.Equals(entries.size(), static_cast<size_t>(3), "three entries parsed");
            t.Equals(entries[0].first, std::string("PS2X_CD_IMAGE"), "entry 0 key");
            t.Equals(entries[0].second,
                     std::string("/storage/emulated/0/Android/data/com.ps2x.runner/files/SSX3.iso"),
                     "entry 0 value");
            t.Equals(entries[1].first, std::string("PS2X_SKIP_MOVIE"), "entry 1 key");
            t.Equals(entries[1].second, std::string("1"), "entry 1 value");
            t.Equals(entries[2].first, std::string("PS2X_FRAME_DUMP_DIR"), "entry 2 key");
            t.Equals(entries[2].second, std::string("/tmp/frames"), "entry 2 value trimmed");
        });

        tc.Run("env file parser splits at the first equals and ignores malformed lines", [](TestCase &t)
               {
            const std::vector<std::pair<std::string, std::string>> entries = ps2x::parseEnvFileContent(
                "A=B=C\n"
                "NOEQUALS\n"
                "=nokey\n"
                "   \n"
                "#comment\n"
                "EMPTY=\n");
            t.Equals(entries.size(), static_cast<size_t>(2), "two entries parsed");
            t.Equals(entries[0].first, std::string("A"), "first-equals key");
            t.Equals(entries[0].second, std::string("B=C"), "first-equals value keeps rest");
            t.Equals(entries[1].first, std::string("EMPTY"), "empty value key");
            t.Equals(entries[1].second, std::string(""), "empty value parses");

            t.IsTrue(ps2x::parseEnvFileContent("").empty(), "empty content parses to nothing");
            t.IsTrue(ps2x::parseEnvFileContent("# only\n# comments\n").empty(), "comments only parse to nothing");
        });

        tc.Run("env file parser handles CRLF line endings", [](TestCase &t)
               {
            const std::vector<std::pair<std::string, std::string>> entries =
                ps2x::parseEnvFileContent("PS2X_SKIP_MOVIE=1\r\nPS2X_CD_IMAGE=x.iso\r\n");
            t.Equals(entries.size(), static_cast<size_t>(2), "two CRLF entries parsed");
            t.Equals(entries[0].second, std::string("1"), "CRLF value has no carriage return");
            t.Equals(entries[1].second, std::string("x.iso"), "second CRLF value");
        });

        tc.Run("env file path follows the boot ELF directory", [](TestCase &t)
               {
            t.Equals(ps2x::envFilePathForBootElf("/storage/emulated/0/Android/data/com.ps2x.runner/files/SLUS_207.72"),
                     std::string("/storage/emulated/0/Android/data/com.ps2x.runner/files/ps2x.env"),
                     "files dir plus ps2x.env");
            t.Equals(ps2x::envFilePathForBootElf("SLUS_207.72"),
                     std::string("ps2x.env"),
                     "bare name falls back to ps2x.env");
            t.Equals(ps2x::envFilePathForBootElf(nullptr), std::string(""), "null yields empty");
            t.Equals(ps2x::envFilePathForBootElf(""), std::string(""), "empty yields empty");
        });
    });
}
