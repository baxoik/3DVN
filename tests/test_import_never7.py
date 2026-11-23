"""Tests for tools/import_never7.py's script translation (the n7e.love itself is not needed).

Run from the repository root:  python3 -m unittest tests.test_import_never7"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import import_never7 as n7  # noqa: E402


class Sprites:
    """Stands in for LoveAssets: every sprite 300 units wide on the 800 canvas."""

    def canvas_width(self, name):
        return 300


DEC = """\
[00000010] fe: varop VAR_(28_0a)_a1_07 (14) := (00) 0
[00000020] fe: varop VAR_(28_0a)_a0_c8 (14) := (00) 1
[00000030] 10: bgload 00000000 BG01A 0 1
[00000040] 11: fgload 0 00000000 YU01AA 1 3
[00000050]  6: playBGM 20
[00000058]  b: playVoice L1D3A000
[00000060] ff: text 01
{textColor 0 0}【Makoto】 "Good morning.  How are you?" {delay 5}
{textColor 0 2}The sun is out.

[00000070] ff: text 02
{textColor 0 2}{choiceOpt 1 -> Smile. }{choiceOpt 2 -> Ask "Why?" }

[00000080]  3: choice v_a1_08 00 00
[00000090]  a: condJump 00 VAR_(28_1e)_a1_08 (28) != (15) (80)->0 (15) (00)  * 000000b5 (0002)
[000000a0]  5: call5 3 HCYARI
[000000b0]  7: goto7 000000ff (0003)
[000000b6]  a: condJump 00 VAR_(28_1e)_a1_0b (28) != (15) (80)->0 (15) (08) && VAR_(28_1e)_a1_0c (28) == (15) (81)->1 (15) (00)  * 00000010 (0004)
[000000c0]  4: jump staffroll
[000000d0] 22: movie end1
[000000e0]  4: jump L1D4A
"""


class TranslateTests(unittest.TestCase):
    def setUp(self):
        self.tr = n7.DecTranslator(Sprites(), "yl2d7", "pc", {"l1d4a": "script/l1d4a.dec"})
        self.out = self.tr.translate(DEC)

    def test_messages_whole_with_speaker(self):
        self.assertIn('text [Makoto] "Good morning. How are you?"', self.out)
        self.assertIn("text The sun is out.", self.out)

    def test_choice_and_variables(self):
        self.assertIn('choice Smile.|Ask "Why?"', self.out)
        self.assertEqual(self.out[self.out.index('choice Smile.|Ask "Why?"') + 1:][:2],
                         ["setvar v_a1_08 = selected", "setvar v_a1_08 - 1"])
        self.assertIn("setvar v_a1_07 = 0", self.out)
        self.assertIn("gsetvar v_a0_c8 = 1", self.out)            # route flag: global

    def test_jumps_and_labels(self):
        self.assertIn("label L00000010", self.out)
        self.assertIn("label L000000b5", self.out)                  # snapped to line b6
        self.assertLess(self.out.index("label L000000b5"),
                        self.out.index("if v_a1_0b != 0"))
        self.assertEqual(self.out[-1], "label L000000ff")           # past the end
        self.assertIn("jump l1d4a.scr", self.out)
        self.assertIn("text <<Bad End>>", self.out)                 # staffroll
        self.assertIn("jump hcyari.scr", self.out)                  # call and return
        self.assertIn('setvar RETFILE = "yl2d7.scr"', self.out)

    def test_conditions(self):
        at = self.out.index("if v_a1_08 != 0")
        self.assertEqual(self.out[at + 1], "    goto L000000b5")
        at = self.out.index("if v_a1_0b != 0")                      # && nests
        self.assertEqual(self.out[at + 1], "    if v_a1_0c == 1")

    def test_scene(self):
        self.assertIn("bgload bg01a.png", self.out)
        self.assertIn("setimg yu01aa.png 250 0", self.out)          # centre: 400 - 300/2
        self.assertIn("music track_21.ogg", self.out)               # BGM 20 -> track 21
        self.assertIn("sound l1d3a000.ogg", self.out)
        self.assertIn("video end_y", self.out)                      # Yuka's ending
        self.assertEqual(self.tr.movies["end_y"][0], "movie/psp/end_y.ogv")

    def test_variable_operations(self):
        self.assertEqual(self.tr.varop("varop VAR_(28_0a)_a1_48 (14) |= (00) VAR_(28_0a)_a0_cb".split()),
                         ["if v_a0_cb != 0", "    setvar v_a1_48 = 1", "fi"])
        self.assertEqual(self.tr.varop("varop VAR_(28_0a)_a1_0b (14) *= (00) 3".split()),
                         ["setvar _mul = v_a1_0b"] + ["setvar v_a1_0b + _mul"] * 2)

    def test_append_and_title(self):
        tr = n7.AppendTranslator(Sprites(), "c_yuka6", "pc")
        out = tr.translate("@text 【Yuka】 \"Hi.\" \nmultifgload2 00000000 YU02AAn 2 00000000 SA01AA 3\n"
                           "@choice A|B\n@setvar vap_seldt = selected\nreturnTitle\n")
        self.assertEqual(out[0], 'text [Yuka] "Hi."')
        self.assertIn("setimg yu02aan.png 50 0", out)
        self.assertIn("setimg sa01aa.png 450 0", out)
        self.assertEqual(out[out.index("choice A|B") + 1:][:2], ["setvar seldt = selected", "setvar seldt - 1"])
        self.assertEqual(out[-2:], ["text <<Bad End>>", "jump main.scr"])
        title = n7.title_script(True)
        self.assertIn("choice Start Game|Append Story", title)
        self.assertIn("    jump c_yuka0.scr", title)


if __name__ == "__main__":
    unittest.main()
