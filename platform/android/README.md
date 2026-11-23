# 3DVN for Android (colour phones and tablets)

The 3DS / Wii U / PC engine running on Android through SDL2: the same game folders, saves,
Continue and save sync, in the PC's single-screen layout, full screen and landscape. (The
VNDS Android e-ink reader is a separate app, for e-ink devices.)

## Building

Needs the Android SDK with NDK 29.0.14206865 and CMake 3.22.1 (from the SDK manager).

    cd platform/android
    ./get-deps.sh                      # SDL2 2.32.10 and SDL2_ttf 2.24.0 into deps/ (not in git)
    echo "sdk.dir=$HOME/Android/Sdk" > local.properties
    ./gradlew assembleDebug            # app/build/outputs/apk/debug/app-debug.apk
    ./gradlew assembleRelease          # signed if keystore.properties is present

`keystore.properties` (not in git) names the release key: `storeFile`, `storePassword`,
`keyAlias`, `keyPassword`. Android 6.0 or newer; arm64, armv7 and x86_64.

## Games

The app keeps everything in its own folder, which a PC sees over USB:

    Android/data/io.github.davidgith1.threedvn/files/3dvn/
        games/<name>/      or games/<name>.3vp   the same game folders as on the consoles
        saves/  settings/                          as on the consoles (and synced)

With adb: `adb push games/mygame /sdcard/Android/data/io.github.davidgith1.threedvn/files/3dvn/games/`.
Start the app once first, so the folder exists.

## Layouts

Turn the phone to change the layout (it follows the phone's auto-rotate setting):

- **sideways**: one screen, the picture filling the display; the touch-screen menus appear
  over it when needed;
- **upright**: the two screens stacked, as on a 3DS: the story on top, the touch screen with
  its buttons (Menu, Backlog, Save, Load, Hide text, Auto, Quit) below.

The game carries on where it is when the phone is turned.

## Controls

- Tap the picture (upright: the top screen): next line (A). Taps on the menus and on the
  lower screen work as on the 3DS touch screen.
- Back: the menu (Save, Load, Backlog, Sync Saves, Options, Quit), or back out of it.
- Leaving the app (Home, switching apps) saves where you are; Continue on the title
  brings you back.
- Exit on the game list closes the app.

Save sync needs the server's address in Sync Saves; on Android 17 the app asks for the
"nearby devices" (local network) permission it needs to reach it.
