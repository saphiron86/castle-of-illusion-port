// MD+ soundtrack packs (Enhancements > Audio > MD+ Soundtrack). See mdplus.cpp.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class MdPlus {
public:
    ~MdPlus() { stop(); }

    // Reads a pack's .cue sheet (with the patched ROM and the tracks beside it). False, with
    // a message, if it can't be used.
    bool load(const std::filesystem::path& cue, std::string& error);
    bool loaded() const { return !tracks.empty(); }
    const std::string& name() const { return pack_name; }
    const std::filesystem::path& rom_path() const { return rom; }

    // The game's writes to the MD+ registers ($03F7FA, $03F7FE).
    void write(uint32_t address, uint16_t value);

    // Stops the music (a reset, or MD+ switched off).
    void stop();

    // Keeps the music stream supplied; call every frame. The stream takes 44.1 kHz 16-bit
    // stereo, as the tracks are.
    void feed(SDL_AudioStream* stream);

    // Optional: told of each command (testing).
    std::function<void(uint16_t command)> on_command;

private:
    struct Track {
        std::filesystem::path file;
        bool loop = false;
        uint32_t loop_frame = 0;    // where the loop starts, in sample frames
    };
    void play(int number, bool loop);

    std::vector<Track> tracks;      // track n at index n - 1
    std::string pack_name;
    std::filesystem::path rom;
    bool registers_open = false;    // $CD54 written to $03F7FA

    // The playing track.
    SDL_IOStream* file = nullptr;
    int track = -1;
    bool looping = false;
    uint64_t data_start = 0, data_frames = 0, position = 0;
    bool paused = false;
    int fade_frames = 0, fade_left = 0;     // fading out before a pause, in sample frames
};
