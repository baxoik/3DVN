import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "decode_kirikiri_tlg.py"


class DecodeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix=".tlg-test-", dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / "output"
        self.decoder = self.root / "decoder"
        self.decoder.write_text(
            f"#!{sys.executable}\n"
            "import os, sys\n"
            "from PIL import Image\n"
            "from pathlib import Path\n"
            "mode = os.environ.get('FAKE_MODE', 'ok')\n"
            "out = Path(next(a[6:] for a in sys.argv if a.startswith('--out=')))\n"
            "if mode == 'error': sys.exit(1)\n"
            "if mode == 'missing': sys.exit(0)\n"
            "if mode == 'second-fails' and b'FAIL' in (out / 'input.tlg').read_bytes(): sys.exit(1)\n"
            "if mode == 'corrupt': (out / 'input.png').write_bytes(b'bad'); sys.exit(0)\n"
            "im = Image.new('RGB' if mode == 'no-alpha' else 'RGBA', "
            "(4, 2) if mode == 'wrong-size' else (3, 2), 0)\n"
            "im.save(out / 'input.png')\n"
        )
        self.decoder.chmod(0o755)
        raw = b"TLG5.0\x00raw\x1a" + struct.pack("<BIII", 4, 3, 2, 1)
        self.source = self.root / "立ち絵.tlg"
        self.source.write_bytes(b"TLG0.0\x00sds\x1a" + struct.pack("<I", len(raw)) + raw)
        self.original = self.source.read_bytes()

    def run_tool(self, *inputs, mode="ok"):
        return subprocess.run(
            [sys.executable, str(TOOL), "--decoder", str(self.decoder),
             "--output", str(self.output), *(str(p) for p in inputs)],
            capture_output=True, text=True, env={**os.environ, "FAKE_MODE": mode},
        )

    def test_batch_preserves_alpha_names_and_originals(self):
        second = self.root / "another.tlg"
        second.write_bytes(self.original)
        result = self.run_tool(self.source, second)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.source.read_bytes(), self.original)
        self.assertEqual(second.read_bytes(), self.original)
        self.assertEqual({p.name for p in self.output.iterdir()}, {"立ち絵.png", "another.png"})
        with Image.open(self.output / "立ち絵.png") as image:
            self.assertEqual((image.mode, image.size, image.getchannel("A").getextrema()),
                             ("RGBA", (3, 2), (0, 0)))

    def test_failures_leave_no_output_or_modified_input(self):
        for mode in ("error", "missing", "corrupt", "wrong-size", "no-alpha"):
            with self.subTest(mode=mode):
                result = self.run_tool(self.source, mode=mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("error:", result.stderr)
                self.assertFalse(self.output.exists())
                self.assertEqual(self.source.read_bytes(), self.original)

    def test_failure_after_first_conversion_does_not_publish_partial_batch(self):
        second = self.root / "second.tlg"
        second.write_bytes(self.original + b"FAIL")
        result = self.run_tool(self.source, second, mode="second-fails")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        self.assertEqual(self.source.read_bytes(), self.original)
        self.assertEqual(second.read_bytes(), self.original + b"FAIL")

    def test_invalid_inputs_and_existing_output(self):
        bad = self.root / "bad.tlg"
        bad.write_bytes(b"TLG0.0\x00sds\x1a\xff")
        result = self.run_tool(self.source, bad)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        duplicate = self.root / "立ち絵.TLG"
        duplicate.write_bytes(self.original)
        self.assertIn("duplicate", self.run_tool(self.source, duplicate).stderr)
        self.output.mkdir()
        marker = self.output / "existing"
        marker.write_text("unchanged")
        self.assertNotEqual(self.run_tool(self.source).returncode, 0)
        self.assertEqual(marker.read_text(), "unchanged")

    def test_tlg6_header(self):
        self.source.write_bytes(b"TLG6.0\x00raw\x1a" + bytes([4, 0, 0, 0])
                                + struct.pack("<III", 3, 2, 0))
        result = self.run_tool(self.source)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
