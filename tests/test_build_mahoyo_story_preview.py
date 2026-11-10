import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_mahoyo_story_preview as builder


class StoryPreviewTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "one.ks").write_text("first[l]\n@return\n", encoding="utf-8")
        (self.source / "two.ks").write_text("second[l]\n@return\n", encoding="utf-8")
        self.assets = self.root / "assets"
        self.assets.mkdir()
        self.theme = self.root / "theme"
        (self.theme / "ui").mkdir(parents=True)
        (self.theme / "game.3vc").write_text("default_display nvl\n", encoding="utf-8")
        (self.theme / "ui" / "sample.png").write_bytes(b"fixture")
        self.output = self.root / "game"

    def build(self, trace):
        with patch.object(builder, "load_chart", return_value={}), \
             patch.object(builder, "inspect_chart", return_value={}), \
             patch.object(builder, "trace_initial_playthrough", return_value=trace):
            return builder.build_preview(self.root / "chart", "others/main.gc", self.source,
                                         self.assets, self.theme, self.output,
                                         "1024x576", "1024x576")

    def test_build_preserves_scene_order_and_marks_skipped_interludes(self):
        trace = {"reason": "end_of_chart", "scene_order":
                 ["one.ks", "opening.ks", "teatime.ks", "two.ks"]}
        self.assertEqual(self.build(trace), (2, 2))
        script = (self.output / "main.3vs").read_text(encoding="utf-8")
        self.assertLess(script.index('"first"'), script.index("opening sequence is omitted"))
        self.assertLess(script.index("opening sequence is omitted"), script.index("chapter-break interlude is omitted"))
        self.assertLess(script.index("chapter-break interlude is omitted"), script.index('"second"'))
        self.assertTrue(script.endswith("stop\n"))
        self.assertTrue((self.output / "ui" / "sample.png").is_file())
        report = (self.output / "conversion-report.txt").read_text(encoding="utf-8")
        self.assertIn("not a faithful or complete conversion", report)
        check = self.root / "check"
        subprocess.run(["cc", "-o", str(check), str(ROOT / "tests/check_scripts.c"),
                        str(ROOT / "source/gs.c"), "-lm"], check=True, capture_output=True)
        result = subprocess.run([str(check), str(self.output / "main.3vs")],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_incomplete_trace_and_missing_scene_leave_no_output(self):
        with self.assertRaisesRegex(ValueError, "chart trace stopped"):
            self.build({"reason": "condition", "scene_order": ["one.ks"]})
        self.assertFalse(self.output.exists())
        with self.assertRaisesRegex(ValueError, "scene conversion failed"):
            self.build({"reason": "end_of_chart", "scene_order": ["missing.ks"]})
        self.assertFalse(self.output.exists())

    def test_interlude_after_final_scene_is_not_silently_discarded(self):
        self.assertEqual(self.build({"reason": "end_of_chart",
                                     "scene_order": ["one.ks", "teatime.ks"]}), (1, 1))
        script = (self.output / "main.3vs").read_text(encoding="utf-8")
        self.assertLess(script.index('"first"'), script.index("chapter-break interlude is omitted"))
        self.assertLess(script.index("chapter-break interlude is omitted"), script.index("stop\n"))

    def test_existing_output_is_not_modified(self):
        self.output.mkdir()
        marker = self.output / "existing"
        marker.write_text("unchanged", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.build({"reason": "end_of_chart", "scene_order": ["one.ks"]})
        self.assertEqual(marker.read_text(encoding="utf-8"), "unchanged")

    def test_build_uses_real_chart_parser(self):
        def encode(value):
            if value is None:
                return "void"
            if type(value) is int:
                return f"int {value}"
            if isinstance(value, str):
                return "string " + json.dumps(value)
            if isinstance(value, list):
                return "[" + ",".join(encode(item) for item in value) + "]"
            return "%[" + ",".join(json.dumps(key) + "=>" + encode(item)
                                    for key, item in value.items()) + "]"

        block = {"id": 1, "script": ["one.ks", "opening.ks", "two.ks"],
                 "routes": [], "operate": "", "readblocks": None, "force": None,
                 "_diverge": "alternative", "divergedata": {}, "state": 9}
        chart = {"first": 1, "subscript": "", "pages": [{"id": 2, "blocks": [block]}]}
        path = self.root / "main.gc"
        path.write_text(encode(chart), encoding="utf-16")
        self.assertEqual(builder.build_preview(path, "others/main.gc", self.source,
                                               self.assets, self.theme, self.output,
                                               "1024x576", "1024x576"), (2, 1))
        script = (self.output / "main.3vs").read_text(encoding="utf-8")
        self.assertLess(script.index('"first"'), script.index("opening sequence is omitted"))
        self.assertLess(script.index("opening sequence is omitted"), script.index('"second"'))

        other = self.root / "cli-output"
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/build_mahoyo_story_preview.py"),
             str(path), str(self.source), str(other),
             "--assets", str(self.assets), "--theme", str(self.theme)],
            text=True, capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("2 scenes, 1 omitted interludes", result.stdout)
        self.assertEqual((other / "main.3vs").read_text(encoding="utf-8"), script)


if __name__ == "__main__":
    unittest.main()
