// vu0dump (VX2; was GV2's vu1dump, whose VU1 capture mode went with the
// generated VU1 path): emit the generated source for one VU0 code image.
//
//   vu0dump <code.bin> <outdir>   (one 4 KiB VU0 code image ->
//                                  <outdir>/vu0e_<xxh64>.cpp)
//
// No game boot needed. Links the VU TUs (core/upper/lower/recomp) with the
// runner's Release flags; see build-mac.sh and README.md.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu0.h"
#include "runtime/ps2_vu1.h"

#define XXH_INLINE_ALL
#include "runtime/third_party/xxhash.h"

// ---- minimal runtime stubs (the VU TUs reference these; never reached) ----
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
    std::fprintf(stderr, "[vu0dump] fatal: submitGifPacket reached\n");
    std::abort();
}
void GS::processGIFPacket(const uint8_t *, uint32_t)
{
    std::fprintf(stderr, "[vu0dump] fatal: processGIFPacket reached\n");
    std::abort();
}

static int dumpVu0(const char *binPath, const char *outDir)
{
    std::vector<uint8_t> code(PS2_VU0_CODE_SIZE, 0u);
    FILE *f = std::fopen(binPath, "rb");
    if (!f)
    {
        std::fprintf(stderr, "[vu0dump] open %s failed\n", binPath);
        return 1;
    }
    const size_t got = std::fread(code.data(), 1, code.size(), f);
    const bool extra = std::fgetc(f) != EOF;
    std::fclose(f);
    if (got != code.size() || extra)
    {
        std::fprintf(stderr, "[vu0dump] %s is not a %u-byte VU0 code image\n", binPath, PS2_VU0_CODE_SIZE);
        return 1;
    }
    const uint64_t xxh = XXH64(code.data(), code.size(), 0);
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/vu0e_%016llx.cpp", outDir, (unsigned long long)xxh);
    const bool ok = VU0Interpreter::emitRecompSource(code.data(), (uint32_t)code.size(), xxh, path);
    std::printf("%016llx %s %s\n", (unsigned long long)xxh, path, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    // "--vu0" is accepted for the old vu1dump spelling.
    if (argc == 4 && std::strcmp(argv[1], "--vu0") == 0)
        return dumpVu0(argv[2], argv[3]);
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: vu0dump <code.bin> <outdir>\n");
        return 2;
    }
    return dumpVu0(argv[1], argv[2]);
}
