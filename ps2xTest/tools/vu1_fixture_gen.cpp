// VR3: emit the generated VU0 code for the synthetic differential-test images
// (vu1_recomp_fixture.h) with the runtime's own emitter, so the test runs
// exactly what PS2X_VU0_RECOMP_DUMP writes for game images. (VX2: the VU1
// images went with the generated VU1 path.)
//
// Usage: vu1_fixture_gen <out-dir>   (writes <out-dir>/vu0_fixture_<n>.cpp)
//        vu1_fixture_gen --image <vu0 image .bin> <out-dir>
//            (VBK1: re-emits a dumped 4 KiB VU0 code image, e.g. a
//            PS2X_VR3_VU0_IMAGE_DUMP .bin, as <out-dir>/vu0e_<xxh64>.cpp: what
//            PS2X_VU0_RECOMP_DUMP would write for it. Game-derived output:
//            keep it outside the repo.)
#include "runtime/ps2_vu0.h"
#include "vu1_recomp_fixture.h"
#define XXH_NO_XXH32
#define XXH_NO_XXH3
#define XXH_INLINE_ALL
#include "runtime/third_party/xxhash.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    if (argc == 4 && std::string(argv[1]) == "--image")
    {
        std::vector<uint8_t> code(vu1_fixture::kVu0CodeSize, 0u);
        std::ifstream in(argv[2], std::ios::binary);
        in.read(reinterpret_cast<char *>(code.data()), static_cast<std::streamsize>(code.size()));
        if (in.gcount() != static_cast<std::streamsize>(code.size()) || in.peek() != std::ifstream::traits_type::eof())
        {
            std::fprintf(stderr, "vu1_fixture_gen: %s is not a %u-byte VU0 image\n", argv[2], vu1_fixture::kVu0CodeSize);
            return 1;
        }
        const uint64_t hash = XXH64(code.data(), code.size(), 0);
        char name[64];
        std::snprintf(name, sizeof(name), "/vu0e_%016llx.cpp", static_cast<unsigned long long>(hash));
        const std::string path = std::string(argv[3]) + name;
        if (!VU0Interpreter::emitRecompSource(code.data(), vu1_fixture::kVu0CodeSize, hash, path))
        {
            std::fprintf(stderr, "vu1_fixture_gen: cannot write %s\n", path.c_str());
            return 1;
        }
        std::printf("%s\n", path.c_str());
        return 0;
    }
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: vu1_fixture_gen <out-dir> | --image <bin> <out-dir>\n");
        return 2;
    }
    // VR3: the VU0 variants (vu0_fixture_<n>.cpp), emitted by VU0Interpreter (VX1).
    std::vector<uint8_t> vu0Code(vu1_fixture::kVu0CodeSize, 0u);
    for (uint32_t image = 0; image < vu1_fixture::kVu0ImageCount; ++image)
    {
        vu1_fixture::buildVu0Image(image, vu0Code.data());
        const std::string path = std::string(argv[1]) + "/vu0_fixture_" + std::to_string(image) + ".cpp";
        if (!VU0Interpreter::emitRecompSource(vu0Code.data(), vu1_fixture::kVu0CodeSize,
                                              vu1_fixture::kVu0ImageHash[image], path))
        {
            std::fprintf(stderr, "vu1_fixture_gen: cannot write %s\n", path.c_str());
            return 1;
        }
    }
    return 0;
}
