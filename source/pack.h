#ifndef PACK_H
#define PACK_H

#include <stdio.h>

/*
 * Game archives: a whole game folder packed into one uncompressed ("stored") ZIP file,
 * <games>/<name>.3vp. Files inside are read as ordinary FILE* streams, so the rest of
 * the engine is unchanged. Loose files are used when no archive is open.
 */

/* Opens `archive` and serves its files for paths under `root` (the game folder). With
   `overrides`, a real file at the same path (in a folder named like the game) is used
   instead of the archived one, so a packed game can be patched with loose files. */
int pack_open(const char *archive, const char *root, int overrides);
void pack_close(void);
int pack_active(void);

/* fopen for game files: read from the open archive when the path is inside it,
   otherwise (or for writing) the real file system. */
FILE *vn_fopen(const char *path, const char *mode);
/* 1 if the game file exists (in the archive or on disk). */
int vn_exists(const char *path);
/* Reads a small stored file (e.g. "game.3vc") from near the start of a .3vp archive without
   opening it as the current game. Returns its length (NUL-terminated in out), or -1. */
int pack_peek_member(const char *archive_path, const char *member, char *out, size_t out_size);
/* Reads a whole file from the open archive with a single read (stdio would split it into
   small buffered reads, each seeking the archive). Sets *handled to 0 when the path is not
   the archive's to answer (no archive, or a loose override file): use vn_fopen then.
   Returns a malloc'd, NUL-terminated buffer, or NULL. */
unsigned char *vn_read_whole(const char *path, size_t *size, int *handled);
/* Calls `found` for each file directly inside `dir` (archive or disk); returns the count. */
int vn_list(const char *dir, void (*found)(const char *name, void *ctx), void *ctx);

#endif
