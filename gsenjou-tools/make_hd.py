#!/usr/bin/env python3
"""Build the hd/ folder for a converted G-Senjou game from full-size original images.

usage: make_hd.py GAME_DIR DECODED_DIR [--jobs N]

GAME_DIR     the converted game (bg/*.jpg 320x240, sprites/*.png at 0.4 scale)
DECODED_DIR  the game's original images as PNG (any folder layout; searched recursively),
             e.g. decoded from your own copy's bgimage / evimage / fgimage / face archives

For every background, CG, character sprite and face portrait the game uses, the original
is cropped the way the converter cropped it (backgrounds whole; sprites and faces to their
visible area) and written to GAME_DIR/hd/<same path> at full resolution. Each result is
checked against the existing small image (shrunk and compared) and skipped if it does not
match, so a wrong source never replaces a picture. 3DVN on PC and Wii U draws hd/ images
in place of the normal ones at the same size; the 3DS ignores hd/.
Cut-in images (cut_*) are left as they are.
"""
import argparse, concurrent.futures, os, sys
from PIL import Image, ImageChops, ImageStat

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("game")
parser.add_argument("decoded")
parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
parser.add_argument("--max-diff", type=float, default=14.0,
                    help="largest mean colour difference (0-255) accepted between shrunk HD and small image")
args = parser.parse_args()

sources = {}
for folder, _, names in os.walk(args.decoded):
    for name in names:
        if name.lower().endswith(".png"):
            sources.setdefault(name[:-4].lower(), os.path.join(folder, name))
print(f"{len(sources)} decoded originals found")


def difference(hd, small):
    """Mean per-channel difference after shrinking hd to the small image's size."""
    shrunk = hd.resize(small.size, Image.LANCZOS)
    if small.mode == "RGBA":
        # compare colours on black so transparent areas count the same in both
        back = Image.new("RGBA", small.size, (0, 0, 0, 255))
        a = Image.alpha_composite(back, shrunk.convert("RGBA")).convert("RGB")
        b = Image.alpha_composite(back, small.convert("RGBA")).convert("RGB")
    else:
        a, b = shrunk.convert("RGB"), small.convert("RGB")
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3.0


def job(item):
    kind, rel, source = item
    small_path = os.path.join(args.game, rel)
    target = os.path.join(args.game, "hd", rel)
    try:
        small = Image.open(small_path)
        small.load()
        image = Image.open(source)
        if kind == "bg":
            hd = image.convert("RGB")
        else:
            image = image.convert("RGBA")
            box = image.getchannel("A").getbbox()
            if box is None:
                return kind, "empty"
            hd = image.crop(box)
        if abs(hd.width / hd.height - small.width / small.height) > 0.03:
            return kind, "shape"
        if hd.width < small.width * 1.5:
            return kind, "not larger"
        if difference(hd, small) > args.max_diff:
            return kind, "different"
        os.makedirs(os.path.dirname(target), exist_ok=True)
        if kind == "bg":
            hd.save(target, "JPEG", quality=90)
        else:
            hd.save(target, "PNG", optimize=True)
        return kind, "ok"
    except Exception as error:   # unreadable file etc.: keep the small image
        return kind, f"error {type(error).__name__}"


items, missing = [], 0
for name in sorted(os.listdir(os.path.join(args.game, "bg"))):
    stem = os.path.splitext(name)[0].lower()
    if stem in sources:
        items.append(("bg", f"bg/{name}", sources[stem]))
    else:
        missing += 1
for name in sorted(os.listdir(os.path.join(args.game, "sprites"))):
    stem = os.path.splitext(name)[0].lower()
    if stem.startswith("cut_"):
        continue
    key = f"st_{stem[5:]}_f" if stem.startswith("face_") else f"st_{stem}"
    if key in sources:
        items.append(("face" if stem.startswith("face_") else "sprite", f"sprites/{name}", sources[key]))
    else:
        missing += 1

results = {}
with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
    for done, (kind, status) in enumerate(pool.map(job, items), 1):
        results.setdefault(kind, {}).setdefault(status, 0)
        results[kind][status] += 1
        if done % 500 == 0:
            print(f"  {done}/{len(items)}", flush=True)
for kind, counts in sorted(results.items()):
    print(f"{kind}: " + ", ".join(f"{status} {n}" for status, n in sorted(counts.items())))
print(f"no original found for {missing} images (they stay as they are)")
