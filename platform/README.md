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
./3dvn-pc                                 # desktop window
./3dvn-pc --fullscreen                    # fullscreen (also F11 / Alt+Enter)
./3dvn-pc --dual                          # the classic two-screen window
```

**Desktop mode** (the default): one resizable window showing the main screen in the wide
16:9 layout, rendered sharply for your screen. Menus, save slots, options, choices, the title
menu and the game list appear as a clickable overlay over the dimmed scene, only while needed.

| | reading | with the overlay up |
| --- | --- | --- |
| left click | next line | click a button |
| right click | menu | back |
| mouse wheel | down: next line, up: backlog | move through lists |
| Esc | menu | back |
| F11 / Alt+Enter | fullscreen on / off | |

The keyboard keys below still work. Quit with the window's close button, the game list's
Exit, or the in-game Quit. Games, saves and settings are the same as on the 3DS and Wii U
(`./3dvn/games/`, `./3dvn/saves/`), including Sync Saves.

**Two-screen mode** (`--dual`, `VN_DUAL=1`; also used by the test scripts): both screens in one
window, top above bottom (2x by default, `VN_SCALE=3` etc.); clicking on the bottom screen is a
touch. Keys: Z/Enter/Space = A, X/Backspace = B, S = X, A = Y, Q = L, W/Tab = R (hold to
fast-forward), Esc = START, Shift = SELECT, arrows = D-pad. A game controller also works. The
system font is used (set `VN_FONT=/path/to/font.ttf` to choose one).

## Wii U

Needs devkitPro with devkitPPC, wut and the Wii U SDL2 / SDL2_ttf portlibs (all in
the `devkitpro/devkitppc` container image).

```sh
/opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -S platform/wiiu -B build-wiiu
cmake --build build-wiiu                  # -> build-wiiu/3dvn.wuhb (and 3dvn.rpx)
```

**Install:** Aroma: copy `3dvn.wuhb` to `sd:/wiiu/apps/`. Homebrew Launcher (Tiramisu,
Haxchi, CBHC): copy `3dvn.rpx`, `meta.xml` and `platform/wiiu/hbl-icon.png` renamed to
`icon.png` into `sd:/wiiu/apps/3dvn/`. Games go in `sd:/wiiu/3dvn/games/` (a folder per
game or one `<name>.3vp` archive). Large archives copy much faster with a card reader
than over FTP.

**Screens:** the TV shows the scene at the console's resolution (1080p or 720p); the
GamePad (854x480, its full resolution) shows the controls, menus, save slots, backlog
and choices as touch buttons. Options → "TV screen: Wide" uses the TV's whole 16:9
width for text box and menus (pictures keep their shape). **Minus** switches to
off-TV play: everything on the GamePad, taps advance the text; Minus again returns to
the TV.

**Buttons** follow the GamePad labels (A, B, X, Y, L, R/ZR, + = START). START opens a
menu to leave to the Homebrew Launcher or the Wii U Menu; the in-game Quit offers the
same plus the game list.

**Per game:** a text file `games/<name>.txt` adds a Guide button (read on the GamePad,
swipe to scroll, reopens where you left off); `games/<name>.endings.txt` or the game's
own `endings.txt` adds an endings checklist. Playtime and the time of day show on the
GamePad.

The console's own font is used; if it can't be read, put a `font.ttf` in
`sd:/wiiu/3dvn/`. `sd:/wiiu/3dvn/boot.log` (also sent to `OSReport`) traces start-up
and slow frames.

**Status:** tested on a real Wii U (Homebrew Launcher) and in Cemu 2.6. Cemu note: on
Linux set Options → General → Audio API to **Cubeb**; with the Windows-only default,
Cemu hangs every homebrew title before its first frame. Screenshots from the test hook
(`shot`) come out garbled on Wii U (pixel readback format); they are fine on PC.

## Sync saves over the network (in game)

**Sync Saves** (title menu and in-game menu) syncs the game's saves with a small FTP
server on your network: [sync-server/](../sync-server/README.md) (a Docker container,
also for ARM boards such as an ODROID or Raspberry Pi). Enter the server's address
(`192.168.0.10:2121`) on the keypad; it is remembered in `settings/sync.cfg`. 3DVN then
lists each save slot as newer here, newer on the server, or only on one side, and you
choose **Pull** (the server's saves replace yours), **Push** (yours replace the server's)
or **Newest of each**. Endings / unlocks and read lines are merged both ways. Saves are
stored in the shared 3DS / Wii U format (older Wii U saves are converted), and every
replaced file is kept once as `.bak`, on the console and on the server.

**Auto sync while playing:** the same screen's last entry turns it on (stored as `auto 1` in
`settings/sync.cfg`). Then each time you save, the game's saves sync in the background,
newest of each (only then; nothing is sent otherwise): no waiting, a short notice when
saves were downloaded or uploaded, or once when the server can't be reached. A download only
replaces a slot that wasn't saved over meanwhile; endings and read lines are merged.

## Continue (resume where you left off)

Besides the 15 slots, each game keeps `saves/<game>_resume.3vsav`: where you left off. It is
written on every way of leaving a game (back to the title or the game list, closing the
app, HOME and sleep on the 3DS, HOME on the Wii U, before a sync) and when a new chapter
begins, in case the power goes; only while a line or a choice is on screen, otherwise the
previous one stays. The end of the story removes it. The title offers it as **Continue**.
The same file is used by the 3DS, Wii U and PC versions and the Android reader.

With auto sync on, a resume written on leaving is uploaded (it is where you really were
last, whatever the clocks say), and **Continue** first asks the server for its resume
(one request): if it is the same, or there is none, you continue here; if there is only
the server's, you continue from it; if they differ, you pick, "Continue from server
(time)" or "Continue here (time)" (console clocks are often off, so the newer one is not
just taken). Auto sync never replaces the resume by itself. Sync Saves lists it as "Res.".

## Sharing saves between 3DS and Wii U

Save files are the same on both: every 3DVN build from 2026-11-22 on writes them in one
byte order (the 3DS's) and reads both orders, so older Wii U saves still load. To sync,
run FTP servers on both consoles (ftpd on the 3DS, FTPiiU on the Wii U) and, on a PC:

```sh
python3 tools/sync_saves.py 3ds:<3DS-IP> wiiu:<Wii-U-IP>
```

Each save slot goes both ways (the newer one wins); endings and unlocks
(`saves/<game>.3vglobal`) and read lines (`settings/<game>.3vread`) are merged. Nothing is
deleted, and every replaced file is backed up on the PC first. `--dry-run` shows what
would change; a mounted SD card folder works in place of either console. The game must
have the same name on both (`games/<name>.3vp`). PC saves use a different layout and are
not shared.

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
