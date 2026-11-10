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
Images that would exceed the 3DS texture limit are reported and omitted in
preview mode rather than aborting the whole conversion. Supported partial
background crops can still be made from an oversized source.

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

### Local Mahoyo opening audio

A separate local copy under `../mahoyo/` has readable `bgm.xp3` and
`sound.xp3` indexes. The versions under `witch on the holy night/` failed
earlier extraction attempts; identical archive sizes do not imply identical
contents. With the separately installed arc_unpacker, the readable archives
can be extracted locally into two new directories:

```sh
arc_unpacker --dec=kirikiri/xp3 --plugin=noop --threads=1 --no-recurse \
  --out=NEW_BGM_DIRECTORY "/path/to/mahoyo/bgm.xp3"
arc_unpacker --dec=kirikiri/xp3 --plugin=noop --threads=1 --no-recurse \
  --out=NEW_SOUND_DIRECTORY "/path/to/mahoyo/sound.xp3"
```

Verify the extracted files: in this copy, audio entries named `.ogg` or
`.wav` actually contain AAC in MP4 containers. FFmpeg detects the contents
and the importer converts referenced tracks to 3DVN WAV files. Combine the
extracted audio with decoded opening images in a separate input directory,
checking for filename collisions, then run the importer with `--assets`.
Keep extracted commercial assets private.

`games/mahoyo-audio-preview` is a new local two-scene preview built from
`1-0.ks` and `1-1.ks` with the original NVL theme and recovered music and
sound effects. It contains 24 audio events and 15 referenced WAVs. It stops
before the `opening.ks` interlude and does not interpret the route
chart, and approximates or omits audio fades, delays, looping metadata, and
other effects. See its conversion report; it is not a full playable game.
Existing previews and saves are unchanged.

`games/mahoyo-chapter1-preview` is a separate, incomplete preview of
`1-2.ks` through `1-5.ks`, ordered according to the chart, with recovered
music and sound effects. It begins after the opening timeline, which has not
been converted, and it does not connect to the earlier preview or continue
to Chapter 2. Many foregrounds, effects, and transitions are omitted or
approximated; inspect its conversion report before using it. It is not a
faithful or fully playable conversion.

`games/mahoyo-through-chapter2` joins the opening scenes and Chapter 1 to a
new Chapter 2 diagnostic preview (`2-1.ks` through `2-5.ks`). The opening
timeline and the chapter-break menu are skipped with notices. Foregrounds,
effects, timing, and some images remain missing or approximate; it has not
been tested on a 3DS or emulator. See its conversion report.

`games/mahoyo-main-story-diagnostic` extends that approach across all 120
patch scenes on the chart's main path, through `d-9.ks`. It omits the opening
timeline and 13 chapter-break interludes and does not include bonus content.
The report lists 55,679 issues, including missing images, unsupported
foregrounds and effects, and approximated timing. Chart order alone does not
prove that every scene's dialogue or behavior was converted correctly.
It is a large local diagnostic (about 818 MiB), not a finished game, and its
memory use and playback have not been tested on a 3DS or emulator.

To build a new chart-ordered diagnostic from local assets, use:

```sh
python3 tools/build_mahoyo_story_preview.py \
  "witch on the holy night/data.xp3" "witch on the holy night/patch.xp3" \
  games/mahoyo-new-preview --assets /path/to/decoded-assets \
  --theme games/mahoyo-nvl-pages
```

The output directory must be new. The builder checks that chart tracing
reaches its end, retains the chart's scene order, and places notices where the
unconverted `opening.ks` and `teatime.ks` occur. It requires the extracted
images and audio to be available as uniquely named files in the asset
directory, and uses the existing theme's `game.3vc` and `ui` directory. It
does not interpret scene calls or make unsupported effects faithful; check the
generated report. Use `--entry` to select a different ZIP chart entry, or
pass an extracted chart directly. Keep commercial assets private.

`games/mahoyo-charted-preview` was built with this tool using the same local
assets as the previous diagnostic. It retains the same visual and audio
limitations but places notices at the omitted opening and each chapter break.
Its full script compiles on the host; playback on a 3DS or emulator remains
untested.

`games/mahoyo-recovered-preview` is another separately built diagnostic with
five exact-name images recovered from the local patch archive. Compared with
the previous charted preview, its report contains 669 rather than 698 missing
image references. It has the same major presentation limitations and has not
been tested on a 3DS or emulator; prior previews and saves are unchanged.

Some foreground images have been recovered as TLG files. The separate
`tools/decode_kirikiri_tlg.py` utility can decode selected files locally;
see `tools/README_decode_kirikiri_tlg.md`. Decoding does not reproduce the
game's layer placement or animation, and those foregrounds are not yet
integrated into the Chapter 1 preview.

The local Mahoyo chart can be inspected with `tools/inspect_mahoyo_chart.py`.
For a ZIP-format archive containing both chart entries, choose the main chart:

```sh
python3 tools/inspect_mahoyo_chart.py "witch on the holy night/data.xp3" \
  --entry others/main.gc --initial-playthrough
```

For a genuine XP3 archive, extract it locally with
`arc_unpacker --dec=kirikiri/xp3 --plugin=noop --threads=1 --no-recurse -o DIR data.xp3`
(the file is `others/main.gc`, KiriKiri zlib-packed UTF-16). The default ZIP
entry is `override/main.gc`; `--entry` selects an alternative. The tool parses
chart literals without executing TJS. By default it traces only unambiguous flow and
dumps the whole chart; `--initial-playthrough` prints just a fresh-game trace.

That trace follows the original tracer's `next()` logic (read from the game's
own `tracerplugin.tjs`): apply the block's flag operation, then follow the
route. It interprets only `name=N`, `name+=N` and `name=name<N?N:name`
operations and stops on multiple routes, conditions, choices, or unknown fields. For the
local game it reaches the end without any stop: 72 chart blocks, 134 scene
entries (121 distinct story scenes plus 13 `teatime.ks` interludes) from
`1-0.ks` to `d-9.ks`, with no player choices. Choices exist only in the
separate `wik_*` bonus page. The order, including `opening.ks` between `1-1.ks`
and `1-2.ks`, is chart order only; it does not verify the scene scripts, and
extras (`1.5-*`, `7-ex`, `8-ex`, `8.5`, `nz*`, `sp*`) are not on this path.

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

### Experimental Mahoyo foreground composition

`@fg`, `@chgfg`, `@movefg` and `@clfg` are implemented by the game's own
`fgplugin.tjs`. In scenes 1-0 to 1-5 they total about 530 `fg`, 56 `chgfg`, 9
`movefg` and 2 `clfg` calls. Their result depends on the back/fore page and
implicit `backlay`, the later `@trans`, layer `index`, per-call effects (`type`,
`effect`, blur, zoom, rotate, contrast), `partbg` targets and `|variant`
character images, and many calls build several layers at once. By default
these tags remain unsupported.

An opt-in `--compose-foreground` mode uses full-resolution `--assets` and
`--source-size` to flatten a limited subset into static backgrounds. It
models plain `fg` layers with literal center, vcenter, index, opacity and
positive zoom, `clfg`/`clall`, and selected back-page transitions. Positioning,
fade timing and page behavior are approximations, and each emitted
composition is reported. It is not a general KiriKiri layer renderer.
Effects, blur, rotation, blend types, `chgfg`, `movefg`, animations, TLG
sprite variants, unknown bases, equal-index ordering, and many other cases
remain unsupported and are reported. Do not use thumbnail assets with this
mode. A Chapter 1 test using the current local asset set emitted only one
composite; character sprites still require their original placement metadata.

Convert only files you are authorized to use. The tool does not include game
assets or distribute converted game content.

## Mahoyo opening timeline (inspection only)

`scenario/opening.ks` in the local `data.xp3` (a ZIP) is not a video. It plays a
serialized Kirikiri timeline (`@playtimeline scene=4`, or `scene=7` when
`cf.speffect` is false, with `next=197250`) of layer, BG, BGM and SE objects.
No movie file exists in the local archives' ZIP indexes. Summarize it with:

```sh
python3 tools/inspect_mahoyo_opening_timeline.py "path/to/data.xp3" [--json]
```

The tool reports objects, keyframe counts/time ranges and candidate image
names inferred from an unverified keyframe offset. It does not render, decode
easing or layer semantics, or produce a
`.3vv`; a faithful opening needs a renderer for these keyframes or a screen
capture from the game itself, which `tools/mk3vv.py` can then convert.

## Recovering exact-name missing images

`tools/recover_mahoyo_missing_images.py` scans a conversion report and local
image directories or ZIP-format archives for exact, case-insensitive asset
stems. It copies supported PNG/JPEG/BMP images and uses the separate
`tools/decode_kirikiri_tlg.py` utility for matching TLG files. It does not
guess aliases or extract proprietary XP3 archives. Ambiguous names are listed
and skipped; unmatched names are preserved in `recovery-report.txt`.

For the local main-story diagnostic, the `patch2.xp3` ZIP contains five exact
matches among the 245 normalized missing asset stems. To recover them without
changing the existing diagnostic or source archive:

```sh
python3 tools/recover_mahoyo_missing_images.py \
  games/mahoyo-main-story-diagnostic/conversion-report.txt \
  games/mahoyo-main-story-recovered-assets \
  --source "witch on the holy night/patch2.xp3" \
  --decoder /path/to/arc_unpacker
```

The current recovery produces five original-size assets (one `bg` image, two
`ev` event images and two `im` images), covering 29 of 698 missing-image
report occurrences. The report contains 246 distinct raw storage strings;
two share an importer-normalized stem, leaving 245 unique lookup names. The
other 240 names have no exact match in this archive, and no names were
ambiguous. The output is a separate asset directory with its own
`recovery-report.txt`; recovered images are not integrated into or used to
rewrite the diagnostic preview. Keep archives and recovered commercial
content local.
