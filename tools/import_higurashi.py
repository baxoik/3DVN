#!/usr/bin/env python3
"""Convert a Higurashi When They Cry Hou chapter (MangaGamer, Steam) into a 3DVN game.

usage: import_higurashi.py GAME_FOLDER OUT_GAME_DIR [options]   (see --help)

GAME_FOLDER is the installed chapter (the folder with HigurashiEpNN_Data), e.g.
  ~/.local/share/Steam/steamapps/common/Higurashi When They Cry
The story is read from StreamingAssets/Scripts (Update/ overrides, as the game does):
English text, backgrounds, character sprites, music and sound effects, the chapter order
from flow.txt, and the TIPS with the day each one unlocks (from the game's own data).
07th-Mod voice files (StreamingAssets/voice, PlayVoice in the scripts) are used if present.
Only convert games you own.

Output: main.3vs, ch/*.3vs (one per day / TIPS, plus ch/flow.3vs that runs them in order
and offers each day's new TIPS), bg/, sprites/ (3DS size), hd/ (originals for PC, Wii U and
Android), audio/ (Ogg, copied unchanged), game.3vc (NVL display, text kept on screen).
"""
import argparse, concurrent.futures, glob, os, re, shutil, struct, sys
from collections import Counter

try:
    from PIL import Image
except ImportError:
    Image = None

SCREEN_W, SCREEN_H = 400, 240
CANVAS_H = 480                          # the game's coordinate space is 480 high: 640 wide
                                        # (4:3), or 853 with 07th-Mod's 16:9 screen
# 07th-Mod art and audio sets: folders searched in order (as in its init.txt)
ART_SETS = {"console": ["CG"], "remake": ["CGAlt", "CG"],
            "original": ["OGBackgrounds", "OGSprites", "CG"]}
AUDIO_SETS = {"mangagamer": (["April2019BGM", "BGM"], ["SE"]),
              "gin": (["OGBGM", "BGM"], ["OGSE", "SE"]),
              "houplus": (["HouPlusBGM", "BGM"], ["OGSE", "SE"])}
PAGE_CHARS = 420                        # start a new NVL page before it overflows a 3DS screen
ARCS = {"onik": "Onikakushi", "wata": "Watanagashi", "tata": "Tatarigoroshi",
        "hima": "Himatsubushi", "meak": "Meakashi", "tsum": "Tsumihoroboshi",
        "mina": "Minagoroshi", "mats": "Matsuribayashi"}

report = Counter()
notes = []


def warn(key, msg=None):
    report[key] += 1
    if msg and report[key] <= 3:
        notes.append(f"{key}: {msg}")


def quote(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("{", "\\{").replace("\n", "\\n")
    return '"' + s + '"'


def ident(s):
    s = re.sub(r"[^A-Za-z0-9_]", "_", s)
    return s if s and not s[0].isdigit() else "v_" + s


# ---- the game's script language: C-like calls inside void functions ----
TOKEN = re.compile(r'\s+|//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|-?\d+|[A-Za-z_][A-Za-z0-9_]*|.', re.S)


def tokens(text):
    for m in TOKEN.finditer(text):
        t = m.group(0)
        if t[0].isspace() or t.startswith("//") or t.startswith("/*"):
            continue
        yield t


def unstring(t):
    body = t[1:-1]
    return re.sub(r'\\(.)', lambda m: "\n" if m.group(1) == "n" else m.group(1), body)


def parse_functions(text):
    """{function name: [statement]}; a statement is ("call", name, [args]) or ("if", ...)
    (ifs only appear in flow.txt, whose logic is read separately)."""
    toks = list(tokens(text.lstrip("\ufeff")))
    funcs, i = {}, 0
    while i < len(toks):
        if toks[i] == "void" and i + 3 < len(toks) and toks[i + 2] == "(":
            name = toks[i + 1]
            j = toks.index("{", i)
            depth, k = 1, j + 1
            while k < len(toks) and depth:
                depth += toks[k] == "{"
                depth -= toks[k] == "}"
                k += 1
            funcs[name] = statements(toks[j + 1:k - 1])
            i = k
        else:
            i += 1
    return funcs


def branch(toks, j):
    """the statement or { block } at j -> (statements, index after it)"""
    if toks[j] == "{":
        d, k = 1, j + 1
        while d:
            d += toks[k] == "{"
            d -= toks[k] == "}"
            k += 1
        return statements(toks[j + 1:k - 1]), k
    if toks[j] == "if":   # "else if": exactly one if statement
        k = _end_of_if(toks, j)
        return statements(toks[j:k]), k
    k = j
    depth = 0
    while k < len(toks) and not (toks[k] == ";" and depth == 0):
        depth += toks[k] == "("
        depth -= toks[k] == ")"
        k += 1
    return statements(toks[j:k + 1]), k + 1


def _end_of_if(toks, j):
    depth, k = 0, j + 1
    while True:
        depth += toks[k] == "("
        depth -= toks[k] == ")"
        k += 1
        if depth == 0:
            break
    _, k = branch(toks, k)
    if k < len(toks) and toks[k] == "else":
        _, k = branch(toks, k + 1)
    return k


def statements(toks):
    out, i = [], 0
    while i < len(toks):
        t = toks[i]
        if t == "if":
            depth, j = 0, i + 1     # the condition "( ... )"
            while True:
                depth += toks[j] == "("
                depth -= toks[j] == ")"
                j += 1
                if depth == 0:
                    break
            cond = toks[i + 2:j - 1]
            then, j = branch(toks, j)
            other = []
            if j < len(toks) and toks[j] == "else":
                other, j = branch(toks, j + 1)
            out.append(("if", cond, then, other))
            i = j
        elif re.fullmatch(r"[A-Za-z_]\w*", t) and i + 1 < len(toks) and toks[i + 1] == "(":
            depth, j, args, cur = 0, i + 1, [], []
            while True:
                tok = toks[j]
                if tok == "(":
                    depth += 1
                    if depth > 1:
                        cur.append(tok)
                elif tok == ")":
                    depth -= 1
                    if depth == 0:
                        break
                    cur.append(tok)
                elif tok == "," and depth == 1:
                    args.append(cur)
                    cur = []
                else:
                    cur.append(tok)
                j += 1
            if cur:
                args.append(cur)
            values = []
            for a in args:
                if len(a) == 1 and a[0].startswith('"'):
                    values.append(unstring(a[0]))
                elif len(a) == 1 and re.fullmatch(r"-?\d+", a[0]):
                    values.append(int(a[0]))
                elif len(a) == 2 and a[0] == "-" and a[1].isdigit():
                    values.append(-int(a[1]))
                else:
                    values.append(" ".join(a))
            out.append(("call", t, values))
            i = j + 1
            if i < len(toks) and toks[i] == ";":
                i += 1
        else:
            i += 1
    return out


# ---- TIPS: which day each unlocks, from the game's .NET code ----
def read_tips(dll_path, prefix):
    """[(script, unlock chapter number, English title)] from TipsData in Assembly-CSharp.dll:
    each entry stores its script name (ldstr), the unlock number (ldc.i4) and two titles."""
    data = open(dll_path, "rb").read()
    m = data.find(b"BSJB")
    ver = struct.unpack_from("<I", data, m + 12)[0]
    p = m + 16 + ver + 2
    count = struct.unpack_from("<H", data, p)[0]
    p += 2
    streams = {}
    for _ in range(count):
        off, size = struct.unpack_from("<II", data, p)
        p += 8
        e = data.index(b"\0", p)
        streams[data[p:e].decode()] = (m + off, size)
        p = (e + 4) & ~3
    us_off, us_size = streams["#US"]
    us = data[us_off:us_off + us_size]

    def entry(i):
        b = us[i]
        if b & 0x80 == 0:
            return b, 1
        if b & 0xC0 == 0x80:
            return ((b & 0x3F) << 8) | us[i + 1], 2
        return ((b & 0x1F) << 24) | (us[i + 1] << 16) | (us[i + 2] << 8) | us[i + 3], 4

    def string(token):
        ln, h = entry(token & 0xFFFFFF)
        i = token & 0xFFFFFF
        return us[i + h:i + h + ln - 1].decode("utf-16le", "replace")

    def ldc(at):   # decode one ldc.i4 form -> (value, length) or None
        op = data[at]
        if 0x16 <= op <= 0x1E:
            return op - 0x16, 1
        if op == 0x1F:
            return struct.unpack_from("<b", data, at + 1)[0], 2
        if op == 0x20:
            return struct.unpack_from("<i", data, at + 1)[0], 5
        return None

    tips, i = [], 1
    while i < len(us):
        ln, h = entry(i)
        s = us[i + h:i + h + ln - 1].decode("utf-16le", "replace") if ln else ""
        if re.fullmatch(re.escape(prefix) + r"_tips_\d+", s):
            at = data.find(b"\x72" + struct.pack("<I", 0x70000000 | i))
            # ldstr script; stfld; ldloc; ldc.i4 unlock; stfld; ldloc; ldstr title; ...; ldstr title
            k = at + 5 + 5 + 1
            got = ldc(k)
            titles = []
            if got:
                k += got[1] + 5
                for _ in range(2):
                    while k < len(data) - 5 and data[k] != 0x72:
                        k += 1
                    titles.append(string(struct.unpack_from("<I", data, k + 1)[0]))
                    k += 5
            english = next((t for t in titles if t and all(ord(c) < 0x3000 for c in t)), s)
            if got:
                tips.append((s, got[0], english))
            else:
                warn("TIPS unlock day not found (shown after the story)", s)
                tips.append((s, 10 ** 6, english))
        i += h + ln
    return sorted(tips)


class Converter:
    def __init__(self, a):
        self.a = a
        self.out = a.out
        data = sorted(glob.glob(os.path.join(a.src, "*_Data")))
        if not data or not os.path.isdir(os.path.join(data[0], "StreamingAssets")):
            sys.exit(f"no *_Data/StreamingAssets folder in {a.src}")
        self.data = data[0]
        self.sa = os.path.join(self.data, "StreamingAssets")
        art = "remake" if a.alt_art else a.art
        self.art = [os.path.join(self.sa, f) for f in ART_SETS[art]]
        bgm, se = AUDIO_SETS[a.audio]
        self.bgm = [os.path.join(self.sa, f) for f in bgm]
        self.se = [os.path.join(self.sa, f) for f in se]
        self.voice = [os.path.join(self.sa, "voice"), os.path.join(self.sa, "SE")]
        self.used = {"bg": {}, "sprites": {}, "audio": set()}
        self.indexes = {}
        self.sources = {}     # converted audio name -> source file
        self.parsed = {}
        self.chars = {}       # character id -> (name, colour)
        # settings the scripts test (07th-Mod), decided at conversion time
        self.flags = {"GADVMode": 1 if a.adv else 0, "GCensor": a.censor, "GHideCG": 0,
                      "GLipSync": 0, "GArtStyle": 0}
        init = "".join(open(p, encoding="utf-8-sig", errors="replace").read()
                       for p in (self.script_path("init"),) if p)
        wide = re.search(r'SetScreenAspect\(\s*"0\.5625"', init) is not None
        self.canvas_w = CANVAS_H * 16 / 9 if wide else 640
        self.scale = min(SCREEN_W / self.canvas_w, SCREEN_H / CANVAS_H)
        self.xoff = (SCREEN_W - self.canvas_w * self.scale) / 2
        self.yoff = (SCREEN_H - CANVAS_H * self.scale) / 2

    # ---- files ----
    def script_path(self, name):
        for folder in ("Update", "Scripts"):
            path = os.path.join(self.sa, folder, name + ".txt")
            if os.path.isfile(path):
                return path
        return None

    def index(self, folder):
        """lower-case file name -> real path (the game was made for Windows, where scripts
        say "Furiker_a" for furiker_a.png)"""
        if folder not in self.indexes:
            found = {}
            if os.path.isdir(folder):
                for f in os.listdir(folder):
                    found[f.lower()] = os.path.join(folder, f)
            self.indexes[folder] = found
        return self.indexes[folder]

    def find(self, folders, name, ext):
        """name (may have subfolders, any case) in the first folder that has it ->
        (real path, real name relative to that folder without extension), or (None, None)"""
        parts = name.replace("\\", "/").split("/")
        for folder in folders:
            current, real = folder, []
            for i, part in enumerate(parts):
                found = self.index(current).get(part.lower() + (ext if i == len(parts) - 1 else ""))
                if not found:
                    break
                real.append(os.path.basename(found))
                current = found
            else:
                return current, "/".join(real[:-1] + [os.path.splitext(real[-1])[0]])
        return None, None

    def picture(self, name):
        return self.find(self.art, name, ".png")

    @staticmethod
    def factor(w, h):
        """pixels per game unit (the game is 480 units high): the art comes as 640x480
        originals, 1280x960 remakes, 1920x1080 PS3 backgrounds and 2560x1440 PS3 sprites"""
        if h == 1440:
            return 3.0
        if h in (1080, 2160) or w in (1920, 2880):
            return 1080 / CANVAS_H
        if (w, h) == (1280, 960):
            return 2.0
        return 1.0

    def logical_size(self, path):
        with Image.open(path) as image:
            w, h = image.size
        f = self.factor(w, h)
        return w / f, h / f

    def sound(self, folders, name, prefix):
        path, real = self.find(folders, name, ".ogg")
        if not path:
            return None
        converted = prefix + "/" + real
        self.sources[converted] = path
        return converted

    def condition(self, cond, where):
        """a script condition from the settings: GetGlobalFlag(X) [op N] [&& ...]"""
        text = " ".join(cond)
        text = re.sub(r"GetGlobalFlag\s*\(\s*(\w+)\s*\)",
                      lambda m: str(self.flags.get(m.group(1), 0)), text)
        text = re.sub(r"([<>=!])\s+=", r"\1=", text)
        text = text.replace("&&", " and ").replace("||", " or ").replace("TRUE", "1").replace("FALSE", "0")
        text = re.sub(r"!(?!=)", " not ", text)
        if not re.fullmatch(r"[\d\s()<>=!andortn]*", text):
            warn("condition not understood (taken as false)", f"{where}: {' '.join(cond)}")
            return False
        try:
            return bool(eval(text, {"__builtins__": {}}))
        except Exception:
            warn("condition not understood (taken as false)", f"{where}: {' '.join(cond)}")
            return False

    def functions(self, name):
        if name not in self.parsed:
            path = self.script_path(name)
            self.parsed[name] = parse_functions(open(path, encoding="utf-8-sig", errors="replace").read()) if path else {}
        return self.parsed[name]

    def run_block(self, stmts, out, where, depth=0):
        for st in stmts:
            if st[0] == "if":
                self.run_block(st[2] if self.condition(st[1], where) else st[3], out, where, depth)
            elif st[1] == "ModCallScriptSection" and depth < 8:
                file, section = st[2][0], st[2][1]
                body = self.functions(file).get(section)
                if body is None:
                    warn("missing script section", f"{file} {section}")
                else:
                    self.run_block(body, out, f"{file}:{section}", depth + 1)
            else:
                self.statement(st[1], st[2], out, where)

    # ---- one script file -> one chapter ----
    def convert(self, name):
        out = [f"# converted from {name}.txt", "include characters.3vs"]
        self.text, self.page, self.speaker = "", 0, None
        self.music_channel = None
        self.run_block(self.functions(name).get("main", []), out, name)
        self.flush(out)
        out.append("script ch/flow")   # back to the order of play
        return out

    def flush(self, out, wait=True):
        text = re.sub(r"<[^>]*>", "", self.text)
        if not text.strip():
            return   # only line breaks so far: keep them for the next text
        self.text = ""
        if self.a.adv:   # one line at a time, with the speaker's name
            text = text.strip("\n")
            speaker, self.speaker = self.speaker, None
            out.append(f"{speaker}: {quote(text)}" if speaker else quote(text))
            return
        if self.page and self.page + len(text) > PAGE_CHARS:
            out.append("page")
            self.page = 0
            text = text.lstrip("\n")
        self.page += len(text)
        out.append(quote(text))

    def character(self, name):
        """an ADV speaker "<color=#5ec69a>Mion</color>" -> its 3DVN character id"""
        colour = re.search(r"#([0-9a-fA-F]{6})", name)
        plain = re.sub(r"<[^>]*>", "", name).strip()
        cid = "c_" + ident(plain.lower())[:28]
        self.chars.setdefault(cid, (plain, "#" + colour.group(1).lower() if colour else ""))
        return cid

    def statement(self, cmd, args, out, where):
        a = self.a
        ms = lambda v: f"{max(0, v) / 1000:.2f}" if isinstance(v, int) else "0"
        if cmd == "OutputLine":
            # (Japanese name, Japanese text, English name, English text, mode)
            name, text, mode = args[2], args[3], args[4] if len(args) > 4 else ""
            text = text if isinstance(text, str) and text != "NULL" else ""
            if isinstance(name, str) and name not in ("NULL", ""):
                if not text:   # 07th-Mod ADV: the name first, then the line
                    self.speaker = self.character(name)
                    return
                text = re.sub(r"<[^>]*>", "", name) + ": " + text
            self.text += text
            if mode != "Line_ContinueAfterTyping":
                self.flush(out)
        elif cmd == "OutputLineAll":
            text, mode = args[1], args[2] if len(args) > 2 else ""
            self.text += text if isinstance(text, str) and text != "NULL" else ""
            if mode != "Line_ContinueAfterTyping":
                self.flush(out)
        elif cmd == "ClearMessage":
            self.flush(out)
            self.text = ""   # line breaks left at the end of a page are not needed
            if self.page and not self.a.adv:
                out.append("page")
                self.page = 0
        elif cmd == "FadeBG":   # fade the background out
            self.flush(out)
            out.append(f"bg black fade {ms(args[0])}" if isinstance(args[0], int) and args[0] > 0 else "bg black")
        elif cmd in ("DrawScene", "DrawSceneWithMask", "DrawBG", "ChangeScene"):
            self.flush(out)
            pic = args[0]
            path, real = self.picture(pic)
            seconds = (args[-1] if cmd == "DrawSceneWithMask" else args[2] if cmd == "ChangeScene"
                       else args[1] if len(args) > 1 else 0)
            fade = f" fade {ms(seconds)}" if isinstance(seconds, int) and seconds > 0 else ""
            if cmd != "DrawBG":
                out.append("hide *")       # a new scene clears the characters
            if path:
                self.used["bg"][real] = path
                out.append(f"bg {real}{fade}")
            elif pic in ("black", "white"):
                out.append(f"bg {pic}{fade}")
            else:
                warn("missing picture", f"{where}: {pic}")
        elif cmd in ("DrawBustshot", "DrawBustshotWithFiltering", "ModDrawCharacter",
                     "ModDrawCharacterWithFiltering"):
            self.flush(out)
            if cmd.startswith("Mod"):
                # (layer, character, "sprite/name_", "mouth frame", [mask, style,] x, y, ...)
                layer, pic = args[0], f"{args[2]}{args[3]}"
                x, y = (args[6], args[7]) if cmd.endswith("Filtering") else (args[4], args[5])
                path, real = self.picture(pic)
                if not path:
                    path, real = self.picture(f"{args[2]}0")
            else:
                layer, pic = args[0], args[1]
                if pic in ("", "NULL"):   # an empty picture clears the layer
                    out.append(f"hide L{layer}")
                    return
                x, y = (args[4], args[5]) if cmd.endswith("Filtering") else (args[2], args[3])
                path, real = self.picture(pic)
            seconds = args[-2] if len(args) >= 2 else 0
            if not path:
                warn("missing picture", f"{where}: {pic}")
                return
            if not isinstance(x, int) or not isinstance(y, int):
                x, y = 0, 0
            self.used["sprites"][real] = path
            w, h = self.logical_size(path)
            left = self.xoff + (self.canvas_w / 2 + x - w / 2) * self.scale
            top = self.yoff + (CANVAS_H / 2 + y - h / 2) * self.scale
            pic = real
            fade = f" fade {ms(seconds)}" if isinstance(seconds, int) and seconds > 0 else ""
            out.append(f"show L{layer} {pic} at {round(left)} {round(top)}{fade}")
        elif cmd in ("FadeBustshot", "FadeBustshotWithFiltering"):
            self.flush(out)
            seconds = args[-2] if len(args) >= 2 else 0
            fade = f" fade {ms(seconds)}" if isinstance(seconds, int) and seconds > 0 else ""
            out.append(f"hide L{args[0]}{fade}")
        elif cmd == "FadeAllBustshots":
            self.flush(out)
            seconds = args[0] if args else 0
            fade = f" fade {ms(seconds)}" if isinstance(seconds, int) and seconds > 0 else ""
            out.append(f"hide *{fade}")
        elif cmd == "Wait":
            self.flush(out)
            if isinstance(args[0], int) and args[0] > 0:
                out.append(f"wait {ms(args[0])}")
        elif cmd == "PlayBGM":
            name = self.sound(self.bgm, args[1], "bgm")
            if name:
                self.used["audio"].add(name)
                self.music_channel = args[0]
                out.append(f"music {name}")
            else:
                warn("missing music", f"{where}: {args[1]}")
        elif cmd in ("FadeOutBGM", "StopBGM"):
            if self.music_channel is not None and args and args[0] == self.music_channel:
                out.append("music stop")
                self.music_channel = None
        elif cmd == "PlaySE":
            name = self.sound(self.se, args[1], "se")
            if name:
                self.used["audio"].add(name)
                out.append(f"sfx {name}")
            else:
                warn("missing sound", f"{where}: {args[1]}")
        elif cmd == "ModPlayVoiceLS" and args[2] in ("", "NULL"):
            pass
        elif cmd == "ModPlayVoiceLS":   # 07th-Mod: (channel, character, "ps3/s19/..", volume, lip sync)
            name = self.sound(self.voice, args[2], "voice")
            if name:
                self.used["audio"].add(name)
                out.append(f"sfx {name}")
            else:
                warn("missing voice", f"{where}: {args[2]}")
        elif cmd == "StopSE":
            out.append("sfx stop")
        elif cmd == "PlayVoice":
            name = self.sound(self.voice, args[1], "voice")
            if name:
                self.used["audio"].add(name)
                out.append(f"sfx {name}")
        elif cmd in ("ShakeScreen", "ShakeScreenSx", "StartShakingOfAllObjects"):
            self.flush(out)
            out.append("shake 0.40 6")
        elif cmd in ("DisableWindow", "SetValidityOfInput", "SetSpeedOfMessage", "GetAchievement",
                     "SetDrawingPointOfMessage", "EnableJumpingOfReturnIcon", "SetStyleOfMessageSwinging",
                     "SetValidityOfSaving", "SetValidityOfWindowDisablingWhenGraphicsControl",
                     "SetValidityOfTextFade", "SetValidityOfInterface", "SavePoint", "SetFontSize",
                     "SetFontId", "SetCharSpacing", "SetLineSpacing", "SetScreenAspect", "SetWindowPos",
                     "SetWindowSize", "SetWindowMargins", "SetNameFormat", "SetGUIPosition",
                     "SetGlobalFlag", "SetLocalFlag", "ModSetMainFontOutlineWidth", "ModSetConfigFontSize",
                     "ModGenericCall", "ModNVLModeSettingLoad", "ModADVModeSettingLoad",
                     "ModNVLADVModeSettingLoad", "WaitToFinishSEPlaying", "WaitToFinishVoicePlaying",
                     "TerminateShakingOfAllObjects", "TerminateShakingOfWindow"):
            pass   # presentation details 3DVN handles itself
        elif cmd in ("DrawFilm", "FadeFilm", "Negative", "ModSetLayerFilter"):
            warn("colour filter (tint / negative) not converted")
        elif cmd == "ModPlayMovie":
            warn("movie not converted", f"{where}: {args[0] if args else ''}")
        elif cmd in ("DrawSprite", "MoveSprite", "FadeSprite", "MoveBustshot",
                     "DrawSpriteWithFiltering"):
            warn(f"effect '{cmd}' not converted")
        else:
            warn(f"command '{cmd}' skipped", where)

    # ---- order of play, from flow.txt ----
    def flow(self, prefix):
        funcs = parse_functions(open(self.script_path("flow"), encoding="utf-8-sig", errors="replace").read())
        steps, seen, state = [], set(), {"chapter": 0}
        helpers = {"SetDay", "ViewTips", "ViewTipsDisplay", "BrandLogo", "ChapterPreview", "Title"}

        def walk(section):
            if section in seen or section not in funcs:
                return
            seen.add(section)
            for st in funcs[section]:
                if st[0] != "call":
                    continue
                cmd, args = st[1], st[2]
                if cmd == "CallScript":
                    steps.append(("script", args[0]))
                elif cmd == "StoreValueToLocalWork" and args and args[0] == "ChapterNumber":
                    state["chapter"] = args[1]
                elif cmd == "CallSection" and args[0] == "ViewTips":
                    steps.append(("tips", state["chapter"]))
                elif cmd in ("CallSection", "JumpSection") and args[0] not in helpers:
                    walk(args[0])
                    if cmd == "JumpSection":
                        return

        walk("Game")
        return steps

    def run(self):
        if Image is None:
            sys.exit("Pillow is required: pip install pillow")
        names = [os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(self.sa, "Scripts", "*.txt"))]
        prefix = Counter(n.split("_")[0] for n in names if "_" in n and n.split("_")[0] in ARCS).most_common(1)
        prefix = prefix[0][0] if prefix else None
        if not prefix:
            sys.exit("no Higurashi chapter scripts found (onik_*, wata_*, ...)")
        title = self.a.title or f"Higurashi - {ARCS[prefix]}"
        steps = self.flow(prefix)
        if not any(s[0] == "script" for s in steps):
            sys.exit("could not read the chapter order from flow.txt")
        dll = os.path.join(self.data, "Managed", "Assembly-CSharp.dll")
        tips = read_tips(dll, prefix) if os.path.isfile(dll) and not self.a.no_tips else []
        print(f"{title}: {sum(s[0] == 'script' for s in steps)} story scripts, {len(tips)} TIPS")

        os.makedirs(os.path.join(self.out, "ch"), exist_ok=True)
        flow = ["# the order of play (from flow.txt); every chapter comes back here",
                'if _hig_step == ""', '  set _hig_step = "s0"', "end"]
        for i in range(len(steps) + 1):
            flow += [f'if _hig_step == "s{i}"', f"  jump s{i}", "end"]
        shown = 0
        converted = set()
        for i, (kind, value) in enumerate(steps):
            flow.append(f"label s{i}")
            if kind == "script":
                if value not in converted:
                    if not self.script_path(value):
                        warn("missing script", value)
                        flow.append(f"jump s{i + 1}")
                        continue
                    self.write(value, self.convert(value))
                    converted.add(value)
                flow += [f'set _hig_step = "s{i + 1}"', f"script ch/{ident(value)}"]
            else:   # TIPS newly unlocked at this point
                new = [t for t in tips if shown < t[1] <= value]
                shown = max(shown, value)
                flow += self.tips_menu(new, f"s{i}", f"s{i + 1}", converted)
        flow.append(f"label s{len(steps)}")
        late = [t for t in tips if t[1] > shown]
        if late:
            flow.append(f'set _hig_step = "s{len(steps)}"')
            flow += self.tips_menu(late, f"s{len(steps)}", "end", converted)
            flow.append("label end")
        flow.append("stop")
        self.write("flow", flow, raw=True)
        with open(os.path.join(self.out, "characters.3vs"), "w", encoding="utf-8") as fh:
            fh.write("# speakers (07th-Mod ADV mode); never empty: 3DVN can't include an empty file\n")
            for cid, (name, colour) in sorted(self.chars.items()):
                fh.write(f"character {cid} {quote(name)} {colour}".rstrip() + "\n")
        with open(os.path.join(self.out, "main.3vs"), "w", encoding="utf-8") as fh:
            fh.write(f"# {title} - converted by import_higurashi.py\n"
                     'set _hig_step = ""\nscript ch/flow\n')
        with open(os.path.join(self.out, "game.3vc"), "w", encoding="utf-8") as fh:
            fh.write(f"title {title}\nmenu_title {title}\n"
                     + ("default_display adv\n" if self.a.adv else "default_display nvl\nnvl_accumulate 1\n"))
        self.convert_pictures()
        self.copy_audio()
        return steps, tips

    def tips_menu(self, new, here, after, converted):
        if not new:
            return [f"jump {after}"]
        lines = ["menu"]
        for script, _, title in new:
            if script not in converted:
                self.write(script, self.convert(script))
                converted.add(script)
            lines += [f"  choice {quote('TIPS: ' + title)}",
                      f'    set _hig_step = "{here}"', f"    script ch/{ident(script)}"]
        lines += ['  choice "Continue"', f"    jump {after}", "end"]
        return lines

    def write(self, name, lines, raw=False):
        with open(os.path.join(self.out, "ch", ident(name) + ".3vs"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")

    # ---- assets ----
    def convert_picture(self, job):
        pic, path, dest = job
        target = os.path.join(self.out, dest, pic + (".jpg" if dest == "bg" else ".png"))
        hd = os.path.join(self.out, "hd", dest, pic + (".jpg" if dest == "bg" else ".png"))
        if os.path.exists(target):
            return
        with Image.open(path) as image:
            image.load()
            lw, lh = self.logical_size(path)
            w, h = max(1, round(lw * self.scale)), max(1, round(lh * self.scale))
            if w > 1024 or h > 1024:   # 3DS texture limit (very wide/tall scenes)
                fit = 1024 / max(w, h)
                w, h = max(1, int(w * fit)), max(1, int(h * fit))
            if dest == "bg":
                image = image.convert("RGB")
            else:
                image = image.convert("RGBA")
            os.makedirs(os.path.dirname(target), exist_ok=True)
            small = image.resize((w, h), Image.LANCZOS)
            if dest == "bg":
                small.save(target, quality=88)
            else:
                small.save(target, optimize=True)
            if not self.a.no_hd and (image.width > w * 1.2 or image.height > h * 1.2):
                if image.width > 2048 or image.height > 2048:   # PC/Wii U/Android limit
                    fit = 2048 / max(image.width, image.height)
                    image = image.resize((int(image.width * fit), int(image.height * fit)), Image.LANCZOS)
                os.makedirs(os.path.dirname(hd), exist_ok=True)
                if dest == "bg":
                    image.save(hd, quality=92)
                else:
                    image.save(hd, optimize=True)

    def convert_pictures(self):
        jobs = [(p, path, "bg") for p, path in sorted(self.used["bg"].items())]
        jobs += [(p, path, "sprites") for p, path in sorted(self.used["sprites"].items())]
        with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 2) as pool:
            list(pool.map(self.convert_picture, jobs))

    def copy_audio(self):
        for name in sorted(self.used["audio"]):
            dst = os.path.join(self.out, "audio", name + ".ogg")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if not os.path.exists(dst):
                shutil.copy(self.sources[name], dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src", help="the installed chapter folder (with HigurashiEpNN_Data)")
    ap.add_argument("out")
    ap.add_argument("--art", choices=sorted(ART_SETS), default="console",
                    help="art set: console (the game's default; PS3 art with 07th-Mod), remake "
                         "(CGAlt), original (07th-Mod's Ryukishi07 art)")
    ap.add_argument("--alt-art", action="store_true", help="same as --art remake")
    ap.add_argument("--audio", choices=sorted(AUDIO_SETS), default="mangagamer",
                    help="07th-Mod music/sound set (default mangagamer)")
    ap.add_argument("--adv", action="store_true",
                    help="07th-Mod: one line at a time with speaker names, instead of full pages")
    ap.add_argument("--censor", type=int, default=2, help="07th-Mod censorship level 0-5 (default 2)")
    ap.add_argument("--title", default=None)
    ap.add_argument("--no-tips", action="store_true", help="leave out the TIPS")
    ap.add_argument("--no-hd", action="store_true", help="don't keep the full-size pictures in hd/")
    a = ap.parse_args()
    c = Converter(a)
    steps, tips = c.run()
    print(f"done: {len(c.used['bg'])} backgrounds, {len(c.used['sprites'])} sprites, "
          f"{len(c.used['audio'])} sounds")
    for k, v in report.most_common():
        print(f"  warning: {k} x{v}")
    for s in notes:
        print(f"    e.g. {s}")


if __name__ == "__main__":
    main()
