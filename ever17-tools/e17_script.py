"""Ever17 (KID, English PC release) script decoding: SC3 bytecode -> decoded operations.

Ported from Ever17Converter by weeaboo (GPLv2): nl.weeaboo.kid.ever17.ScriptConverter
(decode / decodeOp), Ever17Util.readExpr, StringCommandReader and the opcode tables.
`render_dec` reproduces the Java converter's .dec listing exactly, which is how this
port is checked. This file is GPLv2 as well.
"""
import struct

OPCODES = {
    0x00: "end", 0x01: "jump", 0x03: "playBGM", 0x04: "stopBGM", 0x05: "playSFX", 0x06: "stopSFX",
    0x07: "waitForSFX", 0x0A: "gotoif", 0x0C: "bgload", 0x0D: "removeBG", 0x0F: "fgload",
    0x10: "removeFG", 0x12: "multifgload2", 0x13: "multiremoveFG2", 0x14: "multiremoveFG3",
    0x16: "multifgload3", 0x18: "hideTextbox", 0x19: "showTextbox", 0x1A: "choiceId",
    0x1D: "chapterCutin", 0x1E: "delay", 0x1F: "clock", 0x20: "unknown20", 0x21: "unknown21",
    0x24: "scriptLocationId", 0x26: "_switch", 0x27: "overlayImage", 0x28: "_switch3",
    0x37: "unknown37", 0x40: "specialEffectImage", 0x41: "tweenZoom", 0x45: "specialEffect",
    0x46: "unknown46", 0xFE: "varop", 0xFF: "text",
}
OPCODE_ARGS = {"bgload": 5, "removeBG": 3, "fgload": 5, "removeFG": 2, "multifgload2": 9,
               "multiremoveFG2": 2, "multiremoveFG3": 3, "multifgload3": 10, "jump": 1, "playBGM": 2,
               "playSFX": 3, "choiceId": 2, "chapterCutin": 2, "delay": 1, "clock": 2, "unknown20": 1,
               "unknown21": 1, "scriptLocationId": 1, "_switch": 1, "overlayImage": 1, "_switch3": 1,
               "unknown37": 2, "specialEffectImage": 8, "tweenZoom": 5, "specialEffect": 2,
               "unknown46": 1, "text": 1}
# Opcodes that appear without the 0x10 command prefix.
BARE_OPS = {"end", "waitForSFX", "gotoif", "_switch", "varop", "text"}
STR_OPCODES = {0x04: "textboxColor", 0x05: "clearText", 0x0B: "choice", 0x0C: "waitForSound",
               0x0D: "sound", 0x0E: "unknownE", 0x10: "appendText", 0x11: "unknown11"}
COMPARE = {0x0C14: "==", 0x0D14: "!=", 0x0E14: "<=", 0x0F14: ">=", 0x1014: "< ", 0x1114: "> "}
ASSIGN = {0x1414: ":=", 0x1714: "+="}


def _hex(value, width=2):
    return format(value & ((1 << (4 * width)) - 1), f"0{width}x")


class Reader:
    """Byte reader with a limit, mirroring Java's ByteBuffer semantics where it matters."""

    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.limit = len(data)

    def remaining(self):
        return self.limit - self.pos

    def peek(self):
        return self.data[self.pos] if self.pos < self.limit else 0

    def u8(self):
        if self.pos >= self.limit:
            raise EOFError
        value = self.data[self.pos]
        self.pos += 1
        return value

    def s8(self):
        value = self.u8()
        return value - 256 if value >= 128 else value

    def u16(self):
        if self.pos + 2 > self.limit:
            raise EOFError
        value = struct.unpack_from("<H", self.data, self.pos)[0]
        self.pos += 2
        return value

    def i32(self):
        if self.pos + 4 > self.limit:
            raise EOFError
        value = struct.unpack_from("<i", self.data, self.pos)[0]
        self.pos += 4
        return value

    def cstring(self):
        end = self.pos
        while end < self.limit and self.data[end] != 0:
            end += 1
        text = self.data[self.pos:end].decode("utf-8", errors="replace")
        self.pos = min(end + 1, self.limit)
        return text

    def expr(self):
        """Ever17Util.readExpr: a constant, variable or global reference."""
        a0 = self.u8()
        if a0 == 0xC0:
            return f"VAR_{a0:02x}_{self.u8()}"
        if 0xC1 <= a0 <= 0xCF:
            a, b = self.u16(), self.u16()
            return f"VAR_{a0:02x}_{a}_{b}"
        if 0xA0 <= a0 <= 0xAF:
            a, b, nil = self.u8(), self.u8(), self.s8()
            if b == 0 and nil == 0:
                return str(256 * (a0 - 0xA0) + a)
            if a0 == 0xA4:
                return f"v_{a:02x}"
            return f"CONST_{a0:02x}_{a:02x}_{b:02x}"
        if 0x80 <= a0 <= 0x8F:
            self.u8()
            self.s8()
            return str(a0 - 0x80)
        if a0 == 0x28:
            self.s8()
            return self.expr()
        return str(a0)


CUSTOM_GLYPHS = {0xA2: "c", 0xA3: "C", 0xA4: "o"}


class TextReader:
    """StringCommandReader: game text with its inline commands, as {command args} markup."""

    def __init__(self, data):
        self.r = Reader(data)

    def read_char(self):
        r = self.r
        c = r.u8()
        # The English release is Windows-1252 text, not Shift-JIS as the Java reader
        # assumed (that ate the byte after é, ’ etc.); only 0x81 and 0x87 are 2-byte
        # codes. 0xA2-0xA4 are custom font glyphs used in the names Coco and You.
        if c == 0x01 and r.pos >= 2 and r.data[r.pos - 2] == 0x2D:
            return ""          # soft break inside a hyphenated word ("already-|cultivated")
        if c in (0x01, 0x02, 0x10):
            c = 0x20
        if c == 0x03:
            c = 0x0A
        if c == 0x81:
            r.u8()
            return "."
        if c == 0x87:
            r.u8()
            return "--"
        if c in CUSTOM_GLYPHS:
            return CUSTOM_GLYPHS[c]
        return bytes([c]).decode("cp1252", errors="replace")

    def choice_option(self):
        r = self.r
        r.u8()                                     # 0x0B marker
        out = []
        while r.peek() not in (0x0B, 0x00):
            if r.peek() == 0x28:
                a0, a1, a2, var, op, a3 = (r.u8() for _ in range(6))
                out.append(f"{{cond ({a0:02x} {a1:02x} {a2:02x}) {var:02x} {op:02x} ({a3:02x})}}")
            else:
                out.append(self.read_char().replace("\n", " "))
        return "".join(out)

    def parse(self, offset):
        r = self.r
        r.pos = offset
        out = []
        while r.peek() != 0:
            c = r.peek()
            op = STR_OPCODES.get(c)
            if op is None:
                out.append(self.read_char())
                continue
            r.u8()
            if op == "textboxColor":
                out.append(f"{{{op} {r.i32() & 0xFFFFFFFF:08x}}}")
            elif op == "clearText":
                out.append(f"{{{op} {r.expr()}}}")
            elif op == "sound":
                out.append(f"{{{op} {r.cstring()}}}")
            elif op == "waitForSound":
                out.append(f"{{{op}}}")
            elif op == "choice":
                a0, a1 = r.u8(), r.u16()
                out.append(f"{{{op} {a0:02x} {a1:04x} ")
                while r.peek() != 0:
                    out.append("|" + self.choice_option())
                r.u8()
                out.append("}")
                break
            elif op == "appendText":
                out.append(f"{{{op} {r.u8():02x}}}")
            elif op == "unknown11":
                out.append(f"{{{op}}}")
        return "".join(out)


class Scene:
    """One .scr file: tables plus the decoded operation list."""

    def __init__(self, data):
        if struct.unpack_from("<I", data, 0)[0] != 0x00334353:      # 'SC3\0'
            raise ValueError("not an SC3 script")
        self.data = data
        text_off, gfx_off, header = struct.unpack_from("<III", data, 4)
        self.jump_table = [header] + list(struct.unpack_from(f"<{(header - 12) // 4 - 1}i", data, 16))
        self.text_table = list(struct.unpack_from(f"<{(gfx_off - text_off) // 4}i", data, text_off))
        self.gfx_table = list(struct.unpack_from(f"<{(len(data) - gfx_off) // 4}i", data, gfx_off))
        self.code_end = min(text_off, gfx_off)

    def text(self, offset):
        return TextReader(self.data).parse(offset)

    def target(self, index):
        return self.jump_table[index] if 0 <= index < len(self.jump_table) else -1

    def decode(self):
        """[(address, opcode byte, decoded text or None)] like ScriptConverter.decode."""
        r = Reader(self.data)
        r.pos, r.limit = self.jump_table[0], self.code_end
        ops, command = [], False
        while r.remaining() > 0:
            addr = r.pos
            code = r.u8()
            text = None
            if command:
                command = False
                name = OPCODES.get(code)
                if name is None:
                    text = f"unsupported {code} "
                else:
                    text = self._safe(r, name)
            elif code == 0x10:
                command = True
            elif OPCODES.get(code) in BARE_OPS:
                text = self._safe(r, OPCODES[code])
            ops.append((addr, code, text))
        return ops

    def _safe(self, r, name):
        try:
            return self.decode_op(r, name)
        except (EOFError, struct.error, IndexError):
            return None

    def decode_op(self, r, op):
        gfx = lambda index: self.text(self.gfx_table[index])
        if op == "end":
            return op
        if op == "removeFG":
            return f"{op} {r.expr()} {r.expr()}"
        if op == "jump":
            return f"{op} {r.cstring()}"
        if op == "bgload":
            a0, idx = r.i32(), r.u16()
            n1, n2 = r.expr(), r.expr()
            return f"bgload {a0 & 0xFFFFFFFF:08x} {gfx(idx)} {n1} {n2}"
        if op == "removeBG":
            return f"removeBG {r.expr()} {r.expr()} {r.expr()}"
        if op == "fgload":
            n1 = r.expr()
            a0, idx = r.i32(), r.u16()
            x, n3 = r.expr(), r.expr()
            return f"fgload {n1} {a0 & 0xFFFFFFFF:08x} {gfx(idx)} {x} {n3}"
        if op == "multifgload2":
            a0, a1 = r.expr(), r.expr()
            a2, i1, a4, i2 = r.i32(), r.u16(), r.i32(), r.u16()
            x1, x2, a8 = r.expr(), r.expr(), r.expr()
            return (f"{op} {a0} {a1} {a2 & 0xFFFFFFFF:08x} {gfx(i1)} {a4 & 0xFFFFFFFF:08x} {gfx(i2)} "
                    f"{x1} {x2} {a8}")
        if op == "multifgload3":
            a0, i1, a1, i2, a2, i3 = r.i32(), r.u16(), r.i32(), r.u16(), r.i32(), r.u16()
            a3, a4, a5, a6 = r.expr(), r.expr(), r.expr(), r.expr()
            return (f"{op} {a0 & 0xFFFFFFFF:08x} {gfx(i1)} {a1 & 0xFFFFFFFF:08x} {gfx(i2)} "
                    f"{a2 & 0xFFFFFFFF:08x} {gfx(i3)} {a3} {a4} {a5} {a6}")
        if op == "multiremoveFG2":
            a0, a1, a2 = r.u8(), r.u8(), r.u8()
            return f"{op} {a0:02x} {a1:02x} {a2:02x} {r.expr()}"
        if op == "multiremoveFG3":
            return f"{op} {r.expr()} {r.expr()} {r.expr()}"
        if op == "clock":
            return f"clock {r.expr()}:{r.expr()}"
        if op == "text":
            idx = r.u16()
            body = self.text(self.text_table[idx]) if 0 <= idx < len(self.text_table) else "***"
            return f"{op} {idx:02x}\n{body}\n"
        if op in ("hideTextbox", "showTextbox", "stopBGM", "stopSFX"):
            return op
        if op == "playBGM":
            return f"playBGM {r.expr()} {r.expr()}"
        if op == "playSFX":
            name = r.cstring()
            return f"playSFX {name} {r.expr()} {r.expr()}"
        if op == "gotoif":
            a0, a1, a2, a3, var = (r.u8() for _ in range(5))
            cmp = r.u16()
            operator = COMPARE.get(cmp, f"{cmp:04x}")
            a4 = r.u8()
            v = r.peek()
            if 0x80 <= v <= 0x8F:
                val = str(r.u8() - 0x80)
            elif 0xA0 <= v <= 0xAF:
                v = r.u8()
                val = str(256 * (v - 0xA0) + r.u8())
            else:
                val = "00"
            a7, idx = r.u16(), r.u16()
            return (f"{op} {a0} ({a1:02x} {a2:02x} {a3:02x}) {var:02x} {operator} ({a4:02x}) {val} "
                    f"({a7:04x}) -> {self.target(idx) & 0xFFFFFFFF:08x} ({idx:08x})")
        if op == "choiceId":
            return f"{op} {r.expr()} {r.expr()}"
        if op == "_switch":
            lines = [f"{op}"]
            a0, a1, a2, var = r.u8(), r.u8(), r.u8(), r.u8()
            operator, nil = r.u16(), r.u8()
            lines.append(f"switch_varop ({a0:02x} {a1:02x} {a2:02x}) {var:02x} {operator:04x} ({nil:02x})")
            body = []
            while r.peek() != 0x26:
                sub = r.u8()
                arg = r.expr()
                idx = r.u16()
                body.append(f"{sub:02x} -> {arg} {self.target(idx) & 0xFFFFFFFF:08x} ({idx:08x})\n")
                if r.peek() == 0:
                    r.u8()
                else:
                    break
            return "\n".join(lines) + "\n" + "".join(body)
        if op == "overlayImage":
            a0, idx = r.i32(), r.u16()
            name = gfx(idx)
            return f"{op} {a0 & 0xFFFFFFFF:08x} {name} {r.expr()} {r.expr()}"
        if op == "specialEffectImage":
            a0, idx = r.i32(), r.u16()
            args = [r.expr() for _ in range(6)]
            return f"{op} {a0 & 0xFFFFFFFF:08x} {gfx(idx)} " + " ".join(args)
        if op == "tweenZoom":
            return f"{op} " + " ".join(r.expr() for _ in range(5))
        if op == "waitForSFX":
            if r.peek() != 0x10:
                mark = r.pos
                idx = r.u16()
                if 0 <= idx < len(self.jump_table):
                    return f"goto {self.jump_table[idx] & 0xFFFFFFFF:08x} ({idx:08x})"
                r.pos = mark
            return op
        if op == "varop":
            a0, a1, a2, a3 = r.u8(), r.u8(), r.u8(), r.u8()
            code = r.u16()
            operator = ASSIGN.get(code, f"{code:04x}")
            a4 = r.u8()
            return f"{op} ({a0:02x} {a1:02x} {a2:02x}) {a3:02x} {operator} ({a4:02x}) {r.expr()}"
        if op in ("chapterCutin", "unknown37"):
            a0, idx = r.i32(), r.u16()
            return f"{op} {a0 & 0xFFFFFFFF:08x} {gfx(idx)}"
        if op in ("delay", "unknown20", "unknown21", "scriptLocationId", "unknown46"):
            return f"{op} {r.expr()}"
        if op == "specialEffect":
            return f"{op} {r.expr()} {r.expr()}"
        return None


def render_dec(ops):
    """The Java converter's .dec text for a decoded scene."""
    out = []
    for addr, code, text in ops:
        out.append(f"[{addr:08x}] {code:2x}: " + (text or "") + "\n")
    return "".join(out)
