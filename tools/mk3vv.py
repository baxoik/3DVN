#!/usr/bin/env python3
"""Make a 3DVN video (.3vv + .wav) from a video file or a folder of JPEG frames.

  mk3vv.py clip.mp4 OUTDIR/NAME [--fps 15] [--size 256x144] [--quality 6]
  mk3vv.py --frames FOLDER OUTDIR/NAME --fps 15      (pre-made JPEG frames, no ffmpeg)

Writes OUTDIR/NAME.3vv and (with ffmpeg and an audio track) OUTDIR/NAME.wav.
Put both in GAME/video/.  See documentation.txt, section 7.
"""
import argparse, glob, os, shutil, struct, subprocess, sys, tempfile


def pack(frames, fps, width, height, out_path):
    """frames: list of JPEG byte strings."""
    count = len(frames)
    pos = 16 + (count + 1) * 4
    offsets = []
    for data in frames:
        offsets.append(pos)
        pos += len(data)
    offsets.append(pos)
    with open(out_path, "wb") as f:
        f.write(b"3VV1" + struct.pack("<HHII", width, height, int(round(fps * 1000)), count))
        f.write(struct.pack("<%dI" % len(offsets), *offsets))
        for data in frames:
            f.write(data)


def jpeg_size(data):
    i = 2
    while i < len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        if marker in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack(">HH", data[i + 5:i + 9])
            return w, h
        i += 2 + struct.unpack(">H", data[i + 2:i + 4])[0]
    raise ValueError("no JPEG size found")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="video file, or a folder with --frames")
    ap.add_argument("out", help="output path without extension")
    ap.add_argument("--fps", type=float, default=15)
    ap.add_argument("--size", default="256x144", help="WxH, at most 512x256 (default 256x144)")
    ap.add_argument("--quality", type=int, default=6, help="ffmpeg JPEG quality 2 (best) .. 31 (smallest)")
    ap.add_argument("--frames", action="store_true", help="source is a folder of JPEG frames")
    args = ap.parse_args()

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    temp = tempfile.mkdtemp()
    try:
        if args.frames:
            files = sorted(glob.glob(os.path.join(args.source, "*.jpg")))
        else:
            if shutil.which("ffmpeg") is None:
                sys.exit("ffmpeg is required to convert a video file")
            w, h = args.size.split("x")
            subprocess.check_call(["ffmpeg", "-v", "error", "-y", "-i", args.source,
                "-vf", "fps=%g,scale=%s:%s" % (args.fps, w, h), "-q:v", str(args.quality),
                "-pix_fmt", "yuvj420p", os.path.join(temp, "f%06d.jpg")])
            files = sorted(glob.glob(os.path.join(temp, "f*.jpg")))
            subprocess.call(["ffmpeg", "-v", "error", "-y", "-i", args.source, "-vn", "-ac", "1",
                "-ar", "22050", "-c:a", "pcm_s16le", args.out + ".wav"])
        if not files:
            sys.exit("no frames found")
        frames = [open(f, "rb").read() for f in files]
        width, height = jpeg_size(frames[0])
        if width > 512 or height > 256:
            sys.exit("frames are %dx%d; the maximum is 512x256" % (width, height))
        if any(len(f) > 256 * 1024 for f in frames):
            sys.exit("a frame is larger than 256 KB; use a higher --quality number or a smaller --size")
        pack(frames, args.fps, width, height, args.out + ".3vv")
        total = os.path.getsize(args.out + ".3vv")
        print("%s.3vv: %d frames, %dx%d, %g fps, %.1f MB" % (args.out, len(frames), width, height, args.fps, total / 1e6))
    finally:
        shutil.rmtree(temp, ignore_errors=True)


if __name__ == "__main__":
    main()
