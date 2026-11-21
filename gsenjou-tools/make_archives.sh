#!/bin/sh
# Make both G-Senjou archives from a voiced game folder (after build_voiced.sh):
#   1. make_hd.py: hd/ images and whole face portraits (it also replaces the small
#      portraits, which the 3DS archive uses, so it must run before packing either one);
#      make_loops.py: loop points of looping sound effects (audio/<name>.loop)
#   2. <name>-3ds.3vp: no hd/, audio converted to Ogg Vorbis (smaller, easier to decode)
#   3. <name>.3vp:     hd/ and the original Steam audio unchanged (PC and Wii U)
#
# Settings (environment variables):
#   GAME       voiced game folder (default: voiced/g-senjou-no-maou)
#   DECODED    original images decoded to PNG (tools/decode_tlg.py ... --compose)
#   ORIGINALS  folder with the original voice/, bgm/ and sound/ folders of your copy
#   OUT        where the archives go (default: packs)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
GAME=${GAME:-voiced/g-senjou-no-maou}
OUT=${OUT:-packs}
: "${DECODED:?set DECODED}" "${ORIGINALS:?set ORIGINALS}"
NAME=$(basename "$GAME")
mkdir -p "$OUT"
python3 -I "$HERE/make_hd.py" "$GAME" "$DECODED"
python3 -I "$HERE/make_loops.py" "$ORIGINALS/sound" "$GAME"   # loop points of looping sounds
python3 -I "$ROOT/tools/make_pack.py" "$GAME" "$OUT/$NAME-3ds.3vp" --ogg --exclude hd \
	--cache "$OUT/ogg-cache"
python3 -I "$ROOT/tools/make_pack.py" "$GAME" "$OUT/$NAME.3vp" --ogg --cache "$OUT/ogg-cache" \
	--audio-originals "$ORIGINALS/voice" --audio-originals "$ORIGINALS/bgm" \
	--audio-originals "$ORIGINALS/sound"
ls -la "$OUT/$NAME-3ds.3vp" "$OUT/$NAME.3vp"
