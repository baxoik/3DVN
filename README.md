# 3DVN

3DVN is a WIP native visual novel engine for the Nintendo 3DS, heavily inspired by VNDS for the original DS (you can find an archive [here](https://github.com/BASLQC/vnds)), but intended to allow for more advanced visual novels to be made, and for novels converted to the engine to provide a closer experience to their original PC versions.

Engine compatibility: Eventually, I would like to implement support for various VN engines, to the point where you can simply run a game throught a converter and have a functional 3DVN game. Currently the only engine with any worthwhile support is VNDS, which has full compatibilty due to every VNDS term having a 3DVN equivalent. VNDS terms can be converted using [import_vnds.py] (./tools/import_vnds.py).

For documentation (building the engine, running games, terms and usage of the engine, etc), see [documentation.txt](documentation.txt).

Quick start: run `make` to  build `3dvn.3dsx` (or download it from a release), copy `3dvn.3dsx` to `sd:/3ds`, create a `/3dvn` folder in the `/3ds` folder, and a `/games` folder inside of that folder, which is where you will put your games.
The structure should look like `sd:/3ds/3dvn/games/[your game]`