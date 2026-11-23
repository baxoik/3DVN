#!/usr/bin/env python3
"""Convert Never 7 to 3DVN: the VNDS version's script, with the pictures, voices, music,
movies and Append Story of "Never 7 - the end of infinity" for LÖVE (n7e.love).

usage: import_never7.py VNDS_FOLDER N7E_LOVE OUT_GAME_DIR [--music pc|psp] [--no-movies]
                        [--no-append] [--movie-size WxH] [import_vnds options]

VNDS_FOLDER is the VNDS release ("Never 7", with script/ and img.ini); N7E_LOVE the .love
file (read in place, nothing is extracted). From the .love file come:
  - backgrounds and sprites: upscale/ (twice the VNDS size: the 3DS gets them scaled to
    its screen, PC / Wii U / Android draw the full-size ones from hd/); the tall panning
    CGs are cut into the VNDS top / -middle / -bottom pieces
  - voices and sound effects: sound/ (WAV, converted to Ogg Vorbis)
  - music: music/pc/ (or music/psp/ with --music psp)
  - movies (unless --no-movies): the opening, the five heroine clips and the endings with
    their songs, placed where the .love scripts play them (video/, made with mk3vv.py)
  - the Append Story (unless --no-append): Yuka's eight-part story from script/append/,
    translated to VNDS commands; the VNDS title menu's "Append Story" starts it
Anything the .love file lacks comes from the VNDS release.
Everything else is import_vnds.py (see tools/README_import_vnds.md); --speakers is on.
Only convert games you own.
"""
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import import_vnds  # noqa: E402

try:
    from PIL import Image
except ImportError:
    Image = None

NIGHT = (0x7e, 0x99, 0xff)            # the .love's tint for sprites named ...n (night)
PAN_PIECES = {"": 0.0, "-middle": 0.5, "-bottom": 1.0}   # VNDS cuts of a tall CG
SLOT_X = {"1": 400, "2": 200}          # sprite position -> centre on the 800 canvas; else 600
BGM_MAP = {"20": "21", "21": "22"}     # as the .love's conv.lua
HEROINE_CLIPS = ("yuuka", "haruka", "saki", "kurumi", "izumi")


def stem(name):
    """'sound/I26D000.waf.wav.wav' -> 'i26d000'; 'bg00a.jpg' -> 'bg00a'"""
    return re.sub(r"(\.[A-Za-z0-9]+)+$", "", os.path.basename(name.replace("\\", "/"))).lower()


def png(image):
    buf = io.BytesIO()
    image.save(buf, "PNG")
    return buf.getvalue()


class LoveOverlay:
    """A VNDS asset folder (import_vnds.Assets) whose files are replaced, where the .love
    file has them, by the .love's; names only the .love has are added. For pictures also:
    a tall panning CG is cut like the VNDS pieces, and NAMEn is NAME tinted for night."""

    def __init__(self, base, archive, folders, images=False, pans=False):
        self.base = base
        self.pans = pans      # backgrounds: cut tall panning CGs
        self.files = base.files
        self.archive = archive
        self.members = {}
        for member in archive.namelist():
            if member.endswith("/"):
                continue
            for folder in folders:
                if member.startswith(folder):
                    self.members.setdefault(stem(member), member)
        self.images = images
        self.factors = {}
        self.cache = {}
        self.replaced = set()
        self.added = {}       # synthetic key -> name, for files VNDS lacks

    # -- lookups: "love:NAME" keys for what only the .love has, "tint:NAME" for night sprites
    def find(self, name):
        key = self.base.find(name)
        if key is not None:
            return key
        s = stem(name)
        if s in self.members:
            return self._add("love:" + s, s + os.path.splitext(self.members[s])[1])
        if self.images and s.endswith("n") and len(s) > 4:
            inner = self.find(s[:-1] + ".png")
            if inner is not None:
                return self._add("tint:" + s, s + ".png")
        return None

    def _add(self, key, rel):
        self.added[key] = rel
        return key

    def rel(self, key):
        return self.added[key] if key in self.added else self.base.rel(key)

    def path(self, key):
        if key in self.added or self.big(key) is not None:
            return None
        return self.base.path(key)

    def read(self, key):
        data = self.big(key)
        if data is None:
            return self.base.read(key)
        self.replaced.add(key)
        return data

    def big(self, key):
        """The .love's bytes for key (cut or tinted as needed), or None to use VNDS's."""
        if key in self.cache:
            return self.cache[key]
        s = stem(self.rel(key))
        data = None
        if key.startswith("tint:"):
            inner = self.find(s[:-1] + ".png")
            with Image.open(io.BytesIO(self.read(inner))) as im:
                im = im.convert("RGBA")
                r, g, b, a = im.split()
                r, g, b = (ch.point(lambda v, f=f: v * f // 255) for ch, f in zip((r, g, b), NIGHT))
                data = png(Image.merge("RGBA", (r, g, b, a)))
        elif self.pans and (s in self.members or any(
                s.endswith(p) and s[:-len(p)] in self.members for p in PAN_PIECES if p)):
            piece = next((p for p in PAN_PIECES if p and s.endswith(p)), "")
            data = self.archive.read(self.members[s[:-len(piece)] if piece else s])
            with Image.open(io.BytesIO(data)) as im:
                w, h = im.size
                if h > w * 3 // 4 + 2:     # tall panning CG: cut a 4:3 piece like VNDS
                    ph = w * 3 // 4
                    top = round((h - ph) * PAN_PIECES[piece])
                    data = png(im.crop((0, top, w, top + ph)))
                elif piece:
                    data = None             # a piece of a CG that is not tall: VNDS's own
        elif s in self.members:
            data = self.archive.read(self.members[s])
        self.cache[key] = data
        return data

    def factor(self, key):
        """How many times larger the picture used is than the VNDS canvas (2 for n7e)."""
        if key not in self.factors:
            factor = 1.0
            data = self.big(key) if self.images else None
            if data is not None:
                if key.startswith("tint:"):
                    factor = self.factor(self.find(stem(self.rel(key))[:-1] + ".png"))
                elif key.startswith("love:"):
                    factor = 2.0               # upscale/ is twice the VNDS size throughout
                else:
                    with Image.open(io.BytesIO(data)) as big, \
                            Image.open(io.BytesIO(self.base.read(key))) as small:
                        factor = big.width / small.width if small.width else 1.0
            self.factors[key] = factor
        return self.factors[key]

    def canvas_width(self, name):
        key = self.find(name)
        if key is None:
            return None
        with Image.open(io.BytesIO(self.read(key))) as im:
            return round(im.width / self.factor(key))


# ---- movies -------------------------------------------------------------------------

def movie_for(chapter, arg, music):
    """(video name, .ogv in the .love, song in the .love or None), as the .love's video.scr"""
    if arg == "op":
        return ("op", "movie/opening.ogv", None) if music == "pc" else \
            ("op", "movie/psp/op.ogv", "music/psp/track_26.ogg")
    if arg in HEROINE_CLIPS:
        return (arg, f"movie/{arg}.ogv", None)
    if arg == "end1":
        if chapter.startswith("yl"):
            return ("end_y", "movie/psp/end_y.ogv", "music/psp/track_27.ogg")
        if chapter.startswith("kl"):
            return ("end_k", "movie/end_k.ogv", "music/psp/track_20.ogg")
        if chapter.startswith("il"):
            return ("end4", "movie/end3.ogv", "music/psp/track_20.ogg")
        return ("end1", "movie/end1.ogv", "music/pc/track_20.ogg")
    if arg == "end2":
        return ("end2", "movie/end2.ogv", f"music/{music}/track_25.ogg")
    if arg == "end3":
        return ("end3", "movie/end3.ogv", "music/infinity.ogg")
    if arg == "end_yc":
        return ("end_yc", "movie/end_yc.ogv", "music/pc/track_20.ogg")
    return None


def norm(text):
    return re.sub(r"[^a-z0-9]", "", text.lower())


def dec_message(line):
    """The text of a .dec message line ('{textColor 0 0}【Name】 "..."'), speaker removed."""
    text = re.sub(r"\{[^}]*\}", "", line)
    return re.sub(r"^\s*【[^】]*】", "", text).strip()


def place_movies(archive, vnds_lines, music, warn):
    """Inserts 'video NAME' lines into VNDS scripts (dict chapter -> lines), at the places
    the .love's scripts play movies. Returns {video name: (ogv, song)} and the places."""
    used, places = {}, []
    for member in sorted(archive.namelist()):
        if not re.match(r"script/[^/]+\.dec$", member):
            continue
        chapter = stem(member)
        lines = archive.read(member).decode("utf-8", "replace").splitlines()
        if chapter not in vnds_lines:
            continue
        anchors = Counter()
        for i, line in enumerate(lines):
            m = re.match(r"\[[0-9a-f]+\]\s+[0-9a-f]+: movie (\S+)", line)
            if not m:
                continue
            movie = movie_for(chapter, m.group(1), music)
            if movie is None:
                warn(f"unknown movie {m.group(1)} in {chapter}")
                continue
            # the nearest message before (empty when the movie is a jump target: then the
            # text before it belongs to another branch, and the text after it is used)
            before = next((dec_message(l) for l in reversed(lines[:i]) if l.startswith("{")), "")
            after = next((dec_message(l) for l in lines[i + 1:] if l.startswith("{")
                          and dec_message(l)), "")
            # the last sentence before the movie, or the first one after it
            if len(norm(before)) >= 10:
                key, side = norm(re.split(r"\s{2,}", before.strip())[-1])[-30:], "after"
            else:
                key, side = norm(re.split(r"\s{2,}", after.strip())[0])[:30], "before"
            anchors[(key, side)] += 1
            at = find_anchor(vnds_lines[chapter], key, side, anchors[(key, side)])
            if at is None:
                warn(f"no place found for movie {movie[0]} in {chapter}")
                continue
            insert = [f"video {movie[0]}"]
            if movie[0] == "end2":
                insert.append("music track_25.ogg")   # the song goes on under the epilogue
            vnds_lines[chapter][at:at] = insert
            used[movie[0]] = movie[1:]
            places.append((chapter, movie[0], vnds_lines[chapter][at - 1 if side == "after" else at + len(insert)]))
    return used, places


def find_anchor(lines, key, side, nth):
    """Index to insert at: after the VNDS text line ending the sentence key (side 'after'),
    or before the one starting it (side 'before'); the nth such place."""
    texts = [(i, norm(re.sub(r"^\[[^\]]*\]", "", l[5:].strip()))) for i, l in enumerate(lines)
             if l.startswith("text ") and l[5:].strip() not in ("~", "!", "")]
    found = []
    for t, (i, _) in enumerate(texts):
        if side == "after":
            joined = "".join(n for _, n in texts[max(0, t - 4):t + 1])
            if key and joined.endswith(key):
                found.append(i + 1)
        else:
            joined = "".join(n for _, n in texts[t:t + 5])
            if key and joined.startswith(key):
                found.append(i)
    return found[nth - 1] if len(found) >= nth else None


def make_movies(archive, used, out, size_limit, warn):
    if not shutil.which("ffmpeg"):
        warn("movies not converted (needs ffmpeg)")
        return []
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mk3vv.py")
    target_dir = os.path.join(out, "video")
    os.makedirs(target_dir, exist_ok=True)
    limit_w, limit_h = size_limit
    work = tempfile.mkdtemp(prefix="n7movies")

    def one(item):
        name, (ogv, song) = item
        src = os.path.join(work, name + ".ogv")
        with open(src, "wb") as handle:
            handle.write(archive.read(ogv))
        probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                "stream=width,height:format=duration", "-of", "csv=p=0", src],
                               capture_output=True, text=True).stdout.split()
        w, h = (int(v) for v in probe[0].split(",")[:2])
        duration = float(probe[1])
        scale = min(1.0, limit_w / w, limit_h / h)
        size = f"{round(w * scale / 2) * 2}x{round(h * scale / 2) * 2}"
        target = os.path.join(target_dir, import_vnds.ident(name))
        result = subprocess.run([sys.executable, "-I", tool, src, target, "--fps", "15", "--size", size],
                                capture_output=True, text=True)
        if result.returncode or not os.path.exists(target + ".3vv"):
            return f"movie {name} not converted: {result.stderr.strip()[-200:]}"
        # the sound: the movie's own track or its song, as Ogg Vorbis (the engine tells the
        # format by its content, so it keeps the name video/NAME.wav)
        sound = src
        if song:
            sound = os.path.join(work, name + ".song.ogg")
            with open(sound, "wb") as handle:
                handle.write(archive.read(song))
        has_audio = song or subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "a", "-show_entries", "stream=index",
             "-of", "csv=p=0", src], capture_output=True, text=True).stdout.strip()
        if os.path.exists(target + ".wav"):
            os.remove(target + ".wav")
        if has_audio:
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", sound, "-vn", "-t", f"{duration:.2f}",
                            "-c:a", "libvorbis", "-q:a", "4", "-ar", "44100", "-ac", "2",
                            "-f", "ogg", target + ".wav"], check=True)
        return f"movie {name}: {size}, {duration:.0f} s" + (", with sound" if has_audio else "")

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        notes = list(pool.map(one, sorted(used.items())))
    shutil.rmtree(work, ignore_errors=True)
    return notes


# ---- the Append Story ---------------------------------------------------------------

class AppendTranslator:
    """script/append/c_yukaN.dec (VNDS-like '@' lines mixed with the original engine's
    commands) -> VNDS .scr lines."""

    def __init__(self, fgs, chapter):
        self.fgs = fgs
        self.chapter = chapter
        self.bg = "special/black.jpg"
        self.pan = None
        self.sprites = {}     # slot -> (name, position)

    def sprite_lines(self):
        out = []
        for slot in sorted(self.sprites):
            name, pos = self.sprites[slot]
            width = self.fgs.canvas_width(name + ".png") or 0
            x = SLOT_X.get(pos, 600) - width // 2
            out.append(f"setimg {name}.png {x} 0")
        return out

    def redraw(self):
        return [f"bgload {self.bg}"] + self.sprite_lines()

    def translate(self, text):
        out = []
        for raw in text.splitlines():
            line = raw.strip()
            if not line:
                continue
            if line.startswith("@"):
                body = line[1:].strip()
                if body.startswith("text "):
                    message = body[5:].strip()
                    m = re.match(r"【([^】]*)】\s*(.*)$", message)
                    out.append(f"text [{m.group(1)}] {m.group(2)}" if m else f"text {message}")
                elif body == "setvar vap_seldt = selected":
                    out += ["setvar seldt = selected", "setvar seldt - 1"]
                elif body:
                    out.append(body)
                continue
            out += self.command(line.split())
        return out

    def command(self, t):
        cmd = t[0]
        if cmd in ("bgload", "createPanBG"):
            name = t[2].lower()
            self.sprites = {}
            if len(t) > 3 and t[3] == "2":           # panning CG, shown from the bottom
                self.pan, self.bg = name, f"{name}-bottom.jpg"
            else:
                self.pan, self.bg = None, f"{name}.jpg"
            return [f"bgload {self.bg}"]
        if cmd == "movePanBG" and self.pan:
            steps = [f"{self.pan}-middle.jpg"] + ([f"{self.pan}.jpg"] if t[1] == "0" else [])
            self.bg = steps[-1]
            return [f"bgload {s} 18" for s in steps] + self.sprite_lines()
        if cmd == "removeBG":
            self.sprites, self.pan = {}, None
            self.bg = "special/white.jpg" if t[1] == "1" else "special/black.jpg"
            return [f"bgload {self.bg}"]
        if cmd == "dayIntro":
            self.sprites, self.pan, self.bg = {}, None, f"{t[2].lower()}.jpg"
            return [f"bgload {self.bg}", "delay 90"]
        if cmd == "fgload":
            self.sprites[t[1]] = (t[3].lower(), t[4])
            return self.redraw()
        if cmd == "multifgload2":
            self.sprites = {"0": (t[2].lower(), t[3]), "1": (t[5].lower(), t[6])}
            return self.redraw()
        if cmd == "removeFG":
            self.sprites.pop(t[1], None)
            return self.redraw()
        if cmd == "removeAllFG":
            self.sprites = {}
            return self.redraw()
        if cmd == "moveFG" and t[1] in self.sprites:
            self.sprites[t[1]] = (self.sprites[t[1]][0], t[2])
            return self.redraw()
        if cmd == "playBGM":
            return [f"music track_{int(BGM_MAP.get(t[1], t[1])):02d}.ogg"]
        if cmd == "stopBGM":
            return ["music ~"]
        if cmd == "playSFX":
            return [f"sound se{int(t[1]):03d}.ogg" + (" -1" if len(t) > 2 and t[2] == "0" else "")]
        if cmd == "stopSFX":
            return ["sound ~"]
        if cmd == "delay":
            return [f"delay {t[1]}"]
        if cmd == "jump":
            return [f"jump {stem(t[1])}.scr"]
        if cmd == "movie":
            return [f"video {t[1]}"]
        if cmd == "returnTitle":
            bad_end = ["text <<Bad End>>"] if self.chapter == "c_yuka6" else []
            return ["music ~", "sound ~"] + bad_end + ["jump main.scr"]
        return []     # showClock, screen effects (cmd1/cmd2): not shown


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
    music = take_option(args, "--music", True) or "pc"
    if music not in ("pc", "psp"):
        sys.exit("--music must be pc or psp")
    no_movies = take_option(args, "--no-movies")
    no_append = take_option(args, "--no-append")
    movie_size = take_option(args, "--movie-size", True) or "320x240"
    m = re.fullmatch(r"(\d+)x(\d+)", movie_size)
    if not m or not (0 < int(m.group(1)) <= 512 and 0 < int(m.group(2)) <= 256):
        sys.exit("--movie-size must be WxH, at most 512x256")
    size_limit = (int(m.group(1)), int(m.group(2)))
    if len(args) < 3 or args[0].startswith("-"):
        sys.exit(__doc__)
    vnds, love, out, rest = args[0], args[1], args[2], args[3:]
    if not os.path.isfile(os.path.join(vnds, "img.ini")) or not os.path.isdir(os.path.join(vnds, "script")):
        sys.exit(f"{vnds}: not the VNDS Never 7 folder (needs script/ and img.ini)")
    archive = zipfile.ZipFile(love)
    if not any(n.startswith("upscale/background/") for n in archive.namelist()):
        sys.exit(f"{love}: not n7e.love (no upscale/background/)")

    notes = []
    work = tempfile.mkdtemp(prefix="n7scripts")
    state = {}

    # import_vnds as usual, with the asset folders swapped and the scripts added to
    sys.argv = [sys.argv[0], vnds, out, "--speakers", "--title", "Never 7"] + rest
    original_init = import_vnds.Converter.__init__

    def init(self, a):
        original_init(self, a)
        self.bgs = LoveOverlay(self.bgs, archive, ["upscale/background/"], images=True, pans=True)
        self.fgs = LoveOverlay(self.fgs, archive, ["upscale/foreground/"], images=True)
        self.snd = LoveOverlay(self.snd, archive, ["sound/", f"music/{music}/"])
        state["overlays"] = (self.bgs, self.fgs, self.snd)
        new = {}
        if not no_movies:
            vnds_lines = {stem(k): self.script_lines(k) for k in self.scr.files if k.endswith(".scr")}
            used, places = place_movies(archive, vnds_lines, music, notes.append)
            state.setdefault("movies", {}).update(used)
            for chapter, name, anchor in places:
                notes.append(f"movie {name} in {chapter}, by: {anchor[:70]}")
            for chapter in {p[0] for p in places}:
                new[chapter] = vnds_lines[chapter]
        if not no_append:
            parts = sorted(n for n in archive.namelist() if re.match(r"script/append/c_yuka\d+\.dec$", n))
            for member in parts:
                chapter = stem(member)
                text = archive.read(member).decode("utf-8", "replace")
                new[chapter] = AppendTranslator(self.fgs, chapter).translate(text)
                for line in new[chapter]:
                    movie = movie_for(chapter, line[6:], music) if line.startswith("video ") else None
                    if movie and not no_movies:
                        state.setdefault("movies", {})[movie[0]] = movie[1:]
                    elif line.startswith("video "):
                        line_at = new[chapter].index(line)
                        new[chapter][line_at] = "delay 1"    # --no-movies: no movie there
            if parts:
                new["main_append"] = [f"jump {stem(parts[0])}.scr"]
            state["append"] = len(parts)
        for chapter, lines in new.items():
            path = os.path.join(work, chapter + ".scr")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("\n".join(lines) + "\n")
            self.scr.files[chapter + ".scr"] = ("dir", path, chapter + ".scr")

    import_vnds.Converter.__init__ = init
    try:
        import_vnds.main()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if state.get("movies"):
        notes += make_movies(archive, state["movies"], out, size_limit, notes.append)
    for note in notes:
        print(note)
    bgs, fgs, snd = state["overlays"]
    print(f"from n7e.love: {len(bgs.replaced)} backgrounds, {len(fgs.replaced)} sprites, "
          f"{len(snd.replaced)} sounds ({music} music), {len(state.get('movies', {}))} movies, "
          f"Append Story in {state.get('append', 0)} parts; the rest from VNDS")


if __name__ == "__main__":
    main()
