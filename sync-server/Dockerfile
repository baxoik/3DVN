# Minimal FTP server for game save sync (multi-arch: amd64, arm64, arm/v7 e.g. ODROID HC2).
FROM docker.io/library/python:3.12-alpine
RUN pip install --no-cache-dir pyftpdlib==2.0.1 && adduser -D -u 1000 gamesync && mkdir /data && chown gamesync /data
COPY server.py /server.py
USER gamesync
VOLUME /data
EXPOSE 2121 30000-30009
CMD ["python3", "-u", "/server.py"]
