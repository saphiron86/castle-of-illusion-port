// MD+ soundtrack packs.
//
// MD+ is the CD-audio feature of the MegaSD and Mega EverDrive Pro cartridges: a patched ROM
// asks the cartridge to play tracks from a .cue sheet instead of the game's own music. A pack
// is that patched ROM (.md), the .cue sheet and the tracks as WAV files; like the game's ROM,
// it is never included, you supply it.
//
// The patch talks to the cartridge through two words in ROM space: writing $CD54 to $03F7FA
// opens the MD+ registers, a command goes to $03F7FE, and $0000 to $03F7FA closes them again.
// Commands (in Castle of Illusion's packs): $11xx play track xx once, $12xx play it looping,
// $13xx pause with a fade of xx (in 1/75 s, CD frames; $00 at once), $1400 resume.
//
// The .cue sheet lists one WAV per track; REM LOOP n gives the loop start in CD sectors
// (2352 bytes, 588 sample frames), REM NOLOOP a track that plays once. Tracks are streamed
// from disk as they play.

#include "mdplus.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

constexpr int kRate = 44100;
constexpr int kFramesPerSector = 588;

}  // namespace

bool MdPlus::load(const std::filesystem::path& cue, std::string& error)
{
    tracks.clear();
    std::ifstream in(cue);
    if (!in) {
        error = "can't read " + cue.string();
        return false;
    }
    const std::filesystem::path dir = cue.parent_path();
    int current = -1;
    for (std::string line; std::getline(in, line);) {
        std::istringstream words(line);
        std::string word;
        words >> word;
        if (word == "FILE") {
            const size_t first = line.find('"'), last = line.rfind('"');
            if (first == std::string::npos || last <= first)
                continue;
            tracks.push_back({dir / line.substr(first + 1, last - first - 1)});
            current = (int)tracks.size() - 1;
        } else if (word == "REM" && current >= 0) {
            std::string what;
            words >> what;
            if (what == "LOOP") {
                uint32_t sectors = 0;
                words >> sectors;
                tracks[current].loop = true;
                tracks[current].loop_frame = sectors * kFramesPerSector;
            } else if (what == "NOLOOP") {
                tracks[current].loop = false;
            }
        }
    }
    if (tracks.empty()) {
        error = "no tracks in " + cue.string();
        return false;
    }
    for (const Track& t : tracks)
        if (!std::filesystem::exists(t.file)) {
            error = "missing track " + t.file.filename().string();
            tracks.clear();
            return false;
        }
    rom = cue;
    rom.replace_extension(".md");
    if (!std::filesystem::exists(rom)) {
        error = "missing the patched ROM " + rom.filename().string();
        tracks.clear();
        return false;
    }
    pack_name = cue.stem().string();
    return true;
}

void MdPlus::write(uint32_t address, uint16_t value)
{
    if (address == 0x03F7FA) {
        registers_open = value == 0xCD54;
        return;
    }
    if (address != 0x03F7FE || !registers_open)
        return;
    if (on_command)
        on_command(value);
    switch (value >> 8) {
    case 0x11: play(value & 0xFF, false); break;
    case 0x12: play(value & 0xFF, true); break;
    case 0x13:                                      // pause, fading out over xx/75 s
        if (file && !paused) {
            fade_frames = fade_left = (value & 0xFF) * kRate / 75;
            if (fade_frames == 0)
                paused = true;
        }
        break;
    case 0x14:                                      // resume
        if (file) {
            paused = false;
            fade_frames = fade_left = 0;
        }
        break;
    default:
        break;
    }
}

void MdPlus::play(int number, bool loop)
{
    stop();
    if (number < 1 || number > (int)tracks.size())
        return;
    const Track& t = tracks[number - 1];
    file = SDL_IOFromFile(t.file.string().c_str(), "rb");
    if (!file)
        return;
    // Find the data chunk (16-bit stereo PCM at 44.1 kHz, as the packs are).
    uint8_t header[12];
    if (SDL_ReadIO(file, header, 12) != 12 || std::memcmp(header, "RIFF", 4) || std::memcmp(header + 8, "WAVE", 4)) {
        stop();
        return;
    }
    for (;;) {
        uint8_t chunk[8];
        if (SDL_ReadIO(file, chunk, 8) != 8) {
            stop();
            return;
        }
        const uint32_t size = chunk[4] | chunk[5] << 8 | chunk[6] << 16 | (uint32_t)chunk[7] << 24;
        if (!std::memcmp(chunk, "data", 4)) {
            data_start = SDL_TellIO(file);
            data_frames = size / 4;
            break;
        }
        SDL_SeekIO(file, size + (size & 1), SDL_IO_SEEK_CUR);
    }
    track = number;
    looping = loop && t.loop;
    position = 0;
    paused = false;
    fade_frames = fade_left = 0;
}

void MdPlus::stop()
{
    if (file)
        SDL_CloseIO(file);
    file = nullptr;
    track = -1;
    paused = false;
}

void MdPlus::feed(SDL_AudioStream* stream)
{
    if (!stream || !file || paused)
        return;
    const int wanted = kRate / 4 * 4;               // keep a quarter of a second queued
    int16_t buffer[2048 * 2];
    while (file && !paused && SDL_GetAudioStreamQueued(stream) < wanted) {
        if (position >= data_frames) {
            if (!looping) {
                stop();
                return;
            }
            position = std::min<uint64_t>(tracks[track - 1].loop_frame, data_frames ? data_frames - 1 : 0);
        }
        const uint64_t count = std::min<uint64_t>(2048, data_frames - position);
        SDL_SeekIO(file, data_start + position * 4, SDL_IO_SEEK_SET);
        const size_t got = SDL_ReadIO(file, buffer, count * 4) / 4;
        if (got == 0) {
            stop();
            return;
        }
        if (fade_frames > 0) {                      // fading out towards a pause
            for (size_t i = 0; i < got; i++, fade_left = fade_left > 0 ? fade_left - 1 : 0)
                for (int c = 0; c < 2; c++)
                    buffer[i * 2 + c] = (int16_t)(buffer[i * 2 + c] * fade_left / fade_frames);   // silent once faded
            if (fade_left <= 0) {
                paused = true;
                fade_frames = 0;
            }
        }
        SDL_PutAudioStreamData(stream, buffer, (int)(got * 4));
        position += got;
    }
}
