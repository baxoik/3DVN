# Ever17 → 3DVN converter

Converts your own copy of **Ever17 -the out of infinity-** (English PC release,
optionally with the Himmel Edition patch applied) into a game for
[3DVN-wiiu](https://github.com/davidgith1/3DVN-wiiu) on Nintendo 3DS, Wii U and PC, with the
full voice acting, music and sound effects.

No game data is included or downloaded; everything is read from the installed game.

## Requirements

- Python 3 with Pillow and numpy
- This repository's `tools/make_pack.py` (and an engine with global variables and
  MS-ADPCM playback, as in 3DVN-wiiu)
- ffmpeg with libvorbis only for a smaller 3DS archive (`make_pack.py --ogg`)

## Use

All you need is one complete game folder: the installed English PC release, ideally
with the Himmel Edition patch applied (its improved text and
images are used). Then, from the 3DVN-wiiu checkout:

```sh
ever17-tools/build.sh "path/to/Ever17 -The Out of Infinity-"
```

That converts the game and packs both archives (about 4 minutes):

- `packs/ever17.3vp` (about 1.9 GB): PC and Wii U, with HD images and the game's own audio.
- `packs/ever17-3ds.3vp` (about 570 MB): 3DS, without HD images, audio as Ogg Vorbis.

Copy one to `<3dvn root>/games/ever17.3vp` (Wii U: `sd:/wiiu/3dvn/games/`). Settings:
`THREEDVN` (the 3DVN-wiiu checkout, default: the one this folder is in), `OUT` (default
`out/ever17`), `PACKS` (default `packs`). The Himmel Edition zip on its own is a patch,
not the whole game: apply it to the installed game first.

### Step by step

`build.sh` runs these:

```sh
python3 ever17-tools/e17_convert.py "path/to/Ever17 -The Out of Infinity-" out/ever17
python3 tools/make_pack.py out/ever17 packs/ever17.3vp --exclude images.json
python3 tools/make_pack.py out/ever17 packs/ever17-3ds.3vp --exclude hd \
  --exclude images.json --ogg --voice-quality 3 --music-quality 5
```

The game folder `e17_convert.py` writes:

| folder | contents |
| --- | --- |
| `bg/`, `sprites/` | backgrounds, CGs and characters at 0.4 scale (800x600 → 320x240) |
| `hd/bg/`, `hd/sprites/` | the same at the original size, lossless PNG, used by PC and Wii U |
| `audio/` | music and sound effects, voices in `audio/v/<group>/`: the original MS-ADPCM data, unchanged, in WAV files |
| `ch/*.3vs` | one script per original scene |
| `characters.3vs`, `main.3vs`, `game.3vc`, `endings.txt` | speakers, entry point, theme, endings checklist |

`images.json` (image sizes, for `--no-images` reruns) is a work file, left out of the
archives. `--no-audio` and `--no-images` skip those steps.

17.3vp`.

## How it works

- `kid_formats.py` reads KID engine data: LNK archives (`.dat`, LND compression),
  CPS/PRT images and WAF (MS-ADPCM) audio.
- `e17_script.py` decodes the SC3 script bytecode. Its output matches
  Ever17Converter's decoder line for line on all 82 scenes. Text is read as
  Windows-1252 (the English release is not Shift-JIS), with the font's custom
  glyphs for "Coco" and "You".
- `e17_convert.py` turns each scene into 3DVN script: text with speaker names and
  voices, backgrounds, sprites at their original positions, music, sound effects,
  choices, flags and jumps. Story variables become `v_xx`; the global flags
  (endings seen, final route unlocked) become `g_xx`, which 3DVN keeps across
  playthroughs. The ending selection scene (`y_ed`) is replaced by a small
  script, as Ever17Converter does.

Not converted: screen effects (shakes, flashes, transitions), the clock animation
(shown as a text line), and the movies.

## License

The files in `ever17-tools/` are GPLv2 (see `LICENSE` here). Parts are ported from
Ever17Converter by weeaboo, which is GPLv2. The rest of the repository is not covered
by this license.
