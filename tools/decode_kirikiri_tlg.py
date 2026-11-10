#!/usr/bin/env python3
"""Decode selected local TLG images to a new directory using arc_unpacker."""

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

try:
    from PIL import Image
except ImportError:
    sys.exit("error: Pillow is required to validate decoded images")

TLG0 = b"TLG0.0\x00sds\x1a"
TLG5 = b"TLG5.0\x00raw\x1a"
TLG6 = b"TLG6.0\x00raw\x1a"


def image_info(path):
    """Read the dimensions and alpha-channel flag without altering the input."""
    with path.open("rb") as source:
        header = source.read(40)
    offset = 0
    if header.startswith(TLG0):
        if len(header) < 15:
            raise ValueError(f"{path}: truncated TLG0 header")
        payload_size = struct.unpack_from("<I", header, 11)[0]
        if payload_size < 24 or payload_size > path.stat().st_size - 15:
            raise ValueError(f"{path}: invalid TLG0 payload length")
        offset = 15
    magic = header[offset:offset + 11]
    if magic not in (TLG5, TLG6):
        raise ValueError(f"{path}: unsupported TLG format")
    dimensions_offset = offset + (12 if magic == TLG5 else 15)
    if len(header) < dimensions_offset + 8:
        raise ValueError(f"{path}: truncated TLG header")
    channels = header[offset + 11]
    width, height = struct.unpack_from("<II", header, dimensions_offset)
    if channels not in (3, 4) or not width or not height:
        raise ValueError(f"{path}: invalid dimensions or channel count")
    return (width, height), channels


def decode(decoder, output, inputs):
    decoder = decoder.resolve(strict=True)
    if not decoder.is_file() or not os.access(decoder, os.X_OK):
        raise ValueError(f"decoder is not an executable file: {decoder}")
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError(f"output already exists: {output}")
    if not output.parent.is_dir():
        raise ValueError(f"output parent does not exist: {output.parent}")

    sources = []
    names = set()
    for path in inputs:
        if path.suffix.lower() != ".tlg" or not path.is_file():
            raise ValueError(f"not a TLG file: {path}")
        name = path.stem + ".png"
        if name.casefold() in names:
            raise ValueError(f"duplicate output name: {name}")
        names.add(name.casefold())
        sources.append((path.resolve(), name, image_info(path)))

    with tempfile.TemporaryDirectory(prefix=".tlg-decode-", dir=output.parent) as temp:
        root = Path(temp)
        converted = root / "converted"
        converted.mkdir()
        work = root / "work"
        work.mkdir()
        copied_input = work / "input.tlg"
        decoded_output = work / "input.png"
        for source, name, (size, channels) in sources:
            shutil.copyfile(source, copied_input)
            result = subprocess.run(
                [str(decoder), "--dec=kirikiri/tlg", "--threads=1", "--no-recurse",
                 "--no-vfs", "--no-color", f"--out={work}", str(copied_input)],
                capture_output=True, text=True, errors="replace", check=False,
            )
            if result.returncode or not decoded_output.is_file():
                detail = (result.stderr + "\n" + result.stdout).strip()
                raise ValueError(f"decoder failed for {source}: {detail[-2000:]}")
            try:
                with Image.open(decoded_output) as image:
                    image.load()
                    if image.format != "PNG" or image.size != size:
                        raise ValueError("incorrect PNG format or dimensions")
                    if channels == 4 and "A" not in image.getbands():
                        raise ValueError("missing alpha channel")
            except (OSError, ValueError) as exc:
                raise ValueError(f"invalid decoder output for {source}: {exc}") from exc
            decoded_output.rename(converted / name)
            copied_input.unlink()

        # Reserve the destination without replacing a pre-existing directory.
        output.mkdir()
        try:
            for path in converted.iterdir():
                path.rename(output / path.name)
        except Exception:
            shutil.rmtree(output)
            raise
    return len(sources)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--decoder", required=True, type=Path, help="arc_unpacker executable")
    parser.add_argument("--output", required=True, type=Path, help="new output directory")
    parser.add_argument("inputs", nargs="+", type=Path, help="selected extracted .tlg files")
    args = parser.parse_args()
    try:
        count = decode(args.decoder, args.output, args.inputs)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"Decoded {count} image(s) to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
