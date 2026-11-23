#include "ftp.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>

#ifdef __3DS__
#include <3ds.h>
#include <malloc.h>
#endif

#define TIMEOUT_SECONDS 8
#define MAX_FILE (4u * 1024u * 1024u)   /* saves are ~70 KB; refuse anything absurd */

#ifdef __3DS__
static u32 *soc_buffer;
#endif

int ftp_net_init(void)
{
#ifdef __3DS__
	if (soc_buffer != NULL) return 0;
	soc_buffer = memalign(0x1000, 0x100000);
	if (soc_buffer == NULL) return -1;
	if (R_FAILED(socInit(soc_buffer, 0x100000))) {
		free(soc_buffer);
		soc_buffer = NULL;
		return -1;
	}
#endif
	return 0;
}

void ftp_net_exit(void)
{
#ifdef __3DS__
	if (soc_buffer != NULL) {
		socExit();
		free(soc_buffer);
		soc_buffer = NULL;
	}
#endif
}

static int fail(FtpConn *ftp, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(ftp->error, sizeof(ftp->error), format, args);
	va_end(args);
	return -1;
}

static int wait_socket(int sock, int writing)
{
	fd_set set;
	FD_ZERO(&set);
	FD_SET(sock, &set);
	struct timeval tv = {TIMEOUT_SECONDS, 0};
	return select(sock + 1, writing ? NULL : &set, writing ? &set : NULL, NULL, &tv);
}

/* Connects with a timeout (non-blocking connect, then select). */
static int open_socket(FtpConn *ftp, unsigned host, int port)
{
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) return fail(ftp, "no network (socket)");
	int flags = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flags | O_NONBLOCK);
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)port);
	address.sin_addr.s_addr = host;
	int result = connect(sock, (struct sockaddr *)&address, sizeof(address));
	if (result < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EAGAIN) {
		close(sock);
		return fail(ftp, "can't connect (port %d)", port);
	}
	if (result < 0) {
		if (wait_socket(sock, 1) <= 0) {
			close(sock);
			return fail(ftp, "no answer from the server (port %d)", port);
		}
		/* Connected? Asking for the peer works the same on every system (the 3DS does not
		   report SO_ERROR the usual way, which looked like "refused"). */
		struct sockaddr_in peer;
		socklen_t length = sizeof(peer);
		if (getpeername(sock, (struct sockaddr *)&peer, &length) != 0) {
			close(sock);
			return fail(ftp, "connection refused (port %d)", port);
		}
	}
	fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
	return sock;
}

static int send_all(int sock, const void *data, size_t size)
{
	const char *p = data;
	while (size > 0) {
		if (wait_socket(sock, 1) <= 0) return -1;
		ssize_t sent = send(sock, p, size, 0);
		if (sent <= 0) return -1;
		p += sent;
		size -= (size_t)sent;
	}
	return 0;
}

/* Reads one reply (multi-line ones too); returns its code, or -1. */
static int reply(FtpConn *ftp, char *text, size_t size)
{
	char line[512];
	size_t used = 0;
	int code = -1, multi = 0;
	for (;;) {
		char c;
		if (wait_socket(ftp->control, 0) <= 0) return fail(ftp, "the server stopped answering");
		if (recv(ftp->control, &c, 1, 0) != 1) return fail(ftp, "the server closed the connection");
		if (c != '\n') {
			if (used + 1 < sizeof(line) && c != '\r') line[used++] = c;
			continue;
		}
		line[used] = '\0';
		used = 0;
		if (strlen(line) < 3) continue;
		int this_code = atoi(line);
		if (code < 0) {
			code = this_code;
			multi = line[3] == '-';
		}
		if (!multi || (this_code == code && line[3] == ' ')) {
			if (text != NULL) snprintf(text, size, "%s", line);
			return code;
		}
	}
}

static int command(FtpConn *ftp, char *text, size_t size, const char *format, ...)
{
	char line[600];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line) - 2, format, args);
	va_end(args);
	strcat(line, "\r\n");
	if (send_all(ftp->control, line, strlen(line)) != 0) return fail(ftp, "lost the connection");
	return reply(ftp, text, size);
}

int ftp_connect(FtpConn *ftp, const char *host, int port, const char *user, const char *password)
{
	memset(ftp, 0, sizeof(*ftp));
	ftp->control = -1;
	struct in_addr address;
	if (inet_aton(host, &address) == 0) {
		struct hostent *entry = gethostbyname(host);
		if (entry == NULL || entry->h_addr_list[0] == NULL) return fail(ftp, "unknown server \"%s\"", host);
		memcpy(&address, entry->h_addr_list[0], sizeof(address));
	}
	ftp->host = address.s_addr;
	ftp->control = open_socket(ftp, ftp->host, port);
	if (ftp->control < 0) return -1;
	char text[512];
	if (reply(ftp, text, sizeof(text)) != 220) {
		ftp_close(ftp);
		return fail(ftp, "not an FTP server");
	}
	const char *name = user != NULL && user[0] ? user : "anonymous";
	int code = command(ftp, text, sizeof(text), "USER %s", name);
	if (code == 331) code = command(ftp, text, sizeof(text), "PASS %s",
		password != NULL && password[0] ? password : "3dvn@");
	if (code != 230) {
		ftp_close(ftp);
		return fail(ftp, "login refused (%d)", code);
	}
	if (command(ftp, text, sizeof(text), "TYPE I") != 200) {
		ftp_close(ftp);
		return fail(ftp, "binary mode refused");
	}
	return 0;
}

void ftp_close(FtpConn *ftp)
{
	if (ftp->control >= 0) {
		const char quit[] = "QUIT\r\n";
		send(ftp->control, quit, sizeof(quit) - 1, 0);
		close(ftp->control);
	}
	ftp->control = -1;
}

/* PASV, then connect to the data port (on the control connection's address, which
   also works when the server reports a different one behind NAT or in a container). */
static int open_data(FtpConn *ftp)
{
	char text[512];
	if (command(ftp, text, sizeof(text), "PASV") != 227) return fail(ftp, "passive mode refused");
	const char *open = strchr(text, '(');
	int h1, h2, h3, h4, p1, p2;
	if (open == NULL || sscanf(open + 1, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
		return fail(ftp, "odd passive reply");
	}
	return open_socket(ftp, ftp->host, p1 * 256 + p2);
}

static int read_all(FtpConn *ftp, int sock, unsigned char **data, size_t *size)
{
	size_t capacity = 65536, used = 0;
	unsigned char *buffer = malloc(capacity + 1);
	if (buffer == NULL) return fail(ftp, "out of memory");
	for (;;) {
		if (wait_socket(sock, 0) <= 0) {
			free(buffer);
			return fail(ftp, "download stalled");
		}
		if (used == capacity) {
			if (capacity >= MAX_FILE) {
				free(buffer);
				return fail(ftp, "file too large");
			}
			capacity *= 2;
			unsigned char *bigger = realloc(buffer, capacity + 1);
			if (bigger == NULL) {
				free(buffer);
				return fail(ftp, "out of memory");
			}
			buffer = bigger;
		}
		ssize_t got = recv(sock, buffer + used, capacity - used, 0);
		if (got < 0) {
			free(buffer);
			return fail(ftp, "download failed");
		}
		if (got == 0) break;
		used += (size_t)got;
	}
	buffer[used] = '\0';
	*data = buffer;
	*size = used;
	return 0;
}

int ftp_list(FtpConn *ftp, const char *folder, char *out, size_t size)
{
	out[0] = '\0';
	int sock = open_data(ftp);
	if (sock < 0) return -1;
	char text[512];
	int code = command(ftp, text, sizeof(text), "NLST %s", folder);
	if (code == 550 || code == 450) {   /* no such folder: empty */
		close(sock);
		return 0;
	}
	if (code != 150 && code != 125) {
		close(sock);
		return fail(ftp, "can't list %s (%d)", folder, code);
	}
	unsigned char *data;
	size_t length;
	int ok = read_all(ftp, sock, &data, &length);
	close(sock);
	code = reply(ftp, text, sizeof(text));
	if (ok != 0) return -1;
	if (code != 226 && code != 250) {
		free(data);
		return fail(ftp, "listing failed (%d)", code);
	}
	int count = 0;
	size_t used = 0;
	for (char *line = strtok((char *)data, "\r\n"); line != NULL; line = strtok(NULL, "\r\n")) {
		const char *name = strrchr(line, '/');
		name = name != NULL ? name + 1 : line;
		size_t n = strlen(name);
		if (n == 0 || used + n + 2 > size) continue;
		memcpy(out + used, name, n);
		used += n;
		out[used++] = '\n';
		out[used] = '\0';
		++count;
	}
	free(data);
	return count;
}

int ftp_get(FtpConn *ftp, const char *path, unsigned char **data, size_t *size)
{
	int sock = open_data(ftp);
	if (sock < 0) return -1;
	char text[512];
	int code = command(ftp, text, sizeof(text), "RETR %s", path);
	if (code != 150 && code != 125) {
		close(sock);
		return fail(ftp, "can't download %s (%d)", path, code);
	}
	int ok = read_all(ftp, sock, data, size);
	close(sock);
	code = reply(ftp, text, sizeof(text));
	if (ok != 0) return -1;
	if (code != 226 && code != 250) {
		free(*data);
		return fail(ftp, "download of %s failed (%d)", path, code);
	}
	return 0;
}

static int store(FtpConn *ftp, const char *path, const unsigned char *data, size_t size)
{
	int sock = open_data(ftp);
	if (sock < 0) return -1;
	char text[512];
	int code = command(ftp, text, sizeof(text), "STOR %s", path);
	if (code != 150 && code != 125) {
		close(sock);
		return fail(ftp, "can't upload %s (%d)", path, code);
	}
	int sent = send_all(sock, data, size);
	close(sock);
	code = reply(ftp, text, sizeof(text));
	if (sent != 0 || (code != 226 && code != 250)) return fail(ftp, "upload of %s failed (%d)", path, code);
	return 0;
}

int ftp_put(FtpConn *ftp, const char *path, const unsigned char *data, size_t size)
{
	char temporary[300], backup[300], text[512];
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	snprintf(backup, sizeof(backup), "%s.bak", path);
	if (store(ftp, temporary, data, size) != 0) return -1;
	/* read it back: a broken upload must not replace a good save */
	unsigned char *check;
	size_t check_size;
	if (ftp_get(ftp, temporary, &check, &check_size) != 0) return -1;
	int same = check_size == size && memcmp(check, data, size) == 0;
	free(check);
	if (!same) return fail(ftp, "upload of %s came out different", path);
	if (command(ftp, text, sizeof(text), "RNFR %s", path) == 350) {
		command(ftp, text, sizeof(text), "DELE %s", backup);
		command(ftp, text, sizeof(text), "RNFR %s", path);
		if (command(ftp, text, sizeof(text), "RNTO %s", backup) != 250) {
			command(ftp, text, sizeof(text), "DELE %s", path);
		}
	}
	if (command(ftp, text, sizeof(text), "RNFR %s", temporary) != 350
		|| command(ftp, text, sizeof(text), "RNTO %s", path) != 250) {
		return fail(ftp, "can't put %s in place", path);
	}
	return 0;
}

int ftp_retire(FtpConn *ftp, const char *path)
{
	char backup[300], text[512];
	snprintf(backup, sizeof(backup), "%s.bak", path);
	if (command(ftp, text, sizeof(text), "RNFR %s", path) != 350) return 1;   /* not there */
	command(ftp, text, sizeof(text), "DELE %s", backup);
	command(ftp, text, sizeof(text), "RNFR %s", path);
	return command(ftp, text, sizeof(text), "RNTO %s", backup) == 250 ? 0 : -1;
}

void ftp_mkdir(FtpConn *ftp, const char *folder)
{
	char text[512];
	command(ftp, text, sizeof(text), "MKD %s", folder);
}
