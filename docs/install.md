---
title: Installing
nav_order: 2
---

# Installing

Every platform needs your own dump of the US release of the game. It is not included and can't be downloaded here.

| Hash  | Value                                      |
|-------|--------------------------------------------|
| Size  | 524,288 bytes (512 KB), any name ending in `.bin` |
| SHA-1 | `4ac3687634a5acc55ac7f156c6de9749158713e4` |
| MD5   | `24863151abd5baf28c9386d49ffe3c09`         |
| CRC32 | `BA4E9FD0`                                 |

The downloads are on the [Releases](https://github.com/saphiron86/castle-of-illusion-port/releases) page.

## Windows

1. Download `coi-port-windows.zip` and unzip it anywhere.
2. Put your ROM in the `rom` folder beside `coi.exe`.
3. Start `coi.exe`. Press F1 for the menu, F2/F3/F4 for the Quick Settings.

The window is drawn with OpenGL 3.3, which any graphics driver from the last decade provides. Settings are saved in `%APPDATA%\CastleOfIllusion\Port\settings.ini`.

## Linux

64-bit Linux with glibc 2.29 or later (SteamOS, Bazzite, Ubuntu 20.04 and newer, and most other current distributions) and OpenGL 3.3.

1. Download `coi-port-linux.tar.gz` and unpack it anywhere, for example in your home folder.
2. Put your ROM in the `rom` folder beside `coi`.
3. Start `coi`. It starts fullscreen; F11 switches to a window. Press F1, or a controller's Back/View button, for the menu.

Settings are saved in `~/.local/share/CastleOfIllusion/Port/settings.ini`.

## Steam Deck

Install it as for Linux, in Desktop Mode. To play in Game Mode, add it to Steam: in Desktop Mode open Steam, choose Games > Add a Non-Steam Game to My Library..., browse to `coi` (set the file type to All files if it isn't listed) and add it. Back in Game Mode it is in the library's Non-Steam section. The Deck's controls work as a controller, and the View button opens the menu.

## Android

Android 7.0 or later, on a 64-bit phone with OpenGL ES 3.0.

1. Download `coi-port-android.apk` on the phone and open it. Android asks you to allow installs from your browser or file manager first.
2. On first start the app asks for the ROM: pick your dump with the file picker. It is copied into the app's storage, so it is asked for only once.
3. Play with the on-screen controls or a USB or Bluetooth controller (a controller hides the touch controls until the screen is touched again). Tap the menu button, or press Back, for the menu. During the story scenes a fast forward button (>>) shows under A, B and C: hold it to run the scene at four times its speed (Y on a controller does the same).

Updates install over the previous version and keep your ROM, settings and MD+ pack.

A phone's GPU can't do everything a PC's can, so the Android menu offers HQ2x but not HQ3x or HQ4x, cleanEdge turns Smooth Scrolling off, and the CRT shader draws at half resolution with its phosphor mask at full resolution. It is tuned on a OnePlus 9.

## Controls

| Genesis | Keyboard | Controller (Xbox layout) |
|---------|----------|--------------------------|
| D-pad   | arrows   | D-pad or left stick      |
| A       | Z        | X                        |
| B       | X        | A                        |
| C       | C        | B                        |
| Start   | Enter    | Menu                     |
| Fast forward (story scenes) | Backspace (hold) | Y (hold) |
| Port menu | F1     | Back (View)              |

Controllers go through SDL's gamepad support (XInput, DirectInput and other Windows input APIs; on Linux evdev, hidraw and Steam Input), can be connected at any time, and work alongside the keyboard.

Holding Backspace, or Y on a controller, runs the game's story scenes at four times their speed: the story pictures after boot (up to the title), the story after Start (Mizrabel takes Minnie), the scenes between worlds, and the ending and the scenes after it. The game's own frames are the same as without it, just shown faster, and its sound is muted while it is held. It does nothing in the title screen and menus, in level play or in boss fights, nor on a Boss Rush's results screen.
