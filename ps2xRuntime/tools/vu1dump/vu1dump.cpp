// GV2 vu1dump: emit VU1 generated sources from a vu1bench capture's code bytes.
//
//   vu1dump <vu1cap.bin> <outdir>
//
// Regenerates <outdir>/vu1_<xxh64>.cpp for every code image in the capture with
// the linked emitter — no game boot needed. A capture covers only the images its
// window ran; images outside it still need a PS2X_VU1_RECOMP_DUMP boot.
// Links the same VU1 TUs as vu1bench (core/upper/lower/recomp) with the same
// Release flags; see build-mac.sh and README.md.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ps2_vu1cap.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#define XXH_INLINE_ALL
#include "runtime/third_party/xxhash.h"

// ---- minimal runtime stubs (copied shape from vu1bench.cpp) ----
PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false),
      m_gsVRAM(nullptr)
{
    m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE]();
}
PS2Memory::~PS2Memory()
{
    delete[] m_vu1Code;
    m_vu1Code = nullptr;
}
GS::GS() = default;
GS::~GS() = default;
GsWorker::~GsWorker() {}
void PS2Memory::submitGifPacket(GifPathId, const uint8_t *, uint32_t, bool, bool)
{
    std::fprintf(stderr, "[vu1dump] fatal: submitGifPacket reached\n");
    std::abort();
}
void GS::processGIFPacket(const uint8_t *, uint32_t)
{
    std::fprintf(stderr, "[vu1dump] fatal: processGIFPacket reached\n");
    std::abort();
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: vu1dump <vu1cap.bin> <outdir>\n");
        return 2;
    }
    ps2_vu1cap::Reader reader;
    std::string err;
    if (!reader.open(argv[1], err))
    {
        std::fprintf(stderr, "[vu1dump] open: %s\n", err.c_str());
        return 1;
    }
    auto &images = reader.images();
    for (size_t i = 0; i < images.size(); ++i)
    {
        const uint64_t xxh = XXH64(images[i].code.data(), images[i].code.size(), 0);
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/vu1_%016llx.cpp", argv[2],
                      (unsigned long long)xxh);
        const bool ok = VU1Interpreter::emitRecompSource(
            images[i].code.data(), (uint32_t)images[i].code.size(), xxh, path,
            VU1Interpreter::Unit::VU1);
        std::printf("%016llx %s %s\n", (unsigned long long)xxh, path,
                    ok ? "ok" : "FAILED");
        if (!ok)
            return 1;
    }
    return 0;
}
