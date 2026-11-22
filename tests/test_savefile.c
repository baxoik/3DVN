/* Round trip of real save files through savefile.c:
     test_savefile SAVE [EXPECTED]
   decodes SAVE and encodes it again; the result must equal EXPECTED (default: SAVE's own
   first 66544 bytes, i.e. a current little-endian save comes back byte for byte). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "savefile.h"

static unsigned char *slurp(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	*size = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *d = malloc(*size);
	if (fread(d, 1, *size, f) != *size) { free(d); d = NULL; }
	fclose(f);
	return d;
}

static int normalize(const char *path, unsigned char *out)
{
	size_t size;
	unsigned char *data = slurp(path, &size);
	static SaveData save;
	int ok = data && save_decode(data, size, &save);
	if (ok) save_encode(&save, out);
	free(data);
	return ok;
}

/* test_savefile A [B]: A decodes; encoding is stable (decode(encode(A)) encodes the same);
   with B, A and B are the same save (e.g. an old big-endian Wii U file and its converted
   little-endian copy): both encode to identical bytes. Text fields are written with zeros
   after their end, so old files (which kept leftover memory there) differ only there. */
int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s A [B]\n", argv[0]); return 2; }
	static unsigned char a[SAVE_FILE_SIZE], again[SAVE_FILE_SIZE], b[SAVE_FILE_SIZE];
	static SaveData save;
	if (!normalize(argv[1], a)) { printf("%s: not decodable\n", argv[1]); return 1; }
	save_decode(a, SAVE_FILE_SIZE, &save);
	save_encode(&save, again);
	int stable = !memcmp(a, again, SAVE_FILE_SIZE);
	int same = argc < 3 || (normalize(argv[2], b) && !memcmp(a, b, SAVE_FILE_SIZE));
	printf("%-28s stable=%s%s | pc %d, %d vars, \"%s: %.30s\", backlog %d, bg %s\n",
		strrchr(argv[1], '/') + 1, stable ? "yes" : "NO", argc < 3 ? "" : same ? " same-as-converted=yes" : " same-as-converted=NO",
		save.state.pc, save.state.var_count, save.event.speaker, save.event.text, save.backlog.count, save.scene.bg);
	return stable && same ? 0 : 1;
}
