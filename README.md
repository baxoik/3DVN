# 3DVN

> **3DVN-wiiu.** This fork of [baxoik/3DVN](https://github.com/baxoik/3DVN) brings the engine to the **Wii U**, **Android** and PC, alongside the 3DS, and adds what's needed to play long voiced novels such as G-Senjou no Maou and Ever17 comfortably. See [platform/README.md](platform/README.md) for building and installing on Wii U and PC, and [platform/android/README.md](platform/android/README.md) for Android.
>
> **Wii U**
> - TV + GamePad: the scene on the TV at 1080p (or 720p), controls, menus, save slots, backlog and choices as touch buttons on the GamePad at its full resolution. Optional wide TV layout (text box and menus use the full 16:9 width; pictures keep their shape).
> - Off-TV play: Minus puts everything on the GamePad.
> - Leave to the Homebrew Launcher or the Wii U Menu (START, or the in-game Quit).
> - Saves shared with the 3DS version: **Sync Saves** in the game menu syncs them with a small FTP server on your network ([sync-server/](sync-server/README.md), a Docker container that also runs on ARM boards such as an ODROID); `tools/sync_saves.py` does the same from a PC.
> - Guide reader (`games/<name>.txt`, read on the GamePad, swipe to scroll, remembers your place), endings checklist, playtime and clock, GamePad backlog with voice replay.
>
> **Engine (all platforms)**
> - SDL2 platform layer: the unchanged engine builds for PC and Wii U.
> - Game archives (`.3vp`, one file per game), Ogg Vorbis and MS-ADPCM audio, HD images for PC / Wii U (`hd/`), looping sound effects with loop points.
> - Auto mode, read-only fast-forward, text speed, volume sliders (master, music, voice), text on the top or bottom screen, smooth text reveal, italics (PC / Wii U), long lines shrunk to fit the text box, global variables (endings seen, unlocks), saves that keep working when a game's scripts are updated.
>
> **Android** ([platform/android/](platform/android/README.md))
> - The same engine for colour phones and tablets: same game folders, saves, Continue and save sync. Landscape like the PC; portrait stacks the two screens like a 3DS.
>
> **Continue (all platforms)**
> - Leaving the game in any way (Home, sleep, closing the app, START) keeps your place; **Continue** on the title picks it up. With save sync, Continue offers the newer place from another console.
>
> **Converters**
> - [gsenjou-tools/](gsenjou-tools/README.md): build G-Senjou no Maou from your own Steam copy in one command (voices, HD art, the unrated patch if installed, chapter select).
> - [ever17-tools/](ever17-tools/README.md): build Ever17 from one complete game folder (the PC release, ideally with the Himmel Edition patch applied) in one command: full voices, music, HD art, endings checklist. GPLv2, ported from Ever17Converter.
>
> - [tools/import_vnds.py](tools/README_import_vnds.md): any VNDS game, plus `import_never7.py` for Never 7 from "Never 7 - the end of infinity" (n7e.love), with the Append Story, movies and optionally the Steam remaster's music.
> - [tools/import_higurashi.py](tools/README_import_higurashi.md): Higurashi When They Cry Hou from Steam (plain or with the 07th-Mod full patch; art styles, chapters, the opening movie).
> - [tools/import_narcissu.py](tools/README_import_narcissu.md): the Narcissu 10th Anniversary Anthology from Steam (Majiro engine): 1st & 2nd, Zero, Iris, Sumire, Himeko.
>
> No game content is included; the tools work on files from your own copy. Third-party code in the builds: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Upstream has no license, so this fork is offered as-is. These changes were written with AI assistance (Claude Code).

3DVN is a WIP native visual novel engine for the Nintendo 3DS, heavily inspired by VNDS for the original DS (you can find an archive [here](https://github.com/BASLQC/vnds)), but intended to allow for more advanced visual novels to be made, and for novels converted to the engine to provide a closer experience to their original PC versions.

Features: Speaker names above the textbox, optional custom textboxes and UI for games, optional NVL style textbox, an optional glossary feature for novels that have those (for example SciAdv games), which even supports unlocking terms through game progression, and a very early and very experimental video player.

Engine compatibility: Eventually, I would like to implement support for various VN engines, to the point where you can simply run a game throught a converter and have a functional 3DVN game. Currently the only engine with any worthwhile support is VNDS, which has full compatibilty due to every VNDS term having a 3DVN equivalent. VNDS terms can be converted using [import_vnds.py](/tools/import_vnds.py). Some VNDS games may have issues when converted (the only one I have seen so far is duplicated lines), but these can all be easily fixed by editing the .3vs file by hand.

For documentation (building the engine, running games, terms and usage of the engine, etc), see [documentation.txt](documentation.txt).

Quick start: run `make` to  build `3dvn.3dsx` (or download it from a release), copy `3dvn.3dsx` to `sd:/3ds`, create a `/3dvn` folder in the `/3ds` folder, and a `/games` folder inside of that folder, which is where you will put your games.
The structure should look like `sd:/3ds/3dvn/games/[your game]`

AI disclosure: This project is AI-assisted and was built using my own coding experience and familiarity with vnds, alongside Visual Studio agents, primarily for the video player and converters for other engines. The final release will try to feature as little ai generated code as possible, but ai was still used along the way in the development of the engine.