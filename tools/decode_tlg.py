#!/usr/bin/env python3
"""Convert KiriKiri TLG images (TLG5, optionally inside a TLG0 wrapper) to PNG.

usage: decode_tlg.py OUT_DIR FILE.tlg [FILE.tlg ...] [--jobs N] [--compose]
       decode_tlg.py OUT_DIR --from DIR [--from DIR ...]     (all .tlg files in DIR, recursively)

--compose: a sprite part with a .pos file next to it (KiriKiri expression layers:
[ string "base", int TOP, int LEFT ], i.e. y first) is pasted onto its base image at
that position, so every output PNG is a complete picture.

Pure Python plus numpy and Pillow; no external tools. Only already-extracted image
files are read (this does not open game archives). TLG6 is not supported yet; such
files are reported and skipped.

TLG5 layout: a header (colour count, width, height, block height, block size table),
then for each block of rows and each colour channel an LZSS-compressed or raw plane.
The planes hold deltas: blue and red are stored relative to green, and every pixel is
the running sum along its row plus the pixel above it.
"""
import argparse, concurrent.futures, os, struct, sys

import numpy as np
from PIL import Image


def _lzss(src, pos, size, out_len, text, r):
    """KiriKiri's LZSS (4 KiB ring buffer shared by all planes of an image)."""
    out = bytearray(out_len)
    o = 0
    end = pos + size
    flags = 0
    while pos < end and o < out_len:
        flags >>= 1
        if not flags & 0x100:
            flags = src[pos] | 0xFF00
            pos += 1
        if flags & 1:
            lo, hi = src[pos], src[pos + 1]
            pos += 2
            mpos = lo | ((hi & 0x0F) << 8)
            mlen = (hi >> 4) + 3
            if mlen == 18:
                mlen += src[pos]
                pos += 1
            for _ in range(mlen):
                c = text[mpos]
                mpos = (mpos + 1) & 0xFFF
                if o < out_len:
                    out[o] = c
                    o += 1
                text[r] = c
                r = (r + 1) & 0xFFF
        else:
            c = src[pos]
            pos += 1
            out[o] = c
            o += 1
            text[r] = c
            r = (r + 1) & 0xFFF
    return out, r


def decode_tlg5(data):
    if data[:11] != b"TLG5.0\x00raw\x1a":
        raise ValueError("not TLG5")
    colors = data[11]
    width, height, block_h = struct.unpack_from("<III", data, 12)
    if colors not in (3, 4):
        raise ValueError(f"unsupported colour count {colors}")
    blocks = (height + block_h - 1) // block_h
    pos = 24 + 4 * blocks                       # skip the block size table
    planes = np.zeros((colors, height, width), np.uint8)
    text = bytearray(4096)
    r = 0
    for b in range(blocks):
        y0 = b * block_h
        rows = min(block_h, height - y0)
        for c in range(colors):
            mark = data[pos]
            size = struct.unpack_from("<I", data, pos + 1)[0]
            pos += 5
            if mark == 0:
                plane, r = _lzss(data, pos, size, width * block_h, text, r)
            else:
                plane = data[pos:pos + size]
            pos += size
            planes[c, y0:y0 + rows] = np.frombuffer(bytes(plane[:width * rows]), np.uint8).reshape(rows, width)
    deltas = planes.astype(np.uint32)
    deltas[0] += deltas[1]                      # blue  relative to green
    deltas[2] += deltas[1]                      # red   relative to green
    values = np.cumsum(np.cumsum(deltas, axis=2), axis=1) & 0xFF   # along rows, then down
    rgba = np.empty((height, width, 4), np.uint8)
    rgba[..., 0], rgba[..., 1], rgba[..., 2] = values[2], values[1], values[0]
    rgba[..., 3] = values[3] if colors == 4 else 255
    return Image.fromarray(rgba, "RGBA").convert("RGBA" if colors == 4 else "RGB")


def decode_tlg(data):
    if data[:11] == b"TLG0.0\x00sds\x1a":      # wrapper: raw size, then the inner image
        size = struct.unpack_from("<I", data, 11)[0]
        data = data[15:15 + size]
    if data[:6] == b"TLG5.0":
        return decode_tlg5(data)
    raise ValueError(f"unsupported TLG type {data[:6]!r}")


def convert(job):
    source, target = job
    try:
        with open(source, "rb") as handle:
            image = decode_tlg(handle.read())
        os.makedirs(os.path.dirname(target), exist_ok=True)
        image.save(target, optimize=False)
        return None
    except Exception as error:
        return f"{source}: {error}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out")
    parser.add_argument("files", nargs="*")
    parser.add_argument("--from", dest="sources", action="append", default=[], metavar="DIR")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--compose", action="store_true", help="paste .pos expression layers onto their base")
    args = parser.parse_args()
    jobs = [(f, os.path.join(args.out, os.path.splitext(os.path.basename(f))[0] + ".png")) for f in args.files]
    for top in args.sources:
        for folder, _, names in os.walk(top):
            for name in names:
                if name.lower().endswith(".tlg"):
                    rel = os.path.relpath(os.path.join(folder, name[:-4] + ".png"), top)
                    jobs.append((os.path.join(folder, name),
                                 os.path.join(args.out, os.path.basename(os.path.normpath(top)), rel)))
    failed = 0
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for done, problem in enumerate(pool.map(convert, jobs, chunksize=4), 1):
            if problem:
                failed += 1
                print(problem, file=sys.stderr)
            if done % 500 == 0:
                print(f"  {done}/{len(jobs)}", flush=True)
    print(f"{len(jobs) - failed} converted, {failed} failed")
    if args.compose:
        composed = compose_layers(jobs)
        print(f"{composed} expression layers composed onto their base images")
    return 1 if failed else 0


def read_pos(path):
    """KiriKiri .pos: '[ string "base", int top, int left ]' (UTF-16 or UTF-8) -> (base, x, y).
    The numbers are y first: checked by matching expression layers against their base images."""
    import re
    raw = open(path, "rb").read()
    text = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8", "replace")
    m = re.search(r'string\s+"([^"]+)"\s*,\s*int\s+(-?\d+)\s*,\s*int\s+(-?\d+)', text)
    return (m.group(1), int(m.group(3)), int(m.group(2))) if m else None


def compose_layers(jobs):
    """Pastes each layer that has a .pos file onto its decoded base (same output folder)."""
    composed = 0
    for source, target in jobs:
        pos_path = os.path.splitext(source)[0] + ".pos"
        if not os.path.exists(pos_path) or not os.path.exists(target):
            continue
        pos = read_pos(pos_path)
        if pos is None:
            continue
        base_path = os.path.join(os.path.dirname(target), pos[0] + ".png")
        if not os.path.exists(base_path) or os.path.abspath(base_path) == os.path.abspath(target):
            continue
        base = Image.open(base_path).convert("RGBA")
        layer = Image.open(target).convert("RGBA")
        canvas = base.copy()
        canvas.alpha_composite(layer, (pos[1], pos[2]))
        canvas.save(target)
        composed += 1
    return composed


if __name__ == "__main__":
    sys.exit(main())
