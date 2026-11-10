#!/usr/bin/env python3
"""Recover exact-name missing images from local directories or ZIP archives."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile
import zipfile

try:
    from PIL import Image
except ImportError:
    sys.exit("error: Pillow is required to validate recovered images")


IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".bmp"}
SOURCE_EXTENSIONS = IMAGE_EXTENSIONS | {".tlg"}
MISSING_IMAGE = re.compile(r"(?:background|foreground|CG) '([^']+)' not found")
FORMAT_EXTENSIONS = {"PNG": ".png", "JPEG": ".jpg", "BMP": ".bmp"}


@dataclass(frozen=True)
class Candidate:
    source: Path
    member: str | None
    filename: str

    @property
    def stem(self):
        return Path(self.filename).stem

    @property
    def suffix(self):
        return Path(self.filename).suffix.lower()

    def read_bytes(self):
        if self.member is None:
            return self.source.read_bytes()
        with zipfile.ZipFile(self.source) as archive:
            return archive.read(self.member)

    def description(self):
        if self.member is None:
            return f"{self.source.name}/{self.filename}"
        return f"{self.source.name}/{self.member}"


def report_missing(report_path):
    counts = Counter()
    with report_path.open(encoding="utf-8-sig") as report:
        for line in report:
            match = MISSING_IMAGE.search(line)
            if match:
                storage = match.group(1).replace("\\", "/").rsplit("/", 1)[-1]
                path = Path(storage)
                stem = path.stem if path.suffix.lower() in IMAGE_EXTENSIONS else storage
                counts[stem.casefold()] += 1
    if not counts:
        raise ValueError(f"no missing image references found in {report_path}")
    return counts


def index_sources(sources):
    index = {}
    source_paths = []
    for source in sources:
        source = source.resolve(strict=True)
        if source.is_dir():
            source_paths.append(source)
            for path in sorted(source.rglob("*")):
                if path.is_file() and not path.is_symlink() and path.suffix.lower() in SOURCE_EXTENSIONS:
                    candidate = Candidate(path, None, path.name)
                    index.setdefault(candidate.stem.casefold(), []).append(candidate)
        elif zipfile.is_zipfile(source):
            with zipfile.ZipFile(source) as archive:
                for info in archive.infolist():
                    if info.is_dir():
                        continue
                    filename = PurePosixPath(info.filename.replace("\\", "/")).name
                    if Path(filename).suffix.lower() in SOURCE_EXTENSIONS:
                        candidate = Candidate(source, info.filename, filename)
                        index.setdefault(candidate.stem.casefold(), []).append(candidate)
        else:
            raise ValueError(f"source is neither a directory nor a ZIP archive: {source}")
    return index, source_paths


def validate_raster(data, suffix, destination):
    with Image.open(BytesIO(data)) as image:
        image.load()
        image_format = image.format
        if image_format not in FORMAT_EXTENSIONS:
            raise ValueError(f"unsupported source image format: {image_format}")
        expected_formats = {
            ".png": "PNG",
            ".jpg": "JPEG",
            ".jpeg": "JPEG",
            ".bmp": "BMP",
        }
        if suffix in expected_formats and expected_formats[suffix] == image_format:
            destination = destination.with_suffix(suffix)
            destination.write_bytes(data)
        else:
            destination = destination.with_suffix(FORMAT_EXTENSIONS[image_format])
            image.save(destination)
    return destination


def recover(report_path, output, sources, decoder=None):
    report_path = report_path.resolve(strict=True)
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError(f"output already exists: {output}")
    if not output.parent.is_dir():
        raise ValueError(f"output parent does not exist: {output.parent}")

    counts = report_missing(report_path)
    index, source_dirs = index_sources(sources)
    for source_dir in source_dirs:
        if output.resolve().is_relative_to(source_dir):
            raise ValueError(f"output must not be inside source directory: {source_dir}")

    recovered = []
    unmatched = []
    ambiguous = []
    for name in sorted(counts):
        matches = index.get(name, [])
        if not matches:
            unmatched.append(name)
        elif len(matches) > 1:
            ambiguous.append((name, matches))
        else:
            recovered.append((name, matches[0]))

    tlg_candidates = [(name, candidate) for name, candidate in recovered
                      if candidate.suffix == ".tlg"]
    if tlg_candidates and decoder is None:
        raise ValueError("matching TLG images require --decoder PATH")

    with tempfile.TemporaryDirectory(prefix=".mahoyo-recovery-", dir=output.parent) as temp:
        staging = Path(temp) / "output"
        staging.mkdir()
        staged_tlg = Path(temp) / "tlg-inputs"
        decoded_tlg = Path(temp) / "decoded-tlg"
        if tlg_candidates:
            staged_tlg.mkdir()
        outputs = set()
        recovered_files = []
        for name, candidate in recovered:
            if candidate.suffix == ".tlg":
                source_path = staged_tlg / (candidate.stem + ".tlg")
                source_path.write_bytes(candidate.read_bytes())
                continue
            data = candidate.read_bytes()
            destination = staging / candidate.stem
            result_path = validate_raster(data, candidate.suffix, destination)
            output_name = result_path.name.casefold()
            if output_name in outputs:
                raise ValueError(f"duplicate output image name: {result_path.name}")
            outputs.add(output_name)
            recovered_files.append((name, candidate, result_path.name))

        if tlg_candidates:
            decoder = Path(decoder).resolve(strict=True)
            tool = Path(__file__).with_name("decode_kirikiri_tlg.py")
            inputs = [staged_tlg / (candidate.stem + ".tlg")
                      for _, candidate in tlg_candidates]
            command = [sys.executable, str(tool), "--decoder", str(decoder),
                       "--output", str(decoded_tlg), *(str(path) for path in inputs)]
            result = subprocess.run(command, capture_output=True, text=True, errors="replace")
            if result.returncode:
                detail = (result.stderr + "\n" + result.stdout).strip()
                raise ValueError(f"TLG decoder failed: {detail[-2000:]}")
            for name, candidate in tlg_candidates:
                source_path = decoded_tlg / (candidate.stem + ".png")
                destination = staging / source_path.name
                if destination.name.casefold() in outputs:
                    raise ValueError(f"duplicate output image name: {destination.name}")
                source_path.rename(destination)
                outputs.add(destination.name.casefold())
                recovered_files.append((name, candidate, destination.name))

        resolved_occurrences = sum(counts[name] for name, _ in recovered)
        lines = [
            "Exact-name missing image recovery",
            f"Diagnostic: {report_path.name}",
            f"Missing unique image names: {len(counts)}",
            f"Missing report occurrences: {sum(counts.values())}",
            f"Recovered unique image names: {len(recovered_files)}",
            f"Recovered report occurrences: {resolved_occurrences}",
            f"Unmatched unique image names: {len(unmatched)}",
            f"Ambiguous unique image names: {len(ambiguous)}",
            "",
            "Recovered files:",
        ]
        lines.extend(
            f"{name}: {filename} <- {candidate.description()}"
            for name, candidate, filename in sorted(recovered_files)
        )
        lines.extend(("", "Unmatched names:"))
        lines.extend(unmatched)
        lines.extend(("", "Ambiguous names:"))
        for name, matches in ambiguous:
            lines.append(f"{name}: " + "; ".join(match.description() for match in matches))
        (staging / "recovery-report.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        staging.rename(output)

    return len(recovered_files), len(counts), resolved_occurrences, len(unmatched), len(ambiguous)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="conversion-report.txt")
    parser.add_argument("output", type=Path, help="new directory for recovered original-size images")
    parser.add_argument("--source", action="append", required=True, type=Path,
                        help="local image directory or ZIP-format archive; repeat as needed")
    parser.add_argument("--decoder", type=Path,
                        help="arc_unpacker executable, required only for matching TLG files")
    args = parser.parse_args()
    try:
        recovered, unique, occurrences, unmatched, ambiguous = recover(
            args.report, args.output, args.source, args.decoder)
    except (OSError, ValueError, zipfile.BadZipFile, subprocess.SubprocessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(
        f"Recovered {recovered} of {unique} unique missing images "
        f"({occurrences} report occurrences); {unmatched} unmatched and "
        f"{ambiguous} ambiguous. Output: {args.output}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
