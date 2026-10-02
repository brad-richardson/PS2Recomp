// VR3: emit the generated VU0 code for the synthetic differential-test images
// (vu1_recomp_fixture.h) with the runtime's own emitter, so the test runs
// exactly what PS2X_VU0_RECOMP_DUMP writes for game images. (VX2: the VU1
// images went with the generated VU1 path.)
//
// Usage: vu1_fixture_gen <out-dir>   (writes <out-dir>/vu0_fixture_<n>.cpp)
#include "runtime/ps2_vu0.h"
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
