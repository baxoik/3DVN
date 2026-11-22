# G-Senjou no Maou on 3DVN

Tools that build G-Senjou no Maou for 3DVN (3DS, PC and Wii U) from **your own Steam
copy of the game**, get it onto a 3DS, and test it in an emulator.

**This repository contains no game content.** Every tool here works on files
from your own copy of the game. Keep those files, and anything built from them,
out of git (see `.gitignore`).

## Engine features this game uses

(All in 3DVN `main`; the repository README lists everything the fork adds.)

- **Auto mode:** Y, or the "Auto (Y)" touch button, toggles it. Voiced lines
  advance 0.5 s after the voice ends. Unvoiced lines wait 1 s plus 0.04 s per
  character, up to 6 s. It stops at choices and turns off when you leave a game.
- **Long lines fit the text box:** a line that would wrap past the box is drawn
  smaller, down to 60% at most.
- **Close-up sprites in front:** sprites whose name ends in `_b` are drawn after
  the others, matching the original's layer order.
- **No "THE END" over end cards:** it's only drawn over an empty or black screen.
- **Endings checklist:** 3DVN records the chapter each ending finishes in;
  `endings.txt` names them.
- **Test build:** `make EXTRA_CFLAGS=-DAUTOTEST` logs on-screen notices, errors,
  chapter loads and shrunk or overflowing lines to `sdmc:/3dvn-test.log`.

## What you need

- G-Senjou no Maou from Steam, installed (with or without its voices: a copy without
  `arc/voice.xp3` builds an unvoiced game).
- Python 3 with numpy and Pillow, ffmpeg, and a C compiler (for the script checker).
- About 10 GB of free space while building.

## Build

```sh
gsenjou-tools/build_from_steam.sh
```

That's all. If your Steam library is somewhere else, point `STEAM` at the game's
install folder (the one with `data.xp3` and `arc/`):

```sh
STEAM="/path/to/G-senjou no Maou - The Devil on G-String" gsenjou-tools/build_from_steam.sh
```

It makes:

- `packs/g-senjou-no-maou-3ds.3vp`: for the 3DS (pictures scaled for its screen, sound
  as Ogg Vorbis).
- `packs/g-senjou-no-maou.3vp`: for PC and Wii U (full-resolution pictures in `hd/`,
  the game's own audio unchanged).

Copy one to `<3dvn root>/games/g-senjou-no-maou.3vp` (renaming the 3DS one). Settings:
`WORK` (where the unpacked copy goes, default `steam-work`; reused on later runs),
`OUT` (the game folder, default `voiced/g-senjou-no-maou`), `PACKS` (default `packs`;
empty to skip packing).

### What the steps do

1. `extract_steam.py` unpacks the game's `.xp3` archives in Python (no Wine, GARbro or
   KrkrExtract): the Steam release XORs each file with a byte taken from its checksum,
   and every file is checked against that checksum. Scripts are written out as plain
   text, and `tools/decode_tlg.py --compose` turns the TLG images into PNGs with each
   expression pasted onto its body.
2. `tools/import_gsenjou.py --steam` converts the scripts (the Steam English text), with
   each spoken line's voice taken from the script itself. The game draws at 1280x720:
   pictures are scaled by 1/3 for the 3DS (backgrounds 427x240, a few pixels cropped at
   the sides of its 400-wide screen, shown whole on the Wii U's wide TV layout) and the
   originals go to `hd/` for PC and Wii U. Sprites stand where the original puts them
   (slot positions and per-character offsets from the game's `character_pos.tjs`).
   Voices are stored in `audio/v/<group>/` folders of at most 100 files, because FAT32
   slows down badly with one huge folder.
3. Fixes and extras:
   - `fix_scene.py` shows face portraits the original way. `@mface` and `@chr` only
     remember a face; each line shows its speaker's remembered face (ADV mode only);
     `@clear_mface` forgets them all. Every background change and chapter start also
     hides all five sprite slots. It reads the speaker table from the game's `macro.ks`.
   - `fix_leaked_commands.py` turns KiriKiri commands that ended up as dialogue back into
     script (`@return`, `@fobgm`), and `[ font italic="true" ]` into `[i]` (italics on
     PC and Wii U).
   - `chapter_select.py` adds a nested chapter-select menu after the adult-scenes
     question. Each entry sets the story flags you'd have earned before that chapter,
     so jumping ahead doesn't trigger flag-gated bad endings.
   - `normalize_text.py` replaces characters the 3DS system font may lack.
   - `endings.txt` lists the seven endings for the endings checklist.
   - Every script is checked with `tests/check_scripts.c`.
   - `make_loops.py` turns the loop points of looping sound effects (`*.ogg.sli`) into
     `audio/<name>.loop` (rain, phone and other ambience).
4. `tools/make_pack.py` packs the two archives.

### Older builds from the fan dump

Before the Steam tools, the game was built from a separate 800x600 fan dump
(`import_gsenjou.py --src`), with voices lined up from the Steam scripts
(`build_voiced.sh`, `add_voices.py`) and HD pictures matched in (`make_hd.py`,
`make_archives.sh`). Those tools still work for such a dump but aren't needed with a
Steam copy.

## Get it onto the 3DS

Copy `3dvn.3dsx` to `sd:/3ds/` and the game folder to
`sd:/3ds/3dvn/games/g-senjou-no-maou/`. A card reader is much faster than FTP
for the roughly 16,000 files. If you use FTP (e.g. ftpd on the 3DS, port 5000):

```sh
python3 gsenjou-tools/ftp_upload.py <3DS-IP> 5000 voiced/g-senjou-no-maou /3ds/3dvn/games
```

`ftp_restructure.py` moves voices that are already on the card from `audio/`
into the `audio/v/<group>/` layout with FTP renames, then uploads the scripts.

## Test in an emulator

`emu/drive.py` is a small headless libretro frontend for the Azahar 3DS core
(`azahar_libretro.so` from an Azahar release). It renders with an off-screen
EGL/OpenGL context, presses scripted buttons and saves screenshots.
`emu/run_chapter.sh` plays one chapter with the `AUTOTEST` engine build:
fast-forward, tapping A at choices, until the next chapter loads.

```sh
GAME=voiced/g-senjou-no-maou CORE=azahar_libretro.so ENGINE=3dvn-autotest.3dsx \
  gsenjou-tools/emu/run_chapter.sh g01
```

Each run leaves `result.txt`, `test.log` and sample screenshots in
`emu/runs/<chapter>/`. The emulator has no DSP firmware, so it runs without
sound, and "no audio" notices are expected. With `PACK=path/to/game.3vp` the
chapter plays from the archive instead of the folder (results in
`emu/runs-pack/<chapter>/`), which tests the archive reading code too.

`tests/walk_story.c` plays a whole game through the script VM many times with
random choices (no emulator needed) and reports script errors, missing images
and sounds, endless loops and which chapters are reached.

---

Written with AI assistance (Claude Code).
