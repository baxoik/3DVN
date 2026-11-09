#!/usr/bin/env python3
"""Extract named TLG5 thumbnail backgrounds for an experimental 3DVN preview."""

import argparse
import os
import re
import shlex
import struct
import subprocess
import tempfile
import zipfile
from pathlib import Path

from import_kirikiri import read_scene
from import_vnds import Image

TLG0 = b"TLG0.0\x00sds\x1a"
TLG5 = b"TLG5.0\x00raw\x1a"
MAX_FILE_SIZE = 16 * 1024 * 1024


def background_names(script):
    names = set()
    for line_no, line in enumerate(script.splitlines(), 1):
        line = line.strip()
        if not line.startswith("@"):
            continue
        words = line[1:].split(None, 1)
        if not words:
            continue
        tag = words[0].lower()
        if tag not in ("bg", "background", "partbg"):
            continue
        try:
            parts = shlex.split(line[1:])
        except ValueError as exc:
            raise ValueError(f"line {line_no}: invalid tag syntax: {exc}") from exc
        for part in parts[1:]:
            key, sep, value = part.partition("=")
            if sep and key.lower() == "storage" and value.lower() not in ("black", "white"):
                if (not re.fullmatch(r"[^/\\:*?\"<>|\x00-\x1f]+", value)
                        or value in (".", "..") or value.startswith(("&", "%"))):
                    raise ValueError(f"line {line_no}: unsupported thumbnail name: {value!r}")
                names.add(value)
    return sorted(names)


def unwrap_tlg(data):
    if data.startswith(TLG0):
        if len(data) < 15:
            raise ValueError("truncated TLG wrapper")
        length = struct.unpack_from("<I", data, 11)[0]
        if length > len(data) - 15:
            raise ValueError("truncated TLG payload")
        data = data[15:15 + length]
    if not data.startswith(TLG5) or len(data) < 24:
        raise ValueError("not a supported TLG5 image")
    channels, width, height, block_height = struct.unpack_from("<BIII", data, 11)
    if channels not in (3, 4) or not (1 <= width <= 1024 and 1 <= height <= 1024):
        raise ValueError("unsupported TLG dimensions or channels")
    if not 1 <= block_height <= height:
        raise ValueError("invalid TLG block height")
    return data, (width, height)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help="ZIP archive containing t_*.tlg thumbnails")
    parser.add_argument("output", type=Path, help="new directory for extracted PNG thumbnails")
    parser.add_argument("--scripts", type=Path, help="script directory or ZIP archive")
    parser.add_argument("--scene", help="scene to scan")
    parser.add_argument("--all", action="store_true",
                        help="extract all t_*.tlg thumbnails rather than scanning a scene")
    parser.add_argument("--encoding", default="auto")
    parser.add_argument("--decoder", required=True, type=Path, help="TLG5-to-BMP executable")
    parser.add_argument("--wine", help="Wine executable, required for Windows decoder on Linux")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists; choose a new directory")
    if Image is None:
        parser.error("Pillow is required")
    if not args.decoder.is_file():
        parser.error("decoder does not exist")
    if args.all and (args.scripts or args.scene):
        parser.error("--all cannot be combined with --scripts or --scene")
    if not args.all and (not args.scripts or not args.scene):
        parser.error("provide both --scripts and --scene, or use --all")
    if args.scripts and args.scripts.is_dir() and not (args.scripts / args.scene).resolve().is_relative_to(args.scripts.resolve()):
        parser.error("scene must be inside script directory")
    try:
        names = None if args.all else background_names(read_scene(args.scripts, args.scene, args.encoding))
        with zipfile.ZipFile(args.archive) as archive:
            entries = {}
            for entry in archive.infolist():
                if not entry.is_dir():
                    entries.setdefault(Path(entry.filename).name.casefold(), []).append(entry)
            if names is None:
                names = []
                for entry in archive.infolist():
                    filename = Path(entry.filename).name
                    if not entry.is_dir() and filename.lower().startswith("t_") and filename.lower().endswith(".tlg"):
                        name = filename[2:-4]
                        if (not re.fullmatch(r"[^/\\:*?\"<>|\x00-\x1f]+", name)
                                or name in (".", "..") or name.startswith(("&", "%"))):
                            raise ValueError(f"unsupported thumbnail name: {name!r}")
                        names.append(name)
                names.sort()
            missing = []
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix=".3dvn-thumbnails-", dir=args.output.parent) as temp:
                root = Path(temp)
                result_dir = root / "assets"
                result_dir.mkdir()
                converted = []
                dimensions = set()
                for name in names:
                    found = entries.get(("t_" + name + ".tlg").casefold(), [])
                    if not found:
                        missing.append(name)
                        continue
                    if len(found) != 1:
                        raise ValueError(f"ambiguous thumbnail for {name}")
                    if found[0].file_size > MAX_FILE_SIZE:
                        raise ValueError(f"thumbnail too large: {found[0].filename}")
                    raw, size = unwrap_tlg(archive.read(found[0]))
                    input_path, output_path = root / "input.tlg", root / "output.bmp"
                    input_path.write_bytes(raw)
                    if output_path.exists():
                        output_path.unlink()
                    if args.wine:
                        cmd = [args.wine, str(args.decoder.resolve()),
                               "Z:" + str(input_path.resolve()).replace("/", "\\"),
                               "Z:" + str(output_path.resolve()).replace("/", "\\")]
                    else:
                        cmd = [str(args.decoder.resolve()), str(input_path), str(output_path)]
                    env = os.environ.copy()
                    if args.wine:
                        env["WINEDEBUG"] = "-all"
                    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
                    if result.returncode or not output_path.is_file():
                        raise ValueError(f"decoder failed for {name}: {result.stderr.strip()}")
                    with Image.open(output_path) as image:
                        image.load()
                        if image.size != size:
                            raise ValueError(f"decoder returned incorrect dimensions for {name}")
                        image.convert("RGB").save(result_dir / (name + ".png"))
                    dimensions.add(size)
                    converted.append(name)
                    if args.all and len(converted) % 100 == 0:
                        print(f"Extracted {len(converted)} of {len(names)} thumbnails...", flush=True)
                if not converted:
                    raise ValueError("no matching thumbnails found")
                report = ["Low-resolution thumbnails, not original backgrounds.",
                          "Source: all archive thumbnails" if args.all else f"Source scene: {args.scene}",
                          "Converted:"]
                report.extend(converted)
                report.append("Not found:")
                report.extend(missing)
                (result_dir / "thumbnail-report.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
                result_dir.rename(args.output)
        print(f"Extracted {len(converted)} thumbnails; {len(missing)} not found.")
        print("Thumbnail dimensions: " + ", ".join(f"{w}x{h}" for w, h in sorted(dimensions)))
        for name in missing:
            print(f"  not found: {name}")
    except (OSError, ValueError, LookupError, zipfile.BadZipFile) as exc:
        parser.exit(1, f"thumbnail extraction failed: {exc}\n")


if __name__ == "__main__":
    main()
