import os
from io import BytesIO
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "recover_mahoyo_missing_images.py"


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix=".mahoyo-recovery-test-", dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.report = self.root / "conversion-report.txt"
        self.archive = self.root / "patch2.xp3"
        self.output = self.root / "recovered"

    def write_report(self, *names):
        self.report.write_text(
            "\n".join(f"scene.ks:1: background '{name}' not found" for name in names) + "\n",
            encoding="utf-8",
        )

    def create_archive(self, entries):
        with zipfile.ZipFile(self.archive, "w") as archive:
            for name, data in entries.items():
                archive.writestr(name, data)

    def make_tlg(self, width=3, height=2):
        return b"TLG5.0\x00raw\x1a" + struct.pack("<BIII", 4, width, height, 1)

    def fake_decoder(self):
        decoder = self.root / "decoder"
        decoder.write_text(
            f"#!{sys.executable}\n"
            "import sys\n"
            "from pathlib import Path\n"
            "from PIL import Image\n"
            "out = Path(next(a[6:] for a in sys.argv if a.startswith('--out=')))\n"
            "Image.new('RGBA', (3, 2), (1, 2, 3, 4)).save(out / 'input.png')\n",
            encoding="utf-8",
        )
        decoder.chmod(0o755)
        return decoder

    def run_tool(self, *extra):
        return subprocess.run(
            [sys.executable, str(TOOL), str(self.report), str(self.output),
             "--source", str(self.archive), *extra],
            capture_output=True,
            text=True,
            env=os.environ.copy(),
        )

    def test_recovers_exact_zip_images_and_decodes_tlg(self):
        jpeg = self.root / "candidate.jpg"
        Image.new("RGB", (4, 3), (10, 20, 30)).save(jpeg)
        self.write_report("ev-first", "ev-first", "ev-second", "unavailable")
        self.create_archive({
            "images/ev-first.jpg": jpeg.read_bytes(),
            "images/ev-second.tlg": self.make_tlg(),
            "images/ev-first-extra.jpg": jpeg.read_bytes(),
        })

        result = self.run_tool("--decoder", str(self.fake_decoder()))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Recovered 2 of 3 unique missing images (3 report occurrences)", result.stdout)
        self.assertEqual(
            {path.name for path in self.output.iterdir()},
            {"ev-first.jpg", "ev-second.png", "recovery-report.txt"},
        )
        with Image.open(self.output / "ev-second.png") as image:
            self.assertEqual((image.format, image.size, image.mode), ("PNG", (3, 2), "RGBA"))
        saved_report = (self.output / "recovery-report.txt").read_text(encoding="utf-8")
        self.assertIn("Unmatched unique image names: 1", saved_report)
        self.assertIn("unavailable", saved_report)

    def test_ambiguous_exact_matches_are_not_copied(self):
        jpeg = self.root / "candidate.jpg"
        Image.new("RGB", (4, 3)).save(jpeg)
        png = BytesIO()
        Image.new("RGB", (2, 2)).save(png, format="PNG")
        self.write_report("same-name")
        self.create_archive({
            "first/same-name.jpg": jpeg.read_bytes(),
            "second/same-name.png": png.getvalue(),
        })

        result = self.run_tool()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("0 of 1 unique missing images", result.stdout)
        self.assertEqual({path.name for path in self.output.iterdir()}, {"recovery-report.txt"})
        saved_report = (self.output / "recovery-report.txt").read_text(encoding="utf-8")
        self.assertIn("Ambiguous unique image names: 1", saved_report)

    def test_existing_output_and_missing_tlg_decoder_are_rejected(self):
        self.write_report("ev-second")
        self.create_archive({"ev-second.tlg": self.make_tlg()})

        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("require --decoder", result.stderr)
        self.assertFalse(self.output.exists())

        self.output.mkdir()
        marker = self.output / "keep.txt"
        marker.write_text("unchanged", encoding="utf-8")
        result = self.run_tool("--decoder", str(self.fake_decoder()))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("output already exists", result.stderr)
        self.assertEqual(marker.read_text(encoding="utf-8"), "unchanged")

    def test_rejects_output_inside_source_directory(self):
        source_dir = self.root / "source"
        source_dir.mkdir()
        Image.new("RGB", (2, 2)).save(source_dir / "ev-first.png")
        self.write_report("ev-first")
        output = source_dir / "recovered"

        result = subprocess.run(
            [sys.executable, str(TOOL), str(self.report), str(output),
             "--source", str(source_dir)],
            capture_output=True,
            text=True,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must not be inside source directory", result.stderr)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
