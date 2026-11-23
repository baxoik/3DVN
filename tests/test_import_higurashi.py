"""Tests for tools/import_higurashi.py on a small made-up Higurashi chapter.

Run from the repository root:  python3 -m unittest tests.test_import_higurashi"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "import_higurashi.py"

FLOW = """\
void main()
{
\tCallSection("BrandLogo");
\tJumpSection("Title");
}
void Game()
{
\tCallScript( "onik_op" );
\tCallSection("Day1");
}
void Day1()
{
\tCallScript( "onik_000" );
\tStoreValueToLocalWork( ChapterNumber, 1 );
\tCallSection( "ViewTips" );
\tJumpSection("Day2");
}
void Day2()
{
\tCallScript( "onik_001" );
\tSetGlobalFlag( GFlag_GameClear, TRUE );
\tJumpSection("Title");
}
"""

OP = """\
void main()
{
\tDrawScene( "Black", 400 );
\tOutputLine(NULL, "jp", NULL, "Opening.", Line_Normal);
\tClearMessage();
}
"""

DAY1 = """\
void main()
{
\tPlayBGM( 1, "Lsys22", 128, 0 );
\tDrawSceneWithMask( "bg_080", "m1", 0, 0, 3000 );
//\tcomment with "quotes" and ( parens
\tOutputLine(NULL, "jp", NULL, "First part, ", Line_ContinueAfterTyping);
\tOutputLine(NULL, "jp", NULL, "second part.", Line_WaitForInput);
\tOutputLineAll(NULL, "\\n", Line_ContinueAfterTyping);
\tDrawBustshot( 1, "Re_si_de_a1", -160, 0, 0, FALSE, 0, 0, 0, 0, 0, 0, 0, 0, 200, TRUE );
\tOutputLine(NULL, "jp", NULL, "\\"Hi!\\" <color=#fff>she</color> said.", Line_WaitForInput);
\tPlaySE( 3, "WA_002", 128, 64 );
\tShakeScreen( 1, 50, 20, 3, 0, );
\tFadeBustshot( 1, FALSE, 0, 0, 0, 0, 200, TRUE );
\tOutputLine(NULL, "jp", NULL, "Bye.", Line_Normal);
\tClearMessage();
\tFadeOutBGM( 1, 1000, TRUE );
}
"""

DAY2 = """\
void main()
{
\tChangeScene( "black", 8, 200, );
\tOutputLine(NULL, "jp", NULL, "The end.", Line_Normal);
}
"""


class ImportHigurashiTests(unittest.TestCase):
    def test_chapter(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        game, out = Path(temp.name) / "Higurashi", Path(temp.name) / "out"
        sa = game / "HigurashiEp01_Data" / "StreamingAssets"
        for folder in ("Scripts", "Update", "CG", "BGM", "SE"):
            (sa / folder).mkdir(parents=True)
        (sa / "Scripts" / "flow.txt").write_text(FLOW)
        (sa / "Scripts" / "onik_op.txt").write_text(OP)
        (sa / "Scripts" / "onik_000.txt").write_text("void main() { }")
        (sa / "Update" / "onik_000.txt").write_text(DAY1)        # Update/ wins
        (sa / "Scripts" / "onik_001.txt").write_text(DAY2)
        Image.new("RGB", (640, 480), (0, 0, 0)).save(sa / "CG" / "black.png")
        Image.new("RGB", (640, 480), (90, 120, 30)).save(sa / "CG" / "bg_080.png")
        Image.new("RGBA", (640, 480), (200, 0, 0, 128)).save(sa / "CG" / "re_si_de_a1.png")
        (sa / "BGM" / "lsys22.ogg").write_bytes(b"OggS music")
        (sa / "SE" / "wa_002.ogg").write_bytes(b"OggS sound")

        result = subprocess.run([sys.executable, "-I", str(TOOL), str(game), str(out), "--no-tips"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("3 story scripts", result.stdout)

        flow = (out / "ch" / "flow.3vs").read_text()
        self.assertLess(flow.index("script ch/onik_op"), flow.index("script ch/onik_000"))
        self.assertLess(flow.index("script ch/onik_000"), flow.index("script ch/onik_001"))
        self.assertTrue(flow.rstrip().endswith("stop"))

        day1 = (out / "ch" / "onik_000.3vs").read_text()
        self.assertIn("music bgm/lsys22", day1)                         # any case
        self.assertIn("hide *\nbg bg_080 fade 3.00", day1)
        self.assertIn('"First part, second part."', day1)               # joined
        self.assertIn('"\\n\\"Hi!\\" she said."', day1)                    # \n kept, tags gone
        self.assertIn("show L1 re_si_de_a1 at -40 0 fade 0.20", day1)   # 640 canvas -> 3DS
        self.assertIn("sfx se/wa_002", day1)
        self.assertIn("shake", day1)
        self.assertIn("hide L1 fade 0.20", day1)
        self.assertIn('"Bye."\npage', day1)
        self.assertIn("music stop", day1)
        self.assertTrue(day1.rstrip().endswith("script ch/flow"))
        self.assertIn("bg black fade 0.20", (out / "ch" / "onik_001.3vs").read_text())

        self.assertIn("default_display nvl", (out / "game.3vc").read_text())
        self.assertTrue((out / "audio" / "bgm" / "lsys22.ogg").is_file())
        with Image.open(out / "sprites" / "re_si_de_a1.png") as image:
            self.assertEqual(image.size, (320, 240))
        with Image.open(out / "hd" / "bg" / "bg_080.jpg") as image:
            self.assertEqual(image.size, (640, 480))


if __name__ == "__main__":
    unittest.main()
