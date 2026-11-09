#!/usr/bin/env python3
"""Inspect the observed System-NNN SPT layout without executing the script."""

import argparse
import json
import struct
from pathlib import Path


MAX_FILE_SIZE = 64 * 1024 * 1024
DATA = 0x66660001
CONTROL = 0x66660002
SYSTEM_COMMAND = 0x66660006
SYSTEM_FUNCTION = 0x66660007
PRINTS = {0x22220001: "print", 0x22220002: "lprint", 0x22220003: "append"}
NAMES = {0x33330050: "film_name", 0x33330051: "story_name"}
SELECT = 0x22220006
GOTO = 0x55560020
SCRIPT_CALL = 0x5556000A
BLOCK_OPEN = 0xA000007B
BLOCK_CLOSE = 0xA000007D


def load_sjis_extension(path):
    with path.open("rb") as file:
        data = file.read(19)
    if len(data) != 18:
        raise ValueError(f"{path}: expected a nine-entry, 18-byte SJIS extension table")
    values = struct.unpack("<9H", data)
    if any(value == 0 or 0xD800 <= value <= 0xDFFF for value in values):
        raise ValueError(f"{path}: invalid Unicode code point in SJIS extension table")
    return tuple(chr(value) for value in values)


def decode_text(raw, extension=None):
    if extension is None:
        try:
            return {"text": raw.decode("cp932")}
        except UnicodeDecodeError as exc:
            return {"encoding_error": f"invalid CP932 at byte {exc.start}",
                    "raw_hex": raw.hex()}
    output = []
    pos = 0
    while pos < len(raw):
        first = raw[pos]
        length = 2 if 0x81 <= first <= 0x9f or 0xe0 <= first <= 0xfc else 1
        if first == 0x81 and pos + 1 < len(raw) and 1 <= raw[pos + 1] <= len(extension):
            output.append(extension[raw[pos + 1] - 1])
        else:
            chunk = raw[pos:pos + length]
            try:
                output.append(chunk.decode("cp932"))
            except UnicodeDecodeError:
                return {"encoding_error": f"invalid CP932 at byte {pos}", "raw_hex": raw.hex()}
        pos += length
    return {"text": "".join(output)}


def inspect_spt(raw, include_text=False, extension=None):
    """Return a physical record listing; it is not an execution trace."""
    if len(raw) > MAX_FILE_SIZE:
        raise ValueError("SPT exceeds the 64 MiB limit")
    if len(raw) < 80 or len(raw) % 4:
        raise ValueError("SPT must contain an aligned 20-word header")
    data = bytes(value ^ 0xff for value in raw)
    if data[0x30:0x3a] != b"SPTHEADER0":
        raise ValueError("missing SPTHEADER0 (unsupported encryption or format)")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] < 20 or words[0] > len(words) or words[1] != DATA:
        raise ValueError("invalid header record")

    def table(count, start, name):
        if start > len(words) or count > len(words) - start:
            raise ValueError(f"{name} table is outside the file")
        return words[start:start + count]

    messages = table(words[4], words[5], "message")
    strings = table(words[6], words[7], "string")
    films = table(words[8], words[9], "film")
    warnings = []
    for name, start, offsets, subtype in (
            ("message", words[5], messages, 0x44440002),
            ("string", words[7], strings, 0x44440001)):
        if start < 4 or words[start - 3:start] != (DATA, 0x55550002, subtype):
            warnings.append(f"{name} table record header not recognized")
            continue
        record_start = start - 4
        record_end = record_start + words[record_start]
        if record_end > len(words) or record_end < start + len(offsets):
            warnings.append(f"{name} table record has an invalid declared extent")
        else:
            outside = sum(not start <= offset < record_end for offset in offsets)
            if outside:
                warnings.append(f"{name} table has {outside} pointer(s) outside its declared record")

    # In the observed game the film table is preceded by a four-word data
    # record header. Do not extrapolate this boundary to other SPT variants.
    end = words[9] - 4
    if (end < words[0] or end + 4 > len(words)
            or words[end + 1:end + 4] != (DATA, 0x55550002, 0x44440003)):
        raise ValueError("unrecognized instruction-region boundary")
    if words[end] < 4 or words[end] > len(words) - end or words[8] > words[end] - 4:
        raise ValueError("invalid film-table data record")

    def resolve(offsets, index, name):
        if index >= len(offsets):
            raise ValueError(f"{name} index {index} is outside the table")
        start = offsets[index] * 4
        if start >= len(data):
            raise ValueError(f"{name} index {index} points outside the file")
        stop = data.find(b"\0", start)
        if stop < 0:
            raise ValueError(f"{name} index {index} is not NUL-terminated")
        return decode_text(data[start:stop], extension)

    records = []
    blocks = []
    pc = 0
    while pc < end:
        length = words[pc]
        if length < 3 or length > end - pc:
            raise ValueError(f"invalid record length {length} at word {pc}")
        category, opcode = words[pc + 1:pc + 3]
        operands = list(words[pc + 3:pc + length]) if category != DATA else []
        record = {"word": pc, "length": length, "category": f"0x{category:08x}",
                  "opcode": f"0x{opcode:08x}", "operands": operands}
        if category == SYSTEM_COMMAND and opcode in PRINTS:
            if len(operands) < 2:
                raise ValueError(f"missing message index or serial at word {pc}")
            index = operands[0]
            text = resolve(messages, index, "message")
            record.update(kind=PRINTS[opcode], message_index=index)
            if include_text:
                record["message"] = text
            elif "encoding_error" in text:
                record["encoding_error"] = text["encoding_error"]
        elif category == SYSTEM_COMMAND and opcode == SELECT:
            if not operands:
                raise ValueError(f"invalid selection operands at word {pc}")
            count = operands[0]
            prompt_count = 0 if length == count + 5 else operands[-1]
            total = count + prompt_count
            if total > len(operands) - 2:
                raise ValueError(f"invalid selection operands at word {pc}")
            indices = operands[1:1 + total]
            texts = [resolve(strings, index, "string") for index in indices]
            record.update(kind="select", selection_count=count, prompt_count=prompt_count,
                          string_indices=indices, prompt_indices=indices[:prompt_count],
                          option_indices=indices[prompt_count:])
            if include_text:
                record["strings"] = texts
            else:
                errors = [item["encoding_error"] for item in texts if "encoding_error" in item]
                if errors:
                    record["encoding_errors"] = errors
        elif category == CONTROL and opcode == GOTO:
            if not operands:
                raise ValueError(f"missing goto target at word {pc}")
            record.update(kind="goto", target_word=operands[0])
        elif category == CONTROL and opcode == SCRIPT_CALL:
            if not operands:
                raise ValueError(f"missing script name index at word {pc}")
            name = resolve(strings, operands[0], "string")
            record.update(kind="script_call", string_index=operands[0])
            if include_text:
                record["script_name"] = name
            elif "encoding_error" in name:
                record["encoding_error"] = name["encoding_error"]
        elif category == SYSTEM_FUNCTION and opcode in NAMES:
            if len(operands) < 2:
                raise ValueError(f"missing {NAMES[opcode]} string index at word {pc}")
            name = resolve(strings, operands[1], "string")
            record.update(kind=NAMES[opcode], string_index=operands[1])
            if include_text:
                record["name"] = name
            elif "encoding_error" in name:
                record["encoding_error"] = name["encoding_error"]
        elif category == CONTROL and opcode == BLOCK_OPEN:
            if not operands:
                raise ValueError(f"missing block skip at word {pc}")
            record.update(kind="block_open", skip_target_word=pc + operands[0])
            blocks.append(record)
        elif category == CONTROL and opcode == BLOCK_CLOSE:
            if not blocks:
                raise ValueError(f"unmatched block close at word {pc}")
            opening = blocks.pop()
            if opening["skip_target_word"] != pc + length:
                raise ValueError(f"block at word {opening['word']} has an invalid skip target")
            opening["close_word"] = pc
            record.update(kind="block_close", open_word=opening["word"])
        records.append(record)
        pc += length

    if blocks:
        raise ValueError(f"unmatched block open at word {blocks[-1]['word']}")
    boundaries = {record["word"] for record in records}
    for index, target in enumerate(films):
        if target not in boundaries:
            raise ValueError(f"film table entry {index} is not a record boundary: {target}")
    for record in records:
        if record.get("kind") == "goto" and record["target_word"] not in boundaries:
            raise ValueError(f"goto at word {record['word']} does not target a record boundary")
    film_base = words[10]
    return {
        "format": "systemnnn-spt-inspection-v1",
        "notice": "Physical records only; not an execution trace or playable conversion.",
        "instruction_end_word": end,
        "message_count": len(messages),
        "string_count": len(strings),
        "film_base_index": film_base,
        "film_targets": list(films),
        "warnings": warnings,
        "records": records,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="SPT file using the default XOR key")
    parser.add_argument("--include-text", action="store_true",
                        help="include referenced game text in the JSON output")
    parser.add_argument("--sjis-ext", type=Path,
                        help="game-specific nine-entry SJIS extension table")
    args = parser.parse_args()
    try:
        if args.source.stat().st_size > MAX_FILE_SIZE:
            raise ValueError("SPT exceeds the 64 MiB limit")
        extension = load_sjis_extension(args.sjis_ext) if args.sjis_ext else None
        report = inspect_spt(args.source.read_bytes(), args.include_text, extension)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"SPT inspection failed: {exc}\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
