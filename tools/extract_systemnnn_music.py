#!/usr/bin/env python3
"""Convert System-NNN PACKTYPE=6 WGQ music into 3DVN-compatible PCM WAV."""

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

from import_vnds import find_ffmpeg


HEADER_SIZE = 64


def convert_music(source, destination, ffmpeg, rate):
    with source.open("rb") as file:
        header = file.read(HEADER_SIZE)
        if len(header) != HEADER_SIZE or header[48:64].rstrip(b" ") != b"PACKTYPE=6":
            raise ValueError(f"{source}: expected a PACKTYPE=6 WGQ header")
        if file.read(4) != b"OggS":
            raise ValueError(f"{source}: missing Ogg stream after WGQ header")
        file.seek(HEADER_SIZE)
        with tempfile.TemporaryFile() as ogg:
            shutil.copyfileobj(file, ogg)
            ogg.seek(0)
            result = subprocess.run(
                [ffmpeg, "-nostdin", "-y", "-loglevel", "error", "-i", "pipe:0",
                 "-vn", "-ac", "1", "-ar", str(rate), "-c:a", "pcm_s16le", str(destination)],
                stdin=ogg, capture_output=True, text=True,
            )
    if result.returncode:
        raise ValueError(f"{source}: ffmpeg failed: {result.stderr.strip()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new directory for converted WAV files")
    parser.add_argument("sources", nargs="+", type=Path, help="WGQ music files to convert")
    parser.add_argument("--ffmpeg", help="path to ffmpeg")
    parser.add_argument("--rate", type=int, default=32000, help="WAV sample rate (4000..48000)")
    args = parser.parse_args()
    if not 4000 <= args.rate <= 48000:
        parser.error("--rate must be between 4000 and 48000 Hz")
    if args.output.exists():
        parser.error("output already exists; choose a new directory")
    names = [source.stem.casefold() for source in args.sources]
    if len(set(names)) != len(names):
        parser.error("source filenames must be unique ignoring case")
    ffmpeg = find_ffmpeg(args.ffmpeg)
    if not ffmpeg:
        parser.error("ffmpeg was not found")
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".3dvn-systemnnn-", dir=args.output.parent) as temp:
            staging = Path(temp) / "music"
            staging.mkdir()
            for source in args.sources:
                convert_music(source, staging / (source.stem + ".wav"), ffmpeg, args.rate)
            staging.rename(args.output)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"conversion failed: {exc}\n")
    print(f"Converted {len(args.sources)} music file(s) to {args.output}")


if __name__ == "__main__":
    main()
