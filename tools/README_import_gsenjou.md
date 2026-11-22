# G-Senjou no Maou to 3DVN converter

`tools/import_gsenjou.py` builds a playable 3DVN game folder from G-Senjou no Maou's
KiriKiri scripts and assets. It reads either:

- **a Steam copy** (recommended), unpacked by `gsenjou-tools/extract_steam.py`: the
  1280x720 release, with voices when the copy has them and the adult scenes when the
  unrated patch is installed. `gsenjou-tools/build_from_steam.sh` runs the whole build;
  see `gsenjou-tools/README.md`.
- **the older 800x600 release's scripts and assets** laid out as `1-raw-scripts/` (UTF-16
  `.ks` files) plus `Img-Sound Files/background.zip`, `foreground.zip`, `sound.zip`.

It is tuned for this game, not a generic KiriKiri converter.

## Usage

```sh
python3 tools/import_gsenjou.py --steam steam-work --out games/g-senjou-no-maou --only g01
python3 tools/import_gsenjou.py --src "g-senjou no maou" --out games/g-senjou-no-maou
```

Useful options:

- `--no-assets` skip source image/audio conversion (the small generated UI skin
  is still written).
- `--voices` also convert voice clips (very large output).
- `--only g01 g02 ...` convert only selected entry chapters (and whatever
  they reference through jumps/calls).
- `--bgm-rate` / `--voice-rate` choose WAV sample rate.

## What it converts

- Chapter scripts into `ch/*.3vs`.
- Chapter/file transitions into the 3DVN `script` command.
- Branching choices (`@exlink`/`@showexlink`) into `menu`/`choice`.
- Main route flags (`f.*`) and conditions into `set`/`if`.
- Backgrounds/CGs from 800x600 down to 320x240, centered on 400x240.
- Character sprites cropped to alpha bounds and scaled to fit 3DS.
- BGM/SE (and optionally voice) OGG into mono PCM WAV via `ffmpeg`.
- Looping sound effects (`@se loop=true`) into `sfx NAME loop`, stopped by `@sse`,
  `@fose` or a new sound on the same buffer.
- Indented commands and comments (inside `@if` blocks) are read as commands, not text.
- With `--steam`: pictures scaled by 1/3 for the 3DS plus the originals in `hd/`, sprites
  placed from the game's `character_pos.tjs`, voices from each line's `[nm ... s=...]`.
- "Include adult scenes?" is only asked when the scripts have adult scenes.
- A G-Senjou-themed ADV/NVL textbox in `ui/`.

The output includes:

- `main.3vs`
- `ch/*.3vs`
- `characters.3vs`
- `bg/`, `sprites/`, `audio/`
- `ui/textbox.png` and `ui/nvl_textbox.png`
- `conversion-report.txt` with unsupported/approximated commands and missing assets

The supplied archives do not include the original message-window artwork. The
converter generates a dark translucent textbox based on the provided screenshot,
not extracted original assets. In ADV mode portraits overlap its left edge and
the dialogue moves aside when a portrait is visible. Speaker names remain visible
for clarity and the original PC's on-screen controls remain on the 3DS bottom
screen. The layout and colors are set in `game.3vc` and can be changed there or
by replacing the PNGs.

## Known approximations and omissions

- Many visual effects are dropped or simplified: camera pans, quake, cut-ins,
  snow, mosaic transforms, keyboard animation effects, etc. Speaker face
  overlays are converted from the supplied face images.
- Route/chapter system jumps to `title.ks`/`ex_evmode.ks` are flattened for a
  linear 3DVN flow.
- Some system flags (`sf.*`, `tf.*`) are ignored except where needed for
  branching.
- If source lines still contain Japanese text, they are reported.

Always check `conversion-report.txt` after each run.

## Gap fixes (latest run)

- `@quake`/`@squake` -> `shake`, `@flash`/`@flash_3times` -> `flash`.
- `ev_mouw_04a` (typo in g08.ks) is aliased to `ev_maou_04a`.
- `ev_fin` (not in the dump) is drawn as a simple "FIN" card.
- `keyenter` (not in the dump) is synthesized as a short click.
- A corrupted untranslated duplicate line in gm04 is dropped.
- `@cutin` and `@strans` are converted to sprites (`cut_*.png`) placed at the final
  position of the cut-in path (the slide-in animation is not reproduced); sprites
  use the visible artwork's origin instead of the dump's transparent padding,
  then are clipped to the original 800x600 scene bounds to prevent them bleeding
  into the widescreen side margins. `@strans` swaps the cut-in image. Explicit
  clip regions select the image before placement. `@eff` (zooming/floating
  images in g08 and g55) is still ignored.
- `@sse` becomes `sfx stop`; `@wse` becomes a wait as long as the last sound effect.
- The title-menu Extras script opens a CG gallery. It includes all 279 stills in
  `background/ev/`, reuses the converted full images in `bg/`, and generates
  matching JPEG thumbnails under `gallery/thumbs/`.
- `@mface name=X` / `@clear_mface` become a `face` sprite (`show face face_X at face`)
  built from `foreground/face/st_X_f.png`; it overlaps the ADV text box and is
  cleared on scene changes.
