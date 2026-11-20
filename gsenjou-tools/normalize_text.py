#!/usr/bin/env python3
"""Replace characters the 3DS system font may lack with plain equivalents.

usage: normalize_text.py GAME_DIR    (edits main.3vs, characters.3vs and ch/*.3vs in place)
Only displayed text is touched: dialogue, choice labels and character names.
Full-width ASCII forms become ASCII, the full-width / wave tilde becomes "~",
and a few rare characters get close stand-ins. Common punctuation the font has
(curly quotes, dashes, ellipsis, Latin-1 letters, Japanese marks) is kept.
"""
import glob, os, re, sys, collections

game = sys.argv[1]
SPECIAL = {"〜": "~", "～": "~", "ﾟ": "°", "ř": "r", "܈": "“"}


def fix(text, counts):
    out = []
    for ch in text:
        code = ord(ch)
        new = SPECIAL.get(ch)
        if new is None and 0xFF01 <= code <= 0xFF5E:   # full-width ASCII block
            new = chr(code - 0xFEE0)
        if new is not None:
            counts[ch] += 1
            ch = new
        out.append(ch)
    return "".join(out)


TEXT = re.compile(r'^(\s*(?:[A-Za-z0-9_]+: )?|\s*choice |character \S+ )(".*)$')
counts = collections.Counter()
files = [os.path.join(game, "main.3vs"), os.path.join(game, "characters.3vs")] + glob.glob(os.path.join(game, "ch", "*.3vs"))
for path in files:
    if not os.path.exists(path):
        continue
    with open(path, encoding="utf-8") as handle:
        lines = handle.read().split("\n")
    changed = False
    for i, line in enumerate(lines):
        m = TEXT.match(line)
        if m:
            new = m.group(1) + fix(m.group(2), counts)
            if new != line:
                lines[i] = new
                changed = True
    if changed:
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines))
print("normalized:", ", ".join(f"{k!r} x{v}" for k, v in counts.most_common()) or "nothing to change")
