#!/usr/bin/env python3
"""Convert Never 7 to 3DVN: the VNDS version's script, with the pictures, voices and music
of "Never 7 - the end of infinity" for LÖVE (n7e.love).

usage: import_never7.py VNDS_FOLDER N7E_LOVE OUT_GAME_DIR [--music pc|psp] [import_vnds options]

VNDS_FOLDER is the VNDS release ("Never 7", with script/ and img.ini); N7E_LOVE the .love
file (read in place, nothing is extracted). From the .love file come:
  - backgrounds and sprites: upscale/ (twice the VNDS size: the 3DS gets them scaled to
    its screen, PC / Wii U / Android draw the full-size ones from hd/)
  - voices and sound effects: sound/ (WAV, converted to Ogg Vorbis)
  - music: music/pc/ (or music/psp/ with --music psp)
Anything the .love file lacks (a few VNDS-only pictures) comes from the VNDS release.
Everything else is import_vnds.py (see tools/README_import_vnds.md); --speakers is on.
Only convert games you own.
"""
import io
import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import import_vnds  # noqa: E402

try:
    from PIL import Image
except ImportError:
    Image = None


def stem(name):
    """'sound/I26D000.waf.wav.wav' -> 'i26d000'; 'bg00a.jpg' -> 'bg00a'"""
    return re.sub(r"(\.[A-Za-z0-9]+)+$", "", os.path.basename(name.replace("\\", "/"))).lower()


class LoveOverlay:
    """A VNDS asset folder (import_vnds.Assets) whose files are replaced, where the .love
    file has them, by the .love's: same names for the scripts, other bytes."""

    def __init__(self, base, archive, folders, images=False):
        self.base = base
        self.files = base.files
        self.archive = archive
        self.members = {}
        for member in archive.namelist():
            if member.endswith("/"):
                continue
            for folder in folders:
                if member.startswith(folder):
                    self.members.setdefault(stem(member), member)
        self.images = images
        self.factors = {}
        self.replaced = set()

    def find(self, name):
        return self.base.find(name)

    def rel(self, key):
        return self.base.rel(key)

    def member(self, key):
        return self.members.get(stem(key))

    def read(self, key):
        member = self.member(key)
        if member is None:
            return self.base.read(key)
        self.replaced.add(key)
        return self.archive.read(member)

    def path(self, key):
        return None if self.member(key) else self.base.path(key)

    def factor(self, key):
        """How many times larger the .love picture is than the VNDS one (2 for Never 7)."""
        if key not in self.factors:
            member = self.member(key)
            factor = 1.0
            if member is not None and self.images:
                try:
                    with Image.open(io.BytesIO(self.archive.read(member))) as big, \
                            Image.open(io.BytesIO(self.base.read(key))) as small:
                        factor = big.width / small.width if small.width else 1.0
                except (OSError, ValueError, KeyError):
                    factor = 2.0
            self.factors[key] = factor
        return self.factors[key]


def main():
    args = sys.argv[1:]
    music = "pc"
    if "--music" in args:
        at = args.index("--music")
        music = args[at + 1]
        del args[at:at + 2]
        if music not in ("pc", "psp"):
            sys.exit("--music must be pc or psp")
    if len(args) < 3 or args[0].startswith("-"):
        sys.exit(__doc__)
    vnds, love, out, rest = args[0], args[1], args[2], args[3:]
    if not os.path.isfile(os.path.join(vnds, "img.ini")) or not os.path.isdir(os.path.join(vnds, "script")):
        sys.exit(f"{vnds}: not the VNDS Never 7 folder (needs script/ and img.ini)")
    archive = zipfile.ZipFile(love)
    if not any(n.startswith("upscale/background/") for n in archive.namelist()):
        sys.exit(f"{love}: not n7e.love (no upscale/background/)")

    # import_vnds as usual, with the asset folders swapped before it runs
    sys.argv = [sys.argv[0], vnds, out, "--speakers", "--title", "Never 7"] + rest
    original_init = import_vnds.Converter.__init__

    def init(self, a):
        original_init(self, a)
        self.bgs = LoveOverlay(self.bgs, archive, ["upscale/background/"], images=True)
        self.fgs = LoveOverlay(self.fgs, archive, ["upscale/foreground/"], images=True)
        self.snd = LoveOverlay(self.snd, archive, ["sound/", f"music/{music}/"])
        overlays.extend([self.bgs, self.fgs, self.snd])

    overlays = []
    import_vnds.Converter.__init__ = init
    import_vnds.main()
    bgs, fgs, snd = overlays
    print(f"from n7e.love: {len(bgs.replaced)} backgrounds, {len(fgs.replaced)} sprites, "
          f"{len(snd.replaced)} sounds ({music} music); the rest from VNDS")


if __name__ == "__main__":
    main()
