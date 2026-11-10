#include "../source/gs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
	long size = ftell(f);
	if (size < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
	char *buf = malloc((size_t)size + 1);
	if (!buf) { fclose(f); return NULL; }
	if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		fclose(f);
		return NULL;
	}
	if (fclose(f)) { free(buf); return NULL; }
	buf[size] = '\0';
	return buf;
}

static char *load(const char *name, void *ctx)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", (const char *)ctx, name);
	return slurp(path);
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; ++i) {
		char *buf = slurp(argv[i]);
		if (!buf) { perror(argv[i]); return 2; }
		char dir[1024];
		const char *root = getenv("GS_ROOT"); /* game folder, if includes are not next to the script */
		snprintf(dir, sizeof(dir), "%s", root ? root : argv[i]);
		char *slash = root ? NULL : strrchr(dir, '/');
		if (slash) *slash = 0; else if (!root) strcpy(dir, ".");
		char err[256];
		GsProgram *p = gs_compile_ex(buf, load, dir, err, sizeof(err));
		printf("%s: %s\n", argv[i], p ? "ok" : err);
		if (!p) return 1;
		gs_free(p);
		free(buf);
	}
	return 0;
}
