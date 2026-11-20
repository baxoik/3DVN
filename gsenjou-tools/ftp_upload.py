#!/usr/bin/env python3
"""Upload a local file or folder to an FTP server. usage: ftp_upload.py HOST PORT LOCAL REMOTE_DIR"""
import ftplib, os, sys, time
host, port, local, remote = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4].rstrip('/')
def connect():
    f = ftplib.FTP(); f.connect(host, port, timeout=60); f.login(); return f
ftp = connect(); made = set()
def mkdirs(path):
    parts = path.strip('/').split('/'); cur = ''
    for p in parts:
        cur += '/' + p
        if cur in made: continue
        try: ftp.mkd(cur)
        except ftplib.error_perm: pass
        made.add(cur)
files = []
if os.path.isfile(local): files.append((local, remote + '/' + os.path.basename(local)))
else:
    base = os.path.join(remote, os.path.basename(local.rstrip('/')))
    for d, _, names in os.walk(local):
        for n in sorted(names):
            p = os.path.join(d, n); files.append((p, base + '/' + os.path.relpath(p, local)))
total = sum(os.path.getsize(p) for p, _ in files); done = 0; t0 = time.time()
for i, (p, r) in enumerate(files, 1):
    for attempt in range(3):
        try:
            mkdirs(os.path.dirname(r))
            with open(p, 'rb') as h: ftp.storbinary('STOR ' + r, h, 65536)
            break
        except Exception as e:
            print('retry', r, e, flush=True); time.sleep(2)
            try: ftp.close()
            except Exception: pass
            ftp = connect(); made.clear()
    else: print('FAILED', r, flush=True); sys.exit(1)
    done += os.path.getsize(p)
    if i % 200 == 0 or i == len(files):
        el = time.time() - t0; print(f'{i}/{len(files)} files, {done>>20}/{total>>20} MB, {done/el/1e6:.2f} MB/s', flush=True)
ftp.quit(); print('UPLOAD DONE')
