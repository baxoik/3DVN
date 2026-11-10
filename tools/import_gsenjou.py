#!/usr/bin/env python3
"""Convert the translated G-Senjou no Maou KiriKiri scripts into a 3DVN game.

Usage:
  python3 tools/import_gsenjou.py --src "g-senjou no maou" --out games/g-senjou-no-maou
Options:
  --no-assets   skip converting source images and audio; still write the UI skin
  --voices      also convert the voice files (about 14,000 WAVs, several GB)
  --only g01    convert only the listed chapters (plus what they call)

Every chapter becomes ch/NAME.3vs and chapters are chained with the 3DVN
`script` command. Effects 3DVN cannot show (camera, quake, cut-ins, ...) are
dropped and counted in conversion-report.txt.
"""
import argparse
import collections
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
import zlib
from concurrent.futures import ThreadPoolExecutor

SCALE = 0.4            # 800x600 source -> 320x240 picture centred on the 400x240 screen
SCREEN_X = 40          # (400 - 320) / 2
SLOT_CENTERS = {"ll": 100, "l": 250, "c": 400, "r": 550, "rr": 700}
SLOT_IDS = {"ll": "sll", "l": "sl", "c": "sc", "r": "sr", "rr": "srr"}
SAY_LIMIT = 200

NAMES = {
    "京介": ("kyousuke", "Kyousuke"), "ハル": ("haru", "Haru"), "椿姫": ("tsubaki", "Tsubaki"),
    "花音": ("kanon", "Kanon"), "栄一": ("eiichi", "Eiichi"), "水羽": ("mizuha", "Mizuha"),
    "ユキ": ("yuki", "Yuki"), "魔王": ("maou", "Maou"), "まおう": ("maou", "Maou"),
    "浅井権三": ("gonzo", "Azai Gonzo"), "恭平": ("kyouhei", "Kyouhei"),
    "広明": ("hiroaki", "Hiroaki"), "パパ": ("papa", "Papa"), "西条": ("saijou", "Saijou"),
    "郁子": ("ikuko", "Ikuko"), "秋元": ("akimoto", "Akimoto"), "橋本": ("hashimoto", "Hashimoto"),
    "堀部": ("horibe", "Horibe"), "染谷": ("someya", "Someya"), "理事長": ("chairman", "Chairman"),
    "刑事": ("detective", "Detective"), "時田彰浩": ("tokita", "Tokita Akihiro"),
    "女教師": ("teacher_f", "Teacher"), "桑島": ("kuwajima", "Kuwajima"),
    "宇佐美義則": ("usami", "Usami Yoshinori"), "岩井": ("iwai", "Iwai"), "大男": ("bigman", "Big Man"),
    "店長": ("manager", "Manager"), "母": ("mother", "Mother"), "ママ": ("mama", "Mama"),
    "瀬田": ("seta", "Seta"), "鮫島利勝": ("samejima", "Samejima Toshikatsu"),
    "数学教師": ("mathteacher", "Math Teacher"), "小谷": ("kotani", "Kotani"),
    "ペンギン": ("penguin", "Penguin"), "飯島": ("iijima", "Iijima"), "叔父": ("uncle", "Uncle"),
    "ウェイター": ("waiter", "Waiter"), "ちろ美": ("chiromi", "Chiromi"), "孝明": ("takaaki", "Takaaki"),
    "紗枝": ("sae", "Sae"), "紗江": ("sae", "Sae"), "ヤクザ": ("yakuza", "Yakuza"),
    "子供１": ("child1", "Child 1"), "子供２": ("child2", "Child 2"), "役員": ("officer", "Officer"),
    "外国人": ("foreigner", "Foreigner"), "記者": ("reporter", "Reporter"),
    "記者１": ("reporter1", "Reporter 1"), "記者２": ("reporter2", "Reporter 2"),
    "記者３": ("reporter3", "Reporter 3"), "アナウンス": ("announcer", "Announcement"),
    "プロデューサー": ("producer", "Producer"), "客１": ("guest1", "Guest 1"),
    "客２": ("guest2", "Guest 2"), "課長": ("section_chief", "Section Chief"),
    "ヤクザ１": ("yakuza1", "Yakuza 1"), "少女": ("girl", "Girl"), "老人１": ("elder1", "Old Man 1"),
    "老人２": ("elder2", "Old Man 2"), "おばちゃん": ("auntie", "Auntie"), "今川": ("imagawa", "Imagawa"),
    "ハルの娘": ("harus_daughter", "Haru's Daughter"), "娘": ("daughter", "Daughter"),
    "ニュース": ("news", "News"), "Tsubaki": ("tsubaki", "Tsubaki"), "観客": ("audience", "Audience"),
    "声": ("voice", "Voice"), "ＡＶ女優": ("actress", "Actress"), "片倉": ("katakura", "Katakura"),
    "京介＆水羽": ("kyousuke_mizuha", "Kyousuke & Mizuha"),
}
NAME_COLORS = {"kyousuke": "#9CC8F0", "haru": "#F0A8C8", "tsubaki": "#F0C890",
               "kanon": "#F0E090", "mizuha": "#B8A0F0", "yuki": "#A0E0D0", "maou": "#E08080"}


BG_ALIASES = {"ev_mouw_04a": "ev_maou_04a"}  # typo in g08.ks; the comment above it names ev_maou_04a


def read_ks(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
        text = data.decode("utf-16", errors="replace")
    else:
        text = data.decode("utf-8-sig", errors="replace")
    return text.replace("\r\n", "\n").replace("\r", "\n").split("\n")


def parse_args(rest):
    return {key: value.strip('"') for key, value in re.findall(r'(\w+)=("[^"]*"|\S+)', rest)}


def sanitize(text):
    return re.sub(r"[^A-Za-z0-9_]", "_", text)


def write_ui_theme(out):
    from PIL import Image, ImageDraw

    ui_dir = os.path.join(out, "ui")
    os.makedirs(ui_dir, exist_ok=True)

    def write_panel(name, size, alpha):
        image = Image.new("RGBA", size, (5, 6, 8, alpha))
        draw = ImageDraw.Draw(image)
        right, bottom = size[0] - 1, size[1] - 1
        draw.line((0, 0, right, 0), fill=(191, 191, 191, 130))
        for y in range(10, bottom, 4):
            draw.line((0, y, right, y), fill=(20, 20, 22, alpha))
        image.save(os.path.join(ui_dir, name), optimize=True)

    write_panel("textbox.png", (400, 90), 160)
    write_panel("nvl_textbox.png", (384, 224), 180)


def escape(text):
    return text.replace("\\", "\\\\").replace('"', '\\"').replace("{", "\\{")


def split_sentences(text):
    if len(text) <= SAY_LIMIT:
        return [text]
    pieces, current = [], ""
    for part in re.split(r"(?<=[.!?…”’])\s+", text):
        while len(part) > 2 * SAY_LIMIT:
            cut = part.rfind(" ", 0, SAY_LIMIT)
            cut = cut if cut > 0 else SAY_LIMIT
            part, rest = part[:cut], part[cut:].lstrip()
            pieces.append((current + " " + part).strip() if current else part)
            current, part = "", rest
        if current and len(current) + 1 + len(part) > SAY_LIMIT:
            pieces.append(current)
            current = part
        else:
            current = (current + " " + part).strip()
    if current:
        pieces.append(current)
    return pieces


class Assets:
    """Finds, converts and caches the images and audio a script uses."""

    def __init__(self, src, out, enabled, voices, bgm_rate, voice_rate):
        self.out, self.enabled, self.voices = out, enabled, voices
        self.bgm_rate, self.voice_rate = bgm_rate, voice_rate
        self.zips = {}
        self.index = {}
        self.sprite_cache = {}
        self.missing = collections.defaultdict(set)
        self.used = collections.defaultdict(set)
        self.audio_jobs = []
        if True:
            base = os.path.join(src, "Img-Sound Files")
            for kind in ("background", "foreground", "sound"):
                path = os.path.join(base, kind + ".zip")
                if os.path.exists(path):
                    archive = zipfile.ZipFile(path)
                    self.zips[kind] = archive
                    for name in archive.namelist():
                        if not name.endswith("/"):
                            self.index[(kind, name)] = True

    def _find(self, kind, folders, stem, exts):
        for folder in folders:
            for ext in exts:
                name = f"{kind}/{folder}{stem}{ext}"
                if (kind, name) in self.index:
                    return name
        return None

    def background(self, stem):
        if stem in ("black", "white"):
            return stem
        stem = BG_ALIASES.get(stem, stem)
        if stem == "ev_fin":
            return self.end_card(stem)
        self.used["bg"].add(stem)
        found = self._find("background", ["", "ev/", "special/"], stem, [".jpg", ".png"])
        source = ("background", found) if found else None
        if source is None:
            found = self._find("foreground", ["ev/"], stem, [".png"])
            source = ("foreground", found) if found else None
        if source is None:
            self.missing["bg"].add(stem)
            return stem
        if self.enabled:
            target = os.path.join(self.out, "bg", stem + ".jpg")
            if not os.path.exists(target):
                from PIL import Image
                image = Image.open(io.BytesIO(self.zips[source[0]].read(source[1])))
                if image.mode == "RGBA":
                    back = Image.new("RGB", image.size, (0, 0, 0))
                    back.paste(image, mask=image.getchannel("A"))
                    image = back
                image = image.convert("RGB").resize((320, 240), Image.LANCZOS)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                image.save(target, quality=88)
        return stem

    def gallery_assets(self):
        stems = sorted({
            os.path.splitext(path.rsplit("/", 1)[-1])[0]
            for archive, path in self.index
            if archive == "background" and path.startswith("background/ev/")
            and path.lower().endswith((".jpg", ".jpeg", ".png"))
        })
        if not self.enabled:
            return len(stems)

        from PIL import Image
        thumb_dir = os.path.join(self.out, "gallery", "thumbs")
        os.makedirs(thumb_dir, exist_ok=True)
        for stem in stems:
            self.background(stem)
            source = os.path.join(self.out, "bg", stem + ".jpg")
            target = os.path.join(thumb_dir, stem + ".jpg")
            if os.path.isfile(source) and not os.path.exists(target):
                with Image.open(source) as image:
                    image = image.convert("RGB")
                    image.thumbnail((112, 78), Image.LANCZOS)
                    image.save(target, quality=76, optimize=True)
        return len(stems)

    def sprite(self, stem):
        """Returns (name, x_offset, y_offset) in 400x240 pixels for a slot centred at 0, or None."""
        if stem in self.sprite_cache:
            return self.sprite_cache[stem]
        self.used["sprite"].add(stem)
        found = self._find("foreground", ["chr/"], "st_" + stem, [".png"])
        result = None
        if found is None:
            self.missing["sprite"].add(stem)
        else:
            from PIL import Image
            image = Image.open(io.BytesIO(self.zips["foreground"].read(found))).convert("RGBA")
            width, height = image.size
            box = image.getchannel("A").getbbox() or (0, 0, width, height)
            left, top, right, bottom = box
            cropped = image.crop(box)
            size = (max(1, round(cropped.width * SCALE)), max(1, round(cropped.height * SCALE)))
            if self.enabled:
                target = os.path.join(self.out, "sprites", stem + ".png")
                os.makedirs(os.path.dirname(target), exist_ok=True)
                cropped.resize(size, Image.LANCZOS).save(target, optimize=True)
            result = (stem, SCALE * (left - width / 2), SCALE * (600 - height + top))
        self.sprite_cache[stem] = result
        return result

    def face(self, name):
        """Speaker portrait shown above the text box; returns the sprite name or None."""
        base = re.sub(r"_[bs]$", "", name)
        key = ("face", base)
        if key in self.sprite_cache:
            return self.sprite_cache[key]
        self.used["face"].add(base)
        found = self._find("foreground", ["face/"], "st_" + base + "_f", [".png"])
        result = None
        if found is None:
            self.missing["face"].add(base)
        else:
            from PIL import Image
            image = Image.open(io.BytesIO(self.zips["foreground"].read(found))).convert("RGBA")
            cropped = image.crop(image.getchannel("A").getbbox() or (0, 0, image.width, image.height))
            size = (max(1, round(cropped.width * SCALE)), max(1, round(cropped.height * SCALE)))
            result = "face_" + base
            if self.enabled:
                target = os.path.join(self.out, "sprites", result + ".png")
                os.makedirs(os.path.dirname(target), exist_ok=True)
                cropped.resize(size, Image.LANCZOS).save(target, optimize=True)
        self.sprite_cache[key] = result
        return result

    def overlay(self, stem, x, y, clip=None):
        """Return cut-in art cropped to its visible area within the original 800x600 scene."""
        key = ("overlay", stem, x, y, clip)
        if key in self.sprite_cache:
            return self.sprite_cache[key]
        self.used["sprite"].add("cut:" + stem)
        found = self._find("foreground", ["ev/"], stem, [".png"])
        source = "foreground" if found else None
        if not found:
            found = self._find("background", ["ev/"], stem, [".jpg", ".png"])
            source = "background" if found else None
        result = None
        if not found:
            self.missing["cutin"].add(stem)
        else:
            from PIL import Image
            image = Image.open(io.BytesIO(self.zips[source].read(found))).convert("RGBA")
            if clip:
                cl, ct, cw, ch = clip
                image = image.crop((cl, ct, cl + cw, ct + ch))
            else:
                # The dump pads cut-in art to a canvas; script coordinates position
                # the visible image, not that transparent padding.
                bounds = image.getchannel("A").getbbox()
                if bounds is None:
                    self.missing["cutin"].add(stem + " (transparent)")
                    self.sprite_cache[key] = None
                    return None
                image = image.crop(bounds)
            left, top = max(0, -x), max(0, -y)
            right, bottom = min(image.width, 800 - x), min(image.height, 600 - y)
            if right <= left or bottom <= top:
                self.missing["cutin"].add(stem + " (outside scene)")
                self.sprite_cache[key] = None
                return None
            image = image.crop((left, top, right, bottom))
            box = image.getchannel("A").getbbox()
            if box is None:
                self.missing["cutin"].add(stem + " (transparent)")
                self.sprite_cache[key] = None
                return None
            cropped = image.crop(box)
            size = (max(1, round(cropped.width * SCALE)), max(1, round(cropped.height * SCALE)))
            name = f"cut_{stem}_{x}_{y}" + ("" if not clip else "_" + "_".join(str(v) for v in clip))
            if self.enabled:
                target = os.path.join(self.out, "sprites", name + ".png")
                os.makedirs(os.path.dirname(target), exist_ok=True)
                cropped.resize(size, Image.LANCZOS).save(target, optimize=True)
            result = (name, left + box[0], top + box[1])
        self.sprite_cache[key] = result
        return result

    def end_card(self, stem):
        """The original ev_fin art is not in the dump; draw a simple ending card."""
        self.used["bg"].add(stem)
        if self.enabled:
            target = os.path.join(self.out, "bg", stem + ".jpg")
            if not os.path.exists(target):
                from PIL import Image, ImageDraw, ImageFont
                image = Image.new("RGB", (320, 240), (0, 0, 0))
                draw = ImageDraw.Draw(image)
                font_path = next((path for path in (
                    "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf",
                    "/usr/share/fonts/truetype/lato/Lato-Medium.ttf") if os.path.exists(path)), None)
                big = ImageFont.truetype(font_path, 34) if font_path else ImageFont.load_default()
                small = ImageFont.truetype(font_path, 14) if font_path else ImageFont.load_default()
                for text, font, y in (("FIN", big, 80), ("G-Senjou no Maou", small, 140)):
                    width = draw.textlength(text, font=font)
                    draw.text(((320 - width) / 2, y), text, fill=(230, 230, 230), font=font)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                image.save(target, quality=92)
        return stem

    def synth_click(self, stem):
        """keyenter is missing from the dump; synthesize a short keyboard click."""
        self.used["se"].add(stem)
        if self.enabled:
            import random
            import struct
            import wave
            target = os.path.join(self.out, "audio", stem + ".wav")
            if not os.path.exists(target):
                os.makedirs(os.path.dirname(target), exist_ok=True)
                rng = random.Random(7)
                rate = self.bgm_rate
                frames = []
                for index in range(int(rate * 0.09)):
                    envelope = (1.0 - index / (rate * 0.09)) ** 3
                    frames.append(int(14000 * envelope * rng.uniform(-1, 1)))
                with wave.open(target, "wb") as handle:
                    handle.setnchannels(1)
                    handle.setsampwidth(2)
                    handle.setframerate(rate)
                    handle.writeframes(struct.pack("<%dh" % len(frames), *frames))
        return stem

    def music(self, stem):
        self.used["bgm"].add(stem)
        return self._audio("sound/bgm/" + stem + ".ogg", stem, self.bgm_rate, "bgm")

    def duration(self, stem):
        """Length in seconds of a sound effect, used to turn @wse into a wait."""
        member = "sound/" + stem + ".ogg"
        if ("sound", member) not in self.index:
            return 0.0
        import subprocess
        import tempfile
        with tempfile.NamedTemporaryFile(suffix=".ogg") as handle:
            handle.write(self.zips["sound"].read(member))
            handle.flush()
            result = subprocess.run(
                ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", handle.name],
                capture_output=True)
        try:
            return float(result.stdout.decode().strip())
        except ValueError:
            return 0.0

    def sound(self, stem):
        if stem == "keyenter":
            return self.synth_click(stem)
        self.used["se"].add(stem)
        return self._audio("sound/" + stem + ".ogg", stem, self.bgm_rate, "se")

    def voice(self, stem):
        if not self.voices:
            return None
        self.used["voice"].add(stem)
        return self._audio("sound/voice/" + stem + ".ogg", stem, self.voice_rate, "voice")

    def _audio(self, member, stem, rate, kind):
        if ("sound", member) not in self.index:
            self.missing[kind].add(stem)
            return None
        if self.enabled:
            self.audio_jobs.append((member, os.path.join(self.out, "audio", stem + ".wav"), rate))
        return stem

    def run_audio(self):
        jobs = {target: (member, target, rate) for member, target, rate in self.audio_jobs}
        jobs = [job for job in jobs.values() if not os.path.exists(job[1])]
        os.makedirs(os.path.join(self.out, "audio"), exist_ok=True)

        def convert(job):
            member, target, rate = job
            with tempfile.TemporaryDirectory() as folder:
                source = os.path.join(folder, "in.ogg")
                with zipfile.ZipFile(self.zips["sound"].filename) as archive:
                    with open(source, "wb") as handle:
                        handle.write(archive.read(member))
                subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", source, "-ac", "1", "-ar",
                                str(rate), "-sample_fmt", "s16", "-f", "wav", target + ".part"],
                               check=True)
                os.replace(target + ".part", target)

        with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
            for count, _ in enumerate(pool.map(convert, jobs), 1):
                if count % 500 == 0:
                    print(f"  audio {count}/{len(jobs)}")
        return len(jobs)


class Converter:
    def __init__(self, src, assets):
        self.dir = os.path.join(src, "1-raw-scripts")
        self.assets = assets
        self.stats = collections.Counter()
        self.characters = {}
        self.queue = []
        self.done = set()
        self.untranslated = collections.Counter()

    # ---- helpers -------------------------------------------------------
    def character_id(self, jp):
        ident, display = NAMES.get(jp, (None, None))
        if ident is None:
            ident = "c_" + sanitize(jp)[:20] if jp.isascii() else "x%04x" % (zlib.crc32(jp.encode()) % 65536)
            display = jp
            self.stats["unmapped speaker " + jp] += 1
        self.characters[ident] = display
        return ident

    def convert_expression(self, text):
        text = text.strip()
        text = re.sub(r"\b(?:f|tf|sf)\.(\w+)", r"\1", text)
        return text.replace("global.nonero", "nonero")

    def convert_assignment(self, text):
        match = re.fullmatch(r"\s*f\.(\w+)\s*(\+=|-=|=)\s*(.+?)\s*", text)
        if not match:
            return None
        value = {"true": "1", "false": "0"}.get(match.group(3), match.group(3))
        return f"set {match.group(1)} {match.group(2)} {value}"

    # ---- one file ------------------------------------------------------
    def convert_file(self, name, subroutine=False):
        stem = name[:-3] if name.endswith(".ks") else name
        path = os.path.join(self.dir, stem + ".ks")
        lines = read_ks(path)
        body = []
        out = body.append
        referenced = set(re.findall(r'target="?\*(\w+)', "\n".join(lines)))
        state = {"slots": {}, "all_unknown": False, "speaker": None, "voice": None,
                 "pending": None, "page_open": False, "segments": 0, "skip": False,
                 "menu": []}

        def label_name(label):
            return f"{sanitize(stem)}_{label}"

        def flush_pending(last):
            pending = state["pending"]
            if pending is None:
                return
            speaker, voice, text = pending
            state["pending"] = None
            for index, piece in enumerate(split_sentences(text)):
                last_piece = last and index == len(split_sentences(text)) - 1
                shown = piece if last_piece else piece + " "
                if voice and index == 0:
                    sound = self.assets.voice(voice)
                    if sound:
                        out(f"sfx {sound}")
                prefix = f"{speaker}: " if speaker else ""
                out(f'{prefix}"{escape(shown)}"')
                state["page_open"] = True

        def add_segment(text):
            text = text.replace("\u3000", " ").strip()
            if not text:
                return
            if re.search(r"[\u3040-\u30ff\u4e00-\u9fff]", text):
                self.stats["dropped untranslated duplicate line"] += 1
                return
            flush_pending(False)
            state["pending"] = (state["speaker"], state["voice"], text)
            state["voice"] = None

        def end_page():
            flush_pending(True)
            if state["page_open"]:
                out("page")
            state["page_open"] = False
            state["speaker"] = None
            state["voice"] = None

        def hide_all():
            if state.get("face"):
                out("hide face")
                state["face"] = None
            for ident in state.get("cuts", {}).values():
                out(f"hide {ident}")
            state["cuts"] = {}
            for slot in SLOT_IDS:
                if slot in state["slots"] or state["all_unknown"]:
                    out(f"hide {SLOT_IDS[slot]}")
            state["slots"].clear()
            state["all_unknown"] = False

        def show_slot(slot, image):
            if slot not in SLOT_IDS:
                return
            sprite = self.assets.sprite(image)
            if sprite is None:
                return
            if state["slots"].get(slot) == image:
                return
            state["slots"][slot] = image
            x = round(SCREEN_X + SCALE * SLOT_CENTERS[slot] + sprite[1])
            y = round(sprite[2])
            out(f"show {SLOT_IDS[slot]} {sprite[0]} at {x} {y}")

        def fade_of(args):
            if "time" in args and args["time"].isdigit() and int(args["time"]) > 0:
                return f" fade {int(args['time']) / 1000:g}"
            return ""

        def change_bg(storage, args, keep_sprites=False):
            if not keep_sprites:
                hide_all()
            out(f"bg {self.assets.background(storage)}{fade_of(args)}")

        def slots_of(text):
            slots, index = [], 0
            while index < len(text):
                if text[index:index + 2] in ("ll", "rr"):
                    slots.append(text[index:index + 2])
                    index += 2
                else:
                    slots.append(text[index])
                    index += 1
            return [slot for slot in slots if slot in SLOT_IDS]

        def hide_slots(slot_list):
            for slot in slot_list:
                out(f"hide {SLOT_IDS[slot]}")
                state["slots"].pop(slot, None)

        def flush_menu():
            if not state["menu"]:
                return
            out("menu")
            for text, target, expression in state["menu"]:
                out(f'  choice "{escape(text)}"')
                if expression:
                    converted = self.convert_assignment(expression)
                    if converted:
                        out("    " + converted)
                    else:
                        self.stats["ignored exlink exp"] += 1
                out("    " + target)
            out("end")
            state["menu"] = []

        def jump_to(args):
            storage = args.get("storage")
            if storage:
                target_stem = storage[:-3] if storage.endswith(".ks") else storage
                if target_stem in ("title", "ex_evmode"):
                    return "stop" if target_stem == "title" else None
                self.enqueue(target_stem)
                return f'script "ch/{target_stem}"'
            if "target" in args:
                return f"jump {label_name(args['target'].lstrip('*'))}"
            return None

        for number, raw in enumerate(lines, 1):
            line = raw.rstrip()
            if state["skip"]:
                if line.startswith("@endscript"):
                    state["skip"] = False
                continue
            if not line.strip() or line.startswith(";"):
                continue
            if state["menu"] and not line.startswith("@exlink") and not line.startswith("@showexlink"):
                flush_menu()
            if line.startswith("*"):
                label = line[1:].split("|")[0].strip()
                if label in referenced:
                    end_page()
                    out(f"label {label_name(label)}")
                    state["all_unknown"] = True
                    state["face"] = "?"
                    state["slots"].clear()
                continue
            if line.startswith("@"):
                parts = line[1:].split(None, 1)
                command = parts[0] if parts else ""
                args = parse_args(parts[1] if len(parts) > 1 else "")
                if command not in ("np", "r"):
                    flush_pending(False)
                self.command(command, args, parts, state, out, label_name, change_bg, show_slot,
                             hide_all, hide_slots, slots_of, jump_to, end_page, fade_of, stem)
                continue
            self.text_line(line, state, add_segment, end_page, show_slot)
        end_page()
        flush_menu()
        return body

    def command(self, command, args, parts, state, out, label_name, change_bg, show_slot,
                hide_all, hide_slots, slots_of, jump_to, end_page, fade_of, stem):
        stats = self.stats
        bare = parts[1].split()[0] if len(parts) > 1 and "=" not in parts[1].split()[0] else None
        if command in ("bg", "ev") and "storage" not in args and bare:
            args["storage"] = bare
        if command == "np":
            end_page()
        elif command == "r":
            pass
        elif command in ("bg", "ev", "bg_mosaic", "ev_mosaic") and "storage" in args:
            keep = "left" in args or "top" in args
            change_bg(args["storage"], args, keep)
        elif command in ("black", "white"):
            hide_all()
            out(f"bg {command}{fade_of(args)}")
        elif command == "hide":
            hide_all()
        elif command in ("chr", "chr_walk", "chr_dash"):
            for slot in SLOT_IDS:
                if slot in args:
                    show_slot(slot, args[slot])
        elif command in ("dellay", "dellay_walk", "dellay_dash", "dellay_far"):
            slot_list = slots_of(args.get("pos", "")) if not args.get("pos", "0").isdigit() else None
            if slot_list is None:
                hide_all()
            else:
                hide_slots(slot_list)
            for key in args:
                if key.endswith("way") and key[:-3] in SLOT_IDS:
                    hide_slots([key[:-3]])
        elif command == "chr_pos_change":
            before, after = args.get("before"), args.get("after")
            image = state["slots"].get(before)
            if image and after in SLOT_IDS:
                hide_slots([before])
                show_slot(after, image)
            else:
                stats["chr_pos_change without known sprite"] += 1
        elif command in ("quake", "squake"):
            amp = max([int(args[key]) for key in ("sx", "sy", "hamx", "vmax") if args.get(key, "").lstrip("-").isdigit()]
                      or [6])
            seconds = int(args["time"]) / 1000 if args.get("time", "").isdigit() else 0.5
            out(f"shake {min(seconds, 3):g} {max(2, min(amp // 2, 14))}")
        elif command == "mface" and "name" in args:
            face = self.assets.face(args["name"].strip('"'))
            if face:
                if state.get("face") != face:
                    out(f"show face {face} at face")
                    state["face"] = face
            else:
                stats["mface image missing"] += 1
        elif command == "clear_mface":
            if state.get("face"):
                out("hide face")
                state["face"] = None
        elif command in ("cutin", "strans") and "storage" in args:
            clip = None
            if all(args.get(k, "").isdigit() for k in ("clipleft", "cliptop", "clipwidth", "clipheight")):
                clip = tuple(int(args[k]) for k in ("clipleft", "cliptop", "clipwidth", "clipheight"))
            layer = args.get("layer")
            cuts = state.setdefault("cuts", {})
            if command == "cutin":
                point = re.findall(r"\((-?\d+),\s*(-?\d+)", args.get("path", ""))
                px, py = (int(v) for v in point[-1]) if point else (int(args.get("x", 0)), int(args.get("y", 0)))
                key = layer or args["storage"]
            else:
                key = layer or state.get("cut_last") or args["storage"]
                last = state.get("cut_pos", (0, 0))
                px = int(args["left"]) if args.get("left", "").lstrip("-").isdigit() else last[0]
                py = int(args["top"]) if args.get("top", "").lstrip("-").isdigit() else last[1]
            art = self.assets.overlay(args["storage"].strip('"'), px, py, clip)
            if art is None:
                stats["cutin image missing"] += 1
            else:
                ident = cuts.setdefault(key, f"cut{len(cuts) + 1}")
                screen_x = round(SCREEN_X + SCALE * (px + art[1]))
                screen_y = round(SCALE * (py + art[2]))
                seconds = min(1.0, int(args["time"]) / 1000) if args.get("time", "").isdigit() else 0.4
                out(f"show {ident} {art[0]} at {screen_x} {screen_y} fade {seconds:g}")
                state["cut_last"], state["cut_pos"] = key, (px, py)
        elif command in ("camera", "camera_small"):
            angle = args.get("angle", "c")[:1]
            angle = angle if angle in ("l", "c", "r", "d") else "c"
            seconds = int(args["time"]) / 1000 if args.get("time", "").isdigit() else 0.5
            out(f"camera {angle} {seconds:g}" + (" small" if command == "camera_small" else ""))
        elif command == "chr_jump":
            slot = args.get("pos", "c")
            if slot in state["slots"]:
                out(f"hop {SLOT_IDS[slot]}")
            else:
                stats["chr_jump without known sprite"] += 1
        elif command == "displeasure":
            seconds = int(args["time"]) / 1000 if args.get("time", "").isdigit() else 1.0
            out(f"shake {min(seconds, 3):g} 5")
        elif command in ("sq", "stopquake", "wq", "wm", "stopmove"):
            pass
        elif command == "flash":
            out("flash 0.4 white")
        elif command == "flash_3times":
            for index in range(3):
                out("flash 0.2 white")
                out("wait 0.25")
        elif command == "wait":
            if args.get("time", "").isdigit() and int(args["time"]) > 0:
                out(f"wait {int(args['time']) / 1000:g}")
        elif command in ("bgm", "fibgm", "xbgm") and "storage" in args:
            sound = self.assets.music(args["storage"])
            if sound:
                out(f"music {sound}{fade_of(args)}")
        elif command in ("fobgm", "sbgm"):
            out(f"music stop{fade_of(args)}")
        elif command in ("se", "fise") and "storage" in args:
            sound = self.assets.sound(args["storage"])
            if sound:
                state["last_se"] = args["storage"]
                out(f"sfx {sound}")
        elif command == "sse":
            out("sfx stop")
        elif command == "wse":
            length = self.assets.duration(state["last_se"]) if state.get("last_se") else 0.0
            if length > 0:
                out(f"wait {min(length, 10):.1f}")
        elif command == "exlink":
            if "storage" in args and "target" not in args:
                target = jump_to(args)
            else:
                target = jump_to({"target": args.get("target", "*end")})
            state["menu"].append((args.get("txt", ""), target or "stop", args.get("exp")))
        elif command == "showexlink":
            pass
        elif command == "jump":
            target = jump_to(args)
            condition = args.get("cond")
            if target is None:
                stats["jump ignored"] += 1
            elif condition and re.search(r"tf\.go_next_chapter|tf\.evcgmode", condition):
                stats["chapter-break/gallery jump removed"] += 1
            elif condition:
                out(f"if {self.convert_expression(condition)}")
                out(f"  {target}")
                out("end")
            else:
                out(target)
        elif command == "call":
            target_stem = args.get("storage", "").strip('"')
            target_stem = target_stem[:-3] if target_stem.endswith(".ks") else target_stem
            if target_stem in ("staffroll_text", ""):
                stats["call ignored " + target_stem] += 1
            else:
                self.subroutines.add(target_stem)
                out(f"call sub_{sanitize(target_stem)}")
        elif command == "return":
            out("return")
        elif command == "if":
            out(f"if {self.convert_expression(args.get('exp', '0'))}")
        elif command == "endif":
            out("end")
        elif command == "eval":
            converted = self.convert_assignment(args.get("exp", ""))
            if converted:
                out(converted)
            else:
                stats["eval ignored (system flag)"] += 1
        elif command == "iscript":
            state["skip"] = True
        elif command in ("waitclick", "show", "avg", "novel", "wjump"):
            pass
        else:
            stats["ignored @" + command] += 1

    def text_line(self, line, state, add_segment, end_page, show_slot):
        position = 0
        buffer = ""
        for match in re.finditer(r"\[(\w+)([^\]]*)\]", line):
            buffer += line[position:match.start()]
            position = match.end()
            tag, rest = match.group(1), match.group(2)
            if tag == "nm":
                add_segment(buffer)
                buffer = ""
                args = parse_args(rest)
                if "t" in args:
                    state["speaker"] = self.character_id(args["t"])
                    state["voice"] = args.get("s")
                continue
            if tag in ("l", "wvl", "wveh"):
                add_segment(buffer)
                buffer = ""
            elif tag == "np":
                add_segment(buffer)
                buffer = ""
                end_page()
            elif tag == "r":
                buffer += " "
            elif tag in ("chr", "chr2"):
                args = parse_args(rest)
                for slot in SLOT_IDS:
                    if slot in args:
                        show_slot(slot, args[slot])
            else:
                self.stats["ignored inline [" + tag + "]"] += 1
        buffer += line[position:]
        add_segment(buffer)

    def enqueue(self, stem):
        if stem not in self.done and stem not in self.queue and os.path.exists(
                os.path.join(self.dir, stem + ".ks")):
            self.queue.append(stem)

    def convert_all(self, first):
        chapters = {}
        self.queue = list(first)
        while self.queue:
            stem = self.queue.pop(0)
            if stem in self.done:
                continue
            self.done.add(stem)
            self.subroutines = set()
            body = self.convert_file(stem)
            appended = []
            seen = set()
            while self.subroutines - seen:
                sub = sorted(self.subroutines - seen)[0]
                seen.add(sub)
                extra = self.convert_file(sub, subroutine=True)
                appended.append(f"label sub_{sanitize(sub)}")
                appended.extend(extra)
                appended.append("return")
            if not body or not body[-1].startswith(("stop", "script", "jump")):
                body.append("stop")
            chapters[stem] = body + appended
        return chapters


def write_game(out, chapters, converter):
    os.makedirs(os.path.join(out, "ch"), exist_ok=True)
    write_ui_theme(out)
    with open(os.path.join(out, "characters.3vs"), "w", encoding="utf-8") as handle:
        for ident, display in sorted(converter.characters.items()):
            color = NAME_COLORS.get(ident, "")
            handle.write(f'character {ident} "{escape(display)}" {color}\n'.rstrip() + "\n")
    for stem, body in chapters.items():
        with open(os.path.join(out, "ch", stem + ".3vs"), "w", encoding="utf-8") as handle:
            handle.write("include characters.3vs\n")
            handle.write("\n".join(body) + "\n")
    main = [
        "# G-Senjou no Maou (fan translation), converted for 3DVN.",
        "bg black",
        "set nonero = 0",
        '"This story contains adult scenes. Include them?"',
        "menu",
        '  choice "Include adult scenes"',
        "    set nonero = 0",
        '  choice "Skip adult scenes"',
        "    set nonero = 1",
        "end",
        "page",
        'script "ch/g01"',
    ]
    with open(os.path.join(out, "main.3vs"), "w", encoding="utf-8") as handle:
        handle.write("\n".join(main) + "\n")
    with open(os.path.join(out, "game.3vc"), "w", encoding="utf-8") as handle:
        handle.write("# Converted G-Senjou no Maou.\ntitle G-Senjou no Maou\nmenu_title G-Senjou no Maou\n"
                     "accent #C04048\npanel #120F12\ntext #F2EEE8\n"
                     "textbox_img textbox.png\n"
                     "nvl_textbox_img nvl_textbox.png\n"
                     "textbox_x 0\ntextbox_y 150\ntextbox_w 400\ntextbox_h 90\n"
                     "textbox_alpha 160\ntext_x 12\ntext_y 26\nnarration_y 10\ntext_size 48\n"
                     "name_x 12\nname_y 5\nname_size 48\nface_box 1\n"
                     "nvl_dim 110\nnvl_border 0\nnvl_margin_x 14\nnvl_margin_y 14\n"
                     "nvl_accumulate 1\ndefault_display adv\n")
    with open(os.path.join(out, "extras.3vs"), "w", encoding="utf-8") as handle:
        handle.write('"G-Senjou no Maou CG Gallery"\ngallery\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", default="g-senjou no maou")
    parser.add_argument("--out", default="games/g-senjou-no-maou")
    parser.add_argument("--no-assets", action="store_true",
                        help="skip source image/audio conversion (still generate the UI skin)")
    parser.add_argument("--voices", action="store_true")
    parser.add_argument("--bgm-rate", type=int, default=22050)
    parser.add_argument("--voice-rate", type=int, default=16000)
    parser.add_argument("--only", nargs="*", default=["g01"])
    options = parser.parse_args()

    assets = Assets(options.src, options.out, not options.no_assets, options.voices,
                    options.bgm_rate, options.voice_rate)
    converter = Converter(options.src, assets)
    chapters = converter.convert_all(options.only)
    os.makedirs(options.out, exist_ok=True)
    write_game(options.out, chapters, converter)
    if not options.no_assets:
        print("converting audio ...")
        assets.run_audio()
    gallery_count = assets.gallery_assets()

    report = [f"Chapters converted: {len(chapters)}", f"Gallery CGs: {gallery_count}", ""]
    report.append("Dropped or approximated (count):")
    for key, value in converter.stats.most_common():
        report.append(f"  {value:6d}  {key}")
    report.append("")
    report.append("Pages still containing Japanese text:")
    for key, value in sorted(converter.untranslated.items()):
        report.append(f"  {key}: {value}")
    report.append("")
    for kind, names in sorted(assets.missing.items()):
        report.append(f"Missing {kind} assets ({len(names)}): " + ", ".join(sorted(names)[:80]))
    report.append("")
    for kind, names in sorted(assets.used.items()):
        report.append(f"Referenced {kind}: {len(names)}")
    with open(os.path.join(options.out, "conversion-report.txt"), "w", encoding="utf-8") as handle:
        handle.write("\n".join(report) + "\n")
    print("\n".join(report[:40]))


if __name__ == "__main__":
    sys.exit(main())
