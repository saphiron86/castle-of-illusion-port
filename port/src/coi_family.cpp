// coi_family ROM DIR: the image family the readable C is generated from (rom_family.h), made from
// your ROM by the build (port/CMakeLists.txt, COI_READABLE) before coi_recomp runs. No SDL, no
// emulation: only the port's ROM patches.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "rom_family.h"

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: coi_family ROM DIR\n");
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<uint8_t> rom((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (rom.size() != 0x80000) {
        std::fprintf(stderr, "coi_family: %s is not a 512 KB ROM\n", argv[1]);
        return 1;
    }
    std::error_code error;
    std::filesystem::create_directories(argv[2], error);
    if (!save_rom_family(rom, argv[2])) {
        std::fprintf(stderr, "coi_family: couldn't write the images to %s\n", argv[2]);
        return 1;
    }
    std::printf("coi_family: base.bin and %zu setting images in %s\n", kRomSettingCount, argv[2]);
    return 0;
}
