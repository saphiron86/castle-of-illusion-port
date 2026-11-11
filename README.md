# Castle of Illusion: PC port

> **AI disclosure:** This project was made with extensive use of AI. Most of the code, tools and documentation were written by Claude, Anthropic's AI coding assistant, under my direction: I chose the features, play-tested every change and decided what went in. Expect the kinds of mistakes AI-written code can contain, and please report any you find in [Issues](https://github.com/saphiron86/castle-of-illusion-port/issues).

An unofficial PC (Windows, Linux and Steam Deck) and Android port of the US Sega Genesis release of *Castle of Illusion Starring Mickey Mouse* (1990), with widescreen, smoother motion and modern display options.

**No game data is included.** This repository and the release downloads contain no ROM, graphics, sound, level data or game code. The port runs the game from your own dump of the cartridge.

**Documentation:** every setting, Boss Rush, MD+ packs, building from source and the technical notes are on the [project site](https://saphiron86.github.io/castle-of-illusion-port/).

## Features

- **Widescreen:** 400 x 224 (16:9) in every stage and the story scenes, switchable at any time, with the camera, object ranges and sprite clipping adjusted to the wider view.
- **Smoother motion:** Smooth Scrolling (sub-pixel scrolling), Smooth Swinging (ropes and vines at 60 frames a second), Smooth Fades, and High Refresh Interpolation for 120 and 240 Hz displays.
- **Cleaner pictures:** Smooth Water and Smooth Glass (see-through water and jars instead of dithering), Clean Rock Edges and Remove Sprite Limit.
- **Display options:** HQ2x-HQ4x and cleanEdge upscalers, scanlines, and a CRT shader with adjustable mask, glow and curvature.
- **Exact gameplay:** the game's own code runs, translated to native code as it starts, frame for frame as on the console. Every enhancement can be switched off (F3 gives the original game).
- **Boss Rush:** the six boss fights back to back, timed to 1/100 s, from the game's options screen.
- **MD+ soundtrack packs:** play a fan-made CD-quality soundtrack in place of the game's music.
- **Cheats:** invincibility, infinite power, tries, items and continues, faster walking, moon jump and more.
- **Controllers and touch:** keyboard, any SDL-supported controller, the Steam Deck's controls, and on-screen controls on Android.
- **Story fast forward:** hold Backspace (Y on a controller) to run story scenes at four times their speed.

## How to play

You need your own dump of the US release: a 524,288-byte (512 KB) `.bin` file, SHA-1 `4ac3687634a5acc55ac7f156c6de9749158713e4`. Download the latest release from the [Releases](https://github.com/saphiron86/castle-of-illusion-port/releases) page.

### Windows

1. Unzip `coi-port-windows.zip` anywhere.
2. Put your ROM in the `rom` folder beside `coi.exe`.
3. Start `coi.exe`.

### Linux and Steam Deck

64-bit Linux with glibc 2.29 or later (SteamOS, Bazzite, Ubuntu 20.04 and newer, and most other current distributions) and OpenGL 3.3.

1. Unpack `coi-port-linux.tar.gz` anywhere (on a Steam Deck, in Desktop Mode).
2. Put your ROM in the `rom` folder beside `coi`.
3. Start `coi`. It starts fullscreen; F11 switches to a window.

On a Steam Deck, add `coi` to Steam with Games > Add a Non-Steam Game to My Library... in Desktop Mode to play it in Game Mode.

### Android

Android 7.0 or later, on a 64-bit phone with OpenGL ES 3.0.

1. Download `coi-port-android.apk` on the phone and open it (Android asks you to allow installs from your browser or file manager first).
2. On first start, pick your ROM with the file picker. It is copied into the app, so it is asked for only once.
3. Play with the on-screen controls or a USB or Bluetooth controller. The menu button, or Back, opens the menu.

### Controls

| Genesis | Keyboard | Controller (Xbox layout) |
|---------|----------|--------------------------|
| D-pad   | arrows   | D-pad or left stick      |
| A       | Z        | X                        |
| B       | X        | A                        |
| C       | C        | B                        |
| Start   | Enter    | Menu                     |
| Fast forward (story scenes) | Backspace (hold) | Y (hold) |
| Port menu | F1     | Back (View)              |

The menu holds every setting; F2 picks the Enhanced settings (the default), F3 the original game (Vanilla) and F4 your own choices. Settings are remembered between runs. See [Settings](https://saphiron86.github.io/castle-of-illusion-port/settings.html) for what each one does.

## Legal

This is a fan project. It is not affiliated with, endorsed or sponsored by Disney or Sega. *Castle of Illusion*, Mickey Mouse, Sega and Genesis are trademarks of their respective owners, used here only to name the game this port runs. You must supply your own copy of the game; please do not ask for, or share, ROMs.

The project's own code is under the MIT License ([LICENSE](LICENSE)). Third-party libraries and their licences are listed in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) and on the [Credits](https://saphiron86.github.io/castle-of-illusion-port/credits.html) page.
