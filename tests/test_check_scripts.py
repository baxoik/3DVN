import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class ScriptCheckerTests(unittest.TestCase):
    def test_checks_entire_large_main_and_included_scripts(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            checker = folder / "check"
            subprocess.run(
                ["cc", "-o", str(checker), str(ROOT / "tests/check_scripts.c"),
                 str(ROOT / "source/gs.c"), "-lm"],
                check=True,
            )
            main = folder / "main.3vs"
            part = folder / "part.3vs"
            padding = "# padding\n" * 120000
            main.write_text(padding + '"end"\nstop\n', encoding="utf-8")
            result = subprocess.run([str(checker), str(main)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            main.write_text(padding + "bogus\n", encoding="utf-8")
            result = subprocess.run([str(checker), str(main)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("unknown command", result.stdout)
            main.write_text("include part.3vs\n", encoding="utf-8")
            part.write_text(padding + "bogus\n", encoding="utf-8")
            result = subprocess.run([str(checker), str(main)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("unknown command", result.stdout)


if __name__ == "__main__":
    unittest.main()
