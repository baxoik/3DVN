# 3DVN

3DVN is a WIP native visual novel engine for the Nintendo 3DS, heavily inspired by VNDS for the original DS (you can find an archive [here](https://github.com/BASLQC/vnds)), but intended to allow for more advanced visual novels to be made, and for novels converted to the engine to provide a closer experience to their original PC versions.

Features: Speaker names above the textbox, optional custom textboxes and UI for games, optional NVL style textbox, an optional glossary feature for novels that have those (for example SciAdv games), which even supports unlocking terms through game progression, and a very early and very experimental video player.

Engine compatibility: Eventually, I would like to implement support for various VN engines, to the point where you can simply run a game throught a converter and have a functional 3DVN game. Currently the only engine with any worthwhile support is VNDS, which has full compatibilty due to every VNDS term having a 3DVN equivalent. VNDS terms can be converted using [import_vnds.py](/tools/import_vnds.py). Some VNDS games may have issues when converted (the only one I have seen so far is duplicated lines), but these can all be easily fixed by editing the .3vs file by hand.

For documentation (building the engine, running games, terms and usage of the engine, etc), see [documentation.txt](documentation.txt).

Quick start: run `make` to  build `3dvn.3dsx` (or download it from a release), copy `3dvn.3dsx` to `sd:/3ds`, create a `/3dvn` folder in the `/3ds` folder, and a `/games` folder inside of that folder, which is where you will put your games.
The structure should look like `sd:/3ds/3dvn/games/[your game]`

AI disclosure: This project is AI-assisted and was built using my own coding experience and familiarity with vnds, alongside Visual Studio agents, primarily for the video player and converters for other engines. The final release will try to feature as little ai generated code as possible, but ai was still used along the way in the development of the engine.