#include "MiniTest.h"
#include "ps2_env_file.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// I25: ps2x.env parser, ${VAR} expansion and layer merge (the iOS loader's
// pure parts; the file reads + setenv live in ps2_ios_runtime.cpp).
void register_ps2_env_file_tests()
{
    MiniTest::Case("Ps2EnvFile", [](TestCase &tc)
                   {
        tc.Run("parses KEY=VALUE, comments, CRLF, first '=' split, empty value", [](TestCase &t)
               {
            const auto e = ps2x::parseEnvFileContent(
                "# comment\r\n  A = 1 \r\n\nB=x=y\nnoequals\n=novalue\nC=\n  # indented comment\nD=last");
            t.Equals(e.size(), static_cast<size_t>(4), "4 entries");
            t.IsTrue(e[0] == std::make_pair(std::string("A"), std::string("1")), "trimmed A=1");
            t.IsTrue(e[1] == std::make_pair(std::string("B"), std::string("x=y")), "split at first '='");
            t.IsTrue(e[2] == std::make_pair(std::string("C"), std::string()), "empty value kept");
            t.IsTrue(e[3] == std::make_pair(std::string("D"), std::string("last")), "no trailing newline"); });

        tc.Run("expands known ${VAR}, leaves unknown and unterminated as written", [](TestCase &t)
               {
            const std::map<std::string, std::string> vars{{"BUNDLE", "/b/app"}, {"DOCUMENTS", "/h/Documents"}};
            t.Equals(ps2x::expandEnvPlaceholders("${BUNDLE}/SSX3.iso", vars), std::string("/b/app/SSX3.iso"), "BUNDLE");
            t.Equals(ps2x::expandEnvPlaceholders("${DOCUMENTS}/mc0:${BUNDLE}", vars), std::string("/h/Documents/mc0:/b/app"), "two");
            t.Equals(ps2x::expandEnvPlaceholders("${NOPE}/x", vars), std::string("${NOPE}/x"), "unknown kept");
            t.Equals(ps2x::expandEnvPlaceholders("a${BUNDLE", vars), std::string("a${BUNDLE"), "unterminated kept");
            t.Equals(ps2x::expandEnvPlaceholders("10350:start:2500", vars), std::string("10350:start:2500"), "plain"); });

        tc.Run("later layers override, launcher keys win, empty clears", [](TestCase &t)
               {
            const std::vector<std::vector<std::pair<std::string, std::string>>> layers{
                {{"PS2X_CD_IMAGE", "${BUNDLE}/SSX3.iso"}, {"PS2X_PAD_SCRIPT", "1:start:1"}, {"PS2X_SKIP_MOVIE", "1"}},
                {{"PS2X_PAD_SCRIPT", ""}, {"PS2X_SKIP_MOVIE", "0"}}};
            const auto m = ps2x::mergeEnvLayers(layers, {"PS2X_SKIP_MOVIE"}, {{"BUNDLE", "/b"}});
            t.Equals(m.size(), static_cast<size_t>(2), "launcher key dropped");
            t.Equals(m.at("PS2X_CD_IMAGE"), std::string("/b/SSX3.iso"), "expanded");
            t.Equals(m.at("PS2X_PAD_SCRIPT"), std::string(), "Documents layer clears the script");
            t.IsTrue(m.count("PS2X_SKIP_MOVIE") == 0, "launcher value untouched"); });

        tc.Run("fnv1a64Hex matches the FNV-1a 64 vectors", [](TestCase &t)
               {
            t.Equals(ps2x::fnv1a64Hex(""), std::string("cbf29ce484222325"), "offset basis");
            t.Equals(ps2x::fnv1a64Hex("a"), std::string("af63dc4c8601ec8c"), "single byte");
            t.Equals(ps2x::fnv1a64Hex("PS2X_DETERMINISTIC=1\n"), ps2x::fnv1a64Hex("PS2X_DETERMINISTIC=1\n"),
                     "deterministic");
            t.IsTrue(ps2x::fnv1a64Hex("x") != ps2x::fnv1a64Hex("y"), "distinct inputs differ"); });

        tc.Run("recorded env hash defaults to none, round-trips", [](TestCase &t)
               {
            t.IsTrue(std::string(ps2x::recordedEnvFileHash()) == "none", "nothing loaded reads none");
            ps2x::setRecordedEnvFileHash("0123456789abcdef");
            t.IsTrue(std::string(ps2x::recordedEnvFileHash()) == "0123456789abcdef", "stash reads back");
            ps2x::setRecordedEnvFileHash("");
            t.IsTrue(std::string(ps2x::recordedEnvFileHash()) == "none", "reset reads none"); }); });
}
