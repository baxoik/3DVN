#!/usr/bin/env python3
"""Fix leftover character sprites and face portraits in a converted G-Senjou game.

usage: fix_scene.py CONVERTER_PY KS_DIR HEADS_TXT GAME_DIR   (edits GAME_DIR/ch in place)

Sprites: the converter only hides sprites it saw being shown in the same file,
so sprites from a previous chapter or a calling scene stay on screen. Here every
background change (and every chapter start) hides all five sprite slots; hiding
an unshown sprite is a no-op in 3DVN.

Faces: in the original, @mface and every @chr only *remember* a face per
character; each speaker line shows that speaker's remembered face (in ADV mode
only), and every page end hides it; @clear_mface forgets all. The converter
instead showed the face at @mface and kept it up for everyone. The converter is
re-run on the original scripts with stand-in assets and markers for
@clear_mface / @avg / @novel to work out which face each line should show; lines
are matched to the game's (fan translation) lines like add_voices.py does.
HEADS_TXT is the case list of getStNameHead() from the game's macro.ks, copied from
your own copy (lines of the form "<name>":tempName = "<head>"); see README.md.
"""
import difflib, importlib.util, os, re, sys, tempfile

FUZZY = 0.85
conv_py, ks_dir, heads_txt, game = sys.argv[1:5]

source = open(conv_py, encoding="utf-8").read()
for old, new in (
    ('        elif command == "clear_mface":\n',
     '        elif command == "clear_mface":\n            out("# clear_mface")\n'),
    ('        elif command in ("waitclick", "show", "avg", "novel", "wjump"):\n            pass\n',
     '        elif command in ("avg", "avg_with_name", "novel"):\n            out("# mode " + command)\n'
     '        elif command in ("waitclick", "show", "wjump"):\n            pass\n'),
):
    assert source.count(old) == 1, old
    source = source.replace(old, new)
conv = importlib.util.module_from_spec(importlib.util.spec_from_loader("gsenjou_conv", loader=None))
exec(compile(source, conv_py, "exec"), conv.__dict__)

heads = dict(re.findall(r'"([^"]+)":tempName = "([a-z]+)"', open(heads_txt, encoding="utf-8").read()))
id2head = {conv.NAMES[jp][0]: head for jp, head in heads.items() if jp in conv.NAMES}

SAY = re.compile(r'^(?:([A-Za-z0-9_]+): )?"')
SLOTS = ["sll", "sl", "sc", "sr", "srr"]
SHOW_SLOT = re.compile(r'^show (sll|sl|sc|sr|srr) (\S+) at ')
SHOW_FACE = re.compile(r'^\s*show face (\S+) at face')
sprites_dir = os.path.join(game, "sprites")
faces_available = {f[:-4] for f in os.listdir(sprites_dir) if f.startswith("face_") and f.endswith(".png")}


FONT_TAG = re.compile(r'\[\s*(?:font|resetfont)\b[^\]]*\]')


def norm(line):
    # Font tags are compared away: the game's lines may still carry "[ font italic=... ]"
    # (removed later by fix_leaked_commands.py) while the re-run converter drops them.
    line = FONT_TAG.sub("", line)
    return re.sub(r"[^a-z0-9]+", "", re.sub(r'^[A-Za-z0-9_]+: ', '', line).lower())


def speaker(line):
    return SAY.match(line).group(1)


def similarity(x, y):
    return difflib.SequenceMatcher(None, norm(x), norm(y), autojunk=False).ratio()


def face_of_sprite(stem):
    return "face_" + re.sub(r"_[bs]$", "", stem)


with tempfile.TemporaryDirectory() as src:
    os.symlink(os.path.abspath(ks_dir), os.path.join(src, "1-raw-scripts"))
    assets = conv.Assets(src, os.path.join(src, "unused"), False, False, 22050, 16000)
    assets.sprite = lambda stem: (stem, 0, 0)
    assets.face = lambda name: face_of_sprite(name)
    converter = conv.Converter(src, assets)
    chapters = sorted(f[:-4] for f in os.listdir(os.path.join(game, "ch")) if f.endswith(".3vs"))
    generated = converter.convert_all([c for c in chapters if os.path.exists(os.path.join(ks_dir, c + ".ks"))])

stats = {"face shows": 0, "face hides": 0, "sprite hides added": 0, "lines matched": 0, "lines": 0}
for stem in chapters:
    path = os.path.join(game, "ch", stem + ".3vs")
    with open(path, encoding="utf-8") as h:
        lines = h.read().split("\n")

    # wanted face for each say line of the original, simulating faceRecord
    gen_say, gen_face = [], []
    rec, adv = {}, True
    for line in generated.get(stem, []):
        m = SHOW_FACE.match(line)
        s = SHOW_SLOT.match(line)
        if m:
            rec[m.group(1)[5:9]] = m.group(1)
        elif s:
            rec[s.group(2)[:4]] = face_of_sprite(s.group(2))
        elif line == "# clear_mface":
            rec = {}
        elif line.startswith("# mode "):
            adv = line != "# mode novel"
        elif SAY.match(line):
            want = rec.get(id2head.get(speaker(line))) if adv else None
            gen_say.append(line)
            gen_face.append(want if want in faces_available else None)

    say_idx = [i for i, line in enumerate(lines) if SAY.match(line)]
    old_say = [lines[i] for i in say_idx]
    wanted = [None] * len(old_say)
    known = [False] * len(old_say)
    sm = difflib.SequenceMatcher(None, [norm(x) for x in gen_say], [norm(x) for x in old_say], autojunk=False)
    for tag, a1, a2, b1, b2 in sm.get_opcodes():
        if tag == "equal":
            for k in range(a2 - a1):
                wanted[b1 + k], known[b1 + k] = gen_face[a1 + k], True
            continue
        taken = set()
        for a in range(a1, a2):
            best, score = None, 0.0
            for b in range(b1, b2):
                if b not in taken and speaker(old_say[b]) == speaker(gen_say[a]):
                    sc = similarity(gen_say[a], old_say[b])
                    if sc > score:
                        best, score = b, sc
            if best is not None and score >= FUZZY:
                wanted[best], known[best] = gen_face[a], True
                taken.add(best)
    for k in range(len(old_say)):  # unmatched: keep the previous line's face if the same person goes on
        if not known[k] and k and speaker(old_say[k]) and speaker(old_say[k]) == speaker(old_say[k - 1]):
            wanted[k] = wanted[k - 1]
    stats["lines"] += len(old_say)
    stats["lines matched"] += sum(known)

    out, visible, say_no = [], "?", 0

    def hide_sprites():
        recent = out[-8:]
        for slot in SLOTS:
            if f"hide {slot}" not in recent:
                out.append(f"hide {slot}")
                stats["sprite hides added"] += 1

    start = 1 if lines and lines[0].startswith("include") else 0
    out.extend(lines[:start])
    hide_sprites()
    out.append("hide face")
    visible = None
    for i in range(start, len(lines)):
        line = lines[i]
        if SHOW_FACE.match(line) or line.strip() == "hide face":
            continue
        if line.startswith("bg "):
            if visible is not None:
                out.append("hide face")
                visible = None
            hide_sprites()
        if SAY.match(line):
            want = wanted[say_no]
            say_no += 1
            if want != visible:
                cmd = f"show face {want} at face" if want else "hide face"
                at = len(out)
                while at and out[at - 1].startswith("sfx v/"):
                    at -= 1
                out.insert(at, cmd)
                stats["face shows" if want else "face hides"] += 1
                visible = want
        out.append(line)
        if line.startswith("label ") or line.startswith(" "):
            visible = "?"  # reached by jumps / inside choices: state not known, be explicit next time
    with open(path, "w", encoding="utf-8") as h:
        h.write("\n".join(out))

print(", ".join(f"{k}: {v}" for k, v in stats.items()))
