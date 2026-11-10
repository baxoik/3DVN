#!/usr/bin/env python3
"""Build a small approximate 3DVN excerpt from the local GSS opening assets."""

import argparse
import shutil
import tempfile
import wave
from pathlib import Path

try:
    from PIL import Image, ImageOps
except ImportError:
    Image = ImageOps = None

from inspect_systemnnn_spt import inspect_spt, load_sjis_extension


ROOT = Path(__file__).resolve().parents[1]
PRINT_WORD = 1411
VOICE_WORD = 1405
MAX_SCRIPT_TEXT_BYTES = 480


def quote_script(text):
    escaped = (text.replace("\\", "\\\\").replace('"', '\\"')
               .replace("{", "\\{").replace("\r\n", "\n").replace("\r", "\n")
               .replace("\n", "\\n"))
    return f'"{escaped}"'


def get_excerpt(report):
    records = {record["word"]: record for record in report["records"]}
    voice = records.get(VOICE_WORD)
    dialogue = records.get(PRINT_WORD)
    if not voice or voice.get("kind") != "voice" or voice.get("string_index") != 27:
        raise ValueError("opening voice reference at word 1405 did not match the known entry")
    if voice.get("voice_name", {}).get("text") != "yk180001":
        raise ValueError("opening voice name at word 1405 did not match the expected asset")
    if not dialogue or dialogue.get("kind") != "print" or dialogue.get("message_index") != 1:
        raise ValueError("opening dialogue at word 1411 did not match the known message")
    value = dialogue.get("message")
    if not value or "text" not in value:
        raise ValueError("decoded message text is required; check --sjis-ext")
    text = value["text"]
    if "\x00" in text or "#" in text:
        raise ValueError("message contains unsupported inline controls")
    if "\r" in text.replace("\r\n", ""):
        raise ValueError("message contains an unsupported carriage return")
    lines = text.split("\r\n")
    if len(lines) != 2 or not lines[0] or not lines[1]:
        raise ValueError("expected one speaker line followed by one dialogue line")
    speaker, body = lines
    if (not speaker.isascii() or not speaker.replace("_", "").isalnum()
            or not (speaker[0].isalpha() or speaker[0] == "_") or len(speaker) > 31):
        raise ValueError("speaker label is not safe for a 3DVN character identifier")
    if len(body.encode("utf-8")) > MAX_SCRIPT_TEXT_BYTES:
        raise ValueError("dialogue exceeds the 3DVN script text limit")
    return speaker, body


def validate_voice(path):
    try:
        with wave.open(str(path), "rb") as audio:
            if (audio.getnchannels(), audio.getsampwidth(), audio.getframerate()) != (1, 2, 32000):
                raise ValueError("voice must be mono 16-bit PCM WAV at 32 kHz")
            if not audio.getnframes():
                raise ValueError("voice file is empty")
    except wave.Error as exc:
        raise ValueError(f"invalid WAV file: {exc}") from exc


def build_preview(source, extension_path, background, voice_path, output):
    if output.exists():
        raise ValueError(f"output already exists: {output}")
    if Image is None:
        raise ValueError("Pillow is required to resize the background")
    if background.suffix.lower() not in (".jpg", ".jpeg", ".png") or not background.is_file():
        raise ValueError("background must be an existing JPEG or PNG")
    if voice_path.name.casefold() != "yk180001.wav" or not voice_path.is_file():
        raise ValueError("voice must be the extracted yk180001.wav entry")
    validate_voice(voice_path)
    extension = load_sjis_extension(extension_path)
    report = inspect_spt(source.read_bytes(), include_text=True, extension=extension)
    speaker, body = get_excerpt(report)

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".3dvn-gss-opening-", dir=output.parent) as temp:
        staging = Path(temp) / "game"
        (staging / "bg").mkdir(parents=True)
        (staging / "audio").mkdir()
        bg_name = "gss_opening.jpg"
        try:
            with Image.open(background) as source_image:
                ImageOps.fit(source_image.convert("RGB"), (400, 240),
                             method=Image.Resampling.LANCZOS).save(
                                 staging / "bg" / bg_name, format="JPEG", quality=90)
        except OSError as exc:
            raise ValueError(f"cannot decode background image: {exc}") from exc
        shutil.copyfile(voice_path, staging / "audio" / "yk180001.wav")
        identifier = speaker.lower()
        script = (
            "# Approximate excerpt from the local System-NNN opening.\n"
            f"character {identifier} {quote_script(speaker)}\n"
            "label opening\n"
            f"bg gss_opening\n"
            "sfx yk180001\n"
            f"{identifier}: {quote_script(body)}\n"
            "menu\n"
            "  choice \"Replay this excerpt\"\n"
            "    jump opening\n"
            "  choice \"End preview\"\n"
            "    stop\n"
            "end\n"
        )
        (staging / "main.3vs").write_text(script, encoding="utf-8", newline="\n")
        (staging / "game.3vc").write_text(
            "menu_title System-NNN Opening Excerpt\n"
            "default_display adv\n"
            "accent #C59052\n", encoding="utf-8", newline="\n")
        (staging / "PREVIEW.txt").write_text(
            "Approximate opening excerpt, not a complete conversion.\n"
            "Starts at the first inspected spoken line (op1.spt word 1411), "
            "after the preceding opening montage.\n"
            "Uses the locally available park background and associated voice. "
            "The original animated effects, music sequence, timing, complete "
            "choice flow, and speaker presentation are not reproduced.\n"
            "The two choices are preview controls, not original game choices.\n"
            "This output includes game assets and should remain private.\n",
            encoding="utf-8", newline="\n")
        staging.rename(output)
    return speaker


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "gss/spt/op1.spt")
    parser.add_argument("--sjis-ext", type=Path, default=ROOT / "gss/sjis_ext.bin")
    parser.add_argument("--background", type=Path,
                        default=ROOT / "gss-opening-extracted/bg0/bg020300hy.jpg")
    parser.add_argument("--voice", type=Path,
                        default=ROOT / "gss-opening-extracted/voices/yk180001.wav")
    parser.add_argument("--output", type=Path, default=ROOT / "games/gss-opening-preview")
    args = parser.parse_args()
    try:
        speaker = build_preview(args.source, args.sjis_ext, args.background, args.voice, args.output)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"opening preview failed: {exc}\n")
    print(f"Built approximate opening excerpt for speaker {speaker!r} at {args.output}")


if __name__ == "__main__":
    main()
