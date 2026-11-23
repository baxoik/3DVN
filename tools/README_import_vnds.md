# Converting VNDS games to 3DVN

`import_vnds.py` turns a VNDS visual novel into a 3DVN game folder.


## What you need

- Python 3.8 or newer
- Pillow (image resizing): `pip install pillow`
- ffmpeg (audio conversion), one of:
  - install it with your package manager (`apt install ffmpeg`,
    `brew install ffmpeg`, or download it from ffmpeg.org on Windows) so
    that `ffmpeg` is on your PATH, or
  - `pip install imageio-ffmpeg`, which the script finds automatically, or
  - pass the program's location with `--ffmpeg PATH`.

  Without ffmpeg the images and script still convert, but only `.wav`
  sounds are copied; other audio is listed as "not converted".

## Usage

    python3 import_vnds.py SOURCE OUTPUT [options]

`SOURCE` is the VNDS game folder, as it would be copied to a DS or to the
VNDS Android app. It needs `script/main.scr` (or a `script.zip` containing
it) and normally has `background`, `foreground` and `sound`, each either as
a folder or as a `.zip` (both kinds of game are common; when both exist,
loose files win). `img.ini` gives the original screen size and `info.txt`
the title; `icon.png` becomes the game's icon. `OUTPUT` is the new game
folder to create.

Example:

    python3 tools/import_vnds.py ~/vnds/Kanon games/kanon --speakers

Then copy `games/kanon` into the `3dvn/games/` folder on your SD card (Wii U:
`sd:/wiiu/apps/3dvn/games/`), or import it in the Android app, and pick it
from the game list. Running it again over the same OUTPUT only converts what
is missing.

Tested with Anon's Nice Day, CLANNAD, Ever 17, Kanon, Narcissu 2, Never 7,
Planetarian, Red Shift, True Remembrance and Tsukihime from the VNDS
archive.

## Options

| Option | Meaning |
| --- | --- |
| `--speakers` | Treat lines like `Name: some text` as dialogue spoken by `Name` (shown as a name tag). Off by default, because narration that happens to contain a colon would be affected. |
| `--nvl` | Keep text on screen like VNDS does, a page at a time (new page at `cleartext` and when the page is full), instead of one line per text box. |
| `--source-size WIDTHxHEIGHT` | The game's original screen size, e.g. `--source-size 256x192`. Default: `img.ini`, else the backgrounds' size. |
| `--scale S` | Override the automatic scale, e.g. `--scale 1` keeps images at their original size. Sprite positions are adjusted to match. |
| `--title T` | Game title (default: from `info.txt`, else the folder name). |
| `--keep-main-loop` | See "Endings" below. |
| `--no-hd` | Don't keep the full-size pictures in `hd/` (smaller, 3DS only). |
| `--no-audio` | Skip audio. |
| `--wav` | Convert audio to mono WAV, for 3DVN builds older than Ogg support. `--music-rate` / `--sfx-rate` set its sample rates. |
| `--ffmpeg PATH` | Use this ffmpeg program. |
| `--jobs N` | Parallel conversions (default: one per CPU). |

## What you get

- `main.3vs`: the entry point; it starts `ch/main.3vs`.
- `ch/NAME.3vs`: one chapter per VNDS script. 3DVN loads one chapter at a
  time (the `script` command), so even CLANNAD-sized games (380 scripts,
  half a million lines) start quickly and fit the 3DS's memory.
- `characters.3vs`: the speakers found with `--speakers`.
- `bg/`, `sprites/`: pictures scaled to the 3DS top screen. Backgrounds are
  JPEG, sprites PNG with transparency.
- `hd/bg/`, `hd/sprites/`: the original pictures when they are larger than
  the 3DS version (most 640x480 and larger games). The PC, Wii U and Android
  versions draw these instead, so the game is as sharp as the original
  there; the 3DS ignores them.
- `audio/`: music, sounds and voices as Ogg Vorbis. Sources that already are
  Ogg are copied unchanged; others (AAC, MP3, WAV...) are converted with
  ffmpeg. Subfolders are kept, which matters on FAT32 SD cards with tens of
  thousands of voice files.
- `game.3vc`: title (and NVL display with `--nvl`).

## Image sizes and positioning

The converter fits the source canvas inside the 400x240 top screen without
stretching, centred. A 256x192 DS game becomes 320x240; a 640x480 game also
320x240 (with its full-size pictures kept in `hd/`). Sprites and their
coordinates use the same scale and offset as the canvas. The chosen size and
scale are printed. The converter stops with an error if a scaled image would
exceed the 3DS's 1024x1024 texture limit; check the source size or choose a
smaller scale.

## How commands are converted

| VNDS | 3DVN |
| --- | --- |
| `text` | narration (or dialogue with `--speakers`); `text ~` / `text !` are skipped |
| `text @...` | joined with the next line into one text box, as VNDS shows them |
| `bgload` | `bg` (fade if a time is given); hides sprites, as in VNDS |
| `setimg` | `show ... at X Y` (`setimg ~` hides all sprites) |
| `sound`, `music` | `sfx`, `music`; `~` stops them; `sound X -1` loops |
| `choice a\|b` | `menu`; the pick is stored in `selected` (1, 2, ...) |
| `setvar` | `set` (per save) |
| `gsetvar` | `set g_...`: a global, kept across saves and new games (endings seen, routes unlocked) and synced with save sync |
| `if`, `else`, `fi` | `if`, `else`, `end` |
| `goto`, `label` | `jump`, `label` |
| `jump file.scr [label]` | `script ch/file` (then to the label) |
| `jump $file $label` | the same with the script and label taken from variables (CLANNAD's "call and return") |
| `delay` | `wait` (VNDS counts 1/60 s) |
| `cleartext` | new page with `--nvl`; otherwise nothing (each line is its own box) |
| `endscript` | `stop` |

## Variables

As in VNDS:

- `setvar x = "text"`: quoted values are literal text (quotes removed).
- `setvar x = y` / `if x == y`: a bare word that names a variable means its
  value (Ever 17 and Never 7 rely on this); otherwise it is a number or text.
- `$name` or `{$name}` in text and choices shows the variable's value.
- `$name` in a file name (`bgload {$strS[1000]}.png`, `setimg $face 0 0`,
  `sound $voice`) picks the file at run time. CLANNAD and Kanon choose most of
  their pictures this way. The converter rewrites the names assigned to such
  variables to the converted files, and converts every file they can name.

## Endings

VNDS games go back to their own title script (`jump main.scr`, sometimes
with a label such as `Menu`) when a route ends. The converter turns that into the end of the story, so 3DVN counts the
ending and returns to its own title screen, with Continue, Load and the
other menus. Pass `--keep-main-loop` to keep the original jump instead (the
game's own title menu then comes back, as on a DS).

A jump that leads only to other jumps in a circle (Kanon does this after an
ending, which freezes VNDS on the last screen) also ends the story.

## Limits

The converter prints a summary of everything it had to skip or approximate.
Almost all of it in the tested games is mistakes in the games' own scripts
(misspelled commands, files named differently from the script), which the
VNDS player skips too.

- `random` has no equivalent yet: the variable is set to its low value.
- `setvar ~` (clear all variables) is skipped.
- Choices with more than 8 options are split into pages of 7 with a
  "More..." option. More than 8 sprites on screen: the oldest is hidden.
- Missing images or audio are skipped (a missing background becomes black).
- Text longer than about 400 bytes is split into several text boxes (3DVN
  boxes hold 512 bytes).
- A picture that would exceed the 3DS's 1024x1024 limit (a tall scrolling
  image in True Remembrance) is shrunk to fit, with a warning.
- Games with more than 128 variables (Kanon has about 950) need this
  version of 3DVN or later on every device: their saves carry the extra
  variables in an added block that older builds don't read.

## Never 7 from "the end of infinity" (n7e.love)

For Never 7, `import_never7.py` converts "Never 7 - the end of infinity" for LÖVE
(`n7e.love`) on its own; the VNDS release is not needed (to convert that one alone, use
import_vnds.py as for any VNDS game). It reads the .love file in place:

    python3 tools/import_never7.py n7e.love games/never7 \
        [--music pc|psp] [--no-movies] [--no-append] [--movie-size 320x240]

- the story from `script/*.dec` (the original engine's commands as the port decodes
  them), translated to VNDS commands and then converted like any VNDS game. Each
  message is one line, as in the original (the VNDS release splits them);
- Yuka's eight-part Append Story (`script/append/`), offered on the title menu;
- backgrounds and sprites from `upscale/` (1600x1200: scaled for the 3DS screen, full
  size in `hd/` for PC, Wii U and Android). Tall panning CGs are cut into top /
  `-middle` / `-bottom` pieces; sprites named `...n` / `...d` are the night / dusk
  tints the port draws;
- voices and sound effects from `sound/` (WAV, converted to Ogg Vorbis);
- music from `music/pc/` (default) or `music/psp/`. With `--steam <…/steamapps/common/Never7>`
  each track is replaced by the 2025 Steam remaster's (`Data/bgm_ogg.mpk`: 48 kHz,
  ~150 kbps, a newer master with wider stereo) when the two are recognised as the same
  piece by their sound, copied unchanged (24 of 25 tracks; one has no Steam equivalent).
  The voices stay the .love's uncompressed ones: Steam's are the same recordings, more
  compressed, with the line endings trimmed;
- movies: the opening, the five heroine clips and the eight endings with their songs,
  where the scripts play them. Their sound is Ogg Vorbis kept as `video/NAME.wav` (the
  engine reads either format there). About 290 MB; `--no-movies` leaves them out,
  `--movie-size` limits the picture.

The title shows the port's title picture, with a heroine added for each route cleared.
The port's debug menu is left out. import_vnds.py accepts a `video NAME` line in a
script (a 3DVN extension, not VNDS) for `video/NAME.3vv`.

## Checking the result

`tests/walk_story.c` plays a converted game many times with random choices
and lists script errors, missing files and the chapters reached:

    cc -O2 -Isource tests/walk_story.c source/gs.c source/wrap.c -o /tmp/walk_story
    /tmp/walk_story games/kanon 50

The `.3vs` files are plain text, described in `documentation.txt`, and can
be edited by hand.
