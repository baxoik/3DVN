"""Tests for tools/import_narcissu.py on made-up Majiro files (the game is not needed).

Run from the repository root:  python3 -m unittest tests.test_import_narcissu"""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import import_narcissu as nar  # noqa: E402


def op(code, payload=b""):
    return struct.pack("<H", code) + payload


def string(code, text):
    data = text.encode("cp932") + b"\0"
    return op(code, struct.pack("<H", len(data)) + data)


def call(h, argc):
    return op(0x810, struct.pack("<IIH", h, 0, argc))


def syscall(h, argc):
    return op(0x835, struct.pack("<IH", h, argc))


def mjo(code, encrypt=True):
    """A MajiroObj file: one function (main at 0), code optionally encrypted."""
    if encrypt:
        code = bytes(b ^ nar.KEY[i & 1023] for i, b in enumerate(code))
    sig = b"MajiroObjX1.000\0" if encrypt else b"MajiroObjV1.000\0"
    return sig + struct.pack("<III", 0, 10, 1) + struct.pack("<II", 0x1234, 0) + \
        struct.pack("<I", len(code)) + code


SCRIPT = b"".join([
    op(0x83a, struct.pack("<H", 1)),
    op(0x800, struct.pack("<i", 1600)), string(0x801, "Room01"), call(nar.PICTURE, 2),
    call(nar.APPLY, 0),
    string(0x801, "bgm01"), call(nar.MUSIC, 1),
    string(0x801, "hime_0001"), call(nar.VOICE, 1),
    string(0x840, "Himeko"), string(0x840, "「「\"Hello.\""), op(0x841), string(0x842, "p"),
    string(0x842, "w"),
    string(0x840, "First half,"), op(0x841), string(0x842, "n"),
    string(0x840, " second half."), op(0x841), string(0x842, "p"),
    op(0x800, struct.pack("<i", 500)), syscall(nar.SYS_WAIT, 1),
    string(0x801, "se\\door"), syscall(nar.SYS_SOUND, 1),
    string(0x801, "b"), call(nar.PICTURE, 1), call(nar.APPLY, 0),
    syscall(sorted(nar.SYS_MUSIC_STOP)[0], 0),
    op(0x800, struct.pack("<i", 0)), op(0x82b),
])

NAMES = {"pic": {"room01"}, "voice": {"hime_0001"}, "music": {"bgm01"}, "sound": {"door"}}


class MajiroTests(unittest.TestCase):
    def test_decrypt_and_disassemble(self):
        for encrypt in (True, False):
            main, code = nar.load_mjo(mjo(SCRIPT, encrypt))
            self.assertEqual(main, 0)
            ops = nar.disassemble(code)
            self.assertIn((nar.TEXT, "Himeko"), [(o, a) for _, o, a in ops])
            self.assertEqual(ops[-1][1], 0x82b)

    def test_translate(self):
        tr = nar.ChapterTranslator(NAMES)
        out = tr.translate(mjo(SCRIPT))
        self.assertEqual(out, [
            "bgload room01.png 96",
            "music bgm01.ogg",
            "sound hime_0001.ogg",
            'text [Himeko] "Hello."',
            "cleartext",
            "text @First half,",
            "text second half.",
            "delay 30",
            "sound door.ogg",
            "bgload special/black.png",
            "music ~",
        ])
        self.assertEqual(tr.warnings, [])

    def test_archive(self):
        files = {"a.png": b"PNGDATA", "Voice_01.ogg": b"OGG"}
        names = b"".join(n.encode() + b"\0" for n in files)
        count = len(files)
        names_at = 28 + count * 16
        data_at = names_at + len(names)
        table, blob, at = b"", b"", data_at
        for data in files.values():
            table += struct.pack("<QII", 0, at, len(data))
            blob += data
            at += len(data)
        with tempfile.NamedTemporaryFile(suffix=".arc", delete=False) as f:
            f.write(b"MajiroArcV3.000\0" + struct.pack("<III", count, names_at, data_at) +
                    table + names + blob)
        try:
            arc = nar.MajiroArc(f.name)
            self.assertEqual(arc.read("voice_01.ogg"), b"OGG")
            assets = nar.ArcAssets([arc], {".png"}, plain=True)
            self.assertEqual(assets.read(assets.find("A.png")), b"PNGDATA")
            self.assertIsNone(assets.find("voice_01"))
            self.assertTrue(assets.read(assets.find("special/black.png")).startswith(b"\x89PNG"))
        finally:
            Path(f.name).unlink()

    def test_names(self):
        self.assertEqual([nar.asset_name(n) for n in ("se\\ele2", "2na.ogg", "Car1")],
                         ["ele2", "2na", "car1"])
        self.assertEqual(nar.syscall("wait"), 0xcf35f0e3)


if __name__ == "__main__":
    unittest.main()
