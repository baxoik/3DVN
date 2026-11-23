#!/usr/bin/env python3
"""Convert Never 7 to 3DVN from "Never 7 - the end of infinity" for LÖVE (n7e.love) alone.

usage: import_never7.py N7E_LOVE OUT_GAME_DIR [--steam NEVER7_STEAM_FOLDER] [--music pc|psp]
                        [--no-movies] [--no-append]
                        [--movie-size WxH] [import_vnds options]

Everything comes from the .love file (read in place, nothing is extracted):
  - the story: the English scripts in script/*.dec (the original engine's commands as
    decoded by the port), translated to VNDS commands and then converted by
    import_vnds.py; each message is one line, as in the original
  - the Append Story (unless --no-append): Yuka's eight-part story, script/append/;
    the title menu offers it
  - backgrounds and sprites: upscale/ (1600x1200; the 3DS gets them scaled to its
    screen, PC / Wii U / Android draw the full-size ones from hd/). Tall panning CGs
    are cut into top / -middle / -bottom pieces; sprites named ...n / ...d are the
    night / dusk tints the port draws
  - voices and sound effects: sound/ (WAV, converted to Ogg Vorbis)
  - music: music/pc/ (or music/psp/ with --music psp). With --steam (the folder of the
    2025 Steam release, "steamapps/common/Never7"), each track is replaced by the Steam
    remaster's (Data/bgm_ogg.mpk: 48 kHz, ~150 kbps, a newer, wider-stereo master) where
    the two are recognised as the same piece by their sound; copied unchanged. The voices
    stay the .love's (uncompressed; Steam's are the same recordings, more compressed)
  - movies (unless --no-movies): the opening, the five heroine clips and the endings with
    their songs, where the scripts play them (video/, made with mk3vv.py)
The VNDS release is not needed (for that one alone, use import_vnds.py).
Only convert games you own.
"""
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import import_vnds  # noqa: E402

try:
    from PIL import Image
except ImportError:
    Image = None

TINTS = {"n": (0x7e, 0x99, 0xff), "d": (0xed, 0xb8, 0x7c)}   # night / dusk, as the port
PAN_PIECES = {"": 0.0, "-middle": 0.5, "-bottom": 1.0}         # cuts of a tall CG
SLOT_X = {"1": 400, "2": 200}          # sprite position -> centre on the 800 canvas; else 600
BGM_MAP = {"20": "21", "21": "22"}     # as the port's conv.lua
HEROINE_CLIPS = ("yuuka", "haruka", "saki", "kurumi", "izumi")
CANVAS = (800, 600)                    # the original's screen; upscale/ is twice that
UPSCALE = 2.0
HEROINES = (("d0", "izumi"), ("ce", "kurumi"), ("cd", "saki"), ("cc", "haruka"), ("cb", "yuka"))


def stem(name):
    """'sound/I26D000.waf.wav.wav' -> 'i26d000'; 'special/black.jpg' -> 'black'"""
    return re.sub(r"(\.[A-Za-z0-9]+)+$", "", os.path.basename(name.replace("\\", "/"))).lower()


def png(image):
    buf = io.BytesIO()
    image.save(buf, "PNG")
    return buf.getvalue()


class LoveAssets:
    """One kind of asset (pictures from upscale/..., or sounds) read from the .love file,
    with the lookup interface of import_vnds.Assets. Pictures may also be a piece of a tall
    panning CG (NAME-middle, NAME-bottom), a tinted sprite (NAMEn, NAMEd) or a plain
    colour (black, white)."""

    def __init__(self, archive, folders, images=False, pans=False):
        self.archive = archive
        self.images = images
        self.pans = pans
        self.members = {}
        for member in archive.namelist():
            if not member.endswith("/") and any(member.startswith(f) for f in folders):
                self.members.setdefault(stem(member), member)
        self.files = {}       # key -> name (keys: love:, piece:, tint:, plain:)
        self.override = {}    # name -> function giving other bytes for it (Steam music)
        self.cache = {}
        self.used = set()

    def find(self, name):
        s = stem(name)
        if s in self.members:
            return self._key("love:" + s, s + os.path.splitext(self.members[s])[1])
        if not self.images:
            return None
        if self.pans:
            for piece in PAN_PIECES:
                if piece and s.endswith(piece) and s[:-len(piece)] in self.members:
                    return self._key("piece:" + s, s + ".png")
            if s in ("black", "white"):
                return self._key("plain:" + s, s + ".png")
        if s[-1:] in TINTS and len(s) > 4 and self.find(s[:-1]) is not None:
            return self._key("tint:" + s, s + ".png")
        return None

    def _key(self, key, rel):
        self.files[key] = rel
        return key

    def rel(self, key):
        return self.files[key]

    def path(self, key):
        return None

    def read(self, key):
        if key in self.cache:
            return self.cache[key]
        kind, s = key.split(":", 1)
        self.used.add(key)
        if kind == "love" and s in self.override:
            return self.override[s]()
        if kind == "plain":
            size = (int(CANVAS[0] * UPSCALE), int(CANVAS[1] * UPSCALE))
            data = png(Image.new("RGB", size, (0, 0, 0) if s == "black" else (255, 255, 255)))
        elif kind == "tint":
            with Image.open(io.BytesIO(self.read(self.find(s[:-1])))) as im:
                r, g, b, a = im.convert("RGBA").split()
                r, g, b = (ch.point(lambda v, f=f: v * f // 255)
                           for ch, f in zip((r, g, b), TINTS[s[-1]]))
                data = png(Image.merge("RGBA", (r, g, b, a)))
        else:
            piece = next((p for p in PAN_PIECES if p and s.endswith(p)), "") if kind == "piece" else ""
            data = self.archive.read(self.members[s[:-len(piece)] if piece else s])
            if self.pans:
                with Image.open(io.BytesIO(data)) as im:
                    w, h = im.size
                    if h > w * 3 // 4 + 2:     # tall panning CG: a 4:3 piece of it
                        ph = w * 3 // 4
                        top = round((h - ph) * PAN_PIECES[piece])
                        data = png(im.crop((0, top, w, top + ph)))
        if len(data) < 8 << 20:
            self.cache[key] = data
        return data

    def factor(self, key):
        return UPSCALE if self.images else 1.0

    def canvas_width(self, name):
        key = self.find(name)
        if key is None:
            return None
        with Image.open(io.BytesIO(self.read(key))) as im:
            return round(im.width / UPSCALE)


# ---- the Steam release's music ---------------------------------------------------------

class Mpk:
    """MAGES .mpk archive (MPK\\0, version 2): name -> bytes."""

    def __init__(self, path):
        self.path = path
        self.entries = {}
        with open(path, "rb") as f:
            head = f.read(16)
            if head[:4] != b"MPK\0":
                raise ValueError(f"{path}: not an MPK archive")
            count = struct.unpack_from("<Q", head, 8)[0]
            f.seek(0x40)
            for _ in range(count):
                e = f.read(256)
                compressed, _, offset, csize, size = struct.unpack_from("<IIQQQ", e, 0)
                name = e[32:].split(b"\0")[0].decode("utf-8", "replace")
                if not compressed:
                    self.entries[name.lower()] = (offset, csize)

    def read(self, name):
        offset, size = self.entries[name]
        with open(self.path, "rb") as f:
            f.seek(offset)
            return f.read(size)


def sound_print(data):
    """A minute of the track as a normalised log-spectrogram, for telling pieces apart."""
    import numpy as np
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", "pipe:0", "-t", "60", "-ac", "1", "-ar", "8000",
                          "-f", "s16le", "pipe:1"], input=data, capture_output=True).stdout
    x = np.frombuffer(raw, dtype=np.int16).astype(float)
    frames = [np.abs(np.fft.rfft(x[i:i + 2048] * np.hanning(2048)))[8:400]
              for i in range(0, len(x) - 2048, 800)]
    if not frames:
        return None
    spec = np.log1p(np.array(frames))
    return (spec - spec.mean(0)) / (spec.std(0) + 1e-9)


def print_match(a, b):
    best = 0.0
    for shift in range(-15, 16):        # up to ~1.5 s apart
        x, y = a[max(0, shift):], b[max(0, -shift):]
        n = min(len(x), len(y))
        if n:
            best = max(best, float((x[:n] * y[:n]).mean()))
    return best


def steam_music(steam, archive, folder):
    """{track name: function giving the Steam remaster's Ogg}, for the .love's music/FOLDER/
    tracks that clearly match a Steam track: a strong match (score > 0.45; a close second
    is then another version of the same piece, e.g. vocal and instrumental), or a weaker
    one (> 0.35) well ahead of the rest (1.5x)."""
    data_dir = steam if os.path.isfile(os.path.join(steam, "bgm_ogg.mpk")) else os.path.join(steam, "Data")
    mpk = Mpk(os.path.join(data_dir, "bgm_ogg.mpk"))
    # bgmNN (the looping versions; bgmNNnl are the same without loop) and songNN (vocals)
    steam_tracks = sorted(n for n in mpk.entries if re.fullmatch(r"(bgm|song)\d+\.ogg", n))
    love_tracks = sorted(n for n in archive.namelist() if re.fullmatch(rf"music/{folder}/track_\d+\.ogg", n))
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        prints_s = dict(zip(steam_tracks, pool.map(lambda n: sound_print(mpk.read(n)), steam_tracks)))
        prints_l = dict(zip(love_tracks, pool.map(lambda n: sound_print(archive.read(n)), love_tracks)))
    chosen, notes = {}, []
    for track in love_tracks:
        if prints_l[track] is None:
            continue
        scores = sorted(((print_match(prints_l[track], p), n) for n, p in prints_s.items() if p is not None),
                        reverse=True)
        best, runner = scores[0], scores[1] if len(scores) > 1 else (0.0, "")
        if best[0] > 0.45 or (best[0] > 0.35 and best[0] > 1.5 * runner[0]):
            chosen[stem(track)] = (lambda n=best[1]: mpk.read(n))
            notes.append(f"music {stem(track)}: Steam {best[1]} (match {best[0]:.2f})")
        else:
            notes.append(f"music {stem(track)}: kept the .love's (no clear Steam match, best {best[0]:.2f})")
    return chosen, notes


# ---- movies -------------------------------------------------------------------------

def movie_for(chapter, arg, music):
    """(video name, .ogv in the .love, song in the .love or None), as the port's video.scr"""
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


def make_movies(archive, used, out, size_limit, songs=None):
    if not shutil.which("ffmpeg"):
        return ["warning: movies not converted (needs ffmpeg)"]
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
            return f"warning: movie {name} not converted: {result.stderr.strip()[-200:]}"
        # the sound: the movie's own track or its song, as Ogg Vorbis (the engine tells the
        # format by its content, so it keeps the name video/NAME.wav)
        sound = src
        if song:
            sound = os.path.join(work, name + ".song.ogg")
            with open(sound, "wb") as handle:
                handle.write(songs[song]() if songs and song in songs else archive.read(song))
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


# ---- scripts: the original engine's commands -> VNDS ---------------------------------

VAR = r"VAR_\([0-9a-f]+_[0-9a-f]+\)_(a[0-9a-f]_[0-9a-f]{2})"
# one condition: VAR (type) OP (type) [(code)]->VALUE, the value a number or another VAR
TERM = re.compile(VAR + r" \(\w+\) (\S+)\s+\(\w+\) (?:\(\w+\))?->"
                  r"(VAR_\([0-9a-f]+_[0-9a-f]+\)_a[0-9a-f]_[0-9a-f]{2}|-?\w+)")
OPS = {"==", "!=", "<", ">", "<=", ">="}


def var_name(token):
    m = re.fullmatch(VAR, token)
    return "v_" + m.group(1) if m else token


def is_global(var):
    """As the port: a0_c8 .. a0_f9 are kept across playthroughs (routes cleared ...)."""
    m = re.fullmatch(r"v_a0_([0-9a-f]{2})", var)
    return bool(m) and 200 <= int(m.group(1), 16) < 250


class SceneTranslator:
    """The original engine's scene commands (pictures, sprites, sound) -> VNDS lines."""

    def __init__(self, fgs, chapter, music):
        self.fgs = fgs
        self.chapter = chapter
        self.music = music
        self.bg = "special/black.png"
        self.pan = None
        self.sprites = {}     # slot -> (name, position)
        self.movies = {}
        self.warnings = []

    def sprite_lines(self):
        out = []
        for slot in sorted(self.sprites):
            name, pos = self.sprites[slot]
            width = self.fgs.canvas_width(name)
            if width is None:
                self.warnings.append(f"missing sprite {name}")
                continue
            out.append(f"setimg {name}.png {SLOT_X.get(pos, 600) - width // 2} 0")
        return out

    def redraw(self):
        return [f"bgload {self.bg}"] + self.sprite_lines()

    def command(self, t):
        cmd = t[0]
        if cmd in ("bgload", "createPanBG"):
            name = stem(t[2])
            self.sprites = {}
            if len(t) > 3 and t[3] == "2":           # panning CG, shown from the bottom
                self.pan, self.bg = name, f"{name}-bottom.png"
            else:
                self.pan, self.bg = None, f"{name}.png"
            return [f"bgload {self.bg}"]
        if cmd == "movePanBG":
            if not self.pan:
                return []
            steps = [f"{self.pan}-middle.png"] + ([f"{self.pan}.png"] if t[1] == "0" else [])
            self.bg = steps[-1]
            return [f"bgload {s} 18" for s in steps] + self.sprite_lines()
        if cmd in ("removeBG", "removeD"):
            self.sprites, self.pan = {}, None
            white = cmd == "removeBG" and t[1] == "1"
            self.bg = "special/white.png" if white else "special/black.png"
            return [f"bgload {self.bg}"]
        if cmd == "dayIntro":
            self.sprites, self.pan, self.bg = {}, None, f"{stem(t[2])}.png"
            return [f"bgload {self.bg}", "delay 90"]
        if cmd == "fgload":
            self.sprites[t[1]] = (stem(t[3]), t[4])
            return self.redraw()
        if cmd == "multifgload2":
            self.sprites = {"0": (stem(t[2]), t[3]), "1": (stem(t[5]), t[6])}
            return self.redraw()
        if cmd == "removeFG":
            self.sprites.pop(t[1], None)
            return self.redraw()
        if cmd == "removeAllFG":
            self.sprites = {}
            return self.redraw()
        if cmd == "moveFG":
            if t[1] in self.sprites:
                self.sprites[t[1]] = (self.sprites[t[1]][0], t[2])
            return self.redraw()
        if cmd == "playBGM":
            return [f"music track_{int(BGM_MAP.get(t[1], t[1])):02d}.ogg"]
        if cmd == "stopBGM":
            return ["music ~"]
        if cmd == "playSFX":
            name = t[1].lower() if not t[1].isdigit() else f"se{int(t[1]):03d}"
            return [f"sound {name}.ogg" + (" -1" if len(t) > 2 and t[2] == "0" else "")]
        if cmd == "playVoice":
            return [f"sound {t[1].lower()}.ogg"]
        if cmd == "stopSFX":
            return ["sound ~"]
        if cmd == "delay":
            return [f"delay {t[1]}"]
        if cmd == "movie":
            movie = movie_for(self.chapter, t[1], self.music)
            if movie is None:
                self.warnings.append(f"unknown movie {t[1]}")
                return []
            self.movies[movie[0]] = movie[1:]
            return [f"video {movie[0]}"] + (["music track_25.ogg"] if movie[0] == "end2" else [])
        return []     # showClock, screen effects (cmd1 ...), waits: not shown


def message(line):
    """A message line '{textColor 0 0}【Name】 "..."' -> 'text [Name] "..."' (or None)."""
    text = re.sub(r"\{[^}]*\}", "", line).replace("{", "")
    text = re.sub(r"\s{2,}", " ", text).strip()
    if not text:
        return None
    m = re.match(r"【([^】]*)】\s*(.*)$", text)
    return f"text [{m.group(1)}] {m.group(2)}" if m else f"text {text}"


class DecTranslator(SceneTranslator):
    """script/NAME.dec (decoded original scripts: '[address] op: command' lines, each
    'text' followed by its message lines) -> VNDS .scr lines."""

    def __init__(self, fgs, chapter, music, chapters):
        super().__init__(fgs, chapter, music)
        self.chapters = chapters
        self.calls = 0
        self.skips = 0

    def translate(self, text):
        lines = text.splitlines()
        # jump targets: an address between two lines means the next line, one past the
        # last line the end of the script (0123abcd is the dummy of skipped conditions)
        targets = set(re.findall(r"(?:\* |goto7 )([0-9a-f]{8})", text)) - {"0123abcd"}
        addresses = sorted({int(a, 16) for a in re.findall(r"^\[([0-9a-f]{8})\]", text, re.M)})
        at_line, at_end = {}, []
        for target in sorted(targets):
            following = [a for a in addresses if a >= int(target, 16)]
            if following:
                at_line.setdefault(f"{following[0]:08x}", []).append(target)
            else:
                at_end.append(target)
        labelled, out, options, in_text = set(), [], [], False
        for raw in lines:
            m = re.match(r"\[([0-9a-f]{8})\]\s*[0-9a-f]+:\s?(.*)$", raw)
            if not m:
                if in_text:
                    options += re.findall(r"\{choiceOpt \d+ -> ([^}]*)\}", raw)
                    if "{choiceOpt" not in raw:
                        msg = message(raw)
                        if msg:
                            out.append(msg)
                continue
            address, body = m.groups()
            in_text = False
            if address not in labelled:
                labelled.add(address)
                out += [f"label L{target}" for target in at_line.get(address, [])]
            t = body.split()
            if not t:
                continue
            if t[0] in ("text", "notice"):
                in_text = True
            elif t[0] == "choice":
                out.append("choice " + "|".join(o.strip().replace("|", "/") for o in options))
                var = var_name(t[1])
                out += [f"setvar {var} = selected", f"setvar {var} - 1"]
                options = []
            else:
                out += self.command_dec(t, body)
        return out + [f"label L{target}" for target in at_end]

    def command_dec(self, t, body):
        cmd = t[0]
        if cmd == "varop":
            return self.varop(t)
        if cmd == "condJump":
            return self.cond_jump(body)
        if cmd == "goto7":
            return [f"goto L{t[1]}"]
        if cmd == "jump":
            target = t[1].lower()
            if target == "staffroll":                   # a bad end: back to the title
                return ["music ~", "sound ~", "text <<Bad End>>", "jump main.scr"]
            if target not in self.chapters:
                self.warnings.append(f"jump to {t[1]} (not in the game) skipped")
                return []
            return [f"jump {target}.scr"]
        if cmd == "call5":                               # a side scene, then back here
            self.calls += 1
            back = f"ret{self.calls}"
            return [f'setvar RETFILE = "{self.chapter}.scr"', f'setvar RETLABEL = "{back}"',
                    f"jump {t[2].lower()}.scr", f"label {back}"]
        if cmd == "return":
            return ["jump $RETFILE $RETLABEL"]
        if cmd == "returnTitle":
            return ["music ~", "sound ~", "jump main.scr"]
        return self.command(t)

    def varop(self, t):
        var, op, value = var_name(t[1]), t[3], var_name(t[5])
        setvar = "gsetvar" if is_global(var) else "setvar"
        if op == ":=":
            return [f"{setvar} {var} = {value}"]
        if op in ("+=", "-="):
            return [f"{setvar} {var} {op[0]} {value}"]
        if op == "|=":          # only ever ORs 0/1 route flags together
            return [f"if {value} != 0", f"    {setvar} {var} = 1", "fi"]
        if op == "*=" and value.isdigit():
            return [f"setvar _mul = {var}"] + [f"{setvar} {var} + _mul"] * (int(value) - 1)
        self.warnings.append(f"variable operation {op} skipped")
        return []

    def cond_jump(self, body):
        target = re.search(r"\* ([0-9a-f]{8})", body)
        terms = list(TERM.finditer(body))
        if not target or not terms:      # _COP_87 forms: skipped by the port as well
            return []
        conds = []
        for m in terms:
            op = m.group(2)
            if op not in OPS:        # _COP_03 / _COP_87: skipped by the port as well
                return []
            conds.append(f"v_{m.group(1)} {op} {var_name(m.group(3))}")
        joins = [next((j for j in ("||", "&&", "CJ_18") if f" {j} " in body[a.end():b.start()]), "?")
                 for a, b in zip(terms, terms[1:])]
        goto = f"    goto L{target.group(1)}"
        if not joins:
            return [f"if {conds[0]}", goto, "fi"]
        if all(j == "||" for j in joins):
            return [x for c in conds for x in (f"if {c}", goto, "fi")]
        if all(j == "&&" for j in joins):
            out = [f"{'    ' * i}if {c}" for i, c in enumerate(conds)]
            out.append("    " * len(conds) + goto.strip())
            out += [f"{'    ' * i}fi" for i in reversed(range(len(conds)))]
            return out
        if joins == ["CJ_18"]:     # as the port: jump if the first does not hold, the second does
            self.skips += 1
            skip = f"skip{self.skips}"
            return [f"if {conds[0]}", f"    goto {skip}", "fi", f"if {conds[1]}", goto, "fi",
                    f"label {skip}"]
        self.warnings.append("mixed condition skipped")
        return []


class AppendTranslator(SceneTranslator):
    """script/append/c_yukaN.dec (VNDS-like '@' lines mixed with the original engine's
    commands) -> VNDS .scr lines."""

    def translate(self, text):
        out = []
        for raw in text.splitlines():
            line = raw.strip()
            if not line:
                continue
            if line.startswith("@"):
                body = line[1:].strip()
                if body.startswith("text "):
                    out.append(message(body[5:]) or "text ~")
                elif body == "setvar vap_seldt = selected":
                    out += ["setvar seldt = selected", "setvar seldt - 1"]
                elif body:
                    out.append(body)
                continue
            t = line.split()
            if t[0] == "jump":
                out.append(f"jump {stem(t[1])}.scr")
            elif t[0] == "returnTitle":
                bad_end = ["text <<Bad End>>"] if self.chapter == "c_yuka6" else []
                out += ["music ~", "sound ~"] + bad_end + ["jump main.scr"]
            else:
                out += self.command(t)
        return out


def title_script(append):
    """The title: the port's picture, a heroine added for each route cleared."""
    out = ["music track_21.ogg", "bgload special/title1_bg.png"]
    for flag, name in HEROINES:
        out += [f"if v_a0_{flag} != 0", f"    setimg special/title-{name}.png 0 0", "fi"]
    out += ["text ~", "choice Start Game" + ("|Append Story" if append else ""),
            "if selected == 1", "    music ~", "    bgload special/black.png", "    jump init.scr", "fi"]
    if append:
        out += ["if selected == 2", "    music ~", "    bgload special/black.png",
                "    jump c_yuka0.scr", "fi"]
    return out


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
    steam = take_option(args, "--steam", True)
    movie_size = take_option(args, "--movie-size", True) or "320x240"
    m = re.fullmatch(r"(\d+)x(\d+)", movie_size)
    if not m or not (0 < int(m.group(1)) <= 512 and 0 < int(m.group(2)) <= 256):
        sys.exit("--movie-size must be WxH, at most 512x256")
    size_limit = (int(m.group(1)), int(m.group(2)))
    if len(args) < 2 or args[0].startswith("-"):
        sys.exit(__doc__)
    love, out, rest = args[0], args[1], args[2:]
    if os.path.isdir(love):
        sys.exit(f"{love} is a folder: give the n7e.love file (the VNDS release is not needed;"
                 " for it alone use import_vnds.py)")
    archive = zipfile.ZipFile(love)
    names = archive.namelist()
    if "script/init.dec" not in names or not any(n.startswith("upscale/background/") for n in names):
        sys.exit(f"{love}: not n7e.love (no script/init.dec, upscale/background/)")

    # a VNDS-shaped source folder: the translated scripts, img.ini; pictures and sounds
    # are read from the .love file by LoveAssets
    src = tempfile.mkdtemp(prefix="n7src")
    notes, movies = [], {}
    try:
        os.makedirs(os.path.join(src, "script"))
        with open(os.path.join(src, "img.ini"), "w") as handle:
            handle.write(f"width={CANVAS[0]}\nheight={CANVAS[1]}\n")
        bgs = LoveAssets(archive, ["upscale/background/"], images=True, pans=True)
        fgs = LoveAssets(archive, ["upscale/foreground/"], images=True)
        snd = LoveAssets(archive, ["sound/", f"music/{music}/"])
        songs = {}
        if steam:
            chosen, music_notes = steam_music(steam, archive, music)
            snd.override.update(chosen)
            songs = {f"music/{music}/{name}.ogg": get for name, get in chosen.items()}
            notes += music_notes

        chapters = {stem(n): n for n in names
                    if re.fullmatch(r"script/[a-z0-9_]+\.dec", n)}     # (not ...-unlock-...)
        scripts = {}
        for chapter, member in sorted(chapters.items()):
            tr = DecTranslator(fgs, chapter, music, chapters)
            scripts[chapter] = tr.translate(archive.read(member).decode("utf-8", "replace"))
            movies.update(tr.movies)
            notes += [f"warning: {chapter}: {w}" for w in sorted(set(tr.warnings))]
        parts = [] if no_append else sorted(
            n for n in names if re.fullmatch(r"script/append/c_yuka\d+\.dec", n))
        for member in parts:
            tr = AppendTranslator(fgs, stem(member), music)
            scripts[stem(member)] = tr.translate(archive.read(member).decode("utf-8", "replace"))
            movies.update(tr.movies)
            notes += [f"warning: {stem(member)}: {w}" for w in sorted(set(tr.warnings))]
        scripts["main"] = title_script(bool(parts))
        for chapter, lines in scripts.items():
            if no_movies:
                lines = [("delay 1" if l.startswith("video ") else l) for l in lines]
            with open(os.path.join(src, "script", chapter + ".scr"), "w", encoding="utf-8") as handle:
                handle.write("\n".join(lines) + "\n")

        sys.argv = [sys.argv[0], src, out, "--speakers", "--title", "Never 7"] + rest
        original_init = import_vnds.Converter.__init__

        def init(self, a):
            original_init(self, a)
            self.bgs, self.fgs, self.snd = bgs, fgs, snd

        import_vnds.Converter.__init__ = init
        import_vnds.main()
    finally:
        shutil.rmtree(src, ignore_errors=True)
    if movies and not no_movies:
        notes += make_movies(archive, movies, out, size_limit, songs)
    for note in notes:
        print(note)
    print(f"from n7e.love: {len(scripts) - 1} scripts ({len(parts)} Append Story), "
          f"{len(bgs.used)} backgrounds, {len(fgs.used)} sprites, {len(snd.used)} sounds "
          f"({music} music), {0 if no_movies else len(movies)} movies")


if __name__ == "__main__":
    main()
