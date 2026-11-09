#!/usr/bin/env python3
"""Trace a supported subset of one System-NNN SPT, stopping on unknown behavior."""

import argparse
import json
from pathlib import Path

from inspect_systemnnn_spt import MAX_FILE_SIZE, inspect_spt, load_sjis_extension


CONTROL = "0x66660002"
DATA = "0x66660001"
SYSTEM_FUNCTION = "0x66660007"
CALCULATION = "0x66660005"
CASE = "0x55560008"
SUB = "0x55560009"
RETURN = "0x55560007"
NOP = "0x55560001"
END = "0x55560005"
NEXT = "0x55560006"
IF = "0x55560002"
ELSE = "0x55560004"
ELSIF = "0x55560003"
ENDIF = "0x55560013"
ENDKOMA = "0x5556000e"
FILM_TYPE_TIME = "0x33330052"
UNSUPPORTED_FUNCTION_NAMES = {
    "0x33330007": "music",
    "0x33330012": "clear_all_effects",
    "0x33330013": "set_effect",
    "0x33330014": "clear_effect_layer",
    "0x3333001b": "start_message",
    "0x3333001c": "start_koma",
    "0x33330057": "auto_message",
    "0x33330058": "cannot_click",
    "0x33330059": "cannot_skip",
    "0x3333005a": "option_off",
}


def skip_body(records, pc, skip_following_close=False):
    body = records.get(pc)
    if body is None:
        raise ValueError(f"branch body at word {pc} is not a record")
    if body.get("kind") == "block_open":
        target = body["skip_target_word"]
        closing = records.get(target)
        if skip_following_close and closing and closing.get("kind") == "block_close":
            target += closing["length"]
        return target
    return pc + body["length"]


def selection_cases(records, pc, count):
    first = records.get(pc)
    if first and first.get("kind") == "block_open":
        pc += first["length"]
    cases = {}
    for _ in range(count):
        case = records.get(pc)
        if not case or case["category"] != CONTROL or case["opcode"] != CASE:
            raise ValueError(f"expected selection case at word {pc}")
        if not case["operands"]:
            raise ValueError(f"missing case value at word {pc}")
        value = case["operands"][0] & 0xffff
        if value in cases:
            raise ValueError(f"duplicate case value {value} at word {pc}")
        body = pc + case["length"]
        cases[value] = body
        pc = skip_body(records, body, skip_following_close=True)
    return cases


def equality_expression(operands, variables):
    """Evaluate the supported variable/literal equality form, or report its limit."""
    if len(operands) != 5 or operands[:2] != [0xA0000000, 3] or operands[4] != 0xA500003D:
        return "unsupported", None
    values = []
    for token in operands[2:4]:
        value = token & 0xffff
        if token & 0xffff0000 == 0x80000000 and value < 0x8000:
            if value not in variables:
                return "state_required", value
            values.append(variables[value])
        elif token & 0xffff0000 == 0x90000000:
            values.append(value - 0x10000 if value & 0x8000 else value)
        else:
            return "unsupported", None
    return "ok", values[0] == values[1]


def trace(report, start_word, choices=(), max_steps=1000, variables=None):
    """Trace with caller-supplied state, stopping before unsupported operations."""
    if max_steps < 1:
        raise ValueError("step limit must be positive")
    variables = {} if variables is None else dict(variables)
    records = {record["word"]: record for record in report["records"]}
    if start_word not in records:
        raise ValueError(f"start word {start_word} is not a record boundary")
    selected = iter(choices)
    stack = []
    events = []
    pc = start_word
    for _ in range(max_steps):
        record = records.get(pc)
        if record is None:
            return {"status": "invalid_target", "word": pc, "events": events}
        category, opcode = record["category"], record["opcode"]
        kind = record.get("kind")
        next_pc = pc + record["length"]
        event = {"word": pc, "category": category, "opcode": opcode,
                 "operands": record["operands"]}
        if kind in ("print", "lprint", "append"):
            event.update(kind=kind, message_index=record["message_index"])
            if "message" in record:
                event["message"] = record["message"]
            elif "encoding_error" in record:
                event["encoding_error"] = record["encoding_error"]
        elif kind == "select":
            cases = selection_cases(records, next_pc, record["selection_count"])
            event.update(kind="select", prompt_indices=record["prompt_indices"],
                         option_indices=record["option_indices"], cases=cases)
            if "strings" in record:
                event["strings"] = record["strings"]
            try:
                choice = next(selected)
            except StopIteration:
                events.append(event)
                return {"status": "choice_required", "word": pc, "events": events}
            if choice not in cases:
                raise ValueError(f"case {choice} is invalid for selection at word {pc}")
            event["selected_case"] = choice
            next_pc = cases[choice]
        elif category == CONTROL and opcode == IF:
            status, value = equality_expression(record["operands"], variables)
            if status != "ok":
                if status == "state_required":
                    event["variable"] = value
                events.append(event)
                return {"status": status, "word": pc, "events": events}
            event.update(kind="if", condition=value)
            if not value:
                next_pc = skip_body(records, next_pc)
                following = records.get(next_pc)
                if following and following["category"] == CONTROL and following["opcode"] == ELSE:
                    next_pc += following["length"]
                elif following and following["category"] == CONTROL and following["opcode"] == ELSIF:
                    events.append(event)
                    events.append({"word": next_pc, "category": CONTROL, "opcode": ELSIF,
                                   "operands": following["operands"]})
                    return {"status": "unsupported", "word": next_pc, "events": events}
        elif category == CALCULATION:
            ops = record["operands"]
            if (len(ops) != 5 or ops[:2] != [0xA0000000, 3]
                    or ops[2] & 0xFFFF0000 != 0x80000000
                    or not 100 <= (ops[2] & 0xFFFF) < 0x8000
                    or ops[3] & 0xFFFF0000 != 0x90000000 or ops[4] != 0xA000003D):
                events.append(event)
                return {"status": "unsupported", "word": pc, "events": events}
            number = ops[3] & 0xFFFF
            number = number - 0x10000 if number & 0x8000 else number
            index = ops[2] & 0xFFFF
            variables[index] = number
            event.update(kind="set_variable", variable=index, value=number,
                         assumption="visual side effects of variable assignments are not modeled")
        elif category == CONTROL and opcode in (ELSE, ELSIF):
            next_pc = skip_body(records, next_pc)
            event.update(kind="skip_else", target_word=next_pc)
        elif kind == "goto":
            event.update(kind=kind, target_word=record["target_word"])
            next_pc = record["target_word"]
        elif kind in ("story_name", "film_name"):
            event.update(kind=kind, string_index=record["string_index"])
            if "name" in record:
                event["name"] = record["name"]
            elif "encoding_error" in record:
                event["encoding_error"] = record["encoding_error"]
        elif category == SYSTEM_FUNCTION and opcode == FILM_TYPE_TIME:
            if len(record["operands"]) < 2:
                raise ValueError(f"missing film type time operand at word {pc}")
            event.update(kind="film_type_time", raw_value=record["operands"][1],
                         assumption="reference callback has no effect")
        elif category == CONTROL and opcode == SUB:
            if len(record["operands"]) < 2:
                raise ValueError(f"missing subroutine operand at word {pc}")
            index = record["operands"][0]
            if index >= len(report["film_targets"]):
                raise ValueError(f"subroutine index {index} is outside the film table")
            if len(stack) >= 249:
                raise ValueError("subroutine stack limit exceeded")
            next_pc = report["film_targets"][index]
            stack.append(pc + record["length"])
            event.update(kind="sub", table_index=index, target_word=next_pc)
        elif category == CONTROL and opcode == RETURN:
            if not stack:
                events.append(event)
                return {"status": "return_without_caller", "word": pc, "events": events}
            next_pc = stack.pop()
            event.update(kind="return", target_word=next_pc)
        elif category == CONTROL and opcode == CASE:
            next_pc = skip_body(records, next_pc)
            event.update(kind="skip_case", target_word=next_pc)
        elif category == CONTROL and opcode == END:
            event["kind"] = "end"
            events.append(event)
            return {"status": "end", "word": pc, "events": events}
        elif category == CONTROL and opcode == NEXT:
            event["kind"] = "next_script"
            events.append(event)
            return {"status": "next_script", "word": pc, "events": events}
        elif category == CONTROL and opcode in (ENDIF, ENDKOMA):
            event["kind"] = "endif" if opcode == ENDIF else "endkoma"
        elif category == DATA or (category == CONTROL and opcode == NOP) or kind in (
                "block_open", "block_close"):
            event["kind"] = kind or ("data" if category == DATA else "nop")
        else:
            if category == SYSTEM_FUNCTION and opcode in UNSUPPORTED_FUNCTION_NAMES:
                event["unsupported_function"] = UNSUPPORTED_FUNCTION_NAMES[opcode]
            events.append(event)
            return {"status": "unsupported", "word": pc, "events": events}
        events.append(event)
        pc = next_pc
    return {"status": "step_limit", "word": pc, "events": events}


def parse_variable(text):
    try:
        index, value = text.split("=", 1)
        index, value = int(index, 0), int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected VARIABLE_NUMBER=VALUE") from exc
    if not 0 <= index < 0x8000 or not -(2 ** 31) <= value < 2 ** 31:
        raise argparse.ArgumentTypeError("variable index or 32-bit value out of range")
    return index, value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--start-word", type=int, required=True,
                        help="explicit record offset (not necessarily an actual game entry)")
    parser.add_argument("--case", type=int, action="append", default=[],
                        help="case value for each selection, in encounter order")
    parser.add_argument("--var", type=parse_variable, action="append", default=[],
                        help="explicit initial state as VARIABLE_NUMBER=VALUE; repeat as needed")
    parser.add_argument("--max-steps", type=int, default=1000)
    parser.add_argument("--include-text", action="store_true",
                        help="include local game text in the report")
    parser.add_argument("--sjis-ext", type=Path,
                        help="game-specific nine-entry SJIS extension table")
    args = parser.parse_args()
    try:
        if args.source.stat().st_size > MAX_FILE_SIZE:
            raise ValueError("SPT exceeds the 64 MiB limit")
        extension = load_sjis_extension(args.sjis_ext) if args.sjis_ext else None
        report = inspect_spt(args.source.read_bytes(), args.include_text, extension)
        if len(dict(args.var)) != len(args.var):
            raise ValueError("variable specified more than once")
        result = trace(report, args.start_word, args.case, args.max_steps, dict(args.var))
    except (OSError, ValueError) as exc:
        parser.exit(1, f"SPT trace failed: {exc}\n")
    result["notice"] = ("Partial trace from a caller-selected entry; dialogue is assumed "
                        "to advance. Unsupported commands are not executed.")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if result["status"] != "end":
        parser.exit(2)


if __name__ == "__main__":
    main()
