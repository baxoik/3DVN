#!/usr/bin/env python3
"""Dump what a Chaos;Head Noah PSP scene script (SCENE00.AFS/SCHxx_xxx.BIN) does:
the message text in order, interleaved with the image commands. For your own copy
of the game only.

  c0scene.py SCH01_002.BIN

Format findings (reverse engineered, partly unverified):
  - Instructions are [opcode u8][size u8][size-2 bytes of u16 arguments].
  - 0x18 with a u32 pointer at args[2:4] shows a message string (Shift-JIS, with
    %N newline, %P/%K wait for key, %Tnnn pause, %Onnn ... codes).
  - 0xC9 [layer, id, alpha, ...] loads an image. Layer 1 and 0 ids are 1..1399
    (BGL00.AFS entries); layer 2 ids are 0xC00+n (character sprite sets).
  - 0x9C [0x101, obj, slot] binds a texture slot to a display object.
  - Mapping of ids to the right pictures is NOT confirmed: several ids in
    SCH01_002 point at CGs that do not match the room scene.
"""
import re, struct, sys


def disassemble(d):
    m = re.search(rb"%[NPTO]", d)
    text_start = m.start() if m else len(d)
    while text_start > 0 and d[text_start - 1] >= 0x20:
        text_start -= 1
    out, p = [], 0x20
    while p < min(len(d) - 4, text_start):
        op, size = d[p], d[p + 1]
        if size < 2 or size % 2 or p + size > len(d):
            p += 2
            continue
        out.append((p, op, struct.unpack("<%dH" % ((size - 2) // 2), d[p + 2:p + size])))
        p += size
        if op == 0xFE and size == 2:
            while d[p:p + 2] == b"\x90\x90":
                p += 2
    return out


def main():
    d = open(sys.argv[1], "rb").read()
    for p, op, a in disassemble(d):
        if op == 0x18 and len(a) >= 4:
            ptr = a[2] | (a[3] << 16)
            if 0x1000 < ptr < len(d):
                text = d[ptr:d.find(b"\0", ptr)].decode("cp932", "replace").replace("\n", " ")
                if text.replace("%N", "").strip():
                    print("%05x  TEXT   %s" % (p, text))
        elif op == 0xC9 and len(a) == 5:
            print("%05x  IMAGE  layer=%d id=%d (0x%x) alpha=%d" % (p, a[0], a[1], a[1], a[2]))
        elif op in (0xCA, 0xCB, 0xCC, 0xF4, 0x9F):
            print("%05x  op%02X   %s" % (p, op, list(a)))


if __name__ == "__main__":
    main()
