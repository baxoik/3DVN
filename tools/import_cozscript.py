#!/usr/bin/env python3
"""Convert a script from the Committee of Zero Chaos;Head translation (.mst files)
into a 3DVN game script. Run it on your own copy of the files; the output stays on
your computer. Only the dialogue text is converted: the original game's images,
sounds and scene logic are stored in binary formats that this tool does not read,
so the result plays as text over a plain background (add "bg" / "show" lines by hand).

  import_cozscript.py OUTPUT_GAME_DIR FILE.mst [FILE.mst ...] [--lines N]

Line format of an .mst file:  ID:text, where text may contain codes ending in ';':
  \\n;  line break      \\p; \\pe; \\pnc; \\a:N;  end of page
  \\c:N; \\w:N; \\lf:N; \\lc; \\sp:N;  display effects (ignored here)
Lines with no ending code are the game's backlog copies of text already shown; they
are dropped. Overlong pages are split at sentence ends to fit the text box.
and an optional leading 〔Name〕 giving the speaker.
"""
import argparse, os, re, sys

CODE = re.compile(r"\\([a-z]+)(?::\d+)?;")
SPEAKER = re.compile(r"^〔([^〕]+)〕")
TAG = re.compile(r"</?[a-z]+>")
END_CODES = ("p", "pe", "pnc", "a")  # codes that end a page
MAX_PAGE = 150  # characters that fit comfortably in the default dialogue box


def parse_line(raw):
    """Returns (speaker, text, kind). kind is "end" when the line finishes a page,
    "cont" when it carries on into the next line (ends with a line break or pause),
    and "bare" when it has no ending code at all."""
    text = raw.partition(":")[2]
    speaker = None
    m = SPEAKER.match(text)
    if m:
        speaker, text = m.group(1), text[m.end():]
    out, pos, last = "", 0, None
    for m in CODE.finditer(text):
        out += text[pos:m.start()]
        pos = m.end()
        code = m.group(1)
        if code == "n":
            out += "\n"
        last = code
        if code in END_CODES:
            last = "end"
    out += text[pos:]
    out = TAG.sub("", out)
    tail = text[pos:].strip()
    if last == "end" and not tail:
        kind = "end"
    elif last in ("n", "w") and not tail:
        kind = "cont"
    else:
        kind = "bare"
    return speaker, out, kind


def squash(text):
    return re.sub(r"[\s“”\"]+", "", text)


def split_page(text):
    """Cuts an overlong page at sentence ends so it fits the text box."""
    parts = []
    for line in text.split("\n"):
        parts.append(line.strip())
    text = "\n".join(p for p in parts if p)
    if len(text) <= MAX_PAGE:
        return [text]
    sentences = re.findall(r".+?(?:[.!?…]+[”’\"]*(?:\s+|$)|\n|$)", text, re.S)
    pages, current = [], ""
    for sentence in sentences:
        if current and len((current + sentence).replace("\n", " ")) > MAX_PAGE:
            pages.append(current.strip())
            current = ""
        current += sentence
    if current.strip():
        pages.append(current.strip())
    return pages


def convert(raw_lines):
    """Yields (speaker, page text)."""
    pending, pending_speaker = "", None
    recent = []
    for index, raw in enumerate(raw_lines):
        speaker, text, kind = parse_line(raw)
        following = raw_lines[index + 1].partition(":")[2] if index + 1 < len(raw_lines) else ""
        if kind == "bare" and following.startswith("\\n;"):
            kind = "cont"  # the next line carries on from this one
        if kind == "bare" and not pending and squash(text) and squash(TAG.sub("", text.replace("\n", ""))) in "".join(recent):
            continue  # the engine's backlog copy of text that was already shown
        if speaker:
            pending_speaker = speaker
        pending += text
        if kind == "cont":
            continue
        page, who = pending.strip(), pending_speaker
        pending, pending_speaker = "", None
        if not page.strip():
            continue
        for piece in split_page(page):
            recent.append(squash(piece))
            recent = recent[-8:]
            yield who, piece
    if pending.strip():
        yield pending_speaker, pending.strip()


def quote(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("{", "\\{") + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", help="output game folder")
    ap.add_argument("mst", nargs="+", help=".mst files, converted in the order given")
    ap.add_argument("--lines", type=int, default=0, help="stop after this many dialogue lines (default: all)")
    args = ap.parse_args()

    raw = []
    for path in args.mst:
        with open(path, encoding="utf-8") as f:
            raw.extend(l.rstrip("\r\n") for l in f if re.match(r"^\d+:", l))
    if not raw:
        sys.exit("no message lines found")

    ids, body = {}, []
    for speaker, text in convert(raw):
        ident = None
        if speaker:
            if speaker not in ids:
                base = re.sub(r"[^a-z0-9]", "", speaker.lower()) or "who"
                ident, n = base, 2
                while ident in ids.values():
                    ident, n = "%s%d" % (base, n), n + 1
                ids[speaker] = ident
            ident = ids[speaker]
        text = '"%s"' % "\\n".join(quote(line)[1:-1] for line in text.split("\n"))
        body.append("%s: %s" % (ident, text) if ident else text)
        if args.lines and len(body) >= args.lines:
            break

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "main.3vs"), "w", encoding="utf-8") as f:
        f.write("# Converted by tools/import_cozscript.py from %s\n"
                % ", ".join(os.path.basename(p) for p in args.mst))
        for speaker, ident in ids.items():
            f.write("character %s %s\n" % (ident, quote(speaker)))
        f.write("\nbg black\n\n")
        f.write("\n".join(body) + "\n")
    print("wrote %d dialogue lines to %s/main.3vs" % (len(body), args.out))


if __name__ == "__main__":
    main()
