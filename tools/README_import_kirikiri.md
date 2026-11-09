# KiriKiri to 3DVN: experimental scene previews

This tool converts individual scenes or an explicitly ordered sequence,
not an entire KiriKiri game.
It was developed against the opening script of Witch on the Holy Night.
The output is a diagnostic preview, not a faithful or playable conversion.

## Requirements and usage

Use Python 3.9 or later. Give the tool an extracted script directory or a
ZIP-format archive containing `.ks` files. Some files named `.xp3` are
actually ZIP archives; genuine XP3 archives are not supported.

```sh
python3 tools/import_kirikiri.py SOURCE NEW_GAME_DIRECTORY --scene SCENE.ks
```

For example, to preview the first two page breaks of the local game's patch:

```sh
python3 tools/import_kirikiri.py "witch on the holy night/patch.xp3" \
  games/mahoyo-preview --scene 1-0.ks --pages 2 --preview
```

That example deliberately stops after two page breaks (six dialogue boxes in
this scene); it does not continue through the opening. To preview the rest
of the opening instead, omit `--pages` and choose a different output folder:

```sh
python3 tools/import_kirikiri.py "witch on the holy night/patch.xp3" \
  games/mahoyo-opening --scene 1-0.ks --preview
```

This still ends at the scene's first return, not at the end of the game.
Without extracted assets, unsupported background changes are omitted: the
most recent converted black or white background stays on screen.

For a diagnostic sequence, repeat `--scene` in the desired order:

```sh
python3 tools/import_kirikiri.py "witch on the holy night/patch.xp3" \
  games/mahoyo-story-preview --scene 1-0.ks --scene 1-1.ks --preview
```

The order here follows these two entries in the original game's route chart,
but the tool does **not** read that chart or interpret calls and branches.
Each scene is converted independently through its first return, then joined
without a stop between scenes. Use `--preview` to acknowledge that this may
skip data, decisions, or events and is not faithful playback. The report
identifies each scene's issues. `--pages` cannot be used with a sequence.
For this game the next chart entry is an opening interlude that requires
timeline and conditional support; it is not included in this example.

Add `--nvl-pages` to preserve line-break whitespace and emit `page` commands
at source `p`/`pg` waits. Set `nvl_accumulate 1` in the output game's
`game.3vc` to display those events as a growing NVL page. Without the option,
the original single-event preview behavior is retained. The converter still
approximates inline waits and other unsupported tags, which can create extra
dialogue stops; it does not yet distinguish line and page wait indicators.

### Extracting original-size images

The importer expects assets to have already been extracted and decoded. One
possible separate extractor is [arc_unpacker](https://github.com/vn-tools/arc_unpacker),
which supports XP3 and TLG and is licensed under GPL-3.0-or-later. Its
game-specific `mahoyoru` XP3 plugin is intended for the 2012 PC game, but
different editions or repacked archives may require different handling.

For example, one local copy's `bg.xp3` was extracted without decryption:

```sh
arc_unpacker --dec=kirikiri/xp3 --plugin=noop --threads=1 \
  --out=EXTRACTED_DIRECTORY "/path/to/game/bg.xp3"
```

This option is **not** a general rule for Mahoyo or other games. Verify the
results by opening them: an extractor can report saved files even if the
wrong decryption produces unreadable data. In this particular archive,
many files named `.png` actually contained WebP data. Pillow could read them
and re-save real PNGs at their stored dimensions. These originals still need
resizing for the 3DS, which the scene importer performs when it uses them.
They may be compressed or layered assets rather than a complete scene.

A replacement `image.xp3` in the same local copy also extracted with `noop`,
including full-resolution opening images. As with `bg.xp3`, some `.png`
entries actually contain WebP and must be decoded to real PNGs. Its `fg.xp3`
still did not extract successfully. The ZIP-format `patch2.xp3` also contained
some usable JPEGs. Verify your own files and keep all game assets private.

The local `games/mahoyo-animated` preview uses the extracted `txtwindow01.png`
message frame, resized to 384x224. The original `override/config.tjs` selects
that frame and specifies 162-pixel side and 69-pixel vertical margins on its
1024x576 canvas; the preview approximates these as 61 and 27 pixels. Its
`game.3vc` selects NVL for new preferences and disables the engine's extra
NVL border and dimming because the image already includes transparency.
Existing saved display preferences are not replaced. This is a visual preview,
not the original engine's accumulated multi-line message behavior. The
converter does not yet extract or generate these theme settings automatically.
The local `games/mahoyo-story-extended` preview also uses the original
40-frame line-wait glyph, decoded from `override/linebreak_a.tlg`. It is
displayed for every dialogue stop: the converter does not yet retain the
original distinction between line and page wait glyphs. The glyph remains
local game artwork and is not included in the conversion tool.

### Low-resolution thumbnail fallback

Some games include small preview images in an unencrypted ZIP-format archive.
The separate thumbnail tool can extract TLG5 entries named `t_NAME.tlg`
for backgrounds referenced in the scene. This is an opt-in approximation:
thumbnails are not the original backgrounds, transparency is discarded, and
sprites, animation and effects are not reproduced. A limited static partial
background approximation is available as described below.

On Linux with Wine, Pillow, and the bundled FSNConverter decoder:

```sh
python3 tools/extract_kirikiri_thumbnails.py "witch on the holy night/data.xp3" \
  /tmp/mahoyo-thumbnails --scripts "witch on the holy night/patch.xp3" \
  --scene 1-0.ks --decoder FSNConverter/tools/tlg2bmp.exe --wine wine
python3 tools/import_kirikiri.py "witch on the holy night/patch.xp3" \
  games/mahoyo-images --scene 1-0.ks --assets /tmp/mahoyo-thumbnails \
  --source-size 168x91 --scene-size 1024x576 --thumbnail-assets --preview
```

Both destination folders must be new. Use the dimensions printed by the
thumbnail tool as `--source-size` when thumbnails share one canvas size.
`--scene-size` is the original script coordinate canvas, not the thumbnail
size. The example uses 1024x576 for this game's opening.
If their dimensions differ, the tool reports them; using one scale for all
may be inaccurate. Missing or ambiguous entries are reported. This helper
uses the original bundled executable separately; it does not incorporate
FSNConverter source code. Windows users can omit `--wine`.

To extract every `t_*.tlg` thumbnail in a ZIP-format archive instead of just
the backgrounds named by one scene, use:

```sh
python3 tools/extract_kirikiri_thumbnails.py "witch on the holy night/data.xp3" \
  mahoyo-thumbnail-assets --all --decoder FSNConverter/tools/tlg2bmp.exe --wine wine
```

Omit `--scripts` and `--scene` with `--all`. This can take a long time: it
runs the external decoder separately for each image. Progress is printed
every 100 images. The destination is published only on success; if decoding
fails, inspect the error and retry with a new destination. The resulting
folder is a thumbnail asset library for the scene importer, not a converted
game or a replacement for original full-resolution images.

Choose a destination that does not already exist. The script will not
overwrite an existing game. It creates `main.3vs` and a
`conversion-report.txt` listing issues with source line numbers. Run the
game with 3DVN as usual.

By default, the tool refuses to write output if it encounters unsupported
commands or approximations. `--preview` explicitly permits incomplete output
for inspection; review the report before relying on it. `--pages N` stops
after N page breaks. Omit it to process each scene up to its first return.
Scripts with a UTF-16 BOM and UTF-8 scripts are detected; use
`--encoding CODEC` to override detection, for example `--encoding shift_jis`.

## Current coverage and limitations

- Converts text, line breaks, page breaks, literal millisecond waits, and
  basic black and white backgrounds. Long text is split to fit 3DVN.
- With `--assets DIRECTORY`, resolves simple literal `bg`, `play`, `se` and
  `playstop` commands against already-extracted assets. Supported images are
  PNG, JPEG and BMP; supported audio includes WAV, Ogg, MP3, AAC, M4A and
  FLAC. Asset names must be unique ignoring case and file extension.
  Supply the original canvas size with `--source-size WIDTHxHEIGHT` for
  images; Pillow is required. Images are scaled proportionally to fit the
  top screen using the same scale for each asset. The engine centers them.
  Audio conversion requires ffmpeg (on PATH, the `imageio-ffmpeg` package,
  or `--ffmpeg PATH`); music becomes 32 kHz mono WAV, sounds 22.05 kHz mono
  WAV. Converted sound effects must fit the engine's 2 MB limit.
  Literal `playstop time` with `nowait=1` becomes a nonblocking music fade-out;
  other playback and stop options may be approximated and are reported.
- A background marked `page=back` is held until `trans`. Literal `time`
  values and `rule=crossfade` become 3DVN background fades; `nowait=1`
  continues immediately, while waiting transitions and `wt` use `waitbg`.
  Unknown rules fall back to crossfade with a report entry. Waiting transition
  skip behavior, unsupported options and composited partial backgrounds
  remain approximations.
- The importer recognizes one specific staged, two-overlay `fgact` pattern
  from the Witch on the Holy Night opening and approximates it with a
  three-image `bgloop`. It requires all three extracted images and a
  1024x576 source canvas; thumbnail assets are not supported for this loop.
  Other `fgact` sequences, transformations, timing variations and layering
  remain unsupported and are reported. This is not a general KiriKiri
  animation interpreter.
  A fade into an animated loop uses only the prior and next base frames;
  the old overlays disappear and the new animation begins after the fade.
- A limited `partbg` with literal `width`, `height`, `center`, optional
  `srcleft`, `srctop` and `vcenter`, and explicit black or white `bgstorage`
  can be flattened into a static 400x240 image. This replaces the existing
  background instead of reproducing independent layers or transitions.
  For thumbnail assets, `--scene-size` is required: the importer assumes each
  thumbnail represents the entire original coordinate canvas and scales
  crop coordinates accordingly. This assumption and the default vertical
  positioning are approximations, not verified KiriKiri semantics; review
  the preview. Unsupported options are reported and not converted.
- Other layer and image placement options are not implemented.
- **Does not yet convert sprites, voice-specific commands, choices,
  branching, calls, general game-specific macros, arbitrary animation or TJS.** Unsupported
  commands and missing, unsupported or ambiguous assets are reported.
- A `return` ends the current converted scene; calls and their data are not
  interpreted. Complex scripts can therefore produce incomplete or
  out-of-order previews, including explicitly ordered scene sequences.
- This tool neither extracts XP3 archives nor decodes TLG images. It does not
  use or incorporate the GPL-licensed FSNConverter code. XP3 encryption,
  filenames and supported image formats vary between games and releases.
- The Witch on the Holy Night opening preview can use separately extracted
  full-resolution images, but audio, image positioning, game-specific
  layering and effects have not all been recovered or converted.

Convert only files you are authorized to use. The tool does not include game
assets or distribute converted game content.
