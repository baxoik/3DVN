#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* fopencookie */
#endif
#include "pack.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __GLIBC__
typedef off64_t pack_off_t;
#else
typedef _off64_t pack_off_t;   /* newlib (3DS, Wii U) */
#endif

typedef struct {
	char *name;          /* path inside the archive, '/' separated */
	uint32_t header;     /* offset of the local file header */
	uint32_t size;
	uint32_t data;       /* offset of the data, 0 until first opened */
} Entry;

static FILE *archive;
static Entry *entries;
static int entry_count;
static char *names;      /* storage for all entry names */
static char root[320];
static size_t root_len;
static int use_overrides;

#define STREAM_BUFFER (64 * 1024)   /* stdio buffer for the archive and each file in it */

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int by_name(const void *a, const void *b)
{
	return strcmp(((const Entry *)a)->name, ((const Entry *)b)->name);
}

void pack_close(void)
{
	if (archive != NULL) fclose(archive);
	archive = NULL;
	free(entries);
	free(names);
	entries = NULL;
	names = NULL;
	entry_count = 0;
	root[0] = '\0';
	root_len = 0;
}

int pack_active(void) { return archive != NULL; }

int pack_open(const char *path, const char *game_root, int overrides)
{
	pack_close();
	FILE *file = fopen(path, "rb");
	if (file == NULL) return 0;
	setvbuf(file, NULL, _IOFBF, STREAM_BUFFER);   /* fewer, larger reads from the SD card */
	/* End of central directory: last record with signature 0x06054b50 (comment <= 64 KiB). */
	if (fseek(file, 0, SEEK_END) != 0) goto fail;
	long length = ftell(file);
	long tail = length < 65557 ? length : 65557;
	unsigned char *buf = malloc((size_t)tail);
	if (buf == NULL || fseek(file, length - tail, SEEK_SET) != 0 || fread(buf, 1, (size_t)tail, file) != (size_t)tail) {
		free(buf);
		goto fail;
	}
	long eocd = -1;
	for (long i = tail - 22; i >= 0; --i) {
		if (le32(buf + i) == 0x06054b50) { eocd = i; break; }
	}
	if (eocd < 0) { free(buf); goto fail; }
	uint32_t count = le16(buf + eocd + 10), dir_size = le32(buf + eocd + 12), dir_offset = le32(buf + eocd + 16);
	free(buf);
	unsigned char *dir = malloc(dir_size);
	if (dir == NULL || fseek(file, (long)dir_offset, SEEK_SET) != 0 || fread(dir, 1, dir_size, file) != dir_size) {
		free(dir);
		goto fail;
	}
	entries = calloc(count ? count : 1, sizeof(Entry));
	names = malloc(dir_size);   /* names are shorter than the directory itself */
	size_t names_used = 0;
	uint32_t at = 0;
	for (uint32_t i = 0; i < count && entries != NULL && names != NULL; ++i) {
		if (at + 46 > dir_size || le32(dir + at) != 0x02014b50) break;
		uint16_t method = le16(dir + at + 10), name_len = le16(dir + at + 28);
		uint16_t extra_len = le16(dir + at + 30), comment_len = le16(dir + at + 32);
		uint32_t size = le32(dir + at + 24), header = le32(dir + at + 42);
		if (at + 46 + name_len > dir_size) break;
		const char *name = (const char *)dir + at + 46;
		if (method == 0 && name_len > 0 && name[name_len - 1] != '/') {   /* stored files only */
			Entry *e = &entries[entry_count++];
			e->name = names + names_used;
			memcpy(e->name, name, name_len);
			e->name[name_len] = '\0';
			names_used += (size_t)name_len + 1;
			e->size = size;
			e->header = header;
		}
		at += 46u + name_len + extra_len + comment_len;
	}
	free(dir);
	if (entries == NULL || names == NULL || entry_count == 0) goto fail;
	qsort(entries, (size_t)entry_count, sizeof(Entry), by_name);
	archive = file;
	snprintf(root, sizeof(root), "%s", game_root);
	root_len = strlen(root);
	use_overrides = overrides;
	return 1;
fail:
	fclose(file);
	free(entries);
	free(names);
	entries = NULL;
	names = NULL;
	entry_count = 0;
	return 0;
}

/* Archive entry for an engine path ("<root>/bg/x.jpg"), or NULL. */
static Entry *find(const char *path)
{
	if (archive == NULL || strncmp(path, root, root_len) != 0 || path[root_len] != '/') return NULL;
	Entry key;
	key.name = (char *)path + root_len + 1;
	Entry *e = bsearch(&key, entries, (size_t)entry_count, sizeof(Entry), by_name);
	if (e != NULL && e->data == 0) {
		unsigned char local[30];
		if (fseek(archive, (long)e->header, SEEK_SET) != 0 || fread(local, 1, 30, archive) != 30
			|| le32(local) != 0x04034b50) return NULL;
		e->data = e->header + 30u + le16(local + 26) + le16(local + 28);
	}
	return e;
}

typedef struct {
	uint32_t base, size, pos;
} Stream;

/* All streams share the archive FILE: every read seeks first (the engine is single-threaded
   around file access; the audio mixer only reads decoded memory). */
static ssize_t stream_read(void *cookie, char *out, size_t length)
{
	Stream *s = cookie;
	if (s->pos >= s->size) return 0;
	if (length > s->size - s->pos) length = s->size - s->pos;
	if (fseek(archive, (long)(s->base + s->pos), SEEK_SET) != 0) return -1;
	size_t got = fread(out, 1, length, archive);
	s->pos += (uint32_t)got;
	return (ssize_t)got;
}

static int stream_seek(void *cookie, pack_off_t *offset, int whence)
{
	Stream *s = cookie;
	int64_t target = *offset;
	if (whence == SEEK_CUR) target += s->pos;
	else if (whence == SEEK_END) target += s->size;
	if (target < 0 || target > (int64_t)s->size) return -1;
	s->pos = (uint32_t)target;
	*offset = target;
	return 0;
}

static int stream_close(void *cookie)
{
	free(cookie);
	return 0;
}

FILE *vn_fopen(const char *path, const char *mode)
{
	if (mode[0] == 'r' && strchr(mode, '+') == NULL) {
		if (use_overrides) {
			FILE *loose = fopen(path, mode);
			if (loose != NULL) return loose;
		}
		Entry *e = find(path);
		if (e != NULL) {
			Stream *s = malloc(sizeof(Stream));
			if (s == NULL) return NULL;
			s->base = e->data;
			s->size = e->size;
			s->pos = 0;
			cookie_io_functions_t io = {stream_read, NULL, stream_seek, stream_close};
			FILE *f = fopencookie(s, "rb", io);
			if (f == NULL) {
				free(s);
				return NULL;
			}
			/* stdio's default buffer is small (1 KB on newlib): every refill seeks the shared
			   archive, so a 300 KB image took hundreds of SD reads on Wii U. */
			setvbuf(f, NULL, _IOFBF, STREAM_BUFFER);
			return f;
		}
		if (archive != NULL && !use_overrides && strncmp(path, root, root_len) == 0) {
			/* Inside an archived game and not in the archive: it does not exist. Skip the
			   real file system, where a failed open is slow on consoles (the engine probes
			   names, e.g. .png before .jpg). */
			errno = ENOENT;
			return NULL;
		}
	}
	return fopen(path, mode);
}

unsigned char *vn_read_whole(const char *path, size_t *size, int *handled)
{
	*handled = 0;
	if (archive == NULL) return NULL;
	if (use_overrides) {
		struct stat info;
		if (stat(path, &info) == 0) return NULL;   /* a loose override file: caller reads it */
	}
	Entry *e = find(path);
	if (e == NULL) {
		*handled = strncmp(path, root, root_len) == 0;   /* in the archived game: missing */
		return NULL;
	}
	*handled = 1;
	unsigned char *data = malloc((size_t)e->size + 1);
	if (data == NULL) return NULL;
	if (fseek(archive, (long)e->data, SEEK_SET) != 0 || fread(data, 1, e->size, archive) != e->size) {
		free(data);
		return NULL;
	}
	data[e->size] = '\0';
	*size = e->size;
	return data;
}

int pack_peek_member(const char *archive_path, const char *member, char *out, size_t out_size)
{
	/* make_pack.py stores a game's top-level files first, so game.3vc is found after a few
	   local headers; reading the whole central directory would be slow on consoles. */
	FILE *file = fopen(archive_path, "rb");
	if (file == NULL || out_size == 0) {
		if (file != NULL) fclose(file);
		return -1;
	}
	long offset = 0;
	int result = -1;
	size_t member_length = strlen(member);
	for (int entry = 0; entry < 64; ++entry) {
		unsigned char head[30];
		if (fseek(file, offset, SEEK_SET) != 0 || fread(head, 1, 30, file) != 30
			|| le32(head) != 0x04034b50) break;
		uint32_t size = le32(head + 18);
		uint16_t name_length = le16(head + 26), extra_length = le16(head + 28);
		char name[256];
		if (name_length >= sizeof(name) || fread(name, 1, name_length, file) != name_length) break;
		name[name_length] = '\0';
		if (name_length == member_length && !memcmp(name, member, member_length) && le16(head + 8) == 0) {
			size_t want = size < out_size - 1 ? size : out_size - 1;
			if (fseek(file, offset + 30 + name_length + extra_length, SEEK_SET) == 0
				&& fread(out, 1, want, file) == want) {
				out[want] = '\0';
				result = (int)want;
			}
			break;
		}
		offset += 30L + name_length + extra_length + (long)size;
	}
	fclose(file);
	return result;
}

int vn_exists(const char *path)
{
	struct stat info;
	if ((archive == NULL || use_overrides) && stat(path, &info) == 0) return 1;
	return find(path) != NULL;
}

int vn_list(const char *dir, void (*found)(const char *name, void *ctx), void *ctx)
{
	int count = 0;
	if (archive != NULL && strncmp(dir, root, root_len) == 0 && (dir[root_len] == '/' || dir[root_len] == '\0')) {
		const char *sub = dir[root_len] == '/' ? dir + root_len + 1 : "";
		size_t sub_len = strlen(sub);
		for (int i = 0; i < entry_count; ++i) {
			const char *name = entries[i].name;
			if (sub_len > 0 && (strncmp(name, sub, sub_len) != 0 || name[sub_len] != '/')) continue;
			const char *rest = sub_len > 0 ? name + sub_len + 1 : name;
			if (strchr(rest, '/') != NULL) continue;   /* only direct children */
			found(rest, ctx);
			++count;
		}
		return count;
	}
	DIR *handle = opendir(dir);
	if (handle == NULL) return -1;
	struct dirent *item;
	while ((item = readdir(handle)) != NULL) {
		if (item->d_name[0] == '.') continue;
		found(item->d_name, ctx);
		++count;
	}
	closedir(handle);
	return count;
}
