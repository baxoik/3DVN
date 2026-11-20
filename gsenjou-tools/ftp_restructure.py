#!/usr/bin/env python3
"""On the 3DS (via ftpd), move voice WAVs from audio/ into audio/v/<group>/ and
upload the new chapter scripts. Safe to re-run: already-moved files are skipped.

usage: ftp_restructure.py HOST PORT LOCAL_GAME_DIR REMOTE_GAME_DIR [--test N]
"""
import ftplib, os, sys, time

host, port, local, remote = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4].rstrip("/")
test = int(sys.argv[sys.argv.index("--test") + 1]) if "--test" in sys.argv else None


def connect():
    f = ftplib.FTP()
    f.connect(host, port, timeout=120)
    f.login()
    return f


ftp = connect()
# voices wanted: audio/v/<group>/<name>.wav in the local build
moves = []
vroot = os.path.join(local, "audio", "v")
for group in sorted(os.listdir(vroot)):
    for name in sorted(os.listdir(os.path.join(vroot, group))):
        moves.append((f"{remote}/audio/{name}", f"{remote}/audio/v/{group}/{name}", group))
on_card = {os.path.basename(p) for p in ftp.nlst(f"{remote}/audio")}
todo = [m for m in moves if os.path.basename(m[0]) in on_card]
print(f"{len(moves)} voices in build, {len(todo)} still to move on the card", flush=True)
if test:
    todo = todo[:test]

made = set()
for attempt_dir in ("v",):
    try:
        ftp.mkd(f"{remote}/audio/v")
    except ftplib.error_perm:
        pass
t0 = time.time()
for i, (src, dst, group) in enumerate(todo, 1):
    for attempt in range(3):
        try:
            if group not in made:
                try:
                    ftp.mkd(f"{remote}/audio/v/{group}")
                except ftplib.error_perm:
                    pass
                made.add(group)
            ftp.rename(src, dst)
            break
        except (ftplib.error_temp, OSError, EOFError) as e:
            print("retry", src, e, flush=True)
            time.sleep(2)
            try:
                ftp.close()
            except Exception:
                pass
            ftp = connect()
    else:
        sys.exit(f"FAILED moving {src}")
    if i % 250 == 0 or i == len(todo):
        el = time.time() - t0
        print(f"moved {i}/{len(todo)}, {el / i:.3f} s each, ~{el / i * (len(todo) - i) / 60:.0f} min left", flush=True)

if test:
    print("test done")
    sys.exit(0)

for name in sorted(os.listdir(os.path.join(local, "ch"))) + ["main.3vs"]:
    path = os.path.join(local, "ch", name) if name != "main.3vs" else os.path.join(local, name)
    target = f"{remote}/ch/{name}" if name != "main.3vs" else f"{remote}/main.3vs"
    with open(path, "rb") as h:
        ftp.storbinary("STOR " + target, h, 65536)
print("scripts uploaded", flush=True)

# verify: every voice in place, nothing left in audio/ that should have moved, script sizes match
left = {os.path.basename(p) for p in ftp.nlst(f"{remote}/audio")} & {os.path.basename(m[0]) for m in moves}
bad = 0
for group in sorted({m[2] for m in moves}):
    want = set(os.listdir(os.path.join(vroot, group)))
    have = {os.path.basename(p) for p in ftp.nlst(f"{remote}/audio/v/{group}")}
    bad += len(want - have)
ftp.voidcmd("TYPE I")
size_bad = [n for n in os.listdir(os.path.join(local, "ch"))
            if ftp.size(f"{remote}/ch/{n}") != os.path.getsize(os.path.join(local, "ch", n))]
print(f"VERIFY: missing voices={bad}, unmoved voices={len(left)}, script size mismatches={len(size_bad)}")
ftp.quit()
