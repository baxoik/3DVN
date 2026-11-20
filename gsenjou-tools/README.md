# G-Senjou no Maou on 3DVN: voices, scene fixes and testing

Tools for turning a voiceless G-Senjou build made with `tools/import_gsenjou.py`
into a fully voiced one, fixing face portraits and leftover sprites, adding a
chapter select, getting the result onto a 3DS, and testing it in an emulator.

**This repository contains no game content.** Every tool here works on files
from your own copy of the game. Keep those files, and anything built from them,
out of git (see `.gitignore`).

## Engine changes on this branch

- **Auto mode:** Y, or the "Auto (Y)" touch button, toggles it. Voiced lines
  advance 0.5 s after the voice ends. Unvoiced lines wait 1 s plus 0.04 s per
  character, up to 6 s. It stops at choices and turns off when you leave a game.
- **Long lines fit the text box:** a line that would wrap past the box is drawn
  smaller, down to 60% at most.
- **Close-up sprites in front:** sprites whose name ends in `_b` are drawn after
  the others, matching the original's layer order.
- **No "THE END" over end cards:** it's only drawn over an empty or black screen.
- **Test build:** `make EXTRA_CFLAGS=-DAUTOTEST` logs on-screen notices, errors,
  chapter loads and shrunk or overflowing lines to `sdmc:/3dvn-test.log`.

## What you need

- A voiceless game folder from `tools/import_gsenjou.py`.
- The chapter scripts as readable `.ks` files (`g01.ks`, `g02.ks`, ...) and the
  voice clips as `.ogg`, from your own copy of the game. Getting those files out
  of your copy isn't covered here.
- `heads.txt`: the `getStNameHead()` case list from the game's `macro.ks`, one
  line per case, in the form `"<name>":tempName = "<head>"`. Copy it from your
  own copy.
- Python 3 with Pillow, ffmpeg, and a C compiler (for the script checker).

## Build the voiced game

1. Convert the voice clips to WAV (16 kHz mono). A sound effect can be at most
   2 MB, so re-encode any longer clip at 11025 Hz:

   ```sh
   mkdir -p voice-wav
   for f in voice/*.ogg; do
     ffmpeg -v error -i "$f" -ac 1 -ar 16000 -sample_fmt s16 "voice-wav/$(basename "$f" .ogg).wav"
   done
   find voice-wav -name '*.wav' -size +2048k   # re-encode these with -ar 11025
   ```

2. Build:

   ```sh
   GAME=path/to/g-senjou-no-maou KS=path/to/ks VOICES=voice-wav HEADS=heads.txt \
     gsenjou-tools/build_voiced.sh
   ```

   This runs, in order:
   - `add_voices.py` puts a voice line before each spoken line. The importer's
     own text pipeline is run on the readable scripts and lined up with your
     build's lines: exact matches first, then near matches (same speaker, ≥85%
     similar, numbers spelled out), then lines merged or split between versions,
     then reworded lines paired in order between matched anchors. Voices are
     stored in `audio/v/<group>/` folders of at most 100 files, because FAT32
     slows down badly with one huge folder.
   - `fix_scene.py` shows face portraits the original way. `@mface` and `@chr`
     only remember a face; each line shows its speaker's remembered face (ADV
     mode only); `@clear_mface` forgets them all. Every background change and
     chapter start also hides all five sprite slots.
   - `chapter_select.py` adds a nested chapter-select menu after the
     adult-scenes question. Each entry sets the story flags you'd have earned
     before that chapter, so jumping ahead doesn't trigger flag-gated bad endings.
   - `normalize_text.py` replaces characters the 3DS system font may lack in
     dialogue, choices and names: full-width forms become ASCII, `～`/`〜` become
     `~`, and a few rare symbols get plain stand-ins.
   - Every script is checked with `tests/check_scripts.c`.

   `REPORT=silent.md python3 gsenjou-tools/add_voices.py ...` writes a report of
   voiced lines that couldn't be matched, instead of building. To pin one by
   hand, add `<voice><TAB><text found only in its build line>` to a local
   `voice_overrides.tsv` (or point `VOICE_OVERRIDES` at it). Keep that file
   local: it quotes dialogue.

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
sound, and "no audio" notices are expected.

---

Written with AI assistance (Claude Code).
