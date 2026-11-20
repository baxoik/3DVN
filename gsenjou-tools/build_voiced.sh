#!/bin/sh
# Build the voiced, scene-fixed G-Senjou game for 3DVN from your own files.
# Settings (environment variables):
#   GAME     existing voiceless 3DVN game folder (from tools/import_gsenjou.py)
#   KS       folder of readable chapter scripts: g01.ks, g02.ks, ...
#   VOICES   folder of voice clips converted to WAV: <voice>.wav (see README.md)
#   HEADS    heads.txt (the getStNameHead table, see README.md)
#   OUT      output game folder (default: ./voiced/g-senjou-no-maou)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
: "${GAME:?set GAME}" "${KS:?set KS}" "${VOICES:?set VOICES}" "${HEADS:?set HEADS}"
OUT=${OUT:-voiced/g-senjou-no-maou}
CHECK="$HERE/check_scripts"
[ -x "$CHECK" ] || cc -O1 -o "$CHECK" "$ROOT/tests/check_scripts.c" "$ROOT/source/gs.c"
rm -rf "$OUT" && mkdir -p "$(dirname "$OUT")"
python3 -I "$HERE/add_voices.py" "$ROOT/tools/import_gsenjou.py" "$KS" "$GAME" "$VOICES" "$OUT"
python3 -I "$HERE/fix_scene.py" "$ROOT/tools/import_gsenjou.py" "$KS" "$HEADS" "$OUT"
python3 -I "$HERE/fix_leaked_commands.py" "$OUT"
python3 -I "$HERE/chapter_select.py" "$OUT"
python3 -I "$HERE/normalize_text.py" "$OUT"
"$CHECK" "$OUT/main.3vs"
for f in "$OUT"/ch/*.3vs; do cp "$f" "$OUT/_chk.3vs"; "$CHECK" "$OUT/_chk.3vs" >/dev/null || { echo "FAILED: $f"; exit 1; }; done
rm -f "$OUT/_chk.3vs"
du -sh "$OUT"
