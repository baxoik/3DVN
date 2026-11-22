#!/usr/bin/env python3
"""A small FTP server for syncing game saves on a home network.

Everything lives under one folder (DATA_DIR, the container's /data volume), one
subfolder per app: 3DVN uses /3dvn (saves/, settings/), and other games can use their
own folders later.

Settings (environment variables):
  FTP_PORT          control port (default 2121)
  FTP_PASV_PORTS    passive data ports, "FIRST-LAST" (default 30000-30009)
  FTP_PUBLIC_IP     the address clients reach the server at, given to them for data
                    connections; needed when the container is not on the host network
  FTP_USER, FTP_PASSWORD   if both are set, only this user may log in; otherwise
                    anyone may log in (anonymous) and read and write. Only use the
                    anonymous mode on a network you trust.
  DATA_DIR          default /data
"""
import logging, os

from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler
from pyftpdlib.servers import FTPServer


def main():
    data = os.environ.get("DATA_DIR", "/data")
    port = int(os.environ.get("FTP_PORT", "2121"))
    first, last = (int(p) for p in os.environ.get("FTP_PASV_PORTS", "30000-30009").split("-"))
    user, password = os.environ.get("FTP_USER"), os.environ.get("FTP_PASSWORD")
    os.makedirs(os.path.join(data, "3dvn"), exist_ok=True)

    authorizer = DummyAuthorizer()
    permissions = "elradfmwMT"   # list, read, append, delete, rename, mkdir, write, ...
    if user and password:
        authorizer.add_user(user, password, data, perm=permissions)
        who = f"user {user}"
    else:
        authorizer.add_anonymous(data, perm=permissions)
        who = "anonymous (no password)"

    handler = FTPHandler
    handler.authorizer = authorizer
    handler.passive_ports = range(first, last + 1)
    handler.banner = "game-sync ready"
    handler.permit_foreign_addresses = False
    if os.environ.get("FTP_PUBLIC_IP"):
        handler.masquerade_address = os.environ["FTP_PUBLIC_IP"]

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    server = FTPServer(("0.0.0.0", port), handler)
    server.max_cons, server.max_cons_per_ip = 32, 8
    logging.info("game-sync FTP on port %d, data in %s, passive ports %d-%d, login: %s",
                 port, data, first, last, who)
    server.serve_forever()


if __name__ == "__main__":
    main()
