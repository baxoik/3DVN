#!/usr/bin/env python3
"""Convert a VNDS game folder into a 3DVN game folder.

usage: import_vnds.py SRC_VNDS_DIR OUT_GAME_DIR [--source-size WIDTHxHEIGHT]
                      [--scale S] [--speakers]
                      [--title T] [--no-audio]
                      [--ffmpeg PATH] [--music-rate HZ] [--sfx-rate HZ]

SRC has script/, background/, foreground/, sound/ (and optional info.txt).
Only convert games you own or that are freely licensed.
"""
import argparse, math, os, re, shutil, subprocess, sys
from collections import Counter

try:
    from PIL import Image
except ImportError:
    Image = None

RESERVED = {"true", "false", "and", "or", "not"}
SCREEN_W, SCREEN_H = 400, 240
DEFAULT_SOURCE_SIZE = (256, 192)
IMG_EXT = (".png", ".jpg", ".jpeg", ".bmp", ".gif")


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

report = Counter()
notes = []


def warn(key, msg=None):
    report[key] += 1
    if msg and report[key] <= 3:
        notes.append(msg)


def ci_index(folder):
    """lower-case relative path -> real relative path"""
    out = {}
    for root, _, files in os.walk(folder):
        for f in files:
            rel = os.path.relpath(os.path.join(root, f), folder)
            out[rel.replace(os.sep, "/").lower()] = rel
    return out


def ident(s):
    s = re.sub(r"[^A-Za-z0-9_]", "_", s)
    if not s or s[0].isdigit() or s.lower() in RESERVED:
        s = "v_" + s
    return s


def quote(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("{", "\\{")
    return '"' + s + '"'


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


class Converter:
    def __init__(self, a):
        self.a = a
        self.src, self.out = a.src, a.out
        self.bgs = ci_index(os.path.join(a.src, "background"))
        self.fgs = ci_index(os.path.join(a.src, "foreground"))
        self.snd = ci_index(os.path.join(a.src, "sound"))
        self.used = {"bg": set(), "fg": set(), "snd": {}, "music": set()}
        self.chars = {}
        self.sprites = []  # active sprite slots, oldest first

    def configure_canvas(self, scripts, sdir):
        if self.a.source_size:
            size = self.a.source_size
            origin = "--source-size"
        else:
            referenced = set()
            for filename in scripts.values():
                with open(os.path.join(sdir, filename), encoding="utf-8", errors="replace") as f:
                    for line in f:
                        parts = line.strip().lstrip("\ufeff").split()
                        if len(parts) >= 2 and parts[0].lower() == "bgload":
                            real = self.find(self.bgs, parts[1])
                            if real:
                                referenced.add(real)
            sizes = Counter()
            for real in sorted(referenced):
                path = os.path.join(self.src, "background", real)
                try:
                    with Image.open(path) as image:
                        sizes[image.size] += 1
                except (OSError, ValueError) as exc:
                    raise ValueError(f"cannot read background {path}: {exc}") from exc
            if sizes:
                # Prefer the largest image when counts tie, avoiding tiny placeholders.
                size = max(sizes, key=lambda s: (sizes[s], s[0] * s[1], s))
                origin = "most common referenced background size"
                if len(sizes) > 1:
                    warn("referenced backgrounds have different dimensions; "
                         "verify the source canvas or use --source-size",
                         ", ".join(f"{w}x{h}: {count}" for (w, h), count in sorted(sizes.items())))
            else:
                size = DEFAULT_SOURCE_SIZE
                origin = "default; no referenced background found"
                warn("source size could not be inferred; assuming 256x192; use --source-size if needed")
        self.scale = self.a.scale if self.a.scale is not None else min(
            SCREEN_W / size[0], SCREEN_H / size[1])
        canvas_w = max(1, round(size[0] * self.scale))
        canvas_h = max(1, round(size[1] * self.scale))
        self.xoff = (SCREEN_W - canvas_w) / 2
        self.yoff = (SCREEN_H - canvas_h) / 2
        print(f"source canvas: {size[0]}x{size[1]} ({origin}); "
              f"scale {self.scale:.6g}, output canvas {canvas_w}x{canvas_h}")

    def stem(self, s):
        return os.path.splitext(os.path.basename(s))[0]

    def find(self, idx, name):
        return idx.get(name.replace("\\", "/").lower())

    def sprite_slot(self, real):
        return ident(self.stem(real))

    def convert_script(self, path, prefix, label_map):
        lines = [f"# converted from {os.path.basename(path)}", f"label {prefix}start"]
        depth = 0
        with open(path, encoding="utf-8", errors="replace") as f:
            raw = f.read().splitlines()
        for n, line in enumerate(raw, 1):
            line = line.strip().lstrip("\ufeff")
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 1)
            cmd, arg = parts[0].lower(), (parts[1] if len(parts) > 1 else "")
            ind = "  " * depth
            emit = lambda s: lines.append(ind + s)
            where = f"{os.path.basename(path)}:{n}"

            if cmd == "text":
                self.text(arg, emit)
            elif cmd == "bgload":
                self.bgload(arg, emit, where)
            elif cmd == "setimg":
                self.setimg(arg, emit, where)
            elif cmd in ("sound", "music"):
                self.audio(cmd, arg, emit, where)
            elif cmd == "choice":
                opts = [o.strip() for o in arg.split("|")]
                opts = [o for o in opts if o]
                if len(opts) > 8:
                    warn("choice with more than 8 options (extra dropped)", where)
                    opts = opts[:8]
                emit("menu")
                for i, o in enumerate(opts, 1):
                    emit(f"  choice {quote(o)}")
                    emit(f"    set selected = {i}")
                emit("end")
            elif cmd in ("setvar", "gsetvar"):
                m = re.match(r"(\S+)\s+(\S+)\s*(.*)$", arg)
                if cmd == "gsetvar":
                    warn("gsetvar treated as a normal (per-save) variable")
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
                        emit(f"set {ident(var)} {o} {value(val) if val else '0'}")
            elif cmd == "if":
                m = re.match(r"(\S+)\s*(==|!=|<=|>=|<|>|=)\s*(.+)$", arg)
                if not m:
                    warn("malformed if (replaced by if false)", where)
                    emit("if false")
                else:
                    op = "==" if m.group(2) == "=" else m.group(2)
                    emit(f"if {ident(m.group(1))} {op} {value(m.group(3).strip())}")
                depth += 1
            elif cmd == "else":
                lines.append("  " * max(depth - 1, 0) + "else")
            elif cmd == "fi":
                depth = max(depth - 1, 0)
                lines.append("  " * depth + "end")
            elif cmd == "goto":
                emit(f"jump {prefix}{ident(arg.strip())}")
            elif cmd == "label":
                emit(f"label {prefix}{ident(arg.strip())}")
            elif cmd == "jump":
                bits = arg.split()
                if not bits:
                    warn("malformed jump skipped", where)
                    continue
                target = os.path.splitext(bits[0].replace("\\", "/").split("/")[-1])[0].lower()
                if target not in label_map:
                    warn("jump to a missing script skipped", where + " " + bits[0])
                    continue
                lab = ident(bits[1]) if len(bits) > 1 else "start"
                emit(f"jump {label_map[target]}{lab}")
            elif cmd == "delay":
                try:
                    emit(f"wait {int(arg.split()[0]) / 60:.2f}")
                except (ValueError, IndexError):
                    warn("malformed delay skipped", where)
            elif cmd == "cleartext":
                pass  # every text line is its own page already
            elif cmd == "random":
                m = re.match(r"(\S+)\s+(-?\d+)\s+(-?\d+)", arg)
                if m:
                    emit(f"set {ident(m.group(1))} = {m.group(2)}")
                warn("random is not supported: variable set to its low value", where)
            elif cmd == "endscript":
                emit("stop")
            else:
                warn(f"unknown command '{cmd}' skipped", where)
        while depth > 0:
            depth -= 1
            lines.append("  " * depth + "end")
            warn("unclosed if closed at end of script", os.path.basename(path))
        lines.append("stop")
        return lines

    def text(self, arg, emit):
        arg = arg.strip()
        if arg.startswith("@"):
            arg = arg[1:].strip()
        if not arg or arg == "~":
            return
        if self.a.speakers:
            m = re.match(r"([A-Z][\w.' -]{0,23}?):\s+(\S.*)$", arg)
            if m and len(m.group(1).split()) <= 3:
                name = m.group(1)
                cid = ident(name.lower())
                if cid not in self.chars:
                    self.chars[cid] = name
                emit(f"{cid}: {quote(m.group(2))}")
                return
        emit(quote(arg))

    def bgload(self, arg, emit, where):
        bits = arg.split()
        if not bits:
            return
        # a new background removes all sprites in VNDS
        for s in self.sprites:
            emit(f"hide {s}")
        self.sprites.clear()
        if bits[0] == "~":
            emit("bg black")
            return
        real = self.find(self.bgs, bits[0])
        if not real:
            warn("missing background (shown as black)", where + " " + bits[0])
            emit("bg black")
            return
        self.used["bg"].add(real)
        fade = ""
        if len(bits) > 1 and bits[1].isdigit() and int(bits[1]) > 0:
            fade = f" fade {min(int(bits[1]) / 60, 3):.2f}"
        emit(f"bg {self.stem(real)}{fade}")

    def setimg(self, arg, emit, where):
        bits = arg.split()
        if len(bits) < 3:
            warn("malformed setimg skipped", where)
            return
        real = self.find(self.fgs, bits[0])
        if not real:
            warn("missing sprite skipped", where + " " + bits[0])
            return
        try:
            x, y = int(bits[1]), int(bits[2])
        except ValueError:
            warn("malformed setimg skipped", where)
            return
        self.used["fg"].add(real)
        slot = self.sprite_slot(real)
        if slot in self.sprites:
            self.sprites.remove(slot)
        elif len(self.sprites) >= 8:
            emit(f"hide {self.sprites.pop(0)}")
            warn("more than 8 sprites at once (oldest hidden)", where)
        self.sprites.append(slot)
        px, py = round(self.xoff + x * self.scale), round(self.yoff + y * self.scale)
        emit(f"show {slot} {self.stem(real)} at {px} {py}")

    def audio(self, cmd, arg, emit, where):
        bits = arg.split()
        if not bits:
            return
        if bits[0] == "~":
            if cmd == "music":
                emit("music stop")
            return
        real = self.find(self.snd, bits[0])
        if not real:
            warn("missing audio file skipped", where + " " + bits[0])
            return
        name = ident(self.stem(real))
        self.used["snd"][real] = name
        if cmd == "music":
            self.used["music"].add(real)
            emit(f"music {name}")
        else:
            if len(bits) > 1 and bits[1] == "-1":
                warn("looping 'sound' played once as a sound effect", where)
            emit(f"sfx {name}")

    # ---- assets ----
    def copy_images(self):
        for kind, folder, dest in (("bg", "background", "bg"), ("fg", "foreground", "sprites")):
            os.makedirs(os.path.join(self.out, dest), exist_ok=True)
            for real in sorted(self.used[kind]):
                src = os.path.join(self.src, folder, real)
                stem = self.stem(real)
                try:
                    with Image.open(src) as image:
                        w = max(1, round(image.width * self.scale))
                        h = max(1, round(image.height * self.scale))
                        if w > 1024 or h > 1024:
                            raise ValueError(f"{real} would become {w}x{h}, exceeding "
                                             "the 3DVN 1024x1024 image limit; "
                                             "check --source-size or use a smaller --scale")
                        mode = "RGBA" if kind == "fg" or image.mode in ("RGBA", "LA", "P") else "RGB"
                        image = image.convert(mode).resize((w, h), Image.LANCZOS)
                        image.save(os.path.join(self.out, dest, stem + ".png"))
                except OSError as exc:
                    raise ValueError(f"cannot convert image {src}: {exc}") from exc

    def convert_audio(self):
        os.makedirs(os.path.join(self.out, "audio"), exist_ok=True)
        ff = find_ffmpeg(self.a.ffmpeg)
        if self.used["snd"] and not ff and not self.a.no_audio:
            print("note: ffmpeg not found; only .wav audio can be converted "
                  "(see README, or use --ffmpeg PATH)")
        for real, name in sorted(self.used["snd"].items()):
            src = os.path.join(self.src, "sound", real)
            dst = os.path.join(self.out, "audio", name + ".wav")
            if self.a.no_audio:
                continue
            rate = self.a.music_rate if real in self.used["music"] else self.a.sfx_rate
            if ff:
                r = subprocess.run([ff, "-y", "-loglevel", "error", "-i", src, "-vn",
                                    "-ac", "1", "-ar", str(rate), "-c:a", "pcm_s16le", dst])
                if r.returncode:
                    warn("audio conversion failed", real)
                    if os.path.exists(dst):
                        os.remove(dst)
            elif real.lower().endswith(".wav"):
                shutil.copy(src, dst)
            else:
                warn("audio not converted (ffmpeg not found)", real)

    def run(self):
        scripts = {}
        sdir = os.path.join(self.src, "script")
        if not os.path.isdir(sdir):
            sys.exit(f"no script/ folder in {self.src}")
        for f in sorted(os.listdir(sdir)):
            if f.lower().endswith(".scr"):
                scripts[os.path.splitext(f)[0].lower()] = f
        if "main" not in scripts:
            sys.exit("no main.scr found")
        if Image is None:
            sys.exit("Pillow is required for image conversion; install it with: pip install pillow")
        self.configure_canvas(scripts, sdir)
        label_map = {k: f"s_{ident(k)}_" for k in scripts}
        os.makedirs(self.out, exist_ok=True)
        files = []
        bodies = {}
        for k, f in scripts.items():
            bodies[k] = self.convert_script(os.path.join(sdir, f), label_map[k], label_map)
            self.sprites.clear()
        defined = {l.split()[1] for b in bodies.values() for l in b if l.startswith("label ")}
        for k, f in scripts.items():
            body = []
            for l in bodies[k]:
                m = re.match(r"(\s*)jump (\S+)$", l)
                if m and m.group(2) not in defined:
                    warn("jump to a missing label skipped", f"{f}: {m.group(2)}")
                    l = f"{m.group(1)}# missing label {m.group(2)}"
                body.append(l)
            name = f"s_{ident(k)}.3vs"
            files.append(name)
            with open(os.path.join(self.out, name), "w", encoding="utf-8") as fh:
                fh.write("\n".join(body) + "\n")
        head = [f"# {self.a.title} - converted from VNDS by import_vnds.py"]
        for cid, name in sorted(self.chars.items()):
            head.append(f"character {cid} {quote(name)}")
        head.append(f"jump {label_map['main']}start")
        head += [f"include {n}" for n in files]
        with open(os.path.join(self.out, "main.3vs"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(head) + "\n")
        self.copy_images()
        self.convert_audio()
        return len(scripts)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--source-size", type=source_size, metavar="WIDTHxHEIGHT",
                    help="source canvas size (default: infer from referenced backgrounds)")
    ap.add_argument("--scale", type=positive_scale, default=None,
                    help="override automatic image and coordinate scale")
    ap.add_argument("--speakers", action="store_true",
                    help='treat "Name: text" lines as dialogue by Name')
    ap.add_argument("--title", default=None)
    ap.add_argument("--no-audio", action="store_true", help="skip audio files")
    ap.add_argument("--ffmpeg", default=None, help="path to the ffmpeg program")
    ap.add_argument("--music-rate", type=int, default=32000,
                    help="music sample rate in Hz (default 32000)")
    ap.add_argument("--sfx-rate", type=int, default=22050,
                    help="sound effect sample rate in Hz (default 22050)")
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
    print(f"converted {n} scripts, {len(c.used['bg'])} backgrounds, "
          f"{len(c.used['fg'])} sprites, {len(c.used['snd'])} audio files")
    for k, v in report.items():
        print(f"  warning: {k} x{v}")
    for s in notes:
        print(f"    e.g. {s}")


if __name__ == "__main__":
    main()
