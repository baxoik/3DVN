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
/* More than 128 variables: the first 128 in the main block, the rest in VARX; both must
   come back exactly, and a game with 128 or fewer writes no VARX block at all. */
static int test_extra_vars(void)
{
	static SaveData save, back;
	static unsigned char main_block[SAVE_FILE_SIZE], extra[400000];
	memset(&save, 0, sizeof(save));
	save.header.magic = SAVE_MAGIC;
	save.state.var_count = 700;
	for (int i = 0; i < save.state.var_count; ++i) {
		GsVar *v = &save.state.vars[i];
		snprintf(v->name, sizeof(v->name), "v_flag_%d", i);
		v->value.is_str = i % 7 == 0;
		if (v->value.is_str) snprintf(v->value.str, sizeof(v->value.str), "text %d", i);
		else v->value.num = i * 3 - 500;
	}
	save_encode(&save, main_block);
	size_t extra_size = save_encode_vars(&save, extra, sizeof(extra));
	int ok = extra_size > 8 && save_decode(main_block, SAVE_FILE_SIZE, &back)
		&& back.state.var_count == SAVE_FILE_VARS
		&& save_decode_vars(extra, extra_size, &back) == (long)extra_size
		&& back.state.var_count == save.state.var_count;
	for (int i = 0; ok && i < save.state.var_count; ++i) {
		const GsVar *x = &save.state.vars[i], *y = &back.state.vars[i];
		ok = !strcmp(x->name, y->name) && x->value.is_str == y->value.is_str
			&& (x->value.is_str ? !strcmp(x->value.str, y->value.str) : x->value.num == y->value.num);
	}
	save.state.var_count = 100;
	ok = ok && save_encode_vars(&save, extra, sizeof(extra)) == 0;
	ok = ok && save_decode_vars(main_block, 8, &back) == 0;   /* no VARX tag: nothing read */
	printf("extra variables (VARX): %s\n", ok ? "ok" : "FAILED");
	return ok;
}

int main(int argc, char **argv)
{
	if (argc < 2) return test_extra_vars() ? 0 : 1;
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
