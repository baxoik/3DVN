#!/usr/bin/env python3
"""Turn KiriKiri commands that leaked into dialogue back into script, in a converted
G-Senjou game.

usage: fix_leaked_commands.py GAME_DIR   (edits GAME_DIR/ch/*.3vs in place)

Older imports treated indented KiriKiri lines as text (tools/import_gsenjou.py now
handles them), so the "no adult scenes" branch of each H-scene subroutine showed
lines like "@return", "@jump storage=..." and "; comment" as dialogue and then fell
through into the scene anyway. Here:
  "@return"           -> return            (the branch skips the scene again)
  "@fobgm ..."        -> music stop fade 1 (fade out the music)
  "@jump ... ex_evmode.ks ..." and "; ..." -> removed (gallery mode, comments)
  [ font italic="true" ] ... [ resetfont ]  -> removed from dialogue (no italics in 3DVN;
                          the spaced form slipped past the importer's tag handling)
Any other "@..." text line is reported and left alone.
"""
import os, re, sys

game = sys.argv[1]
TEXT = re.compile(r'^(\s*)"(.*)"\s*$')
counts = {"return": 0, "music fade": 0, "removed": 0, "font tags": 0}
FONT_TAG = re.compile(r'\[\s*(?:font|resetfont)\b[^\]]*\]\s?')
unknown = []
for name in sorted(os.listdir(os.path.join(game, "ch"))):
    if not name.endswith(".3vs"):
        continue
    path = os.path.join(game, "ch", name)
    lines = open(path, encoding="utf-8").read().split("\n")
    out = []
    for line in lines:
        if "font" in line and line.lstrip().startswith(('"', "choice")) or re.match(r'^\s*[a-z_]+: "', line):
            cleaned, n = FONT_TAG.subn(lambda t: " " if t.group(0).endswith(" ") else "", line)
            if n:
                counts["font tags"] += n
                line = re.sub(r'  +', ' ', cleaned)
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
