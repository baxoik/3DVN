#!/usr/bin/env python3
"""Build the hd/ folder for a converted G-Senjou game from full-size original images.

usage: make_hd.py GAME_DIR DECODED_DIR [--jobs N]

GAME_DIR     the converted game (bg/*.jpg 320x240, sprites/*.png at 0.4 scale)
DECODED_DIR  the game's original images as PNG (any folder layout; searched recursively),
             e.g. decoded from your own copy's bgimage / evimage / fgimage / face archives

For every background, CG, character sprite and face portrait the game uses, the area of
the original that the small image shows is found by template matching (scale and
position; the small images are reframed crops) and written to GAME_DIR/hd/<same path> at
full resolution. Each result must correlate with the small image (--min-match), so a
wrong source or a different composition never replaces a picture. 3DVN on PC and Wii U draws hd/ images
in place of the normal ones at the same size; the 3DS ignores hd/.
Cut-in images (cut_*) are left as they are.
"""
import argparse, concurrent.futures, os, sys
import numpy as np
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("game")
parser.add_argument("decoded")
parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
parser.add_argument("--min-redrawn", type=float, default=0.4,
                    help="minimum correlation for a background redrawn for widescreen; its closest "
                         "4:3 area is used (backgrounds only; all G-Senjou ones score 0.45+)")
parser.add_argument("--min-match", type=float, default=0.9,
                    help="minimum correlation (0..1) between the small image and the found crop of the "
                         "original, compared at quarter resolution; correct matches score 0.92+, "
                         "different compositions (e.g. zoomed-out 4:3 CGs) 0.81 or less")
args = parser.parse_args()

sources = {}
for folder, _, names in os.walk(args.decoded):
    for name in names:
        if name.lower().endswith(".png"):
            sources.setdefault(name[:-4].lower(), os.path.join(folder, name))
print(f"{len(sources)} decoded originals found")


def similarity(hd, small):
    """Correlation of the two pictures at quarter resolution. Unlike a plain difference
    this does not favour dark images and ignores slight misregistration of fine detail."""
    size = (max(8, small.width // 4), max(8, small.height // 4))
    a = planes(hd.convert("RGBA") if small.mode == "RGBA" else hd, size).ravel()
    b = planes(small, size).ravel()
    a, b = a - a.mean(), b - b.mean()
    norm = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / norm) if norm else 0.0


def planes(image, size=None):
    """float32 H x W x C: colours on black, plus alpha for images that have it."""
    if size:
        image = image.resize(size, Image.BOX if image.width > size[0] else Image.BICUBIC)
    if image.mode == "RGBA":
        array = np.asarray(image, np.float32)
        array[..., :3] *= array[..., 3:4] / 255.0
        return array
    return np.asarray(image.convert("RGB"), np.float32)


def match(big, small):
    """Best (ssd, x, y) of small inside big (both H x W x C) by FFT correlation."""
    bh, bw, _ = big.shape
    sh, sw, _ = small.shape
    if bh < sh or bw < sw:
        return None
    fh, fw = bh + sh, bw + sw
    corr = np.zeros((bh - sh + 1, bw - sw + 1))
    for c in range(big.shape[2]):
        f = np.fft.rfft2(big[..., c], (fh, fw)) * np.conj(np.fft.rfft2(small[..., c], (fh, fw)))
        corr += np.fft.irfft2(f, (fh, fw))[:bh - sh + 1, :bw - sw + 1]
    sq = np.pad((big ** 2).sum(axis=2), ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    window = sq[sh:, sw:] - sq[:-sh, sw:] - sq[sh:, :-sw] + sq[:-sh, :-sw]
    ssd = window - 2 * corr + (small ** 2).sum()
    y, x = np.unravel_index(np.argmin(ssd), ssd.shape)
    return ssd[y, x] / small.size, int(x), int(y)


def find_crop(hd, small):
    """Box in hd that shows what small shows: the 3DS images are scaled-down crops
    (other framing for 4:3, upper bodies of full-length sprites) of the originals.
    Searches scale and position at low resolution, then refines at full size."""
    k_max = min(hd.width / small.width, hd.height / small.height)
    if k_max < 1.5:
        return None
    f = 64 / max(small.size)                         # search resolution
    target = planes(small, (max(8, round(small.width * f)), max(8, round(small.height * f))))
    best = None
    k = k_max
    while k >= max(1.5, k_max * 0.55):
        size = (round(hd.width / k * target.shape[1] / small.width), round(hd.height / k * target.shape[0] / small.height))
        found = match(planes(hd, size), target)
        if found and (best is None or found[0] < best[0]):
            best = (found[0], k, found[1] * hd.width / size[0], found[2] * hd.height / size[1])
        k *= 0.97
    if best is None:
        return None
    _, k, x, y = best
    # refine: full small resolution, scales around the best one, positions near it
    target = planes(small)
    refined = None
    for kk in np.linspace(k * 0.975, k * 1.025, 7):
        if kk * small.width > hd.width or kk * small.height > hd.height:
            continue
        margin = 6 * kk
        left, top = max(0, x - margin), max(0, y - margin)
        right = min(hd.width, x + kk * small.width + margin)
        bottom = min(hd.height, y + kk * small.height + margin)
        region = hd.crop((round(left), round(top), round(right), round(bottom)))
        size = (round(region.width / kk), round(region.height / kk))
        found = match(planes(region, size), target)
        if found and (refined is None or found[0] < refined[0]):
            refined = (found[0], kk, left + found[1] * region.width / size[0], top + found[2] * region.height / size[1])
    if refined:
        _, k, x, y = refined
    w, h = k * small.width, k * small.height
    x, y = min(max(0, x), hd.width - w), min(max(0, y), hd.height - h)
    return round(x), round(y), round(x + w), round(y + h)


def _gray(image, size):
    return np.asarray(image.convert("L").resize(size, Image.BOX), np.float32)


def _corr(a, b):
    a, b = a - a.mean(), b - b.mean()
    norm = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / norm) if norm else 0.0


def _search(hd, small, base, scales, xs, ys, min_overlap=0.5):
    """Best (corr, scale, x, y): hd scaled by `scale` (small px per hd px) at (x, y) in the
    small image's frame; hd may stick out of the frame or leave parts of it uncovered."""
    f = base / small.width
    W, H = base, round(small.height * f)
    S = _gray(small, (W, H))
    best = None
    for scale in scales:
        w, h = round(hd.width * scale * f), round(hd.height * scale * f)
        if w < 8 or h < 8:
            continue
        T = _gray(hd, (w, h))
        for y in (ys(H, h, f) if callable(ys) else [round(v * f) for v in ys]):
            for x in (xs(W, w, f) if callable(xs) else [round(v * f) for v in xs]):
                x0, y0, x1, y1 = max(0, x), max(0, y), min(W, x + w), min(H, y + h)
                if x1 <= x0 or y1 <= y0 or (x1 - x0) * (y1 - y0) < min_overlap * W * H:
                    continue
                c = _corr(S[y0:y1, x0:x1], T[y0 - y:y1 - y, x0 - x:x1 - x])
                if best is None or c > best[0]:
                    best = (c, scale, x / f, y / f)
    return best


def place(hd, small):
    """For pictures reframed between versions: finds where the original lines up with the
    small image (coarse search, then two finer passes), and returns (canvas, coverage):
    the small image's frame at the original's resolution, the original blended in with
    soft edges, the uncovered border filled from the upscaled small image."""
    fit_w, fit_h = small.width / hd.width, small.height / hd.height
    span = lambda W, w, f: range(min(0, W - w), max(0, W - w) + 1)
    best = _search(hd, small, 96, np.geomspace(min(fit_w, fit_h) * 0.85, max(fit_w, fit_h) * 1.2, 28),
                   span, span)
    if best is None:
        return None
    for base, step in ((small.width, 1.0), (small.width * 2, 0.5)):
        _, scale, x, y = best
        near = [x + d * step for d in range(-3, 4)]
        near_y = [y + d * step for d in range(-3, 4)]
        found = _search(hd, small, base, np.linspace(scale * 0.985, scale * 1.015, 7), near, near_y)
        if found and found[0] >= best[0]:
            best = found
    corr, scale, x, y = best
    k = 1 / scale                                     # canvas px per small px
    cw, ch = round(small.width * k), round(small.height * k)
    if corr < args.min_match or cw < small.width * 1.5:
        return None
    px, py = round(x * k), round(y * k)
    covered = (max(0, px), max(0, py), min(cw, px + hd.width), min(ch, py + hd.height))
    coverage = (covered[2] - covered[0]) * (covered[3] - covered[1]) / (cw * ch)
    if coverage < 0.5:
        return None
    canvas = small.convert("RGB").resize((cw, ch), Image.LANCZOS)
    layer = Image.new("RGB", (cw, ch))
    layer.paste(hd.convert("RGB"), (px, py))
    # alpha: 1 inside the original, ramping to 0 over `feather` px at edges inside the frame
    feather = max(8, round(k * 6))
    yy, xx = np.mgrid[0:ch, 0:cw].astype(np.float32)
    ramp = np.ones((ch, cw), np.float32)
    if covered[0] > 0: ramp = np.minimum(ramp, (xx - covered[0]) / feather)
    if covered[1] > 0: ramp = np.minimum(ramp, (yy - covered[1]) / feather)
    if covered[2] < cw: ramp = np.minimum(ramp, (covered[2] - 1 - xx) / feather)
    if covered[3] < ch: ramp = np.minimum(ramp, (covered[3] - 1 - yy) / feather)
    mask = Image.fromarray((np.clip(ramp, 0, 1) * 255).astype(np.uint8), "L")
    canvas.paste(layer, (0, 0), mask)
    return canvas, coverage


def job(item):
    kind, rel, source = item
    small_path = os.path.join(args.game, rel)
    target = os.path.join(args.game, "hd", rel)
    try:
        small = Image.open(small_path)
        small.load()
        image = Image.open(source)
        if kind == "bg":
            whole = image.convert("RGB")
        else:
            image = image.convert("RGBA")
            box = image.getchannel("A").getbbox()
            if box is None:
                return kind, "empty"
            whole = image.crop(box)
        box = find_crop(whole, small)
        if box is None:
            return kind, "not larger"
        hd = whole.crop(box)
        if kind == "face" and similarity(hd, small) >= args.min_match:
            # The converted portraits are zoomed-in crops that cut off hair and chin.
            # Use the whole original face instead, and replace the small portrait with a
            # matching version at the same height (the 3DS uses it; HD draws over it).
            if hd.size != whole.size:
                height = small.height
                width = max(1, round(whole.width * height / whole.height))
                whole.resize((width, height), Image.LANCZOS).save(small_path, "PNG", optimize=True)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            whole.save(target, "PNG", optimize=True)
            return kind, "ok, whole face"
        if similarity(hd, small) < args.min_match:
            if kind != "bg":
                return kind, "different"
            # Reframed picture (the 4:3 version shows more than the 16:9 original):
            # place the original where it lines up and fill the rest from the small image.
            placed = place(whole, small)
            if placed is not None:
                hd, coverage = placed
                status = "ok, reframed"
            elif os.path.basename(rel).startswith("bg_") and similarity(hd, small) >= args.min_redrawn:
                # Background redrawn for widescreen (objects moved or added): use the
                # closest 4:3 area of the new drawing. Not for CGs, where that area is
                # only part of a close-up.
                status = "ok, redrawn"
            else:
                return kind, "different"
        else:
            status = "ok"
        os.makedirs(os.path.dirname(target), exist_ok=True)
        if kind == "bg":
            hd.save(target, "JPEG", quality=90)
        else:
            hd.save(target, "PNG", optimize=True)
        return kind, status
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
