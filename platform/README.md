# 3DVN on PC and Wii U (SDL2)

`sdl/` is a small platform layer that implements the subset of the 3DS libraries
3DVN uses (libctru input and timing, citro2d drawing and text, ndsp audio) on top
of SDL2 and SDL2_ttf. `sdl/include/3ds.h` and `sdl/include/citro2d.h` stand in for
the real headers, so the engine in `source/` builds unchanged; only three small
spots in `source/main.c` are `#ifdef VN_SDL` (texture upload, root folder, test log).

The engine still draws a 400x240 top screen and a 320x240 bottom screen. Each is
rendered off-screen at a higher resolution (text stays sharp) and shown:

- **PC:** both in one window, top above bottom (2x by default, `VN_SCALE=3` etc.).
- **Wii U:** top screen on the **TV** (1200x720), bottom screen on the **GamePad**
  (640x480). One GX2 swap updates both screens, so only the GamePad window swaps
  (the TV window uses SDL_WINDOW_WIIU_PREVENT_SWAP); swapping per window flickers. With Options → Text screen → "Top, choices below", the TV shows the
  scene and dialogue and the GamePad holds the controls and tappable choices.

Games are the same folders as on the 3DS (`<root>/games/<name>/main.3vs` ...).

## PC

Needs a C compiler, SDL2 and SDL2_ttf (e.g. `dnf install SDL2-devel SDL2_ttf-devel`).

```sh
make -f platform/sdl/Makefile.pc          # -> ./3dvn-pc, games in ./3dvn/games/
./3dvn-pc
```

Keys: Z/Enter/Space = A, X/Backspace = B, S = X, A = Y, Q = L, W/Tab = R (hold to
fast-forward), Esc = START, Shift = SELECT, arrows = D-pad. Clicking on the bottom
screen is a touch. A game controller also works. The system font is used (set
`VN_FONT=/path/to/font.ttf` to choose one).

## Wii U

Needs devkitPro with devkitPPC, wut and the Wii U SDL2 / SDL2_ttf portlibs (all in
the `devkitpro/devkitppc` container image).

```sh
/opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -S platform/wiiu -B build-wiiu
cmake --build build-wiiu                  # -> build-wiiu/3dvn.wuhb (and 3dvn.rpx)
```

Copy `3dvn.wuhb` to `sd:/wiiu/apps/` (or `3dvn.rpx` plus `platform/wiiu/hbl-icon.png`
renamed to `icon.png` into `sd:/wiiu/apps/3dvn/` for the Homebrew Launcher) and games to
`sd:/wiiu/3dvn/games/<name>/`
(Aroma environment). The console's own font is used; if it can't be read, put a
`font.ttf` in `sd:/wiiu/3dvn/`. Buttons follow the GamePad labels (A, B, X, Y, L,
R/ZR, + = START, − = SELECT); the GamePad touchscreen works for touch.

**Status:** runs in Cemu 2.6 (game list, title, chapters on TV + GamePad, no
flicker). Cemu note: on Linux set Options → General → Audio API to **Cubeb**; with
the Windows-only default, Cemu hangs every homebrew title before its first frame.
Not yet checked on a real console. `sd:/wiiu/3dvn/boot.log` (also sent to
`OSReport`) traces start-up. Screenshots from the test hook (`shot`) come out
garbled on Wii U (pixel readback format); they are fine on PC.

## Unattended tests (PC and Wii U)

If `VN_SCRIPT=<file>` is set (or `<root>/autotest.txt` exists), the platform layer
plays a button script: `wait N`, `press KEY[+KEY] [N]`, `hold KEY N`,
`tap X Y` (bottom-screen pixels), `shot NAME` (saved as BMP in `VN_SHOTS` or
`<root>/shots/`, one per window), `quit`. Headless on PC:

```sh
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy VN_SOFTWARE_RENDERER=1 \
  VN_SCRIPT=test.txt VN_SHOTS=shots ./3dvn-pc
```

## Game archives (.3vp) and Ogg audio (all platforms)

A game can be one file instead of a folder: `games/<name>.3vp`, an uncompressed
("stored") ZIP of the game folder. The engine reads files straight out of it, so a
whole game is one copy to the SD card (much faster than thousands of small files on
FAT32) and the game list shows it like a folder. A folder `games/<name>/` next to the
archive holds override files (patches, mods): any file there replaces the archived one.

Audio can be Ogg Vorbis (`.ogg`, tried before `.wav`), decoded while streaming with
stb_vorbis, for music and for voices / sound effects (no 2 MB limit for .ogg clips).

```sh
python3 tools/make_pack.py games/<name> <name>.3vp --ogg   # needs ffmpeg for --ogg
```

`--ogg` converts all WAV audio (voices `--voice-quality`, default 2; other audio
`--music-quality`, default 4) with a conversion cache, then packs. Example: the
voiced G-Senjou build goes from 2.2 GB in 16,000 files to one 352 MB archive.

## HD images (PC, Wii U)

A game may include `hd/<folder>/<file>` (e.g. `hd/bg/bg_02a.jpg`,
`hd/sprites/kanon_a_se_04_b.png`): sharper versions of its images. The PC and Wii U
builds draw them in place of `<folder>/<file>`, at that image's size, so layouts stay
the same and only detail improves. The 3DS ignores `hd/` (memory); pack a 3DS archive
with `make_pack.py ... --exclude hd`.
