#!/usr/bin/env python3
"""Build a chart-ordered, explicitly incomplete Mahoyo story preview."""

import argparse
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

from inspect_mahoyo_chart import ChartError, inspect_chart, load_chart, trace_initial_playthrough


ROOT = Path(__file__).resolve().parents[1]
MARKER = "# Experimental KiriKiri scene preview"
SKIPPED = {
    "opening.ks": "The original opening sequence is omitted here.",
    "teatime.ks": "The original chapter-break interlude is omitted here.",
}


def build_preview(chart_path, entry, source, assets, theme, output, source_size, scene_size):
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError(f"output already exists: {output}")
    if not output.parent.is_dir():
        raise ValueError(f"output parent does not exist: {output.parent}")
    if not assets.is_dir():
        raise ValueError(f"assets directory does not exist: {assets}")
    if not (theme / "game.3vc").is_file() or not (theme / "ui").is_dir():
        raise ValueError(f"theme must contain game.3vc and ui/: {theme}")

    chart = inspect_chart(load_chart(chart_path, entry))
    trace = trace_initial_playthrough(chart)
    if trace["reason"] != "end_of_chart":
        raise ValueError(f"chart trace stopped: {trace['reason']}")
    order = trace["scene_order"]
    scenes = [scene for scene in order if scene not in SKIPPED]
    if not scenes:
        raise ValueError("chart has no convertible scenes")

    command = [sys.executable, str(ROOT / "tools/import_kirikiri.py"),
               str(source), "PLACEHOLDER"]
    for scene in scenes:
        command.extend(("--scene", scene))
    command.extend(("--preview", "--nvl-pages", "--assets", str(assets),
                    "--source-size", source_size, "--scene-size", scene_size))

    with tempfile.TemporaryDirectory(prefix=".mahoyo-preview-", dir=output.parent) as temp:
        game = Path(temp) / "game"
        command[3] = str(game)
        converted = subprocess.run(command, capture_output=True, text=True, errors="replace")
        if converted.returncode:
            detail = (converted.stderr or converted.stdout).strip()
            raise ValueError(f"scene conversion failed: {detail[-3000:]}")
        main_path = game / "main.3vs"
        script = main_path.read_text(encoding="utf-8").splitlines()
        if sum(line == MARKER for line in script) != len(scenes):
            raise ValueError("converter output does not match the chart scene count")
        if not script or script[-1] != "stop":
            raise ValueError("converter output has no final stop")

        inserts = []
        pending = []
        for scene in order:
            if scene in SKIPPED:
                pending.append(SKIPPED[scene])
            else:
                inserts.append(pending)
                pending = []
        result = []
        index = 0
        for line in script:
            if line == MARKER:
                result.append(line)
                if index == 0:
                    result.extend(('bg black',
                                   '"Diagnostic only: images, animations and effects may be missing or approximate."',
                                   '"Original opening and chapter-break sequences are omitted where noted."',
                                   'page'))
                for message in inserts[index]:
                    result.extend((f'"{message}"', "page"))
                index += 1
            elif line == "stop":
                for message in pending:
                    result.extend((f'"{message}"', "page"))
                pending = []
                result.extend(("page",
                               '"End of diagnostic preview. Many original effects remain unconverted."'))
                result.append(line)
            else:
                result.append(line)
        main_path.write_text("\n".join(result) + "\n", encoding="utf-8")
        report_path = game / "conversion-report.txt"
        report = report_path.read_text(encoding="utf-8")
        report_path.write_text(
            "Chart-ordered diagnostic preview. Opening and chapter-break interludes "
            "are omitted and marked in the script.\n"
            "Calls, effects and other source behavior remain unresolved; "
            "this is not a faithful or complete conversion.\n\n" + report,
            encoding="utf-8",
        )
        shutil.copy2(theme / "game.3vc", game / "game.3vc")
        shutil.copytree(theme / "ui", game / "ui")
        output.mkdir()
        try:
            for path in game.iterdir():
                path.rename(output / path.name)
        except OSError:
            shutil.rmtree(output)
            raise
    return len(scenes), len(order) - len(scenes)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("chart", type=Path, help="ZIP archive or extracted main.gc")
    parser.add_argument("source", type=Path, help="scene directory or ZIP-format patch archive")
    parser.add_argument("output", type=Path, help="new preview directory")
    parser.add_argument("--entry", default="others/main.gc", help="chart entry in ZIP archive")
    parser.add_argument("--assets", required=True, type=Path, help="extracted image and audio assets")
    parser.add_argument("--theme", required=True, type=Path, help="directory with game.3vc and ui/")
    parser.add_argument("--source-size", default="1024x576")
    parser.add_argument("--scene-size", default="1024x576")
    args = parser.parse_args(argv)
    try:
        count, omitted = build_preview(args.chart, args.entry, args.source, args.assets,
                                       args.theme, args.output, args.source_size, args.scene_size)
    except (OSError, ValueError, ChartError, zipfile.BadZipFile) as exc:
        parser.exit(1, f"preview build failed: {exc}\n")
    print(f"Wrote diagnostic preview: {count} scenes, {omitted} omitted interludes, {args.output}")


if __name__ == "__main__":
    main()
