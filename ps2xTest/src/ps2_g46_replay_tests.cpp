// G46 DIAGNOSTIC: offline replay of a PS2X_GS_SHADOW_REC stream through the
// CPU GS backend. Runs only with PS2X_G46_REC=<rec file>; writes the latched
// presentation frame (PPM) for each tick in PS2X_G46_TICKS (comma list) to
// PS2X_G46_OUT (default "."). Unset = trivial pass.
#include "MiniTest.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace
{

bool writePpm(const std::string &path, const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3u);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
    {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    return true;
}

} // namespace

void register_ps2_g46_replay_tests()
{
    MiniTest::Case("G46Replay", [](TestCase &tc)
    {
        tc.Run("replay PS2X_G46_REC through the CPU backend", [](TestCase &t)
        {
            const char *recPath = std::getenv("PS2X_G46_REC");
            if (!recPath || !recPath[0])
            {
                t.IsTrue(true, "PS2X_G46_REC unset: replay not run");
                return;
            }
            std::set<uint64_t> ticks;
            if (const char *tl = std::getenv("PS2X_G46_TICKS"))
            {
                std::string s(tl);
                size_t p = 0;
                while (p < s.size())
                {
                    size_t q = s.find(',', p);
                    if (q == std::string::npos)
                        q = s.size();
                    ticks.insert(std::strtoull(s.substr(p, q - p).c_str(), nullptr, 10));
                    p = q + 1;
                }
            }
            const char *outEnv = std::getenv("PS2X_G46_OUT");
            const std::string outDir = (outEnv && outEnv[0]) ? outEnv : ".";
            const uint64_t lastTick = ticks.empty() ? ~0ull : *ticks.rbegin();

            FILE *f = std::fopen(recPath, "rb");
            if (!f)
            {
                t.IsTrue(false, "cannot open PS2X_G46_REC");
                return;
            }
            char magic[8];
            if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "G46REC1", 8) != 0)
            {
                std::fclose(f);
                t.IsTrue(false, "bad G46REC1 magic");
                return;
            }
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GSRegisters regs{};
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), &regs);
            std::vector<uint8_t> pkt;
            uint64_t nG = 0, nR = 0, nV = 0, wrote = 0;
            for (;;)
            {
                int type = std::fgetc(f);
                if (type == EOF)
                    break;
                if (type == 'G')
                {
                    uint8_t path = 0;
                    uint32_t size = 0;
                    if (std::fread(&path, 1, 1, f) != 1 || std::fread(&size, 4, 1, f) != 1)
                        break;
                    pkt.resize(size);
                    if (std::fread(pkt.data(), 1, size, f) != size)
                        break;
                    gs.noteGifPath(static_cast<GifPathId>(path));
                    gs.processGIFPacket(pkt.data(), size);
                    ++nG;
                }
                else if (type == 'R')
                {
                    uint8_t addr = 0;
                    uint64_t value = 0;
                    if (std::fread(&addr, 1, 1, f) != 1 || std::fread(&value, 8, 1, f) != 1)
                        break;
                    gs.writeRegister(addr, value);
                    ++nR;
                }
                else if (type == 'V')
                {
                    uint64_t v[16];
                    if (std::fread(v, 8, 16, f) != 16)
                        break;
                    ++nV;
                    const uint64_t tick = v[0];
                    if (tick > lastTick)
                        break;
                    if (ticks.count(tick) == 0u)
                        continue;
                    regs.pmode = v[1];
                    regs.smode1 = v[2];
                    regs.smode2 = v[3];
                    regs.srfsh = v[4];
                    regs.synch1 = v[5];
                    regs.synch2 = v[6];
                    regs.syncv = v[7];
                    regs.dispfb1 = v[8];
                    regs.display1 = v[9];
                    regs.dispfb2 = v[10];
                    regs.display2 = v[11];
                    regs.extbuf = v[12];
                    regs.extdata = v[13];
                    regs.extwrite = v[14];
                    regs.bgcolor = v[15];
                    gs.latchHostPresentationFrame();
                    std::vector<uint8_t> px;
                    uint32_t w = 0, h = 0;
                    if (gs.copyLatchedHostPresentationFrame(px, w, h) && w && h)
                    {
                        char name[64];
                        std::snprintf(name, sizeof(name), "/rep-%llu.ppm", static_cast<unsigned long long>(tick));
                        if (writePpm(outDir + name, px, w, h))
                            ++wrote;
                    }
                }
                else
                {
                    std::fclose(f);
                    t.IsTrue(false, "bad record type");
                    return;
                }
            }
            std::fclose(f);
            std::printf("[g46-replay] G=%llu R=%llu V=%llu frames=%llu\n",
                        static_cast<unsigned long long>(nG), static_cast<unsigned long long>(nR),
                        static_cast<unsigned long long>(nV), static_cast<unsigned long long>(wrote));
            t.IsTrue(nG > 0u, "replayed packets");
        });
    });
}
