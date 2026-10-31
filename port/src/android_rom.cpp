// Android ROM picker.
//
// A phone has no folder beside the program to put the ROM in, so on first start (no ROM in the
// app's storage) the port opens the system's file picker. The chosen file comes back as a
// content:// URI, which SDL reads through Android's content resolver; it is checked against the
// US release's CRC32 and copied into the app's private storage, so it is asked for only once.

#include "android_rom.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

constexpr size_t kRomSize = 0x80000;
constexpr Uint32 kUsCrc32 = 0xBA4E9FD0;             // the US Genesis release (rom/README.md)

struct Choice {
    SDL_AtomicInt done;
    std::string uri;                                // empty if the picker was closed
};

// Called on Android's UI thread when the picker closes.
void SDLCALL on_chosen(void* userdata, const char* const* files, int)
{
    auto* choice = static_cast<Choice*>(userdata);
    if (files && files[0])
        choice->uri = files[0];
    SDL_SetAtomicInt(&choice->done, 1);
}

// The button pressed, by its id (the buttons are numbered from 0 in the order given).
int ask(SDL_MessageBoxFlags flags, const char* message, std::initializer_list<const char*> buttons)
{
    std::vector<SDL_MessageBoxButtonData> data;
    int id = 0;
    for (const char* text : buttons)
        data.push_back({id == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0, id++, text});
    const SDL_MessageBoxData box = {flags, nullptr, "Castle of Illusion", message, (int)data.size(), data.data(), nullptr};
    int pressed = -1;
    SDL_ShowMessageBox(&box, &pressed);
    return pressed;
}

}  // namespace

std::vector<uint8_t> android_pick_rom()
{
    SDL_InitSubSystem(SDL_INIT_VIDEO);              // for the picker's events; main's SDL_Init adds to it
    const char* message =
        "Castle of Illusion needs your own dump of the US Genesis cartridge: a 512 KB .bin file.\n\n"
        "Choose it on the next screen. The app keeps a copy, so you only do this once.";
    for (;;) {
        if (ask(SDL_MESSAGEBOX_INFORMATION, message, {"Choose ROM", "Quit"}) != 0)
            return {};
        Choice choice;
        SDL_SetAtomicInt(&choice.done, 0);
        SDL_ShowOpenFileDialog(on_chosen, &choice, nullptr, nullptr, 0, nullptr, false);
        while (!SDL_GetAtomicInt(&choice.done)) {
            SDL_PumpEvents();
            SDL_Delay(10);
        }
        if (choice.uri.empty())
            continue;

        size_t size = 0;
        void* data = SDL_LoadFile(choice.uri.c_str(), &size);
        std::vector<uint8_t> rom;
        if (data)
            rom.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        SDL_free(data);
        if (rom.size() != kRomSize) {
            message = data ? "That file isn't a 512 KB ROM. Choose your dump of the US Genesis cartridge (a .bin file)."
                           : "That file couldn't be read. Choose your dump of the US Genesis cartridge (a .bin file).";
            continue;
        }
        if (SDL_crc32(0, rom.data(), rom.size()) != kUsCrc32
            && ask(SDL_MESSAGEBOX_WARNING,
                   "That ROM isn't the US Genesis release the port is made for (its CRC32 differs). "
                   "It may not work properly.",
                   {"Choose another", "Use it anyway"}) != 1) {
            message = "Choose your dump of the US Genesis cartridge (a 512 KB .bin file).";
            continue;
        }

        if (const char* internal = SDL_GetAndroidInternalStoragePath()) {
            const std::filesystem::path dir = std::filesystem::path(internal) / "rom";
            std::error_code error;
            std::filesystem::create_directories(dir, error);
            std::ofstream file(dir / "coi.bin", std::ios::binary);
            file.write(reinterpret_cast<const char*>(rom.data()), (std::streamsize)rom.size());
            if (!file)
                SDL_Log("Could not keep a copy of the ROM in %s", dir.string().c_str());
        }
        return rom;
    }
}
