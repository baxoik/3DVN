#!/bin/sh
# Build Ever17 for 3DVN from one complete game folder, in one go.
#
#   ./build.sh "path/to/Ever17 -The Out of Infinity-"
#
# The folder is the installed English PC release, ideally with the Himmel Edition
# patch already applied (its improved text in script.dat and its graph/ images are
# used). Writes:
#   packs/ever17.3vp       PC and Wii U (HD images, the game's own audio)
#   packs/ever17-3ds.3vp   3DS (no HD images, audio as Ogg Vorbis)
#
# Settings (environment variables):
#   THREEDVN  the 3DVN-wiiu checkout whose tools/make_pack.py is used (default: the one
#             this folder is in)
#   OUT       game folder to build (default: out/ever17)
#   PACKS     where the archives go (default: packs)
set -e
GAME=${1:?usage: build.sh GAME_DIR}
HERE=$(cd "$(dirname "$0")" && pwd)
THREEDVN=${THREEDVN:-"$(dirname "$HERE")"}
OUT=${OUT:-out/ever17}
PACKS=${PACKS:-packs}
PACK="$THREEDVN/tools/make_pack.py"
[ -f "$PACK" ] || { echo "set THREEDVN to a 3DVN-wiiu checkout (no $PACK)"; exit 1; }
[ -f "$GAME/script.dat" ] && [ -f "$GAME/voice.dat" ] || { echo "$GAME: not a complete Ever17 folder (script.dat, voice.dat ...)"; exit 1; }
[ -f "$GAME/readme_patch.html" ] && echo "Himmel Edition patch found" || echo "no Himmel Edition patch: building the original release"

python3 -I "$HERE/e17_convert.py" "$GAME" "$OUT"
mkdir -p "$PACKS"
python3 -I "$PACK" "$OUT" "$PACKS/ever17.3vp" --exclude images.json
python3 -I "$PACK" "$OUT" "$PACKS/ever17-3ds.3vp" --exclude hd --exclude images.json --ogg \
	--voice-quality 3 --music-quality 5 --cache "$PACKS/ogg-cache"
ls -la "$PACKS/ever17.3vp" "$PACKS/ever17-3ds.3vp"
