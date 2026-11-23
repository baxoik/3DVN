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
- Voices from 07th-Mod's voice-only patch (see below).

Not converted: colour filters (red flashes, the negative effect), the title
screen and the extras (`omake`). The converter lists what it skipped.

## Voices (07th-Mod)

07th-Mod's voice-only patch adds the voices of the console versions and keeps
the original art, so it converts as is. It has two parts, from GitHub:

- [Onikakushi-Voices.7z](https://github.com/07th-mod/patch-releases/releases/download/higurashi-voice-only-v1.0/Onikakushi-Voices.7z)
  (180 MB, the voices, in `SE/s19/...`)
- [Onikakushi.Voice.Patch.v0.9.0.7z](https://github.com/07th-mod/onikakushi/releases/download/v0.9.0/Onikakushi.Voice.Patch.v0.9.0.7z)
  (the scripts with voice commands, in `Update/`)

Extract both into a copy of the game folder (so the Steam install stays as it
is) and convert the copy:

    cp -a "<chapter folder>" ~/higurashi-voiced
    cd ~/higurashi-voiced && 7z x Onikakushi-Voices.7z && 7z x Onikakushi.Voice.Patch.v0.9.0.7z
    python3 tools/import_higurashi.py ~/higurashi-voiced games/onikakushi

Onikakushi voiced is about 310 MB (5,000 voice clips). The voices play like
3DVN voice clips: in the backlog, and auto mode waits for them.

The newer full 07th-Mod patch (PS3 sprites and backgrounds, ADV mode) is not
supported yet: its scripts use the mod's own commands.

## Checking

    cc -O2 -Isource tests/walk_story.c source/gs.c source/wrap.c -o /tmp/walk_story
    /tmp/walk_story games/onikakushi 10
