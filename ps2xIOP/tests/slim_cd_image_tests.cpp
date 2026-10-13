#include "ps2x/iop/slim_cd_image.h"

#include <cassert>
#include <chrono>
#include <fstream>
#include <string>

int main()
{
    using namespace ps2x::slim_cd;
    const auto dir = std::filesystem::temp_directory_path() /
        ("ps2x-slim-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    const auto image = dir / "test.iso";
    {
        std::ofstream out(image, std::ios::binary);
        for (uint8_t sector : {uint8_t(0), uint8_t(3), uint8_t(4), uint8_t(5), uint8_t(7)})
        {
            std::string bytes(SectorBytes, char(sector));
            out.write(bytes.data(), std::streamsize(bytes.size()));
        }
    }
    assert(load(image).status == Status::Plain);
    {
        std::ofstream out(image.string() + ".holes", std::ios::binary);
        out.write("PS2XSLM2", 8);
        out.write(PinnedSourceSha, 64);
        const uint8_t metadata[] = {
            8,0,0,0,0,0,0,0, 3,0,0,0,
            1,0,0,0, 2,0,0,0, 1,0,0,0,
            5,0,0,0, 1,0,0,0, 3,0,0,0,
            6,0,0,0, 1,0,0,0, 2,0,0,0};
        out.write(reinterpret_cast<const char *>(metadata), sizeof(metadata));
    }
    const Table table = load(image);
    assert(table.status == Status::Slim && table.logicalSectors == 8);
    assert(!table.moviePrefix(2) && table.moviePrefix(5));
    assert(!table.movieTail(5) && table.movieTail(6));
    const auto pieces = table.pieces(0, 8);
    assert(pieces.size() == 5);
    assert(pieces[0].payloadOffset == 0 && pieces[0].sectors == 1 && !pieces[0].zero);
    assert(pieces[1].zero && pieces[1].sectors == 2);
    assert(pieces[2].payloadOffset == SectorBytes && pieces[2].sectors == 3);
    assert(pieces[3].zero && pieces[3].sectors == 1);
    assert(pieces[4].payloadOffset == 4 * SectorBytes && pieces[4].sectors == 1);
    {
        std::fstream out(image.string() + ".holes", std::ios::in | std::ios::out | std::ios::binary);
        out.seekp(8);
        out.put('0');
    }
    assert(load(image).status == Status::Invalid);
}
