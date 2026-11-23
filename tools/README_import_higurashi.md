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
| `--art console\|remake\|original` | Art set. `console` is the game's default (with 07th-Mod's full patch: the PS3 art); `remake` the alternative sprites (`CGAlt`); `original` 07th-Mod's Ryukishi07 art. `--alt-art` = `--art remake`. |
| `--adv` | 07th-Mod full patch: one line at a time in a text box with the speaker's name (in their colour), instead of full pages. |
| `--audio mangagamer\|gin\|houplus` | 07th-Mod music and sound set (default `mangagamer`). |
| `--censor N` | 07th-Mod censorship level 0-5 (default 2, the mod's default). |
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

## 07th-Mod full patch (PS3 art, voices, ADV mode)

The full voice and graphics patch converts too. The 07th-Mod installer
(github.com/07th-mod/python-patcher) lists its parts in `installData.json`;
for Onikakushi, extract into a copy of the game, in this order:

1. `onikakushi-base.7z` (1.35 GB: voices, PS3 backgrounds and sprites)
2. `MangaGamer-Question-BGM-SE-Restore.7z` (music and sound sets)
3. `Onikakushi-UI_5.2.2f1_unix.7z` (not used by the converter)
4. optionally `hou-ogsprites-ep01.7z` (Ryukishi07 sprites for `--art original`)
5. `Onikakushi.Voice.and.Graphics.Patch.zip` from github.com/07th-mod/onikakushi
   (the scripts, last)

The mod's settings are decided when converting (`--adv`, `--art`, `--audio`,
`--censor`): its ADV/NVL switches, censorship branches and called script
sections are resolved then. The 16:9 PS3 art becomes 400x225 on the 3DS, with
the full-size art in `hd/` (at most 2048 pixels wide) for PC, Wii U and
Android. Onikakushi: about 430 MB, 5,000 voices. Lip sync, sliding sprites
(MoveBustshot) and the colour filters (night/sunset tints) are not converted.

## Checking

    cc -O2 -Isource tests/walk_story.c source/gs.c source/wrap.c -o /tmp/walk_story
    /tmp/walk_story games/onikakushi 10
