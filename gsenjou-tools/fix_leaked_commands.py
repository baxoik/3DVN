#!/usr/bin/env python3
"""Turn KiriKiri commands that leaked into dialogue back into script, in a converted
G-Senjou game.

usage: fix_leaked_commands.py GAME_DIR [--steam]   (edits GAME_DIR/ch/*.3vs in place)

Older imports treated indented KiriKiri lines as text (tools/import_gsenjou.py now
handles them), so the "no adult scenes" branch of each H-scene subroutine showed
lines like "@return", "@jump storage=..." and "; comment" as dialogue and then fell
through into the scene anyway. Here:
  "@return"           -> return            (the branch skips the scene again)
  "@fobgm ..."        -> music stop fade 1 (fade out the music)
  "@jump ... ex_evmode.ks ..." and "; ..." -> removed (gallery mode, comments)
  [ font italic="true" ] ... [ resetfont ]  -> [i] ... [/i] (3DVN italics; the spaced
                          form slipped past the importer's tag handling)
Any other "@..." text line is reported and left alone.

Character spacing: the original places standing sprites at fixed fractions of the screen
width (macro.ks: stBaseLL = width/10, stBaseL = width/4, C = width/2, R and RR mirrored),
i.e. 80/200/400/600/720 on an 800-wide picture. tools/import_gsenjou.py uses
100/250/400/550/700, so characters stood too close together. "show <slot> ... at X Y" is
shifted by the difference at 0.4 scale: sll -8, sl -20, sr +20, srr +8 (sc unchanged).
Run once per build on a fresh import (the shift is not idempotent). A build from the Steam
release (--steam) already uses the original positions, so it is not shifted.
"""
import os, re, sys

game = sys.argv[1]
respace = "--steam" not in sys.argv[2:]
TEXT = re.compile(r'^(\s*)"(.*)"\s*$')
counts = {"return": 0, "music fade": 0, "removed": 0, "font tags": 0, "sprites respaced": 0}
SLOT_SHIFT = {"sll": -8, "sl": -20, "sr": 20, "srr": 8}
SHOW_AT = re.compile(r'^(\s*show (sll|sl|sr|srr) \S+ at )(-?\d+)( -?\d+.*)$')
FONT_TAG = re.compile(r'\[\s*(font|resetfont)\b([^\]]*)\]')


def font_tag(match):
    if match.group(1) == "resetfont":
        return "[/i]"
    return "[i]" if "italic" in match.group(2) else ""
unknown = []
for name in sorted(os.listdir(os.path.join(game, "ch"))):
    if not name.endswith(".3vs"):
        continue
    path = os.path.join(game, "ch", name)
    lines = open(path, encoding="utf-8").read().split("\n")
    out = []
    for line in lines:
        shown = SHOW_AT.match(line)
        if shown and respace:
            line = f"{shown.group(1)}{int(shown.group(3)) + SLOT_SHIFT[shown.group(2)]}{shown.group(4)}"
            counts["sprites respaced"] += 1
        if "font" in line and line.lstrip().startswith(('"', "choice")) or re.match(r'^\s*[a-z_]+: "', line):
            cleaned, n = FONT_TAG.subn(font_tag, line)
            if n:
                counts["font tags"] += n
                line = cleaned
        m = TEXT.match(line)
        body = m.group(2).replace('\\"', '"').strip() if m else ""
        if m and body.startswith("@return"):
            out.append(m.group(1) + "return")
            counts["return"] += 1
        elif m and body.startswith("@fobgm"):
            out.append(m.group(1) + "music stop fade 1")
            counts["music fade"] += 1
        elif m and (body.startswith(";") or (body.startswith("@jump") and "ex_evmode" in body)):
            counts["removed"] += 1
        else:
            if m and body.startswith("@"):
                unknown.append(f"{name}: {body[:60]}")
            out.append(line)
    if out != lines:
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("\n".join(out))
print("leaked commands fixed: " + ", ".join(f"{k} {v}" for k, v in counts.items()))
for line in unknown:
    print("  left alone:", line)
