#!/usr/bin/env python3
"""Convert the English PC release of Ever17 into a 3DVN game folder.

usage: e17_convert.py GAME_DIR OUT_DIR [--no-audio] [--no-images] [--jobs N]

GAME_DIR  the installed game (bg.dat, chara.dat, script.dat, voice.dat, ...; the
          Himmel Edition patch can already be applied - its graph/ images win)
OUT_DIR   new 3DVN game folder (e.g. <3dvn root>/games/ever17)

Output: bg/ (320x240) and hd/bg/ (800x600) backgrounds and CGs, sprites/ and
hd/sprites/ characters, audio/ music, sound effects and voices as Ogg Vorbis,
ch/<scene>.3vs scripts, characters.3vs, main.3vs and game.3vc.

Ported from Ever17Converter by weeaboo (GPLv2): the script semantics follow its
CommandHandler / GraphicsState / Choice classes, but scripts are written for 3DVN
directly (real sprite layers instead of pre-composited VNDS images). GPLv2.
"""
import argparse, concurrent.futures, json, os, re, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kid_formats as kid
import e17_script as e17

SRC_W, SRC_H = 800, 600          # original screen
SCALE = 0.4                      # 800x600 -> 320x240 on the 400x240 top screen
SCREEN_X = 40                    # left edge of the 320-wide game area
GLOBAL_VARS = range(0x01, 0x20)  # KID global flags (endings seen, routes unlocked)


# ---------------------------------------------------------------- resources

def save_scaled(image, small_path, hd_path, jpeg):
    os.makedirs(os.path.dirname(small_path), exist_ok=True)
    os.makedirs(os.path.dirname(hd_path), exist_ok=True)
    from PIL import Image
    small = image.resize((max(1, round(image.width * SCALE)), max(1, round(image.height * SCALE))), Image.LANCZOS)
    if jpeg:
        small.convert("RGB").save(small_path, "JPEG", quality=90)
        image.convert("RGB").save(os.path.splitext(hd_path)[0] + ".png", "PNG")   # lossless
    else:
        small.save(small_path, "PNG", optimize=True)
        image.save(hd_path, "PNG")


def image_job(job):
    """Decode one archived image; returns (name, kind, (w, h, left, top)) or an error string."""
    kind, name, data, out = job
    from PIL import Image
    try:
        image, offset = kid.cps_to_image(data)
        stem = os.path.splitext(name)[0]
        if kind == "bg":
            if image.size != (SRC_W, SRC_H):
                full = Image.new("RGB", (SRC_W, SRC_H))
                full.paste(image.convert("RGB"), offset)
                image = full
            save_scaled(image, f"{out}/bg/{stem}.jpg", f"{out}/hd/bg/{stem}.png", True)
            return stem, kind, (SRC_W, SRC_H, 0, 0)
        save_scaled(image, f"{out}/sprites/{stem}.png", f"{out}/hd/sprites/{stem}.png", False)
        return stem, kind, (image.width, image.height, offset[0], offset[1])
    except Exception as error:
        return f"{name}: {error}"


def convert_images(game, out, jobs):
    """Backgrounds and CGs (bg.dat) and sprites (chara.dat). Loose files in graph/bg and
    graph/chara (e.g. the Himmel Edition) replace archived ones. wallpaper.dat only holds
    the desktop wallpaper extras, which the story never shows."""
    work = []
    for archive, kind in (("bg.dat", "bg"), ("chara.dat", "sprite")):
        path = os.path.join(game, archive)
        if not os.path.exists(path):
            continue
        loose = os.path.join(game, "graph", "bg" if kind == "bg" else "chara")
        with open(path, "rb") as f:
            for name, offset, length in kid.lnk_entries(path):
                override = os.path.join(loose, name)
                data = open(override, "rb").read() if os.path.exists(override) else kid.lnk_read(f, offset, length)
                work.append((kind, name, data, out))
    info, errors = {}, []
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as pool:
        for done, result in enumerate(pool.map(image_job, work, chunksize=8), 1):
            if isinstance(result, str):
                errors.append(result)
            else:
                info[result[0]] = (result[1], result[2])
            if done % 400 == 0:
                print(f"  images {done}/{len(work)}", flush=True)
    for line in errors[:10]:
        print("  image error:", line)
    return info


def voice_path(stem):
    return f"v/{stem[:-2]}/{stem}"


def convert_audio(game, out):
    """Music (bgm.dat), sound effects (se.dat) and voices (voice.dat) as WAV files holding
    the original MS-ADPCM data unchanged (only the header is rebuilt); 3DVN decodes it.
    For a smaller archive, 3DVN's make_pack.py --ogg converts them to Ogg Vorbis."""
    written = 0
    for archive, is_voice in (("bgm.dat", False), ("se.dat", False), ("voice.dat", True)):
        path = os.path.join(game, archive)
        if not os.path.exists(path):
            continue
        with open(path, "rb") as f:
            for name, offset, length in kid.lnk_entries(path):
                if not name.endswith(".waf"):
                    continue
                stem = name[:-4]
                target = f"{out}/audio/{voice_path(stem) if is_voice else stem}"
                if os.path.exists(target + ".ogg"):
                    os.remove(target + ".ogg")      # the engine would prefer an old .ogg
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target + ".wav", "wb") as handle:
                    handle.write(kid.waf_to_wav(kid.lnk_read(f, offset, length)))
                written += 1
                if written % 2000 == 0:
                    print(f"  audio {written}", flush=True)
    return written


# ---------------------------------------------------------------- scripts

def var(name_hex):
    """KID variable (hex byte) -> 3DVN variable; globals are g_xx (kept across games)."""
    number = int(name_hex, 16)
    return f"g_{number:02x}" if number in GLOBAL_VARS else f"v_{number:02x}"


def value(token):
    """Varop / condition operand -> 3DVN expression, or None if not representable."""
    if re.fullmatch(r"-?\d+", token):
        return token
    m = re.fullmatch(r"v_([0-9a-f]{2})", token)
    if m:
        return var(m.group(1))
    return None


def escape(text):
    return text.replace("\\", "\\\\").replace('"', '\\"').replace("{", "\\{")


def label(address):
    return f"L{address & 0xFFFFFFFF:08x}"


class SceneWriter:
    """Turns one decoded scene into 3DVN lines (CommandHandler semantics, real sprites)."""

    def __init__(self, name, scene, images, characters, stats, voices):
        self.voices = voices       # names in voice.dat (a few lines reference missing ones)
        self.name = name
        self.scene = scene
        self.images = images
        self.characters = characters
        self.stats = stats
        self.out = []
        self.sprites = {}          # slot id -> (image, dx)

    def emit(self, line):
        self.out.append(line)

    # graphics ------------------------------------------------------------
    def has_image(self, stem, kind):
        entry = self.images.get(stem.lower())
        return entry is not None and entry[0] == kind

    def hide_sprites(self):
        for slot in list(self.sprites):
            self.emit(f"hide s{slot}")
        self.sprites.clear()

    def background(self, stem):
        stem = stem.lower()
        self.hide_sprites()
        if stem in ("black", "white"):
            self.emit(f"bg {stem}")
        elif self.has_image(stem, "bg"):
            self.emit(f"bg {stem}")
        elif self.has_image(stem, "sprite"):         # full-screen picture stored as a sprite
            self.show(9, stem, 320)
        else:
            self.stats["missing bg"] = self.stats.get("missing bg", 0) + 1

    def show(self, slot, stem, dx):
        stem = stem.lower()
        if stem in ("black", "white", "red") or stem.startswith("bgcc"):
            # opaque "sprites" cover the screen; show them like a background
            if self.has_image(stem, "bg"):
                self.emit(f"bg {stem}")
            elif stem in ("black", "white"):
                self.emit(f"bg {stem}")
            return
        if not self.has_image(stem, "sprite"):
            self.stats["missing sprite"] = self.stats.get("missing sprite", 0) + 1
            return
        if dx == 320 and slot in self.sprites:        # 320 = keep the previous position
            dx = self.sprites[slot][1]
        _, (w, h, left, top) = self.images[stem]
        x = SRC_W / 640 * (dx - 320) + left
        y = max(top, SRC_H - h)
        self.emit(f"show s{slot} {stem} at {round(SCREEN_X + SCALE * x)} {round(SCALE * y)}")
        self.sprites[slot] = (stem, dx)

    def hide(self, slot):
        if slot in self.sprites:
            self.emit(f"hide s{slot}")
            del self.sprites[slot]

    # text ----------------------------------------------------------------
    def text_block(self, body):
        for line in body.split("\n"):
            self.text_line(line.strip())

    def text_line(self, line):
        if not line:
            return
        commands, clean = [], []
        index = 0
        while True:
            start = line.find("{", index)
            if start < 0:
                clean.append(line[index:])
                break
            clean.append(line[index:start])
            depth, end = 1, start + 1
            while end < len(line) and depth:
                depth += {"{": 1, "}": -1}.get(line[end], 0)
                end += 1
            commands.append(line[start + 1:end - 1])
            index = end
        voice = None
        for command in commands:
            parts = command.split()
            if not parts:
                continue
            if parts[0] == "sound" and len(parts) > 1:
                voice = parts[1].lower()
            elif parts[0] == "choice":
                self.choice(command)
        text = "".join(clean).strip()
        if not text:
            return
        if voice in self.voices:
            self.emit(f"sfx {voice_path(voice)}")
        elif voice:
            self.stats["missing voice"] = self.stats.get("missing voice", 0) + 1
        m = re.match(r"^\[([^\]]+)\]\s*(.*)$", text, re.S)
        if m:
            speaker = self.character(m.group(1).strip())
            self.emit(f'{speaker}: "{escape(m.group(2).strip())}"')
        else:
            self.emit(f'"{escape(text)}"')

    def character(self, display):
        ident = self.characters.get(display)
        if ident is None:
            base = re.sub(r"[^a-z0-9]+", "_", display.lower()).strip("_") or "unknown"
            ident = base if base[0].isalpha() else "c_" + base
            while ident in self.characters.values():
                ident += "_"
            self.characters[display] = ident
        return ident

    def choice(self, command):
        # "choice 00 0000 |opt|{cond (..) 1f 14 (..)}opt" -> menu; the picked option's
        # original index goes into v_b3 (the following switch branches on it).
        rest = command.split("|", 1)[1] if "|" in command else ""
        self.emit("menu")
        for index, option in enumerate(rest.split("|")):
            option = option.strip()
            condition = ""
            m = re.match(r"\{cond \([^)]*\) ([0-9a-f]{2}) [0-9a-f]{2} \([^)]*\)\}(.*)", option)
            if m:
                condition = f" if {var(m.group(1))} != 0"
                option = m.group(2).strip()
            self.emit(f'  choice "{escape(option)}"{condition}')
            self.emit(f"    set v_b3 = {index}")
        self.emit("end")

    # logic -----------------------------------------------------------------
    def gotoif(self, text):
        m = re.match(r"gotoif \d+ \(28 0a a4\) ([0-9a-f]{2}) (==|!=|<=|>=|< |> |[0-9a-f]{4}) \([^)]*\) (\S+) "
                     r"\([^)]*\) -> ([0-9a-f]{8})", text)
        if not re.match(r"gotoif \d+ \(28 0a a4\)", text):
            # not a story variable (engine state / fragments; these never jump anywhere valid)
            self.stats["engine condition skipped"] = self.stats.get("engine condition skipped", 0) + 1
            return
        if not m or len(m.group(2).strip()) > 2:
            self.emit(f"# {text}")
            self.stats["unhandled condition"] = self.stats.get("unhandled condition", 0) + 1
            return
        target = int(m.group(4), 16)
        if target == 0xFFFFFFFF:
            return
        self.emit(f"if {var(m.group(1))} {m.group(2).strip()} {m.group(3)}")
        self.emit(f"  jump {label(target)}")
        self.emit("end")

    def varop(self, text):
        if not text.startswith("varop (28 0a a4)"):
            # banks a0/a2/a3: engine settings such as the next transition effect
            self.stats["engine setting skipped"] = self.stats.get("engine setting skipped", 0) + 1
            return
        m = re.match(r"varop \([^)]*\) ([0-9a-f]{2}) (:=|\+=|[0-9a-f]{4}) \([^)]*\) (\S+)", text)
        operand = value(m.group(3)) if m else None
        if not m or operand is None or m.group(2) not in (":=", "+="):
            self.emit(f"# {text}")
            self.stats["unhandled varop"] = self.stats.get("unhandled varop", 0) + 1
            return
        self.emit(f"set {var(m.group(1))} {'=' if m.group(2) == ':=' else '+='} {operand}")

    def switch(self, text):
        variable = None
        for line in text.split("\n")[1:]:
            line = line.strip()
            m = re.match(r"switch_varop \([^)]*\) ([0-9a-f]{2})", line)
            if m:
                variable = var(m.group(1))
                continue
            m = re.match(r"27 -> (-?\d+) ([0-9a-f]{8})", line)
            if m and variable and int(m.group(2), 16) != 0xFFFFFFFF:
                self.emit(f"if {variable} == {m.group(1)}")
                self.emit(f"  jump {label(int(m.group(2), 16))}")
                self.emit("end")

    # main ------------------------------------------------------------------
    def convert(self):
        targets = set(t for t in self.scene.jump_table if t >= 0)
        for addr, code, text in self.scene.decode():
            if addr in targets:
                self.emit(f"label {label(addr)}")
            if not text:
                continue
            first = text.split("\n", 1)[0].split()
            op = first[0] if first else ""
            args = first[1:]
            try:
                self.op(op, args, text)
            except (ValueError, IndexError):
                self.emit(f"# {text.splitlines()[0]}")
                self.stats["unparsed op"] = self.stats.get("unparsed op", 0) + 1
        return self.out

    def op(self, op, a, text):
        if op == "text":
            self.text_block(text.split("\n", 1)[1] if "\n" in text else "")
        elif op == "bgload":
            self.background(a[1])
        elif op in ("overlayImage", "specialEffectImage"):
            self.background(a[1])
        elif op == "removeBG":
            self.hide_sprites()
            self.emit("bg white" if a[0] == "1" else "bg black")
        elif op == "fgload":
            self.show(int(a[0]), a[2], int(a[3]))
        elif op == "multifgload2":
            self.show(int(a[0]), a[3], int(a[6]))
            self.show(int(a[1]), a[5], int(a[7]))
        elif op == "multifgload3":
            self.show(1, a[1], int(a[6]))
            self.show(2, a[3], int(a[7]))
            self.show(4, a[5], int(a[8]))
        elif op == "removeFG":
            self.hide(int(a[0]))
        elif op in ("multiremoveFG2", "multiremoveFG3"):
            self.hide_sprites()
        elif op == "chapterCutin":
            stem = a[1].lower()
            if self.has_image(stem, "bg"):
                self.stats.setdefault("cutins", set()).add(stem)   # copied to sprites/ later
                self.emit(f"show cutin {stem} at {SCREEN_X} 0 fade 0.5")
                self.emit("wait 1.5")
                self.emit("hide cutin fade 0.5")
        elif op == "playBGM":
            self.emit(f"music bgm{int(a[0]):02d}")
        elif op == "stopBGM":
            self.emit("music stop")
        elif op == "playSFX":
            name = a[0].lower()
            # names ending in "l" are the game's looping sounds (ambience, alarms); they
            # run until stopSFX, as in Ever17Converter
            self.emit(f"sfx {name} loop" if name.endswith("l") else f"sfx {name}")
        elif op == "stopSFX":
            self.emit("sfx stop")
        elif op == "waitForSFX":
            self.emit("wait 0.5")
        elif op == "goto":
            self.emit(f"jump {label(int(a[0], 16))}")
        elif op == "delay":
            frames = int(a[0]) if a and a[0].isdigit() else 30
            self.emit(f"wait {max(0.1, min(2.0, frames / 60)):g}")
        elif op == "clock":
            self.emit(f'"[ {a[0]} ]"')
        elif op == "specialEffect":
            if a and a[0] == "3":                      # change of perspective
                self.emit('"* * *"')
        elif op == "gotoif":
            self.gotoif(text.strip())
        elif op == "varop":
            self.varop(text.strip())
        elif op == "_switch":
            self.switch(text)
        elif op == "jump":
            self.emit(f'script "ch/{a[0].lower()}"')
        # end, hide/showTextbox, choiceId, scriptLocationId, unknowns: nothing to do


# The endings checklist (3DVN's endings.txt): the global flags each ending scene sets.
ENDINGS = """# Ever17 endings: name = global flag(s) set when the ending is seen
Tsugumi - good ending = g_09 g_0f
Sora - good ending = g_0a g_10
Takeshi - bad ending = g_12
You - good ending = g_14
You - bad ending = g_15
Sara - good ending = g_17
Sara - bad ending = g_18
Coco - true ending = g_0c g_19
"""

ENDSELECT = """# Replaces y_ed (ending select), after Ever17Converter's endselect.scr:
# route points -> ending; Coco's route is unlocked once all four good endings are seen.
# Each ending scene jumps back here when it is over: the second visit ends the story.
if v_yedseen != 0
  set v_yedseen = 0
  "This story hasn't ended yet."
  "Because only you are in the infinity loop."
  stop
end
set v_yedseen = 1
set v_bf = g_09 + g_10 + g_14 + g_17
if v_bf >= 4
  set g_1a = 1
end
if v_c7 == 0 or v_c7 == 1
  script "ch/t_ep"
end
if v_c7 == 2
  script "ch/syep"
end
if v_c7 == 3
  script "ch/ssep"
end
"This story hasn't ended yet."
"Because only you are in the infinity loop."
stop
"""


GAME_CONFIG = """# Ever17 -the out of infinity-, converted by e17_convert.py
title Ever17 -the out of infinity-
menu_title Ever17
accent #4FA8E0
panel #0B1824
text #EEF4F8
textbox_img textbox
textbox_x 8
textbox_y 150
textbox_w 384
textbox_h 90
textbox_alpha 190
default_display adv
"""


def convert_scripts(game, out, images):
    path = os.path.join(game, "script.dat")
    characters, stats, scenes = {}, {}, []
    voice_dat = os.path.join(game, "voice.dat")
    voices = {n[:-4].lower() for n, _, _ in kid.lnk_entries(voice_dat)} if os.path.exists(voice_dat) else set()
    os.makedirs(os.path.join(out, "ch"), exist_ok=True)
    with open(path, "rb") as f:
        for name, offset, length in kid.lnk_entries(path):
            stem = name[:-4]
            if stem in ("startup", "system"):
                continue
            if stem == "y_ed":
                lines = ["include characters.3vs"] + ENDSELECT.strip().split("\n")
            else:
                scene = e17.Scene(kid.lnk_read(f, offset, length))
                lines = ["include characters.3vs"] + SceneWriter(stem, scene, images, characters, stats, voices).convert()
            with open(os.path.join(out, "ch", stem + ".3vs"), "w", encoding="utf-8") as handle:
                handle.write("\n".join(lines) + "\n")
            scenes.append(stem)
    # chapter cut-ins (calendar cards) are backgrounds shown over the scene as a sprite
    for stem in sorted(stats.pop("cutins", ())):
        for folder in ("", "hd/"):
            for ext in (".jpg", ".png"):
                source = os.path.join(out, f"{folder}bg/{stem}{ext}")
                if os.path.exists(source):
                    os.makedirs(os.path.join(out, f"{folder}sprites"), exist_ok=True)
                    shutil.copyfile(source, os.path.join(out, f"{folder}sprites/{stem}{ext}"))
    with open(os.path.join(out, "characters.3vs"), "w", encoding="utf-8") as handle:
        for display, ident in sorted(characters.items(), key=lambda item: item[1]):
            handle.write(f'character {ident} "{escape(display)}"\n')
    with open(os.path.join(out, "main.3vs"), "w", encoding="utf-8") as handle:
        handle.write("# Ever17 -the out of infinity- (English PC release), converted for 3DVN.\n"
                     "# Endings seen and the final route unlock are g_ variables (kept across games).\n"
                     'script "ch/op00"\n')
    title = os.path.join(out, "bg", "title1.jpg")
    if os.path.exists(title):
        os.makedirs(os.path.join(out, "ui"), exist_ok=True)
        from PIL import Image, ImageEnhance   # darkened, so the menu reads over the logo
        ImageEnhance.Brightness(Image.open(title).convert("RGB")).enhance(0.45).save(
            os.path.join(out, "ui", "menu_bg.jpg"), quality=90)
    # A text box image, so the theme colours (blue names and border) apply to it.
    from PIL import Image
    os.makedirs(os.path.join(out, "ui"), exist_ok=True)
    box = Image.new("RGBA", (384, 90), (11, 24, 36, 200))
    box.paste((79, 168, 224, 255), (0, 0, 384, 2))
    box.save(os.path.join(out, "ui", "textbox.png"))
    with open(os.path.join(out, "game.3vc"), "w", encoding="utf-8") as handle:
        handle.write(GAME_CONFIG + ("menu_bg menu_bg\n" if os.path.exists(title) else ""))
    with open(os.path.join(out, "endings.txt"), "w", encoding="utf-8") as handle:
        handle.write(ENDINGS)
    return scenes, characters, stats


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("game")
    parser.add_argument("out")
    parser.add_argument("--no-audio", action="store_true")
    parser.add_argument("--no-images", action="store_true")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    index = os.path.join(args.out, "images.json")   # sizes/offsets, for script-only reruns
    if args.no_images:
        if not os.path.exists(index):
            sys.exit("--no-images needs a previous full run (images.json)")
        with open(index) as handle:
            images = {k: (v[0], tuple(v[1])) for k, v in json.load(handle).items()}
        print(f"images: skipped, {len(images)} known from images.json")
    else:
        print("converting images ...", flush=True)
        images = convert_images(args.game, args.out, args.jobs)
        with open(index, "w") as handle:
            json.dump(images, handle)
        print(f"  {len(images)} images")
    if not args.no_audio:
        print("converting audio ...", flush=True)
        print(f"  {convert_audio(args.game, args.out)} sounds written (original ADPCM)")
    print("converting scripts ...", flush=True)
    scenes, characters, stats = convert_scripts(args.game, args.out, images)
    print(f"  {len(scenes)} scenes, {len(characters)} speakers")
    for key, count in sorted(stats.items()):
        print(f"  {key}: {count}")


if __name__ == "__main__":
    main()
