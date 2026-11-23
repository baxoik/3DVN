#!/usr/bin/env python3
"""Convert a Narcissu 10th Anniversary Anthology Project story (Steam; Majiro engine) to 3DVN.

usage: import_narcissu.py STORY_FOLDER OUT_GAME_DIR [--no-movies] [--movie-size WxH]
                          [--chapters NAME,NAME,...] [import_vnds options]

STORY_FOLDER is one of the anthology's folders, e.g.
  ".../steamapps/common/Narcissu 10th Anniversary Anthology Project/narci_onetwo"
It is read in place (nothing is extracted):
  - the scripts: scenario.arc (compiled, lightly encrypted Majiro scripts, *.mjo), whose
    text, voices, pictures, music, sounds, waits and movie are translated to VNDS commands
    and converted by import_vnds.py (NVL display, as the original's full-screen text)
  - pictures: data*.arc, fastdata.arc, update.arc (800x600 PNG)
  - voices: voice*.arc; music: stream.arc; sound effects: data*.arc
  - the movie (unless --no-movies): movie.arc, made into video/ with mk3vv.py
The stories and their chapters are found by the chapter scripts' names (see STORIES);
--chapters gives them by hand. Only convert games you own.
"""
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from collections import OrderedDict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import import_vnds  # noqa: E402

try:
    from PIL import Image
except ImportError:
    Image = None

CANVAS = (800, 600)
# folder -> (game title, [(story title, [chapter-name patterns])]): a story's chapters are
# each pattern's scripts in name order, pattern after pattern. Left out: the Japanese
# originals, the one-script "novel" versions (the same text again), older translations.
STORIES = {
    "narci_onetwo": ("Narcissu 1st & 2nd", [("Narcissu", [r"nar1_\d+"]),
                                            ("Narcissu -Side 2nd-", [r"nar2_\d+"])]),
    "narci_zero": ("Narcissu Zero", [("Narcissu Zero", [r"nar4_\d+", r"nar4_ep"]),
                                     ("Afterword", [r"4atogaki"])]),
    "narci_iris": ("Narcissu Iris", [("Iris", [r"nar3_\d+"]), ("Afterword", [r"atogaki"])]),
    "narci_sumire": ("Narcissu Sumire", [("Sumire", [r"3narsumi\d+", r"ep"]),
                                         ("Akari", [r"@あかり\d+"]),
                                         ("Character introductions", [r"omake"])]),
    "narci_himeko": ("Narcissu Himeko", [("Himeko", [r"himeko_ep\d+"]),
                                         ("Afterword", [r"himeko_atogaki"])]),
}
BLACK = {"b", "b2", "b3", "black"}
WHITE = {"w", "w2", "w3", "white"}


# ---- Majiro archives and scripts ----------------------------------------------------

class MajiroArc:
    """MajiroArcV3.000: name -> bytes (names compared without case or extension)."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            head = f.read(28)
            if head[:16] != b"MajiroArcV3.000\0":
                raise ValueError(f"{path}: not a MajiroArcV3 archive")
            count, names_at, data_at = struct.unpack_from("<III", head, 16)
            f.seek(0)
            table = f.read(data_at)
        entries = [struct.unpack_from("<QII", table, 28 + i * 16) for i in range(count)]
        names = [n.decode("cp932", "replace") for n in table[names_at:data_at].split(b"\0")[:count]]
        self.files = {}
        for name, (_, offset, size) in zip(names, entries):
            self.files.setdefault(name.lower(), (name, offset, size))

    def read(self, name):
        real, offset, size = self.files[name]
        with open(self.path, "rb") as f:
            f.seek(offset)
            return f.read(size)


def _crc_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
        table.append(c)
    return b"".join(struct.pack("<I", c) for c in table)


KEY = _crc_table()   # MajiroObjX: the bytecode is XORed with the CRC-32 table


def load_mjo(data):
    """-> (main entry offset, bytecode)"""
    if not data.startswith((b"MajiroObjX1.000", b"MajiroObjV1.000")):
        raise ValueError("not a Majiro script")
    main, _, nfunc = struct.unpack_from("<III", data, 16)
    at = 28 + nfunc * 8
    size = struct.unpack_from("<I", data, at)[0]
    code = bytearray(data[at + 4:at + 4 + size])
    if data.startswith(b"MajiroObjX"):
        for i in range(len(code)):
            code[i] ^= KEY[i & 1023]
    return main, bytes(code)


JUMPS = {0x82c, 0x82d, 0x82e, 0x830, 0x831, 0x832, 0x838, 0x839, 0x83b, 0x83c, 0x83d,
         0x843, 0x845, 0x846, 0x847}
NO_OPERAND = {0x82b, 0x82f, 0x83e, 0x83f, 0x841, 0x844}
TEXT, PROC, CTRL, LDC_I, LDSTR, LDC_R = 0x840, 0x841, 0x842, 0x800, 0x801, 0x803
CALLS = {0x80f, 0x810}
SYSCALLS = {0x834, 0x835}


def disassemble(code):
    """-> [(offset, opcode, operand)]; calls: (hash, argc)."""
    out, p = [], 0
    while p < len(code):
        at = p
        op = struct.unpack_from("<H", code, p)[0]
        p += 2
        arg = None
        if 0x100 <= op < 0x1b0 or op in NO_OPERAND:
            pass
        elif 0x1b0 <= op < 0x330 or op in (0x802, 0x837):        # variables
            arg = struct.unpack_from("<HIh", code, p)
            p += 8
        elif op in (LDC_I,) or op in JUMPS:
            arg = struct.unpack_from("<i", code, p)[0]
            p += 4
        elif op == LDC_R:
            arg = struct.unpack_from("<f", code, p)[0]
            p += 4
        elif op == 0x83a:                                         # source line
            arg = struct.unpack_from("<H", code, p)[0]
            p += 2
        elif op in (LDSTR, TEXT, CTRL):
            n = struct.unpack_from("<H", code, p)[0]
            arg = code[p + 2:p + 2 + n].rstrip(b"\0").decode("cp932", "replace")
            p += 2 + n
        elif op in CALLS:
            h, _, argc = struct.unpack_from("<IIH", code, p)
            arg = (h, argc)
            p += 10
        elif op in SYSCALLS:
            arg = struct.unpack_from("<IH", code, p)
            p += 6
        elif op in (0x829, 0x836):                                # type lists
            n = struct.unpack_from("<H", code, p)[0]
            arg = list(code[p + 2:p + 2 + n])
            p += 2 + n
        elif op == 0x850:                                         # switch
            n = struct.unpack_from("<H", code, p)[0]
            arg = list(struct.unpack_from(f"<{n}i", code, p + 2))
            p += 2 + 4 * n
        else:
            raise ValueError(f"unknown Majiro opcode {op:#x} at {at:#x}")
        out.append((at, op, arg))
    return out


def syscall(name):
    return zlib.crc32(f"${name}@MAJIRO_INTER".encode())


# the anthology's script library functions (user calls), known by what they are given
PICTURE, APPLY, VOICE, MUSIC, WAIT = 0xa4eb1e4c, 0x5f271e74, 0x812afdf0, 0xd334ba75, 0x13059346
SYS_WAIT, SYS_SOUND, SYS_SOUND2 = syscall("wait"), syscall("sound"), 0x708b0256
SYS_VOICE, SYS_MOVIE = 0x90d5298a, syscall("movie_play")
SYS_MUSIC_STOP = {syscall("music_stop"), syscall("music_fadeout")}
SYS_SOUND_STOP = {syscall("sound_fadeout")}


# ---- translation to VNDS -------------------------------------------------------------

def asset_name(name):
    """'se\\ele2' / '2na.ogg' / 'Car1' -> 'ele2' / '2na' / 'car1'"""
    base = name.replace("\\", "/").split("/")[-1].lower()
    return re.sub(r"\.(ogg|wav|png|jpg|bmp|mpg)$", "", base)


def frames(ms):
    return max(0, round(ms * 60 / 1000))


class ChapterTranslator:
    """One chapter script -> VNDS lines."""

    def __init__(self, names):
        self.names = names          # what exists: {"pic": set, "voice": set, ...}
        self.warnings = []
        self.movies = set()

    def picture(self, name):
        s = asset_name(name)
        if s in BLACK:
            return "special/black.png"
        if s in WHITE:
            return "special/white.png"
        if s not in self.names["pic"]:
            self.warnings.append(f"missing picture {name}")
            return None
        return s + ".png"

    def translate(self, data):
        main, code = load_mjo(data)
        ins = [i for i in disassemble(code) if i[0] >= main and i[1] != 0x83a]
        out, pending, held = [], [], []
        speaker = None

        def flush(click):
            nonlocal held, speaker
            if not held:
                return
            text = "".join(held).strip()
            held = []
            if speaker:
                text = f"[{speaker}] {text}"
                speaker = None
            out.append(("text " if click else "text @") + text)

        for n, (at, op, arg) in enumerate(ins):
            if op == TEXT:
                text = arg.replace("　", " ")
                if text.startswith("「「"):                # the text before was the speaker
                    if held and not speaker:
                        speaker = "".join(held).strip()
                        held = []
                    text = text[2:]
                held.append(text)
            elif op == CTRL:
                if arg == "p":
                    flush(True)
                elif arg == "n":
                    flush(False)
                elif arg == "w":
                    flush(True)
                    out.append("cleartext")
            elif op in CALLS or op in SYSCALLS:
                h, argc = arg
                args = []
                for _, aop, a in reversed(ins[max(0, n - argc):n]):
                    if aop not in (LDC_I, LDSTR, LDC_R):
                        break
                    args.insert(0, a)
                out += self.call(op in SYSCALLS, h, args, pending)
        flush(True)
        return out

    def call(self, system, h, args, pending):
        strings = [a for a in args if isinstance(a, str)]
        numbers = [a for a in args if not isinstance(a, str)]
        if not system and h == PICTURE and strings:
            pending.append((strings[-1], numbers[0] if numbers else 0))
            return []
        if not system and h == APPLY:
            out = []
            for name, ms in pending:
                pic = self.picture(name)
                if pic:
                    out.append(f"bgload {pic} {frames(ms)}" if ms else f"bgload {pic}")
            pending.clear()
            return out
        if (h == VOICE and not system) or (h == SYS_VOICE and system):
            if strings and asset_name(strings[-1]) in self.names["voice"]:
                return [f"sound {asset_name(strings[-1])}.ogg"]
            return []
        if not system and h == MUSIC and strings:
            if asset_name(strings[-1]) in self.names["music"]:
                return [f"music {asset_name(strings[-1])}.ogg"]
            self.warnings.append(f"missing music {strings[-1]}")
            return []
        if system and h in (SYS_SOUND, SYS_SOUND2) and strings:
            name = asset_name(strings[-1])
            if name not in self.names["sound"] and name not in self.names["voice"]:
                self.warnings.append(f"missing sound {strings[-1]}")
                return []
            loop = " -1" if numbers and numbers[0] == 1 else ""
            return [f"sound {name}.ogg{loop}"]
        if (h == WAIT and not system) or (h == SYS_WAIT and system):
            return [f"delay {frames(numbers[0])}"] if numbers and numbers[0] > 0 else []
        if system and h in SYS_MUSIC_STOP:
            return ["music ~"]
        if system and h in SYS_SOUND_STOP:
            return ["sound ~"]
        if system and h == SYS_MOVIE and strings:
            name = os.path.splitext(strings[-1])[0].lower()
            self.movies.add(strings[-1])
            return [f"video {name}"]
        return []      # text speed, window, colours, ruby ...: not shown


# ---- assets ---------------------------------------------------------------------------

class ArcAssets:
    """Assets for import_vnds from Majiro archives (later archives win); plain colours made."""

    def __init__(self, arcs, exts, plain=False):
        self.entries = {}
        for arc in arcs:
            for name, (real, _, _) in arc.files.items():
                stem, ext = os.path.splitext(name)
                if ext in exts:
                    self.entries[stem] = (arc, name, real)
        self.plain = plain
        self.files = {}

    def names(self):
        return set(self.entries)

    def find(self, name):
        s = os.path.splitext(os.path.basename(name.replace("\\", "/")))[0].lower()
        if self.plain and name.lower().startswith("special/") and s in ("black", "white"):
            self.files["plain:" + s] = s + ".png"
            return "plain:" + s
        if s in self.entries:
            self.files[s] = self.entries[s][2]
            return s
        return None

    def rel(self, key):
        return self.files[key]

    def path(self, key):
        return None

    def read(self, key):
        if key.startswith("plain:"):
            buf = io.BytesIO()
            Image.new("RGB", CANVAS, (0, 0, 0) if key.endswith("black") else (255, 255, 255)).save(buf, "PNG")
            return buf.getvalue()
        arc, name, _ = self.entries[key]
        return arc.read(name)


def make_movie(arcs, name, out, size_limit):
    if not shutil.which("ffmpeg"):
        return "warning: movie not converted (needs ffmpeg)"
    arc = next((a for a in arcs if name.lower() in a.files), None)
    if arc is None:
        return f"warning: movie {name} not found"
    work = tempfile.mkdtemp(prefix="narmovie")
    try:
        src = os.path.join(work, os.path.basename(name))
        with open(src, "wb") as handle:
            handle.write(arc.read(name.lower()))
        probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                "stream=width,height", "-of", "csv=p=0", src],
                               capture_output=True, text=True).stdout.split(",")
        w, h = int(probe[0]), int(probe[1])
        scale = min(1.0, size_limit[0] / w, size_limit[1] / h)
        size = f"{round(w * scale / 2) * 2}x{round(h * scale / 2) * 2}"
        os.makedirs(os.path.join(out, "video"), exist_ok=True)
        target = os.path.join(out, "video", import_vnds.ident(os.path.splitext(name)[0].lower()))
        tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mk3vv.py")
        result = subprocess.run([sys.executable, "-I", tool, src, target, "--fps", "15", "--size", size],
                                capture_output=True, text=True)
        if result.returncode:
            return f"warning: movie {name} not converted: {result.stderr.strip()[-200:]}"
        if os.path.exists(target + ".wav"):     # the sound as Ogg Vorbis (read by content)
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-vn", "-c:a", "libvorbis",
                            "-q:a", "4", "-f", "ogg", target + ".wav"], check=True)
        return f"movie {name}: {size}"
    finally:
        shutil.rmtree(work, ignore_errors=True)


# ---- main ---------------------------------------------------------------------------

def take_option(args, flag, value=False):
    if flag not in args:
        return None
    at = args.index(flag)
    if value:
        if at + 1 >= len(args):
            sys.exit(f"{flag} needs a value")
        result = args[at + 1]
        del args[at:at + 2]
        return result
    del args[at]
    return True


def main():
    args = sys.argv[1:]
    no_movies = take_option(args, "--no-movies")
    chapters_opt = take_option(args, "--chapters", True)
    movie_size = take_option(args, "--movie-size", True) or "320x240"
    m = re.fullmatch(r"(\d+)x(\d+)", movie_size)
    if not m or not (0 < int(m.group(1)) <= 512 and 0 < int(m.group(2)) <= 256):
        sys.exit("--movie-size must be WxH, at most 512x256")
    if len(args) < 2 or args[0].startswith("-"):
        sys.exit(__doc__)
    folder, out, rest = args[0].rstrip("/"), args[1], args[2:]
    if not os.path.isfile(os.path.join(folder, "scenario.arc")):
        sys.exit(f"{folder}: no scenario.arc (give one story folder, e.g. narci_onetwo)")

    arcs = OrderedDict()
    for f in sorted(os.listdir(folder), key=lambda n: (n.lower() == "update.arc", n.lower())):
        if f.lower().endswith(".arc"):
            try:
                arcs[f.lower()] = MajiroArc(os.path.join(folder, f))
            except ValueError:
                pass
    # the other stories' archives, searched last: the anthology's scripts sometimes name a
    # sound or picture that only a sibling story ships (the original then plays nothing)
    siblings = []
    parent = os.path.dirname(os.path.abspath(folder))
    for other in sorted(os.listdir(parent)):
        path = os.path.join(parent, other)
        if os.path.isdir(path) and path != os.path.abspath(folder) and \
                os.path.isfile(os.path.join(path, "scenario.arc")):
            for f in sorted(os.listdir(path)):
                if f.lower().endswith(".arc") and f.lower() not in ("scenario.arc", "movie.arc"):
                    try:
                        siblings.append((f.lower(), MajiroArc(os.path.join(path, f))))
                    except ValueError:
                        pass

    def pick(pattern):
        """own archives (later ones win), after the siblings' (so the own ones win)"""
        return [a for n, a in siblings if re.fullmatch(pattern, n)] + \
               [a for n, a in arcs.items() if re.fullmatch(pattern, n)]
    pics = ArcAssets(pick(r"(data\d*|fastdata|update)\.arc"), {".png", ".jpg", ".bmp"}, plain=True)
    voices = ArcAssets(pick(r"voice\d*\.arc"), {".ogg", ".wav"})
    music = ArcAssets(pick(r"stream\.arc"), {".ogg", ".wav"})
    sounds = ArcAssets(pick(r"(data\d*|update|stream)\.arc"), {".ogg", ".wav"})
    names = {"pic": pics.names(), "voice": voices.names(), "music": music.names(), "sound": sounds.names()}

    scenario = arcs["scenario.arc"]
    scripts = {os.path.splitext(n)[0]: n for n in scenario.files if n.endswith(".mjo")}
    key = os.path.basename(folder).lower()
    title, stories = STORIES.get(key, (os.path.basename(folder), []))
    story_chapters = []
    if chapters_opt:
        found = [c.strip().lower() for c in chapters_opt.split(",") if c.strip().lower() in scripts]
        story_chapters = [(title, found)] if found else []
    else:
        for story, patterns in stories:
            found = [s for pattern in patterns for s in sorted(scripts) if re.fullmatch(pattern, s)]
            if found:
                story_chapters.append((story, found))
    if not story_chapters:
        sys.exit(f"{folder}: no known chapters; give them with --chapters (scripts: "
                 f"{', '.join(sorted(scripts)[:40])})")

    src = tempfile.mkdtemp(prefix="narsrc")
    notes, movies = [], set()
    try:
        os.makedirs(os.path.join(src, "script"))
        with open(os.path.join(src, "img.ini"), "w") as handle:
            handle.write(f"width={CANVAS[0]}\nheight={CANVAS[1]}\n")
        written = {}
        # script names: the chapter's own when it is a plain name, else the story's + number
        # ("@あかり1" -> "akari1", "4atogaki" -> "afterword1")
        ids = {}
        for story, chapters in story_chapters:
            slug = re.sub(r"[^a-z0-9]", "", story.lower()) or "part"
            for i, chapter in enumerate(chapters, 1):
                plain = re.fullmatch(r"[a-z][a-z0-9_]*", chapter) and chapter != "main"
                ids[chapter] = chapter if plain else f"{slug}{i}"
        story_chapters = [(story, [ids[c] for c in chapters], chapters)
                          for story, chapters in story_chapters]
        for story, chapters, originals in story_chapters:
            for i, (chapter, original) in enumerate(zip(chapters, originals)):
                tr = ChapterTranslator(names)
                lines = tr.translate(scenario.read(scripts[original]))
                nxt = chapters[i + 1] if i + 1 < len(chapters) else "main"
                written[chapter] = lines + ["music ~", "sound ~", f"jump {nxt}.scr"]
                movies |= tr.movies
                notes += [f"warning: {chapter}: {w}" for w in sorted(set(tr.warnings))]
        menu = ["bgload special/black.png"]
        if len(story_chapters) > 1:
            menu.append("choice " + "|".join(s for s, _, _ in story_chapters))
            for i, (_, chapters, _) in enumerate(story_chapters, 1):
                menu += [f"if selected == {i}", f"    jump {chapters[0]}.scr", "fi"]
        else:
            menu.append(f"jump {story_chapters[0][1][0]}.scr")
        written["main"] = menu
        if no_movies:
            written = {c: [("delay 1" if l.startswith("video ") else l) for l in ls] for c, ls in written.items()}
        for chapter, lines in written.items():
            with open(os.path.join(src, "script", chapter + ".scr"), "w", encoding="utf-8") as handle:
                handle.write("\n".join(lines) + "\n")

        sys.argv = [sys.argv[0], src, out, "--speakers", "--nvl", "--title", title] + rest
        original_init = import_vnds.Converter.__init__

        def init(self, a):
            original_init(self, a)
            self.bgs, self.fgs = pics, pics
            self.snd = ArcAssets(pick(r"(voice\d*|stream|data\d*|update)\.arc"), {".ogg", ".wav"})

        import_vnds.Converter.__init__ = init
        import_vnds.main()
    finally:
        shutil.rmtree(src, ignore_errors=True)
    if movies and not no_movies:
        for name in sorted(movies):
            notes.append(make_movie(list(arcs.values()), name, out, (int(m.group(1)), int(m.group(2)))))
    for note in notes:
        print(note)
    print(f"{title}: " + "; ".join(f"{s}: {len(c)} chapters" for s, c, _ in story_chapters))


if __name__ == "__main__":
    main()
