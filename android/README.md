# Android build

The Android app is SDL's Java activity (used straight from `third_party/SDL/android-project`)
running the port, built from `port/CMakeLists.txt` as `libmain.so` next to SDL's
`libSDL3.so`. It draws with OpenGL ES 3.0 and paces its pictures with Swappy, the Android
Games SDK's frame pacing library (`port/src/android_pacing.cpp`).

It plays with on-screen touch controls or a USB or Bluetooth controller (which hides the touch
controls until the screen is touched again); Back opens and closes the menu. MD+ soundtrack
packs are imported from the menu (below). The port's messages go to the log
(`adb logcat -s COI`).

## Requirements

- Android Studio (its bundled Java 17 is enough), with the Android SDK: NDK 28.2.13676358 (r28c) and
  CMake 3.22.1. The first build fetches Gradle 9.8.2, the Android Gradle plugin 9.4, SDK Build
  Tools 36, SDK Platform 37.2 and Swappy (`androidx.games:games-frame-pacing`).
- The desktop build in `build/port` first (`tools/build.py`, or CMake as in the main README):
  the Android build runs its `m68kmake` to generate Musashi's opcode handlers.
- `android/local.properties` with the SDK's location (not committed), for example
  `sdk.dir=C:/Users/you/AppData/Local/Android/Sdk`.

## Building and installing

Use the release build: the debug build's native code isn't optimised and plays far too
slowly.

```bash
cd android
JAVA_HOME="C:/Program Files/Android/Android Studio/jbr" ./gradlew.bat assembleRelease
adb install -r app/build/outputs/apk/release/app-release.apk
```

Signing: with `android/keystore.properties` (not committed; `storeFile`, `storePassword`,
`keyAlias`, `keyPassword`) the release build is signed with that key, otherwise with the
debug key. An APK signed with one key won't install over one signed with another: uninstall
first (which clears the app's ROM, settings and MD+ pack).

For testing:

- `-Pdebuggable=true` makes the release build debuggable, so `adb run-as` can reach the app's
  files (`files/settings.ini`, `files/rom`).
- `-PtimingLog=true` logs the frame rates and the CPU and GPU time every 2 seconds to
  `adb logcat -s COI`.

The APK holds 64-bit ARM (`arm64-v8a`, phones) and `x86_64` (the emulator) code, for Android
7.0 or later.

With a controller in the phone's USB port, use wireless debugging (Developer options):
`adb pair IP:PORT CODE` once with the pairing code, then `adb connect IP:PORT` with the port
the Wireless debugging screen shows (it changes when wireless debugging is turned on again).

## The ROM

On first start the app asks for the ROM with Android's file picker: choose your own dump of the
US Genesis cartridge (a 512 KB .bin file). It is checked against the US release's CRC32 (another
dump can still be used, after a warning) and copied into the app's private storage
(`files/rom/coi.bin`), so it is asked for only once. To be asked again, clear the app's storage.

For testing, it can also be copied in with adb:

```bash
adb push "rom/your-rom.bin" /data/local/tmp/coi.bin
adb shell "run-as io.github.saphiron86.coiport mkdir -p files/rom && run-as io.github.saphiron86.coiport cp /data/local/tmp/coi.bin files/rom/coi.bin"
adb shell rm /data/local/tmp/coi.bin
```

Settings live in the same place (`files/settings.ini`).

## MD+ soundtrack packs

Enhancements > Audio > Import MD+ Pack... asks for the folder that holds a pack (its .cue, .md
and WAV files, or a folder with the pack in it) and copies it into the app's storage
(`files/mdplus`, replacing any pack there), then switches its soundtrack on. Choose the folder
from the phone's storage, where the picker opens: Android's Downloads and Recent views list only
media files, so the pack's .cue and .md don't show there.
