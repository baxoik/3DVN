#ifndef VN_FTP_H
#define VN_FTP_H

#include <stddef.h>

/* A small blocking FTP client (passive mode, binary) for save sync. Every network wait
   has a timeout, so an unreachable server can't hang the game. */

typedef struct {
	int control;              /* control connection socket, -1 when closed */
	unsigned host;            /* server IPv4 address, network byte order */
	char error[160];          /* why the last call failed */
} FtpConn;

/* Starts / stops the console's network stack (3DS: the soc service); harmless elsewhere. */
int ftp_net_init(void);
void ftp_net_exit(void);

/* host: an IPv4 address or name; user NULL or "" logs in anonymously. 0 on success. */
int ftp_connect(FtpConn *ftp, const char *host, int port, const char *user, const char *password);
void ftp_close(FtpConn *ftp);

/* Names in a folder (no path), separated by '\n', into out; -1 on error, else the count.
   A missing folder lists as empty. */
int ftp_list(FtpConn *ftp, const char *folder, char *out, size_t size);

/* Downloads a whole file (malloc'd into *data). 0 on success. */
int ftp_get(FtpConn *ftp, const char *path, unsigned char **data, size_t *size);

/* Uploads safely: to PATH.new, then the old PATH (if any) becomes PATH.bak, then PATH.new
   becomes PATH. 0 on success. */
int ftp_put(FtpConn *ftp, const char *path, const unsigned char *data, size_t size);

/* Creates a folder; an existing one is fine. */
void ftp_mkdir(FtpConn *ftp, const char *folder);

#endif
