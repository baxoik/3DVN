#!/usr/bin/env python3
"""Convert extracted KiriKiri scenes to an experimental 3DVN preview."""

import argparse
import hashlib
import re
import shlex
import subprocess
import sys
import tempfile
import zipfile
from collections import Counter
from pathlib import Path

from import_vnds import Image, find_ffmpeg, quote, source_size


TAG = re.compile(r"\[([^\]]*)\]")
MAX_TEXT_BYTES = 480
IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".bmp"}
# Tags that can alter or remove existing foreground layers; their effect is not modelled.
CF_LAYER_TAGS = {"chgfg", "movefg", "putfg", "infg", "outfg", "freefg", "layopt", "freeimage",
                 "image", "clpartbg", "cltransparent"}
BACKGROUND_MARGIN = 48  # KiriKiri bg tag default: bottom-aligned with this overscan
AUDIO_EXTENSIONS = {".wav", ".ogg", ".mp3", ".m4a", ".aac", ".flac"}


class Assets:
    def __init__(self, root, canvas, ffmpeg, scene_canvas=None, thumbnails=False):
        self.root = root
        self.canvas = canvas
        self.scene_canvas = scene_canvas or (None if thumbnails else canvas)
        self.thumbnails = thumbnails
        self.ffmpeg = ffmpeg
        self.index = {}
        self.used = {}
        self.partials = {}
        self.composites = {}
        self.image_sizes = {}
        if root:
            for path in sorted(root.rglob("*")):
                if path.is_file():
                    self.index.setdefault(path.stem.casefold(), []).append(path)

    def lookup(self, storage, kind):
        if not self.root:
            raise ValueError(f"{kind} {storage!r} requires --assets")
        # KiriKiri storage names refer to resource names, not host paths.
        name = storage.replace("\\", "/").rsplit("/", 1)[-1]
        extensions = IMAGE_EXTENSIONS if kind == "background" else AUDIO_EXTENSIONS
        stem = (Path(name).stem if Path(name).suffix.lower() in extensions else name).casefold()
        matches = [p for p in self.index.get(stem, []) if p.suffix.lower() in extensions]
        if len(matches) != 1:
            raise ValueError(f"{kind} {storage!r} " + ("not found" if not matches else "is ambiguous"))
        return matches[0]

    def resolve(self, storage, kind, full_background=True):
        path = self.lookup(storage, kind)
        if kind == "background" and not self.canvas:
            raise ValueError(f"background {storage!r} requires --source-size")
        if kind == "background" and Image is None:
            raise ValueError("background conversion requires Pillow")
        if kind == "background" and full_background:
            if path not in self.image_sizes:
                with Image.open(path) as image:
                    self.image_sizes[path] = image.size
            scale = min(400 / self.canvas[0], 240 / self.canvas[1])
            width, height = self.image_sizes[path]
            if max(round(width * scale), round(height * scale)) > 1024:
                raise ValueError(f"scaled image exceeds 1024x1024: {path}")
        if kind != "background" and not self.ffmpeg:
            raise ValueError(f"{kind} conversion requires ffmpeg")
        key = (kind, path)
        name = "k_" + hashlib.sha256(
            (kind + "/" + path.relative_to(self.root).as_posix()).encode("utf-8")
        ).hexdigest()[:20]
        self.used[key] = name
        return name

    def image_size(self, path):
        if path not in self.image_sizes:
            with Image.open(path) as image:
                self.image_sizes[path] = image.size
        return self.image_sizes[path]

    def composite(self, base, layers):
        """Register a static composition of a base image and plain foreground layers.

        base is ("solid", "black"|"white") or ("image", path, left, top); each layer is
        (path, center, vcenter, opacity, zoomx, zoomy). Coordinates use the source scene canvas.
        """
        if Image is None:
            raise ValueError("foreground composition requires Pillow")
        if self.thumbnails or not self.scene_canvas:
            raise ValueError("foreground composition requires full-resolution assets and a scene size")
        canvas_w, canvas_h = self.scene_canvas
        scale = min(400 / canvas_w, 240 / canvas_h)
        if base[0] == "image":
            width, height = self.image_size(base[1])
            left = base[2] if base[2] is not None else (canvas_w - width) // 2
            top = base[3] if base[3] is not None else canvas_h - height + BACKGROUND_MARGIN
            base = ("image", base[1], left, top)
        for path, _, _, _, zoomx, zoomy in layers:
            width, height = self.image_size(path)
            if max(width * zoomx, height * zoomy) / 100 * scale > 4096:
                raise ValueError(f"scaled foreground layer exceeds 4096 pixels: {path}")
        spec = (base, tuple(layers))
        name = "c_" + hashlib.sha256(repr(spec).encode("utf-8")).hexdigest()[:20]
        self.composites[name] = spec
        return name

    def partial_background(self, storage, attrs):
        if not self.scene_canvas:
            raise ValueError("partial background requires --scene-size when using thumbnails")
        for field in ("srcleft", "srctop", "width", "height", "center", "vcenter"):
            if field in attrs and not re.fullmatch(r"-?\d+", attrs[field]):
                raise ValueError(f"partial background {field} must be a literal integer")
        if not all(field in attrs for field in ("width", "height", "center")):
            raise ValueError("partial background requires width, height and center")
        width, height = int(attrs["width"]), int(attrs["height"])
        if width <= 0 or height <= 0 or width > 4096 or height > 4096:
            raise ValueError("partial background dimensions must be between 1 and 4096")
        base = attrs.get("bgstorage", "").lower()
        if base not in ("black", "white"):
            raise ValueError("partial background requires black or white bgstorage")
        unsupported = set(attrs) - {"storage", "srcleft", "srctop", "width", "height",
                                    "center", "vcenter", "bgstorage", "id", "index", "rule", "time"}
        if unsupported:
            raise ValueError("unsupported partial background options: " + ", ".join(sorted(unsupported)))
        previous = set(self.used)
        asset_name = self.resolve(storage, "background", full_background=False)
        path = next(p for (kind, p), name in self.used.items() if name == asset_name and kind == "background")
        # A partial background is rendered separately, not as the full source image.
        if ("background", path) not in previous:
            del self.used[("background", path)]
        spec = (path, int(attrs.get("srcleft", "0")), int(attrs.get("srctop", "0")),
                width, height, int(attrs["center"]), int(attrs.get("vcenter", str(height // 2))), base)
        name = "p_" + hashlib.sha256(repr(spec).encode("utf-8")).hexdigest()[:20]
        self.partials[name] = spec
        return name

    def write(self, output):
        for (kind, path), name in self.used.items():
            folder = output / ("bg" if kind == "background" else "audio")
            folder.mkdir(exist_ok=True)
            if kind == "background":
                with Image.open(path) as image:
                    scale = min(400 / self.canvas[0], 240 / self.canvas[1])
                    width = max(1, round(image.width * scale))
                    height = max(1, round(image.height * scale))
                    if width > 1024 or height > 1024:
                        raise ValueError(f"scaled image exceeds 1024x1024: {path}")
                    mode = "RGBA" if "A" in image.getbands() or image.mode == "P" else "RGB"
                    image.convert(mode).resize((width, height), Image.Resampling.LANCZOS).save(
                        folder / (name + ".png"))
            else:
                rate = 32000 if kind == "music" else 22050
                dest = folder / (name + ".wav")
                result = subprocess.run([self.ffmpeg, "-y", "-loglevel", "error", "-i", str(path),
                                         "-vn", "-ac", "1", "-ar", str(rate),
                                         "-c:a", "pcm_s16le", str(dest)],
                                        capture_output=True, text=True)
                if result.returncode:
                    raise ValueError(f"ffmpeg failed for {path}: {result.stderr.strip()}")
                if kind != "music" and dest.stat().st_size > 2 * 1024 * 1024:
                    raise ValueError(f"converted sound exceeds 3DVN's 2 MB limit: {path}")
        for name, (base, layers) in self.composites.items():
            folder = output / "bg"
            folder.mkdir(exist_ok=True)
            self.render_composite(base, layers).save(folder / (name + ".png"))
        for name, (path, left, top, width, height, center, vcenter, base) in self.partials.items():
            folder = output / "bg"
            folder.mkdir(exist_ok=True)
            canvas_w, canvas_h = self.scene_canvas
            scale = min(400 / canvas_w, 240 / canvas_h)
            with Image.open(path) as image:
                image = image.convert("RGBA")
                if self.thumbnails:
                    # The thumbnail covers the source scene canvas at reduced resolution.
                    sx, sy = image.width / canvas_w, image.height / canvas_h
                else:
                    sx = sy = 1
                left_px, top_px = round(left * sx), round(top * sy)
                right_px = max(left_px + 1, round((left + width) * sx))
                bottom_px = max(top_px + 1, round((top + height) * sy))
                target_w, target_h = max(1, round(width * scale)), max(1, round(height * scale))
                if max(right_px - left_px, bottom_px - top_px, target_w, target_h) > 4096:
                    raise ValueError(f"partial background crop or scaled image exceeds 4096 pixels: {path}")
                crop = image.crop((left_px, top_px, right_px, bottom_px))
                crop = crop.resize((target_w, target_h), Image.Resampling.LANCZOS)
                background = Image.new("RGBA", (400, 240), (0, 0, 0, 255) if base == "black"
                                       else (255, 255, 255, 255))
                x = round((400 - canvas_w * scale) / 2 + (center - width / 2) * scale)
                y = round((240 - canvas_h * scale) / 2 + (vcenter - height / 2) * scale)
                background.alpha_composite(crop, (x, y))
                background.convert("RGB").save(folder / (name + ".png"))


    def render_composite(self, base, layers):
        canvas_w, canvas_h = self.scene_canvas
        scale = min(400 / canvas_w, 240 / canvas_h)
        size = (max(1, round(canvas_w * scale)), max(1, round(canvas_h * scale)))
        stage = Image.new("RGBA", size, (255, 255, 255, 255) if base == ("solid", "white")
                          else (0, 0, 0, 255))

        def draw(path, left, top, width, height, zoomx, zoomy, opacity):
            with Image.open(path) as source:
                image = source.convert("RGBA")
            target = (max(1, round(width * zoomx / 100 * scale)),
                      max(1, round(height * zoomy / 100 * scale)))
            image = image.resize(target, Image.Resampling.LANCZOS)
            if opacity != 255:
                alpha = image.getchannel("A").point(lambda value: value * opacity // 255)
                image.putalpha(alpha)
            x, y = round(left * scale), round(top * scale)
            clip_x, clip_y = max(0, -x), max(0, -y)
            right, bottom = min(image.width, size[0] - x), min(image.height, size[1] - y)
            if right <= clip_x or bottom <= clip_y:
                return  # entirely outside the stage
            stage.alpha_composite(image.crop((clip_x, clip_y, right, bottom)),
                                  (x + clip_x, y + clip_y))

        if base[0] == "image":
            width, height = self.image_size(base[1])
            draw(base[1], base[2], base[3], width, height, 100, 100, 255)
        for path, center, vcenter, opacity, zoomx, zoomy in layers:
            width, height = self.image_size(path)
            # Layers zoom about their image center, which is also the center/vcenter anchor.
            draw(path, center - (width // 2) * zoomx / 100, vcenter - (height // 2) * zoomy / 100,
                 width, height, zoomx, zoomy, opacity)
        output = Image.new("RGBA", (400, 240), (0, 0, 0, 255))
        output.alpha_composite(stage, (round((400 - canvas_w * scale) / 2),
                                       round((240 - canvas_h * scale) / 2)))
        return output.convert("RGB")


def read_scene(source, scene, encoding):
    if source.is_dir():
        path = source / scene
        if not path.is_file():
            raise ValueError(f"scene not found: {path}")
        data = path.read_bytes()
    elif zipfile.is_zipfile(source):
        with zipfile.ZipFile(source) as archive:
            try:
                data = archive.read(scene)
            except KeyError as exc:
                raise ValueError(f"scene {scene!r} not found in {source}") from exc
    else:
        raise ValueError("input must be a directory of extracted scripts or a ZIP-format archive")
    if encoding == "auto":
        encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    try:
        return data.decode(encoding)
    except UnicodeError as exc:
        raise ValueError(f"cannot decode {scene} as {encoding}: {exc}") from exc


class SceneConverter:
    def __init__(self, scene, pages, assets, nvl_pages=False, compose_fg=False):
        self.scene = scene
        self.pages = pages
        self.output = ["# Experimental KiriKiri scene preview"]
        self.pending = ""
        self.issues = []
        self.page_count = 0
        self.in_script = False
        self.assets = assets
        self.pending_bg = None
        self.pending_loop = []
        self.pending_loop_invalid = False
        self.nvl_pages = nvl_pages
        self.cf = compose_fg
        unknown = ("unknown", "no background has been set in this scene")
        self.cf_base = unknown
        self.cf_layers = []
        self.cf_fore = (unknown, [])
        self.cf_shown = False

    def issue(self, line, message):
        self.issues.append((line, message))

    def asset(self, storage, kind, line):
        if not storage:
            self.issue(line, f"{kind} is missing storage")
            return None
        if storage.startswith(("&", "%")):
            self.issue(line, f"dynamic {kind} storage is unsupported: {storage}")
            return None
        try:
            return self.assets.resolve(storage, kind)
        except ValueError as exc:
            self.issue(line, str(exc))
            return None

    def flush(self):
        if self.nvl_pages and not self.pending.strip():
            return
        text = self.pending if self.nvl_pages else self.pending.strip()
        self.pending = ""
        while text:
            length = 0
            end = 0
            for char in text:
                size = len(char.encode("utf-8"))
                if length + size > MAX_TEXT_BYTES:
                    break
                length += size
                end += 1
            if end < len(text):
                split = text.rfind(" ", 0, end)
                if split > end // 2:
                    end = split
            self.output.append(quote(text[:end]).replace("\n", "\\n"))
            text = text[end:] if self.nvl_pages else text[end:].lstrip()

    def stage_loop_frame(self, attrs, line):
        storage = attrs.get("storage", "")
        common = {"textoff": "0", "page": "back", "loop": "1", "storage": storage}
        first_keys = (f"(0,0,n,{storage},512,288,0,1)(450,,l,,,,,)"
                      "(600,,n,,,,255,)(1050,,l,,,,,)(1200,,n,,,,0,)(1800,,,,,,,)")
        second_keys = (f"(0,0,n,{storage},512,288,1100,0,1)(1050,,l,,,,,,)"
                       "(1200,,n,,,,,255,)(1650,,l,,,,,,)(1800,,n,,,,,0,)")
        first = dict(common, props="-storage,center,vcenter,opacity,-visible", keys=first_keys)
        second = dict(common, props="-storage,center,vcenter,absolute,opacity,-visible",
                      keys=second_keys)
        expected = first if not self.pending_loop else second
        if (not self.pending_bg or self.pending_loop_invalid or len(self.pending_loop) >= 2
                or not storage or storage.startswith(("&", "%")) or attrs != expected
                or self.assets.scene_canvas != (1024, 576) or self.assets.thumbnails):
            self.pending_loop_invalid = True
            self.issue(line, "unsupported fgact animation")
            return
        self.pending_loop.append((storage, line))

    def transition(self, attrs, line):
        time = attrs.get("time")
        rule = attrs.get("rule", "crossfade")
        if time is None:
            return 0.0
        if not time.isascii() or not time.isdecimal() or int(time) > 600000:
            self.issue(line, "transition time must be a literal 0..600000 milliseconds")
            return 0.0
        if rule != "crossfade":
            self.issue(line, f"unsupported transition rule {rule!r}; using crossfade")
        return int(time) / 1000

    def transition_wait(self, attrs, seconds, line):
        nowait = attrs.get("nowait", "0")
        if nowait not in ("0", "1"):
            self.issue(line, "unsupported transition nowait; timing may differ")
        elif nowait == "0" and seconds > 0:
            self.output.append("waitbg skippable")
            self.issue(line, "transition wait is skippable")

    def apply_pending_background(self, line, seconds):
        if not self.pending_bg:
            return False
        fade = f" fade {seconds:g}" if seconds else ""
        frames = []
        if len(self.pending_loop) == 2 and not self.pending_loop_invalid:
            for storage, frame_line in self.pending_loop:
                frames.append(self.asset(storage, "background", frame_line))
        if len(frames) == 2 and all(frames):
            self.output.append(f"bgloop {self.pending_bg} {frames[0]} {frames[1]} 0.45 0.15{fade}")
            self.issue(line, "fgact image loop approximated; transition and layer semantics may differ")
        else:
            self.output.append("bg " + self.pending_bg + fade)
            if self.pending_loop:
                self.issue(line, "incomplete fgact image loop omitted")
        self.pending_bg = None
        self.pending_loop = []
        self.pending_loop_invalid = False
        return True

    # --- Opt-in foreground composition (see tools/README_import_kirikiri.md) ---
    def cf_int(self, attrs, key, low, high):
        value = attrs[key]
        if not re.fullmatch(r"-?\d+", value) or not low <= int(value) <= high:
            raise ValueError(f"{key} must be a literal integer from {low} to {high}")
        return int(value)

    def cf_base_from(self, attrs, background):
        unsupported = attrs.keys() - {"storage", "page", "time", "rule", "nowait", "noclear",
                                      "textoff", "left", "top", "noback"}
        if unsupported:
            return ("unknown", "bg options not composed: " + ", ".join(sorted(unsupported)))
        if self.assets.thumbnails or not self.assets.scene_canvas:
            return ("unknown", "composition requires full-resolution assets and --scene-size")
        if background is None:
            return ("unknown", "bg storage could not be resolved")
        storage = attrs.get("storage", "")
        if storage.lower() in ("black", "white"):
            return ("solid", storage.lower())
        try:
            path = self.assets.lookup(storage, "background")
            left = self.cf_int(attrs, "left", -4096, 4096) if "left" in attrs else None
            top = self.cf_int(attrs, "top", -4096, 4096) if "top" in attrs else None
        except ValueError as exc:
            return ("unknown", str(exc))
        return ("image", path, left, top)

    def cf_transition_seconds(self, attrs, line):
        # fgplugin.doTrans defaults a transition without time to 300 ms.
        if attrs.get("time") is None and (attrs.get("rule") or attrs.get("vague") is not None):
            attrs = dict(attrs, time="300")
        return self.transition(attrs, line)

    def cf_wants_transition(self, attrs):
        return bool(attrs.get("rule")) or attrs.get("time") is not None or attrs.get("vague") is not None

    def cf_snapshot(self):
        return (self.cf_base, [dict(layer) for layer in self.cf_layers])

    def cf_backup(self):
        # fgplugin calls backupLayer(fore -> back) before a transitioning fg/clfg.
        self.cf_base, layers = self.cf_fore
        self.cf_layers = [dict(layer) for layer in layers]

    def cf_taint(self, reason):
        for layer in self.cf_layers:
            layer.setdefault("omitted", reason)

    def cf_publish(self, line, seconds, plain=True):
        """Make the back page visible. Returns True when a composed image was emitted.

        When no layer remains but a composed image is on screen, plain=True replaces it with the
        base alone; @bg passes plain=False because it emits its own background.
        """
        base = self.cf_base
        shown, self.cf_shown = self.cf_shown, False
        layers = self.cf_layers
        usable = [layer for layer in layers if "omitted" not in layer]
        for layer in layers:
            if "omitted" in layer:
                self.issue(layer["line"], f"foreground layer not composed: {layer['omitted']}")
        self.cf_fore = self.cf_snapshot()
        if not usable and not (plain and shown and base[0] != "unknown"
                               and not self.pending_loop):
            return False
        if base[0] == "unknown":
            self.issue(line, f"foreground layers not composed: {base[1]}")
            return False
        if self.pending_loop:
            self.issue(line, "foreground layers not composed: combined with fgact image loop")
            return False
        indexes = [layer["index"] for layer in usable]
        if len(set(indexes)) != len(indexes):
            self.issue(line, "foreground layers not composed: equal layer indexes have unverified order")
            return False
        usable.sort(key=lambda layer: layer["index"])
        try:
            name = self.assets.composite(base, [
                (l["path"], l["center"], l["vcenter"], l["opacity"], l["zoomx"], l["zoomy"])
                for l in usable])
        except ValueError as exc:
            self.issue(line, f"foreground layers not composed: {exc}")
            return False
        self.cf_shown = bool(usable)
        self.output.append(f"bg {name}" + (f" fade {seconds:g}" if seconds else ""))
        self.pending_bg = None
        self.pending_loop = []
        self.pending_loop_invalid = False
        if usable:
            self.issue(line, "foreground composed as a static image; effects and animation are not reproduced")
        return True

    def cf_bg(self, attrs, background, line):
        """Track an @bg tag. Returns True when it already emitted the composed screen."""
        page = attrs.get("page")
        base = self.cf_base_from(attrs, background)
        noclear = attrs.get("noclear", "0")
        if noclear not in ("0", "1"):
            base = ("unknown", "noclear must be 0 or 1")
        if page == "fore":
            layers = self.cf_fore[1] if noclear == "1" else []
            self.cf_fore = (base, layers)
            # A direct fore-page change leaves the back page out of step; backlay timing is not
            # modelled, so a later transition must not republish the older back composition.
            if self.cf_layers or self.cf_shown or self.cf_base[0] != "unknown":
                self.cf_base = ("unknown", "fore page was changed directly; back page is unverified")
                self.cf_layers = []
                self.cf_shown = False
                self.issue(line, "foreground back page invalidated after direct fore-page background")
            return False
        if page not in (None, "back"):
            return False
        self.cf_base = base
        if noclear != "1":
            self.cf_layers = []
        if page == "back":
            return False
        seconds = self.transition(attrs, line)
        handled = self.cf_publish(line, seconds, plain=False)
        if handled:
            self.transition_wait(attrs, seconds, line)
        return handled

    def cf_add_layer(self, attrs, line):
        omitted = None
        layer = {"line": line, "storage": attrs.get("storage")}
        try:
            extra = attrs.keys() - {"storage", "center", "vcenter", "index", "opacity", "zoom",
                                    "zoomx", "zoomy", "id", "time", "rule", "vague", "nowait",
                                    "textoff"}
            if extra:
                raise ValueError("unsupported fg options: " + ", ".join(sorted(extra)))
            if attrs.get("page", "back") != "back":
                raise ValueError("fg page must be back")
            for key in ("center", "vcenter", "index"):
                if key not in attrs:
                    raise ValueError(f"fg requires {key}")
            storage = attrs.get("storage", "")
            if "|" in storage or re.search(r"\([全大中近遠小]\)", storage):
                raise ValueError("character sprites need composition metadata that decoded PNGs do not keep")
            layer["path"] = self.assets.lookup(storage, "background")
            layer["center"] = self.cf_int(attrs, "center", -8192, 8192)
            layer["vcenter"] = self.cf_int(attrs, "vcenter", -8192, 8192)
            layer["index"] = self.cf_int(attrs, "index", 1, 999999)
            layer["opacity"] = self.cf_int(attrs, "opacity", 0, 255) if "opacity" in attrs else 255
            if "zoom" in attrs:
                layer["zoomx"] = layer["zoomy"] = self.cf_int(attrs, "zoom", 1, 1000)
            else:
                layer["zoomx"] = self.cf_int(attrs, "zoomx", 1, 1000) if "zoomx" in attrs else 100
                layer["zoomy"] = self.cf_int(attrs, "zoomy", 1, 1000) if "zoomy" in attrs else 100
            layer["id"] = attrs.get("id")
        except ValueError as exc:
            omitted = str(exc)
            layer["id"] = attrs.get("id")
        if layer["id"]:
            self.cf_layers = [item for item in self.cf_layers if item.get("id") != layer["id"]]
        if omitted:
            layer["omitted"] = omitted
        self.cf_layers.append(layer)

    def cf_fg(self, name, attrs, line):
        wants = self.cf_wants_transition(attrs)
        page = attrs.get("page", "back")
        if page != "back":
            self.cf_taint(f"{name} on the fore page is not modelled")
            self.issue(line, f"unsupported {name} page: {page}")
            return
        if wants:
            self.cf_backup()
        if name == "fg":
            self.cf_add_layer(attrs, line)
        else:
            extra = attrs.keys() - {"page", "id", "time", "rule", "vague", "nowait", "textoff"}
            if extra:
                self.cf_taint("clfg options not modelled: " + ", ".join(sorted(extra)))
            elif "id" in attrs:
                self.cf_layers = [item for item in self.cf_layers if item.get("id") != attrs["id"]]
            else:
                self.cf_layers = []
        if wants:
            seconds = self.cf_transition_seconds(attrs, line)
            self.cf_publish(line, seconds)
            self.transition_wait(attrs, seconds, line)

    def cf_clall(self, attrs, line):
        if attrs.keys() - {"time", "rule", "vague", "nowait", "textoff", "canskip"}:
            self.cf_taint("clall options not modelled")
            self.issue(line, "unsupported clall options")
            return
        self.cf_layers = []
        if attrs.get("time") is not None:
            seconds = self.transition(attrs, line)
            self.cf_publish(line, seconds)
            self.transition_wait(attrs, seconds, line)

    def command(self, content, line):
        try:
            tokens = shlex.split(content, posix=True)
        except ValueError as exc:
            self.issue(line, f"invalid tag syntax: {exc}")
            return False
        if not tokens:
            self.issue(line, "empty tag")
            return False
        name = tokens[0].lower()
        attrs = {}
        for token in tokens[1:]:
            if "=" not in token:
                self.issue(line, f"unsupported parameter in {name}: {token}")
                return False
            key, value = token.split("=", 1)
            attrs[key.lower()] = value
        if self.in_script:
            if name == "endscript":
                self.in_script = False
            return False
        if name in ("l", "p", "pg"):
            self.flush()
            if name in ("p", "pg"):
                if self.nvl_pages:
                    self.pending = ""
                    self.output.append("page")
                self.page_count += 1
                return self.pages is not None and self.page_count >= self.pages
        elif name == "r":
            self.pending += "\n"
        elif name in ("bg", "background"):
            self.flush()
            storage = attrs.get("storage", "")
            if storage.lower() in ("black", "white"):
                background = storage.lower()
            else:
                background = self.asset(storage, "background", line)
            if attrs.keys() - {"storage", "page", "time", "rule", "nowait"}:
                self.issue(line, "background timing or other options approximated")
            page = attrs.get("page", "fore")
            if self.cf and name == "bg" and page in ("fore", "back") and self.cf_bg(
                    attrs, background, line):
                return False
            if page not in ("fore", "back"):
                self.issue(line, f"unsupported background page: {page}")
            elif page == "back":
                if self.pending_loop:
                    self.issue(line, "unapplied fgact image loop discarded")
                self.pending_loop = []
                self.pending_loop_invalid = False
                self.pending_bg = background
            elif background:
                seconds = self.transition(attrs, line)
                self.output.append("bg " + background + (f" fade {seconds:g}" if seconds else ""))
                self.transition_wait(attrs, seconds, line)
        elif name == "fgact":
            self.flush()
            self.stage_loop_frame(attrs, line)
            if self.cf:
                for layer in self.cf_layers:
                    if layer.get("storage") == attrs.get("storage"):
                        layer.setdefault("omitted", "fgact animation is not reproduced")
        elif name == "bgact" and self.cf:
            self.flush()
            self.cf_base = ("unknown", "bgact animation is not reproduced")
            self.issue(line, f"unsupported tag: {name}")
        elif name in ("fg", "clfg") and self.cf:
            self.flush()
            self.cf_fg(name, attrs, line)
        elif name == "clall" and self.cf:
            self.flush()
            self.cf_clall(attrs, line)
        elif name == "partbg":
            self.flush()
            if self.cf:
                # Even an unconverted partbg changes the real screen, so later layers have no known base.
                unknown = ("unknown", "partbg is not composed with foreground layers")
                self.cf_base = unknown
                self.cf_fore = (unknown, [])
                self.cf_layers = []
                self.cf_shown = False
            try:
                background = self.assets.partial_background(attrs.get("storage", ""), attrs)
            except ValueError as exc:
                self.issue(line, f"partial background not converted: {exc}")
            else:
                seconds = self.transition(attrs, line)
                self.output.append("bg " + background + (f" fade {seconds:g}" if seconds else ""))
                self.transition_wait(attrs, seconds, line)
                self.issue(line, "partial background composited as a static image; layering and transition approximated")
        elif name == "trans":
            self.flush()
            seconds = self.transition(attrs, line)
            if self.cf and not attrs.keys() - {"time", "rule", "nowait", "textoff", "noback"}:
                if self.cf_publish(line, seconds):
                    self.transition_wait(attrs, seconds, line)
                    return False
            if self.apply_pending_background(line, seconds):
                self.transition_wait(attrs, seconds, line)
            if attrs.keys() - {"time", "rule", "nowait", "textoff"}:
                self.issue(line, "transition effect or options approximated")
        elif name == "wt":
            self.flush()
            canskip = attrs.get("canskip", "1")
            if canskip not in ("0", "1") or attrs.keys() - {"canskip"}:
                self.issue(line, "unsupported wt options; using unskippable wait")
                canskip = "0"
            self.output.append("waitbg" + (" skippable" if canskip == "1" else ""))
        elif name in ("play", "se"):
            self.flush()
            kind = "music" if name == "play" else "sound"
            asset = self.asset(attrs.get("storage", ""), kind, line)
            if asset:
                self.output.append(("music " if name == "play" else "sfx ") + asset)
            if attrs.keys() - {"storage"}:
                self.issue(line, f"{name} playback options approximated")
        elif name == "playstop":
            self.flush()
            time = attrs.get("time", "0")
            valid_time = time.isascii() and time.isdecimal() and int(time) <= 600000
            if valid_time:
                seconds = int(time) / 1000
                self.output.append("music stop" + (f" fade {seconds:g}" if seconds else ""))
            else:
                self.output.append("music stop")
                self.issue(line, "music stop time must be a literal 0..600000 milliseconds")
            if (attrs.keys() - {"time", "nowait"}
                    or attrs.get("nowait", "1") != "1"):
                self.issue(line, "music stop options approximated")
        elif name == "wait":
            self.flush()
            value = attrs.get("time", "")
            if value.isdecimal():
                self.output.append(f"wait {int(value) / 1000:g}")
                if attrs.keys() - {"time"}:
                    self.issue(line, "wait options approximated")
            else:
                self.issue(line, "wait requires a literal millisecond time")
        elif name == "return":
            self.flush()
            self.output.append("stop")
            return True
        elif name == "iscript":
            self.flush()
            self.in_script = True
            self.issue(line, "embedded TJS is not supported")
        else:
            self.flush()
            if self.cf and name in CF_LAYER_TAGS:
                self.cf_taint(f"{name} changes layers in an unmodelled way")
            self.issue(line, f"unsupported tag: {name}")
        return False

    def convert(self, text):
        for line_no, line in enumerate(text.splitlines(), 1):
            stripped = line.strip()
            if self.in_script:
                if stripped.lower() in ("@endscript", "[endscript]"):
                    self.in_script = False
                continue
            if not stripped or stripped.startswith((";", "*")):
                continue
            if stripped.startswith("@"):
                if self.command(stripped[1:], line_no):
                    break
                continue
            position = 0
            stop = False
            for match in TAG.finditer(line):
                self.pending += line[position:match.start()]
                if self.command(match.group(1), line_no):
                    stop = True
                    break
                position = match.end()
            if stop:
                break
            self.pending += line[position:]
        if self.in_script:
            self.issue(line_no, "unterminated embedded TJS block")
        if self.pending_loop:
            self.issue(line_no, "unapplied fgact image loop discarded")
        if self.cf and any("omitted" not in layer for layer in self.cf_layers) \
                and self.cf_snapshot()[1] != self.cf_fore[1]:
            self.issue(line_no, "foreground layers were never made visible by a transition")
        self.flush()
        if not self.output or self.output[-1] != "stop":
            self.output.append("stop")
        return self.output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="directory of .ks scripts or ZIP-format archive")
    parser.add_argument("output", type=Path, help="new 3DVN game directory")
    parser.add_argument("--scene", required=True, action="append",
                        help="scene path within source; repeat in the desired preview order")
    parser.add_argument("--encoding", default="auto", help="script encoding (default: UTF-16 BOM or UTF-8)")
    parser.add_argument("--pages", type=int, help="stop after this many page breaks")
    parser.add_argument("--preview", action="store_true", help="write an incomplete preview despite reported issues")
    parser.add_argument("--nvl-pages", action="store_true",
                        help="preserve line spacing and page clears for an accumulating NVL theme")
    parser.add_argument("--assets", type=Path, help="directory of extracted PNG/JPEG/BMP and audio files")
    parser.add_argument("--thumbnail-assets", action="store_true",
                        help="mark extracted thumbnail images as low-resolution approximations")
    parser.add_argument("--source-size", type=source_size, metavar="WIDTHxHEIGHT",
                        help="original scene canvas, required for background conversion")
    parser.add_argument("--scene-size", type=source_size, metavar="WIDTHxHEIGHT",
                        help="original coordinate canvas for partial backgrounds")
    parser.add_argument("--compose-foreground", action="store_true",
                        help="compose a verified subset of @fg/@clfg/@clall/@trans into static images")
    parser.add_argument("--ffmpeg", help="path to ffmpeg for audio conversion")
    args = parser.parse_args()
    if args.pages is not None and args.pages <= 0:
        parser.error("--pages must be positive")
    if len(args.scene) > 1 and not args.preview:
        parser.error("multiple scenes require --preview; route order is not interpreted")
    if len(args.scene) > 1 and args.pages is not None:
        parser.error("--pages cannot be combined with multiple scenes")
    if args.output.exists():
        parser.error("output already exists; choose a new directory")
    if args.assets and not args.assets.is_dir():
        parser.error("--assets must be an existing directory")
    if args.thumbnail_assets and not args.assets:
        parser.error("--thumbnail-assets requires --assets")
    if args.compose_foreground and not (args.assets and args.source_size):
        parser.error("--compose-foreground requires --assets and --source-size")
    if args.source.is_dir():
        root = args.source.resolve()
        for scene in args.scene:
            path = (root / scene).resolve()
            if not path.is_relative_to(root):
                parser.error("--scene must be inside the source directory")
    try:
        assets = Assets(args.assets, args.source_size, find_ffmpeg(args.ffmpeg),
                        args.scene_size, args.thumbnail_assets)
        converters = []
        output = []
        for scene in args.scene:
            text = read_scene(args.source, scene, args.encoding)
            converter = SceneConverter(scene, args.pages, assets, args.nvl_pages,
                                       args.compose_foreground)
            if args.thumbnail_assets:
                converter.issue(0, "backgrounds use low-resolution thumbnails, not full-resolution images")
            scene_output = converter.convert(text)
            if output:
                output.pop()  # Replace the previous scene's stop with the next scene.
                if args.nvl_pages:
                    output.append("page")
            output.extend(scene_output)
            converters.append(converter)
    except (OSError, ValueError, LookupError, zipfile.BadZipFile) as exc:
        parser.exit(1, f"conversion failed: {exc}\n")
    for converter in converters:
        summary = Counter(message for _, message in converter.issues)
        print(f"{converter.scene}: {converter.page_count} page breaks, {len(converter.issues)} issues")
        for message, count in summary.most_common():
            print(f"  {count}x {message}")
    incomplete = len(converters) > 1 or any(converter.issues for converter in converters)
    if incomplete and not args.preview:
        parser.exit(1, "Conversion incomplete; no output written. Use --preview to inspect an incomplete result.\n")
    report = []
    if len(converters) > 1:
        report.extend(["Experimental multi-scene preview; scene order was supplied manually.",
                       "Route charts, calls, branches and intervening scenes are not interpreted.", ""])
    for converter in converters:
        report.extend([f"Source scene: {converter.scene}",
                       "Experimental preview; not a complete conversion." if converter.issues
                       else "No unsupported tags encountered in this scene.",
                       f"Page breaks: {converter.page_count}", f"Issues: {len(converter.issues)}", ""])
        report.extend(f"{converter.scene}:{line}: {message}" for line, message in converter.issues)
        report.append("")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix=".3dvn-kirikiri-", dir=args.output.parent) as temp:
            staging = Path(temp)
            assets.write(staging)
            (staging / "main.3vs").write_text("\n".join(output) + "\n", encoding="utf-8")
            (staging / "conversion-report.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
            staging.rename(args.output)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"conversion failed: {exc}\n")
    print(f"Wrote {'incomplete preview' if incomplete else 'scene'} to {args.output}")


if __name__ == "__main__":
    main()
