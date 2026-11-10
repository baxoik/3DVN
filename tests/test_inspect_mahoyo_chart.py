import copy
import json
import os
import subprocess
import sys
import struct
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import inspect_mahoyo_chart as chart_tool


def route(route_id, target, **changes):
    value = {"id": route_id, "arrival": target, "condition": None,
             "alternative": "", "status": None, "state": 131072, "points": []}
    value.update(changes)
    return value


def block(block_id, scripts, routes, **changes):
    value = {"id": block_id, "script": scripts, "routes": routes, "operate": "",
             "readblocks": None, "force": None, "_diverge": "alternative",
             "divergedata": {}, "state": 9}
    value.update(changes)
    return value


def example():
    return {"first": 10, "subscript": "", "pages": [
        {"id": 1, "blocks": [
            block(10, "a.ks", [route(50, 20)]),
            block(20, ["b.ks", "c.ks"], [route(51, 30)]),
        ]},
        {"id": 2, "blocks": [block(30, "", [])]},
    ]}


def trace(value):
    return chart_tool.trace_chart(chart_tool.inspect_chart(value))


class LiteralParserTests(unittest.TestCase):
    def test_nested_typed_values_order_and_escapes(self):
        parsed = chart_tool.parse_literal(
            r'%["z" => [int -7, string "a\"b\\c\n", void], "a" => %[]]')
        self.assertEqual(list(parsed), ["z", "a"])
        self.assertEqual(parsed["z"], [-7, 'a"b\\c\n', None])
        self.assertEqual(parsed["a"], {})

    def test_rejects_non_literals_and_malformed_input(self):
        for text in ('%["x" => int 1, "x" => int 2]', 'int 1 + int 2',
                     'string "bad\\q"', 'int 01', '[int 1,]',
                     '%["x" => void', 'string "unterminated',
                     'function() {}', 'int 1; dangerous()', 'int 2 trailing',
                     'int1', 'string"no separator"'):
            with self.subTest(text=text), self.assertRaises(chart_tool.ChartError):
                chart_tool.parse_literal(text)
        with self.assertRaisesRegex(chart_tool.ChartError, "nesting"):
            chart_tool.parse_literal("[" * 102 + "void" + "]" * 102)


class ChartTests(unittest.TestCase):
    def test_preserves_order_ids_and_scene_references(self):
        inspected = chart_tool.inspect_chart(example())
        self.assertEqual([p["id"] for p in inspected["pages"]], [1, 2])
        self.assertEqual([b["id"] for b in inspected["pages"][0]["blocks"]], [10, 20])
        result = chart_tool.trace_chart(inspected)
        self.assertEqual(result["reason"], "no_routes")
        self.assertEqual(result["blocks"], [
            {"id": 10, "scenes": ["a.ks"]},
            {"id": 20, "scenes": ["b.ks", "c.ks"]},
            {"id": 30, "scenes": []}])

    def test_stop_before_operation_condition_choice_or_ambiguous_control(self):
        cases = [
            ("operation", lambda x: x["pages"][0]["blocks"][0].update(operate="flag=1")),
            ("condition", lambda x: x["pages"][0]["blocks"][0]["routes"][0].update(condition="")),
            ("choice", lambda x: x["pages"][0]["blocks"][0]["routes"][0].update(alternative={"text": "A"})),
            ("ambiguous_route_status", lambda x: x["pages"][0]["blocks"][0]["routes"][0].update(status=0)),
            ("ambiguous_route_status", lambda x: x["pages"][0]["blocks"][0]["routes"][0].update(popup={})),
            ("ambiguous_block_control", lambda x: x["pages"][0]["blocks"][0].update(force="x")),
            ("ambiguous_block_data", lambda x: x["pages"][0]["blocks"][0].update(data="x")),
            ("ambiguous_divergence", lambda x: x["pages"][0]["blocks"][0].update(divergedata={"x": 1})),
            ("unknown_block_state", lambda x: x["pages"][0]["blocks"][0].update(state=0)),
            ("unknown_route_state", lambda x: x["pages"][0]["blocks"][0]["routes"][0].update(state=0)),
            ("unknown_route_fields", lambda x: x["pages"][0]["blocks"][0]["routes"][0].update(extra=1)),
            ("unknown_block_fields", lambda x: x["pages"][0]["blocks"][0].update(extra=1)),
            ("unknown_page_fields", lambda x: x["pages"][0].update(extra=1)),
            ("unknown_chart_fields", lambda x: x.update(extra=1)),
            ("chart_subscript", lambda x: x.update(subscript="foo")),
        ]
        for expected, mutate in cases:
            value = example()
            mutate(value)
            with self.subTest(expected=expected):
                result = trace(value)
                self.assertEqual(result["reason"], expected)
                self.assertLessEqual(len(result["blocks"]), 1)

    def test_multiple_routes_missing_target_and_cycle(self):
        value = example()
        value["pages"][0]["blocks"][0]["routes"].append(route(52, 30))
        result = trace(value)
        self.assertEqual(result["reason"], "multiple_routes")
        self.assertEqual(result["route_ids"], [50, 52])
        value = example()
        value["pages"][0]["blocks"][0]["routes"][0]["arrival"] = 99
        self.assertEqual((trace(value)["reason"], trace(value)["target"]), ("missing_target", 99))
        value["pages"][0]["blocks"][0]["routes"][0]["arrival"] = 10
        self.assertEqual((trace(value)["reason"], trace(value)["target"]), ("cycle", 10))
        value["first"] = 999
        self.assertEqual(trace(value)["reason"], "missing_target")

    def test_shape_validation(self):
        mutations = [
            lambda x: x.update(first="10"),
            lambda x: x.pop("subscript"),
            lambda x: x["pages"][0]["blocks"][0].pop("operate"),
            lambda x: x["pages"][0]["blocks"][0].update(script=["ok", 1]),
            lambda x: x["pages"][0]["blocks"][1].update(id=10),
            lambda x: x["pages"][1].update(id=1),
            lambda x: x["pages"][0]["blocks"][1]["routes"][0].update(id=50),
            lambda x: x["pages"][0]["blocks"][0]["routes"][0].update(arrival=True),
            lambda x: x["pages"][0]["blocks"][0]["routes"][0].update(condition=[]),
            lambda x: x["pages"][0]["blocks"][0]["routes"][0].pop("status"),
        ]
        for mutate in mutations:
            value = copy.deepcopy(example())
            mutate(value)
            with self.subTest(mutation=mutate), self.assertRaises(chart_tool.ChartError):
                chart_tool.inspect_chart(value)

    def test_cli_standalone_zip_and_invalid_input(self):
        literal = ('%["first" => int 1, "subscript" => string "", "pages" => ['
                   '%["id" => int 2, "blocks" => [%["id" => int 1, '
                   '"script" => string "scene.ks", "routes" => [], "operate" => string "", '
                   '"readblocks" => void, "force" => void, "_diverge" => string "alternative", '
                   '"divergedata" => %[], "state" => int 9]]]]]')
        data = literal.encode("utf-16")
        with tempfile.TemporaryDirectory(dir=ROOT) as folder:
            standalone = Path(folder) / "main.gc"
            archive = Path(folder) / "data.xp3"
            standalone.write_bytes(data)
            with zipfile.ZipFile(archive, "w") as z:
                z.writestr(chart_tool.ENTRY, data)
                z.writestr("others/main.gc", literal.replace("scene.ks", "other.ks").encode("utf-16"))
            env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
            for path in (standalone, archive):
                result = subprocess.run([sys.executable, str(ROOT / "tools/inspect_mahoyo_chart.py"),
                                         str(path)], capture_output=True, text=True, env=env)
                self.assertEqual(result.returncode, 0, result.stderr)
                report = json.loads(result.stdout)
                self.assertEqual(report["trace"]["reason"], "no_routes")
                self.assertEqual(report["trace"]["blocks"][0]["scenes"], ["scene.ks"])
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/inspect_mahoyo_chart.py"),
                 "--entry", "others/main.gc", str(archive)],
                capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stderr)
            other_trace = json.loads(result.stdout)["trace"]
            self.assertEqual(other_trace["reason"], "no_routes")
            self.assertEqual(other_trace["blocks"][0]["scenes"], ["other.ks"])
            standalone.write_bytes(b"\xff")
            result = subprocess.run([sys.executable, str(ROOT / "tools/inspect_mahoyo_chart.py"),
                                     str(standalone)], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 2)
            self.assertIn("UTF-16", result.stderr)
            with zipfile.ZipFile(archive, "w") as z:
                z.writestr("other.gc", data)
            with self.assertRaisesRegex(chart_tool.ChartError, "exactly one"):
                chart_tool.load_chart(archive)


class InitialPlaythroughTests(unittest.TestCase):
    def playthrough(self, value):
        return chart_tool.trace_initial_playthrough(chart_tool.inspect_chart(value))

    def test_follows_operations_force_and_terminal_block(self):
        value = example()
        first, second = value["pages"][0]["blocks"]
        first.update(operate="chapter=1,_chapter=_chapter<1?1:_chapter", data="9")
        second.update(operate="_extra=1,\u2605+=1,chapter=5.5", force="2")
        result = self.playthrough(value)
        self.assertEqual(result["reason"], "end_of_chart")
        self.assertEqual(result["scene_order"], ["a.ks", "b.ks", "c.ks"])
        self.assertEqual(result["flags"], {"chapter": "5.5", "_chapter": "1",
                                           "_extra": "1", "\u2605": "1"})
        self.assertEqual(result["blocks"][1]["when_already_read"], "play_if_read")

    def test_raise_keeps_a_higher_persistent_value(self):
        value = example()
        value["pages"][0]["blocks"][0].update(
            operate="_chapter=7,_chapter=_chapter<2?2:_chapter")
        self.assertEqual(self.playthrough(value)["flags"], {"_chapter": "7"})

    def test_stops_instead_of_guessing(self):
        cases = [
            ("unsupported_operation", lambda x: x["pages"][0]["blocks"][0].update(operate="a=b")),
            ("unsupported_operation", lambda x: x["pages"][0]["blocks"][0].update(operate="a=1;b=2")),
            ("unsupported_operation", lambda x: x["pages"][0]["blocks"][0].update(
                operate="a=a<2?3:a")),
            ("condition", lambda x: x["pages"][0]["blocks"][0]["routes"][0].update(condition="a>0")),
            ("choice_or_ambiguous_route", lambda x: x["pages"][0]["blocks"][0]["routes"][0].update(
                alternative={"text": "A"})),
            ("multiple_routes", lambda x: x["pages"][0]["blocks"][0]["routes"].append(route(60, 30))),
            ("ambiguous_block_control", lambda x: x["pages"][0]["blocks"][0].update(force="3")),
            ("ambiguous_block_control", lambda x: x["pages"][0]["blocks"][0].update(force=[])),
            ("ambiguous_block_control", lambda x: x["pages"][0]["blocks"][0].update(readblocks=[1])),
            ("ambiguous_block_data", lambda x: x["pages"][0]["blocks"][0].update(data="x")),
            ("cycle", lambda x: x["pages"][0]["blocks"][1]["routes"][0].update(arrival=10)),
        ]
        for expected, mutate in cases:
            value = example()
            mutate(value)
            with self.subTest(expected=expected, mutate=mutate):
                self.assertEqual(self.playthrough(value)["reason"], expected)

    def test_packed_chart_container_and_apostrophe_escape(self):
        literal = ('%["first" => int 1, "subscript" => string "", "pages" => ['
                   '%["id" => int 2, "blocks" => [%["id" => int 1, '
                   '"script" => string "scene.ks", "routes" => [], "operate" => string "", '
                   '"readblocks" => void, "force" => void, "_diverge" => string "alternative", '
                   '"divergedata" => %[], "state" => int 9, '
                   '"subtitle" => string "Don\\\'t"]]]]]')
        raw = literal.encode("utf-16-le")
        packed = zlib.compress(raw)
        data = (chart_tool.PACKED_MAGIC + struct.pack("<QQ", len(packed), len(raw)) + packed)
        with tempfile.TemporaryDirectory(dir=ROOT) as folder:
            path = Path(folder) / "main.gc"
            path.write_bytes(data)
            chart = chart_tool.load_chart(path)
            self.assertEqual(chart["pages"][0]["blocks"][0]["subtitle"], "Don't")
            for bad in (data[:-1], data[:5] + struct.pack("<QQ", len(packed), 1) + packed):
                path.write_bytes(bad)
                with self.assertRaises(chart_tool.ChartError):
                    chart_tool.load_chart(path)
            path.write_bytes(data)
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/inspect_mahoyo_chart.py"),
                 "--initial-playthrough", str(path)], capture_output=True, text=True,
                env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["trace"]["scene_order"], ["scene.ks"])


if __name__ == "__main__":
    unittest.main()
