#!/usr/bin/env python3
"""Inspect a serialized Mahoyo chart without executing TJS or game scripts.

The trace reports reached chart blocks, not executed or verified scene scripts.
A stop is not proof that the game has ended.
"""

import argparse
import json
import re
import struct
import sys
import zipfile
import zlib
from fractions import Fraction
from pathlib import Path


MAX_BYTES = 8 * 1024 * 1024
MAX_DEPTH = 100
ENTRY = "override/main.gc"
ENTRIES = (ENTRY, "others/main.gc")
INTEGER = re.compile(r"-?(?:0|[1-9][0-9]*)")


class ChartError(ValueError):
    pass


class LiteralParser:
    def __init__(self, text):
        self.text = text
        self.pos = 0

    def error(self, message):
        raise ChartError(f"{message} at character {self.pos}")

    def whitespace(self):
        while self.pos < len(self.text) and self.text[self.pos] in " \t\r\n":
            self.pos += 1

    def take(self, token):
        self.whitespace()
        if self.text.startswith(token, self.pos):
            self.pos += len(token)
            return True
        return False

    def require(self, token):
        if not self.take(token):
            self.error(f"expected {token!r}")

    def type_separator(self):
        if self.pos >= len(self.text) or self.text[self.pos] not in " \t\r\n":
            self.error("expected whitespace after type")
        self.whitespace()

    def string(self):
        self.require('"')
        result = []
        escapes = {'"': '"', "'": "'", "\\": "\\", "n": "\n", "r": "\r", "t": "\t",
                   "b": "\b", "f": "\f"}
        while self.pos < len(self.text):
            char = self.text[self.pos]
            self.pos += 1
            if char == '"':
                return "".join(result)
            if char == "\\":
                if self.pos >= len(self.text):
                    self.error("unterminated escape")
                char = self.text[self.pos]
                self.pos += 1
                if char not in escapes:
                    self.error(f"unsupported escape \\{char}")
                char = escapes[char]
            elif ord(char) < 32:
                self.error("unescaped control character in string")
            result.append(char)
        self.error("unterminated string")

    def value(self, depth=0):
        if depth > MAX_DEPTH:
            self.error("maximum nesting depth exceeded")
        self.whitespace()
        if self.take("%["):
            result = {}
            if self.take("]"):
                return result
            while True:
                key = self.string()
                if key in result:
                    self.error(f"duplicate dictionary key {key!r}")
                self.require("=>")
                result[key] = self.value(depth + 1)
                if self.take("]"):
                    return result
                self.require(",")
        if self.take("["):
            result = []
            if self.take("]"):
                return result
            while True:
                result.append(self.value(depth + 1))
                if self.take("]"):
                    return result
                self.require(",")
        if self.take("string"):
            self.type_separator()
            return self.string()
        if self.take("int"):
            self.type_separator()
            match = INTEGER.match(self.text, self.pos)
            if not match:
                self.error("expected decimal integer")
            try:
                number = int(match.group())
            except ValueError as exc:
                raise ChartError(f"invalid integer at character {self.pos}") from exc
            self.pos += len(match.group())
            return number
        if self.take("void"):
            return None
        self.error("expected literal value")

    def parse(self):
        value = self.value()
        self.whitespace()
        if self.pos != len(self.text):
            self.error("unexpected trailing input")
        return value


def parse_literal(text):
    return LiteralParser(text).parse()


PACKED_MAGIC = b"\xfe\xfe\x02\xff\xfe"


def unpack_chart(data):
    """Decode KiriKiri's zlib text container: magic, packed size, size, stream."""
    if len(data) < 21:
        raise ChartError("truncated packed chart")
    packed, size = struct.unpack("<QQ", data[5:21])
    if size > MAX_BYTES or packed != len(data) - 21:
        raise ChartError("invalid packed chart header")
    decoder = zlib.decompressobj()
    try:
        out = decoder.decompress(data[21:], size + 1)
    except zlib.error as exc:
        raise ChartError(f"invalid packed chart: {exc}") from exc
    if len(out) != size or not decoder.eof or decoder.unconsumed_tail or decoder.unused_data:
        raise ChartError("packed chart size mismatch")
    return out


def load_chart(path, entry=ENTRY):
    path = Path(path)
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            entries = [info for info in archive.infolist() if info.filename == entry]
            if len(entries) != 1:
                raise ChartError(f"archive must contain exactly one {entry}")
            if entries[0].file_size > MAX_BYTES:
                raise ChartError("chart exceeds size limit")
            data = archive.read(entries[0])
    else:
        with path.open("rb") as source:
            data = source.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ChartError("chart exceeds size limit")
    if data.startswith(PACKED_MAGIC):
        data = unpack_chart(data)
    try:
        text = data.decode("utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff"))
                           else "utf-16-le")
    except UnicodeError as exc:
        raise ChartError(f"invalid UTF-16 chart: {exc}") from exc
    return parse_literal(text)


def require_type(value, kind, where):
    if type(value) is not kind:
        raise ChartError(f"{where} must be {kind.__name__}")
    return value


def field(obj, name, kind, where):
    if name not in obj:
        raise ChartError(f"{where} missing {name}")
    return require_type(obj[name], kind, f"{where}.{name}")


def inspect_chart(chart):
    """Validate the chart's control-flow fields and retain source order."""
    require_type(chart, dict, "chart")
    first = field(chart, "first", int, "chart")
    pages = field(chart, "pages", list, "chart")
    field(chart, "subscript", str, "chart")
    result_pages = []
    block_ids, route_ids, page_ids = set(), set(), set()
    for pi, page in enumerate(pages):
        where = f"pages[{pi}]"
        require_type(page, dict, where)
        page_id = field(page, "id", int, where)
        if page_id in page_ids:
            raise ChartError(f"duplicate page id {page_id}")
        page_ids.add(page_id)
        blocks = field(page, "blocks", list, where)
        result_blocks = []
        for bi, block in enumerate(blocks):
            loc = f"{where}.blocks[{bi}]"
            require_type(block, dict, loc)
            block_id = field(block, "id", int, loc)
            if block_id in block_ids:
                raise ChartError(f"duplicate block id {block_id}")
            block_ids.add(block_id)
            script = block.get("script")
            if type(script) is str:
                scenes = [script] if script else []
            elif type(script) is list and all(type(item) is str and item for item in script):
                scenes = list(script)
            else:
                raise ChartError(f"{loc}.script must be a string or array of nonempty strings")
            operation = field(block, "operate", str, loc)
            for name in ("readblocks", "force", "_diverge", "divergedata", "state"):
                if name not in block:
                    raise ChartError(f"{loc} missing {name}")
            require_type(block["_diverge"], str, f"{loc}._diverge")
            require_type(block["divergedata"], dict, f"{loc}.divergedata")
            require_type(block["state"], int, f"{loc}.state")
            routes = field(block, "routes", list, loc)
            result_routes = []
            for ri, route in enumerate(routes):
                rloc = f"{loc}.routes[{ri}]"
                require_type(route, dict, rloc)
                route_id = field(route, "id", int, rloc)
                if route_id in route_ids:
                    raise ChartError(f"duplicate route id {route_id}")
                route_ids.add(route_id)
                target = field(route, "arrival", int, rloc)
                for name in ("condition", "alternative", "status", "state", "points"):
                    if name not in route:
                        raise ChartError(f"{rloc} missing {name}")
                if route["condition"] is not None and type(route["condition"]) is not str:
                    raise ChartError(f"{rloc}.condition must be void or string")
                if type(route["alternative"]) not in (str, dict):
                    raise ChartError(f"{rloc}.alternative must be a string or dictionary")
                if route["status"] is not None and type(route["status"]) not in (str, int):
                    raise ChartError(f"{rloc}.status must be void, string or int")
                require_type(route["state"], int, f"{rloc}.state")
                require_type(route["points"], list, f"{rloc}.points")
                result_routes.append({"id": route_id, "arrival": target,
                                      "condition": route["condition"],
                                      "alternative": route["alternative"],
                                      "status": route["status"], "source": route})
            result_blocks.append({"id": block_id, "scenes": scenes,
                                  "operation": operation, "routes": result_routes,
                                  "source": block})
        result_pages.append({"id": page_id, "blocks": result_blocks,
                             "source": {k: v for k, v in page.items() if k != "blocks"}})
    return {"first": first, "pages": result_pages,
            "metadata": {k: v for k, v in chart.items() if k != "pages"}}


BLOCK_FIELDS = {"width", "left", "readblocks", "script", "routes", "_diverge",
                "operate", "subtitle", "top", "divergedata", "synopsis", "data",
                "title", "id", "height", "state", "force"}
ROUTE_FIELDS = {"status", "points", "alternative", "id", "state", "arrival", "condition", "popup"}
CHART_FIELDS = {"anchors", "first", "flags", "nextid", "project", "subscript",
                "categories", "title", "curpage"}
PAGE_FIELDS = {"baseleft", "basetop", "id", "title"}


def trace_chart(chart):
    """Report reached blocks, stopping before anything not proven unconditional."""
    blocks = {block["id"]: (page, block)
              for page in chart["pages"] for block in page["blocks"]}
    visited = set()
    reached = []
    current = chart["first"]
    metadata = chart["metadata"]
    if set(metadata) - CHART_FIELDS:
        return {"reason": "unknown_chart_fields",
                "fields": sorted(set(metadata) - CHART_FIELDS), "blocks": reached}
    if metadata["subscript"]:
        return {"reason": "chart_subscript", "blocks": reached}
    while True:
        if current not in blocks:
            return {"reason": "missing_target", "target": current, "blocks": reached}
        if current in visited:
            return {"reason": "cycle", "target": current, "blocks": reached}
        visited.add(current)
        page, block = blocks[current]
        source = block["source"]
        reached.append({"id": current, "scenes": block["scenes"]})
        def stop(reason, **details):
            return {"reason": reason, "block": current, "blocks": reached, **details}
        if block["operation"]:
            return stop("operation", operation=block["operation"])
        if set(page["source"]) - PAGE_FIELDS:
            return stop("unknown_page_fields",
                        fields=sorted(set(page["source"]) - PAGE_FIELDS))
        if set(source) - BLOCK_FIELDS:
            return stop("unknown_block_fields", fields=sorted(set(source) - BLOCK_FIELDS))
        if source.get("readblocks") is not None or source.get("force") is not None:
            return stop("ambiguous_block_control")
        if source.get("data") not in (None, ""):
            return stop("ambiguous_block_data", data=source["data"])
        if source["state"] != 9:
            return stop("unknown_block_state", state=source["state"])
        if source.get("_diverge", "alternative") != "alternative" or source.get("divergedata", {}) != {}:
            return stop("ambiguous_divergence")
        routes = block["routes"]
        if not routes:
            return stop("no_routes")
        if len(routes) != 1:
            return stop("multiple_routes", route_ids=[route["id"] for route in routes])
        route = routes[0]
        if set(route["source"]) - ROUTE_FIELDS:
            return stop("unknown_route_fields", route=route["id"],
                        fields=sorted(set(route["source"]) - ROUTE_FIELDS))
        if route["condition"] is not None:
            return stop("condition", route=route["id"], condition=route["condition"])
        if route["alternative"] != "":
            return stop("choice", route=route["id"], alternative=route["alternative"])
        if route["status"] is not None or route["source"].get("popup") is not None:
            return stop("ambiguous_route_status", route=route["id"])
        if route["source"]["state"] != 131072:
            return stop("unknown_route_state", route=route["id"],
                        state=route["source"]["state"])
        current = route["arrival"]


NUMBER = r"-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?"
NAME = r"[^\s,=+<>?:!&|()*/0-9.-][^\s,=+<>?:!&|()*/-]*"
ASSIGN = re.compile(rf"({NAME})=({NUMBER})")
ADD = re.compile(rf"({NAME})\+=({NUMBER})")
RAISE = re.compile(rf"({NAME})=({NAME})<({NUMBER})\?({NUMBER}):({NAME})")
FORCE_MODES = {"1": "skip_if_read", "2": "play_if_read"}


def parse_operation(text):
    """Parse the small flag language used by reachable chart operations.

    Returns a list of (kind, name, number), or None for anything else. Names
    beginning with "_" are the game's persistent system flags.
    """
    steps = []
    for part in text.split(","):
        if match := ASSIGN.fullmatch(part):
            steps.append(("set", match[1], Fraction(match[2])))
        elif match := ADD.fullmatch(part):
            steps.append(("add", match[1], Fraction(match[2])))
        elif (match := RAISE.fullmatch(part)) and len({match[1], match[2], match[5]}) == 1 \
                and match[3] == match[4]:
            steps.append(("raise", match[1], Fraction(match[3])))
        else:
            return None
    return steps


def number_text(value):
    return str(int(value)) if value.denominator == 1 else str(float(value))


def apply_operation(steps, flags):
    for kind, name, number in steps:
        current = flags.get(name, Fraction(0))
        if kind == "set":
            flags[name] = number
        elif kind == "add":
            flags[name] = current + number
        elif current < number:
            flags[name] = number


def trace_initial_playthrough(chart):
    """Follow the original chart as a fresh game would, without choosing anything.

    Flag operations use a strict arithmetic grammar and routes are followed only
    when they are the first, unconditional, non-choice route. This mirrors the
    original tracer: operations run, then the first eligible normal route wins.
    A block with no routes returns to the title, so it is reported as terminal.
    """
    blocks = {block["id"]: (page, block)
              for page in chart["pages"] for block in page["blocks"]}
    metadata = chart["metadata"]
    flags, reached, scenes, visited = {}, [], [], set()
    current = chart["first"]

    def result(reason, **details):
        return {"reason": reason, "blocks": reached, "scene_order": scenes,
                "flags": {k: number_text(v) for k, v in flags.items()}, **details}

    if set(metadata) - CHART_FIELDS:
        return result("unknown_chart_fields", fields=sorted(set(metadata) - CHART_FIELDS))
    if metadata["subscript"]:
        return result("chart_subscript")
    while True:
        if current not in blocks:
            return result("missing_target", target=current)
        if current in visited:
            return result("cycle", target=current)
        visited.add(current)
        page, block = blocks[current]
        source = block["source"]

        def stop(reason, **details):
            return result(reason, block=current, **details)

        if set(page["source"]) - PAGE_FIELDS:
            return stop("unknown_page_fields", fields=sorted(set(page["source"]) - PAGE_FIELDS))
        if set(source) - BLOCK_FIELDS:
            return stop("unknown_block_fields", fields=sorted(set(source) - BLOCK_FIELDS))
        force = source["force"]
        if source["readblocks"] is not None or (
                force is not None and (type(force) is not str or force not in FORCE_MODES)):
            return stop("ambiguous_block_control")
        if source["state"] != 9:
            return stop("unknown_block_state", state=source["state"])
        if source["_diverge"] != "alternative" or source["divergedata"]:
            return stop("ambiguous_divergence")
        data = source.get("data")
        if data not in (None, "") and not re.fullmatch(INTEGER, str(data)):
            return stop("ambiguous_block_data", data=data)
        steps = parse_operation(block["operation"]) if block["operation"] else []
        if steps is None:
            return stop("unsupported_operation", operation=block["operation"])
        entry = {"id": current, "page": page["source"].get("title"),
                 "scenes": block["scenes"]}
        if block["operation"]:
            entry["operation"] = block["operation"]
        if force is not None:
            entry["when_already_read"] = FORCE_MODES[force]
        reached.append(entry)
        scenes.extend(block["scenes"])
        apply_operation(steps, flags)
        routes = block["routes"]
        if not routes:
            return stop("end_of_chart")
        for route in routes:
            if set(route["source"]) - ROUTE_FIELDS:
                return stop("unknown_route_fields", route=route["id"])
            if route["condition"] not in (None, ""):
                return stop("condition", route=route["id"], condition=route["condition"])
            if route["alternative"] not in ("", {}) or route["status"] is not None \
                    or route["source"].get("popup") is not None \
                    or route["source"]["state"] != 131072:
                return stop("choice_or_ambiguous_route", route=route["id"])
        if len(routes) != 1:
            return stop("multiple_routes", route_ids=[r["id"] for r in routes])
        current = routes[0]["arrival"]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="ZIP data.xp3, or extracted main.gc (plain UTF-16 or KiriKiri zlib-packed)")
    parser.add_argument("--entry", choices=ENTRIES, default=ENTRY,
                        help="chart entry within a ZIP archive (default: override/main.gc)")
    parser.add_argument("--initial-playthrough", action="store_true",
                        help="print only the deterministic fresh-game scene order")
    args = parser.parse_args(argv)
    try:
        chart = inspect_chart(load_chart(args.input, args.entry))
        if args.initial_playthrough:
            report = {"trace": trace_initial_playthrough(chart)}
        else:
            report = {"chart": chart, "trace": trace_chart(chart)}
        print(json.dumps(report, ensure_ascii=True, indent=2))
    except (OSError, UnicodeError, ChartError, zipfile.BadZipFile, RuntimeError) as exc:
        print(f"chart error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
