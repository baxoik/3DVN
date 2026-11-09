#!/usr/bin/env python3
"""Inspect MPK v2 archive indexes without reading or extracting payloads."""

import argparse
from collections import Counter
from pathlib import Path
import struct
import sys


HEADER_SIZE = 64
INDEX_OFFSET = HEADER_SIZE
ENTRY_SIZE = 256
ENTRY_HEADER = struct.Struct("<I4xQQQ")
FILENAME_SIZE = ENTRY_SIZE - ENTRY_HEADER.size


def inspect_archive(path, list_all):
    archive_size = path.stat().st_size
    with path.open("rb") as archive:
        header = archive.read(HEADER_SIZE)
        if len(header) != HEADER_SIZE or header[:4] != b"MPK\0":
            raise ValueError("not an MPK archive")
        version = int.from_bytes(header[6:8], "little")
        if version != 2:
            raise ValueError(f"unsupported MPK version {version}")
        if any(header[12:]):
            raise ValueError("unexpected data in reserved header bytes")
        entry_count = int.from_bytes(header[8:12], "little")

        table_size = entry_count * ENTRY_SIZE
        table_end = INDEX_OFFSET + table_size
        if table_end > archive_size:
            raise ValueError("entry table extends beyond archive size")

        extensions = Counter()
        invalid_entries = 0
        displayed = 0
        print(f"{path.name}: MPK v{version}, {entry_count} entries, {archive_size} bytes")

        for entry_number in range(entry_count):
            entry = archive.read(ENTRY_SIZE)
            if len(entry) != ENTRY_SIZE:
                raise ValueError(f"truncated entry {entry_number}")

            index, offset, size, repeated_size = ENTRY_HEADER.unpack_from(entry)
            raw_name = entry[ENTRY_HEADER.size :]
            name_bytes = raw_name.split(b"\0", 1)[0]
            name = name_bytes.decode("utf-8", errors="replace") or f"<entry-{index}>"
            extension = Path(name).suffix.lower() or "<no extension>"
            extensions[extension] += 1

            valid = (
                offset >= table_end
                and offset <= archive_size
                and size <= archive_size - offset
                and size == repeated_size
            )
            if not valid:
                invalid_entries += 1

            if list_all or displayed < 8:
                marker = "" if valid else " [INVALID METADATA]"
                print(f"  {index:5d}  {offset:12d}  {size:12d}  {name}{marker}")
                displayed += 1

        print("File types:")
        for extension, count in sorted(extensions.items()):
            print(f"  {extension}: {count}")
        if not list_all and entry_count > displayed:
            print(f"  (showing first {displayed}; pass --list for all names)")
        if invalid_entries:
            print(f"Invalid entries: {invalid_entries}", file=sys.stderr)
            return 1
        return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help="MPK v2 archive to inspect")
    parser.add_argument("--list", action="store_true", help="list every indexed filename")
    args = parser.parse_args()

    try:
        return inspect_archive(args.archive, args.list)
    except (OSError, ValueError) as error:
        print(f"mpk_index: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())