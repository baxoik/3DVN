#!/bin/sh
# Play one chapter of a converted game in the Azahar libretro core with the
# AUTOTEST engine build: fast-forward, tap A at choices, stop at the next chapter.
# usage: run_chapter.sh CHAPTER        (results in runs/CHAPTER/: result.txt, test.log, shots/)
# Settings (environment variables):
#   GAME     converted game folder (with main.3vs, ch/, bg/, sprites/, audio/ ...)
#   CORE     azahar_libretro.so from an Azahar release (libretro, Linux x86_64)
#   ENGINE   3dvn.3dsx built with: make EXTRA_CFLAGS=-DAUTOTEST
set -e
CH=$1
HERE=$(cd "$(dirname "$0")" && pwd)
: "${GAME:?set GAME}" "${CORE:?set CORE}" "${ENGINE:?set ENGINE}"
W="$HERE/runs/$CH"
rm -rf "$W"
G="$W/Azahar/sdmc/3ds/3dvn/games/zz-test"
mkdir -p "$G"
for f in bg sprites audio ch ui gallery game.3vc characters.3vs extras.3vs; do
	[ -e "$GAME/$f" ] && ln -s "$GAME/$f" "$G/$f"
done
# Start the chapter the way the chapter-select menu does (same flag lines), if it has one.
{ echo 'set nonero = 0'
  awk -v t="script \"ch/$CH\"" '/^  choice /{body=""; next} /^    /{sub(/^    /,""); body=body $0 "\n"; if ($0==t) {printf "%s", body; exit}}' "$GAME/main.3vs"
} > "$G/main.3vs"
grep -q "script \"ch/$CH\"" "$G/main.3vs" || printf 'script "ch/%s"\n' "$CH" >> "$G/main.3vs"
printf 'wait 300\npress A\nwait 60\npress A\nwait 30\nffchapter 60000 1500\n' > "$W/script.txt"
timeout 1800 python3 -I "$HERE/drive.py" "$CORE" "$ENGINE" "$W" "$W/script.txt" > "$W/driver.out" 2>&1 || true
cp "$W/Azahar/sdmc/3dvn-test.log" "$W/test.log" 2>/dev/null || touch "$W/test.log"
rm -rf "$W/Azahar"
echo "$CH: $(cat "$W/result.txt" 2>/dev/null || echo crashed)"
