#!/usr/bin/env python3
"""Loop points for G-Senjou's looping sound effects.

usage: make_loops.py SOUND_DIR GAME_DIR

SOUND_DIR holds your copy's sound effects with their KiriKiri loop files
(se_04.ogg + se_04.ogg.sli). Each .sli "Link { From=A; To=B; }" means: at sample A
jump back to sample B. That becomes GAME_DIR/audio/<name>.loop with "B/rate A/rate"
in seconds, which 3DVN uses for "sfx NAME loop" (seconds, so it stays right when an
archive re-encodes the sound at another sample rate).
"""
import os, re, struct, sys

sound_dir, game = sys.argv[1:3]


def ogg_rate(path):
    """Sample rate from the Vorbis identification header (first Ogg page)."""
    data = open(path, "rb").read(4096)
    at = data.find(b"\x01vorbis")
    if at < 0:
        raise ValueError("no Vorbis header")
    return struct.unpack_from("<I", data, at + 12)[0]


written = 0
for name in sorted(os.listdir(sound_dir)):
    if not name.endswith(".ogg.sli"):
        continue
    text = open(os.path.join(sound_dir, name), encoding="utf-8", errors="replace").read()
    link = re.search(r"From=(\d+);\s*To=(\d+);", text)
    stem = name[:-len(".ogg.sli")]
    if not link or not os.path.exists(os.path.join(game, "audio", stem + ".wav")) \
            and not os.path.exists(os.path.join(game, "audio", stem + ".ogg")):
        continue
    rate = ogg_rate(os.path.join(sound_dir, stem + ".ogg"))
    loop_from, loop_to = int(link.group(1)), int(link.group(2))
    with open(os.path.join(game, "audio", stem + ".loop"), "w") as handle:
        handle.write(f"{loop_to / rate:.6f} {loop_from / rate:.6f}\n")
    written += 1
print(f"loop points: {written} sound effects")
