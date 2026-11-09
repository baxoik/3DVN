# System-NNN investigation tools

The `gss` test game uses compiled `.spt` scripts, `.gpk`/`.gtb` image
archives, `.vpk`/`.vtb` voice archives, and `.wgq` music files. This is
an investigation, not a complete System-NNN converter.

The separate [VNTools](https://github.com/abunomaru/VNTools) project can
extract text from some SPT files offline. A local test extracted 1,665
strings from this game's `01.spt`. Those are string-table entries, not an
interpreted execution trace: their order does not establish story flow,
choices, image commands, or the start of the game. Do not treat that text
as a converted scene. VNTools also offers online translation features;
do not use those if game text must remain private.

## Inspecting SPT records

The Python 3 diagnostic reader inspects the observed SPT variant with
the default XOR key. It reports physical record offsets (in 32-bit words),
raw operands, message references, selection operands, script-call
references, nested block boundaries, film-table targets, and absolute GOTO
targets. It checks bounds
and reports unrecognized or inconsistent table record extents. It uses the
film-table record to identify the instruction-region boundary; files with
other layouts or encryption are rejected.

```sh
python3 tools/inspect_systemnnn_spt.py gss/spt/01.spt > spt-report.json
python3 tools/inspect_systemnnn_spt.py --include-text gss/spt/01.spt > spt-text-report.json
```

By default the report does not resolve string contents; raw operands are
still reported, so even default reports should be kept private.
`--include-text` includes
locally decoded CP932 text; invalid CP932 is reported and preserved as
hex instead of being replaced. This game's `sjis_ext.bin` provides nine
extra glyph mappings for byte pairs `81 01` through `81 09`. Pass it
explicitly to decode those characters:

```sh
python3 tools/inspect_systemnnn_spt.py --include-text \
  --sjis-ext gss/sjis_ext.bin gss/spt/01.spt > spt-text-report.json
```

The extension table is specific to the game; it is not guessed for other
games. Invalid byte sequences remain errors. Decoding the bytes does not
interpret inline text controls or establish how the original game renders
text. Reports can contain
game content and should be kept private. The shell's `>` overwrites an
existing report: pick a new filename if you want to keep an older one.

The listing is **not an execution trace**, and is not a playable game:
branches, conditions, selection options, calls, and game-specific
commands are not evaluated. The selection string indices are raw
operands, not guaranteed to all be visible choice labels. The film base
index and film targets do not establish which scene the game starts in.
Static inspection of this game's `sys.spt` shows script calls to `op1`
and then `01`; the prologue's choices and complete runtime path have not
yet been traced.
In `01.spt`, some text pointers lie outside their table record's declared
extent despite remaining within the file; the diagnostic reports this
instead of assuming those record lengths define the text data bounds.

## Tracing a bounded path

`trace_systemnnn_spt.py` follows a small verified subset of records from
an explicitly chosen word offset: message references, a variable/literal
equality condition, selection CASE values, blocks, subroutine calls and
returns, literal assignments to variables 100 and above, and GOTO. Assignments
update the trace's variable state; their possible graphics side effects are
not modeled and are explicitly marked in trace events. Dialogue is assumed
to be advanced by the player; its
display and inline codes are not simulated. Unknown expressions, commands
and engine callbacks stop the trace instead of being skipped. Variables are
never assumed to be zero; provide their values explicitly. Start offsets and
variables are assumptions unless verified from actual game state. Tracing
from an arbitrary offset is not a trace of a complete new game.
Film/story-name callbacks are recorded as metadata. `ENDKOMA` and `ENDIF`
are recorded and resumed as in the reference scheduler; `NEXT` stops with
`next_script` rather than following the next physical record. The reference
implementation's film-type-time callback does nothing: the tracer records
that assumption, which might not hold for another engine version. It does
not simulate presentation, timing, skip state or scene-mode scheduling.
Recognized but unimplemented callbacks are named in the final event, with
their raw operands; their effects are not applied. In the local opening,
case 1 reaches `clear_all_effects` at word 392 and stops there, while case
2 stops at the `NEXT` handoff at word 98.

For example, static investigation found an opening condition at `op1.spt`
word 51. The game may retain variable 200 across playthroughs, so both
outcomes must be considered:

```sh
python3 tools/trace_systemnnn_spt.py gss/spt/op1.spt --start-word 51 --var 200=1
python3 tools/trace_systemnnn_spt.py gss/spt/op1.spt --start-word 51 --var 200=1 --case 1
python3 tools/trace_systemnnn_spt.py gss/spt/op1.spt --start-word 51 --var 200=1 --case 2
python3 tools/trace_systemnnn_spt.py gss/spt/op1.spt --start-word 51 --var 200=0
```

`--case` supplies the numeric case value passed to the original script,
not a verified mapping from the displayed UI option. Repeat it for later
selections. Add `--include-text --sjis-ext gss/sjis_ext.bin` when text is
needed. Each JSON report includes a stop status and word offset. The
command exits 0 only for a script `END`, 2 for a deliberate stop such as a
`NEXT` script handoff, unknown instruction, missing state or missing case, and 1
for an invalid file or arguments. This is diagnostic work, not a 3DVN
game converter; graphics, audio, choices, and text rendering are not
playable yet.

## Text rendering investigation

The original PRINT instruction carries a message-table index and a message
serial, not an explicit speaker name. `op1.spt` word 1411 is a candidate
dialogue message with a CRLF and a separate following ENDKOMA instruction;
the bounded tracer cannot yet reach it from the opening. It is not safe to
assume that each CRLF is a speaker separator or a page advance, or to infer
the name from the nearby voice command. Inline command, speaker, and wait
semantics need further verification before producing a playable preview.

## Converting WGQ music

Python 3 and ffmpeg are required. The helper accepts only `PACKTYPE=6`
WGQ files with an Ogg stream after the 64-byte wrapper; other formats are
rejected. It writes 16-bit mono PCM WAV at 32 kHz by default, suitable
for 3DVN's streaming music player. Set `--rate` to a value from 4000 to
48000 or `--ffmpeg` to a specific ffmpeg executable if needed.

```sh
python3 tools/extract_systemnnn_music.py NEW_OUTPUT_DIRECTORY \
  gss/wgq/gss01_1.wgq gss/wgq/gss01_2.wgq
```

The output directory must not exist; conversion is staged and published
only when every input succeeds. Filenames are preserved with `.wav`
extensions. These files are large. Use only game assets you are authorized
to use, and keep the original and converted game content private.

`games/gss-audio-test` is a local sound test using one track, not a story
conversion. Copy it and the 3DVN executable to the device as usual.

## Extracting GPK images

The Python 3 and ffmpeg helper reads a GPK archive and its matching GTB
index and extracts standard JPEG (PACKTYPE=5) and PNG (PACKTYPE=8)
resources. It checks each payload with ffmpeg against its header dimensions:

```sh
python3 tools/extract_systemnnn_images.py NEW_OUTPUT_DIRECTORY gss/dwq/bg0.gpk
python3 tools/extract_systemnnn_images.py NEW_OUTPUT_DIRECTORY gss/dwq/bg0.gpk --entry bg000000
```

The output directory must not already exist. Files are staged and published
only after all supported selected entries succeed. Whole-archive extraction
reports each skipped unsupported entry on stderr; explicitly selected
unsupported entries are errors. Alpha-mask and BMP resources are not yet
supported. A local `bg0.gpk` test decoded 153 JPEGs and reported 10
unsupported resources. Opening-specific assets and compositing are not yet
verified; keep extracted assets private.

## Still needed

Execution and conversion of script control flow, branches and choices,
text control codes, remaining graphic formats, sound effects, voices, and
source-specific timing are not yet implemented. The original engine may
use audio loop points; WAV music in
3DVN currently loops the entire track. Full image and audio archive support
has not yet been verified for this game.
