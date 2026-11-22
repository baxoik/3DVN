#!/bin/sh
# Build the Wii U release files (in the devkitpro/devkitppc container or with devkitPro):
#   platform/wiiu/make_release.sh VERSION       e.g. 1.0.0
# Writes release/:
#   3dvn-wiiu-VERSION.wuhb       for Aroma: copy to sd:/wiiu/apps/
#   3dvn-wiiu-VERSION-hbl.zip    for the Homebrew Launcher: unzip to the SD card's root
#                                (wiiu/apps/3dvn/ with 3dvn.rpx, meta.xml, icon.png,
#                                and an empty wiiu/3dvn/games/ for your games)
set -e
VERSION=${1:?usage: make_release.sh VERSION}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
[ -d build-wiiu ] || /opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -S platform/wiiu -B build-wiiu
cmake --build build-wiiu
OUT=release
STAGE="$OUT/stage"
rm -rf "$STAGE" && mkdir -p "$STAGE/wiiu/apps/3dvn" "$STAGE/wiiu/3dvn/games"
cp build-wiiu/3dvn.rpx "$STAGE/wiiu/apps/3dvn/"
cp platform/wiiu/hbl-icon.png "$STAGE/wiiu/apps/3dvn/icon.png"
sed -e "s/@VERSION@/$VERSION/" -e "s/@DATE@/$(date -u +%Y%m%d%H%M%S)/" platform/wiiu/meta.xml \
	> "$STAGE/wiiu/apps/3dvn/meta.xml"
cp build-wiiu/3dvn.wuhb "$OUT/3dvn-wiiu-$VERSION.wuhb"
rm -f "$OUT/3dvn-wiiu-$VERSION-hbl.zip"
(cd "$STAGE" && python3 -I -c "
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    for folder, dirs, files in os.walk('wiiu'):
        z.write(folder)
        for name in files: z.write(os.path.join(folder, name))
" "../3dvn-wiiu-$VERSION-hbl.zip")
rm -rf "$STAGE"
ls -la "$OUT"
