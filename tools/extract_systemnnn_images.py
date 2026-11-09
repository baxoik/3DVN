#!/usr/bin/env python3
"""Extract and decode standard JPEG/PNG images from System-NNN GPK archives."""

import argparse
import re
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from import_vnds import find_ffmpeg


MAX_ENTRIES = 100_000
MAX_IMAGE_PIXELS = 100_000_000
PACKTYPE = re.compile(rb"PACKTYPE=(\d+)(A?) *\Z")


@dataclass(frozen=True)
class Entry:
    name: str
    offset: int
    size: int


class UnsupportedImageError(ValueError):
    pass


def read_entries(archive: Path) -> list[Entry]:
    table_path = archive.with_suffix(".gtb")
    try:
        table = table_path.read_bytes()
        archive_size = archive.stat().st_size
    except OSError as exc:
        raise ValueError(f"cannot read archive pair: {exc}") from exc

    if len(table) < 4:
        raise ValueError(f"{table_path}: truncated GTB header")
    count = struct.unpack_from("<I", table)[0]
    if not 0 < count <= MAX_ENTRIES:
        raise ValueError(f"{table_path}: invalid entry count {count}")

    name_table = 4
    offset_table = name_table + count * 4
    name_base = offset_table + count * 4
    if name_base >= len(table):
        raise ValueError(f"{table_path}: truncated GTB tables")

    data_offsets = [
        struct.unpack_from("<I", table, offset_table + index * 4)[0]
        for index in range(count)
    ]
    if data_offsets[0] != 0:
        raise ValueError(f"{table_path}: first GPK entry does not start at offset 0")

    entries = []
    seen_names = set()
    for index in range(count):
        name_offset = struct.unpack_from("<i", table, name_table + index * 4)[0]
        name_position = name_base + name_offset
        if not name_base <= name_position < len(table):
            raise ValueError(f"{table_path}: entry {index} has an invalid name offset")
        name_end = table.find(b"\0", name_position)
        if name_end < 0:
            raise ValueError(f"{table_path}: entry {index} has an unterminated name")
        try:
            name = table[name_position:name_end].decode("cp932")
        except UnicodeDecodeError as exc:
            raise ValueError(f"{table_path}: entry {index} has an invalid name") from exc
        unsafe_name = (
            not name
            or name in {".", ".."}
            or "/" in name
            or "\\" in name
            or ":" in name
            or name.endswith((" ", "."))
            or any(ord(char) < 32 for char in name)
        )
        if unsafe_name:
            raise ValueError(f"{table_path}: entry {index} has an unsafe name")
        if name.casefold() in seen_names:
            raise ValueError(f"{table_path}: duplicate entry name {name!r}")
        seen_names.add(name.casefold())

        offset = data_offsets[index]
        end = data_offsets[index + 1] if index + 1 < count else archive_size
        entries.append(Entry(name, offset, end - offset))
    return entries


def validate_entry_range(entry: Entry, archive_size: int, table_path: Path) -> None:
    end = entry.offset + entry.size
    if entry.offset >= end or end > archive_size:
        raise ValueError(f"{table_path}: invalid data range for entry {entry.name!r}")


def decode_entry(record: bytes, ffmpeg: str) -> tuple[str, bytes]:
    if len(record) < 64:
        raise ValueError("truncated DWQ resource header")
    match = PACKTYPE.fullmatch(record[48:64])
    if not match:
        raise ValueError("invalid DWQ PACKTYPE field")
    try:
        packtype = int(match.group(1))
    except ValueError as exc:
        raise ValueError("invalid DWQ PACKTYPE value") from exc
    if match.group(2):
        raise UnsupportedImageError("alpha-mask DWQ resources are not supported")
    if packtype == 5 and record[:16].rstrip(b" ") == b"JPEG":
        image_format, extension, signature = "JPEG", ".jpg", b"\xff\xd8"
    elif packtype == 8 and record[:16].rstrip(b" ") == b"PNG":
        image_format, extension, signature = "PNG", ".png", b"\x89PNG\r\n\x1a\n"
    else:
        raise UnsupportedImageError(
            f"unsupported DWQ image format {record[:16].rstrip()!r}, PACKTYPE={packtype}"
        )

    width, height = struct.unpack_from("<II", record, 0x24)
    if not width or not height or width * height > MAX_IMAGE_PIXELS:
        raise ValueError(f"invalid or excessive image dimensions {width}x{height}")
    payload = record[64:]
    if not payload.startswith(signature):
        raise ValueError(f"DWQ {image_format} payload has an invalid signature")

    with tempfile.TemporaryFile() as decoded:
        result = subprocess.run(
            [ffmpeg, "-nostdin", "-v", "error", "-f", "image2pipe", "-i", "pipe:0",
             "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"],
            input=payload,
            stdout=decoded,
            stderr=subprocess.PIPE,
            text=False,
        )
        decoded.seek(0, 2)
        expected_size = width * height * 3
        if result.returncode or decoded.tell() != expected_size:
            message = result.stderr.decode("utf-8", "replace").strip()
            if result.returncode:
                raise ValueError(f"ffmpeg could not decode DWQ {image_format}: {message}")
            raise ValueError(
                f"decoded image size does not match DWQ dimensions {width}x{height}"
            )
    return extension, payload


def extract_archive(archive: Path, output: Path, ffmpeg: str, selected: list[str]) -> tuple[int, int]:
    if output.exists():
        raise ValueError(f"output already exists: {output}")
    entries = read_entries(archive)
    if selected:
        selected_names = [name.casefold() for name in selected]
        if len(set(selected_names)) != len(selected_names):
            raise ValueError("selected entry names must be unique ignoring case")
        by_name = {entry.name.casefold(): entry for entry in entries}
        missing = [name for name in selected if name.casefold() not in by_name]
        if missing:
            raise ValueError("entry not found: " + ", ".join(missing))
        entries = [by_name[name] for name in selected_names]

    archive_size = archive.stat().st_size
    table_path = archive.with_suffix(".gtb")
    for entry in entries:
        validate_entry_range(entry, archive_size, table_path)

    output.parent.mkdir(parents=True, exist_ok=True)
    extracted = skipped = 0
    with tempfile.TemporaryDirectory(prefix=".3dvn-systemnnn-", dir=output.parent) as temp:
        staging = Path(temp) / "images"
        staging.mkdir()
        with archive.open("rb") as source:
            for entry in entries:
                source.seek(entry.offset)
                record = source.read(entry.size)
                if len(record) != entry.size:
                    raise ValueError(f"{archive}: truncated entry {entry.name!r}")
                try:
                    extension, payload = decode_entry(record, ffmpeg)
                except UnsupportedImageError as exc:
                    if selected:
                        raise ValueError(f"{entry.name}: {exc}") from exc
                    print(f"Skipping {entry.name}: {exc}", file=sys.stderr)
                    skipped += 1
                    continue
                except ValueError as exc:
                    raise ValueError(f"{entry.name}: {exc}") from exc
                (staging / (entry.name + extension)).write_bytes(payload)
                extracted += 1
        if not extracted:
            raise ValueError("no supported JPEG or PNG images were found")
        staging.rename(output)
    return extracted, skipped


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new directory for extracted images")
    parser.add_argument("archive", type=Path, help="GPK archive with its matching GTB file")
    parser.add_argument("--entry", action="append", default=[],
                        help="extract this GTB entry name (repeatable; default: supported images)")
    parser.add_argument("--ffmpeg", help="path to ffmpeg")
    args = parser.parse_args()
    ffmpeg = find_ffmpeg(args.ffmpeg)
    if not ffmpeg:
        parser.error("ffmpeg was not found")
    try:
        extracted, skipped = extract_archive(args.archive, args.output, ffmpeg, args.entry)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"image extraction failed: {exc}\n")
    summary = f"Extracted and decoded {extracted} image(s) to {args.output}"
    if skipped:
        summary += f"; skipped {skipped} unsupported entry(s)"
    print(summary)


if __name__ == "__main__":
    main()
