---
title: MD+ soundtrack packs
nav_order: 6
---

# MD+ soundtrack packs

MD+ is the CD-audio feature of the MegaSD and Mega EverDrive Pro cartridges: fan-made packs pair a patched ROM with a new soundtrack, which the cartridge plays from WAV files when the game asks. The port does the cartridge's part. A pack is not included; like the ROM, you supply it.

The pack's ROM must be this game with the pack's changes (it is checked against yours), and the port's own patches and cheats still apply on top. The game's sound effects play as usual; its music is replaced. **Enhancements > Audio > MD+ Soundtrack** switches between the pack and the game's own music.

## Windows, Linux and Steam Deck

Extract the pack into a folder named `mdplus` beside `coi.exe` (or `coi`). It holds a `.cue` sheet, the patched `.md` ROM and one WAV per track.

## Android

Copy the pack's folder to the phone, then in the menu choose **Enhancements > Audio > Import MD+ Pack...** and pick that folder (the one with the `.cue`, `.md` and WAV files, or a folder with the pack in it). Choose it from the phone's storage, where the picker opens: Android's Downloads and Recent views list only media files, so the pack's `.cue` and `.md` don't show there. The pack is copied into the app's storage, replacing any pack there, and its soundtrack is switched on. It stays after updates.

## Technical

Smooth Swinging's data lives at `$0C0000`, clear of the packs' code at `$080000`. When building from source, `mdplus/` and `*.7z` are ignored by git.
