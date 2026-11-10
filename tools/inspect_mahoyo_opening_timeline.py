#!/usr/bin/env python3
"""Inspect the serialized timeline in Mahoyo's scenario/opening.ks.

The opening is a Kirikiri timeline_object data literal, not a movie. This tool
only parses and summarizes that literal; it never executes TJS, renders frames,
or interprets easing/layer semantics. Keyframe cells are reported by index
because their column meanings are not decoded here.
"""

import argparse
import json
import re
import sys
import zipfile
import zlib
from pathlib import Path

ENTRY = "scenario/opening.ks"
MAX_BYTES = 8 * 1024 * 1024
MAX_DEPTH = 100
NUMBER = re.compile(r"-?(?:0[xX][0-9a-fA-F]+|\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)")
WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


class TimelineError(ValueError):
    pass


class Parser:
    def __init__(self, text):
        self.text = text
        self.pos = 0

    def error(self, message):
        raise TimelineError(f"{message} at character {self.pos}")

    def space(self):
        while self.pos < len(self.text) and self.text[self.pos] in " \t\r\n":
            self.pos += 1

    def take(self, token):
        self.space()
        if self.text.startswith(token, self.pos):
            self.pos += len(token)
            return True
        return False

    def require(self, token):
        if not self.take(token):
            self.error(f"expected {token!r}")

    def string(self):
        self.require('"')
        out = []
        escapes = {'"': '"', "'": "'", "\\": "\\", "n": "\n", "r": "\r", "t": "\t"}
        while self.pos < len(self.text):
            ch = self.text[self.pos]
            self.pos += 1
            if ch == '"':
                return "".join(out)
            if ch == "\\":
                if self.pos >= len(self.text) or self.text[self.pos] not in escapes:
                    self.error("unsupported escape")
                ch = escapes[self.text[self.pos]]
                self.pos += 1
            out.append(ch)
        self.error("unterminated string")

    def value(self, depth=0):
        if depth > MAX_DEPTH:
            self.error("maximum nesting depth exceeded")
        self.take("(const)")
        self.space()
        if self.take("%["):
            out = {}
            if self.take("]"):
                return out
            while True:
                key = self.string()
                if key in out:
                    self.error(f"duplicate key {key!r}")
                self.require("=>")
                out[key] = self.value(depth + 1)
                if self.take("]"):
                    return out
                self.require(",")
        if self.take("["):
            out = []
            if self.take("]"):
                return out
            while True:
                out.append(self.value(depth + 1))
                if self.take("]"):
                    return out
                self.require(",")
        if self.text.startswith('"', self.pos):
            return self.string()
        match = NUMBER.match(self.text, self.pos)
        if match:
            self.pos = match.end()
            raw = match.group()
            if raw.lower().startswith(("0x", "-0x")):
                return int(raw, 16)
            return float(raw) if re.search(r"[.eE]", raw) else int(raw)
        match = WORD.match(self.text, self.pos)
        if match and match.group() in ("null", "void", "true", "false"):
            self.pos = match.end()
            return {"true": True, "false": False}.get(match.group())
        self.error("expected literal value")


def parse_literal(text):
    parser = Parser(text)
    try:
        value = parser.value()
    except (ValueError, OverflowError, RecursionError) as exc:
        if isinstance(exc, TimelineError):
            raise
        raise TimelineError(f"invalid literal: {exc}") from exc
    parser.space()
    if parser.pos != len(parser.text):
        parser.error("unexpected trailing input")
    return value


STORAGE_OFFSET = 5  # empirical: storage value sits 5 cells after its column index; unverified


def decode_script(data):
    if len(data) > MAX_BYTES:
        raise TimelineError("script is too large")
    try:
        if data.startswith((b"\xff\xfe", b"\xfe\xff")):
            return data.decode("utf-16")
        for encoding in ("utf-8-sig", "cp932"):
            try:
                return data.decode(encoding)
            except UnicodeError:
                pass
    except UnicodeError as exc:
        raise TimelineError(f"invalid script encoding: {exc}") from exc
    raise TimelineError("unsupported script encoding")


def read_script(path, entry=ENTRY):
    path = Path(path)
    try:
        if zipfile.is_zipfile(path):
            with zipfile.ZipFile(path) as archive:
                try:
                    info = archive.getinfo(entry)
                except KeyError as exc:
                    raise TimelineError(f"{entry} not found in archive") from exc
                if info.file_size > MAX_BYTES:
                    raise TimelineError("script is too large")
                with archive.open(info) as handle:
                    data = handle.read(MAX_BYTES + 1)
        else:
            with path.open("rb") as handle:
                data = handle.read(MAX_BYTES + 1)
    except (zipfile.BadZipFile, zlib.error, EOFError, NotImplementedError, RuntimeError) as exc:
        raise TimelineError(f"unreadable archive: {exc}") from exc
    return decode_script(data)


def extract_literal(script):
    match = re.search(r"Scripts\.eval\('(.*?)'\);\s*global\.__tla_name\s*=\s*\"([^\"]*)\"",
                      script, re.S)
    if not match:
        raise TimelineError("timeline data literal not found")
    return match.group(1), match.group(2)


def playback(script):
    """Return literal @playtimeline tags and their enclosing conditions.

    Conditions are source text, not evaluated. Malformed @if nesting raises.
    """
    result, stack = [], []
    for number, line in enumerate(script.splitlines(), 1):
        line = line.strip()
        word = line[1:].split(None, 1)[0] if line.startswith("@") and len(line) > 1 else ""
        rest = line[1 + len(word):].strip()
        if word in ("if", "elsif"):
            if not rest:
                raise TimelineError(f"line {number}: @{word} has no condition")
            if word == "if":
                stack.append({"prior": [], "current": rest, "else": False})
                continue
            if not stack or stack[-1]["else"]:
                raise TimelineError(f"line {number}: unexpected @elsif")
            frame = stack[-1]
            frame["prior"].append(frame["current"])
            frame["current"] = rest
        elif word == "else":
            if not stack or stack[-1]["else"]:
                raise TimelineError(f"line {number}: unexpected @else")
            frame = stack[-1]
            frame["prior"].append(frame["current"])
            frame["current"], frame["else"] = None, True
        elif word == "endif":
            if not stack:
                raise TimelineError(f"line {number}: @endif without @if")
            stack.pop()
        elif word == "playtimeline":
            conditions = []
            for frame in stack:
                conditions += [f"not({c})" for c in frame["prior"]]
                if frame["current"] is not None:
                    conditions.append(frame["current"])
            result.append({"tag": line, "conditions": conditions})
    if stack:
        raise TimelineError("unterminated @if")
    return result


def walk(node, out):
    if isinstance(node, dict):
        if "className" in node:
            out.append(node)
        for child in node.values():
            walk(child, out)
    elif isinstance(node, list):
        for child in node:
            walk(child, out)


def number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def inspect_object(obj, where):
    name, class_name = obj.get("name"), obj.get("className")
    if not isinstance(class_name, str):
        raise TimelineError(f"{where}: className must be a string")
    if name is not None and not isinstance(name, str):
        raise TimelineError(f"{where}: name must be a string")
    timeline = obj.get("timeline")
    columns, frames = None, []
    if timeline is not None:
        if not isinstance(timeline, list) or len(timeline) < 2:
            raise TimelineError(f"{where}: timeline must be [columns, frames, ...]")
        columns, frames = timeline[0], timeline[1]
        if not isinstance(columns, list) or not all(isinstance(c, str) for c in columns):
            raise TimelineError(f"{where}: timeline columns must be strings")
        if not isinstance(frames, list):
            raise TimelineError(f"{where}: timeline frames must be a list")
        for i, frame in enumerate(frames):
            if not isinstance(frame, list) or not frame or not number(frame[0]):
                raise TimelineError(f"{where}: frame {i} must be a list starting with a time")
    times = [f[0] for f in frames]
    candidates = []
    if columns and "storage" in columns:
        col = columns.index("storage") + STORAGE_OFFSET
        for f in frames:
            if col < len(f) and isinstance(f[col], str) and f[col] not in candidates:
                candidates.append(f[col])
    return {"storageCandidates": candidates, "name": name, "className": class_name,
            "keyframes": len(times), "firstTime": min(times, default=None),
            "lastTime": max(times, default=None), "columns": columns}


def summarize(script):
    literal, name = extract_literal(script)
    data = parse_literal(literal)
    if not isinstance(data, dict):
        raise TimelineError("chart must be a dictionary")
    scenes = data.get("scenes")
    if not isinstance(scenes, list) or len(scenes) % 2:
        raise TimelineError("scenes must be an id/content list")
    summary = {"name": name, "baseWidth": data.get("baseWidth"),
               "baseHeight": data.get("baseHeight"), "scenes": [],
               "storageCandidatesNote": "heuristic: fixed offset from the 'storage' column; "
                                        "not a verified decoding",
               "playback": playback(script)}
    for index in range(0, len(scenes), 2):
        scene_id, content = scenes[index], scenes[index + 1]
        if not number(scene_id) or not isinstance(content, list):
            raise TimelineError(f"scene entry {index // 2}: expected numeric id and list content")
        objects = []
        walk(content, objects)
        entries = [inspect_object(o, f"scene {scene_id} object {i}") for i, o in enumerate(objects)]
        summary["scenes"].append({"id": scene_id, "objects": entries})
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="data.xp3 (ZIP form) or opening.ks")
    parser.add_argument("--json", action="store_true", help="print JSON")
    args = parser.parse_args(argv)
    try:
        summary = summarize(read_script(args.source))
    except (OSError, TimelineError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(summary, ensure_ascii=False, indent=2))
        return 0
    for p in summary["playback"]:
        print(p["conditions"], p["tag"])
    for scene in summary["scenes"]:
        print(f"scene {scene['id']}: {len(scene['objects'])} objects")
        for o in scene["objects"]:
            print(f"  {o['className']:12} {o['name']!s:24} keys={o['keyframes']} "
                  f"{o['firstTime']}..{o['lastTime']} storage candidates (heuristic): "
                  f"{len(o['storageCandidates'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
