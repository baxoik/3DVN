/* Walks a 3DVN game through the script VM without graphics or sound, many times, with
   random choices, and reports problems: script errors, missing images and audio, scripts
   that never end, and which script files are reached. Number variables named g_* carry
   over from one run to the next like the engine's global variables (endings seen,
   routes unlocked), so later runs can reach content that earlier ones unlocked.

   Build and run on a PC (from the repository root):
     cc -O2 -Isource tests/walk_story.c source/gs.c source/wrap.c -o /tmp/walk_story
     /tmp/walk_story GAME_DIR [RUNS] [SEED]
   GAME_DIR is a game folder (not a .3vp); exits 1 if any problem was found. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "gs.h"

#define MAX_SCRIPTS 512
#define MAX_PROBLEMS 400
#define STEP_LIMIT 5000000L

static const char *game;
static char scripts[MAX_SCRIPTS][96];
static int script_hits[MAX_SCRIPTS], script_count;
static char problems[MAX_PROBLEMS][200];
static int problem_count, problem_total;
static struct { char name[GS_NAME_LEN]; long value; } globals[64];
static int global_count;

static char *read_all(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *data = malloc((size_t)n + 1);
	if (data != NULL && fread(data, 1, (size_t)n, f) != (size_t)n) {
		free(data);
		data = NULL;
	}
	fclose(f);
	if (data != NULL) data[n] = '\0';
	return data;
}

static char *load_include(const char *name, void *ctx)
{
	(void)ctx;
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", game, name);
	return read_all(path);
}

static void problem(const char *format, const char *a, const char *b)
{
	char line[200];
	snprintf(line, sizeof(line), format, a, b);
	++problem_total;
	for (int i = 0; i < problem_count; ++i) {
		if (!strcmp(problems[i], line)) return;   /* report each distinct problem once */
	}
	if (problem_count < MAX_PROBLEMS) snprintf(problems[problem_count++], sizeof(problems[0]), "%s", line);
}

static int exists_any(const char *folder, const char *name, const char *const *exts)
{
	char path[600];
	struct stat info;
	for (int i = 0; exts[i] != NULL; ++i) {
		snprintf(path, sizeof(path), "%s/%s/%s%s", game, folder, name, exts[i]);
		if (stat(path, &info) == 0) return 1;
	}
	return 0;
}

static void check_asset(const char *kind, const char *folder, const char *name, const char *script)
{
	static const char *const images[] = {".png", ".jpg", "", NULL};
	static const char *const sounds[] = {".ogg", ".wav", "", NULL};
	if (name[0] == '\0' || !strcmp(name, "black") || !strcmp(name, "white")) return;
	if (!exists_any(folder, name, strcmp(folder, "audio") ? images : sounds)) {
		char what[160];
		snprintf(what, sizeof(what), "%s %s/%s", kind, folder, name);
		problem("missing %s (in %s)", what, script);
	}
}

static int script_index(const char *name)
{
	for (int i = 0; i < script_count; ++i) {
		if (!strcmp(scripts[i], name)) return i;
	}
	if (script_count >= MAX_SCRIPTS) return -1;
	snprintf(scripts[script_count], sizeof(scripts[0]), "%s", name);
	return script_count++;
}

static GsProgram *compile_file(const char *name, char *err, size_t err_size)
{
	char path[512];
	if (name[0]) snprintf(path, sizeof(path), "%s/%s.3vs", game, name);
	else snprintf(path, sizeof(path), "%s/main.3vs", game);
	char *source = read_all(path);
	if (source == NULL) {
		snprintf(err, err_size, "cannot read %s", path);
		return NULL;
	}
	GsProgram *program = gs_compile_ex(source, load_include, NULL, err, err_size);
	free(source);
	return program;
}

/* One playthrough from main.3vs; returns the last script file reached. */
static const char *play(char *last, size_t last_size)
{
	char err[GS_TEXT_LEN];
	GsProgram *program = compile_file("", err, sizeof(err));
	if (program == NULL) {
		problem("main.3vs does not compile: %s%s", err, "");
		return NULL;
	}
	static GsState state;
	GsEvent event;
	gs_state_init(program, &state);
	for (int i = 0; i < global_count; ++i) gs_variable_set_number(&state, globals[i].name, globals[i].value);
	snprintf(last, last_size, "main");
	long steps = 0;
	for (;;) {
		if (++steps > STEP_LIMIT) {
			problem("script never ends (endless loop?) in %s%s", last, "");
			break;
		}
		gs_next(program, &state, &event);
		if (event.type == GS_EV_END) break;
		if (event.type == GS_EV_ERROR) {
			char where[160];
			snprintf(where, sizeof(where), "%s line %d", last, event.line);
			problem("script error in %s: %s", where, event.text);
			break;
		}
		switch (event.type) {
		case GS_EV_MENU:
			gs_choose(program, &state, rand() % event.choice_count);
			break;
		case GS_EV_BG: check_asset("background", "bg", event.asset, last); break;
		case GS_EV_SHOW: check_asset("sprite", "sprites", event.asset, last); break;
		case GS_EV_MUSIC: check_asset("music", "audio", event.asset, last); break;
		case GS_EV_SFX: check_asset("sound", "audio", event.asset, last); break;
		case GS_EV_SCRIPT: {
			GsProgram *next = compile_file(event.asset, err, sizeof(err));
			if (next == NULL) {
				problem("cannot switch to script %s: %s", event.asset, err);
				goto done;
			}
			gs_free(program);
			program = next;
			snprintf(last, last_size, "%s", event.asset);
			int index = script_index(event.asset);
			if (index >= 0) ++script_hits[index];
			break;
		}
		default:
			break;
		}
	}
done:
	/* carry g_* numbers over to the next playthrough */
	for (int v = 0; v < state.var_count; ++v) {
		const GsVar *var = &state.vars[v];
		if (strncmp(var->name, "g_", 2) || var->value.is_str) continue;
		int found = 0;
		for (int i = 0; i < global_count && !found; ++i) {
			if (!strcmp(globals[i].name, var->name)) {
				globals[i].value = var->value.num;
				found = 1;
			}
		}
		if (!found && global_count < 64) {
			snprintf(globals[global_count].name, sizeof(globals[0].name), "%s", var->name);
			globals[global_count++].value = var->value.num;
		}
	}
	gs_free(program);
	return last;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s GAME_DIR [RUNS] [SEED]\n", argv[0]);
		return 2;
	}
	game = argv[1];
	int runs = argc > 2 ? atoi(argv[2]) : 50;
	srand(argc > 3 ? (unsigned)atoi(argv[3]) : 1u);
	char last[96];
	char endings[64][96];
	int ending_hits[64], ending_count = 0;
	for (int run = 0; run < runs; ++run) {
		if (play(last, sizeof(last)) == NULL) break;
		int i;
		for (i = 0; i < ending_count && strcmp(endings[i], last); ++i) {}
		if (i == ending_count && ending_count < 64) {
			snprintf(endings[ending_count], sizeof(endings[0]), "%s", last);
			ending_hits[ending_count++] = 0;
		}
		if (i < 64) ++ending_hits[i];
	}
	printf("%d runs, %d script files reached:", runs, script_count);
	for (int i = 0; i < script_count; ++i) printf(" %s", scripts[i]);
	printf("\n");
	printf("runs ended in:");
	for (int i = 0; i < ending_count; ++i) printf(" %s(%d)", endings[i], ending_hits[i]);
	printf("\nglobals after the last run:");
	for (int i = 0; i < global_count; ++i) printf(" %s=%ld", globals[i].name, globals[i].value);
	printf("\n%d problems (%d distinct)\n", problem_total, problem_count);
	for (int i = 0; i < problem_count; ++i) printf("  %s\n", problems[i]);
	return problem_count ? 1 : 0;
}
