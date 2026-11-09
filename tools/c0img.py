#!/usr/bin/env python3
"""Extract images from the Chaos;Head Noah PSP data archives (BGL00.AFS = event CGs
and backgrounds, CHL00.AFS = character sprites) and save them as PNG. Run it on your
own copy of the game; the output stays on your computer.

  c0img.py extract ARCHIVE.AFS INDEX OUT.png [--size WxH] [--fit]
  c0img.py sheet   ARCHIVE.AFS START END OUT.png     (numbered contact sheet)

Each entry is block-LZ compressed TIM2 (8-bit palette, PS2 CLUT swizzle). Known
issue: a few entries (e.g. BGL 8, 16, 32-34, 56, 58, 200) decode with artifacts.
Useful BGL00 indices: 3 and 6 (opening dream CG), 401 (sky), 402 (chapter title),
474-476 (Takumi's room). --size resizes (keeping aspect, centred on black if --fit).
"""
import argparse, struct
from PIL import Image, ImageDraw


def lz(d, start, limit, out):
    i, end = start, len(out) + limit
    while i < len(d) and len(out) < end:
        m = d[i]; i += 1
        for b in range(8):
            if i >= len(d) or len(out) >= end:
                break
            if not (m >> b) & 1:
                out.append(d[i]); i += 1
            else:
                v = (d[i] << 8) | d[i + 1]; i += 2
                dist, ln = (v >> 5) + 1, (v & 31) + 3
                for _ in range(ln):
                    if len(out) >= end:
                        break
                    out.append(0 if v == 0xFFFF else (out[-dist] if dist <= len(out) else 0))


def unpack(d):
    total = (d[1] << 16) | (d[2] << 8) | d[3]
    out, pos = bytearray(), 4
    n = (d[pos] << 8) | d[pos + 1]; pos += 2
    while len(out) < total:
        lz(d[:pos + n], pos, min(16384, total - len(out)), out)
        pos += n
        if len(out) >= total:
            break
        n = (d[pos] << 8) | d[pos + 1]; pos += 2
    return bytes(out)


def entries(path):
    a = open(path, "rb").read()
    n = struct.unpack("<I", a[4:8])[0]
    return a, [struct.unpack("<II", a[8 + 8 * i:16 + 8 * i]) for i in range(n)]


def to_image(buf):
    assert buf[:4] == b"TIM2"
    _, clut, img, hs, cc, _, _, _, _, w, h = struct.unpack("<IIIHHBBBBHH", buf[16:40])
    data = 16 + hs
    px = buf[data:data + img]
    cl = buf[data + img:data + img + clut]
    pal = [tuple(cl[i * 4:i * 4 + 4]) for i in range(cc)]
    if cc == 256:
        sw = [None] * 256
        for i in range(256):
            sw[(i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)] = pal[i]
        pal = sw
    im = Image.new("RGBA", (w, h))
    im.putdata([(p[0], p[1], p[2], min(255, p[3] * 2)) for p in (pal[x] for x in px[:w * h])])
    return im


def load(path, index):
    a, e = entries(path)
    o, l = e[index]
    return to_image(unpack(a[o:o + l]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    x = sub.add_parser("extract"); x.add_argument("afs"); x.add_argument("index", type=int); x.add_argument("out")
    x.add_argument("--size"); x.add_argument("--fit", action="store_true")
    s = sub.add_parser("sheet"); s.add_argument("afs"); s.add_argument("start", type=int)
    s.add_argument("end", type=int); s.add_argument("out")
    args = ap.parse_args()
    if args.cmd == "extract":
        im = load(args.afs, args.index)
        if args.size:
            w, h = map(int, args.size.lower().split("x"))
            if args.fit:
                im.thumbnail((w, h), Image.LANCZOS)
                canvas = Image.new("RGBA", (w, h), (0, 0, 0, 255))
                canvas.alpha_composite(im, ((w - im.width) // 2, (h - im.height) // 2))
                im = canvas
            else:
                im = im.resize((w, h), Image.LANCZOS)
        im.save(args.out)
    else:
        a, e = entries(args.afs)
        cols, tw, th = 16, 90, 68
        n = args.end - args.start
        sheet = Image.new("RGB", (cols * tw, ((n + cols - 1) // cols) * th), (60, 60, 60))
        dr = ImageDraw.Draw(sheet)
        for k, i in enumerate(range(args.start, args.end)):
            try:
                o, l = e[i]
                im = to_image(unpack(a[o:o + l]))
                bg = Image.new("RGBA", im.size, (255, 0, 255, 255)); bg.alpha_composite(im)
                im = bg.convert("RGB"); im.thumbnail((tw, th))
                sheet.paste(im, ((k % cols) * tw, (k // cols) * th))
            except Exception:
                pass
            dr.text(((k % cols) * tw + 2, (k // cols) * th + 2), str(i), fill=(255, 255, 0))
        sheet.save(args.out)


if __name__ == "__main__":
    main()
