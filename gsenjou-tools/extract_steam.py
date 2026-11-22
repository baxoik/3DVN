#!/usr/bin/env python3
"""Unpack your Steam copy of G-Senjou no Maou for conversion: archives, scripts, images.

usage: extract_steam.py STEAM_DIR OUT_DIR [--jobs N]

STEAM_DIR is the game's install folder (the one holding data.xp3 and arc/). Writes:
  OUT_DIR/data/...        scripts and settings, as UTF-8 text, with patch.xp3, patch2.xp3,
                          ... applied on top in that order, as the game itself does (an
                          unrated patch is one of these; then the adult scenes are built in)
  OUT_DIR/<archive>/...   files of arc/<archive>.xp3: bgimage, evimage, fgimage, face,
                          image, others, bgm, sound and, if your copy has it, voice
                          (a numbered extra archive such as evimage2.xp3 is merged into
                          its base folder, after it)
  OUT_DIR/patch/          images from the patches (they take the place of the originals)
  OUT_DIR/png/<archive>/  the .tlg images decoded to PNG; sprite and face expressions
                          pasted onto their base image (tools/decode_tlg.py --compose)

Pure Python (numpy and Pillow for the images); no Wine, GARbro or KrkrExtract.

XP3: a header, the file data, and a (usually zlib-packed) index of "File" chunks: name
("info"), data segments ("segm") and the Adler-32 checksum of the plain file ("adlr").
The Steam release encrypts the files with a one-byte XOR: every byte after the first five
is XORed with (adler32 >> 12) & 0xFF. Each file is checked against its checksum, so a
different scheme is noticed instead of writing broken files.

Scripts (.ks, .tjs) are stored with KiriKiri's "FE FE" text obfuscation (mode 0 XOR,
mode 1 swapped bits, mode 2 zlib); they are written out as plain UTF-8.
"""
import argparse, concurrent.futures, os, re, struct, subprocess, sys, zlib

MAGIC = b"XP3\r\n \n\x1a\x8b\x67\x01"
TEXT_EXTS = (".ks", ".tjs", ".txt", ".csv", ".ini", ".asd", ".func", ".sli")


def read_index(handle):
    handle.seek(0)
    if handle.read(11) != MAGIC:
        raise ValueError("not an XP3 archive")
    offset = struct.unpack("<Q", handle.read(8))[0]
    if offset == 0x17:   # version 2 header: the index offset is stored at 0x20
        handle.seek(0x20)
        offset = struct.unpack("<Q", handle.read(8))[0]
    while True:
        handle.seek(offset)
        flag = handle.read(1)[0]
        if flag & 0x80:   # continuation to another index
            offset = struct.unpack("<Q", handle.read(8))[0]
            continue
        break
    if flag & 7 == 1:
        packed, size = struct.unpack("<QQ", handle.read(16))
        return zlib.decompress(handle.read(packed))
    size = struct.unpack("<Q", handle.read(8))[0]
    return handle.read(size)


def entries(index):
    pos = 0
    while pos < len(index):
        tag = index[pos:pos + 4]
        size = struct.unpack_from("<Q", index, pos + 4)[0]
        body = index[pos + 12:pos + 12 + size]
        pos += 12 + size
        if tag != b"File":
            continue
        entry = {"segments": [], "adler": None}
        inner = 0
        while inner < len(body):
            kind = body[inner:inner + 4]
            length = struct.unpack_from("<Q", body, inner + 4)[0]
            chunk = body[inner + 12:inner + 12 + length]
            inner += 12 + length
            if kind == b"info":
                flags, _size, _packed, name_len = struct.unpack_from("<IQQH", chunk)
                entry["encrypted"] = bool(flags & 0x80000000)
                entry["name"] = chunk[22:22 + name_len * 2].decode("utf-16le")
            elif kind == b"segm":
                entry["segments"] = [struct.unpack_from("<IQQQ", chunk, i) for i in range(0, len(chunk), 28)]
            elif kind == b"adlr":
                entry["adler"] = struct.unpack_from("<I", chunk)[0]
        if "name" in entry:
            yield entry


def file_data(handle, entry):
    data = bytearray()
    for flags, start, _size, packed in entry["segments"]:
        handle.seek(start)
        raw = handle.read(packed)
        data += zlib.decompress(raw) if flags & 7 else raw
    if entry["encrypted"] and len(data) > 5:
        key = (entry["adler"] >> 12) & 0xFF
        if key:
            body = memoryview(data)[5:]
            data[5:] = bytes(b ^ key for b in body) if len(body) < 64 else \
                (int.from_bytes(body, "little") ^ int.from_bytes(bytes([key]) * len(body), "little")).to_bytes(len(body), "little")
    if entry["adler"] is not None and (zlib.adler32(bytes(data)) & 0xFFFFFFFF) != entry["adler"]:
        raise ValueError(f"{entry['name']}: checksum mismatch (unknown encryption?)")
    return bytes(data)


def fefe_text(data):
    """KiriKiri's obfuscated text ("FE FE mode FF FE ...") or plain text, as a str."""
    if data[:2] == b"\xfe\xfe" and len(data) >= 5:
        mode, body = data[2], data[5:]
        if mode == 2:
            packed, _size = struct.unpack_from("<QQ", body)
            return zlib.decompress(body[16:16 + packed]).decode("utf-16le", "replace")
        units = struct.unpack("<%dH" % (len(body) // 2), body[:len(body) // 2 * 2])
        if mode == 1:
            units = [((u & 0xAAAA) >> 1) | ((u & 0x5555) << 1) for u in units]
        else:
            units = [u ^ (((u & 0xFE) << 8) ^ 1) if u >= 0x20 else u for u in units]
        return struct.pack("<%dH" % len(units), *units).decode("utf-16le", "replace")
    if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return data.decode("utf-16", "replace")
    try:
        return data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return data.decode("cp932", "replace")


IMAGE_EXTS = (".tlg", ".png", ".jpg", ".jpeg", ".bmp")


def unpack(archive, out_dir, place=None):
    """Writes the archive's files under out_dir, or where place(name) says."""
    count = 0
    with open(archive, "rb") as handle:
        for entry in entries(read_index(handle)):
            name = entry["name"].replace("\\", "/").lstrip("/")
            if not name or name.endswith("/") or name.startswith("$$$") or len(name) > 200:
                continue   # directories, and the "This is a protected archive" notice entry
            data = file_data(handle, entry)
            target = place(name) if place else os.path.join(out_dir, name)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            if name.lower().endswith(TEXT_EXTS):
                with open(target, "w", encoding="utf-8", newline="") as text:
                    text.write(fefe_text(data))
            else:
                with open(target, "wb") as binary:
                    binary.write(data)
            count += 1
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("steam")
    parser.add_argument("out")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = parser.parse_args()
    def number(name):   # patch.xp3 = 1, patch2.xp3 = 2, ...; evimage.xp3 = 1, evimage2.xp3 = 2
        digits = re.search(r"(\d*)\.xp3$", name).group(1)
        return int(digits) if digits else 1

    if not os.path.exists(os.path.join(args.steam, "data.xp3")):
        sys.exit(f"{args.steam}: no data.xp3 here; give the game's install folder")
    print(f"data.xp3: {unpack(os.path.join(args.steam, 'data.xp3'), os.path.join(args.out, 'data'))} files")
    # arc/: one job per folder; within a folder base archive first, then evimage2.xp3 ...
    groups = {}
    arc = os.path.join(args.steam, "arc")
    for name in sorted(os.listdir(arc)) if os.path.isdir(arc) else []:
        if name.lower().endswith(".xp3"):
            folder = re.sub(r"\d+$", "", name[:-4])
            groups.setdefault(folder, []).append(os.path.join(arc, name))

    def unpack_group(folder):
        counts = []
        for path in sorted(groups[folder], key=lambda p: number(os.path.basename(p))):
            counts.append(f"{os.path.basename(path)}: {unpack(path, os.path.join(args.out, folder))} files")
        return counts

    with concurrent.futures.ThreadPoolExecutor(max_workers=min(4, args.jobs)) as pool:
        for counts in pool.map(unpack_group, sorted(groups)):
            print("\n".join(counts))

    # Patches last, in order (patch.xp3, patch2.xp3, ...; an unrated patch is one of them).
    # KiriKiri finds files by name in whichever archive has them, the latest patch first,
    # and patches store them without folders. So a patch file replaces the file of the same
    # name wherever it was unpacked; scripts go to data/scenario; images go to patch/,
    # which the converter prefers over the other folders (a patch's .jpg replaces a .tlg).
    existing = {}
    for folder, _dirs, files in os.walk(args.out):
        if os.path.relpath(folder, args.out).split(os.sep)[0] in ("png", "patch"):
            continue
        for name in files:
            existing.setdefault(name.lower(), os.path.join(folder, name))

    def patch_place(name):
        if "/" in name:
            return os.path.join(args.out, "data", name)
        if name.lower().endswith(IMAGE_EXTS):
            return os.path.join(args.out, "patch", name)
        if name.lower() in existing:
            return existing[name.lower()]
        if name.lower().endswith(".ks"):
            return os.path.join(args.out, "data", "scenario", name)
        return os.path.join(args.out, "data", name)

    patches = sorted((name for name in os.listdir(args.steam) if re.fullmatch(r"patch\d*\.xp3", name, re.I)),
                     key=number)
    for name in patches:
        print(f"{name}: {unpack(os.path.join(args.steam, name), None, patch_place)} files")
    if not os.path.isdir(os.path.join(args.out, "voice")):
        print("no voice.xp3: the game will be built without voices")
    decoder = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools", "decode_tlg.py")
    sources = [os.path.join(args.out, folder) for folder in sorted(os.listdir(args.out))
               if folder not in ("png", "voice", "bgm", "sound") and os.path.isdir(os.path.join(args.out, folder))]
    command = [sys.executable, "-I", decoder, os.path.join(args.out, "png"), "--compose", "--jobs", str(args.jobs)]
    for source in sources:
        command += ["--from", source]
    if subprocess.run(command).returncode != 0:
        # e.g. image/g_novel.tlg (TLG6, a menu picture the conversion doesn't use)
        print("some images could not be decoded (listed above); the conversion reports any it needs")


if __name__ == "__main__":
    main()
