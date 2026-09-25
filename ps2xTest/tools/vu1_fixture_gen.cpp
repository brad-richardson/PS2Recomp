// VR2: emit the generated VU1 code for the synthetic differential-test images
// (vu1_recomp_fixture.h) with the runtime's own emitter, so the test runs
// exactly what PS2X_VU1_RECOMP_DUMP writes for game images.
//
// Usage: vu1_fixture_gen <out-dir>   (writes <out-dir>/vu1_fixture_<n>.cpp)
#include "runtime/ps2_vu1.h"
#include "vu1_recomp_fixture.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: vu1_fixture_gen <out-dir>\n");
        return 2;
    }
    std::vector<uint8_t> code(vu1_fixture::kCodeSize, 0u);
    for (uint32_t image = 0; image < vu1_fixture::kImageCount; ++image)
    {
        vu1_fixture::buildImage(image, code.data());
        const std::string path = std::string(argv[1]) + "/vu1_fixture_" + std::to_string(image) + ".cpp";
        if (!VU1Interpreter::emitRecompSource(code.data(), vu1_fixture::kCodeSize,
                                              vu1_fixture::kImageHash[image], path))
        {
            std::fprintf(stderr, "vu1_fixture_gen: cannot write %s\n", path.c_str());
            return 1;
        }
    }
    return 0;
}
