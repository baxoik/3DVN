#!/usr/bin/env python3
"""Convert a VNDS visual novel into a 3DVN game folder.

usage: import_vnds.py SRC_VNDS_DIR OUT_GAME_DIR [options]   (see --help)

SRC is a VNDS game: script/ (or script.zip) with main.scr, and background/, foreground/,
sound/ as folders or as background.zip, foreground.zip, sound.zip (folders win when both
exist). img.ini / info.txt are used when present. Only convert games you own or that are
freely licensed.

Output (OUT):
  main.3vs, ch/<script>.3vs   one 3DVN chapter file per VNDS script (3DVN loads one at a
                              time, so even CLANNAD-sized games fit a 3DS)
  characters.3vs              speakers (with --speakers)
  bg/, sprites/               pictures scaled to the 3DS screen (400x240)
  hd/bg/, hd/sprites/         the originals, drawn sharper by PC, Wii U and Android
  audio/                      sound as Ogg Vorbis (Ogg sources copied unchanged), folders kept
  game.3vc                    title and display settings
"""
import argparse, concurrent.futures, io, math, os, re, shutil, subprocess, sys, zipfile
from collections import Counter, OrderedDict

try:
    from PIL import Image
except ImportError:
    Image = None

RESERVED = {"true", "false", "and", "or", "not"}
SCREEN_W, SCREEN_H = 400, 240
DEFAULT_SOURCE_SIZE = (256, 192)
MAX_CHOICES = 8
NVL_PAGE_CHARS = 420       # --nvl: start a new page after about this much text

report = Counter()
notes = []


def warn(key, msg=None):
    report[key] += 1
    if msg and report[key] <= 3:
        notes.append(f"{key}: {msg}")


def find_ffmpeg(explicit=None):
    """--ffmpeg path, then PATH, then the imageio-ffmpeg pip package."""
    if explicit:
        return explicit if os.path.isfile(explicit) else shutil.which(explicit)
    found = shutil.which("ffmpeg")
    if found:
        return found
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        return None


def ident(s, prefix=""):
    s = re.sub(r"[^A-Za-z0-9_]", "_", s)
    s = prefix + s
    if not s or s[0].isdigit() or s.lower() in RESERVED:
        s = "v_" + s
    return s[:31]


def quote(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("{", "\\{").replace("\n", "\\n")
    return '"' + s + '"'


def strip_ext(text):
    """"x.png" -> "x", ".png" -> "" (os.path.splitext keeps a lone ".png")."""
    return re.sub(r"\.[A-Za-z0-9]{2,4}$", "", text)


def safe_name(text):
    return re.sub(r'[^A-Za-z0-9_./-]', "_", text.replace("\\", "/"))


def value(tok):
    return tok if re.fullmatch(r"-?\d+", tok) else quote(tok)


def source_size(text):
    match = re.fullmatch(r"(\d+)[xX](\d+)", text)
    if not match or int(match.group(1)) == 0 or int(match.group(2)) == 0:
        raise argparse.ArgumentTypeError("source size must be WIDTHxHEIGHT with positive integers")
    return int(match.group(1)), int(match.group(2))


def positive_scale(text):
    try:
        scale = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError("scale must be a positive finite number")
    if not math.isfinite(scale) or scale <= 0:
        raise argparse.ArgumentTypeError("scale must be a positive finite number")
    return scale


class Assets:
    """One kind of VNDS asset (background, foreground, sound, script) from its folder and/or
    its zip, looked up case-insensitively by path relative to that folder."""

    def __init__(self, src, kind):
        self.files = OrderedDict()   # lower rel path -> ("dir", path) or ("zip", zipfile, member)
        folder = os.path.join(src, kind)
        if os.path.isdir(folder):
            for root, _, files in os.walk(folder):
                for f in files:
                    rel = os.path.relpath(os.path.join(root, f), folder).replace(os.sep, "/")
                    self.files[rel.lower()] = ("dir", os.path.join(root, f), rel)
        archive = os.path.join(src, kind + ".zip")
        if os.path.isfile(archive):
            z = zipfile.ZipFile(archive)
            for member in z.namelist():
                if member.endswith("/"):
                    continue
                rel = member.replace("\\", "/")
                if rel.lower().startswith(kind.lower() + "/"):   # zips often keep the folder name
                    rel = rel[len(kind) + 1:]
                self.files.setdefault(rel.lower(), ("zip", z, member, rel))

    def find(self, name):
        key = name.replace("\\", "/").lstrip("/").lower()
        if key in self.files:
            return key
        # VNDS scripts sometimes leave the extension off, or use a different one
        stem = os.path.splitext(key)[0]
        for candidate in self.files:
            if os.path.splitext(candidate)[0] == stem:
                return candidate
        return None

    def rel(self, key):
        return self.files[key][-1]

    def read(self, key):
        entry = self.files[key]
        if entry[0] == "dir":
            with open(entry[1], "rb") as handle:
                return handle.read()
        return entry[1].read(entry[2])

    def path(self, key):
        """A real file path (extracting from the zip to a temporary file if needed)."""
        entry = self.files[key]
        return entry[1] if entry[0] == "dir" else None


class Converter:
    def __init__(self, a):
        self.a = a
        self.out = a.out
        self.bgs = Assets(a.src, "background")
        self.fgs = Assets(a.src, "foreground")
        self.snd = Assets(a.src, "sound")
        self.scr = Assets(a.src, "script")
        self.used = {"bg": set(), "fg": set(), "snd": {}, "music": set()}
        self.chars = {}
        self.sprites = []
        self.globals = set()
        self.cross_targets = {}   # chapter -> labels jumped to from other chapters

    # ---- setup ----
    def scripts(self):
        found = OrderedDict()
        for key in self.scr.files:
            if key.endswith(".scr"):
                name = os.path.splitext(os.path.basename(key))[0].lower()
                found.setdefault(name, key)
        if "main" not in found:
            sys.exit("no main.scr found (in script/ or script.zip)")
        return found

    def script_lines(self, key):
        data = self.scr.read(key)
        for encoding in ("utf-8-sig", "cp932", "latin-1"):
            try:
                return data.decode(encoding).splitlines()
            except UnicodeDecodeError:
                continue
        return data.decode("utf-8", "replace").splitlines()

    def configure_canvas(self, scripts):
        if self.a.source_size:
            size, origin = self.a.source_size, "--source-size"
        else:
            ini = {}
            if os.path.isfile(os.path.join(self.a.src, "img.ini")):
                for line in open(os.path.join(self.a.src, "img.ini"), encoding="utf-8", errors="replace"):
                    if "=" in line:
                        k, v = line.split("=", 1)
                        ini[k.strip().lower()] = v.strip()
            if ini.get("width", "").isdigit() and ini.get("height", "").isdigit():
                size, origin = (int(ini["width"]), int(ini["height"])), "img.ini"
            else:
                sizes = Counter()
                for key in list(self.bgs.files)[:40]:
                    try:
                        with Image.open(io.BytesIO(self.bgs.read(key))) as image:
                            sizes[image.size] += 1
                    except (OSError, ValueError):
                        continue
                if sizes:
                    size, origin = max(sizes, key=lambda s: (sizes[s], s[0] * s[1])), "background sizes"
                else:
                    size, origin = DEFAULT_SOURCE_SIZE, "default"
        self.source = size
        self.scale = self.a.scale if self.a.scale is not None else min(SCREEN_W / size[0], SCREEN_H / size[1])
        canvas_w = max(1, round(size[0] * self.scale))
        canvas_h = max(1, round(size[1] * self.scale))
        self.xoff = (SCREEN_W - canvas_w) / 2
        self.yoff = (SCREEN_H - canvas_h) / 2
        print(f"source canvas: {size[0]}x{size[1]} ({origin}); scale {self.scale:.4g}, "
              f"3DS picture {canvas_w}x{canvas_h}")

    def scan_globals_and_targets(self, scripts):
        """Before converting: which names are global (gsetvar -> g_...), which are assigned at
        all (a bare word naming one means its value, as in VNDS), which variables hold file
        names ("bgload $x": their values become 3DVN asset names) and which labels can be
        entered from another script (static "jump FILE LABEL" or a label kept in a variable)."""
        self.assigned = {"selected"}
        self.file_vars = {}       # VNDS variable -> "bg" / "fg" / "snd" / "scr"
        label_vars = set()
        values = {}               # VNDS variable -> literal values assigned to it
        self.labels = {}          # chapter -> labels defined there
        for name, key in scripts.items():
            self.labels[name] = set()
            for line in self.script_lines(key):
                parts = line.strip().lstrip("\ufeff").split()
                if not parts:
                    continue
                cmd = parts[0].lower()
                if cmd in ("setvar", "gsetvar", "random") and len(parts) >= 2 and parts[1] != "~":
                    self.assigned.add(parts[1])
                    if cmd == "gsetvar":
                        self.globals.add(parts[1])
                    if cmd != "random" and len(parts) >= 4:
                        raw = line.strip().split(None, 3)[3].strip()
                        values.setdefault(parts[1], set()).add(raw[1:-1] if len(raw) >= 2 and raw[0] == raw[-1] == '"' else raw)
                elif cmd == "label" and len(parts) >= 2:
                    self.labels[name].add(parts[1])
                elif cmd in ("bgload", "setimg", "sound", "music", "jump") and len(parts) >= 2:
                    t = self.template(parts[1])
                    if t:
                        kind = {"bgload": "bg", "setimg": "fg", "jump": "scr", "music": "music"}.get(cmd, "snd")
                        specs = self.file_vars.setdefault(t[0], [])
                        if (kind, t[1], t[2]) not in specs:
                            specs.append((kind, t[1], t[2]))
                    if cmd == "jump" and len(parts) >= 3:
                        label_var = self.dollar(parts[2])
                        if label_var:
                            label_vars.add(label_var)
                        else:
                            target = os.path.splitext(parts[1].replace("\\", "/").split("/")[-1])[0].lower()
                            self.cross_targets.setdefault(target, set()).add(parts[2])
        dynamic = set()
        for var in label_vars:
            dynamic |= values.get(var, set())
        for name, labels in self.labels.items():
            if dynamic & labels:
                self.cross_targets.setdefault(name, set()).update(dynamic & labels)

    @staticmethod
    def template(token):
        """A file name made from one variable, like "$x", "{$x}.png" or "bg_$x.jpg" ->
        (name, prefix, suffix); None if it has no variable."""
        found = list(re.finditer(r"\{\$([A-Za-z_][A-Za-z0-9_]*(?:\[\d+\])?)\}|\$([A-Za-z_][A-Za-z0-9_]*(?:\[\d+\])?)", token))
        if len(found) != 1:
            return None
        m = found[0]
        return (m.group(1) or m.group(2)), token[:m.start()], token[m.end():]

    def dollar(self, token):
        t = self.template(token)
        return t[0] if t and not t[1] and not t[2] else None

    def file_ref(self, token):
        """The 3DVN file name for a VNDS template: {v_x} with the literal parts kept and the
        extension dropped (3DVN finds .png/.jpg/.ogg itself)."""
        name, prefix, suffix = self.template(token)
        return safe_name(prefix) + "{" + self.var(name) + "}" + safe_name(strip_ext(suffix))

    def ref(self, name):
        """A variable in a file-name position: {v_name}."""
        return "{" + self.var(name) + "}"

    def operand(self, raw, target=None):
        """A setvar / if value: quoted = literal text; a bare word naming an assigned variable
        = that variable; otherwise a number or literal text. Literals assigned to a variable
        that holds file names become 3DVN asset (or chapter) names."""
        quoted = len(raw) >= 2 and raw[0] == raw[-1] == '"'
        text = raw[1:-1] if quoted else raw
        if not quoted and text in self.assigned:
            return self.var(text)
        specs = self.file_vars.get(target)
        if specs and text not in ("", "~"):
            # every pattern the variable is used in ("X.png", "X_SEPIA.png"...): convert
            # all the files it can name; the value comes from the first that exists
            results = [self.file_value(spec, text) for spec in specs]
            found = [r for r in results if r is not None]
            if found:
                return quote(found[0])
            warn("file named in a variable is missing", text)
        if not quoted and re.fullmatch(r"-?\d+", text):
            return text
        return quote(text)

    def file_value(self, spec, text):
        """A literal assigned to a file-name variable -> the value that makes the 3DVN
        template name the converted file (exact case); marks that file as used."""
        kind, prefix, suffix = spec
        if kind == "scr":
            stem = os.path.splitext(text.replace("\\", "/").split("/")[-1])[0].lower()
            return ident(stem) if stem in self.script_names else None
        assets = {"bg": self.bgs, "fg": self.fgs, "snd": self.snd, "music": self.snd}[kind]
        key = assets.find(prefix + text + suffix)
        if key is None:
            return None
        if kind in ("snd", "music"):
            self.used["snd"][key] = self.asset_name(assets, key)
            if kind == "music":
                self.used["music"].add(key)
        else:
            self.used[kind].add(key)
        full = self.asset_name(assets, key)
        head, tail = len(safe_name(prefix)), len(safe_name(strip_ext(suffix)))
        if full[:head].lower() != safe_name(prefix).lower() or len(full) < head + tail:
            warn("file named in a variable doesn't fit its pattern", prefix + text + suffix)
            return None
        return full[head:len(full) - tail]

    def text_literal(self, text):
        """Dialogue text with VNDS "$name" / "{$name}" turned into 3DVN {name}."""
        out, at = [], 0
        for m in re.finditer(r"\{\$([A-Za-z_][A-Za-z0-9_]*(?:\[\d+\])?)\}|\$([A-Za-z_][A-Za-z0-9_]*(?:\[\d+\])?)", text):
            name = m.group(1) or m.group(2)
            if name not in self.assigned:
                continue
            out.append(quote(text[at:m.start()])[1:-1])
            out.append("{" + self.var(name) + "}")
            at = m.end()
        out.append(quote(text[at:])[1:-1])
        return '"' + "".join(out) + '"'

    def var(self, name):
        return ident(name, "g_") if name in self.globals else ident(name, "v_")

    def label(self, name):
        return ident(name, "l_")

    # ---- scripts ----
    def convert_script(self, name, key, scripts):
        out = [f"# converted from {self.scr.rel(key)}"]
        if self.a.speakers:
            out.append("include characters.3vs")
        targets = sorted(self.cross_targets.get(name, ()))
        if targets:
            # entry from another script's "jump FILE LABEL"
            for t in targets:
                out += [f'if _vnds_entry == {quote(t)}', '  set _vnds_entry = ""', f"  jump {self.label(t)}", "end"]
        out.append('set _vnds_entry = ""')
        self.current_out = out
        depth = 0
        held = []          # "text @..." lines: shown together with the next text line
        page_chars = 0
        self.sprites = []
        for n, raw in enumerate(self.script_lines(key), 1):
            # $'NAME' (Kanon) is the plain name NAME
            line = re.sub(r"\$'([^'\s]*)'", r"\1", raw.strip().lstrip("\ufeff"))
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 1)
            cmd, arg = parts[0].lower(), (parts[1] if len(parts) > 1 else "")
            ind = "  " * depth
            where = f"{self.scr.rel(key)}:{n}"

            def emit(s):
                out.append(ind + s)

            if cmd == "text":
                text = arg.strip()
                if text.startswith("@"):
                    held.append(text[1:].strip())
                    continue
                if text in ("~", "!", ""):
                    if text == "!" and held:
                        text = ""
                    else:
                        continue
                text = "\n".join(held + ([text] if text else []))
                held = []
                if self.a.nvl:
                    if page_chars + len(text) > NVL_PAGE_CHARS and page_chars:
                        emit("page")
                        page_chars = 0
                    page_chars += len(text) + 1
                    self.say(text + "\n", emit)
                else:
                    self.say(text, emit)
                continue
            if held:   # "@" text followed by something else: show it on its own
                self.say("\n".join(held), emit)
                held = []
            if cmd == "cleartext":
                if self.a.nvl:
                    emit("page")
                    page_chars = 0
            elif cmd == "bgload":
                self.bgload(arg, emit, where)
            elif cmd == "setimg":
                self.setimg(arg, emit, where)
            elif cmd in ("sound", "music") or cmd in ("sound~", "music~"):
                if cmd.endswith("~"):
                    cmd, arg = cmd[:-1], "~"
                self.audio(cmd, arg, emit, where)
            elif cmd == "choice":
                opts = [o.strip() for o in arg.split("|") if o.strip()]
                self.menu(opts, 1, ind)
            elif cmd in ("setvar", "gsetvar"):
                m = re.match(r"(\S+)\s+(\S+)\s*(.*)$", arg)
                if not m:
                    warn("malformed setvar skipped", where)
                elif m.group(1) == "~":
                    warn("'setvar ~' (clear all variables) skipped", where)
                else:
                    var, op, val = m.group(1), m.group(2), m.group(3).strip()
                    if op not in ("=", "+", "-"):
                        warn("malformed setvar skipped", where)
                    else:
                        o = {"=": "=", "+": "+=", "-": "-="}[op]
                        emit(f"set {self.var(var)} {o} {self.operand(val, var) if val else '0'}")
            elif cmd == "if":
                m = re.match(r"(\S+?)\s*(==|!=|<=|>=|<|>|=)\s*(.+)$", arg)
                if not m:
                    warn("malformed if (replaced by if false)", where)
                    emit("if false")
                else:
                    op = "==" if m.group(2) == "=" else m.group(2)
                    emit(f"if {self.var(m.group(1))} {op} {self.operand(m.group(3).strip())}")
                depth += 1
            elif cmd == "else":
                out.append("  " * max(depth - 1, 0) + "else")
            elif cmd == "fi":
                if depth == 0:
                    warn("fi without if skipped", where)
                    continue
                depth -= 1
                out.append("  " * depth + "end")
            elif cmd == "goto":
                emit(f"jump {self.label(arg.strip())}")
            elif cmd == "label":
                emit(f"label {self.label(arg.strip())}")
            elif cmd == "jump":
                bits = arg.split()
                if not bits:
                    warn("malformed jump skipped", where)
                    continue
                file_var = self.dollar(bits[0])
                if file_var:   # script (and label) kept in variables: VNDS "call and return"
                    if len(bits) > 1:
                        label_var = self.dollar(bits[1])
                        emit("set _vnds_entry = " + (self.var(label_var) if label_var else quote(bits[1])))
                    emit(f"script ch/{self.ref(file_var)}")
                    continue
                target = os.path.splitext(bits[0].replace("\\", "/").split("/")[-1])[0].lower()
                if target not in scripts:
                    warn("jump to a missing script skipped", where + " " + bits[0])
                    continue
                if target == "main" and not self.a.keep_main_loop:
                    # back to the VNDS title script = the end of a route: end the story, so
                    # 3DVN counts it and returns to its own title screen
                    emit("stop")
                    continue
                if len(bits) > 1:
                    emit(f"set _vnds_entry = {quote(bits[1])}")
                emit(f"script ch/{ident(target)}")
            elif cmd == "delay":
                try:
                    frames = int(arg.split()[0])
                    if frames > 0:
                        emit(f"wait {frames / 60:.2f}")
                except (ValueError, IndexError):
                    warn("malformed delay skipped", where)
            elif cmd == "random":
                m = re.match(r"(\S+)\s+(-?\d+)\s+(-?\d+)", arg)
                if m:
                    emit(f"set {self.var(m.group(1))} = {m.group(2)}")
                warn("random is not supported: variable set to its low value", where)
            elif cmd == "endscript":
                emit("stop")
            elif cmd == "video":   # 3DVN extension (not VNDS): video/NAME.3vv, see mk3vv.py
                if arg.split():
                    emit(f"video {ident(os.path.splitext(arg.split()[0])[0])}")
            else:
                warn(f"unknown command '{cmd}' skipped (VNDS ignores it too)", where)
        if held:
            out.append("  " * depth + self.say_line("\n".join(held)))
        while depth > 0:
            depth -= 1
            out.append("  " * depth + "end")
            warn("unclosed if closed at end of script", self.scr.rel(key))
        out.append("stop")
        return out

    def menu(self, opts, first, ind):
        """A VNDS choice: sets 'selected' to the option's number. 3DVN shows up to 8 options,
        so longer ones become pages of 7 with a "More..." option leading to the next page."""
        out = self.current_out
        out.append(ind + "menu")
        page = opts if len(opts) <= MAX_CHOICES else opts[:MAX_CHOICES - 1]
        for i, o in enumerate(page):
            out.append(ind + f"  choice {self.text_literal(o)}")
            out.append(ind + f"    set {self.var('selected')} = {first + i}")
        if len(page) < len(opts):
            out.append(ind + '  choice "More..."')
            self.menu(opts[len(page):], first + len(page), ind + "    ")
        out.append(ind + "end")

    @staticmethod
    def chunks(text, limit=400):
        """3DVN text boxes hold 512 bytes and script lines 1023: split longer text at spaces."""
        parts = []
        while len(text.encode("utf-8")) > limit:
            cut = limit
            while len(text[:cut].encode("utf-8")) > limit:
                cut -= 1
            space = text.rfind(" ", 0, cut)
            cut = space + 1 if space > cut // 2 else cut
            parts.append(text[:cut].rstrip())
            text = text[cut:].lstrip()
        parts.append(text)
        return parts

    def say_line(self, text):
        pieces = self.chunks(text)
        if len(pieces) > 1:
            return "\n".join(self.say_line(piece) for piece in pieces)
        if self.a.speakers:
            # "Name: text", "[Name] text" (Ever 17), 'Name "text"' (Kanon)
            m = (re.match(r"([A-Z][\w.' -]{0,23}?):\s+(\S.*)$", text, re.S)
                 or re.match(r"\[([^\]\[]{1,24})\]\s*(\S.*)$", text, re.S)
                 or re.match(r"([A-Z][\w.'-]*(?: [A-Z][\w.'-]*){0,2}) (\".*)$", text, re.S))
            if m and len(m.group(1).split()) <= 3:
                name = m.group(1)
                cid = ident(name.lower(), "c_")
                self.chars.setdefault(cid, name)
                return f"{cid}: {self.text_literal(m.group(2))}"
        return self.text_literal(text)

    def say(self, text, emit):
        emit(self.say_line(text))

    def bgload(self, arg, emit, where):
        bits = arg.split()
        if not bits:
            return
        emit("hide *")   # a new background removes all sprites in VNDS
        self.sprites.clear()
        if bits[0] == "~":
            emit("bg black")
            return
        fade = ""
        if len(bits) > 1 and bits[1].isdigit() and int(bits[1]) > 0:
            fade = f" fade {min(int(bits[1]) / 60, 3):.2f}"
        if self.template(bits[0]):
            emit(f"bg {self.file_ref(bits[0])}{fade}")
            return
        key = self.bgs.find(bits[0])
        if not key:
            warn("missing background (shown as black)", where + " " + bits[0])
            emit("bg black")
            return
        self.used["bg"].add(key)
        fade = ""
        if len(bits) > 1 and bits[1].isdigit() and int(bits[1]) > 0:
            fade = f" fade {min(int(bits[1]) / 60, 3):.2f}"
        emit(f"bg {self.asset_name(self.bgs, key)}{fade}")

    def asset_name(self, assets, key):
        """Path inside the 3DVN folder, without extension, keeping the original name and
        subfolders (only characters a script word can't hold are replaced, one for one)."""
        return safe_name(os.path.splitext(assets.rel(key))[0])

    def setimg(self, arg, emit, where):
        bits = arg.split()
        if bits and bits[0] == "~":
            emit("hide *")
            self.sprites.clear()
            return
        if not bits:
            warn("malformed setimg skipped", where)
            return
        t = self.template(bits[0])
        key = None if t else self.fgs.find(bits[0])
        if not t and not key:
            warn("missing sprite skipped", where + " " + bits[0])
            return
        try:   # VNDS: missing coordinates are 0
            x = int(bits[1]) if len(bits) > 1 else 0
            y = int(bits[2]) if len(bits) > 2 else 0
        except ValueError:
            warn("malformed setimg skipped", where)
            return
        if t:
            slot = ident(t[0], "s_")
            picture = self.file_ref(bits[0])
        else:
            self.used["fg"].add(key)
            slot = ident(os.path.splitext(os.path.basename(key))[0], "s_")
            picture = self.asset_name(self.fgs, key)
        if slot in self.sprites:
            self.sprites.remove(slot)
        elif len(self.sprites) >= 8:
            emit(f"hide {self.sprites.pop(0)}")
            warn("more than 8 sprites at once (oldest hidden)", where)
        self.sprites.append(slot)
        px, py = round(self.xoff + x * self.scale), round(self.yoff + y * self.scale)
        emit(f"show {slot} {picture} at {px} {py}")

    def audio(self, cmd, arg, emit, where):
        bits = arg.split()
        if not bits:
            return
        if bits[0] == "~":
            emit("music stop" if cmd == "music" else "sfx stop")
            return
        if self.template(bits[0]):
            loop = " loop" if cmd == "sound" and len(bits) > 1 and bits[1] == "-1" else ""
            emit(f"{'music' if cmd == 'music' else 'sfx'} {self.file_ref(bits[0])}{loop}")
            return
        key = self.snd.find(bits[0])
        if not key:
            warn("missing audio file skipped", where + " " + bits[0])
            return
        name = self.asset_name(self.snd, key)
        self.used["snd"][key] = name
        if cmd == "music":
            self.used["music"].add(key)
            emit(f"music {name}")
        elif len(bits) > 1 and bits[1] == "-1":
            emit(f"sfx {name} loop")
        else:
            emit(f"sfx {name}")

    # ---- assets ----
    def convert_image(self, job):
        assets, key, dest, opaque = job
        name = self.asset_name(assets, key)
        ext = ".jpg" if opaque else ".png"
        target = os.path.join(self.out, dest, name + ext)
        hd = os.path.join(self.out, "hd", dest, name + ext)
        if os.path.exists(target):
            return
        os.makedirs(os.path.dirname(target), exist_ok=True)
        try:
            with Image.open(io.BytesIO(assets.read(key))) as image:
                image.load()
                if opaque:
                    if image.mode in ("RGBA", "LA", "P"):
                        rgba = image.convert("RGBA")
                        image = Image.new("RGB", rgba.size, (0, 0, 0))
                        image.paste(rgba, mask=rgba.getchannel("A"))
                    image = image.convert("RGB")
                else:
                    image = image.convert("RGBA")
                # pixels per canvas unit: 1, or more for higher-resolution replacement art
                # (see import_never7.py); the 3DS size follows the canvas, hd/ keeps the art
                factor = assets.factor(key) if hasattr(assets, "factor") else 1.0
                w = max(1, round(image.width / factor * self.scale))
                h = max(1, round(image.height / factor * self.scale))
                if w > 1024 or h > 1024:   # e.g. a tall scrolling picture: shrink it to fit
                    fit = 1024 / max(w, h)
                    warn("picture too large for the 3DS, shrunk to fit 1024x1024",
                         f"{assets.rel(key)} ({w}x{h})")
                    w, h = max(1, int(w * fit)), max(1, int(h * fit))
                small = image.resize((w, h), Image.LANCZOS) if (w, h) != image.size else image
                if opaque:
                    small.save(target, quality=88)
                else:
                    small.save(target, optimize=True)
                if not self.a.no_hd and (image.width > w * 1.2 or image.height > h * 1.2) \
                        and image.width <= 2048 and image.height <= 2048:
                    os.makedirs(os.path.dirname(hd), exist_ok=True)
                    if opaque:
                        image.save(hd, quality=90)
                    else:
                        image.save(hd, optimize=True)
        except OSError as exc:
            warn("unreadable image skipped", f"{assets.rel(key)}: {exc}")

    def convert_images(self):
        jobs = [(self.bgs, k, "bg", True) for k in sorted(self.used["bg"])]
        jobs += [(self.fgs, k, "sprites", False) for k in sorted(self.used["fg"])]
        with concurrent.futures.ThreadPoolExecutor(self.a.jobs) as pool:
            for _ in pool.map(self.convert_image, jobs):
                pass

    def convert_sound(self, job):
        key, name, is_music, ff = job
        data_ext = os.path.splitext(key)[1]
        target_ogg = os.path.join(self.out, "audio", name + ".ogg")
        target_wav = os.path.join(self.out, "audio", name + ".wav")
        if os.path.exists(target_ogg) or os.path.exists(target_wav):
            return
        os.makedirs(os.path.dirname(target_ogg), exist_ok=True)
        data = self.snd.read(key)
        if not data:
            warn("empty sound file in the original skipped", self.snd.rel(key))
            return
        if data[:4] == b"OggS" and data_ext == ".ogg" and not self.a.wav:
            with open(target_ogg, "wb") as handle:   # already Ogg Vorbis: unchanged
                handle.write(data)
            return
        if data[:4] == b"RIFF" and (self.a.wav or not ff):
            with open(target_wav, "wb") as handle:
                handle.write(data)
            return
        if not ff:
            warn("audio not converted (ffmpeg not found)", self.snd.rel(key))
            return
        source = self.snd.path(key)
        temp = None
        if source is None:   # from an archive: a temporary file named after what it really is
            real_ext = ".wav" if data[:4] == b"RIFF" else ".ogg" if data[:4] == b"OggS" else data_ext
            temp = target_ogg + ".src" + real_ext
            with open(temp, "wb") as handle:
                handle.write(data)
            source = temp
        if self.a.wav:
            command = [ff, "-y", "-loglevel", "error", "-i", source, "-vn", "-ac", "1",
                       "-ar", str(self.a.music_rate if is_music else self.a.sfx_rate), "-c:a", "pcm_s16le", target_wav]
        else:
            command = [ff, "-y", "-loglevel", "error", "-i", source, "-vn", "-c:a", "libvorbis",
                       "-q:a", "4" if is_music else "3"]
            if not is_music:
                command += ["-ac", "1"]
            command += [target_ogg]
        result = subprocess.run(command, capture_output=True)
        if temp:
            os.remove(temp)
        if result.returncode:
            warn("audio conversion failed", self.snd.rel(key))

    def convert_audio(self):
        if self.a.no_audio or not self.used["snd"]:
            return
        ff = find_ffmpeg(self.a.ffmpeg)
        if not ff:
            print("note: ffmpeg not found; only .ogg and .wav sounds can be used (see README, or --ffmpeg PATH)")
        jobs = [(k, n, k in self.used["music"], ff) for k, n in sorted(self.used["snd"].items())]
        with concurrent.futures.ThreadPoolExecutor(self.a.jobs) as pool:
            for i, _ in enumerate(pool.map(self.convert_sound, jobs), 1):
                if i % 2000 == 0:
                    print(f"  audio {i}/{len(jobs)}")

    def write_config(self):
        lines = [f"title {self.a.title}", "menu_title " + self.a.title]
        if self.a.nvl:
            lines += ["default_display nvl", "nvl_accumulate 1"]
        with open(os.path.join(self.out, "game.3vc"), "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
        for icon in ("icon.png", "icon-high.png"):
            path = os.path.join(self.a.src, icon)
            if os.path.isfile(path):
                shutil.copy(path, os.path.join(self.out, "icon.png"))

    @staticmethod
    def break_jump_cycles(lines):
        """A jump that only reaches other jumps, round and round (Kanon after an ending),
        freezes VNDS on the last screen; here it ends the story instead."""
        at = {}
        for i, l in enumerate(lines):
            if l.startswith("label "):
                at[l.split()[1]] = i
        def next_jump(label):
            i = at.get(label)
            if i is None:
                return None
            i += 1
            while i < len(lines) and lines[i].startswith("label "):
                i += 1
            m = re.match(r"jump (\S+)$", lines[i]) if i < len(lines) else None
            return m.group(1) if m else None
        out = list(lines)
        for i, l in enumerate(lines):
            m = re.match(r"jump (\S+)$", l)
            if not m:
                continue
            seen, label = set(), m.group(1)
            while label is not None and label not in seen:
                seen.add(label)
                label = next_jump(label)
            if label is not None:
                warn("endless jump loop in the original ended the story", label)
                out[i] = "stop  # endless jump loop in the original"
        return out

    def run(self):
        if Image is None:
            sys.exit("Pillow is required for image conversion; install it with: pip install pillow")
        scripts = self.scripts()
        self.script_names = set(scripts)
        self.configure_canvas(scripts)
        self.scan_globals_and_targets(scripts)
        os.makedirs(os.path.join(self.out, "ch"), exist_ok=True)
        for name, key in scripts.items():
            body = self.convert_script(name, key, scripts)
            defined = {l.split()[1] for l in body if l.strip().startswith("label ")}
            fixed = []
            for l in body:
                m = re.match(r"(\s*)jump (\S+)$", l)
                if m and m.group(2) not in defined:
                    warn("goto to a missing label skipped", f"{self.scr.rel(key)}: {m.group(2)}")
                    l = f"{m.group(1)}# missing label {m.group(2)}"
                fixed.append(l)
            fixed = self.break_jump_cycles(fixed)
            with open(os.path.join(self.out, "ch", ident(name) + ".3vs"), "w", encoding="utf-8") as fh:
                fh.write("\n".join(fixed) + "\n")
        if self.a.speakers:
            with open(os.path.join(self.out, "characters.3vs"), "w", encoding="utf-8") as fh:
                fh.write("# speakers found with --speakers\n")   # never empty (older 3DVN builds)
                for cid, name in sorted(self.chars.items()):
                    fh.write(f"character {cid} {quote(name)}\n")
        head = [f"# {self.a.title} - converted from VNDS by import_vnds.py", 'set _vnds_entry = ""',
                "script ch/main"]
        with open(os.path.join(self.out, "main.3vs"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(head) + "\n")
        self.write_config()
        print(f"scripts converted: {len(scripts)}; converting {len(self.used['bg'])} backgrounds, "
              f"{len(self.used['fg'])} sprites ...")
        self.convert_images()
        print(f"converting {len(self.used['snd'])} sounds ...")
        self.convert_audio()
        return len(scripts)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--source-size", type=source_size, metavar="WIDTHxHEIGHT",
                    help="source canvas size (default: img.ini, else the backgrounds' size)")
    ap.add_argument("--scale", type=positive_scale, default=None,
                    help="override the automatic image and coordinate scale")
    ap.add_argument("--speakers", action="store_true",
                    help='treat "Name: text" lines as dialogue by Name')
    ap.add_argument("--nvl", action="store_true",
                    help="keep text on screen like VNDS (new page at cleartext and when full) "
                         "instead of one line at a time")
    ap.add_argument("--keep-main-loop", action="store_true",
                    help='keep "jump main.scr" as a jump back to the start instead of ending the story')
    ap.add_argument("--title", default=None)
    ap.add_argument("--no-audio", action="store_true", help="skip audio files")
    ap.add_argument("--no-hd", action="store_true", help="don't keep the original pictures in hd/")
    ap.add_argument("--wav", action="store_true", help="convert audio to mono WAV (older 3DVN builds)")
    ap.add_argument("--ffmpeg", default=None, help="path to the ffmpeg program")
    ap.add_argument("--music-rate", type=int, default=32000, help="--wav: music sample rate (default 32000)")
    ap.add_argument("--sfx-rate", type=int, default=22050, help="--wav: sound sample rate (default 22050)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2, help="parallel conversions")
    a = ap.parse_args()
    if a.title is None:
        a.title = os.path.basename(os.path.normpath(a.src))
        info = os.path.join(a.src, "info.txt")
        if os.path.isfile(info):
            for l in open(info, encoding="utf-8", errors="replace"):
                if l.lower().startswith("title="):
                    a.title = l.split("=", 1)[1].strip()
    c = Converter(a)
    try:
        n = c.run()
    except ValueError as exc:
        sys.exit(f"conversion failed: {exc}")
    print(f"done: {n} scripts, {len(c.used['bg'])} backgrounds, {len(c.used['fg'])} sprites, "
          f"{len(c.used['snd'])} sounds, {len(c.globals)} global variables")
    for k, v in report.most_common():
        print(f"  warning: {k} x{v}")
    for s in notes:
        print(f"    e.g. {s}")


if __name__ == "__main__":
    main()
