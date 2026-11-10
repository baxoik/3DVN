import io
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from import_gsenjou import Assets, Converter, write_game


class GSenjouOverlayTests(unittest.TestCase):
    def test_write_game_generates_dialogue_theme_assets_and_config(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "game"
            write_game(str(output), {}, SimpleNamespace(characters={}))

            expected_sizes = {
                "textbox.png": (400, 90),
                "nvl_textbox.png": (384, 224),
            }
            for name, size in expected_sizes.items():
                with Image.open(output / "ui" / name) as image:
                    self.assertEqual(image.size, size)
                    self.assertEqual(image.mode, "RGBA")
                    self.assertGreater(image.getpixel((size[0] // 2, size[1] // 2))[3], 0)

            config = (output / "game.3vc").read_text(encoding="utf-8")
            self.assertIn("textbox_img textbox.png", config)
            self.assertNotIn("namebox_img", config)
            self.assertIn("nvl_textbox_img nvl_textbox.png", config)
            self.assertIn("textbox_y 150", config)
            self.assertIn("face_box 1", config)
            with Image.open(output / "ui" / "textbox.png") as image:
                self.assertEqual(image.getpixel((200, 45)), (5, 6, 8, 160))

    def test_cutin_uses_visible_art_origin_before_scene_clipping(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "source" / "Img-Sound Files"
            source.mkdir(parents=True)
            output = root / "output"
            image = Image.new("RGBA", (800, 700), (0, 0, 0, 0))
            draw = ImageDraw.Draw(image)
            draw.rectangle((344, 0, 799, 699), fill=(255, 0, 0, 255))
            draw.rectangle((650, 0, 799, 699), fill=(0, 0, 255, 255))
            data = io.BytesIO()
            image.save(data, format="PNG")
            with zipfile.ZipFile(source / "foreground.zip", "w") as archive:
                archive.writestr("foreground/ev/ev_other_09d.png", data.getvalue())

            assets = Assets(str(root / "source"), str(output), True, False, 22050, 16000)
            try:
                overlay = assets.overlay("ev_other_09d", 343, -100)
                self.assertIsNotNone(overlay)
                self.assertEqual((overlay[1], overlay[2]), (0, 100))
                with Image.open(output / "sprites" / f"{overlay[0]}.png") as converted:
                    self.assertEqual(converted.size, (182, 240))
                    self.assertEqual(converted.getpixel((10, 100)), (255, 0, 0, 255))
                    self.assertEqual(converted.getpixel((170, 100)), (0, 0, 255, 255))
                self.assertEqual(round(40 + 0.4 * (343 + overlay[1])), 177)

                script_dir = root / "source" / "1-raw-scripts"
                script_dir.mkdir()
                (script_dir / "g01.ks").write_text(
                    "@cutin storage=ev_other_09d x=343 y=0 path=(343,-100,255)\n",
                    encoding="utf-8")
                body = Converter(str(root / "source"), assets).convert_file("g01")
                self.assertIn(f"show cut1 {overlay[0]} at 177 0 fade 0.4", body)
            finally:
                for archive in assets.zips.values():
                    archive.close()

    def test_cutins_crop_scene_and_explicit_clip_in_layer_coordinates(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "source" / "Img-Sound Files"
            source.mkdir(parents=True)
            output = root / "output"
            image = Image.new("RGBA", (800, 600), (255, 255, 255, 255))
            data = io.BytesIO()
            image.save(data, format="PNG")
            with zipfile.ZipFile(source / "foreground.zip", "w") as archive:
                archive.writestr("foreground/ev/example.png", data.getvalue())

            assets = Assets(str(root / "source"), str(output), True, False, 22050, 16000)
            try:
                clipped = assets.overlay("example", 0, 100, (0, 120, 800, 150))
                self.assertEqual(clipped[1:], (0, 0))
                with Image.open(output / "sprites" / f"{clipped[0]}.png") as converted:
                    self.assertEqual(converted.size, (320, 60))

                cropped = assets.overlay("example", -50, 500)
                self.assertEqual(cropped[1:], (50, 0))
                with Image.open(output / "sprites" / f"{cropped[0]}.png") as converted:
                    self.assertEqual(converted.size, (300, 40))
            finally:
                for archive in assets.zips.values():
                    archive.close()

    def test_gallery_reuses_full_cg_and_writes_matching_thumbnail(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "source" / "Img-Sound Files"
            source.mkdir(parents=True)
            output = root / "output"
            image = Image.new("RGB", (800, 600), (120, 40, 25))
            data = io.BytesIO()
            image.save(data, format="JPEG")
            with zipfile.ZipFile(source / "background.zip", "w") as archive:
                archive.writestr("background/ev/ev_sample.jpg", data.getvalue())

            assets = Assets(str(root / "source"), str(output), True, False, 22050, 16000)
            try:
                self.assertEqual(assets.gallery_assets(), 1)
                self.assertTrue((output / "bg/ev_sample.jpg").is_file())
                with Image.open(output / "gallery/thumbs/ev_sample.jpg") as thumb:
                    self.assertLessEqual(thumb.width, 112)
                    self.assertLessEqual(thumb.height, 78)
            finally:
                for archive in assets.zips.values():
                    archive.close()


if __name__ == "__main__":
    unittest.main()
