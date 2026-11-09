#!/usr/bin/env python3
"""Extract System-NNN VPK/VTB PACKTYPE=2 Ogg voices to mono PCM WAV.

Usage: python tools/extract_systemnnn_voice.py OUTPUT --archive gss/cdvaw/qq.vpk
       --archive gss/cdvaw/yk.vpk --name qq180001 --name yk180001
Omit --name to extract every entry. OUTPUT must not already exist. ffmpeg is
required; --rate defaults to 32000 Hz (4000..48000). Only PACKTYPE=2 with an
Ogg stream after the 108-byte wrapper is supported. Game data stays local.

VTB layout informed by GARbro ArcFormats/BlackCyc/ArcVPK.cs:
https://github.com/morkt/GARbro/blob/b09ee4570ccb1daf6ac56710ee8934dc0b8baeb0/ArcFormats/BlackCyc/ArcVPK.cs
Copyright (C) 2015 by morkt

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to
deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.
"""

import argparse
import re
import shutil
import struct
import subprocess
import tempfile
import wave
from dataclasses import dataclass
from pathlib import Path


MAX_ENTRIES = 100_000
MAX_RECORD_SIZE = 128 * 1024 * 1024
SAFE_NAME = re.compile(r"[A-Za-z0-9_-]{1,8}\Z")


@dataclass(frozen=True)
class Entry:
    name: str
    archive: Path
    start: int
    end: int


def read_entries(archive: Path) -> list[Entry]:
    table_path = archive.with_suffix(".vtb")
    table = table_path.read_bytes()
    size = archive.stat().st_size
    if len(table) < 24 or len(table) % 12 or len(table) // 12 - 1 > MAX_ENTRIES:
        raise ValueError(f"{table_path}: invalid VTB table length")
    if table[-12:-4] != bytes(8):
        raise ValueError(f"{table_path}: missing VTB sentinel")
    records = [struct.unpack_from("<8sI", table, i) for i in range(0, len(table), 12)]
    seen = set()
    entries = []
    for (raw_name, start), (_, end) in zip(records, records[1:]):
        try:
            name = raw_name.rstrip(b"\0").decode("ascii")
        except UnicodeDecodeError as exc:
            raise ValueError(f"{table_path}: non-ASCII entry name") from exc
        if not SAFE_NAME.fullmatch(name):
            raise ValueError(f"{table_path}: unsafe entry name {name!r}")
        if name.casefold() in seen:
            raise ValueError(f"{table_path}: duplicate entry name {name}")
        seen.add(name.casefold())
        if start >= end or end > size or end - start > MAX_RECORD_SIZE:
            raise ValueError(f"{table_path}: invalid or excessive range for {name}")
        entries.append(Entry(name, archive, start, end))
    return entries


def decode_entry(entry: Entry, destination: Path, ffmpeg: str, rate: int) -> None:
    with entry.archive.open("rb") as source:
        source.seek(entry.start)
        record = source.read(entry.end - entry.start)
    if len(record) != entry.end - entry.start:
        raise ValueError(f"{entry.name}: truncated archive entry")
    if len(record) < 112 or record[48:64].rstrip(b" ") != b"PACKTYPE=2":
        raise ValueError(f"{entry.name}: unsupported VAW (expected PACKTYPE=2)")
    if (record[64:68] != b"RIFF" or record[72:80] != b"WAVEfmt "
            or record[100:104] != b"data" or record[108:112] != b"OggS"):
        raise ValueError(f"{entry.name}: unsupported PACKTYPE=2 wrapper or missing Ogg")
    # The RIFF lengths describe decoded PCM, not the embedded Ogg byte count.
    result = subprocess.run(
        [ffmpeg, "-nostdin", "-v", "error", "-xerror", "-f", "ogg",
         "-i", "pipe:0", "-map", "0:a:0", "-vn", "-ac", "1",
         "-ar", str(rate), "-c:a", "pcm_s16le", "-f", "wav", str(destination)],
        input=record[108:], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise ValueError(f"{entry.name}: ffmpeg failed: "
                         + result.stderr.decode("utf-8", "replace").strip())
    try:
        with wave.open(str(destination), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, rate):
                raise ValueError(f"{entry.name}: unexpected output WAV format")
            if not wav.getnframes():
                raise ValueError(f"{entry.name}: empty decoded audio")
    except wave.Error as exc:
        raise ValueError(f"{entry.name}: invalid output WAV: {exc}") from exc


def extract(archives: list[Path], output: Path, names: list[str], ffmpeg: str, rate: int) -> int:
    if output.exists():
        raise ValueError(f"output already exists: {output}")
    entries = [entry for archive in archives for entry in read_entries(archive)]
    by_name = {}
    for entry in entries:
        key = entry.name.casefold()
        if key in by_name:
            raise ValueError(f"duplicate entry across archives: {entry.name}")
        by_name[key] = entry
    if names:
        keys = [name.casefold() for name in names]
        if len(set(keys)) != len(keys):
            raise ValueError("requested names must be unique ignoring case")
        missing = [name for name, key in zip(names, keys) if key not in by_name]
        if missing:
            raise ValueError("entry not found: " + ", ".join(missing))
        entries = [by_name[key] for key in keys]
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".3dvn-systemnnn-voice-", dir=output.parent) as temp:
        staging = Path(temp) / "voices"
        staging.mkdir()
        for entry in entries:
            decode_entry(entry, staging / (entry.name + ".wav"), ffmpeg, rate)
        staging.rename(output)
    return len(entries)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", type=Path, help="new output directory (parent must exist)")
    parser.add_argument("--archive", type=Path, action="append", required=True, help="VPK with adjacent .vtb (repeatable)")
    parser.add_argument("--name", action="append", default=[], help="entry name, case-insensitive (repeatable)")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="ffmpeg executable")
    parser.add_argument("--rate", type=int, default=32000, help="output sample rate (4000..48000)")
    args = parser.parse_args()
    if not 4000 <= args.rate <= 48000:
        parser.error("--rate must be between 4000 and 48000")
    ffmpeg = shutil.which(args.ffmpeg)
    if not ffmpeg:
        parser.error("ffmpeg was not found")
    try:
        count = extract(args.archive, args.output, args.name, ffmpeg, args.rate)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"extraction failed: {exc}\n")
    print(f"Extracted {count} voice(s) to {args.output}")


if __name__ == "__main__":
    main()
