#!/usr/bin/env python3
"""Sync 3DVN saves between a 3DS and a Wii U (or any two 3DVN folders), from a PC.

usage: sync_saves.py A B [--game NAME] [--dry-run] [--backup DIR]

A and B are 3DVN root folders, each one of:
  3ds:HOST[:PORT]      a 3DS running an FTP server (ftpd; port 5000), /3ds/3dvn
  wiiu:HOST[:PORT]     a Wii U running FTPiiU (port 21), /sd/wiiu/3dvn
  ftp://HOST[:PORT]/PATH   any FTP server and folder
  a local folder       e.g. a mounted SD card: /run/media/you/SD/3ds/3dvn

  python3 tools/sync_saves.py 3ds:192.168.0.30 wiiu:192.168.0.25

What it syncs, per game:
  saves/<game>_<slot>.3vsav   each slot both ways: the newer one (by the time it was
                              saved, stored in the file) replaces the older one
  saves/<game>.3vglobal       endings seen and other unlocks: merged (highest value of
                              each flag), so progress on either side counts on both
  settings/<game>.3vread      lines already read (for "fast-forward: read only"):
                              merged
Nothing is deleted. Every file that gets replaced is first copied to the backup folder
(default: save-backups/<date-time>/<side>/...).

The two sides need 3DVN builds from 2026-11-22 or later: their saves are in one byte
order on every console (older Wii U saves are converted when the newer build loads
them). Game names must match on both sides (the archive or folder name in games/).
"""
import argparse, ctypes, datetime, ftplib, io, os, re, struct, sys

SAVE_MAGIC = 0x32564433
SAVE_NAME = re.compile(r"^(?P<game>.+)_(?P<slot>\d+)\.3vsav$")


# ---------------------------------------------------------------- sides

class Local:
    def __init__(self, root):
        self.root, self.label = root, root

    def list(self, folder):
        path = os.path.join(self.root, folder)
        return sorted(os.listdir(path)) if os.path.isdir(path) else []

    def read(self, path):
        with open(os.path.join(self.root, path), "rb") as handle:
            return handle.read()

    def write(self, path, data):
        target = os.path.join(self.root, path)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target + ".new", "wb") as handle:
            handle.write(data)
        os.replace(target + ".new", target)

    def close(self):
        pass


class Ftp:
    """FTP side. Uploads go to NAME.new, are read back and compared, then renamed
    (FTPiiU's SIZE reply is unreliable, and a broken upload must not replace a save)."""

    def __init__(self, host, port, root, label):
        self.label, self.root = label, root.rstrip("/")
        self.ftp = ftplib.FTP()
        try:
            self.ftp.connect(host, port, timeout=20)
            self.ftp.login()
        except (OSError, ftplib.Error) as error:
            sys.exit(f"{label}: can't connect to the FTP server at {host}:{port} ({error}). "
                     "Is the FTP app running on the console?")

    def _path(self, path):
        return f"{self.root}/{path}"

    def list(self, folder):
        try:
            names = self.ftp.nlst(self._path(folder))
        except ftplib.error_perm:
            return []
        return sorted(os.path.basename(name.rstrip("/")) for name in names)

    def read(self, path):
        buffer = io.BytesIO()
        self.ftp.retrbinary(f"RETR {self._path(path)}", buffer.write)
        return buffer.getvalue()

    def write(self, path, data):
        folder = os.path.dirname(self._path(path))
        try:
            self.ftp.mkd(folder)
        except ftplib.error_perm:
            pass
        temporary = self._path(path) + ".new"
        self.ftp.storbinary(f"STOR {temporary}", io.BytesIO(data))
        back = io.BytesIO()
        self.ftp.retrbinary(f"RETR {temporary}", back.write)
        if back.getvalue() != data:
            raise IOError(f"{self.label}: {path} did not upload correctly; left unchanged")
        try:
            self.ftp.delete(self._path(path))
        except ftplib.error_perm:
            pass
        self.ftp.rename(temporary, self._path(path))

    def close(self):
        try:
            self.ftp.quit()
        except Exception:
            pass


def open_side(spec):
    match = re.fullmatch(r"(3ds|wiiu):([^:/]+)(?::(\d+))?", spec)
    if match:
        kind, host, port = match.groups()
        if kind == "3ds":
            return Ftp(host, int(port or 5000), "/3ds/3dvn", f"3DS {host}")
        return Ftp(host, int(port or 21), "/sd/wiiu/3dvn", f"Wii U {host}")
    match = re.fullmatch(r"ftp://([^:/]+)(?::(\d+))?(/.*)?", spec)
    if match:
        host, port, path = match.groups()
        return Ftp(host, int(port or 21), path or "/", spec)
    if os.path.isdir(spec):
        return Local(spec)
    sys.exit(f"{spec}: not a folder, 3ds:HOST, wiiu:HOST or ftp://HOST/PATH")


# ---------------------------------------------------------------- formats

def save_time(data):
    """When a save was made (seconds), read in either byte order; None if not a save."""
    if len(data) < 16:
        return None
    for order in "<>":
        magic, _hash, saved_at = struct.unpack_from(order + "IIq", data)
        if magic == SAVE_MAGIC:
            return saved_at
    return None


# The save struct (source/main.c SaveData), as the Wii U lays it out (big-endian, 4-byte
# event type) and as files store it now (little-endian, the 3DS's layout: a 1-byte type
# followed by the speaker name). Same size: 66544 bytes.
NAME, STR, TEXT, VARS, CALLS, CHOICES, SPRITES, BACKLOG = 32, 256, 512, 128, 32, 8, 8, 40


def save_layout(base, wiiu_head):
    def struct_type(name, fields):
        return type(name, (base,), {"_fields_": fields})
    value = struct_type("Value", [("is_str", ctypes.c_int32), ("num", ctypes.c_int32), ("str", ctypes.c_char * STR)])
    var = struct_type("Var", [("name", ctypes.c_char * NAME), ("value", value)])
    state = struct_type("State", [
        ("pc", ctypes.c_int32), ("var_count", ctypes.c_int32), ("vars", var * VARS),
        ("call_depth", ctypes.c_int32), ("call_stack", ctypes.c_int32 * CALLS),
        ("pending_menu", ctypes.c_int32), ("pending_map", ctypes.c_int32 * CHOICES),
        ("pending_count", ctypes.c_int32)])
    sprite = struct_type("Sprite", [("active", ctypes.c_int32), ("name", ctypes.c_char * NAME),
                                    ("asset", ctypes.c_char * STR), ("pos", ctypes.c_char * NAME)]
                         + [(f, ctypes.c_float) for f in ("alpha", "from_", "to", "duration", "elapsed")])
    scene = struct_type("Scene", [("bg", ctypes.c_char * STR), ("bg_prev", ctypes.c_char * STR),
                                  ("music", ctypes.c_char * STR), ("bg_elapsed", ctypes.c_float),
                                  ("bg_duration", ctypes.c_float), ("sprites", sprite * SPRITES)])
    head = ([("type", ctypes.c_int32), ("speaker", ctypes.c_char * NAME)] if wiiu_head else
            [("type", ctypes.c_uint8), ("speaker", ctypes.c_char * NAME), ("pad", ctypes.c_uint8 * 3)])
    event = struct_type("Event", head + [
        ("color", ctypes.c_uint32), ("text", ctypes.c_char * TEXT), ("name", ctypes.c_char * NAME),
        ("asset", ctypes.c_char * STR), ("pos", ctypes.c_char * NAME), ("seconds", ctypes.c_float),
        ("choice_count", ctypes.c_int32), ("choices", (ctypes.c_char * STR) * CHOICES), ("line", ctypes.c_int32)])
    entry = struct_type("Entry", [("speaker", ctypes.c_char * NAME), ("text", ctypes.c_char * TEXT),
                                  ("color", ctypes.c_uint32)])
    backlog = struct_type("Backlog", [("count", ctypes.c_int32), ("start", ctypes.c_int32),
                                      ("entries", entry * BACKLOG)])
    header = struct_type("Header", [("magic", ctypes.c_uint32), ("script_hash", ctypes.c_uint32),
                                    ("saved_at", ctypes.c_int64), ("preview", ctypes.c_char * 72)])
    save = struct_type("Save", [("header", header), ("state", state), ("scene", scene), ("event", event),
                                ("backlog", backlog)])
    assert ctypes.sizeof(save) == 66544, ctypes.sizeof(save)
    return save


WIIU_SAVE = save_layout(ctypes.BigEndianStructure, True)
FILE_SAVE = save_layout(ctypes.LittleEndianStructure, False)


def copy_fields(source, target):
    """Copies a save field by field between the two layouts (numbers keep their value,
    text is copied byte for byte)."""
    for name, kind, *_ in source._fields_:
        if name == "pad":
            continue
        src_off, dst_off = getattr(type(source), name).offset, getattr(type(target), name).offset
        raw = lambda: ctypes.memmove(ctypes.addressof(target) + dst_off, ctypes.addressof(source) + src_off,
                                     ctypes.sizeof(kind))
        if hasattr(kind, "_fields_"):
            copy_fields(getattr(source, name), getattr(target, name))
        elif issubclass(kind, ctypes.Array):
            element = kind._type_
            if element is ctypes.c_char or issubclass(element, ctypes.Array):
                raw()   # text, or arrays of text
            elif hasattr(element, "_fields_"):
                for index in range(kind._length_):
                    copy_fields(getattr(source, name)[index], getattr(target, name)[index])
            else:
                for index in range(kind._length_):
                    getattr(target, name)[index] = getattr(source, name)[index]
        else:
            setattr(target, name, getattr(source, name))


def is_old_wiiu_save(data):
    return len(data) >= 66544 and struct.unpack_from(">I", data)[0] == SAVE_MAGIC


def to_portable(data):
    """An older Wii U save (big-endian) in the format every console reads now."""
    if not is_old_wiiu_save(data):
        return data
    old = WIIU_SAVE.from_buffer_copy(data[:66544])
    new = FILE_SAVE()
    copy_fields(old, new)
    return bytes(new) + data[66544:]


def merge_globals(a, b):
    values = {}
    for text in (a, b):
        for line in text.decode("utf-8", "replace").splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0].startswith("g_") and re.fullmatch(r"-?\d+", parts[1]):
                values[parts[0]] = max(values.get(parts[0], int(parts[1])), int(parts[1]))
    return "".join(f"{name} {value}\n" for name, value in values.items()).encode()


def parse_read(data):
    maps, pos = {}, 0
    while pos < len(data):
        length = data[pos]
        name = data[pos + 1:pos + 1 + length]
        size = struct.unpack_from("<I", data, pos + 1 + length)[0]
        bits = data[pos + 5 + length:pos + 5 + length + size]
        if len(bits) != size:
            break
        maps[name] = bits
        pos += 5 + length + size
    return maps


def merge_read(a, b):
    maps = parse_read(a)
    for name, bits in parse_read(b).items():
        old = maps.get(name, b"")
        size = max(len(old), len(bits))
        old, bits = old.ljust(size, b"\0"), bits.ljust(size, b"\0")
        maps[name] = bytes(x | y for x, y in zip(old, bits))
    out = bytearray()
    for name, bits in maps.items():
        out += bytes([len(name)]) + name + struct.pack("<I", len(bits)) + bits
    return bytes(out)


# ---------------------------------------------------------------- sync

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--game", help="only this game (its archive / folder name)")
    parser.add_argument("--dry-run", action="store_true", help="show what would change, change nothing")
    parser.add_argument("--convert", action="store_true", help="only convert older Wii U saves on both "
                        "sides to the current format (B may then be the same as A)")
    parser.add_argument("--backup", default=os.path.join(
        "save-backups", datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S")))
    args = parser.parse_args()
    sides = [open_side(args.a), open_side(args.b)]
    changes = 0

    def backup(side_index, path, data):
        target = os.path.join(args.backup, f"side{side_index + 1}", path)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as handle:
            handle.write(data)

    def put(side_index, path, data, old, why):
        nonlocal changes
        changes += 1
        print(f"  {sides[side_index].label}: {path}  ({why})")
        if args.dry_run:
            return
        if old is not None:
            backup(side_index, path, old)
        sides[side_index].write(path, data)

    try:
        listings = [{"saves": set(s.list("saves")), "settings": set(s.list("settings"))} for s in sides]
        wanted = lambda name: args.game is None or name == args.game

        if args.convert:
            done = set()
            for index, (side, listing) in enumerate(zip(sides, listings)):
                if side.label in done:
                    continue
                done.add(side.label)
                for name in sorted(listing["saves"]):
                    match = SAVE_NAME.match(name)
                    if not match or not wanted(match["game"]):
                        continue
                    data = side.read(f"saves/{name}")
                    if is_old_wiiu_save(data):
                        put(index, f"saves/{name}", to_portable(data), data, "converted to the current format")
            return

        # save slots: the newer one wins (older Wii U saves are converted on the way)
        slots = sorted({n for l in listings for n in l["saves"]
                        if SAVE_NAME.match(n) and wanted(SAVE_NAME.match(n)["game"])})
        for name in slots:
            path = f"saves/{name}"
            data = [s.read(path) if name in l["saves"] else None for s, l in zip(sides, listings)]
            times = [save_time(d) if d is not None else None for d in data]
            if data[0] == data[1]:
                continue
            if times[0] is None and times[1] is None:
                print(f"  skipped {path}: not a 3DVN save on either side")
                continue
            newer = 0 if (times[1] is None or (times[0] is not None and times[0] > times[1])) else 1
            when = datetime.datetime.fromtimestamp(times[newer]).strftime("%Y-%m-%d %H:%M")
            put(1 - newer, path, to_portable(data[newer]), data[1 - newer],
                f"newer save from {sides[newer].label}, {when}")
            if is_old_wiiu_save(data[newer]):
                put(newer, path, to_portable(data[newer]), data[newer], "converted to the current format")

        # endings and unlocks, read lines: merged
        for folder, suffix, merge in (("saves", ".3vglobal", merge_globals), ("settings", ".3vread", merge_read)):
            names = sorted({n for l in listings for n in l[folder] if n.endswith(suffix) and wanted(n[:-len(suffix)])})
            for name in names:
                path = f"{folder}/{name}"
                data = [s.read(path) if name in l[folder] else None for s, l in zip(sides, listings)]
                merged = merge(data[0] or b"", data[1] or b"")
                for index in (0, 1):
                    if data[index] != merged and (data[index] is None or merge(data[index], b"") != merged):
                        put(index, path, merged, data[index], "merged")
    finally:
        for side in sides:
            side.close()
    if changes == 0:
        print("Everything is already in sync.")
    elif args.dry_run:
        print(f"{changes} change(s) would be made (dry run).")
    else:
        print(f"{changes} file(s) updated; replaced files are in {args.backup}/")


if __name__ == "__main__":
    main()
