# game-sync: a tiny FTP server for game save sync

A minimal FTP server (Python, [pyftpdlib](https://github.com/giampaolo/pyftpdlib)) in a
Docker container, for syncing game saves on your home network. It runs on small ARM
boards (tested as `linux/arm/v7`, e.g. ODROID HC2, Raspberry Pi) as well as x86.

Each app gets a folder: 3DVN keeps saves in `/3dvn/saves` and read marks in
`/3dvn/settings`. Other games can use their own folders later.

## Install with Portainer

1. **Stacks → Add stack**, name it `game-sync`.
2. Either
   - **Repository**: URL `https://github.com/davidgith1/3DVN-wiiu`, reference
     `refs/heads/main`, compose path `sync-server/docker-compose.yml`; or
   - **Upload** / **Web editor**: build the image on the box first (below) and use
     `docker-compose.yml` from this folder.
3. **Deploy the stack.** The first build on an ARM board takes a minute or two.
4. Note the box's IP address (e.g. `192.168.0.10`). In 3DVN: **Sync Saves** (title menu
   or in-game menu), enter `192.168.0.10:2121`, Connect.

## Install with the command line

```sh
# copy this folder to the box, then in it:
docker compose up -d --build
docker logs game-sync          # "starting FTP server on 0.0.0.0:2121"
```

Without compose: `docker build -t game-sync-ftp . && docker run -d --name game-sync
--restart unless-stopped --network host -v game-sync-data:/data game-sync-ftp`.

## Settings

| variable | default | |
| --- | --- | --- |
| `FTP_PORT` | `2121` | control port |
| `FTP_PASV_PORTS` | `30000-30009` | data ports |
| `FTP_USER`, `FTP_PASSWORD` | (unset) | if both are set, a login is required; otherwise anyone on the network may read and write |
| `FTP_PUBLIC_IP` | (unset) | the box's address, only needed without host networking (then also map 2121 and 30000-30009) |

With a login, put it in 3DVN's `settings/sync.cfg` on each console
(`sd:/3ds/3dvn/settings/sync.cfg`, `sd:/wiiu/3dvn/settings/sync.cfg`):

```
address 192.168.0.10:2121
user sync
password change-me
```

## Data

Saves are in the Docker volume `game-sync-data` (`docker volume inspect game-sync-data`
shows where). To use a folder instead, replace `game-sync-data:/data` with e.g.
`/srv/game-sync:/data` and give it to the container's user once:
`sudo mkdir -p /srv/game-sync && sudo chown 1000:1000 /srv/game-sync`.

Every file a console replaces is kept once as `NAME.bak`. The PC tool
`tools/sync_saves.py` can sync with the server too: `ftp://192.168.0.10:2121/3dvn`.

Only run it on a network you trust: FTP is not encrypted.
