"""Tests for tools/import_higurashi.py on a small made-up Higurashi chapter.

Run from the repository root:  python3 -m unittest tests.test_import_higurashi"""
import shutil
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

    def test_07th_mod_full_patch(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        game = Path(temp.name) / "Higurashi"
        sa = game / "HigurashiEp01_Data" / "StreamingAssets"
        for folder in ("Scripts", "Update", "CG/sprite", "CG/background", "voice/ps3/s19/03", "BGM"):
            (sa / folder).mkdir(parents=True)
        (sa / "Scripts" / "flow.txt").write_text(FLOW)
        (sa / "Update" / "init.txt").write_text('void main() { SetScreenAspect("0.5625"); }')
        for name in ("onik_op", "onik_001"):
            (sa / "Scripts" / f"{name}.txt").write_text("void main() { }")
        (sa / "Update" / "zonik_x.txt").write_text(
            'void dialog001() { OutputLine(NULL, "jp", NULL, "Censored text.", Line_Normal); }')
        (sa / "Update" / "onik_000.txt").write_text("""void main()
{
	DrawScene( "background/gk1", 400 );
	if (GetGlobalFlag(GADVMode)) { OutputLine("<color=#5ec69a>魅音</color>", NULL, "<color=#5ec69a>Mion</color>", NULL, Line_ContinueAfterTyping); }
	ModPlayVoiceLS(3, 3, "ps3/s19/03/990300034", 256, TRUE);
	ModDrawCharacter(3, 3, "sprite/me1a_def_a1_", "0", 160, 0, 0, FALSE, 0, 0, 0, 0, 0, 0, 0, 20, 300, TRUE );
	OutputLine(NULL, "jp", NULL, "\\"We made it!\\"", GetGlobalFlag(GLinemodeSp));
	if (GetGlobalFlag(GADVMode)) { ClearMessage(); } else { OutputLineAll(NULL, "\\n", Line_ContinueAfterTyping); }
	if(GetGlobalFlag(GCensor) >= 4){ModCallScriptSection("zonik_x","dialog001");}
	DrawBustshot( 3, "", 0, 0, 0, FALSE, 0, 0, 0, 0, 0, 0, 0, 0, 0, TRUE );
}
""")
        Image.new("RGB", (1920, 1080)).save(sa / "CG" / "background" / "gk1.png")
        Image.new("RGBA", (2560, 1440)).save(sa / "CG" / "sprite" / "me1a_def_a1_0.png")
        (sa / "voice" / "ps3" / "s19" / "03" / "990300034.ogg").write_bytes(b"OggS")

        def convert(*extra):
            out = Path(temp.name) / ("out" + "".join(extra))
            r = subprocess.run([sys.executable, "-I", str(TOOL), str(game), str(out), "--no-tips", *extra],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
            return out, (out / "ch" / "onik_000.3vs").read_text()

        out, nvl = convert()
        self.assertIn("bg background/gk1", nvl)
        self.assertIn("sfx voice/ps3/s19/03/990300034", nvl)
        self.assertIn("show L3 sprite/me1a_def_a1_0 at 75 8 fade 0.30", nvl)   # 16:9 canvas
        self.assertIn('c_mion: "\\"We made it!\\""', nvl)    # the name, for ADV display
        config = (out / "game.3vc").read_text()
        self.assertIn("default_display nvl", config)
        self.assertIn("nvl_names 0", config)                                   # not on NVL pages
        self.assertNotIn("Censored", nvl)                                      # level 2
        self.assertIn("hide L3", nvl)
        self.assertTrue((out / "characters.3vs").read_text().strip())          # never empty
        with Image.open(out / "sprites" / "sprite" / "me1a_def_a1_0.png") as image:
            self.assertEqual(image.size, (400, 225))
        with Image.open(out / "hd" / "sprites" / "sprite" / "me1a_def_a1_0.png") as image:
            self.assertEqual(image.size, (2048, 1152))                         # PC/Wii U limit

        out, adv = convert("--adv", "--censor", "4")
        self.assertIn('c_mion: "\\"We made it!\\""', adv)
        self.assertIn('character c_mion "Mion" #5ec69a', (out / "characters.3vs").read_text())
        self.assertIn('Censored text."', adv)                                   # inlined section
        self.assertIn("default_display adv", (out / "game.3vc").read_text())

    def test_choices_and_story_flags(self):
        """Himatsubushi's TIPS 9: a red/blue box choice, flags set and tested later;
        and a console-only choice that 07th-Mod's default settings leave out."""
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        game, out = Path(temp.name) / "Higurashi", Path(temp.name) / "out"
        sa = game / "HigurashiEp04_Data" / "StreamingAssets"
        for folder in ("Scripts", "CG"):
            (sa / folder).mkdir(parents=True)
        (sa / "Scripts" / "flow.txt").write_text(
            'void Game() { CallScript("hima_001"); JumpSection("Title"); }')
        (sa / "Scripts" / "hima_001.txt").write_text("""void main()
{
	if (GetGlobalFlag(GChoiceMode) > 0) { char Bad[2]; Bad[0] = "Console only"; Select(1, Bad); }
	char Item[2];
	if (GetGlobalFlag(GLanguage) == 1) { Item[0] = "Red box"; Item[1] = "Blue box"; }
	else { Item[0] = "JP red"; Item[1] = "JP blue"; }
	Select( 2, Item );
	if (LoadValueFromLocalWork( SelectResult ) == 0) {
		SetGlobalFlag(redbox , TRUE);
		CallSection( "red" );
	} else {
		SetGlobalFlag(bluebox , TRUE);
	}
	if(GetGlobalFlag(bluebox) == 1) { OutputLine(NULL, "jp", NULL, "It was blue.", Line_Normal); }
}
void red()
{
	OutputLine(NULL, "jp", NULL, "It was red.", Line_Normal);
}
""")
        result = subprocess.run([sys.executable, "-I", str(TOOL), str(game), str(out), "--no-tips"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        script = (out / "ch" / "hima_001.3vs").read_text()
        self.assertNotIn("Console only", script)
        self.assertIn('menu\n  choice "Red box"\n    set hig_select = 0\n  choice "Blue box"\n'
                      '    set hig_select = 1\nend', script)
        self.assertIn("if hig_select == 0\nset hig_redbox = 1\n\"It was red.\"", script)
        self.assertIn("else\nset hig_bluebox = 1\nend", script)
        self.assertIn('if hig_bluebox == 1\n"It was blue."', script)
        walker = ROOT / "tests" / "walk_story.c"
        if shutil.which("cc"):
            exe = Path(temp.name) / "walk"
            subprocess.run(["cc", "-O1", f"-I{ROOT / 'source'}", str(walker), str(ROOT / "source" / "gs.c"),
                            str(ROOT / "source" / "wrap.c"), "-o", str(exe)], check=True)
            walk = subprocess.run([str(exe), str(out), "8", "1"], capture_output=True, text=True)
            self.assertIn("0 problems", walk.stdout + walk.stderr)


if __name__ == "__main__":
    unittest.main()
