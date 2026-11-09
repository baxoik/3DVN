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

`SOURCE` is the VNDS game folder. It must contain `script/main.scr` and
normally `background/`, `foreground/` and `sound/`. `info.txt` is used for
the title if present. `OUTPUT` is the new game folder to create.

Example:

    python3 tools/import_vnds.py ~/vnds/mygame games/mygame --speakers

Then copy `games/mygame` into the `3dvn/games/` folder on your SD card
(next to the other games) and pick it from the game list.

## Options

| Option | Meaning |
| --- | --- |
| `--source-size WIDTHxHEIGHT` | Set the game's original coordinate canvas explicitly, e.g. `--source-size 1920x1080`. |
| `--scale S` | Override the automatic uniform scale, e.g. `--scale 1` keeps images at their original size. Sprite positions are adjusted to match. |
| `--speakers` | Treat lines like `Name: some text` as dialogue spoken by `Name`. Off by default, because ordinary narration that happens to contain a colon would be affected. |
| `--title T` | Game title (default: from `info.txt`, else the folder name). |
| `--no-audio` | Skip audio conversion. |
| `--ffmpeg PATH` | Use this ffmpeg program. |
| `--music-rate HZ` | Music sample rate, default 32000. |
| `--sfx-rate HZ` | Sound effect sample rate, default 22050. |

Audio is converted to mono 16-bit WAV. WAV files are large, so lower the
rates (for example `--music-rate 22050`) if SD space is a concern.

## Image sizes and positioning

The converter fits the source canvas inside the 400x240 top screen without
stretching, centered horizontally and vertically. A 256x192 DS game becomes
320x240; a 1920x1080 game becomes 400x225. Sprites and their coordinates use
the same scale and offset as the canvas.

By default, the canvas is inferred from the most common dimensions of the
background images referenced by the scripts. A tie favors the largest image.
The chosen size and scale are printed. If referenced backgrounds have
different sizes, the converter warns: check the game's actual coordinate
resolution and pass `--source-size` when needed. If no referenced background
can be found, it warns and assumes 256x192. For example:

    python3 tools/import_vnds.py ~/vnds/mygame games/mygame --source-size 1920x1080

The source size is the game's coordinate canvas, not each sprite's dimensions.
All images are scaled by the same factor; they are not independently fit to
the screen. Backgrounds smaller than the canvas are centered by 3DVN and may
need manual adjustment if the original game places them differently. The
converter stops with an error if a scaled image exceeds the engine's
1024x1024 texture limit; verify the source size or choose a smaller scale.
Pillow is required; images are not silently copied unscaled.

## What you get

- `main.3vs`: entry point; defines speakers and includes the other files.
- `s_NAME.3vs`: one file per VNDS script, with labels prefixed so they
  never clash.
- `bg/`, `sprites/`, `audio/`: converted assets.

## How commands are converted

| VNDS | 3DVN |
| --- | --- |
| `text` | narration (or dialogue with `--speakers`) |
| `bgload` | `bg` (fade if a time is given); hides sprites, as in VNDS |
| `setimg` | `show ... at X Y` (pixel coordinates) |
| `sound`, `music` | `sfx`, `music` (`music ~` stops music) |
| `choice a\|b` | `menu`; the pick is stored in `selected` (1, 2, ...) |
| `setvar`, `gsetvar` | `set` |
| `if`, `else`, `fi` | `if`, `else`, `end` |
| `goto`, `label`, `jump file.scr` | `jump`, `label` |
| `delay` | `wait` (VNDS counts 1/60 s) |
| `cleartext` | ignored (each text line is its own page) |
| `endscript` | `stop` |

## Limits

The script prints a summary of everything it had to skip or approximate.

- `random` has no equivalent yet: the variable is set to its low value.
- `gsetvar` (global variables) is treated as a normal per-save variable.
- A looping `sound` plays only once.
- Sounds, including voice clips, stop when the player advances past a text
  box; background music continues.
- `setvar ~` (clear all variables) is skipped.
- More than 8 choices: extras are dropped. More than 8 sprites on screen:
  the oldest is hidden.
- Missing images or audio are skipped (a missing background becomes black).
- Moving to a new script mid-game is supported, but VNDS games that rely
  on unusual script tricks may need hand editing of the `.3vs` files.

## Checking the result

Open the generated `.3vs` files in a text editor; they are plain text
described in `documentation.txt`. If the game fails to start, the 3DS
shows the file name and line of the error.
