#!/usr/bin/env python3
"""Add voice lines to an already converted G-Senjou 3DVN game.

usage: add_voices.py CONVERTER_PY KS_DIR GAME_DIR VOICE_WAV_DIR OUT_DIR
       REPORT=silent.md add_voices.py ... (only write a report of unplaced lines)

CONVERTER_PY   3DVN's tools/import_gsenjou.py (its text pipeline is reused, so
               dialogue lines come out as the converter would write them)
KS_DIR         readable (decoded) chapter scripts: g01.ks, g02.ks, ...
GAME_DIR       the existing voiceless game folder (left untouched)
VOICE_WAV_DIR  16 kHz mono WAVs named <voice>.wav
OUT_DIR        new game folder: a copy of GAME_DIR with voices inserted

Matching: lines are compared ignoring case, punctuation and quote style. A
sequence alignment places exact matches; inside the stretches that differ, a
voice also goes to a line by the same speaker that is at least FUZZY similar.
"""
import difflib, importlib.util, os, re, shutil, sys, tempfile

FUZZY = 0.85
conv_py, ks_dir, game, wav_dir, out = sys.argv[1:6]
spec = importlib.util.spec_from_file_location("gsenjou_conv", conv_py)
conv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(conv)

SAY = re.compile(r'^(?:([A-Za-z0-9_]+): )?"')
SFX = re.compile(r'^sfx (\S+)$')


ONES = "zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen " \
       "sixteen seventeen eighteen nineteen".split()
TENS = "_ _ twenty thirty forty fifty sixty seventy eighty ninety".split()


def number_words(n):
    n = int(n)
    if n < 20:
        return ONES[n]
    if n < 100:
        return TENS[n // 10] + ("" if n % 10 == 0 else ONES[n % 10])
    return str(n)


def norm(line):
    """Compare text only: no speaker, case, punctuation or quote style; numbers spelled out."""
    line = re.sub(r'^[A-Za-z0-9_]+: ', '', line).lower()
    line = re.sub(r"\b(\d{1,2}):(\d\d)\b",
                  lambda m: number_words(m.group(1)) + ("" if m.group(2) == "00" else number_words(m.group(2))), line)
    line = re.sub(r"\b\d{1,2}\b", lambda m: number_words(m.group(0)), line)
    return re.sub(r"[^a-z0-9]+", "", line)


def speaker(line):
    return SAY.match(line).group(1)


def voice_path(voice):
    """audio-relative name: v/<voice minus last two digits>/<voice>, e.g. v/tub_00/tub_0001."""
    return f"v/{voice[:-2]}/{voice}"


def similarity(x, y):
    return difflib.SequenceMatcher(None, norm(x), norm(y), autojunk=False).ratio()


voice_files = {f[:-4] for f in os.listdir(wav_dir) if f.endswith(".wav")}
OVERRIDES = {}
_ov = os.environ.get("VOICE_OVERRIDES", os.path.join(os.path.dirname(os.path.abspath(__file__)), "voice_overrides.tsv"))
if os.path.exists(_ov):
    for _line in open(_ov, encoding="utf-8"):
        if _line.strip() and not _line.startswith("#"):
            _voice, _text = _line.rstrip("\n").split("\t", 1)
            OVERRIDES[_voice] = _text

with tempfile.TemporaryDirectory() as src:
    os.symlink(os.path.abspath(ks_dir), os.path.join(src, "1-raw-scripts"))
    assets = conv.Assets(src, os.path.join(src, "unused"), False, True, 22050, 16000)
    assets.voice = lambda stem: stem if stem in voice_files else assets.missing["voice"].add(stem)
    converter = conv.Converter(src, assets)
    chapters_in = sorted(f[:-4] for f in os.listdir(os.path.join(game, "ch")) if f.endswith(".3vs"))
    available = [c for c in chapters_in if os.path.exists(os.path.join(ks_dir, c + ".ks"))]
    generated = converter.convert_all(available)


def match_chapter(stem):
    """Returns (old_lines, old_idx, old_say, gen_say, gen_voice, placed{a: b}, unplaced[(a, b1, b2)])."""
    gen = generated[stem]
    gen_say, gen_voice = [], []
    for i, line in enumerate(gen):
        if SAY.match(line):
            prev = SFX.match(gen[i - 1]) if i else None
            gen_say.append(line)
            gen_voice.append(prev.group(1) if prev and prev.group(1) in voice_files else None)
    with open(os.path.join(game, "ch", stem + ".3vs"), encoding="utf-8") as h:
        old_lines = h.read().split("\n")
    old_idx = [i for i, line in enumerate(old_lines) if SAY.match(line)]
    old_say = [old_lines[i] for i in old_idx]
    sm = difflib.SequenceMatcher(None, [norm(x) for x in gen_say], [norm(x) for x in old_say], autojunk=False)
    placed, unplaced = {}, []
    for tag, a1, a2, b1, b2 in sm.get_opcodes():
        if tag == "equal":
            for k in range(a2 - a1):
                if gen_voice[a1 + k]:
                    placed[a1 + k] = b1 + k
            continue
        taken, matched = set(), set()
        for a in range(a1, a2):
            if not gen_voice[a]:
                continue
            best, score = None, 0.0
            for b in range(b1, b2):
                if b in taken or speaker(old_say[b]) != speaker(gen_say[a]):
                    continue
                s = similarity(gen_say[a], old_say[b])
                if s > score:
                    best, score = b, s
            if best is not None and score >= FUZZY:
                if os.environ.get("SHOW_FUZZY"):
                    print(f"FUZZY {score:.2f} {stem}\n  steam: {gen_say[a]}\n  build: {old_say[best]}")
                placed[a] = best
                taken.add(best)
                matched.add(a)

        def place(a, b, why):
            if os.environ.get("SHOW_PASS3"):
                print(f"{why} {stem}\n  steam: {gen_say[a]}\n  build: {old_say[b]}")
            placed[a] = b
            taken.add(b)
            matched.add(a)

        # Pass 3a: one line is the start of the other (the build merged or split a line).
        for a in range(a1, a2):
            if not gen_voice[a] or a in matched:
                continue
            g = norm(gen_say[a])
            for b in range(b1, b2):
                o = norm(old_say[b])
                if (b not in taken and speaker(old_say[b]) == speaker(gen_say[a]) and min(len(g), len(o)) >= 15
                        and (o.startswith(g) or g.startswith(o))):
                    place(a, b, "PREFIX")
                    break
        # Pass 3b: between exact-match anchors, if a speaker has the same number of still
        # unmatched lines on both sides, they are the same lines reworded: pair them in order.
        for who in {speaker(gen_say[a]) for a in range(a1, a2)} - {None}:
            gens = [a for a in range(a1, a2) if speaker(gen_say[a]) == who and a not in matched]
            olds = [b for b in range(b1, b2) if speaker(old_say[b]) == who and b not in taken]
            if gens and len(gens) == len(olds):
                for a, b in zip(gens, olds):
                    if gen_voice[a]:
                        place(a, b, "ORDER")
        # Hand-picked pairs from voice_overrides.tsv for lines reworded beyond recognition.
        for a in range(a1, a2):
            if gen_voice[a] in OVERRIDES and a not in matched:
                hits = [b for b in range(len(old_say)) if OVERRIDES[gen_voice[a]] in old_say[b] and b not in taken]
                if len(hits) == 1:
                    place(a, hits[0], "OVERRIDE")
        for a in range(a1, a2):
            if gen_voice[a] and a not in matched:
                unplaced.append((a, b1, b2))
    return old_lines, old_idx, old_say, gen_say, gen_voice, placed, unplaced


report_path = os.environ.get("REPORT")
if report_path:
    def clean(line):
        line = re.sub(r'^([A-Za-z0-9_]+): ', r'**\1:** ', line)
        return line.replace('\\"', '"').replace("|", "\\|")
    rows, placed_total, want_total = [], 0, 0
    for stem in chapters_in:
        if stem not in generated:
            continue
        old_lines, old_idx, old_say, gen_say, gen_voice, placed, unplaced = match_chapter(stem)
        want = sum(1 for v in gen_voice if v)
        placed_total += len(placed)
        want_total += want
        chapter_rows = []
        for a, b1, b2 in unplaced:
            cands = range(max(0, b1 - 1), min(len(old_say), max(b2, b1 + 1) + 1))
            best = max(cands, key=lambda b: similarity(gen_say[a], old_say[b]), default=None)
            fan = (f"{clean(old_say[best])} (`ch/{stem}.3vs` line {old_idx[best] + 1})"
                   if best is not None else "*(no counterpart)*")
            chapter_rows.append(f"| `{gen_voice[a]}` | {clean(gen_say[a])} | {fan} |")
        if chapter_rows:
            rows.append((stem, want, chapter_rows))
    with open(report_path, "w", encoding="utf-8") as h:
        h.write("# G-Senjou no Maou 3DS: silent voice lines\n\n")
        h.write(f"{placed_total} of {want_total} voiced lines got their voice; {want_total - placed_total} play "
                "silent because the Steam script line could not be matched to the build's (fan) translation "
                f"(exact match, or same speaker and at least {FUZZY:.0%} similar).\n\n"
                "To voice one by hand, add `sfx <voice>` on the line above the build's line in "
                "`ch/<chapter>.3vs` and copy `voice-wav/<voice>.wav` into the game's `audio/` folder.\n\n")
        h.write("| Chapter | Silent | Voiced |\n|---|---|---|\n")
        for stem, want, chapter_rows in rows:
            h.write(f"| {stem} | {len(chapter_rows)} | {want} |\n")
        for stem, want, chapter_rows in rows:
            h.write(f"\n## {stem} ({len(chapter_rows)} silent)\n\n"
                    "| Voice | Steam text | Closest build line |\n|---|---|---|\n")
            h.write("\n".join(chapter_rows) + "\n")
    print(f"report: {want_total - placed_total} silent lines in {len(rows)} chapters -> {report_path}")
    sys.exit(0)

if os.path.exists(out):
    sys.exit(f"{out} already exists; remove it first")
shutil.copytree(game, out)
total = matched = 0
used = set()
for stem in chapters_in:
    path = os.path.join(out, "ch", stem + ".3vs")
    if stem not in generated:
        print(f"{stem}: no readable script, left voiceless")
        continue
    lines, old_idx, old_say, gen_say, gen_voice, placed, unplaced = match_chapter(stem)
    insert = {old_idx[b]: gen_voice[a] for a, b in placed.items()}
    # FAT32 looks files up by scanning the folder, so keep voice folders small (<= 100 files)
    want = sum(1 for v in gen_voice if v)
    total += want
    matched += len(insert)
    result = []
    for i, line in enumerate(lines):
        if i in insert:
            result.append(f"sfx {voice_path(insert[i])}")
            used.add(insert[i])
        result.append(line)
    with open(path, "w", encoding="utf-8") as h:
        h.write("\n".join(result))
    print(f"{stem}: {len(insert)}/{want} voice lines placed")

for v in sorted(used):
    target = os.path.join(out, "audio", voice_path(v) + ".wav")
    os.makedirs(os.path.dirname(target), exist_ok=True)
    shutil.copy2(os.path.join(wav_dir, v + ".wav"), target)
missing = sorted(assets.missing.get("voice", ()))
print(f"TOTAL: {matched}/{total} voiced lines placed, {len(used)} WAVs copied, "
      f"{len(missing)} referenced voices without a WAV{': ' + ', '.join(missing[:20]) if missing else ''}")
