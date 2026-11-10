import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
IMPORTER = ROOT / "tools" / "import_kirikiri.py"


class ImporterTestCase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.output = self.root / "output"

    def convert(self, text, *args):
        (self.source / "test.ks").write_text(text, encoding="utf-16")
        return subprocess.run(
            [sys.executable, str(IMPORTER), str(self.source), str(self.output),
             "--scene", "test.ks", *args],
            capture_output=True, text=True,
        )


class KiriKiriImportTests(ImporterTestCase):
    def test_strict_supported_text_and_timing(self):
        result = self.convert('@bg storage=black\nHello[r]world[l]\n@wait time=1000\n@pg\n@return\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        script = (self.output / "main.3vs").read_text()
        self.assertIn('bg black\n"Hello\\nworld"\nwait 1\n', script)
        self.assertTrue(script.endswith("stop\n"))
        subprocess.run(["gcc", "-o", str(self.root / "check"),
                        str(ROOT / "tests/check_scripts.c"), str(ROOT / "source/gs.c")],
                       check=True, capture_output=True)
        check = subprocess.run([str(self.root / "check"), str(self.output / "main.3vs")],
                               capture_output=True, text=True)
        self.assertEqual(check.returncode, 0, check.stdout)

    def test_nvl_line_waits_keep_text_and_page_breaks_clear(self):
        result = self.convert("@r\nfirst[l][r]\n@r\nsecond[l] third\n@pg\n"
                              "next[p]\n", "--nvl-pages")
        self.assertEqual(result.returncode, 0, result.stderr)
        script = (self.output / "main.3vs").read_text().splitlines()
        self.assertEqual(script[1:], ['"\\nfirst"', '"\\n\\nsecond"', '" third"',
                                      "page", '"next"', "page", "stop"])

    def test_strict_failure_preview_report_and_page_limit(self):
        text = "@call target=*custom\nfirst\n@pg\n@fg storage=x\nsecond\n@pg\nthird\n"
        result = self.convert(text)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        result = self.convert(text, "--preview", "--pages", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        script = (self.output / "main.3vs").read_text()
        self.assertIn('"first"', script)
        self.assertIn('"second"', script)
        self.assertNotIn("third", script)
        report = (self.output / "conversion-report.txt").read_text()
        self.assertIn("test.ks:1: unsupported tag: call", report)
        self.assertIn("test.ks:4: unsupported tag: fg", report)

    def test_embedded_script_is_not_treated_as_dialogue(self):
        result = self.convert("@iscript\nsecret code\n@endscript\nhello\n@return\n", "--preview")
        self.assertEqual(result.returncode, 0, result.stderr)
        script = (self.output / "main.3vs").read_text()
        self.assertNotIn("secret code", script)
        self.assertIn('"hello"', script)
        self.assertIn("embedded TJS", (self.output / "conversion-report.txt").read_text())

    def test_utf16_zip_and_existing_output(self):
        archive = self.root / "scripts.xp3"
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr("opening.ks", "hello[pg]".encode("utf-16"))
        command = [sys.executable, str(IMPORTER), str(archive), str(self.output),
                   "--scene", "opening.ks"]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('"hello"', (self.output / "main.3vs").read_text())
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)

    def test_explicit_scene_sequence_is_preview_only_and_preserves_order(self):
        (self.source / "first.ks").write_text("first[pg]\n@return\nignored", encoding="utf-16")
        (self.source / "second.ks").write_text("@unknown\nsecond[pg]\n@return\n", encoding="utf-16")
        command = [sys.executable, str(IMPORTER), str(self.source), str(self.output),
                   "--scene", "first.ks", "--scene", "second.ks"]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("multiple scenes require --preview", result.stderr)
        self.assertFalse(self.output.exists())
        result = subprocess.run(command + ["--preview", "--pages", "1"],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        result = subprocess.run(command + ["--preview"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        script = (self.output / "main.3vs").read_text()
        self.assertLess(script.index('"first"'), script.index('"second"'))
        self.assertNotIn("ignored", script)
        self.assertEqual(script.splitlines().count("stop"), 1)
        self.assertTrue(script.endswith("stop\n"))
        report = (self.output / "conversion-report.txt").read_text()
        self.assertIn("scene order was supplied manually", report)
        self.assertIn("first.ks", report)
        self.assertIn("second.ks:1: unsupported tag: unknown", report)
        subprocess.run(["gcc", "-o", str(self.root / "check"),
                        str(ROOT / "tests/check_scripts.c"), str(ROOT / "source/gs.c")],
                       check=True, capture_output=True)
        checked = subprocess.run([str(self.root / "check"), str(self.output / "main.3vs")],
                                 capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_sequence_failure_does_not_publish_partial_output(self):
        (self.source / "first.ks").write_text("first[pg]", encoding="utf-16")
        result = subprocess.run(
            [sys.executable, str(IMPORTER), str(self.source), str(self.output),
             "--scene", "first.ks", "--scene", "missing.ks", "--preview"],
            capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("scene not found", result.stderr)
        self.assertFalse(self.output.exists())

    def test_scene_sequence_reuses_shared_asset(self):
        assets = self.root / "assets"
        assets.mkdir()
        Image.new("RGB", (100, 100)).save(assets / "scene.png")
        for scene in ("first.ks", "second.ks"):
            (self.source / scene).write_text("@bg storage=scene\n@return\n", encoding="utf-16")
        result = subprocess.run(
            [sys.executable, str(IMPORTER), str(self.source), str(self.output),
             "--scene", "first.ks", "--scene", "second.ks", "--preview",
             "--assets", str(assets), "--source-size", "100x100"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        backgrounds = [line for line in (self.output / "main.3vs").read_text().splitlines()
                       if line.startswith("bg ")]
        self.assertEqual(len(backgrounds), 2)
        self.assertEqual(backgrounds[0], backgrounds[1])
        self.assertEqual(len(list((self.output / "bg").glob("*.png"))), 1)

    def test_multibyte_text_is_not_truncated(self):
        result = self.convert(("語" * 250) + "[pg]")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = (self.output / "main.3vs").read_text().splitlines()
        self.assertEqual("".join(line.strip('"') for line in lines if line.startswith('"')), "語" * 250)

    def test_asset_conversion_and_staged_background(self):
        assets = self.root / "assets"
        assets.mkdir()
        Image.new("RGB", (1024, 576)).save(assets / "scene.png")
        result = self.convert("@bg page=back storage=scene\nbefore[l]\n@trans time=100\n"
                              "after[l]\n@return\n",
                              "--assets", str(assets), "--source-size", "1024x576", "--preview")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = (self.output / "main.3vs").read_text().splitlines()
        bg = next(line for line in lines if line.startswith("bg "))
        self.assertLess(lines.index('"before"'), lines.index(bg))
        self.assertLess(lines.index(bg), lines.index('"after"'))
        self.assertTrue(bg.endswith(" fade 0.1"))
        with Image.open(self.output / "bg" / (bg.split()[1] + ".png")) as image:
            self.assertEqual(image.size, (400, 225))
        self.assertIn("transition wait is skippable", (self.output / "conversion-report.txt").read_text())
        subprocess.run(["gcc", "-o", str(self.root / "check"),
                        str(ROOT / "tests/check_scripts.c"), str(ROOT / "source/gs.c")],
                       check=True, capture_output=True)
        checked = subprocess.run([str(self.root / "check"), str(self.output / "main.3vs")],
                                 capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_ambiguous_or_missing_assets_are_reported(self):
        assets = self.root / "assets"
        assets.mkdir()
        Image.new("RGB", (100, 100)).save(assets / "same.png")
        Image.new("RGB", (100, 100)).save(assets / "same.jpg")
        result = self.convert("@bg storage=same\n@return\n", "--assets", str(assets),
                              "--source-size", "100x100")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ambiguous", result.stdout)
        self.assertFalse(self.output.exists())

    def test_audio_conversion_and_music_stop(self):
        import wave
        if not shutil.which("ffmpeg"):
            self.skipTest("ffmpeg unavailable")
        assets = self.root / "assets"
        assets.mkdir()
        for name in ("theme", "effect"):
            with wave.open(str(assets / (name + ".wav")), "wb") as file:
                file.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
                file.writeframes(b"\0\0" * 800)
        result = self.convert("@play storage=theme\n@se storage=effect\n@playstop\n@return\n",
                              "--assets", str(assets))
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = (self.output / "main.3vs").read_text().splitlines()
        music = next(line[6:] for line in lines if line.startswith("music k_"))
        sound = next(line[4:] for line in lines if line.startswith("sfx k_"))
        for name, rate in ((music, 32000), (sound, 22050)):
            with wave.open(str(self.output / "audio" / (name + ".wav")), "rb") as file:
                self.assertEqual((file.getnchannels(), file.getsampwidth(), file.getframerate()),
                                 (1, 2, rate))
        self.assertIn("music stop", lines)

    def test_audio_is_detected_by_content_when_extension_is_misleading(self):
        import wave
        if not shutil.which("ffmpeg"):
            self.skipTest("ffmpeg unavailable")
        assets = self.root / "assets"
        assets.mkdir()
        source = assets / "theme.ogg"
        encoded = subprocess.run(
            ["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=0.1",
             "-c:a", "aac", "-f", "mp4", str(source)],
            capture_output=True, text=True,
        )
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        result = self.convert("@play storage=theme\n@return\n", "--assets", str(assets))
        self.assertEqual(result.returncode, 0, result.stderr)
        music = next(line[6:] for line in (self.output / "main.3vs").read_text().splitlines()
                     if line.startswith("music k_"))
        with wave.open(str(self.output / "audio" / (music + ".wav")), "rb") as file:
            self.assertEqual((file.getnchannels(), file.getsampwidth(), file.getframerate()),
                             (1, 2, 32000))

    def test_music_stop_fade_and_unsupported_options(self):
        result = self.convert("@playstop time=10000 nowait=1\n@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("music stop fade 10\n", (self.output / "main.3vs").read_text())

    def test_music_stop_unknown_timing_is_reported(self):
        result = self.convert("@playstop time=&dynamic nowait=0\n@return\n", "--preview")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = (self.output / "conversion-report.txt").read_text()
        self.assertIn("music stop time must be a literal", report)
        self.assertIn("music stop options approximated", report)

    def test_bad_image_does_not_leave_partial_output(self):
        assets = self.root / "assets"
        assets.mkdir()
        (assets / "broken.png").write_bytes(b"not an image")
        result = self.convert("@bg storage=broken\n@return\n", "--assets", str(assets),
                              "--source-size", "100x100")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("conversion failed", result.stderr)
        self.assertFalse(self.output.exists())

    def test_oversized_background_is_reported_but_partial_crop_can_be_used(self):
        assets = self.root / "assets"
        assets.mkdir()
        Image.new("RGB", (500, 100), (255, 0, 0)).save(assets / "large.png")
        script = ("@bg storage=large\n"
                  "@partbg storage=large srcleft=0 srctop=0 "
                  "width=100 height=100 center=50 bgstorage=black\n"
                  "text[l]\n@return\n")
        options = ("--assets", str(assets), "--source-size", "100x100")
        result = self.convert(script, *options)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        result = self.convert(script, *options, "--preview")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = (self.output / "conversion-report.txt").read_text()
        self.assertIn("scaled image exceeds 1024x1024", report)
        backgrounds = list((self.output / "bg").glob("*.png"))
        self.assertEqual(len(backgrounds), 1)
        with Image.open(backgrounds[0]) as image:
            self.assertEqual(image.size, (400, 240))

    def test_thumbnail_use_requires_preview_and_is_reported(self):
        assets = self.root / "assets"
        assets.mkdir()
        Image.new("RGB", (168, 91)).save(assets / "sky.png")
        options = ("--assets", str(assets), "--source-size", "168x91", "--thumbnail-assets")
        result = self.convert("@bg storage=sky\n@return\n", *options)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        result = self.convert("@bg storage=sky\n@return\n", *options, "--preview")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("low-resolution thumbnails",
                      (self.output / "conversion-report.txt").read_text())

    def test_partial_background_composite_and_reused_background(self):
        assets = self.root / "assets"
        assets.mkdir()
        image = Image.new("RGB", (200, 100), (255, 0, 0))
        image.save(assets / "sky.png")
        script = ("@bg storage=sky\n@partbg storage=sky srcleft=100 srctop=0 "
                  "width=100 height=100 center=150 bgstorage=black\ntext[l]\n@return\n")
        result = self.convert(script, "--assets", str(assets), "--source-size", "200x100",
                              "--preview")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = (self.output / "main.3vs").read_text().splitlines()
        names = [line.split()[1] for line in lines if line.startswith("bg ")]
        self.assertEqual(len(names), 2)
        self.assertTrue((self.output / "bg" / (names[0] + ".png")).is_file())
        with Image.open(self.output / "bg" / (names[1] + ".png")) as composite:
            self.assertEqual(composite.size, (400, 240))
            self.assertEqual(composite.getpixel((50, 120)), (0, 0, 0))
            self.assertEqual(composite.getpixel((300, 120)), (255, 0, 0))
        self.assertIn("static image", (self.output / "conversion-report.txt").read_text())
        subprocess.run(["gcc", "-o", str(self.root / "check"),
                        str(ROOT / "tests/check_scripts.c"), str(ROOT / "source/gs.c")],
                       check=True, capture_output=True)
        checked = subprocess.run([str(self.root / "check"), str(self.output / "main.3vs")],
                                 capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_partial_thumbnail_requires_original_canvas(self):
        assets = self.root / "assets"
        assets.mkdir()
        Image.new("RGB", (168, 91), (255, 0, 0)).save(assets / "sky.png")
        script = "@partbg storage=sky srcleft=512 width=512 height=576 center=768 bgstorage=black\n@return\n"
        args = ("--assets", str(assets), "--source-size", "168x91", "--thumbnail-assets", "--preview")
        result = self.convert(script, *args)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("requires --scene-size", (self.output / "conversion-report.txt").read_text())

    def test_partial_thumbnail_maps_original_coordinates(self):
        assets = self.root / "assets"
        assets.mkdir()
        Image.new("RGB", (168, 91), (0, 255, 0)).save(assets / "sky.png")
        script = ("@partbg storage=sky srcleft=512 width=512 height=576 "
                  "center=768 bgstorage=white\n@return\n")
        result = self.convert(script, "--assets", str(assets), "--source-size", "168x91",
                              "--scene-size", "1024x576", "--thumbnail-assets", "--preview")
        self.assertEqual(result.returncode, 0, result.stderr)
        line = next(line for line in (self.output / "main.3vs").read_text().splitlines()
                    if line.startswith("bg "))
        with Image.open(self.output / "bg" / (line[3:] + ".png")) as composite:
            self.assertEqual(composite.getpixel((100, 120)), (255, 255, 255))
            self.assertEqual(composite.getpixel((300, 120)), (0, 255, 0))

    def test_staged_fgact_loop_converts_and_uses_each_image(self):
        assets = self.root / "assets"
        assets.mkdir()
        for name, color in (("sky", "red"), ("sky_b", "green"), ("sky_c", "blue")):
            Image.new("RGB", (1024, 576), color).save(assets / (name + ".png"))
        first = ("@fgact textoff=0 page=back props=-storage,center,vcenter,opacity,-visible "
                 "keys=(0,0,n,sky_b,512,288,0,1)(450,,l,,,,,)(600,,n,,,,255,)"
                 "(1050,,l,,,,,)(1200,,n,,,,0,)(1800,,,,,,,) loop=1 storage=sky_b\n")
        second = ("@fgact textoff=0 page=back props=-storage,center,vcenter,absolute,opacity,-visible "
                  "keys=(0,0,n,sky_c,512,288,1100,0,1)(1050,,l,,,,,,)"
                  "(1200,,n,,,,,255,)(1650,,l,,,,,,)(1800,,n,,,,,0,) loop=1 storage=sky_c\n")
        script = "@bg page=back storage=sky\n" + first + second + "@trans\nhello[l]\n@return\n"
        result = self.convert(script, "--assets", str(assets), "--source-size", "1024x576", "--preview")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = (self.output / "main.3vs").read_text().splitlines()
        loop = next(line for line in lines if line.startswith("bgloop ")).split()
        self.assertEqual(loop[-2:], ["0.45", "0.15"])
        self.assertLess(lines.index(" ".join(loop)), lines.index('"hello"'))
        self.assertEqual(len(set(loop[1:4])), 3)
        for name, color in zip(loop[1:4], ((255, 0, 0), (0, 128, 0), (0, 0, 255))):
            with Image.open(self.output / "bg" / (name + ".png")) as image:
                self.assertEqual(image.size, (400, 225))
                self.assertEqual(image.getpixel((200, 100)), color)
        self.assertIn("image loop approximated", (self.output / "conversion-report.txt").read_text())
        subprocess.run(["gcc", "-o", str(self.root / "check"),
                        str(ROOT / "tests/check_scripts.c"), str(ROOT / "source/gs.c")],
                       check=True, capture_output=True)
        checked = subprocess.run([str(self.root / "check"), str(self.output / "main.3vs")],
                                 capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_crossfade_timing_and_nowait(self):
        script = ("@bg storage=white time=1200 rule=crossfade nowait=0\n"
                  "@bg storage=black time=5000 rule=crossfade nowait=1\n"
                  "hello[l]\n@return\n")
        result = self.convert(script, "--preview")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = (self.output / "main.3vs").read_text().splitlines()
        self.assertIn("bg white fade 1.2", lines)
        self.assertIn("waitbg skippable", lines)
        self.assertIn("bg black fade 5", lines)
        self.assertEqual(lines.count("waitbg skippable"), 1)

    def test_wait_for_in_progress_transition(self):
        result = self.convert("@wt canskip=0\n@wt canskip=1\n@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("waitbg\nwaitbg skippable\n", (self.output / "main.3vs").read_text())

    def test_staged_loop_fade_and_unsupported_transition(self):
        assets = self.root / "assets"
        assets.mkdir()
        for name in ("sky", "sky_b", "sky_c"):
            Image.new("RGB", (1024, 576)).save(assets / (name + ".png"))
        script = ("@bg page=back storage=sky\n"
                  "@fgact textoff=0 page=back props=-storage,center,vcenter,opacity,-visible "
                  "keys=(0,0,n,sky_b,512,288,0,1)(450,,l,,,,,)(600,,n,,,,255,)"
                  "(1050,,l,,,,,)(1200,,n,,,,0,)(1800,,,,,,,) loop=1 storage=sky_b\n"
                  "@fgact textoff=0 page=back props=-storage,center,vcenter,absolute,opacity,-visible "
                  "keys=(0,0,n,sky_c,512,288,1100,0,1)(1050,,l,,,,,,)"
                  "(1200,,n,,,,,255,)(1650,,l,,,,,,)(1800,,n,,,,,0,) loop=1 storage=sky_c\n"
                  "@trans rule=other time=2000 nowait=1\nhello[l]\n@return\n")
        result = self.convert(script, "--assets", str(assets), "--source-size", "1024x576", "--preview")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = (self.output / "main.3vs").read_text().splitlines()
        self.assertTrue(any(line.startswith("bgloop ") and line.endswith("0.45 0.15 fade 2") for line in lines))
        self.assertNotIn("wait 2", lines)
        self.assertIn("unsupported transition rule", (self.output / "conversion-report.txt").read_text())

    def test_unsupported_fgact_does_not_create_loop(self):
        assets = self.root / "assets"
        assets.mkdir()
        Image.new("RGB", (1024, 576)).save(assets / "sky.png")
        script = ("@bg page=back storage=sky\n"
                  "@fgact page=back storage=sky loop=1 keys=(0,0,n,sky)\n"
                  "@trans\n@return\n")
        result = self.convert(script, "--assets", str(assets), "--source-size", "1024x576", "--preview")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("bgloop ", (self.output / "main.3vs").read_text())
        self.assertIn("unsupported fgact", (self.output / "conversion-report.txt").read_text())

    def test_incomplete_fgact_loop_is_reported(self):
        assets = self.root / "assets"
        assets.mkdir()
        Image.new("RGB", (1024, 576)).save(assets / "sky.png")
        script = ("@bg page=back storage=sky\n"
                  "@fgact textoff=0 page=back props=-storage,center,vcenter,opacity,-visible "
                  "keys=(0,0,n,sky_b,512,288,0,1)(450,,l,,,,,)(600,,n,,,,255,)"
                  "(1050,,l,,,,,)(1200,,n,,,,0,)(1800,,,,,,,) loop=1 storage=sky_b\n"
                  "@trans\n@return\n")
        result = self.convert(script, "--assets", str(assets), "--source-size", "1024x576", "--preview")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("bgloop ", (self.output / "main.3vs").read_text())
        self.assertIn("incomplete fgact image loop", (self.output / "conversion-report.txt").read_text())


class ForegroundCompositionTests(ImporterTestCase):
    # A 100x60 scene scales by exactly 4 onto the 400x240 screen, so pixels are easy to check.
    def setUp(self):
        super().setUp()
        self.assets = self.root / "assets"
        self.assets.mkdir()
        Image.new("RGB", (100, 108), (255, 0, 0)).save(self.assets / "room.png")
        Image.new("RGB", (10, 10), (0, 255, 0)).save(self.assets / "green.png")
        Image.new("RGB", (10, 10), (0, 0, 255)).save(self.assets / "blue.png")

    def compose(self, text, *extra):
        return self.convert(text, "--assets", str(self.assets), "--source-size", "100x60",
                            "--scene-size", "100x60", "--compose-foreground", "--preview", *extra)

    def screens(self):
        script = (self.output / "main.3vs").read_text().splitlines()
        return [line for line in script if line.startswith("bg c_")]

    def pixels(self, line):
        with Image.open(self.output / "bg" / (line.split()[1] + ".png")) as image:
            self.assertEqual(image.size, (400, 240))
            return image.convert("RGB").getpixel

    def report(self):
        return (self.output / "conversion-report.txt").read_text()

    def test_default_off_keeps_fg_unsupported(self):
        result = self.convert("@bg storage=room\n@fg storage=green center=50 vcenter=30 index=1000\n"
                              "@trans time=100\n@return\n", "--assets", str(self.assets),
                              "--source-size", "100x60", "--preview")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.screens(), [])
        self.assertIn("unsupported tag: fg", self.report())

    def test_requires_assets_and_source_size(self):
        result = self.convert("@return\n", "--compose-foreground")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--compose-foreground requires", result.stderr)

    def test_layers_are_composed_at_trans_with_default_base_placement(self):
        result = self.compose(
            "@bg page=back storage=room\n@fg storage=green center=50 vcenter=30 index=1000\n"
            "@fg storage=blue center=70 vcenter=30 index=1100 zoom=200\n"
            "@trans rule=crossfade time=500 nowait=0\nshown[l]\n@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        script = (self.output / "main.3vs").read_text()
        screen, = self.screens()
        self.assertTrue(screen.endswith(" fade 0.5"))
        self.assertLess(script.index(screen), script.index('"shown"'))
        pixel = self.pixels(screen)
        self.assertEqual(pixel((20, 20)), (255, 0, 0))   # base is top-aligned (100x108 image)
        self.assertEqual(pixel((180, 100)), (0, 255, 0))  # green starts at 45,25 -> 180,100
        self.assertEqual(pixel((219, 139)), (0, 255, 0))
        self.assertEqual(pixel((222, 120)), (255, 0, 0))
        # blue is zoomed 200% about its center: 20x20 at 60,20 -> 240..320 x 80..160.
        self.assertEqual(pixel((241, 81)), (0, 0, 255))
        self.assertEqual(pixel((318, 158)), (0, 0, 255))
        self.assertEqual(pixel((236, 120)), (255, 0, 0))
        self.assertEqual(pixel((300, 200)), (255, 0, 0))
        self.assertIn("foreground composed as a static image", self.report())

    def test_opacity_blends_and_edges_clip(self):
        result = self.compose(
            "@bg storage=room left=0 top=-48\n"
            "@fg storage=green center=0 vcenter=0 index=5 opacity=128\n"
            "@fg storage=blue center=98 vcenter=58 index=6\n@trans time=1\n@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        pixel = self.pixels(self.screens()[0])
        self.assertEqual(pixel((0, 0))[0], 127)
        self.assertEqual(pixel((0, 0))[1], 128)
        self.assertEqual(pixel((399, 239)), (0, 0, 255))

    def test_bg_clears_layers_unless_noclear_and_publishes_with_its_transition(self):
        result = self.compose(
            "@fg storage=green center=50 vcenter=30 index=1000\n"
            "@bg time=300 storage=room left=0 top=-48\n"      # clears: no composite
            "@fg storage=green center=50 vcenter=30 index=1000\n"
            "@bg time=400 rule=crossfade storage=room left=0 top=-48 noclear=1 noback=1\n"
            "@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        script = (self.output / "main.3vs").read_text().splitlines()
        self.assertEqual(len(self.screens()), 1)
        self.assertTrue(self.screens()[0].endswith(" fade 0.4"))
        self.assertTrue(any(line.startswith("bg k_") and line.endswith(" fade 0.3") for line in script))

    def test_clall_and_clfg_id_clear_before_publication(self):
        result = self.compose(
            "@bg storage=room left=0 top=-48\n"
            "@fg storage=green center=50 vcenter=30 index=1 id=a\n"
            "@fg storage=blue center=10 vcenter=10 index=2 id=b\n"
            "@clfg id=a\n@trans time=1\n"
            "@clall\n@trans time=1\n@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        first, second = self.screens()[0], self.screens()[1]
        self.assertEqual(self.pixels(first)((200, 120)), (255, 0, 0))   # green was cleared
        self.assertEqual(self.pixels(first)((40, 40)), (0, 0, 255))
        # The second transition replaces the sprite-bearing screen with the plain base.
        self.assertEqual(self.pixels(second)((40, 40)), (255, 0, 0))

    def test_id_replaces_existing_layer(self):
        result = self.compose(
            "@bg storage=room left=0 top=-48\n@fg storage=green center=50 vcenter=30 index=1 id=a\n"
            "@fg storage=blue center=10 vcenter=10 index=1 id=a\n@trans time=1\n@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        pixel = self.pixels(self.screens()[0])
        self.assertEqual(pixel((200, 120)), (255, 0, 0))
        self.assertEqual(pixel((40, 40)), (0, 0, 255))

    def test_transitioning_fg_publishes_with_default_300ms_and_copies_fore_first(self):
        result = self.compose(
            "@bg storage=room left=0 top=-48\n"
            "@fg storage=green center=50 vcenter=30 index=1 rule=crossfade\n"
            "@fg storage=blue center=10 vcenter=10 index=2 time=700\n@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        first, second = self.screens()
        self.assertTrue(first.endswith(" fade 0.3"))
        self.assertTrue(second.endswith(" fade 0.7"))
        # The second fg's backup restores the displayed page (with green) before adding blue.
        self.assertEqual(self.pixels(second)((200, 120)), (0, 255, 0))
        self.assertEqual(self.pixels(second)((40, 40)), (0, 0, 255))

    def test_unsupported_options_and_sprites_are_omitted_and_reported(self):
        for options in ("effect=tint", "blur=3", "rotate=5", "type=13", "brightness=-20",
                        "zoomx=-100", "opacity=300", "partbgid=x", "pos=c"):
            with self.subTest(options=options):
                if self.output.exists():
                    shutil.rmtree(self.output)
                result = self.compose(
                    "@bg storage=room left=0 top=-48\n"
                    f"@fg storage=green center=50 vcenter=30 index=1 {options}\n@trans time=1\n@return\n")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.screens(), [])
                self.assertIn("foreground layer not composed", self.report())

    def test_sprite_names_missing_files_and_missing_coordinates_are_omitted(self):
        Image.new("RGB", (10, 10)).save(self.assets / "ann(中).png")
        result = self.compose(
            "@bg storage=room left=0 top=-48\n"
            "@fg storage=ann(中) center=1 vcenter=1 index=1\n"
            "@fg storage=green|a center=1 vcenter=1 index=2\n"
            "@fg storage=missing center=1 vcenter=1 index=3\n"
            "@fg storage=green vcenter=1 index=4\n@trans time=1\n@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.screens(), [])
        report = self.report()
        self.assertIn("character sprites need composition metadata", report)
        self.assertIn("not found", report)
        self.assertIn("fg requires center", report)

    def test_unsupported_base_or_animation_prevents_composition(self):
        cases = ["@bg storage=room left=0 top=-48 zoom=120\n",
                 "@bg storage=room left=0 top=-48\n@bgact page=back props=left keys=(0,0,l,1)(10,,,2)\n",
                 "@bg storage=room left=0 top=-48\n@partbg storage=room srcleft=0 srctop=0 width=10 "
                 "height=10 center=5 bgstorage=black\n",
                 "@fg storage=green center=5 vcenter=5 index=1\n"]   # no background in this scene
        for body in cases:
            with self.subTest(body=body):
                if self.output.exists():
                    shutil.rmtree(self.output)
                fg = "" if "@fg" in body else "@fg storage=green center=5 vcenter=5 index=1\n"
                result = self.compose(body + fg + "@trans time=1\n@return\n")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.screens(), [])
                self.assertIn("foreground layers not composed", self.report())

    def test_fgact_on_a_layer_and_layer_mutating_tags_omit_it(self):
        for tag in ("@fgact page=back props=center keys=(0,0,l,5)(10,,,9) storage=green",
                    "@chgfg storage=green", "@movefg storage=green", "@layopt layer=0 opacity=0"):
            with self.subTest(tag=tag):
                if self.output.exists():
                    shutil.rmtree(self.output)
                result = self.compose(
                    "@bg storage=room left=0 top=-48\n"
                    f"@fg storage=green center=50 vcenter=30 index=1\n{tag}\n@trans time=1\n@return\n")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.screens(), [])
                self.assertIn("foreground layer not composed", self.report())

    def test_equal_indexes_and_unpublished_layers_are_reported(self):
        result = self.compose(
            "@bg storage=room left=0 top=-48\n@fg storage=green center=5 vcenter=5 index=7\n"
            "@fg storage=blue center=9 vcenter=9 index=7\n@trans time=1\n@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.screens(), [])
        self.assertIn("equal layer indexes", self.report())
        shutil.rmtree(self.output)
        result = self.compose("@bg storage=room left=0 top=-48\n"
                              "@fg storage=green center=5 vcenter=5 index=7\n@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("never made visible", self.report())

    def test_clall_with_unsupported_options_is_reported(self):
        result = self.compose("@bg storage=room left=0 top=-48\n@clall page=fore\n@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("unsupported clall options", self.report())

    def test_direct_fore_bg_does_not_republish_stale_back_composition(self):
        result = self.compose(
            "@bg storage=black\n@fg storage=green center=50 vcenter=30 index=1\n@trans time=1\n"
            "@bg storage=white page=fore\n@trans time=1\n@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        script = (self.output / "main.3vs").read_text().splitlines()
        self.assertEqual(len(self.screens()), 1)
        self.assertLess(script.index("bg white"), len(script))
        self.assertEqual(script.count(self.screens()[0]), 1)
        self.assertLess(script.index(self.screens()[0]), script.index("bg white"))
        self.assertEqual(len([line for line in script if line.startswith("bg c_")]), 1)
        self.assertIn("foreground back page invalidated", self.report())

    def test_layers_added_after_fore_bg_compose_on_the_new_base(self):
        result = self.compose(
            "@bg storage=black\n@bg storage=white page=fore\n@bg storage=white\n"
            "@fg storage=green center=50 vcenter=30 index=1\n@trans time=1\n@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.pixels(self.screens()[0])((5, 5)), (255, 255, 255))

    def test_layers_fully_outside_the_stage_are_skipped(self):
        for center, vcenter in ((-500, 30), (600, 30), (50, -500), (50, 400), (-500, -500)):
            with self.subTest(center=center, vcenter=vcenter):
                if self.output.exists():
                    shutil.rmtree(self.output)
                result = self.compose(
                    "@bg storage=black\n@fg storage=green center=%d vcenter=%d index=1\n"
                    "@trans time=1\n@return\n" % (center, vcenter))
                self.assertEqual(result.returncode, 0, result.stderr)
                pixel = self.pixels(self.screens()[0])
                self.assertEqual(pixel((200, 120)), (0, 0, 0))

    def test_generated_script_passes_host_checker(self):
        result = self.compose("@bg storage=room left=0 top=-48\n"
                              "@fg storage=green center=50 vcenter=30 index=1\n@trans time=100\n@return\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run(["gcc", "-o", str(self.root / "check"),
                        str(ROOT / "tests/check_scripts.c"), str(ROOT / "source/gs.c")],
                       check=True, capture_output=True)
        checked = subprocess.run([str(self.root / "check"), str(self.output / "main.3vs")],
                                 capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


if __name__ == "__main__":
    unittest.main()
