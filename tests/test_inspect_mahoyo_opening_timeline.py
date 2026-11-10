import sys
import unittest
import zipfile
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import inspect_mahoyo_opening_timeline as tool

SCRIPT = """@if exp=cf.speffect
@playtimeline scene=4 next=1
@else
@playtimeline scene=7 next=1
@endif
*tladata
@iscript
global.__tmp_tla = Scripts.eval('(const) %[
 "baseHeight" => 576,
 "baseWidth" => 1024,
 "scenes" => (const) [
  4,
  (const) [
   (const) [
    (const) %[
     "className" => "LayerObject",
     "name" => "L",
     "timeline" => (const) [
      (const) ["storage", "opacity"],
      (const) [
       (const) [0, 1, null, null, null, "pic", 0.5, 255],
       (const) [30, null, null, null, null, "pic2", null, 0]
      ],
      31
     ]
    ]
   ]
  ]
 ]
]');
global.__tla_name = "opening";
@endscript
"""


class OpeningTimelineTest(unittest.TestCase):
    def test_summary(self):
        s = tool.summarize(SCRIPT)
        self.assertEqual(s["name"], "opening")
        self.assertEqual([p["conditions"] for p in s["playback"]],
                         [["exp=cf.speffect"], ["not(exp=cf.speffect)"]])
        obj = s["scenes"][0]["objects"][0]
        self.assertEqual((obj["keyframes"], obj["lastTime"]), (2, 30))
        self.assertEqual(obj["storageCandidates"], ["pic", "pic2"])

    def test_zip_and_utf16(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "data.xp3"
            with zipfile.ZipFile(path, "w") as z:
                z.writestr(tool.ENTRY, b"\xff\xfe" + SCRIPT.encode("utf-16-le"))
            self.assertEqual(tool.summarize(tool.read_script(path))["name"], "opening")
            self.assertEqual(tool.main([str(path)]), 0)

    def test_rejects_bad_input(self):
        for text in ('%["a" => 1, "a" => 2]', "[1,]", "[" * 200 + "]" * 200,
                     "[" + "9" * 5000 + "]", "%[1 => 2]", '["\\q"]'):
            with self.assertRaises(tool.TimelineError, msg=text[:20]):
                tool.parse_literal(text)
        with self.assertRaises(tool.TimelineError):
            tool.extract_literal("no timeline")

    def test_decode_errors(self):
        for data in (b"\xff\xfe\x00", b"\xff\xfe\x00\xd8", b"\x81"):
            with self.assertRaises(tool.TimelineError):
                tool.decode_script(data)
        with self.assertRaises(tool.TimelineError):
            tool.decode_script(b"x" * (tool.MAX_BYTES + 1))

    def test_bounded_reads(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "opening.ks"
            path.write_bytes(b"x" * (tool.MAX_BYTES + 5))
            with self.assertRaises(tool.TimelineError):
                tool.read_script(path)
            bad = Path(d) / "bad.xp3"
            with zipfile.ZipFile(bad, "w") as z:
                z.writestr("other", "x")
            with self.assertRaises(tool.TimelineError):
                tool.read_script(bad)
            self.assertEqual(tool.main([str(bad)]), 1)

    def test_control_nesting(self):
        for body in ("@endif", "@else", "@elsif exp=1", "@if exp=1", "@if",
                     "@if exp=1\n@else\n@else\n@endif",
                     "@if exp=1\n@else\n@elsif exp=2\n@endif"):
            with self.assertRaises(tool.TimelineError, msg=body):
                tool.playback(body)
        got = tool.playback("@if a\n@elsif b\n@playtimeline scene=1\n@else\n@endif")
        self.assertEqual(got[0]["conditions"], ["not(a)", "b"])

    def test_shape_validation(self):
        cases = [
            SCRIPT.replace('"scenes" => (const) [', '"scenes" => (const) [1,'),
            SCRIPT.replace("  4,\n", "  \"x\",\n"),
            SCRIPT.replace("  4,\n  (const) [", "  4,\n  7, 8,\n  (const) ["),
            SCRIPT.replace('(const) ["storage", "opacity"]', "5"),
            SCRIPT.replace('(const) ["storage", "opacity"]', "[1]"),
            SCRIPT.replace("(const) [0, 1,", "(const) [null, 1,"),
            SCRIPT.replace("(const) [0, 1,", "7, (const) [0, 1,"),
            SCRIPT.replace('"name" => "L"', '"name" => 3'),
            SCRIPT.replace('"timeline" => (const) [', '"timeline" => 5, "x" => (const) ['),
        ]
        for case in cases:
            self.assertNotEqual(case, SCRIPT)
            with self.assertRaises(tool.TimelineError) as caught:
                tool.summarize(case)
            self.assertNotIn("character", str(caught.exception))

    def test_candidates_are_marked_heuristic(self):
        s = tool.summarize(SCRIPT)
        self.assertIn("heuristic", s["storageCandidatesNote"])
        self.assertNotIn("storages", s["scenes"][0]["objects"][0])


if __name__ == "__main__":
    unittest.main()
