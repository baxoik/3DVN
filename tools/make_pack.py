#!/usr/bin/env python3
"""Pack a 3DVN game folder into one archive file, <name>.3vp, optionally converting
its WAV audio to Ogg Vorbis first.

usage: make_pack.py GAME_DIR OUT.3vp [--ogg] [--voice-quality Q] [--music-quality Q]
                     [--cache DIR] [--jobs N]

The archive is a plain ZIP with no compression ("stored"), so 3DVN can read files
straight out of it and any zip tool can list or extract it. Copy it to
<3dvn root>/games/<name>.3vp; the game list shows it like a game folder.

--ogg converts every audio/*.wav (music and sound effects) and audio/v/**/*.wav
(voices) to .ogg with ffmpeg + libvorbis, in parallel, keeping the same names
without extension (the engine tries .ogg before .wav). Conversions are cached in
--cache (default: OUT.3vp.cache/) so repacking is quick. Requires ffmpeg.
"""
import argparse, concurrent.futures, os, subprocess, sys, zipfile

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("game")
parser.add_argument("out")
parser.add_argument("--ogg", action="store_true", help="convert WAV audio to Ogg Vorbis")
parser.add_argument("--voice-quality", type=float, default=2.0, help="Vorbis quality for audio/v/ (default 2)")
parser.add_argument("--music-quality", type=float, default=4.0, help="Vorbis quality for other audio (default 4)")
parser.add_argument("--cache", default=None)
parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
args = parser.parse_args()

game = os.path.abspath(args.game)
if not os.path.isfile(os.path.join(game, "main.3vs")):
    sys.exit(f"{game} has no main.3vs")
cache = args.cache or args.out + ".cache"

files = []   # (archive name, source path)
for folder, dirs, names in os.walk(game, followlinks=True):
    dirs.sort()
    for name in sorted(names):
        path = os.path.join(folder, name)
        rel = os.path.relpath(path, game).replace(os.sep, "/")
        if rel.startswith("shots/") or name.endswith((".log", ".tmp")):
            continue
        files.append((rel, path))


def convert(job):
    rel, path = job
    target = os.path.join(cache, rel[:-4] + ".ogg")
    if os.path.exists(target) and os.path.getmtime(target) >= os.path.getmtime(path):
        return rel, target
    os.makedirs(os.path.dirname(target), exist_ok=True)
    quality = args.voice_quality if rel.startswith("audio/v/") else args.music_quality
    subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", path, "-c:a", "libvorbis",
                    "-q:a", str(quality), "-f", "ogg", target + ".part"], check=True)
    os.replace(target + ".part", target)
    return rel, target


if args.ogg:
    wavs = [(rel, path) for rel, path in files if rel.startswith("audio/") and rel.endswith(".wav")]
    converted = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for done, (rel, target) in enumerate(pool.map(convert, wavs), 1):
            converted[rel] = target
            if done % 1000 == 0 or done == len(wavs):
                print(f"  converted {done}/{len(wavs)} audio files", flush=True)
    files = [(rel[:-4] + ".ogg", converted[rel]) if rel in converted else (rel, path) for rel, path in files]

if len(files) >= 65535:
    sys.exit("too many files for a ZIP without ZIP64")
total = sum(os.path.getsize(path) for _, path in files)
if total >= 0xFFFFFFFF - 64 * 1024 * 1024:
    sys.exit("game is too large for one archive (4 GB limit)")
tmp = args.out + ".part"
with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_STORED, allowZip64=False) as archive:
    for rel, path in files:
        archive.write(path, rel)
os.replace(tmp, args.out)
print(f"{args.out}: {len(files)} files, {os.path.getsize(args.out) / 1e6:.0f} MB "
      f"(from {total / 1e6:.0f} MB of files)")
