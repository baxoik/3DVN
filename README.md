# 3DVN

> **About this fork.** This is a personal fork of [baxoik/3DVN](https://github.com/baxoik/3DVN), made to play a fully voiced G-Senjou no Maou on a 3DS. It adds the following on top of upstream:
>
> **Engine**
> - **Auto mode:** Y or the "Auto (Y)" touch button. Voiced lines move on 0.5 s after the voice ends; unvoiced lines wait 1 s plus 0.04 s per character (max 6 s). It stops at choices.
> - **Long lines fit the text box:** a line that would wrap past the bottom of the box is drawn smaller (down to 60%) instead of running off the screen. Also sent upstream as [#1](https://github.com/baxoik/3DVN/pull/1).
> - **Close-up sprites in front:** sprites named `*_b` are drawn after the others, as in the original game.
> - **No "THE END" over a game's own end card:** it's only drawn over an empty or black screen.
> - **Text on the bottom screen** (branch `bottom-text`): Options → "Text screen: Bottom" puts the dialogue, speaker name and face on the bottom screen, so the top screen shows only the scene. Choices appear on the bottom screen as buttons you can tap (D-pad + A still work). A third setting, "Top, choices below", keeps the text box on the top screen and uses the bottom screen only for controls and tappable choices, the layout planned for a Wii U TV + GamePad. The other buttons move to a slim strip along the bottom edge. It's saved per game, and the default stays "Top".
> - **More Options** (branch `bottom-text`): text speed (Slow / Normal / Fast / Instant), auto-mode speed (Short / Normal / Long), and "Fast-forward: Read only", which skips only lines you've already read. Read lines are remembered per game on the SD card. In the bottom-text layout, tapping the dialogue also advances like A.
> - **Emulator test build:** `make EXTRA_CFLAGS=-DAUTOTEST` logs notices, errors, chapter loads and shrunk or overflowing lines to `sdmc:/3dvn-test.log`.
>
> **[gsenjou-tools/](gsenjou-tools/README.md)** (for G-Senjou no Maou)
> - **Voices:** adds the voice clips to a voiceless import. All 13,425 voiced lines are matched, even where your build's translation words a line differently. Voices go in small folders so FAT32 SD cards don't stall before each line.
> - **Scene fixes:** face portraits show only on the speaker's own lines, and sprites no longer stay on screen across chapters (the importer part of this is also upstream as [#2](https://github.com/baxoik/3DVN/pull/2)).
> - **Chapter select:** jump to any chapter or route part; earned story flags are set, so you don't land in a flag-gated bad ending.
> - **3DS upload over FTP**, and **emulator testing:** a headless driver for the Azahar libretro core that plays each chapter with fast-forward and collects the test log and screenshots.
>
> No game content is included; the tools work on files from your own copy. Upstream has no license, so this fork is offered as-is. These changes were written with AI assistance (Claude Code).

3DVN is a WIP native visual novel engine for the Nintendo 3DS, heavily inspired by VNDS for the original DS (you can find an archive [here](https://github.com/BASLQC/vnds)), but intended to allow for more advanced visual novels to be made, and for novels converted to the engine to provide a closer experience to their original PC versions.

Features: Speaker names above the textbox, optional custom textboxes and UI for games, optional NVL style textbox, an optional glossary feature for novels that have those (for example SciAdv games), which even supports unlocking terms through game progression, and a very early and very experimental video player.

Engine compatibility: Eventually, I would like to implement support for various VN engines, to the point where you can simply run a game throught a converter and have a functional 3DVN game. Currently the only engine with any worthwhile support is VNDS, which has full compatibilty due to every VNDS term having a 3DVN equivalent. VNDS terms can be converted using [import_vnds.py](/tools/import_vnds.py). Some VNDS games may have issues when converted (the only one I have seen so far is duplicated lines), but these can all be easily fixed by editing the .3vs file by hand.

For documentation (building the engine, running games, terms and usage of the engine, etc), see [documentation.txt](documentation.txt).

Quick start: run `make` to  build `3dvn.3dsx` (or download it from a release), copy `3dvn.3dsx` to `sd:/3ds`, create a `/3dvn` folder in the `/3ds` folder, and a `/games` folder inside of that folder, which is where you will put your games.
The structure should look like `sd:/3ds/3dvn/games/[your game]`

AI disclosure: This project is AI-assisted and was built using my own coding experience and familiarity with vnds, alongside Visual Studio agents, primarily for the video player and converters for other engines. The final release will try to feature as little ai generated code as possible, but ai was still used along the way in the development of the engine.