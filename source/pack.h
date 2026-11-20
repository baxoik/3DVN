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
/* Calls `found` for each file directly inside `dir` (archive or disk); returns the count. */
int vn_list(const char *dir, void (*found)(const char *name, void *ctx), void *ctx);

#endif
