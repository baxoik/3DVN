#!/bin/sh
# Build G-Senjou no Maou for 3DVN from your Steam copy, in one go:
#   1. extract_steam.py    unpack the game's .xp3 archives, scripts and images (no Wine)
#   2. import_gsenjou.py   convert the scripts, images and sounds (--steam); with voices
#                          when your copy has voice.xp3
#   3. fix_scene.py, fix_leaked_commands.py, chapter_select.py, normalize_text.py, the
#      endings list, a check of every script, make_loops.py (looping sound effects)
#   4. tools/make_pack.py  packs/<name>-3ds.3vp (3DS) and packs/<name>.3vp (PC, Wii U)
#
# Settings (environment variables):
#   STEAM   the game's install folder (default: the usual Steam library on Linux)
#   WORK    where the unpacked copy goes (default: steam-work; reused if it is there)
#   OUT     game folder to build (default: voiced/g-senjou-no-maou)
#   PACKS   where the archives go (default: packs); PACKS= (empty) skips packing
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
STEAM=${STEAM:-"$HOME/.local/share/Steam/steamapps/common/G-senjou no Maou - The Devil on G-String"}
WORK=${WORK:-steam-work}
OUT=${OUT:-voiced/g-senjou-no-maou}
PACKS=${PACKS-packs}
NAME=$(basename "$OUT")
CHECK="$HERE/check_scripts"
[ "$CHECK" -nt "$ROOT/source/gs.c" ] || cc -O1 -o "$CHECK" "$ROOT/tests/check_scripts.c" "$ROOT/source/gs.c"

if [ ! -d "$WORK/png" ]; then
	python3 -I "$HERE/extract_steam.py" "$STEAM" "$WORK"
fi
rm -rf "$OUT" && mkdir -p "$(dirname "$OUT")"
python3 -I "$ROOT/tools/import_gsenjou.py" --steam "$WORK" --out "$OUT" --only g01
python3 -I "$HERE/fix_scene.py" "$ROOT/tools/import_gsenjou.py" "$WORK/data/scenario" "$WORK/data/alter/macro.ks" "$OUT"
python3 -I "$HERE/fix_leaked_commands.py" "$OUT" --steam
python3 -I "$HERE/chapter_select.py" "$OUT"
python3 -I "$HERE/normalize_text.py" "$OUT"
cp "$HERE/endings.txt" "$OUT/endings.txt"
"$CHECK" "$OUT/main.3vs"
for f in "$OUT"/ch/*.3vs; do cp "$f" "$OUT/_chk.3vs"; "$CHECK" "$OUT/_chk.3vs" >/dev/null || { echo "FAILED: $f"; exit 1; }; done
rm -f "$OUT/_chk.3vs"
python3 -I "$HERE/make_loops.py" "$WORK/sound" "$OUT"
du -sh "$OUT"
[ -n "$PACKS" ] || exit 0
mkdir -p "$PACKS"
ORIGINALS=""
for folder in voice bgm sound; do
	[ -d "$WORK/$folder" ] && ORIGINALS="$ORIGINALS --audio-originals $WORK/$folder"
done
python3 -I "$ROOT/tools/make_pack.py" "$OUT" "$PACKS/$NAME-3ds.3vp" --ogg --exclude hd --cache "$PACKS/ogg-cache"
# shellcheck disable=SC2086
python3 -I "$ROOT/tools/make_pack.py" "$OUT" "$PACKS/$NAME.3vp" --ogg --cache "$PACKS/ogg-cache" $ORIGINALS
ls -la "$PACKS/$NAME-3ds.3vp" "$PACKS/$NAME.3vp"
