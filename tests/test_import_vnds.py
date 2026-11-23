"""Tests for tools/import_vnds.py on a small made-up VNDS game.

Run from the repository root:  python3 -m unittest tests.test_import_vnds
The converted game can also be walked with tests/walk_story.c (see that file)."""
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "import_vnds.py"

MAIN = """\
bgload room.png
text Welcome.
text @First half,
text second half.
setvar name = "Mira"
text Hello $name and {$name}.
text [Sora] "Hi."
text Girl "Can I ask?"
text Narration with "quotes" stays narration.
setvar pick = 0
choice One|Two|Three|Four|Five|Six|Seven|Eight|Nine|Ten
setvar pick = selected
setvar copy = pick
if copy == 10
  text You took the last one.
fi
setvar picture = "face"
setimg {$picture}.png 10 20
setimg nocoords.png
gsetvar seen = 1
setvar back = "main.scr"
setvar where = "again"
jump other.scr middle
"""

OTHER = """\
text Start of other.
label middle
text Middle of other.
jump $back $where
"""

MAIN_END = """\
label again
text Back again.
jump main.scr
"""


class ImportVndsTests(unittest.TestCase):
    def convert(self, *extra):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        src, out = Path(temp.name) / "Game", Path(temp.name) / "out"
        (src / "script").mkdir(parents=True)
        (src / "script" / "main.scr").write_text(MAIN + MAIN_END)
        (src / "script" / "other.scr").write_text(OTHER)
        (src / "img.ini").write_text("width=640\nheight=480\n")
        (src / "info.txt").write_text("title=Test Game\n")
        (src / "background").mkdir()
        Image.new("RGB", (640, 480), (10, 20, 30)).save(src / "background" / "room.png")
        # foreground only in a zip that keeps the folder name, as many VNDS releases do
        with zipfile.ZipFile(src / "foreground.zip", "w") as z:
            for name in ("face.png", "nocoords.png"):
                path = Path(temp.name) / name
                Image.new("RGBA", (100, 200), (200, 0, 0, 128)).save(path)
                z.write(path, "foreground/" + name)
        result = subprocess.run([sys.executable, "-I", str(TOOL), str(src), str(out), *extra],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return out, result.stdout

    def test_scripts(self):
        out, log = self.convert("--no-audio", "--speakers")
        main = (out / "ch" / "main.3vs").read_text()
        other = (out / "ch" / "other.3vs").read_text()

        self.assertIn('"First half,\\nsecond half."', main)          # "text @" joins
        self.assertIn('set v_name = "Mira"', main)                    # quotes stripped
        self.assertIn('"Hello {v_name} and {v_name}."', main)         # $name in text
        self.assertIn('c_sora: "\\"Hi.\\""', main)                     # speaker forms
        self.assertIn('c_girl: "\\"Can I ask?\\""', main)
        self.assertIn('"Narration with \\"quotes\\" stays narration."', main)
        self.assertIn('character c_girl "Girl"', (out / "characters.3vs").read_text())
        self.assertIn('choice "More..."', main)                       # 10 options paged
        self.assertIn("set v_selected = 10", main)
        self.assertIn("set v_pick = v_selected", main)                # bare word = variable
        self.assertIn("set v_copy = v_pick", main)
        self.assertIn('set v_picture = "face"', main)                 # file-name value
        self.assertIn("show s_picture {v_picture} at", main)
        self.assertIn("show s_nocoords nocoords at 40 0", main)       # VNDS 0 0, centred
        self.assertIn("set g_seen = 1", main)                         # gsetvar -> global
        self.assertIn('set v_back = "main"', main)                    # script name value
        self.assertIn("script ch/other", main)
        self.assertIn('if _vnds_entry == "middle"', other)            # entry from main
        self.assertIn("set _vnds_entry = v_where", other)             # call and return
        self.assertIn("script ch/{v_back}", other)
        self.assertIn('if _vnds_entry == "again"', main)              # dynamic label target
        self.assertTrue(main.rstrip().endswith("stop"))
        self.assertNotIn("script ch/main", main)                      # jump main.scr ends it
        self.assertIn("scale 0.5", log)

        self.assertEqual((out / "game.3vc").read_text().splitlines()[0], "title Test Game")
        with Image.open(out / "bg" / "room.jpg") as image:
            self.assertEqual(image.size, (320, 240))
        with Image.open(out / "hd" / "bg" / "room.jpg") as image:             # original kept
            self.assertEqual(image.size, (640, 480))
        with Image.open(out / "sprites" / "face.png") as image:               # from the zip
            self.assertEqual((image.size, image.mode), ((50, 100), "RGBA"))

    def test_nvl(self):
        out, _ = self.convert("--no-audio", "--nvl")
        self.assertIn("default_display nvl", (out / "game.3vc").read_text())
        self.assertIn('"Welcome.\\n"', (out / "ch" / "main.3vs").read_text())


if __name__ == "__main__":
    unittest.main()
