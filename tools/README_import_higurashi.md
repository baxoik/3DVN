# Converting Higurashi When They Cry Hou to 3DVN

`import_higurashi.py` turns a chapter of the MangaGamer release (Steam: Ch.1
Onikakushi, Ch.2 Watanagashi, ...) into a 3DVN game folder. Only convert games
you own.

## What you need

- Python 3.8 or newer and Pillow (`pip install pillow`). No ffmpeg: the game's
  music and sounds are already Ogg Vorbis and are copied unchanged.
- The installed chapter. On Steam (Linux) for example:

      ~/.local/share/Steam/steamapps/common/Higurashi When They Cry

  (the folder that contains `HigurashiEp01_Data`).

## Usage

    python3 tools/import_higurashi.py "<chapter folder>" games/onikakushi [options]

It works with two kinds of game folder and finds out which it has:

- **the Steam release as installed** (640x480 art, no voices);
- **the Steam release with 07th-Mod's full voice and graphics patch** (voices,
  PS3 backgrounds and sprites, high-resolution versions of the other art, the
  opening movie). See "Adding 07th-Mod" below.

| Option | Meaning |
| --- | --- |
| `--backgrounds ps3\|original` | `ps3` (default): 07th-Mod's high-resolution PS3 backgrounds; `original`: the 640x480 originals. The Steam release only has the originals. |
| `--sprites ps3\|mangagamer\|ryukishi` | `mangagamer`: MangaGamer's remade sprites (the Steam game's alternative art); `ryukishi`: the Ryukishi07-style sprites (the Steam game's default); `ps3`: 07th-Mod's PS3 sprites. Default: the game's own default (`ryukishi` on Steam, `ps3` with 07th-Mod). With 07th-Mod all three come at high resolution. |
| `--adv` | 07th-Mod: one line at a time in a text box with the speaker's name (in their colour), instead of full pages. |
| `--audio mangagamer\|gin\|houplus` | 07th-Mod music and sound set (default `mangagamer`). |
| `--censor N` | 07th-Mod censorship level 0-5 (default 2, the mod's default). |
| `--no-movies` | Leave out the opening movie (07th-Mod). |
| `--movie-size WxH` | Size of the converted movie (default 256x144, smooth on every 3DS; at most 512x256). |
| `--no-tips` | Leave out the TIPS. |
| `--no-hd` | Don't keep the full-size pictures in `hd/` (3DS only). |
| `--title T` | Game title (default "Higurashi - <arc>"). |

Example, high-resolution backgrounds with MangaGamer's sprites:

    python3 tools/import_higurashi.py ~/higurashi-07th-mod games/onikakushi --sprites mangagamer

Where a chosen set lacks a sprite (07th-Mod's MangaGamer set has no close-up
portraits), the PS3 one is used and the converter says how often.

## What is converted

- The story in English: full-screen (NVL) text that stays on screen until the
  game clears the page (on the 3DS a page that would not fit starts a new
  one), or with `--adv` line by line with names.
- Backgrounds, character sprites at their positions, music, sound effects,
  voices (07th-Mod), screen shakes. Pictures are scaled for the 3DS (07th-Mod's
  16:9 art to 400x225); the originals go to `hd/` (at most 2048 pixels wide)
  for PC, Wii U and Android.
- The opening movie (07th-Mod), converted with `tools/mk3vv.py` (needs ffmpeg)
  and played with 3DVN's `video` command; A skips it.
- The order of play from `flow.txt` (`Update/` scripts override `Scripts/`),
  and the TIPS: after each day the ones the game unlocks there are offered in a
  menu, read from the game's own data (`Managed/Assembly-CSharp.dll`).
- 07th-Mod's settings are decided when converting: its ADV/NVL switches,
  censorship branches and called script sections are resolved then.

Not converted: lip sync (the closed-mouth frame is shown), sliding sprites,
colour filters (red flashes, night/sunset tints), the title screen and the
extras (`omake`). The converter lists what it skipped.

## Adding 07th-Mod

Install it into a copy of the game, so the Steam folder stays as it is. The
07th-Mod installer (github.com/07th-mod/python-patcher) lists the parts in
`installData.json`; for Onikakushi, extract in this order:

1. `onikakushi-base.7z` (1.35 GB: voices, PS3 backgrounds and sprites, movie)
2. `MangaGamer-Question-BGM-SE-Restore.7z` (music and sound sets)
3. `Onikakushi-UI_5.2.2f1_unix.7z` (not used by the converter)
4. `hou-ogsprites-ep01.7z` (high-resolution Ryukishi07 sprites)
5. `Onikakushi.Voice.and.Graphics.Patch.zip` from github.com/07th-mod/onikakushi
   (the scripts, last)

Onikakushi with 07th-Mod converts in about 10 seconds to about 450 MB.

## Checking

    cc -O2 -Isource tests/walk_story.c source/gs.c source/wrap.c -o /tmp/walk_story
    /tmp/walk_story games/onikakushi 10
