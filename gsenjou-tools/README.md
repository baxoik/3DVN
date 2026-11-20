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
   - `fix_leaked_commands.py` repairs KiriKiri commands that older imports left
     in as dialogue: with adult scenes off, each H-scene showed "@return" and
     friends as text and then played anyway. `@return` becomes `return` again,
     `@fobgm` a music fade, and gallery jumps and comments are dropped.
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

## HD images for PC and Wii U (optional)

The converted images are sized for the 3DS (backgrounds 320x240). With the game's
original images decoded to PNG, `make_hd.py` builds `hd/`. `tools/decode_tlg.py`
decodes the extracted `.tlg` files (TLG5, numpy + Pillow, no GARbro needed);
`--compose` pastes expression layers onto their bodies using the `.pos` files:

```sh
python3 tools/decode_tlg.py decoded --compose \
  --from extracted/bgimage --from extracted/evimage --from extracted/fgimage --from extracted/face
python3 gsenjou-tools/make_hd.py voiced/g-senjou-no-maou decoded
```

The 3DS images are scaled-down crops of the originals (other 4:3 framing for
backgrounds, upper bodies of full-length sprites), so each one is located in its
original by template matching over scale and position, and accepted only if the
match correlates well. Where the 4:3 version shows more than the 16:9 original
(most CGs are zoomed out compared with Steam), the original is placed where it lines
up and blended into the upscaled small image, which fills only the uncovered border.
Cut-ins keep their small image. With the Steam release about 2,010 images get HD
versions: all sprites and faces and 295 of 305 backgrounds and CGs.
Backgrounds that were redrawn for widescreen (objects moved or added) use the closest
4:3 area of the new drawing, so they are framed slightly differently. Then pack two archives: with `hd/` for PC / Wii U, and
`--exclude hd` for the 3DS.

## Pack it (optional, recommended)

```sh
python3 tools/make_pack.py voiced/g-senjou-no-maou g-senjou-no-maou.3vp --ogg
```

One 352 MB file (Ogg audio) instead of 2.2 GB in about 16,000 files.

For PC and Wii U, use the original audio from your copy unchanged (48 kHz voices,
44.1 kHz stereo music) instead of the 3DS-sized conversions:

```sh
python3 tools/make_pack.py voiced/g-senjou-no-maou g-senjou-no-maou.3vp --ogg \
  --audio-originals path/to/voice --audio-originals path/to/bgm --audio-originals path/to/sound
```

(795 MB; the 3DS archive keeps the smaller converted audio, which is easier to decode.) Put it in
`<3dvn root>/games/` instead of the folder. It works on 3DS, Wii U and PC.

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
