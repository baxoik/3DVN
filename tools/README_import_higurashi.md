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

    python3 tools/import_higurashi.py "<chapter folder>" games/onikakushi

Options:

| Option | Meaning |
| --- | --- |
| `--alt-art` | Use the alternative character art (`CGAlt`, the slimmer style drawn at 1280x960) instead of the default sprites. |
| `--no-tips` | Leave out the TIPS. |
| `--no-hd` | Don't keep the full-size pictures in `hd/` (3DS only). |
| `--title T` | Game title (default "Higurashi - <arc>"). |

Onikakushi converts in under a second to about 100 MB.

## What is converted

- The story in English, as the game shows it: full-screen (NVL) text that
  stays on screen until the game clears the page. On the 3DS a page that would
  not fit starts a new one.
- Backgrounds, character sprites at their positions, music, sound effects,
  screen shakes. Pictures are scaled for the 3DS; the originals go to `hd/` for
  PC, Wii U and Android.
- The order of play from `flow.txt` (scripts in `Update/` override `Scripts/`,
  as in the game).
- TIPS: after each day the TIPS that the game unlocks there are offered in a
  menu ("TIPS: <title>", then "Continue"). Which day unlocks which TIPS is read
  from the game's own data (`Managed/Assembly-CSharp.dll`).
- 07th-Mod voices: `PlayVoice` lines are converted when the mod's voice files
  are installed (StreamingAssets/voice).

Not converted: colour filters (red flashes, the negative effect), the title
screen and the extras (`omake`). The converter lists what it skipped.

## Checking

    cc -O2 -Isource tests/walk_story.c source/gs.c source/wrap.c -o /tmp/walk_story
    /tmp/walk_story games/onikakushi 10
