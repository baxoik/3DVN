#!/usr/bin/env python3
"""Add a chapter-select menu to a converted G-Senjou game's main.3vs.

usage: chapter_select.py GAME_DIR
The adult-scene question stays first; then the player can start from the
beginning or jump to any chapter. Menus have no heading line (it would need an
extra press and disappears when the choices show). Menus hold at most 8 choices, so long lists
are split into pages of 7 plus "Back".
"""
import os, sys

game = sys.argv[1]
chapters = {os.path.splitext(f)[0] for f in os.listdir(os.path.join(game, "ch")) if f.endswith(".3vs")}

def title(stem):
    names = {"gt": "Tsubaki", "gk": "Kanon", "gm": "Mizuha"}
    if stem[:2] in names:
        rest = stem[2:]
        return f"{names[stem[:2]]} route, " + ("ending" if rest == "ed" else f"part {int(rest)}")
    return f"Chapter {int(stem[1:])}"

main_story = sorted((c for c in chapters if c[0] == "g" and c[1:].isdigit()), key=lambda c: int(c[1:]))
routes = []
for prefix, name in (("gt", "Tsubaki route"), ("gk", "Kanon route"), ("gm", "Mizuha route")):
    parts = sorted(c for c in chapters if c.startswith(prefix) and c[2:].isdigit())
    if prefix + "ed" in chapters:
        parts.append(prefix + "ed")
    if parts:
        routes.append((name, parts))

# Story flags a player would have earned (taking every affection choice) before
# each chapter, so jumping in by chapter select doesn't hit a flag-gated bad end.
import re
FLAG_SET = re.compile(r"^\s*set (flag_[a-z]+) \+= (\d+)")
earned = {}
for stem in chapters:
    for line in open(os.path.join(game, "ch", stem + ".3vs"), encoding="utf-8"):
        m = FLAG_SET.match(line)
        if m:
            earned.setdefault(stem, {}).setdefault(m.group(1), 0)
            earned[stem][m.group(1)] += int(m.group(2))
BRANCH = {"gt": "g23", "gk": "g34", "gm": "g42"}

def story_before(stem):
    """Chapters played before `stem` on the way to it."""
    if stem[:2] in BRANCH:
        prefix = stem[:2]
        branch = int(BRANCH[prefix][1:])
        route = [c for c in sorted(chapters) if c.startswith(prefix) and c[2:].isdigit()]
        before_route = route if stem == prefix + "ed" else route[:route.index(stem)]
        return [c for c in main_story if int(c[1:]) <= branch] + before_route
    return [c for c in main_story if int(c[1:]) < int(stem[1:])]

def start(stem):
    flags = {}
    for c in story_before(stem):
        for name, value in earned.get(c, {}).items():
            flags[name] = flags.get(name, 0) + value
    return "\n".join([f"set {k} = {v}" for k, v in sorted(flags.items())] + [f'script "ch/{stem}"'])

out, counter = [], [0]

def label():
    counter[0] += 1
    return f"cs_{counter[0]}"

def page_menu(name, items, back):
    """items: list of (text, line). Emits paged menus; returns first label."""
    pages = [items[i:i + 7] for i in range(0, len(items), 7)] if len(items) > 8 else [items]
    labels = [label() for _ in pages]
    for n, (lab, page) in enumerate(zip(labels, pages)):
        out.append(f"label {lab}")
        out.append("menu")
        for text, line in page:
            out.append(f'  choice "{text}"')
            out.extend(f"    {part}" for part in line.split("\n"))
        if n + 1 < len(pages):
            out.append('  choice "More..."')
            out.append(f"    jump {labels[n + 1]}")
        out.append('  choice "Back"')
        out.append(f"    jump {back}")
        out.append("end")
    return labels[0]

def group_menu(name, stems, back, size):
    groups = [stems[i:i + size] for i in range(0, len(stems), size)]
    me = label()
    pending = []
    entries = []
    for g in groups:
        lab = label()
        entries.append((f"Chapters {int(g[0][1:])}-{int(g[-1][1:])}", f"jump {lab}"))
        pending.append((lab, g))
    out.append(f"label {me}")
    out.append("menu")
    for text, line in entries:
        out.append(f'  choice "{text}"')
        out.append(f"    {line}")
    out.append('  choice "Back"')
    out.append(f"    jump {back}")
    out.append("end")
    for lab, g in pending:
        first = page_menu("Choose a chapter", [(title(s), start(s)) for s in g], me)
        out.append(f"label {lab}")
        out.append(f"jump {first}")
    return me

main_path = os.path.join(game, "main.3vs")
with open(main_path, encoding="utf-8") as h:
    original = h.read().rstrip("\n").split("\n")
if any(line.startswith("label cs_top") for line in original):
    sys.exit("chapter select already present")
assert original[-1] == 'script "ch/g01"', original[-1]
head = original[:-1]

halves = [main_story[:28], main_story[28:]]
top_entries = [("From the beginning", 'script "ch/g01"')]
body_start = len(out)
half_labels = []
for half in halves:
    if half:
        half_labels.append((f"Chapters {int(half[0][1:])}-{int(half[-1][1:])}",
                            group_menu("Main story", half, "cs_top", 7)))
route_entries = [(name, parts) for name, parts in routes]
route_top = label()
out.append(f"label {route_top}")
out.append("menu")
route_labels = []
for name, parts in route_entries:
    lab = label()
    route_labels.append((lab, name, parts))
    out.append(f'  choice "{name}"')
    out.append(f"    jump {lab}")
out.append('  choice "Back"')
out.append("    jump cs_top")
out.append("end")
for lab, name, parts in route_labels:
    first = page_menu(name, [(title(s), start(s)) for s in parts], route_top)
    out.append(f"label {lab}")
    out.append(f"jump {first}")
submenus = out[body_start:]

top = ["label cs_top", "menu"]
for text, line in top_entries + [(t, f"jump {l}") for t, l in half_labels] + [("Heroine routes", f"jump {route_top}")]:
    top += [f'  choice "{text}"', f"    {line}"]
top.append("end")

with open(main_path, "w", encoding="utf-8") as h:
    h.write("\n".join(head + top + submenus) + "\n")
print(f"chapter select added: {len(main_story)} main chapters, "
      + ", ".join(f"{n} ({len(p)})" for n, p in routes))
