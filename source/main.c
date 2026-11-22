#include <math.h>
#include <3ds.h>
#include <citro2d.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

#include "audio.h"
#include "gamecfg.h"
#include "display_options.h"
#include "vid.h"
#include "gs.h"
#include "wrap.h"
#include "nvl_page.h"
#include "pack.h"
#include "ftp.h"
#include "savefile.h"

/* The default 32 KB stack is too small for the script VM state. */
u32 __stacksize__ = 256 * 1024;

/* The top screen is 400x240 (3DS). On a Wii U TV it can be 427 wide (16:9): the interface
   uses the extra width, the scene (backgrounds, sprites) stays 400 wide in the middle. */
static float top_w = 400.0f;
#define TOP_W top_w
#define SCENE_X ((TOP_W - 400.0f) / 2.0f)   /* left edge of the 400-wide scene area */
#define TOP_H 240.0f
/* The 3DS bottom screen is 320 wide; the Wii U GamePad (854x480, drawn at 2x) uses its
   whole 16:9 width instead of a 4:3 area with bars. */
#ifdef VN_BOTTOM_W
#define BOTTOM_W ((float)VN_BOTTOM_W)
#elif defined(__WIIU__)
#define BOTTOM_W 427.0f
#else
#define BOTTOM_W 320.0f
#endif
#ifndef ROOT_DIR   /* SDL builds (PC, Wii U) pass their own */
#define ROOT_DIR "sdmc:/3ds/3dvn"
#endif
#define GAMES_DIR ROOT_DIR "/games"
#define SAVES_DIR ROOT_DIR "/saves"
#define SETTINGS_DIR ROOT_DIR "/settings"
#define PACK_SUFFIX ".3vp"   /* a game packed into one archive: games/<name>.3vp */
#define MAX_GAMES 32
#define MAX_TEXTURES 32
#define MAX_GALLERY_ITEMS 512
#define GALLERY_PAGE_SIZE 6
#define SAVE_SLOTS 15
#define LIST_ROWS 8
#define FF_SPEED 8.0f
#define FF_STEP 0.07f
#define AUTO_DELAY_VOICED 0.5f    /* seconds after a voice clip ends */
#define AUTO_DELAY_BASE 1.0f      /* unvoiced lines: base reading time ... */
#define AUTO_DELAY_PER_CHAR 0.04f /* ... plus this per character */
#define AUTO_DELAY_MAX 6.0f
#define LOG_SCALE 0.45f
#define LOG_WIDTH 372.0f
#define LOG_LINE_H 14.0f
#define LOG_SPEAKER_H 14.0f
#define LOG_GAP 7.0f
#define GLOSSARY_VISIBLE_LINES 10

/* Draw depth: citro2d draws higher z on top, so layers must be explicit. */
#define Z_BG 0.10f
#define Z_BG_LOOP1 0.11f
#define Z_BG_LOOP2 0.12f
#define Z_SPRITE 0.20f
#define Z_BOX 0.30f
#define Z_TEXT 0.40f
#define Z_OVERLAY 0.50f
#define Z_OVERLAY_TEXT 0.60f
#define Z_HEADER 0.70f
#define Z_HEADER_TEXT 0.80f
#define SCRIPT_FILE "main.3vs"
#define EXTRAS_FILE "extras.3vs"
#define SAVE_EXT ".3vsav"
#define TEXT_CHARS_PER_SEC 45.0f

typedef struct {
	C3D_Tex tex;
	Tex3DS_SubTexture sub;
	C2D_Image image;
	int width, height;
} Picture;

typedef struct {
	char key[GS_STR_LEN];
	Picture *picture;
	unsigned last_used;
} CacheEntry;



/* Transient screen effects; not saved. */
static float shake_left, shake_total, shake_amp, flash_left, flash_total, flash_shade;
static float fx_x, fx_y;
static float cam_pan_x, cam_pan_y, cam_zoom = 1.0f;
static float cam_from[3], cam_to[3] = {0.0f, 0.0f, 1.0f}, cam_t = 1.0f, cam_dur;
static float hop_t[16]; /* seconds left, indexed by sprite slot */
static float cur_hop;





typedef struct {
	int used;
	long long saved_at;
	char preview[72];
} SlotInfo;

typedef struct {
	float x, y, w, h;
	const char *label;
} Button;

typedef enum {
	PANEL_NONE, PANEL_MENU, PANEL_BACKLOG, PANEL_SAVE, PANEL_LOAD, PANEL_GLOSSARY, PANEL_QUIT, PANEL_OPTIONS,
	PANEL_ENDINGS, PANEL_GUIDE, PANEL_SYNC
} Panel;

typedef enum { MODE_SELECT, MODE_TITLE, MODE_PLAY, MODE_ERROR } Mode;
typedef enum { TITLE_START, TITLE_LOAD, TITLE_OPTIONS, TITLE_EXTRAS, TITLE_ENDINGS, TITLE_GUIDE, TITLE_SYNC } TitleItem;
typedef enum {
	PLAY_NONE, PLAY_TEXT, PLAY_MENU, PLAY_TIMER, PLAY_END, PLAY_VIDEO, PLAY_WAITBG, PLAY_GALLERY
} Wait;

#define SELECT_ROWS 6          /* game list rows on the bottom screen */
#define SELECT_EXIT_Y 200.0f   /* the game list's Exit button */
static int start_requested;    /* acts like START on the next frame */
static char game_names[MAX_GAMES][64];
static char game_titles[MAX_GAMES][64];   /* "title" from each game's game.3vc, or empty */
static int game_count;
static char game_dir[160];
static char game_name[64];
static GsProgram *program;
static char current_script[GS_STR_LEN]; /* script switched to by `script`; empty while main.3vs runs */
static GsState state;
static GsEvent event;
/* The last line of dialogue, kept on screen during a wait that directly follows it
   (sound may play in between): the original games keep their text window up. */
static GsEvent held_say;
static int say_held;
static Scene scene;
static GsBgLoop bgloop;
static NvlPage nvl_page;
static Picture *loop_pictures[3];
static Mode mode = MODE_SELECT;
static Wait waiting = PLAY_NONE;
static char message[GS_TEXT_LEN];
static char notice[96];

#ifdef AUTOTEST
/* Emulator test build only: log warnings, errors, chapter loads and text fitting. */
#include <stdarg.h>
static void test_log(const char *format, ...)
{
	static FILE *file;
	if (file == NULL) {
#ifdef VN_SDL
		file = fopen(ROOT_DIR "/3dvn-test.log", "a");
#else
		file = fopen("sdmc:/3dvn-test.log", "a");
#endif
		if (file == NULL) return;
	}
	va_list args;
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);
	fputc('\n', file);
	fflush(file);
}
#define TEST_LOG(...) test_log(__VA_ARGS__)
#else
#define TEST_LOG(...) ((void)0)
#endif
static float timer_left;
static float chars_shown;
static u32 advance_started;
static int advance_visible;
static int advance_available;
static int menu_cursor;
static int select_cursor;
static int title_items[7], title_count, title_cursor;
static int extras_available, extras_mode;
static Panel panel;
static int panel_cursor;
static int last_slot;
static int backlog_view;
static unsigned script_hash;
static Backlog backlog;
static WrapLayout log_layout[BACKLOG_MAX];
/* The voice clip that played with each backlog line (kept outside saves: after loading,
   earlier lines have none). */
static char log_voice[BACKLOG_MAX][GS_STR_LEN];
static char pending_voice[GS_STR_LEN];   /* last voice started since the previous line */
static SlotInfo slots[SAVE_SLOTS];
static C2D_TextBuf measure_buf;
static GameConfig config;
static DisplayOptions display_options;

/* Text-on-bottom-screen layout: the option applies to ADV text only (NVL stays on top). */
static int off_tv;   /* Wii U: everything on the GamePad (see OFF_TV) */

static int bottom_text_active(void)
{
	return display_options.bottom_text && !display_options.nvl && !off_tv;
}

/* "Top, choices below": text stays on the top screen; the bottom screen holds the
   control strip and the choices as touch buttons. */
static int choices_below_active(void)
{
	return !bottom_text_active() && display_options.touch_choices && !off_tv;
}

/* Either touch layout: strip buttons, tappable choices, tap to advance. */
static int touch_layout_active(void)
{
	return bottom_text_active() || choices_below_active();
}

/* Text speed and auto-mode pacing from Options. */
static float text_chars_per_sec(void)
{
	static const float speeds[] = {25.0f, 45.0f, 90.0f, 100000.0f};
	int index = display_options.text_speed;
	return speeds[index >= 0 && index <= 3 ? index : 1];
}

static float auto_delay_factor(void)
{
	static const float factors[] = {0.5f, 1.0f, 1.7f};
	int index = display_options.auto_speed;
	return factors[index >= 0 && index <= 2 ? index : 1];
}

/* ---------- read lines (for "Fast-forward: Read only") ----------
   One bit per script position, per script file, saved as settings/<game>.3vread:
   repeated [u8 name length][name][u32 little-endian byte count][bits]. */
#define READ_MAX_SCRIPTS 192
typedef struct {
	char name[64];
	unsigned char *bits;
	int bytes;
} ReadMap;
static ReadMap read_maps[READ_MAX_SCRIPTS];
static int read_map_count, read_dirty;

static ReadMap *read_map(const char *name, int create)
{
	for (int index = 0; index < read_map_count; ++index) {
		if (!strcmp(read_maps[index].name, name)) return &read_maps[index];
	}
	if (!create || read_map_count == READ_MAX_SCRIPTS || strlen(name) >= sizeof(read_maps[0].name)) return NULL;
	ReadMap *map = &read_maps[read_map_count++];
	snprintf(map->name, sizeof(map->name), "%s", name);
	map->bits = NULL;
	map->bytes = 0;
	return map;
}

static int line_read(const char *script, int pc)
{
	ReadMap *map = read_map(script, 0);
	return map != NULL && pc >= 0 && pc / 8 < map->bytes && (map->bits[pc / 8] >> (pc % 8) & 1);
}

static void mark_read(const char *script, int pc)
{
	ReadMap *map = read_map(script, 1);
	if (map == NULL || pc < 0 || line_read(script, pc)) return;
	if (pc / 8 >= map->bytes) {
		int bytes = pc / 8 + 64;
		unsigned char *grown = realloc(map->bits, (size_t)bytes);
		if (grown == NULL) return;
		memset(grown + map->bytes, 0, (size_t)(bytes - map->bytes));
		map->bits = grown;
		map->bytes = bytes;
	}
	map->bits[pc / 8] |= (unsigned char)(1u << (pc % 8));
	++read_dirty;
}

/* Global variables: number variables whose names start with "g_" keep their value across
   new games and saves of the same game (e.g. endings seen, routes unlocked). They live in
   SAVES_DIR/<game>.3vglobal as "name value" lines, are put into the state when a game
   starts or a save is loaded, and the file is rewritten whenever one changes. */
#define MAX_GLOBALS 64
static struct { char name[GS_NAME_LEN]; long number; } globals[MAX_GLOBALS];
static int global_count;

static void draw_endings_top(void);
static void draw_sync_top(void);
static void sync_open(void);
static void draw_guide_top(void);
static void close_panel(void);

static int is_global_name(const char *name)
{
	return name[0] == 'g' && name[1] == '_';
}

static void globals_path(char *path, size_t size)
{
	snprintf(path, size, SAVES_DIR "/%s.3vglobal", game_name);
}

static void globals_load(void)
{
	global_count = 0;
	char path[220], line[96];
	globals_path(path, sizeof(path));
	FILE *file = fopen(path, "r");
	if (file == NULL) return;
	while (global_count < MAX_GLOBALS && fgets(line, sizeof(line), file) != NULL) {
		char name[GS_NAME_LEN];
		long number;
		if (sscanf(line, "%31s %ld", name, &number) == 2 && is_global_name(name)) {
			snprintf(globals[global_count].name, sizeof(globals[0].name), "%s", name);
			globals[global_count++].number = number;
		}
	}
	fclose(file);
}

static void globals_save(void)
{
	if (game_name[0] == '\0') return;
	if ((mkdir(ROOT_DIR, 0777) != 0 && errno != EEXIST) || (mkdir(SAVES_DIR, 0777) != 0 && errno != EEXIST)) return;
	char path[220], temporary[230];
	globals_path(path, sizeof(path));
	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	FILE *file = fopen(temporary, "w");
	if (file == NULL) return;
	int failed = 0;
	for (int index = 0; index < global_count && !failed; ++index) {
		failed = fprintf(file, "%s %ld\n", globals[index].name, globals[index].number) < 0;
	}
	if (fclose(file) != 0) failed = 1;
	if (!failed) {
		remove(path);
		failed = rename(temporary, path) != 0;
	}
	if (failed) remove(temporary);
}

/* Puts the stored globals into the running state (new game or loaded save). */
static void globals_apply(void)
{
	for (int index = 0; index < global_count; ++index) {
		gs_variable_set_number(&state, globals[index].name, globals[index].number);
	}
}

/* Stores globals the script changed; called after the script runs. */
static void globals_sync(void)
{
	int changed = 0;
	for (int index = 0; index < state.var_count; ++index) {
		const GsVar *var = &state.vars[index];
		if (!is_global_name(var->name) || var->value.is_str) continue;
		int found = 0;
		for (int known = 0; known < global_count && !found; ++known) {
			if (strcmp(globals[known].name, var->name) == 0) {
				found = 1;
				if (globals[known].number != var->value.num) {
					globals[known].number = var->value.num;
					changed = 1;
				}
			}
		}
		if (!found && global_count < MAX_GLOBALS) {
			snprintf(globals[global_count].name, sizeof(globals[0].name), "%s", var->name);
			globals[global_count++].number = var->value.num;
			changed = 1;
		}
	}
	if (changed) globals_save();
}

/* Playtime per game in SETTINGS_DIR/<game>.3vtime (seconds). It counts only while the
   story is on screen with no menu open, and not after 90 s without input or a new line. */
static double playtime;
static float playtime_unsaved;
static u32 last_activity;
#define IDLE_LIMIT_MS 90000u

static void playtime_path(char *path, size_t size)
{
	snprintf(path, size, SETTINGS_DIR "/%s.3vtime", game_name);
}

static void playtime_load(void)
{
	char path[220];
	playtime = 0.0;
	playtime_unsaved = 0.0f;
	playtime_path(path, sizeof(path));
	FILE *file = fopen(path, "r");
	if (file != NULL) {
		if (fscanf(file, "%lf", &playtime) != 1 || playtime < 0.0) playtime = 0.0;
		fclose(file);
	}
}

static void playtime_save(void)
{
	if (game_name[0] == '\0' || playtime_unsaved <= 0.0f) return;
	if ((mkdir(ROOT_DIR, 0777) != 0 && errno != EEXIST) || (mkdir(SETTINGS_DIR, 0777) != 0 && errno != EEXIST)) return;
	char path[220];
	playtime_path(path, sizeof(path));
	FILE *file = fopen(path, "w");
	if (file == NULL) return;
	fprintf(file, "%.0f\n", playtime);
	fclose(file);
	playtime_unsaved = 0.0f;
}

/* "Playtime 1:05   14:32" (hours:minutes played, and the time of day in 24 h) */
static void status_text(char *out, size_t size)
{
	long minutes = (long)(playtime / 60.0);
	char clock[8] = "";
	time_t now = time(NULL);
	struct tm *parts = localtime(&now);
	if (parts != NULL) strftime(clock, sizeof(clock), "%H:%M", parts);
	snprintf(out, size, "Playtime %ld:%02ld   %s", minutes / 60, minutes % 60, clock);
}

/* ---- Endings checklist ----
   A game lists its endings in endings.txt (in the game folder / archive), or in
   games/<game>.endings.txt next to it (that one wins), one per line:
       Sora - good ending = g_0a g_10
   An ending counts as seen when any of the globals after "=" is non-zero. When a story
   ends ("stop", or a "return" with nothing to return to) the engine itself sets
   g_end_<chapter file>, e.g. g_end_gm04, so games without their own flags can use that. */
#define MAX_ENDINGS 40
#define ENDING_VARS 6
typedef struct {
	char name[64];
	char vars[ENDING_VARS][GS_NAME_LEN];
	int var_count;
} Ending;
static Ending endings[MAX_ENDINGS];
static int ending_count, ending_top;

static void endings_parse(const char *text)
{
	ending_count = 0;
	while (*text && ending_count < MAX_ENDINGS) {
		const char *end = strchr(text, '\n');
		size_t length = end != NULL ? (size_t)(end - text) : strlen(text);
		char line[256];
		if (length >= sizeof(line)) length = sizeof(line) - 1;
		memcpy(line, text, length);
		line[length] = '\0';
		text += length + (end != NULL);
		char *equals = strrchr(line, '=');
		char *start = line;
		while (*start == ' ' || *start == '\t' || *start == '\r') ++start;
		if (equals == NULL || *start == '#') continue;
		*equals = '\0';
		Ending *ending = &endings[ending_count];
		char *name_end = equals;
		while (name_end > start && (name_end[-1] == ' ' || name_end[-1] == '\t')) --name_end;
		*name_end = '\0';
		snprintf(ending->name, sizeof(ending->name), "%s", start);
		ending->var_count = 0;
		for (char *word = strtok(equals + 1, " \t\r,|"); word != NULL && ending->var_count < ENDING_VARS;
			word = strtok(NULL, " \t\r,|")) {
			if (is_global_name(word)) {
				snprintf(ending->vars[ending->var_count++], GS_NAME_LEN, "%s", word);
			}
		}
		if (ending->name[0] != '\0' && ending->var_count > 0) ++ending_count;
	}
}

static unsigned char *read_file(const char *path, size_t *size);

static void endings_load(void)
{
	char path[256];
	ending_count = 0;
	snprintf(path, sizeof(path), GAMES_DIR "/%s.endings.txt", game_name);
	FILE *file = fopen(path, "rb");
	char *text = NULL;
	if (file != NULL) {
		text = calloc(1, 16384);
		if (text != NULL) fread(text, 1, 16383, file);
		fclose(file);
	} else {
		size_t size = 0;
		snprintf(path, sizeof(path), "%s/endings.txt", game_dir);
		unsigned char *data = read_file(path, &size);
		if (data != NULL) {
			text = calloc(1, size + 1);
			if (text != NULL) memcpy(text, data, size);
			free(data);
		}
	}
	if (text != NULL) endings_parse(text);
	free(text);
}

static int ending_seen(const Ending *ending)
{
	for (int v = 0; v < ending->var_count; ++v) {
		for (int g = 0; g < global_count; ++g) {
			if (globals[g].number != 0 && strcmp(globals[g].name, ending->vars[v]) == 0) return 1;
		}
	}
	return 0;
}

/* The story ended: remember in which chapter file (g_end_<chapter>). */
static void record_story_end(void)
{
	const char *script = gs_variable_text(&state, "_script");
	const char *base = script != NULL && script[0] ? script : "main";
	const char *slash = strrchr(base, '/');
	if (slash != NULL) base = slash + 1;
	char name[GS_NAME_LEN];
	snprintf(name, sizeof(name), "g_end_%s", base);
	char *dot = strrchr(name, '.');
	if (dot != NULL && dot > name + 6) *dot = '\0';
	gs_variable_set_number(&state, name, 1);
	globals_sync();
}

/* ---- Guide: games/<game>.txt, read on the bottom screen ---- */
static float measure_text(const char *text, size_t length, void *user);
static float swap_measure_scale(float scale);   /* returns the previous one */
static char guide_file[256];
static char *guide_text;          /* the file as UTF-8 */
static int *guide_starts, *guide_ends, guide_lines, guide_capacity;
static float guide_scroll, guide_velocity;   /* pixels from the top; swipe momentum */
static int guide_drag, guide_drag_moved;
static float guide_drag_y;
#define GUIDE_SCALE 0.42f
#define GUIDE_TOP 22.0f
#define GUIDE_BOTTOM 206.0f
#define GUIDE_LINE_H (LINE_UNITS_MAIN * GUIDE_SCALE)
#define LINE_UNITS_MAIN 30.0f

static void guide_find(void)
{
	guide_file[0] = '\0';
	const char *names[2] = {game_name, config.title};
	for (int i = 0; i < 2; ++i) {
		if (names[i][0] == '\0') continue;
		char path[256];
		snprintf(path, sizeof(path), GAMES_DIR "/%s.txt", names[i]);
		FILE *file = fopen(path, "rb");
		if (file != NULL) {
			fclose(file);
			snprintf(guide_file, sizeof(guide_file), "%s", path);
			return;
		}
	}
}

static void guide_free(void)
{
	free(guide_text);
	free(guide_starts);
	free(guide_ends);
	guide_text = NULL;
	guide_starts = guide_ends = NULL;
	guide_lines = guide_capacity = 0;
}

static void guide_position_path(char *path, size_t size)
{
	snprintf(path, size, SETTINGS_DIR "/%s.3vguide", game_name);
}

static void guide_add_line(int start, int end)
{
	if (guide_lines == guide_capacity) {
		int capacity = guide_capacity ? guide_capacity * 2 : 1024;
		int *starts = realloc(guide_starts, sizeof(int) * (size_t)capacity);
		if (starts == NULL) return;
		guide_starts = starts;
		int *ends = realloc(guide_ends, sizeof(int) * (size_t)capacity);
		if (ends == NULL) return;
		guide_ends = ends;
		guide_capacity = capacity;
	}
	guide_starts[guide_lines] = start;
	guide_ends[guide_lines++] = end;
}

static int valid_utf8(const unsigned char *data, size_t size)
{
	for (size_t i = 0; i < size;) {
		unsigned char c = data[i];
		int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
		if (extra < 0 || i + (size_t)extra >= size + (extra == 0)) return 0;
		for (int k = 1; k <= extra; ++k) if ((data[i + k] & 0xC0) != 0x80) return 0;
		i += (size_t)extra + 1;
	}
	return 1;
}

/* Reads the guide and breaks it into lines that fit the bottom screen. */
static int guide_load(void)
{
	guide_free();
	FILE *file = fopen(guide_file, "rb");
	if (file == NULL) return 0;
	fseek(file, 0, SEEK_END);
	long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (length < 0 || length > 8 * 1024 * 1024) { fclose(file); return 0; }
	unsigned char *raw = malloc((size_t)length + 1);
	if (raw == NULL) { fclose(file); return 0; }
	size_t got = fread(raw, 1, (size_t)length, file);
	fclose(file);
	size_t start = got >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF ? 3 : 0;
	int utf8 = valid_utf8(raw + start, got - start);
	guide_text = malloc((got - start) * 4 + 1);   /* tabs become 4 spaces; Latin-1 bytes 2 */
	if (guide_text == NULL) { free(raw); return 0; }
	size_t used = 0;
	for (size_t i = start; i < got; ++i) {
		unsigned char c = raw[i];
		if (c == '\r') continue;
		if (c == '\t') { memcpy(guide_text + used, "    ", 4); used += 4; continue; }
		if (c >= 0x80 && !utf8) {   /* not UTF-8: read as Latin-1 */
			guide_text[used++] = (char)(0xC0 | c >> 6);
			guide_text[used++] = (char)(0x80 | (c & 0x3F));
			continue;
		}
		guide_text[used++] = (char)c;
	}
	guide_text[used] = '\0';
	free(raw);
	/* Text files are often wrapped at ~72 columns, wider than the screen: a sentence broken
	   there would show as a long line plus a stub. Rejoin a long line with the next one
	   when that continues the sentence (starts with a letter, not indented or a list). */
	for (char *line = guide_text; *line;) {
		char *newline = strchr(line, '\n');
		if (newline == NULL) break;
		char *next = newline + 1;
		size_t length = (size_t)(newline - line);
		char last = length > 0 ? newline[-1] : '\0';
		int continues = length >= 50 && line[0] != ' ' && last != ':' && last != ' '
			&& ((*next >= 'a' && *next <= 'z') || (*next >= 'A' && *next <= 'Z') || *next == '(' || *next == '"')
			&& !(next[1] == '.' || next[1] == ')');
		if (continues) *newline = ' ';
		line = next;
	}

	float previous = swap_measure_scale(GUIDE_SCALE);
	const float width = BOTTOM_W - 34.0f;
	for (int at = 0; guide_text[at] != '\0' || at == 0;) {
		const char *newline = strchr(guide_text + at, '\n');
		int end = newline != NULL ? (int)(newline - guide_text) : (int)used;
		/* greedy wrap of [at, end) at spaces; very long words are cut */
		int line = at;
		while (line < end) {
			int fit = line, last_space = -1, i = line;
			while (i < end) {
				int next = i + 1;
				while (next < end && ((unsigned char)guide_text[next] & 0xC0) == 0x80) ++next;
				if (measure_text(guide_text + line, (size_t)(next - line), NULL) > width && fit > line) break;
				if (guide_text[i] == ' ') last_space = i;
				fit = next;
				i = next;
			}
			if (fit < end && last_space > line) fit = last_space + 1;
			int shown_end = fit;
			while (shown_end > line && guide_text[shown_end - 1] == ' ') --shown_end;
			guide_add_line(line, shown_end);
			line = fit;
			/* a divider (one symbol repeated, like ~~~~ or ----) is cut to fit, not wrapped */
			int divider = end - at >= 8 && !isalnum((unsigned char)guide_text[at]) && guide_text[at] != ' ';
			for (int k = at; divider && k < end; ++k) divider = guide_text[k] == guide_text[at];
			if (divider) line = end;
		}
		if (line == at) guide_add_line(at, at);   /* empty line */
		if (newline == NULL) break;
		at = end + 1;
	}
	swap_measure_scale(previous);

	/* back to where reading stopped (a byte offset, so it survives other line widths) */
	guide_scroll = 0.0f;
	guide_velocity = 0.0f;
	char path[220];
	guide_position_path(path, sizeof(path));
	FILE *saved = fopen(path, "r");
	long offset = 0;
	if (saved != NULL) {
		if (fscanf(saved, "%ld", &offset) != 1) offset = 0;
		fclose(saved);
	}
	for (int index = 0; index < guide_lines; ++index) {
		if (guide_starts[index] > offset) break;
		guide_scroll = (float)index * GUIDE_LINE_H;
	}
	return 1;
}

static float guide_max_scroll(void)
{
	float total = (float)guide_lines * GUIDE_LINE_H - (GUIDE_BOTTOM - GUIDE_TOP);
	return total > 0.0f ? total : 0.0f;
}

static void guide_save_position(void)
{
	if (guide_lines == 0) return;
	if ((mkdir(ROOT_DIR, 0777) != 0 && errno != EEXIST) || (mkdir(SETTINGS_DIR, 0777) != 0 && errno != EEXIST)) return;
	int top = (int)(guide_scroll / GUIDE_LINE_H + 0.5f);
	if (top >= guide_lines) top = guide_lines - 1;
	char path[220];
	guide_position_path(path, sizeof(path));
	FILE *file = fopen(path, "w");
	if (file == NULL) return;
	fprintf(file, "%d\n", guide_starts[top]);
	fclose(file);
}

static void read_path(char *path, size_t size)
{
	snprintf(path, size, SETTINGS_DIR "/%s.3vread", game_name);
}

static void read_clear(void)
{
	for (int index = 0; index < read_map_count; ++index) free(read_maps[index].bits);
	read_map_count = 0;
	read_dirty = 0;
}

static void read_load(void)
{
	read_clear();
	char path[220];
	read_path(path, sizeof(path));
	FILE *file = fopen(path, "rb");
	if (file == NULL) return;
	int length;
	while ((length = fgetc(file)) != EOF) {
		char name[64];
		unsigned char size[4];
		if (length >= (int)sizeof(name) || fread(name, 1, (size_t)length, file) != (size_t)length
			|| fread(size, 1, 4, file) != 4) break;
		name[length] = '\0';
		long bytes = size[0] | size[1] << 8 | size[2] << 16 | (long)size[3] << 24;
		ReadMap *map = bytes > 0 && bytes < (1 << 20) ? read_map(name, 1) : NULL;
		if (map == NULL || map->bits != NULL) break;
		map->bits = calloc(1, (size_t)bytes);
		if (map->bits == NULL || fread(map->bits, 1, (size_t)bytes, file) != (size_t)bytes) break;
		map->bytes = (int)bytes;
	}
	fclose(file);
	read_dirty = 0;
}

static void read_save(void)
{
	if (!read_dirty || game_name[0] == '\0') return;
	if ((mkdir(ROOT_DIR, 0777) != 0 && errno != EEXIST) || (mkdir(SETTINGS_DIR, 0777) != 0 && errno != EEXIST)) return;
	char path[220], temporary[230];
	read_path(path, sizeof(path));
	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	FILE *file = fopen(temporary, "wb");
	if (file == NULL) return;
	int failed = 0;
	for (int index = 0; index < read_map_count && !failed; ++index) {
		const ReadMap *map = &read_maps[index];
		unsigned char head[4] = {(unsigned char)map->bytes, (unsigned char)(map->bytes >> 8),
			(unsigned char)(map->bytes >> 16), (unsigned char)(map->bytes >> 24)};
		size_t length = strlen(map->name);
		failed = fputc((int)length, file) == EOF || fwrite(map->name, 1, length, file) != length
			|| fwrite(head, 1, 4, file) != 4 || fwrite(map->bits, 1, (size_t)map->bytes, file) != (size_t)map->bytes;
	}
	if (fclose(file) != 0) failed = 1;
	if (!failed) {
		remove(path);
		failed = rename(temporary, path) != 0;
	}
	if (failed) {
		remove(temporary);
	} else {
		read_dirty = 0;
	}
}
static int custom_textbox_available, custom_nvl_available;
static Glossary glossary;
static int gloss_indices[CFG_MAX_TERMS], gloss_visible_count;
static unsigned short gloss_starts[CFG_TEXT_LEN], gloss_ends[CFG_TEXT_LEN];
static int gloss_line_count, gloss_scroll, gloss_detail;
static int gloss_layout_for = -1;
static int menu_items[LABEL_COUNT];
static int menu_count;
static int menu_cursor_item;
static Panel panel_return;
static int ui_hidden;
static int fast_forward;
static float ff_wait;
/* Auto mode: advance by itself once the line is shown and its voice has finished. */
static int auto_mode;
static float auto_wait;
static int line_voiced;
static int text_pc;      /* script position of the line on screen (read tracking) */
static int ff_blocked;   /* R held, but the line is unread and Fast-forward is "Read only" */
static char gallery_items[MAX_GALLERY_ITEMS][GS_STR_LEN];
static int gallery_count, gallery_cursor, gallery_detail;

static CacheEntry cache[MAX_TEXTURES];
static unsigned cache_clock;
static C2D_Font font;         /* in use: the game's font if it has one */
static C2D_Font system_font;
static C2D_Font game_font;

typedef struct {
	FILE *file;
	VidHeader header;
	unsigned *offsets;
	unsigned char *buffer;
	Picture *picture;
	float elapsed;
	unsigned shown;
	int skippable, has_audio;
} Video;
static Video video;
static C2D_TextBuf text_buf;

static u32 now_ms(void) { return (u32)osGetTime(); }
static void fail_with(const char *text);
static int switch_script(const char *name);
static void stop_video(void);

/* ---------- images ---------- */

static unsigned next_pow2(unsigned value)
{
	unsigned result = 8;
	while (result < value) {
		result <<= 1;
	}
	return result;
}

static unsigned morton(unsigned x, unsigned y)
{
	return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2)
		| ((x & 4) << 2) | ((y & 4) << 3);
}

static unsigned char *read_file(const char *path, size_t *size)
{
	int handled;
	unsigned char *whole = vn_read_whole(path, size, &handled);
	if (handled) return whole;
	FILE *file = vn_fopen(path, "rb");
	if (file == NULL) {
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	unsigned char *data = length > 0 ? malloc((size_t)length + 1) : NULL;
	if (data != NULL && fread(data, 1, (size_t)length, file) != (size_t)length) {
		free(data);
		data = NULL;
	}
	fclose(file);
	if (data != NULL) {
		data[length] = '\0';
		*size = (size_t)length;
	}
	return data;
}

/* Allocates an empty texture of the given size; fill_picture uploads pixels into it. */
static Picture *create_picture(int width, int height)
{
	unsigned tex_w = next_pow2((unsigned)width);
	unsigned tex_h = next_pow2((unsigned)height);
	Picture *picture = calloc(1, sizeof(Picture));
	if (picture == NULL || !C3D_TexInit(&picture->tex, (u16)tex_w, (u16)tex_h, GPU_RGBA8)) {
		free(picture);
		return NULL;
	}
	memset(picture->tex.data, 0, (size_t)tex_w * tex_h * 4);
	C3D_TexSetFilter(&picture->tex, GPU_LINEAR, GPU_LINEAR);
	picture->width = width;
	picture->height = height;
	picture->sub.width = (u16)width;
	picture->sub.height = (u16)height;
	picture->sub.left = 0.0f;
	picture->sub.right = (float)width / tex_w;
	picture->sub.top = 1.0f;
	picture->sub.bottom = 1.0f - (float)height / tex_h;
	picture->image.tex = &picture->tex;
	picture->image.subtex = &picture->sub;
	return picture;
}

/* RGBA pixels of exactly picture->width x picture->height, swizzled into the GPU's tiled layout. */
static void fill_picture(Picture *picture, const unsigned char *pixels)
{
#ifdef VN_SDL
	/* SDL builds keep textures linear; the platform layer uploads them. */
	C3D_TexUploadRGBA(&picture->tex, pixels, picture->width, picture->height);
	return;
#endif
	static unsigned char lut[8][8];
	static int lut_ready;
	if (!lut_ready) {
		for (unsigned y = 0; y < 8; ++y) {
			for (unsigned x = 0; x < 8; ++x) {
				lut[y][x] = (unsigned char)morton(x, y);
			}
		}
		lut_ready = 1;
	}
	unsigned tex_w = picture->tex.width, tex_h = picture->tex.height;
	u32 *out = picture->tex.data;
	for (int y = 0; y < picture->height; ++y) {
		unsigned row = (unsigned)y;
		const unsigned char *src = &pixels[(size_t)y * picture->width * 4];
		u32 *tile_row = &out[(row / 8) * (tex_w / 8) * 64];
		const unsigned char *lut_row = lut[row & 7];
		for (int x = 0; x < picture->width; ++x, src += 4) {
			tile_row[((unsigned)x / 8) * 64 + lut_row[(unsigned)x & 7]] =
				((u32)src[0] << 24) | ((u32)src[1] << 16) | ((u32)src[2] << 8) | src[3];
		}
	}
	GSPGPU_FlushDataCache(picture->tex.data, (size_t)tex_w * tex_h * 4);
}

static Picture *load_picture_timed(const char *path);

static Picture *load_picture(const char *path)
{
#ifdef VN_SDL
	u32 started = now_ms();
	Picture *picture = load_picture_timed(path);
	u32 took = now_ms() - started;
	if (took > 30) {
		size_t root = strlen(game_dir);
		vn_log("load %u ms: %s", (unsigned)took, strncmp(path, game_dir, root) ? path : path + root);
	}
	return picture;
#else
	return load_picture_timed(path);
#endif
}

static Picture *load_picture_timed(const char *path)
{
	size_t size;
	unsigned char *file_data = read_file(path, &size);
	if (file_data == NULL) {
		return NULL;
	}
	int width, height, channels;
#ifdef VN_SDL
	/* HD images (PC, Wii U): <game>/hd/<folder>/<file> is drawn in place of <folder>/<file>,
	   at the normal image's size, so layouts are unchanged and only detail improves. */
	size_t root_length = strlen(game_dir);
	if (game_dir[0] != '\0' && !strncmp(path, game_dir, root_length) && path[root_length] == '/'
		&& stbi_info_from_memory(file_data, (int)size, &width, &height, &channels)) {
		char hd_path[600];
		snprintf(hd_path, sizeof(hd_path), "%s/hd%s", game_dir, path + root_length);
		/* the HD file may use the other format (e.g. lossless .png for a small .jpg) */
		char *dot = strrchr(hd_path, '.');
		if (!vn_exists(hd_path) && dot != NULL && strlen(dot) == 4) {
			const char *other = !strcasecmp(dot, ".jpg") ? ".png" : !strcasecmp(dot, ".png") ? ".jpg" : NULL;
			if (other != NULL) memcpy(dot, other, 4);
		}
		size_t hd_size;
		unsigned char *hd_data = vn_exists(hd_path) ? read_file(hd_path, &hd_size) : NULL;
		int hd_w, hd_h, hd_channels;
		unsigned char *hd_pixels = hd_data != NULL
			? stbi_load_from_memory(hd_data, (int)hd_size, &hd_w, &hd_h, &hd_channels, 4) : NULL;
		free(hd_data);
		Picture *picture = hd_pixels != NULL && width <= 1024 && height <= 1024 ? create_picture(width, height) : NULL;
		if (picture != NULL) {
			C3D_TexUploadHD(&picture->tex, hd_pixels, hd_w, hd_h, width, height);
			stbi_image_free(hd_pixels);
			free(file_data);
			return picture;
		}
		stbi_image_free(hd_pixels);
	}
#endif
	unsigned char *pixels = stbi_load_from_memory(file_data, (int)size, &width, &height, &channels, 4);
	free(file_data);
	if (pixels == NULL) {
		return NULL;
	}
	Picture *picture = width <= 1024 && height <= 1024 ? create_picture(width, height) : NULL;
	if (picture != NULL) {
		fill_picture(picture, pixels);
	}
	stbi_image_free(pixels);
	return picture;
}

static void free_picture(Picture *picture)
{
	if (picture != NULL) {
		C3D_TexDelete(&picture->tex);
		free(picture);
	}
}

static void clear_cache(void)
{
	for (int index = 0; index < MAX_TEXTURES; ++index) {
		free_picture(cache[index].picture);
		memset(&cache[index], 0, sizeof(cache[index]));
	}
}

static void clear_bgloop(void)
{
	if (bgloop.active) {
		/* A cache miss may have been caused by memory held by the loop. */
		for (int i = 0; i < MAX_TEXTURES; ++i) {
			if (cache[i].key[0] && !cache[i].picture) memset(&cache[i], 0, sizeof(cache[i]));
		}
	}
	for (int i = 0; i < 3; ++i) {
		free_picture(loop_pictures[i]);
		loop_pictures[i] = NULL;
	}
	memset(&bgloop, 0, sizeof(bgloop));
}

/* Loop textures are owned separately, so cache churn never evicts a displayed layer. */
static Picture *load_loop_picture(const char *name)
{
	static const char *extensions[] = {".png", ".jpg", ""};
	for (int i = 0; i < 3; ++i) {
		char path[512];
		snprintf(path, sizeof(path), "%s/bg/%s%s", game_dir, name, extensions[i]);
		Picture *picture = load_picture(path);
		if (picture) return picture;
	}
	return NULL;
}

static int prepare_bgloop(const char *base, const GsBgLoop *next)
{
	const char *names[] = {base, next->overlays[0], next->overlays[1]};
	clear_bgloop();
	clear_cache();
	for (int i = 0; i < 3; ++i) {
		if (names[i][0]) loop_pictures[i] = load_loop_picture(names[i]);
		if (!loop_pictures[i] && !(i == 0 && (!strcmp(names[i], "black") || !strcmp(names[i], "white")))) {
			char error[GS_TEXT_LEN];
			snprintf(error, sizeof(error), "bgloop: cannot load bg/%.255s (missing, invalid or out of memory)", names[i]);
			clear_bgloop();
			fail_with(error);
			return 0;
		}
	}
	bgloop = *next;
	return 1;
}

/* Looks for GAME/folder/name.png, .jpg, then the exact file name; NULL if missing. */
static Picture *get_picture(const char *folder, const char *name)
{
	char key[GS_STR_LEN];
	snprintf(key, sizeof(key), "%s/%s", folder, name);
	int victim = 0;
	for (int index = 0; index < MAX_TEXTURES; ++index) {
		if (cache[index].key[0] != '\0' && strcmp(cache[index].key, key) == 0) {
			cache[index].last_used = ++cache_clock;
			return cache[index].picture;
		}
		if (cache[index].last_used < cache[victim].last_used) {
			victim = index;
		}
	}

	free_picture(cache[victim].picture);
	memset(&cache[victim], 0, sizeof(cache[victim]));
	static const char *extensions[] = {".png", ".jpg", ""};
	Picture *picture = NULL;
	for (int index = 0; index < 3 && picture == NULL; ++index) {
		char path[320];
		snprintf(path, sizeof(path), "%s/%s/%s%s", game_dir, folder, name, extensions[index]);
		picture = load_picture(path);
	}
	snprintf(cache[victim].key, sizeof(cache[victim].key), "%s", key);
	cache[victim].picture = picture;
	cache[victim].last_used = ++cache_clock;
	return picture;
}

/* ---------- scene ---------- */

static Sprite *find_sprite(const char *name)
{
	for (int index = 0; index < MAX_SPRITES; ++index) {
		if (scene.sprites[index].active && strcmp(scene.sprites[index].name, name) == 0) {
			return &scene.sprites[index];
		}
	}
	return NULL;
}

static void apply_event_to_scene(const GsEvent *e)
{
	if (e->type == GS_EV_BG) {
		clear_bgloop();
		snprintf(scene.bg_prev, sizeof(scene.bg_prev), "%s", scene.bg);
		snprintf(scene.bg, sizeof(scene.bg), "%s", e->asset);
		scene.bg_duration = e->seconds;
		scene.bg_elapsed = 0.0f;
		if (e->seconds <= 0.0f) {
			scene.bg_prev[0] = '\0';
		}
		Picture *picture = get_picture("bg", e->asset);
		if (picture == NULL && strcmp(e->asset, "black") != 0 && strcmp(e->asset, "white") != 0) {
			snprintf(notice, sizeof(notice), "missing bg/%s", e->asset);
		}
	} else if (e->type == GS_EV_BGLOOP) {
		GsBgLoop next = {0};
		next.active = 1;
		next.hold = e->seconds;
		next.fade = strtof(e->pos, NULL);
		snprintf(next.overlays[0], GS_STR_LEN, "%s", e->choices[0]);
		snprintf(next.overlays[1], GS_STR_LEN, "%s", e->choices[1]);
		float transition = strtof(e->text, NULL);
		if (!prepare_bgloop(e->asset, &next)) return;
		snprintf(scene.bg_prev, sizeof(scene.bg_prev), "%s", scene.bg);
		snprintf(scene.bg, sizeof(scene.bg), "%s", e->asset);
		scene.bg_elapsed = 0.0f;
		scene.bg_duration = transition;
		if (transition <= 0.0f) scene.bg_prev[0] = '\0';
	} else if (e->type == GS_EV_SHOW) {
		Sprite *sprite = find_sprite(e->name);
		if (sprite == NULL) {
			for (int index = 0; index < MAX_SPRITES && sprite == NULL; ++index) {
				if (!scene.sprites[index].active) {
					sprite = &scene.sprites[index];
				}
			}
		}
		if (sprite == NULL) {
			snprintf(notice, sizeof(notice), "too many sprites shown");
			return;
		}
		int was_active = sprite->active;
		sprite->active = 1;
		snprintf(sprite->name, sizeof(sprite->name), "%s", e->name);
		snprintf(sprite->asset, sizeof(sprite->asset), "%s", e->asset);
		snprintf(sprite->pos, sizeof(sprite->pos), "%s", e->pos);
		sprite->from = was_active ? sprite->alpha : 0.0f;
		sprite->to = 1.0f;
		sprite->duration = e->seconds;
		sprite->elapsed = 0.0f;
		if (e->seconds <= 0.0f) {
			sprite->alpha = 1.0f;
		}
		if (get_picture("sprites", e->asset) == NULL) {
			snprintf(notice, sizeof(notice), "missing sprites/%s", e->asset);
		}
	} else if (e->type == GS_EV_HIDE) {
		Sprite *sprite = find_sprite(e->name);
		if (sprite != NULL) {
			sprite->from = sprite->alpha;
			sprite->to = 0.0f;
			sprite->duration = e->seconds;
			sprite->elapsed = 0.0f;
			if (e->seconds <= 0.0f) {
				sprite->active = 0;
			}
		}
	}
}

static void update_scene(float dt)
{
	fx_x = fx_y = 0.0f;
	if (cam_t < 1.0f) {
		cam_t += cam_dur > 0.0f ? dt / cam_dur : 1.0f;
		if (cam_t > 1.0f) cam_t = 1.0f;
		float e = cam_t * cam_t * (3.0f - 2.0f * cam_t);
		cam_pan_x = cam_from[0] + (cam_to[0] - cam_from[0]) * e;
		cam_pan_y = cam_from[1] + (cam_to[1] - cam_from[1]) * e;
		cam_zoom = cam_from[2] + (cam_to[2] - cam_from[2]) * e;
	}
	for (int i = 0; i < MAX_SPRITES && i < 16; ++i) {
		if (hop_t[i] > 0.0f) hop_t[i] = hop_t[i] > dt ? hop_t[i] - dt : 0.0f;
	}
	if (shake_left > 0.0f) {
		shake_left -= dt;
		if (shake_left < 0.0f) shake_left = 0.0f;
		float fade = shake_total > 0.0f ? shake_left / shake_total : 0.0f;
		float phase = (shake_total - shake_left) * 55.0f;
		fx_x = shake_amp * fade * sinf(phase);
		fx_y = shake_amp * fade * 0.6f * cosf(phase * 1.3f);
	}
	if (flash_left > 0.0f) {
		flash_left -= dt;
		if (flash_left < 0.0f) flash_left = 0.0f;
	}
	float loop_dt = dt;
	if (scene.bg_prev[0] != '\0') {
		loop_dt = gs_bg_fade_advance(&scene.bg_elapsed, scene.bg_duration, dt);
		if (scene.bg_elapsed >= scene.bg_duration) {
			scene.bg_prev[0] = '\0';
		}
	}
	gs_bgloop_update(&bgloop, loop_dt);
	for (int index = 0; index < MAX_SPRITES; ++index) {
		Sprite *sprite = &scene.sprites[index];
		if (!sprite->active || sprite->alpha == sprite->to) {
			continue;
		}
		sprite->elapsed += dt;
		float t = sprite->duration > 0.0f ? sprite->elapsed / sprite->duration : 1.0f;
		if (t >= 1.0f) {
			sprite->alpha = sprite->to;
			if (sprite->to == 0.0f) {
				sprite->active = 0;
			}
		} else {
			sprite->alpha = sprite->from + (sprite->to - sprite->from) * t;
		}
	}
}

/* ---------- games, saving ---------- */

static void play_audio_event(const GsEvent *e, int is_music)
{
	char err[64];
	if (is_music && e->asset[0] == '\0') {
		scene.music[0] = '\0';
		audio_stop_music(e->seconds);
		return;
	}
	if (!is_music && e->asset[0] == '\0') {
		audio_stop_sfx();
		return;
	}
	if (!is_music && !strncmp(e->asset, "v/", 2)) {
		snprintf(pending_voice, sizeof(pending_voice), "%s", e->asset);   /* for the backlog */
	}
	if (!is_music && fast_forward && !strncmp(e->asset, "v/", 2)) {
		/* Voices are cut by the next line anyway while fast-forwarding; loading each
		   one made fast-forward stutter on Wii U. Other sounds (ambience) still play. */
		return;
	}
	static const char *extensions[] = {".ogg", ".wav", ""};
	char path[400];
	for (int index = 0; index < 3; ++index) {
		snprintf(path, sizeof(path), "%s/audio/%s%s", game_dir, e->asset, extensions[index]);
		if (vn_exists(path)) {
			break;
		}
	}
#ifdef VN_SDL
	u32 started = now_ms();
#endif
	int ok;
	if (!is_music && !strcmp(e->pos, "loop")) {
		/* optional loop points: audio/NAME.loop holds "start end" in seconds (end 0 = end of
		   file); without it the whole sound repeats */
		float loop_start = 0.0f, loop_end = 0.0f;
		char loop_path[420];
		size_t loop_size;
		snprintf(loop_path, sizeof(loop_path), "%s/audio/%s.loop", game_dir, e->asset);
		char *loop_text = (char *)read_file(loop_path, &loop_size);
		if (loop_text != NULL) {
			if (sscanf(loop_text, "%f %f", &loop_start, &loop_end) < 1) loop_start = loop_end = 0.0f;
			free(loop_text);
		}
		ok = audio_play_sfx_loop(path, 1, loop_start, loop_end, err, sizeof(err));
	} else {
		ok = is_music ? audio_play_music(path, e->seconds, err, sizeof(err))
			: audio_play_sfx(path, err, sizeof(err));
	}
#ifdef VN_SDL
	u32 took = now_ms() - started;
	if (took > 30) vn_log("audio %u ms: %s", (unsigned)took, e->asset);
#endif
	if (ok) {
		if (is_music) {
			snprintf(scene.music, sizeof(scene.music), "%s", e->asset);
		}
	} else {
		snprintf(notice, sizeof(notice), "audio %.40s: %.40s", e->asset, err);
	}
}

static void scan_games(void)
{
	game_count = 0;
	DIR *dir = opendir(GAMES_DIR);
	if (dir == NULL) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL && game_count < MAX_GAMES) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		char path[320];
		struct stat info;
		size_t length = strlen(entry->d_name);
		if (length > 4 && !strcmp(entry->d_name + length - 4, PACK_SUFFIX)) {
			/* <name>.3vp game archive (skip it if a folder of the same name is also listed) */
			char name[sizeof(game_names[0])];
			snprintf(name, sizeof(name), "%.*s", (int)(length - 4), entry->d_name);
			int listed = 0;
			for (int i = 0; i < game_count; ++i) listed |= !strcmp(game_names[i], name);
			if (!listed) snprintf(game_names[game_count++], sizeof(game_names[0]), "%s", name);
			continue;
		}
		snprintf(path, sizeof(path), GAMES_DIR "/%s/" SCRIPT_FILE, entry->d_name);
		if (stat(path, &info) == 0) {
			int listed = 0;
			for (int i = 0; i < game_count; ++i) listed |= !strcmp(game_names[i], entry->d_name);
			if (!listed) snprintf(game_names[game_count++], sizeof(game_names[0]), "%s", entry->d_name);
		}
	}
	closedir(dir);
	/* Titles for the list: game.3vc in the folder, else in the archive. */
	for (int i = 0; i < game_count; ++i) {
		char config_text[2048], path[320];
		int length = -1;
		snprintf(path, sizeof(path), GAMES_DIR "/%s/game.3vc", game_names[i]);
		FILE *file = fopen(path, "rb");
		if (file != NULL) {
			length = (int)fread(config_text, 1, sizeof(config_text) - 1, file);
			config_text[length > 0 ? length : 0] = '\0';
			fclose(file);
		} else {
			snprintf(path, sizeof(path), GAMES_DIR "/%s" PACK_SUFFIX, game_names[i]);
			length = pack_peek_member(path, "game.3vc", config_text, sizeof(config_text));
		}
		game_titles[i][0] = '\0';
		for (char *line = length > 0 ? config_text : NULL; line != NULL && *line; ) {
			char *next = strchr(line, '\n');
			if (next != NULL) *next++ = '\0';
			if (!strncmp(line, "title", 5) && (line[5] == ' ' || line[5] == '\t')) {
				char *value = line + 6;
				while (*value == ' ' || *value == '\t') ++value;
				size_t end = strlen(value);
				while (end > 0 && (value[end - 1] == '\r' || value[end - 1] == ' ')) value[--end] = '\0';
				snprintf(game_titles[i], sizeof(game_titles[0]), "%s", value);
				break;
			}
			line = next;
		}
	}
}

/* ---------- backlog ---------- */

static float measure_scale = LOG_SCALE;

static float swap_measure_scale(float scale)
{
	float previous = measure_scale;
	measure_scale = scale;
	return previous;
}

static float measure_text(const char *text, size_t length, void *user)
{
	(void)user;
	if (length == 0) {
		return 0.0f;
	}
	char copy[NVL_PAGE_MAX_BYTES + 1];
	if (length >= sizeof(copy)) {
		length = sizeof(copy) - 1;
	}
	memcpy(copy, text, length);
	copy[length] = '\0';
	C2D_Text parsed;
	float width, height;
	C2D_TextBufClear(measure_buf);
	C2D_TextFontParse(&parsed, font, measure_buf, copy);
	C2D_TextGetDimensions(&parsed, measure_scale, measure_scale, &width, &height);
	return width;
}

static int backlog_slot(int age)
{
	return (backlog.start + backlog.count - 1 - age) % BACKLOG_MAX;
}

static void backlog_push(const char *speaker, unsigned color, const char *text)
{
	int slot;
	if (backlog.count < BACKLOG_MAX) {
		slot = (backlog.start + backlog.count++) % BACKLOG_MAX;
	} else {
		slot = backlog.start;
		backlog.start = (backlog.start + 1) % BACKLOG_MAX;
	}
	BacklogEntry *entry = &backlog.entries[slot];
	snprintf(entry->speaker, sizeof(entry->speaker), "%s", speaker);
	snprintf(entry->text, sizeof(entry->text), "%s", text);
	entry->color = color;
	log_layout[slot].count = 0;
	snprintf(log_voice[slot], sizeof(log_voice[slot]), "%s", pending_voice);
	pending_voice[0] = '\0';
}

static const WrapLayout *backlog_layout(int slot)
{
	WrapLayout *layout = &log_layout[slot];
	if (layout->count == 0) {
		wrap_text(backlog.entries[slot].text, LOG_WIDTH, measure_text, NULL, layout);
	}
	return layout;
}

static float backlog_height(int slot)
{
	const BacklogEntry *entry = &backlog.entries[slot];
	return (entry->speaker[0] != '\0' ? LOG_SPEAKER_H : 0.0f)
		+ (float)backlog_layout(slot)->count * LOG_LINE_H + LOG_GAP;
}

/* ---------- saving ---------- */

static void save_path(int slot, char *path, size_t size)
{
	snprintf(path, size, SAVES_DIR "/%s_%d" SAVE_EXT, game_name, slot + 1);
}

static void options_path(char *path, size_t size)
{
	snprintf(path, size, SETTINGS_DIR "/%s.3vopt", game_name);
}

static int current_textbox_available(void)
{
	return display_options.nvl ? (custom_nvl_available || custom_textbox_available)
		: custom_textbox_available;
}

static void apply_screen_options(void)
{
#ifdef __WIIU__
	top_w = (float)vn_wiiu_set_tv_wide(display_options.tv_wide);
#elif defined(VN_SDL)
	top_w = getenv("VN_WIDE") != NULL ? 427.0f : 400.0f;   /* PC test of the wide TV layout */
#endif
}

static void apply_volumes(void)
{
	audio_set_volumes(display_options.master_volume / 10.0f, display_options.music_volume / 10.0f,
		display_options.voice_volume / 10.0f);
}

static void load_display_options(void)
{
	char path[200];
	display_options_defaults(&display_options);
	options_path(path, sizeof(path));
	DisplayOptionsResult result = display_options_load(path, &display_options);
	display_options_apply_game_default(&display_options, result, config.default_nvl);
	apply_volumes();
	apply_screen_options();
	if (result != DISPLAY_OPTIONS_OK && result != DISPLAY_OPTIONS_MISSING) {
		const char *problem = result == DISPLAY_OPTIONS_RECOVERED
			? "Options recovered from backup; check settings files"
			: result == DISPLAY_OPTIONS_MALFORMED ? "Invalid options file" : "Cannot read options";
		if (notice[0] != '\0') {
			size_t used = strlen(notice);
			snprintf(notice + used, sizeof(notice) - used, "; %s", problem);
		} else {
			snprintf(notice, sizeof(notice), "%s%s", problem,
				result == DISPLAY_OPTIONS_RECOVERED ? "" : "; using defaults");
		}
	}
}

/* Options rows 6-8 are the volumes. A steps up and wraps; Left/Right stop at 0 and 10. */
static int *volume_field(DisplayOptions *options, int row)
{
	return row == 6 ? &options->master_volume : row == 7 ? &options->music_volume
		: row == 8 ? &options->voice_volume : NULL;
}

static void toggle_display_option_ex(int step, int wrap)
{
	DisplayOptions next = display_options;
	char path[200];
	int *volume = volume_field(&next, panel_cursor);
	if (volume != NULL) {
		int value = *volume + step;
		*volume = wrap ? (value + 11) % 11 : value < 0 ? 0 : value > 10 ? 10 : value;
		if (*volume == *volume_field(&display_options, panel_cursor)) return;
	} else if (panel_cursor == 0) {
		if (!current_textbox_available()) {
			snprintf(notice, sizeof(notice), "This game has no custom textbox");
			return;
		}
		next.custom_textbox = !next.custom_textbox;
	} else if (panel_cursor == 1) {
		next.nvl = !next.nvl;
	} else if (panel_cursor == 2) {
		/* Top -> Top, choices below -> Bottom (and back with Left). */
		int mode = next.bottom_text ? 2 : next.touch_choices ? 1 : 0;
		mode = (mode + 3 + step) % 3;
		next.bottom_text = mode == 2;
		next.touch_choices = mode == 1;
	} else if (panel_cursor == 3) {
		next.text_speed = (next.text_speed + 4 + step) % 4;
	} else if (panel_cursor == 4) {
		next.auto_speed = (next.auto_speed + 3 + step) % 3;
	} else if (panel_cursor == 5) {
		next.skip_read = !next.skip_read;
	} else if (panel_cursor == 9) {
		next.tv_wide = !next.tv_wide;
	}
	if ((mkdir(ROOT_DIR, 0777) != 0 && errno != EEXIST)
		|| (mkdir(SETTINGS_DIR, 0777) != 0 && errno != EEXIST)) {
		snprintf(notice, sizeof(notice), "Options not saved: cannot create settings directory");
		return;
	}
	options_path(path, sizeof(path));
	DisplayOptionsResult result = display_options_save(path, &next);
	if (result != DISPLAY_OPTIONS_OK && result != DISPLAY_OPTIONS_SAVED_BACKUP_REMAINS) {
		snprintf(notice, sizeof(notice), "Options not saved: check settings file and storage");
		return;
	}
	display_options = next;
	apply_volumes();
	apply_screen_options();
	snprintf(notice, sizeof(notice), "%s", result == DISPLAY_OPTIONS_SAVED_BACKUP_REMAINS
		? "Options saved; remove old settings backup" : "Options saved");
}

static void toggle_display_option(int step)
{
	toggle_display_option_ex(step, 1);
}

static void make_preview(char *out, size_t size)
{
	char text[GS_TEXT_LEN + GS_NAME_LEN + 8];
	if (event.type == GS_EV_SAY) {
		if (event.speaker[0] != '\0') {
			snprintf(text, sizeof(text), "%s: %s", event.speaker, event.text);
		} else {
			snprintf(text, sizeof(text), "%s", event.text);
		}
	} else if (event.type == GS_EV_MENU && event.choice_count > 0) {
		snprintf(text, sizeof(text), "[Choice] %s", event.choices[0]);
	} else if (event.type == GS_EV_END) {
		snprintf(text, sizeof(text), "The end");
	} else {
		snprintf(text, sizeof(text), "...");
	}
	for (char *c = text; *c != '\0'; ++c) {
		if (*c == '\n') {
			*c = ' ';
		}
	}
	size_t length = strlen(text);
	if (length >= size) {
		length = size - 1;
		while (length > 0 && ((unsigned char)text[length] & 0xC0) == 0x80) {
			--length;
		}
	}
	memcpy(out, text, length);
	out[length] = '\0';
}

static void refresh_slots(void)
{
	for (int slot = 0; slot < SAVE_SLOTS; ++slot) {
		char path[200];
		SaveHeader header;
		unsigned char head[SAVE_HEADER_SIZE];
		save_path(slot, path, sizeof(path));
		memset(&slots[slot], 0, sizeof(slots[slot]));
		FILE *file = fopen(path, "rb");
		if (file != NULL) {
			if (fread(head, sizeof(head), 1, file) == 1 && save_peek(head, sizeof(head), &header)) {
				slots[slot].used = 1;
				slots[slot].saved_at = header.saved_at;
				header.preview[sizeof(header.preview) - 1] = '\0';
				snprintf(slots[slot].preview, sizeof(slots[slot].preview), "%s", header.preview);
			}
			fclose(file);
		}
	}
}

static int save_slot(int slot)
{
	mkdir(ROOT_DIR, 0777);
	mkdir(SAVES_DIR, 0777);
	char path[200], temp_path[210];
	save_path(slot, path, sizeof(path));
	snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

	SaveData *data = calloc(1, sizeof(*data));
	FILE *file = data != NULL ? fopen(temp_path, "wb") : NULL;
	if (file == NULL) {
		free(data);
		snprintf(notice, sizeof(notice), "save failed (cannot write)");
		return 0;
	}
	data->header.magic = SAVE_MAGIC;
	data->header.script_hash = script_hash;
	data->header.saved_at = (long long)time(NULL);
	make_preview(data->header.preview, sizeof(data->header.preview));
	data->state = state;
	data->scene = scene;
	data->event = event;
	data->backlog = backlog;

	/* one format on every console and computer (savefile.c) */
	unsigned char *encoded = malloc(SAVE_FILE_SIZE);
	if (encoded != NULL) save_encode(data, encoded);
	unsigned char loop_data[GS_BGLOOP_SAVE_SIZE];
	int ok = encoded != NULL && gs_bgloop_encode(&bgloop, loop_data)
		&& fwrite(encoded, SAVE_FILE_SIZE, 1, file) == 1
		&& fwrite(loop_data, sizeof(loop_data), 1, file) == 1;
	if (ok && config.num[NUM_NVL_ACCUMULATE]) {
		unsigned char page_data[NVL_PAGE_SAVE_MAX_BYTES];
		size_t page_size = 0;
		ok = nvl_page_encode(&nvl_page, page_data, sizeof(page_data), &page_size) == NVL_PAGE_OK
			&& fwrite(page_data, 1, page_size, file) == page_size;
	}
	ok = fclose(file) == 0 && ok;
	free(data);
	free(encoded);
	if (ok) {
		remove(path);
		ok = rename(temp_path, path) == 0;
	}
	if (!ok) {
		remove(temp_path);
		snprintf(notice, sizeof(notice), "save failed");
		return 0;
	}
	last_slot = slot;
	read_save();
	snprintf(notice, sizeof(notice), "Saved to slot %d", slot + 1);
	return 1;
}

/* "[i]" ... "[/i]" in dialogue: italic markers for renderers that support them (PC, Wii U);
   removed elsewhere (the 3DS system font has no italics). */
static void apply_italics(char *text)
{
	if (strstr(text, "[i]") == NULL && strstr(text, "[/i]") == NULL) return;
	char *out = text;
	for (const char *in = text; *in; ) {
		if (!strncmp(in, "[i]", 3)) {
#ifdef VN_HAS_ITALICS
			*out++ = VN_ITALIC_ON;
#endif
			in += 3;
		} else if (!strncmp(in, "[/i]", 4)) {
#ifdef VN_HAS_ITALICS
			*out++ = VN_ITALIC_OFF;
#endif
			in += 4;
		} else {
			*out++ = *in++;
		}
	}
	*out = '\0';
}

static void show_event(void)
{
	switch (event.type) {
	case GS_EV_SAY:
		waiting = PLAY_TEXT;
		chars_shown = 0.0f;
		last_activity = now_ms();   /* a new line (also in auto mode) counts as playing */
		advance_visible = 0;
		auto_wait = 0.0f;
		line_voiced = audio_sfx_playing();
		text_pc = state.pc;
		break;
	case GS_EV_MENU:
		waiting = PLAY_MENU;
		menu_cursor = 0;
		break;
	case GS_EV_WAIT:
		waiting = PLAY_TIMER;
		timer_left = event.seconds;
		break;
	case GS_EV_WAITBG:
		waiting = scene.bg_prev[0] != '\0' ? PLAY_WAITBG : PLAY_NONE;
		break;
	case GS_EV_END:
		waiting = PLAY_END;
		break;
	default:
		waiting = PLAY_NONE;
		break;
	}
}

/* Rejects anything a damaged or stale save could use to index out of range. */
static int normalize_save(SaveData *data)
{
	GsEvent *e = &data->event;
	if ((e->type > GS_EV_WAIT && e->type != GS_EV_WAITBG) || e->choice_count < 0
		|| e->choice_count > GS_MAX_CHOICES
		|| data->backlog.count < 0 || data->backlog.count > BACKLOG_MAX
		|| data->backlog.start < 0 || data->backlog.start >= BACKLOG_MAX) {
		return 0;
	}
	if (e->type == GS_EV_MENU
		&& (data->state.pending_menu < 0 || e->choice_count != data->state.pending_count)) {
		return 0;
	}
	e->speaker[GS_NAME_LEN - 1] = '\0';
	e->text[GS_TEXT_LEN - 1] = '\0';
	for (int index = 0; index < GS_MAX_CHOICES; ++index) {
		e->choices[index][GS_STR_LEN - 1] = '\0';
	}
	Scene *s = &data->scene;
	s->bg[GS_STR_LEN - 1] = s->bg_prev[GS_STR_LEN - 1] = s->music[GS_STR_LEN - 1] = '\0';
	for (int index = 0; index < MAX_SPRITES; ++index) {
		s->sprites[index].name[GS_NAME_LEN - 1] = '\0';
		s->sprites[index].asset[GS_STR_LEN - 1] = '\0';
		s->sprites[index].pos[GS_NAME_LEN - 1] = '\0';
	}
	for (int index = 0; index < BACKLOG_MAX; ++index) {
		data->backlog.entries[index].speaker[GS_NAME_LEN - 1] = '\0';
		data->backlog.entries[index].text[GS_TEXT_LEN - 1] = '\0';
	}
	return 1;
}

/* A save from another version of the game: its position is an instruction index into the
   old script. Continue right after the same line of dialogue in the current script (the
   line on screen, or the last one before a choice menu), keeping variables and scene.
   Returns NULL on success, else why not. */
static const char *relocate_save(SaveData *data)
{
	if (data->state.call_depth != 0) {
		return "saved inside a subroutine; can't continue it in this version";
	}
	if (data->event.type != GS_EV_SAY) {
		const Backlog *log = &data->backlog;
		if (log->count <= 0 || log->count > BACKLOG_MAX || log->start < 0 || log->start >= BACKLOG_MAX) {
			return "save is from a different script version";
		}
		const BacklogEntry *last = &log->entries[(log->start + log->count - 1) % BACKLOG_MAX];
		GsEvent say;
		memset(&say, 0, sizeof(say));
		say.type = GS_EV_SAY;
		snprintf(say.speaker, sizeof(say.speaker), "%s", last->speaker);
		snprintf(say.text, sizeof(say.text), "%s", last->text);
		say.color = last->color;
		data->event = say;   /* resume at that line; the menu follows again */
	}
	int at = gs_find_say(program, data->event.text, data->state.pc - 1);
	if (at < 0) {
		return "this line is not in the updated game";
	}
	data->state.pc = at + 1;
	data->state.pending_menu = -1;
	data->state.pending_count = 0;
	return NULL;
}

static int load_slot(int slot)
{
	char path[200];
	save_path(slot, path, sizeof(path));
	SaveData *data = malloc(sizeof(*data));
	FILE *file = data != NULL ? fopen(path, "rb") : NULL;
	unsigned char *encoded = data != NULL ? malloc(SAVE_FILE_SIZE) : NULL;
	int read_ok = file != NULL && encoded != NULL && fread(encoded, SAVE_FILE_SIZE, 1, file) == 1
		&& save_decode(encoded, SAVE_FILE_SIZE, data);
	free(encoded);
	GsBgLoop loaded_loop = {0};
	NvlPage loaded_page;
	nvl_page_clear(&loaded_page);
	int has_page = 0;
	if (read_ok) {
		unsigned char loop_data[GS_BGLOOP_SAVE_SIZE];
		size_t count = fread(loop_data, 1, 4, file);
		if (count != 0) {
			size_t loop_size = count == 4 && !memcmp(loop_data, "BGL1", 4) ? GS_BGLOOP_V1_SAVE_SIZE
				: count == 4 && !memcmp(loop_data, "BGL2", 4) ? GS_BGLOOP_SAVE_SIZE : 0;
			if (!loop_size || fread(loop_data + 4, 1, loop_size - 4, file) != loop_size - 4
				|| !gs_bgloop_decode_optional(&loaded_loop, loop_data, loop_size)) {
				read_ok = 0;
			} else {
				unsigned char page_data[NVL_PAGE_SAVE_MAX_BYTES + 1];
				size_t page_size = fread(page_data, 1, sizeof(page_data), file);
				if (page_size) {
					has_page = 1;
					if (nvl_page_decode(page_data, page_size, &loaded_page) != NVL_PAGE_OK)
						read_ok = 0;
				}
			}
		}
		if (ferror(file)) read_ok = 0;
	}
	if (file != NULL) {
		fclose(file);
	}

	const char *problem = NULL;
	char previous_script[sizeof(current_script)];
	int switched = 0;
	snprintf(previous_script, sizeof(previous_script), "%s", current_script);
	if (read_ok && data->header.magic == SAVE_MAGIC
		&& !extras_mode && data->state.var_count >= 0 && data->state.var_count <= GS_MAX_VARS) {
		const char *wanted = gs_variable_text(&data->state, "_script");
		wanted = wanted != NULL && wanted[0] ? wanted : "";
		if (strcmp(wanted, current_script) != 0 && strlen(wanted) < sizeof(current_script)) {
			switched = switch_script(wanted);
			if (!switched) {
				problem = "chapter script is missing";
			}
		}
	}
	if (problem != NULL) {
	} else if (!read_ok) {
		problem = file == NULL ? "slot is empty" : "cannot read save";
	} else if (data->header.magic != SAVE_MAGIC) {
		problem = "unsupported save file";
	} else if (data->header.script_hash != script_hash && (problem = relocate_save(data)) != NULL) {
		/* problem set */
	} else if (data->header.script_hash != script_hash && (nvl_page_clear(&loaded_page), has_page = 0)) {
		/* relocated: the saved NVL page belongs to the old position, start a fresh one */
	} else if (!gs_state_valid(program, &data->state) || !normalize_save(data)) {
		problem = "save file is damaged";
	}
	if (!problem && config.num[NUM_NVL_ACCUMULATE] && data->event.type == GS_EV_SAY) {
		char prefix[GS_NAME_LEN + 3] = "";
		if (data->event.speaker[0]) snprintf(prefix, sizeof(prefix), "%s: ", data->event.speaker);
		size_t prefix_len = strlen(prefix), text_len = strlen(data->event.text);
		if (!has_page) {
			if (nvl_page_append_dialogue(&loaded_page, prefix, prefix_len, data->event.text, text_len) != NVL_PAGE_OK)
				problem = "cannot restore NVL text";
		} else if (loaded_page.length - loaded_page.current_event_offset != prefix_len + text_len
			|| memcmp(loaded_page.text + loaded_page.current_event_offset, prefix, prefix_len)
			|| memcmp(loaded_page.text + loaded_page.current_event_offset + prefix_len, data->event.text, text_len)) {
			problem = "save file has invalid NVL text";
		}
	}
	if (problem != NULL) {
		if (switched) {
			switch_script(previous_script);
		}
		snprintf(notice, sizeof(notice), "%s", problem);
		free(data);
		return 0;
	}

	if (loaded_loop.active) {
		if (!prepare_bgloop(data->scene.bg, &loaded_loop)) {
			free(data);
			return 0;
		}
	} else {
		clear_bgloop();
	}
	state = data->state;
	say_held = 0;
	globals_apply();
	scene = data->scene;
	event = data->event;
	backlog = data->backlog;
	nvl_page = loaded_page;
	memset(log_layout, 0, sizeof(log_layout));
	memset(log_voice, 0, sizeof(log_voice));
	free(data);

	audio_stop_music(0.0f);
	audio_stop_sfx();
	if (scene.music[0] != '\0') {
		GsEvent resume = {0};
		snprintf(resume.asset, sizeof(resume.asset), "%s", scene.music);
		play_audio_event(&resume, 1);
	}
	show_event();
	ui_hidden = 0;
	last_slot = slot;
	snprintf(notice, sizeof(notice), "Loaded slot %d", slot + 1);
	return 1;
}

static void fail_with(const char *text)
{
	snprintf(message, sizeof(message), "%s", text);
	mode = MODE_ERROR;
}

static void set_game_font(const char *name)
{
	if (game_font != NULL) {
		font = system_font;
		C2D_FontFree(game_font);
		game_font = NULL;
	}
	if (name[0] != '\0') {
		char path[400];
		snprintf(path, sizeof(path), "%s/ui/%s.bcfnt", game_dir, name);
		game_font = C2D_FontLoad(path);
		if (game_font == NULL) {
			snprintf(notice, sizeof(notice), "font %.40s not found or invalid", name);
		}
	}
	font = game_font != NULL ? game_font : system_font;
}

/* Optional files: game.3vc (menu appearance) and glossary.3vg (terms). */
static void load_game_extras(void)
{
	char path[320];
	size_t size;
	config_defaults(&config);
	glossary.count = 0;
	gloss_layout_for = -1;
	snprintf(path, sizeof(path), "%s/game.3vc", game_dir);
	unsigned char *text = read_file(path, &size);
	if (text != NULL) {
		config_parse((const char *)text, &config);
		free(text);
	}
	set_game_font(config.font);
	custom_textbox_available = config.images[IMG_TEXTBOX][0] != '\0'
		&& get_picture("ui", config.images[IMG_TEXTBOX]) != NULL;
	custom_nvl_available = config.images[IMG_NVL_TEXTBOX][0] != '\0'
		&& get_picture("ui", config.images[IMG_NVL_TEXTBOX]) != NULL;
	if (config.images[IMG_TEXTBOX][0] != '\0' && !custom_textbox_available) {
		size_t used = strlen(notice);
		snprintf(notice + used, sizeof(notice) - used, "%scustom textbox missing or invalid",
			used ? "; " : "");
	}
	if (config.images[IMG_NVL_TEXTBOX][0] != '\0' && !custom_nvl_available) {
		size_t used = strlen(notice);
		snprintf(notice + used, sizeof(notice) - used, "%sNVL textbox missing or invalid",
			used ? "; " : "");
	}
	advance_available = 0;
	if (config.images[IMG_ADVANCE][0] != '\0') {
		Picture *picture = get_picture("ui", config.images[IMG_ADVANCE]);
		advance_available = picture != NULL && picture->width % config.num[NUM_ADVANCE_FRAMES] == 0;
		if (!advance_available) {
			size_t used = strlen(notice);
			snprintf(notice + used, sizeof(notice) - used, "%sadvance image missing or invalid",
				used ? "; " : "");
		}
	}
	snprintf(path, sizeof(path), "%s/glossary.3vg", game_dir);
	text = read_file(path, &size);
	if (text != NULL) {
		int line = glossary_parse_checked((const char *)text, &glossary);
		if (line) {
			size_t used = strlen(notice);
			snprintf(notice + used, sizeof(notice) - used, "%sglossary: invalid unlock at line %d",
				used ? "; " : "", line);
		}
		free(text);
	}
	snprintf(path, sizeof(path), "%s/" EXTRAS_FILE, game_dir);
	struct stat extras_stat;
	extras_available = stat(path, &extras_stat) == 0 && S_ISREG(extras_stat.st_mode)
		&& extras_stat.st_size > 0;
}

static void hash_bytes(const unsigned char *data, size_t size)
{
	for (size_t index = 0; index < size; ++index) {
		script_hash = (script_hash ^ data[index]) * 16777619u;
	}
}

/* Reads an included script from the game folder; its text also counts towards the save fingerprint. */
static char *load_include(const char *name, void *context)
{
	(void)context;
	if (strstr(name, "..") != NULL || name[0] == '/' || strchr(name, ':') != NULL) {
		return NULL;
	}
	char path[320];
	size_t size;
	snprintf(path, sizeof(path), "%s/%s", game_dir, name);
	unsigned char *data = read_file(path, &size);
	if (data != NULL) {
		hash_bytes(data, size);
	}
	return (char *)data;
}

/* Compiles a chapter script for the `script` command and makes it the running program. */
static int switch_script(const char *name)
{
	TEST_LOG("SCRIPT %s", name);
	read_save();
	if (strstr(name, "..") != NULL || name[0] == '/' || strchr(name, ':') != NULL) {
		snprintf(notice, sizeof(notice), "Bad script name");
		return 0;
	}
	char path[320], error[GS_TEXT_LEN];
	size_t size;
	if (name[0]) {
		snprintf(path, sizeof(path), "%s/%s.3vs", game_dir, name);
	} else {
		snprintf(path, sizeof(path), "%s/" SCRIPT_FILE, game_dir);
	}
	unsigned char *source = read_file(path, &size);
	if (source == NULL) {
		snprintf(notice, sizeof(notice), "Could not read %.40s", path + strlen(game_dir) + 1);
		return 0;
	}
	/* The save fingerprint covers main.3vs and its includes as loaded at start. Includes of
	   chapter scripts must not change it, or saves made in a chapter fail to load
	   ("different script version") once the game is restarted. */
	unsigned fingerprint = script_hash;
	GsProgram *next = gs_compile_ex((const char *)source, load_include, NULL, error, sizeof(error));
	script_hash = fingerprint;
	free(source);
	if (next == NULL) {
		snprintf(notice, sizeof(notice), "%.40s: %.60s", name, error);
		return 0;
	}
	gs_free(program);
	program = next;
	snprintf(current_script, sizeof(current_script), "%s", name);
	return 1;
}

static void start_game(const char *name)
{
	extras_mode = 0;
	current_script[0] = '\0';
	gs_free(program);
	program = NULL;
	clear_bgloop();
	clear_cache();
	clear_bgloop();
	audio_stop_music(0.0f);
	audio_stop_sfx();
	memset(&scene, 0, sizeof(scene));
	nvl_page_clear(&nvl_page);
	notice[0] = '\0';
	snprintf(game_name, sizeof(game_name), "%s", name);
	snprintf(game_dir, sizeof(game_dir), GAMES_DIR "/%s", name);
	{
		/* games/<name>.3vp: the game's archive. A folder games/<name>/ next to it holds
		   override files (patches, mods) that replace the archived ones. */
		char packed[400];
		struct stat info;
		snprintf(packed, sizeof(packed), "%s" PACK_SUFFIX, game_dir);
		pack_close();
		pack_open(packed, game_dir, stat(game_dir, &info) == 0 && S_ISDIR(info.st_mode));
	}

	char path[320];
	size_t size;
	snprintf(path, sizeof(path), "%s/" SCRIPT_FILE, game_dir);
	unsigned char *source = read_file(path, &size);
	if (source == NULL) {
		fail_with("Could not read " SCRIPT_FILE);
		return;
	}
	script_hash = 2166136261u;
	hash_bytes(source, size);
	char error[GS_TEXT_LEN];
	program = gs_compile_ex((const char *)source, load_include, NULL, error, sizeof(error));
	free(source);
	if (program == NULL) {
		fail_with(error);
		return;
	}
	load_game_extras();
	load_display_options();
	read_load();
	globals_load();
	playtime_load();
	last_activity = now_ms();
	endings_load();
	guide_find();
	gs_state_init(program, &state);
	memset(&backlog, 0, sizeof(backlog));
	memset(log_layout, 0, sizeof(log_layout));
	memset(log_voice, 0, sizeof(log_voice));
	memset(&event, 0, sizeof(event));
	panel = PANEL_NONE;
	panel_return = PANEL_NONE;
	ui_hidden = 0;
	last_slot = 0;
	waiting = PLAY_NONE;
	title_count = 0;
	title_items[title_count++] = TITLE_START;
	title_items[title_count++] = TITLE_LOAD;
	title_items[title_count++] = TITLE_OPTIONS;
	if (extras_available) {
		title_items[title_count++] = TITLE_EXTRAS;
	}
	if (ending_count > 0) title_items[title_count++] = TITLE_ENDINGS;
	title_items[title_count++] = TITLE_SYNC;
	title_cursor = 0;
	refresh_slots();
	mode = MODE_TITLE;
}

static void begin_game(void)
{
	say_held = 0;
	gs_state_init(program, &state);
	globals_apply();
	memset(&backlog, 0, sizeof(backlog));
	memset(log_layout, 0, sizeof(log_layout));
	memset(log_voice, 0, sizeof(log_voice));
	memset(&event, 0, sizeof(event));
	panel = PANEL_NONE;
	panel_return = PANEL_NONE;
	ui_hidden = 0;
	waiting = PLAY_NONE;
	mode = MODE_PLAY;
}

static void start_extras(void)
{
	char path[320], error[GS_TEXT_LEN];
	size_t size;
	snprintf(path, sizeof(path), "%s/" EXTRAS_FILE, game_dir);
	unsigned char *source = read_file(path, &size);
	if (source == NULL) {
		snprintf(notice, sizeof(notice), "Could not read " EXTRAS_FILE);
		return;
	}
	unsigned main_hash = script_hash;
	script_hash = 2166136261u;
	hash_bytes(source, size);
	GsProgram *extras_program = gs_compile_ex((const char *)source, load_include, NULL,
		error, sizeof(error));
	free(source);
	if (extras_program == NULL) {
		script_hash = main_hash;
		snprintf(notice, sizeof(notice), "Extras script: %.70s", error);
		return;
	}
	gs_free(program);
	program = extras_program;
	extras_mode = 1;
	memset(&scene, 0, sizeof(scene));
	clear_bgloop();
	audio_stop_music(0.0f);
	audio_stop_sfx();
	nvl_page_clear(&nvl_page);
	begin_game();
}

static void return_to_title(void)
{
	char name[sizeof(game_name)];
	snprintf(name, sizeof(name), "%s", game_name);
	stop_video();
	start_game(name);
}

/* ---------- video ---------- */

static int video_decode(unsigned frame)
{
	unsigned length = video.offsets[frame + 1] - video.offsets[frame];
	if (fseek(video.file, (long)video.offsets[frame], SEEK_SET) != 0
		|| fread(video.buffer, 1, length, video.file) != length) {
		return 0;
	}
	int width, height, channels;
	unsigned char *pixels = stbi_load_from_memory(video.buffer, (int)length, &width, &height, &channels, 4);
	if (pixels == NULL) {
		return 0;
	}
	int ok = (unsigned)width == video.header.width && (unsigned)height == video.header.height;
	if (ok) {
		fill_picture(video.picture, pixels);
	}
	stbi_image_free(pixels);
	return ok;
}

static void stop_video(void)
{
	if (video.file != NULL) {
		fclose(video.file);
	}
	free(video.offsets);
	free(video.buffer);
	free_picture(video.picture);
	if (video.has_audio) {
		audio_stop_music(0.0f);
		if (scene.music[0] != '\0') {
			GsEvent resume = {0};
			snprintf(resume.asset, sizeof(resume.asset), "%s", scene.music);
			play_audio_event(&resume, 1);
		}
	}
	memset(&video, 0, sizeof(video));
	if (waiting == PLAY_VIDEO) {
		waiting = PLAY_NONE;
	}
}

/* Plays video/NAME.3vv (frames) with video/NAME.wav (sound, optional). On any problem it
   shows a notice and the script carries on. */
static void start_video(const GsEvent *e)
{
	char path[400], problem[64] = "";
	unsigned char head[VID_HEADER_SIZE];
	unsigned char *raw = NULL;
	memset(&video, 0, sizeof(video));
	snprintf(path, sizeof(path), "%s/video/%s.3vv", game_dir, e->asset);
	video.skippable = e->seconds == 0.0f;
	video.file = vn_fopen(path, "rb");

	if (video.file == NULL) {
		snprintf(problem, sizeof(problem), "file not found");
	} else if (fread(head, 1, sizeof(head), video.file) != sizeof(head)
		|| !vid_parse_header(head, sizeof(head), &video.header)) {
		snprintf(problem, sizeof(problem), "not a valid .3vv file");
	} else {
		size_t index_bytes = ((size_t)video.header.frames + 1) * 4;
		fseek(video.file, 0, SEEK_END);
		long file_size = ftell(video.file);
		fseek(video.file, VID_HEADER_SIZE, SEEK_SET);
		raw = malloc(index_bytes);
		video.offsets = malloc(index_bytes);
		video.buffer = malloc(VID_MAX_FRAME_BYTES);
		if (raw == NULL || video.offsets == NULL || video.buffer == NULL) {
			snprintf(problem, sizeof(problem), "out of memory");
		} else if (fread(raw, 1, index_bytes, video.file) != index_bytes
			|| file_size < 0
			|| !vid_read_index(raw, index_bytes, &video.header, (size_t)file_size, video.offsets)) {
			snprintf(problem, sizeof(problem), "damaged .3vv file");
		} else {
			video.picture = create_picture((int)video.header.width, (int)video.header.height);
			if (video.picture == NULL || !video_decode(0)) {
				snprintf(problem, sizeof(problem), "cannot decode frames");
			}
		}
	}
	free(raw);
	if (problem[0] != '\0') {
		stop_video();
		snprintf(notice, sizeof(notice), "video %.30s: %s", e->asset, problem);
		return;
	}

	char audio_path[400], err[64];
	snprintf(audio_path, sizeof(audio_path), "%s/video/%s.wav", game_dir, e->asset);
	video.has_audio = audio_play_music(audio_path, 0.0f, err, sizeof(err));
	video.elapsed = 0.0f;
	video.shown = 0;
	ui_hidden = 0;
	waiting = PLAY_VIDEO;
}

/* Shows whichever frame is due now; slow decoding just skips frames, so sound stays in step. */
static void update_video(float dt, u32 down)
{
	video.elapsed += dt;
	if (vid_finished(&video.header, video.elapsed)
		|| (video.skippable && (down & (KEY_A | KEY_TOUCH)))) {
		stop_video();
		return;
	}
	unsigned frame = vid_frame_at(&video.header, video.elapsed);
	if (frame != video.shown) {
		video_decode(frame);
		video.shown = frame;
	}
}

static int gallery_name_compare(const void *left, const void *right)
{
	return strcmp((const char *)left, (const char *)right);
}

static void gallery_found(const char *name, void *ctx)
{
	(void)ctx;
	const char *extension = strrchr(name, '.');
	if (gallery_count >= MAX_GALLERY_ITEMS || extension == NULL
		|| (strcmp(extension, ".png") && strcmp(extension, ".jpg"))) {
		return;
	}
	size_t length = (size_t)(extension - name);
	if (length == 0 || length >= GS_STR_LEN) {
		return;
	}
	for (int i = 0; i < gallery_count; ++i) {
		if (strlen(gallery_items[i]) == length && strncmp(gallery_items[i], name, length) == 0) {
			return;
		}
	}
	memcpy(gallery_items[gallery_count], name, length);
	gallery_items[gallery_count][length] = '\0';
	++gallery_count;
}

static int start_gallery(void)
{
	char directory[320];
	snprintf(directory, sizeof(directory), "%s/gallery/thumbs", game_dir);
	gallery_count = 0;
	if (vn_list(directory, gallery_found, NULL) < 0) {
		snprintf(message, sizeof(message), "Gallery: cannot open gallery/thumbs");
		mode = MODE_ERROR;
		return 0;
	}
	if (gallery_count == 0) {
		snprintf(message, sizeof(message), "Gallery: no thumbnails in gallery/thumbs");
		mode = MODE_ERROR;
		return 0;
	}
	qsort(gallery_items, (size_t)gallery_count, sizeof(gallery_items[0]), gallery_name_compare);
	gallery_cursor = 0;
	gallery_detail = 0;
	return 1;
}

static void start_effect(const GsEvent *e)
{
	if (!strcmp(e->asset, "camera")) {
		int small = !strcmp(e->text, "small");
		float zoom = small ? 1.05f : 1.12f, reach = small ? 10.0f : 24.0f;
		float target[3] = {0.0f, 0.0f, 1.0f};
		if (e->pos[0] == 'l') { target[0] = reach; target[2] = zoom; }
		else if (e->pos[0] == 'r') { target[0] = -reach; target[2] = zoom; }
		else if (e->pos[0] == 'd') { target[1] = -reach / 2.0f; target[2] = zoom; }
		cam_from[0] = cam_pan_x; cam_from[1] = cam_pan_y; cam_from[2] = cam_zoom;
		memcpy(cam_to, target, sizeof(target));
		cam_dur = e->seconds;
		cam_t = cam_dur > 0.0f ? 0.0f : 1.0f;
		if (cam_t >= 1.0f) { cam_pan_x = target[0]; cam_pan_y = target[1]; cam_zoom = target[2]; }
	} else if (!strcmp(e->asset, "hop")) {
		for (int i = 0; i < MAX_SPRITES && i < 16; ++i) {
			if (scene.sprites[i].active && !strcmp(scene.sprites[i].name, e->pos)) hop_t[i] = e->seconds;
		}
	} else if (!strcmp(e->asset, "shake")) {
		shake_total = shake_left = e->seconds;
		shake_amp = strtof(e->pos, NULL);
	} else {
		flash_total = flash_left = e->seconds;
		flash_shade = !strcmp(e->pos, "black") ? 0.0f : 255.0f;
	}
}

/* Runs the script until something needs the player's attention. */
static void advance(void)
{
	for (;;) {
		gs_next(program, &state, &event);
		if (!extras_mode) globals_sync();
		if (event.type == GS_EV_SAY) {
			apply_italics(event.text);
			held_say = event;
			say_held = 1;
		} else if (event.type != GS_EV_SFX && event.type != GS_EV_MUSIC && event.type != GS_EV_WAIT) {
			say_held = 0;
		}
		switch (event.type) {
		case GS_EV_PAGE:
			nvl_page_clear(&nvl_page);
			continue;
		case GS_EV_SCRIPT:
			if (!switch_script(event.asset)) {
				fail_with(notice);
				return;
			}
			continue;
		case GS_EV_GALLERY:
			if (start_gallery()) {
				waiting = PLAY_GALLERY;
				ui_hidden = 0;
			}
			return;
		case GS_EV_BG:
		case GS_EV_BGLOOP:
		case GS_EV_SHOW:
		case GS_EV_HIDE:
			apply_event_to_scene(&event);
			if (mode == MODE_ERROR) return;
			continue;
		case GS_EV_EFFECT:
			start_effect(&event);
			continue;
		case GS_EV_MUSIC:
		case GS_EV_SFX:
			play_audio_event(&event, event.type == GS_EV_MUSIC);
			continue;
		case GS_EV_VIDEO:
			start_video(&event);
			if (waiting == PLAY_VIDEO) {
				return;
			}
			continue;
		case GS_EV_SAY:
			if (config.num[NUM_NVL_ACCUMULATE]) {
				char prefix[GS_NAME_LEN + 3] = "";
				if (event.speaker[0]) snprintf(prefix, sizeof(prefix), "%s: ", event.speaker);
				NvlPageResult result = nvl_page_append_dialogue(&nvl_page, prefix, strlen(prefix),
					event.text, strlen(event.text));
				if (result == NVL_PAGE_TRIMMED) {
					snprintf(notice, sizeof(notice), "NVL page limit reached; older text removed");
				} else if (result != NVL_PAGE_OK) {
					fail_with("Could not retain NVL page text");
					return;
				}
			}
			backlog_push(event.speaker, event.color, event.text);
			show_event();
			ui_hidden = 0;
			return;
		case GS_EV_MENU:
			show_event();
			ui_hidden = 0;
			return;
		case GS_EV_WAIT:
			timer_left = event.seconds;
			waiting = PLAY_TIMER;
			return;
		case GS_EV_WAITBG:
			if (scene.bg_prev[0] != '\0') {
				waiting = PLAY_WAITBG;
				return;
			}
			continue;
		case GS_EV_END:
			if (extras_mode) {
				return_to_title();
				return;
			}
			record_story_end();
			waiting = PLAY_END;
			return;
		case GS_EV_ERROR: {
			char text[GS_TEXT_LEN + 32];
			snprintf(text, sizeof(text), "line %d: %s", event.line, event.text);
			fail_with(text);
			return;
		}
		}
	}
}

/* ---------- drawing ---------- */

static const u32 COLOR_ACCENT = 0xFF5BACE0;  /* ABGR of (224,172,91) */

static u32 rgb_color(unsigned rgb, u8 alpha)
{
	return C2D_Color32((u8)(rgb >> 16), (u8)(rgb >> 8), (u8)rgb, alpha);
}

static u32 theme_accent(void)
{
	return config.has_accent ? rgb_color(config.accent, 255) : COLOR_ACCENT;
}

static u32 theme_panel(u8 alpha)
{
	return config.has_panel ? rgb_color(config.panel, alpha) : C2D_Color32(24, 40, 46, alpha);
}

/* Buttons and list rows on the bottom screen. */
static u32 theme_item(void)
{
	return config.has_panel ? rgb_color(config.panel, 255) : C2D_Color32(40, 62, 66, 255);
}

static u32 theme_text(void)
{
	return config.has_text ? rgb_color(config.text, 255) : C2D_Color32(225, 232, 228, 255);
}

static u32 theme_dark_text(void)
{
	return C2D_Color32(10, 16, 18, 255);
}

static void rect(float x, float y, float w, float h, u32 color, float z)
{
	C2D_DrawRectangle(x, y, z, w, h, color, color, color, color);
}

static void cam_apply(float *x, float *y, float width, float height, float *scale)
{
	*scale = cam_zoom;
	*x = 200.0f + (*x - 200.0f) * cam_zoom + cam_pan_x;
	*y = 120.0f + (*y - 120.0f) * cam_zoom + cam_pan_y;
	(void)width; (void)height;
}

static void draw_picture(const Picture *picture, float x, float y, float alpha)
{
	float scale;
	y += cur_hop;
	cam_apply(&x, &y, (float)picture->width, (float)picture->height, &scale);
	C2D_ImageTint camtint;
	C2D_AlphaImageTint(&camtint, alpha);
	C2D_DrawImageAt(picture->image, x + fx_x, y + fx_y, Z_SPRITE, &camtint, scale, scale);
}

static void draw_bg_layer(const char *name, float alpha, float z)
{
	if (name[0] == '\0') {
		return;
	}
	Picture *picture = get_picture("bg", name);
	if (picture != NULL) {
		float x = (TOP_W - (float)picture->width) / 2.0f;
		float y = (TOP_H - (float)picture->height) / 2.0f;
		C2D_ImageTint tint;
		C2D_AlphaImageTint(&tint, alpha);
		float scale;
		cam_apply(&x, &y, (float)picture->width, (float)picture->height, &scale);
		C2D_DrawImageAt(picture->image, x + fx_x, y + fx_y, z, &tint, scale, scale);
		return;
	}
	u32 shade = strcmp(name, "white") == 0 ? 255 : strcmp(name, "black") == 0 ? 0 : 70;
	rect(0, 0, TOP_W, TOP_H, C2D_Color32((u8)shade, (u8)shade, (u8)shade, (u8)(alpha * 255)), z);
}

static Picture *ui_image(int which)
{
	return config.images[which][0] != '\0' ? get_picture("ui", config.images[which]) : NULL;
}

/* The game's image stretched over the rectangle, or the flat color if it has none. */
static void draw_panel(int image, float x, float y, float w, float h, u32 color, float z)
{
	Picture *picture = ui_image(image);
	if (picture != NULL) {
		C2D_DrawImageAt(picture->image, x, y, z, NULL, w / (float)picture->width, h / (float)picture->height);
	} else {
		rect(x, y, w, h, color, z);
	}
}

/* The first `visible` bytes of `full`, with a line break wherever the whole text wraps, so a
   word being typed out does not start at the end of one line and jump to the next once it no
   longer fits. Lines are measured with 2 px to spare so the renderer keeps these breaks. */
#define REWRAP_SLACK 1.2f

static void reveal_with_breaks(const char *full, size_t visible, float width, float scale,
	char *out, size_t size)
{
	/* The layout of a line does not change while it is typed out: measure it once
	   (measuring is slow on Wii U, and this runs every frame). */
	static unsigned short starts[64], ends[64];
	static char cached_text[GS_TEXT_LEN];
	static float cached_width = -1.0f, cached_scale;
	static int count;
	if (cached_width != width || cached_scale != scale || strncmp(cached_text, full, sizeof(cached_text))) {
		float previous_scale = measure_scale;
		measure_scale = scale;
		count = wrap_text_lines(full, width - 2.0f, measure_text, NULL, starts, ends, 64);
		measure_scale = previous_scale;
		snprintf(cached_text, sizeof(cached_text), "%s", full);
		cached_width = width;
		cached_scale = scale;
	}
	size_t used = 0;
	if (count <= 0 || count >= 64) {   /* fall back to the plain prefix */
		used = visible < size - 1 ? visible : size - 1;
		memcpy(out, full, used);
		out[used] = '\0';
		return;
	}
	for (int i = 0; i < count && starts[i] < visible; ++i) {
		if (i > 0 && used + 1 < size) out[used++] = '\n';
		size_t end = ends[i] < visible ? ends[i] : visible;
		size_t n = end > starts[i] ? end - starts[i] : 0;
		if (used + n >= size) n = size - 1 - used;
		memcpy(out + used, full + starts[i], n);
		used += n;
	}
	out[used] = '\0';
}

static void draw_wrapped(const char *text, float x, float y, float width, float scale, u32 color, float z)
{
	if (text[0] == '\0') {
		return;
	}
	C2D_Text parsed;
	C2D_TextFontParse(&parsed, font, text_buf, text);
	C2D_TextOptimize(&parsed);
	C2D_DrawText(&parsed, C2D_WithColor | C2D_WordWrap, x, y, z, scale, scale, color, width);
}

/* Shrinks the text if needed so it fits on one line. */
static void draw_line(const char *text, float x, float y, float scale, u32 color, float z, float max_width)
{
	if (text[0] == '\0') {
		return;
	}
	C2D_Text parsed;
	float width, height;
	C2D_TextFontParse(&parsed, font, text_buf, text);
	C2D_TextOptimize(&parsed);
	C2D_TextGetDimensions(&parsed, scale, scale, &width, &height);
	if (max_width > 0.0f && width > max_width) {
		scale *= max_width / width;
	}
	C2D_DrawText(&parsed, C2D_WithColor, x, y, z, scale, scale, color);
}

/* Largest scale (at most `scale`, at least 60% of it) at which the whole text wraps
   into `height` pixels, so long lines never run off the bottom of the text box. */
static float fit_text_scale(const char *text, float width, float height, float scale)
{
	static unsigned short starts[GS_TEXT_LEN], ends[GS_TEXT_LEN];
	static char cached_text[GS_TEXT_LEN];
	static float cached_width, cached_height, cached_scale, cached_result;
	if (cached_width == width && cached_height == height && cached_scale == scale
		&& !strncmp(cached_text, text, sizeof(cached_text))) {
		return cached_result;
	}
	float previous_scale = measure_scale;
	float fitted = scale;
	int fits = 0;
	for (int step = 0; step < 10; ++step) {
		measure_scale = fitted;
		int count = wrap_text_lines(text, width, measure_text, NULL, starts, ends, GS_TEXT_LEN);
		C2D_Text metric;
		float line_height;
		C2D_TextBufClear(measure_buf);
		C2D_TextFontParse(&metric, font, measure_buf, "Ag");
		C2D_TextGetDimensions(&metric, fitted, fitted, NULL, &line_height);
		fits = count * line_height <= height;
		if (fits || fitted <= scale * 0.6f) {
			break;
		}
		fitted = fmaxf(scale * 0.6f, fitted * 0.94f);
	}
	measure_scale = previous_scale;
	if (fitted < scale || !fits) {
		TEST_LOG("%s %.0f%% %.70s", fits ? "FIT" : "OVERFLOW", fitted / scale * 100.0f, text);
	}
	snprintf(cached_text, sizeof(cached_text), "%s", text);
	cached_width = width;
	cached_height = height;
	cached_scale = scale;
	cached_result = fitted;
	return fitted;
}

static void draw_dialogue_with_advance(const char *text, float x, float y, float width,
	float scale, u32 color, Picture *picture, int complete)
{
	unsigned short starts[GS_TEXT_LEN], ends[GS_TEXT_LEN];
	float previous_scale = measure_scale;
	measure_scale = scale;
	int count = wrap_text_lines(text, width, measure_text, NULL, starts, ends, GS_TEXT_LEN);
	C2D_Text metric;
	float line_height;
	C2D_TextBufClear(measure_buf);
	C2D_TextFontParse(&metric, font, measure_buf, "Ag");
	C2D_TextGetDimensions(&metric, scale, scale, NULL, &line_height);
	for (int index = 0; index < count; ++index) {
		char line[GS_TEXT_LEN];
		size_t length = ends[index] - starts[index];
		memcpy(line, text + starts[index], length);
		line[length] = '\0';
		draw_line(line, x, y + index * line_height, scale, color, Z_TEXT, 0.0f);
	}
	if (complete && count > 0) {
		int frames = config.num[NUM_ADVANCE_FRAMES];
		int frame_width = picture->width / frames;
		float image_scale = (float)config.num[NUM_ADVANCE_SCALE] / 100.0f;
		float icon_width = frame_width * image_scale;
		float icon_x;
		int icon_line;
		wrap_indicator_position(text, width, icon_width, 2.0f, measure_text, NULL,
			starts, ends, count, &icon_x, &icon_line);
		float px = x + icon_x;
		float py = y + icon_line * line_height;
		if (!advance_visible) {
			advance_visible = 1;
			advance_started = now_ms();
		}
		unsigned frame = ((now_ms() - advance_started) / (u32)config.num[NUM_ADVANCE_FRAME_MS]) % (unsigned)frames;
		Tex3DS_SubTexture sub = picture->sub;
		float units = picture->sub.right / picture->width;
		sub.width = (u16)frame_width;
		sub.left = frame * frame_width * units;
		sub.right = (frame + 1) * frame_width * units;
		C2D_Image image = { picture->image.tex, &sub };
		C2D_DrawImageAt(image, px, py, Z_TEXT + 0.01f, NULL, image_scale, image_scale);
	}
	measure_scale = previous_scale;
}

static size_t utf8_prefix(const char *text, size_t chars);

#ifdef VN_SDL
/* Draws dialogue whose lines are already broken ('\n'), line by line, revealing `reveal`
   characters: whole lines as they are, the line being typed clipped at a width that grows
   smoothly through each character (the renderer fades its edge). Each line's text stays
   the same while it types, so it is rendered once instead of every frame. */
static void draw_text_reveal(const char *text, float x, float y, float scale, u32 color, float reveal)
{
	float previous_scale = measure_scale;
	measure_scale = scale;
	C2D_Text metric;
	float line_height;
	C2D_TextBufClear(measure_buf);
	C2D_TextFontParse(&metric, font, measure_buf, "Ag");
	C2D_TextGetDimensions(&metric, scale, scale, NULL, &line_height);
	int row = 0;
	for (const char *start = text; *start != '\0' && reveal > 0.0f; ++row) {
		const char *end = strchr(start, '\n');
		size_t length = end != NULL ? (size_t)(end - start) : strlen(start);
		if (length >= GS_TEXT_LEN) length = GS_TEXT_LEN - 1;
		size_t chars = 0;
		for (size_t b = 0; b < length; ++b) chars += ((unsigned char)start[b] & 0xC0) != 0x80;
		char line[GS_TEXT_LEN];
		memcpy(line, start, length);
		line[length] = '\0';
		if (reveal < (float)chars) {
			size_t whole = (size_t)reveal;
			size_t at = utf8_prefix(line, whole), next = utf8_prefix(line, whole + 1);
			float left = measure_text(line, at, NULL), right = measure_text(line, next, NULL);
			float clip = left + (right - left) * (reveal - (float)whole);
			if (clip > 0.0f) {
				vn_clip_next_text(clip);
				draw_line(line, x, y + (float)row * line_height, scale, color, Z_TEXT, 0.0f);
			}
			break;
		}
		draw_line(line, x, y + (float)row * line_height, scale, color, Z_TEXT, 0.0f);
		reveal -= (float)chars + 1.0f;   /* the line break stands for the space it replaced */
		if (end == NULL) break;
		start = end + 1;
	}
	measure_scale = previous_scale;
}
#endif

static size_t utf8_prefix(const char *text, size_t chars);

static void draw_nvl_page(float x, float y, float width, float height, float scale, u32 color)
{
	char shown[NVL_PAGE_MAX_BYTES + 1];
	size_t length = nvl_page.length;
	int complete = 0;
	if (waiting == PLAY_TEXT) {
		size_t prefix = event.speaker[0] ? strlen(event.speaker) + 2 : 0;
		size_t visible = utf8_prefix(event.text, (size_t)chars_shown);
		size_t end = nvl_page.current_event_offset + prefix + visible;
		if (end <= length) length = end;
		complete = event.text[visible] == '\0';
	}
	/* Wrap the whole page (with the current line complete) and show only the revealed part,
	   so a word being typed out does not start on one line and jump to the next. */
	memcpy(shown, nvl_page.text, nvl_page.length);
	shown[nvl_page.length] = '\0';
	unsigned short starts[NVL_PAGE_MAX_BYTES + 1], ends[NVL_PAGE_MAX_BYTES + 1];
	float previous_scale = measure_scale;
	measure_scale = scale;
	int count = wrap_text_lines(shown, width, measure_text, NULL, starts, ends, NVL_PAGE_MAX_BYTES + 1);
	while (count > 0 && starts[count - 1] >= length && length > 0) --count;
	for (int i = 0; i < count; ++i) {
		if (ends[i] > length) ends[i] = (unsigned short)length;
	}
	shown[length] = '\0';
	C2D_Text metric;
	float line_height;
	C2D_TextBufClear(measure_buf);
	C2D_TextFontParse(&metric, font, measure_buf, "Ag");
	C2D_TextGetDimensions(&metric, scale, scale, NULL, &line_height);
	if (line_height <= 0.0f) {
		measure_scale = previous_scale;
		return;
	}
	Picture *picture = complete && advance_available ? ui_image(IMG_ADVANCE) : NULL;
	float icon_x = 0.0f;
	int icon_line = count > 0 ? count - 1 : 0;
	int frame_width = picture ? picture->width / config.num[NUM_ADVANCE_FRAMES] : 0;
	float image_scale = (float)config.num[NUM_ADVANCE_SCALE] / 100.0f;
	if (picture && count) {
		wrap_indicator_position(shown, width, frame_width * image_scale, 2.0f,
			measure_text, NULL, starts, ends, count, &icon_x, &icon_line);
	}
	int visible_lines = (int)(height / line_height);
	if (visible_lines < 1) visible_lines = 1;
	int last = picture && count && icon_line >= count ? icon_line + 1 : count;
	int first = last > visible_lines ? last - visible_lines : 0;
	for (int i = first; i < count; ++i) {
		char line[NVL_PAGE_MAX_BYTES + 1];
		size_t line_length = ends[i] - starts[i];
		memcpy(line, shown + starts[i], line_length);
		line[line_length] = '\0';
		draw_line(line, x, y + (i - first) * line_height, scale, color, Z_TEXT, 0.0f);
	}
	if (picture && count) {
		if (!advance_visible) {
			advance_visible = 1;
			advance_started = now_ms();
		}
		unsigned frames = (unsigned)config.num[NUM_ADVANCE_FRAMES];
		unsigned frame = ((now_ms() - advance_started) / (u32)config.num[NUM_ADVANCE_FRAME_MS]) % frames;
		Tex3DS_SubTexture sub = picture->sub;
		float units = picture->sub.right / picture->width;
		sub.width = (u16)frame_width;
		sub.left = frame * frame_width * units;
		sub.right = (frame + 1) * frame_width * units;
		C2D_Image image = { picture->image.tex, &sub };
		C2D_DrawImageAt(image, x + icon_x, y + (icon_line - first) * line_height,
			Z_TEXT + 0.01f, NULL, image_scale, image_scale);
	}
	measure_scale = previous_scale;
}

/* Top-left corner of a sprite: "X,Y" is exact pixels on the 400x240 screen, otherwise a named
   position with the sprite centered on it and its bottom on the screen edge. */
static void sprite_origin(const char *pos, float width, float height, float *x, float *y)
{
	if (strchr(pos, ',') != NULL) {
		*x = SCENE_X + (float)atoi(pos);
		*y = (float)atoi(strchr(pos, ',') + 1);
		return;
	}
	*y = TOP_H - height;
	float center = strcmp(pos, "left") == 0 ? 90.0f
		: strcmp(pos, "right") == 0 ? 310.0f
		: strcmp(pos, "farleft") == 0 ? 40.0f
		: strcmp(pos, "farright") == 0 ? 360.0f : 200.0f;
	*x = SCENE_X + center - width / 2.0f;
}

static size_t utf8_prefix(const char *text, size_t chars)
{
	size_t index = 0;
	while (text[index] != '\0' && chars > 0) {
		++index;
		while (((unsigned char)text[index] & 0xC0) == 0x80) {
			++index;
		}
		--chars;
	}
	return index;
}

static u32 speaker_color(unsigned rgb)
{
	return rgb != 0 ? C2D_Color32((u8)(rgb >> 16), (u8)(rgb >> 8), (u8)rgb, 255) : theme_accent();
}

static void draw_scene(void)
{
	if (scene.bg_prev[0] != '\0') {
		draw_bg_layer(scene.bg_prev, 1.0f, Z_BG);
		float alpha = gs_bg_fade_alpha(scene.bg_elapsed, scene.bg_duration);
		if (bgloop.active && loop_pictures[0] != NULL) {
			Picture *picture = loop_pictures[0];
			C2D_ImageTint tint;
			C2D_AlphaImageTint(&tint, alpha);
			C2D_DrawImageAt(picture->image, (TOP_W - picture->width) / 2.0f,
				(TOP_H - picture->height) / 2.0f, Z_BG_LOOP1, &tint, 1.0f, 1.0f);
		} else {
			draw_bg_layer(scene.bg, alpha, Z_BG_LOOP1);
		}
	} else if (bgloop.active) {
		float alpha1, alpha2;
		gs_bgloop_alpha(&bgloop, &alpha1, &alpha2);
		for (int i = 0; i < 3; ++i) {
			float alpha = i == 0 ? 1.0f : i == 1 ? alpha1 : alpha2;
			Picture *picture = loop_pictures[i];
			if (picture && alpha > 0.0f) {
				C2D_ImageTint tint;
				C2D_AlphaImageTint(&tint, alpha);
				C2D_DrawImageAt(picture->image, (TOP_W - picture->width) / 2.0f,
					(TOP_H - picture->height) / 2.0f,
					i == 0 ? Z_BG : i == 1 ? Z_BG_LOOP1 : Z_BG_LOOP2, &tint, 1.0f, 1.0f);
			} else if (i == 0 && !picture) {
				u8 shade = !strcmp(scene.bg, "white") ? 255 : 0;
				rect(0, 0, TOP_W, TOP_H, C2D_Color32(shade, shade, shade, 255), Z_BG);
			}
		}
	} else {
		draw_bg_layer(scene.bg, 1.0f, Z_BG);
	}

	/* Two passes: close-up sprites (asset name ending in "_b") are drawn after the
	   others so they stay in front, as in the original game (index 5000 vs 3000). */
	for (int pass = 0; pass < 2; ++pass)
	for (int index = 0; index < MAX_SPRITES; ++index) {
		const Sprite *sprite = &scene.sprites[index];
		size_t asset_len = strlen(sprite->asset);
		int close_up = asset_len > 2 && !strcmp(sprite->asset + asset_len - 2, "_b");
		if (close_up != pass) continue;
		Picture *picture = sprite->active ? get_picture("sprites", sprite->asset) : NULL;
		if (picture != NULL && !strcmp(sprite->pos, "face")) {
			/* Speaker portrait: fixed above the text box's left corner, unaffected by camera effects. */
			/* face_box puts portraits inside the text box, the default box included */
			int face_in_box = config.num[NUM_FACE_BOX];
			if (!display_options.nvl && !face_in_box && !bottom_text_active()) {
				C2D_ImageTint tint;
				C2D_AlphaImageTint(&tint, sprite->alpha);
				C2D_DrawImageAt(picture->image, (float)config.num[NUM_TEXTBOX_X],
					(float)config.num[NUM_TEXTBOX_Y] - (float)picture->height - 2.0f, Z_BOX - 0.01f,
					&tint, 1.0f, 1.0f);
			}
		} else if (picture != NULL) {
			float x, y;
			sprite_origin(sprite->pos, (float)picture->width, (float)picture->height, &x, &y);
			cur_hop = index < 16 && hop_t[index] > 0.0f
				? -fabsf(sinf(hop_t[index] / 0.35f * 6.2831853f * 1.0f)) * 14.0f : 0.0f;
			draw_picture(picture, x, y, sprite->alpha);
			cur_hop = 0.0f;
		}
	}
	if (flash_left > 0.0f && flash_total > 0.0f) {
		u8 shade = (u8)flash_shade;
		rect(0, 0, TOP_W, TOP_H, C2D_Color32(shade, shade, shade, (u8)(255.0f * flash_left / flash_total)),
			Z_SPRITE + 0.05f);
	}
}

/* Where the text box was drawn this frame (for the Auto indicator); w = 0 if not drawn. */
static float shown_box_x, shown_box_y, shown_box_w;
static u32 shown_box_frame, frame_counter;

/* Auto mode: a small play mark inside the text box's top right corner (the screen's top
   right when no box is shown), gently pulsing. */
static void draw_auto_indicator(void)
{
	float x = TOP_W - 16.0f, y = 6.0f;
	if (shown_box_frame == frame_counter && shown_box_w > 0.0f) {
		x = shown_box_x + shown_box_w - 15.0f;
		y = shown_box_y + 6.0f;
	}
	float pulse = 0.6f + 0.4f * sinf((float)(now_ms() % 1600u) / 1600.0f * 6.2831853f);
	u32 color = (COLOR_ACCENT & 0x00FFFFFFu) | ((u32)(255.0f * pulse) << 24);
	C2D_DrawTriangle(x, y, color, x, y + 9.0f, color, x + 7.5f, y + 4.5f, color, Z_HEADER_TEXT);
}

static void draw_textbox(void)
{
	GameConfig defaults;
	config_defaults(&defaults);
	int custom = display_options_use_custom(&display_options, current_textbox_available());
	const int *n = custom ? config.num : defaults.num;
	float bx = display_options.nvl ? 8.0f : (float)n[NUM_TEXTBOX_X];
	const float by = display_options.nvl ? 8.0f : (float)n[NUM_TEXTBOX_Y];
	float bw = display_options.nvl ? TOP_W - 16.0f : (float)n[NUM_TEXTBOX_W];
	if (!display_options.nvl && TOP_W > 400.0f) {
		/* widescreen: a box spanning the screen widens with it; others move with the scene */
		if (bx <= 16.0f && bx + bw >= 384.0f) bw += TOP_W - 400.0f;
		else bx += SCENE_X;
	}
	const float bh = display_options.nvl ? 224.0f : (float)n[NUM_TEXTBOX_H];
	shown_box_x = bx;
	shown_box_y = by;
	shown_box_w = bw;
	shown_box_frame = frame_counter;
	int accumulating = display_options.nvl && config.num[NUM_NVL_ACCUMULATE];
	int named = !accumulating && event.speaker[0] != '\0';
	Picture *textbox = custom ? ui_image(display_options.nvl && custom_nvl_available
		? IMG_NVL_TEXTBOX : IMG_TEXTBOX) : NULL;
	u32 fill = custom && config.has_panel ? rgb_color(config.panel, (u8)n[NUM_TEXTBOX_ALPHA])
		: C2D_Color32(8, 14, 20, (u8)n[NUM_TEXTBOX_ALPHA]);
	if (display_options.nvl) {
		rect(0, 0, TOP_W, TOP_H, C2D_Color32(0, 0, 0, (u8)n[NUM_NVL_DIM]), Z_BOX);
		if (textbox != NULL) {
			C2D_DrawImageAt(textbox->image, bx, by, Z_BOX + 0.01f, NULL,
				bw / (float)textbox->width, bh / (float)textbox->height);
		}
		if (n[NUM_NVL_BORDER]) {
			u32 border = custom ? theme_accent() : COLOR_ACCENT;
			rect(bx, by, bw, 2, border, Z_BOX + 0.02f);
			rect(bx, by + bh - 2, bw, 2, border, Z_BOX + 0.02f);
			rect(bx, by, 2, bh, border, Z_BOX + 0.02f);
			rect(bx + bw - 2, by, 2, bh, border, Z_BOX + 0.02f);
		}
	} else if (textbox != NULL) {
		C2D_DrawImageAt(textbox->image, bx, by, Z_BOX, NULL,
			bw / (float)textbox->width, bh / (float)textbox->height);
	} else {
		rect(bx, by, bw, bh, fill, Z_BOX);
		rect(bx, by, bw, 2, custom ? theme_accent() : COLOR_ACCENT, Z_BOX);
	}
	float face_inset = 0.0f;
	if (!display_options.nvl && config.num[NUM_FACE_BOX]) {
		for (int index = 0; index < MAX_SPRITES; ++index) {
			const Sprite *sprite = &scene.sprites[index];
			if (!sprite->active || strcmp(sprite->pos, "face") != 0 || sprite->alpha <= 0.0f) continue;
			Picture *picture = get_picture("sprites", sprite->asset);
			if (picture == NULL) continue;
			float max_width = fmaxf(1.0f, fminf(80.0f, bw - 28.0f));
			float scale = fminf(1.0f, fminf(max_width / (float)picture->width,
				(bh + 8.0f) / (float)picture->height));
			float face_width = (float)picture->width * scale;
			C2D_ImageTint tint;
			C2D_AlphaImageTint(&tint, sprite->alpha);
			C2D_DrawImageAt(picture->image, bx + 4.0f, by + bh - (float)picture->height * scale,
				Z_BOX + 0.03f, &tint, scale, scale);
			face_inset = fmaxf(face_inset, face_width + 12.0f);
		}
	}
	if (named) {
		Picture *plate = custom && !display_options.nvl ? ui_image(IMG_NAMEBOX) : NULL;
		if (plate != NULL) {
			C2D_DrawImageAt(plate->image, bx + (float)n[NUM_NAMEBOX_X], by + (float)n[NUM_NAMEBOX_Y],
				Z_BOX + 0.02f, NULL, 1.0f, 1.0f);
		}
		draw_line(event.speaker, bx + (display_options.nvl ? (float)n[NUM_NVL_MARGIN_X]
			: fmaxf((float)n[NUM_NAME_X], face_inset)),
			by + (display_options.nvl ? (float)n[NUM_NVL_MARGIN_Y] - 2.0f : (float)n[NUM_NAME_Y]),
			(float)n[NUM_NAME_SIZE] / 100.0f,
			event.color != 0 || custom ? speaker_color(event.color) : COLOR_ACCENT, Z_TEXT, 0.0f);
	}
	float text_x = display_options.nvl ? (float)n[NUM_NVL_MARGIN_X] : (float)n[NUM_TEXT_X];
	float right_margin = text_x;
	text_x = fmaxf(text_x, face_inset);
	float text_y = display_options.nvl ? (float)n[NUM_NVL_MARGIN_Y] + (named ? 22.0f : 0.0f)
		: (float)(named ? n[NUM_TEXT_Y] : n[NUM_NARRATION_Y]);
	float width = bw - text_x - right_margin;
	float scale = (float)n[NUM_TEXT_SIZE] / 100.0f;
	u32 color = custom && config.has_text ? theme_text() : C2D_Color32(235, 235, 235, 255);
	if (accumulating) {
		draw_nvl_page(bx + text_x, by + text_y, width, bh - text_y - n[NUM_NVL_MARGIN_Y], scale, color);
		return;
	}
	char shown[GS_TEXT_LEN + 64];
	size_t length = utf8_prefix(event.text, (size_t)chars_shown);
	const char *full = event.text;
	size_t visible = length;
	if (config.num[NUM_NVL_ACCUMULATE] && !display_options.nvl) {
		while (*full == '\n' || *full == '\r') {
			++full;
			if (visible > 0) --visible;
		}
	}
	if (!display_options.nvl) {
		scale = fit_text_scale(event.text, width, bh - text_y - 4.0f, scale);
	}
#ifdef VN_SDL
	/* lay out the whole line once; draw_text_reveal shows the typed part smoothly */
	reveal_with_breaks(full, strlen(full), width, scale, shown, sizeof(shown));
	float reveal = chars_shown - (float)(full - event.text);
	(void)visible;
#else
	reveal_with_breaks(full, visible, width, scale, shown, sizeof(shown));
#endif
	const char *visible_text = shown;
	Picture *advance = advance_available ? ui_image(IMG_ADVANCE) : NULL;
	/* The lines are already broken (reveal_with_breaks); give the renderer room so it never
	   wraps again: fonts render slightly wider at some sizes (Wii U), which pushed an extra
	   line out of the bottom of the box. */
	float draw_width = width * REWRAP_SLACK;
#ifdef VN_SDL
	if (advance != NULL && event.text[length] == '\0') {
		draw_dialogue_with_advance(visible_text, bx + text_x, by + text_y, draw_width, scale, color, advance, 1);
	} else {
		draw_text_reveal(visible_text, bx + text_x, by + text_y, scale, color, reveal);
	}
#else
	if (advance != NULL) {
		draw_dialogue_with_advance(visible_text, bx + text_x, by + text_y, draw_width, scale, color,
			advance, event.text[length] == '\0');
	} else {
		draw_wrapped(visible_text, bx + text_x, by + text_y, draw_width, scale, color, Z_TEXT);
	}
#endif
}

static void draw_choices(void)
{
	const int *n = config.num;
	const float width = (float)n[NUM_CHOICE_W], height = (float)n[NUM_CHOICE_H];
	const float row = height + (float)n[NUM_CHOICE_GAP];
	const float scale = (float)n[NUM_CHOICE_SIZE] / 100.0f;
	rect(0, 0, TOP_W, TOP_H, C2D_Color32(0, 0, 0, (u8)n[NUM_CHOICE_DIM]), Z_BOX);
	float y = (TOP_H - ((float)event.choice_count * row - (float)n[NUM_CHOICE_GAP])) / 2.0f;
	float x = (TOP_W - width) / 2.0f;
	for (int index = 0; index < event.choice_count; ++index, y += row) {
		int selected = index == menu_cursor;
		int skinned = ui_image(selected ? IMG_CHOICE_SEL : IMG_CHOICE) != NULL;
		draw_panel(selected ? IMG_CHOICE_SEL : IMG_CHOICE, x, y, width, height,
			selected ? theme_accent() : theme_panel(235), Z_OVERLAY);
		draw_line(event.choices[index], x + (float)n[NUM_CHOICE_TEXT_X], y + (height - 30.0f * scale) / 2.0f,
			scale, selected && !skinned ? theme_dark_text() : theme_text(), Z_OVERLAY_TEXT,
			width - 2.0f * (float)n[NUM_CHOICE_TEXT_X]);
	}
}

/* The game's full-screen image if it has one, otherwise a dark tint over the scene. */
static void draw_backdrop(int image, u8 tint_alpha)
{
	Picture *picture = ui_image(image);
	if (picture != NULL) {
		rect(0, 0, TOP_W, TOP_H, C2D_Color32(0, 0, 0, 255), Z_BOX);
		C2D_DrawImageAt(picture->image, (TOP_W - (float)picture->width) / 2.0f,
			(TOP_H - (float)picture->height) / 2.0f, Z_BOX + 0.01f, NULL, 1.0f, 1.0f);
	} else {
		rect(0, 0, TOP_W, TOP_H, config.has_panel ? theme_panel(tint_alpha)
			: C2D_Color32(6, 10, 14, tint_alpha), Z_BOX);
	}
}

static void draw_backlog(void)
{
	draw_backdrop(IMG_BACKLOG_BG, 244);

	float bottom = TOP_H - 4.0f;
	for (int age = backlog_view; age < backlog.count; ++age) {
		int slot = backlog_slot(age);
		const BacklogEntry *entry = &backlog.entries[slot];
		const WrapLayout *layout = backlog_layout(slot);
		float y = bottom - backlog_height(slot);
		float cursor = y;
		if (entry->speaker[0] != '\0') {
			draw_line(entry->speaker, 14, cursor, LOG_SCALE, speaker_color(entry->color),
				Z_OVERLAY_TEXT, 0.0f);
			cursor += LOG_SPEAKER_H;
		}
		for (int line = 0; line < layout->count; ++line, cursor += LOG_LINE_H) {
			char text[GS_TEXT_LEN];
			size_t length = (size_t)(layout->end[line] - layout->start[line]);
			memcpy(text, entry->text + layout->start[line], length);
			text[length] = '\0';
			draw_line(text, 14, cursor, LOG_SCALE, theme_text(),
				Z_OVERLAY_TEXT, 0.0f);
		}
		bottom = y;
		if (bottom < 0.0f) {
			break;
		}
	}

	rect(0, 0, TOP_W, 17, theme_panel(255), Z_HEADER);
	char header[64];
	snprintf(header, sizeof(header), "BACKLOG   %d / %d", backlog.count - backlog_view, backlog.count);
	draw_line(header, 8, 1, 0.45f, theme_accent(), Z_HEADER_TEXT, 0.0f);
	draw_line("Up/Down scroll   B close", 232, 2, 0.4f, C2D_Color32(150, 170, 165, 255),
		Z_HEADER_TEXT, 0.0f);
}

/* Backlog on the bottom screen: newest line at the bottom, swipe to scroll, tap a line to
   hear its voice again. Rows show the speaker and the start of the line. */
#define LOG_ROWS 5
#define LOG_ROW_H 36.0f
#define LOG_TOP 24.0f
static int log_drag, log_drag_moved;   /* touch drag state */
static float log_drag_y;

static int backlog_row_age(int row)   /* row 0 is the top one */
{
	return backlog_view + (LOG_ROWS - 1 - row);
}

static void scroll_backlog(int direction);

/* A round-cornered button with a play triangle (voice replay in the backlog). */
static void draw_play_icon(float x, float y, float size)
{
	const u32 face = theme_accent();
	const float r = size * 0.22f;
	rect(x + r, y, size - 2 * r, size, face, Z_BOX + 0.01f);
	rect(x, y + r, size, size - 2 * r, face, Z_BOX + 0.01f);
	rect(x + r * 0.3f, y + r * 0.3f, size - r * 0.6f, size - r * 0.6f, face, Z_BOX + 0.01f);
	const u32 mark = theme_dark_text();
	float cx = x + size * 0.54f, cy = y + size * 0.5f, h = size * 0.24f;
	C2D_DrawTriangle(cx - h * 0.8f, cy - h, mark, cx - h * 0.8f, cy + h, mark, cx + h * 1.0f, cy, mark, Z_BOX + 0.02f);
}

static void draw_backlog_touch(void)
{
	const u32 dim = C2D_Color32(150, 170, 165, 255);
	draw_line("Backlog", 14, 4, 0.5f, theme_accent(), Z_TEXT, 0.0f);
	char where[32];
	snprintf(where, sizeof(where), "%d / %d", backlog.count - backlog_view, backlog.count);
	draw_line(where, BOTTOM_W - 70, 6, 0.4f, dim, Z_TEXT, 0.0f);
	for (int row = 0; row < LOG_ROWS; ++row) {
		int age = backlog_row_age(row);
		if (age < 0 || age >= backlog.count) continue;
		int slot = backlog_slot(age);
		const BacklogEntry *entry = &backlog.entries[slot];
		float y = LOG_TOP + row * LOG_ROW_H;
		int voiced = log_voice[slot][0] != '\0';
		rect(8, y, BOTTOM_W - 16, LOG_ROW_H - 4, theme_item(), Z_BOX);
		float text_w = BOTTOM_W - (voiced ? 64.0f : 32.0f);
		if (entry->speaker[0]) {
			draw_line(entry->speaker, 14, y + 1, 0.36f, speaker_color(entry->color), Z_TEXT, text_w);
		}
		/* the start of the line, cut to fit */
		char shown[GS_TEXT_LEN];
		const char *text = entry->text;
		while (*text == '\n' || *text == ' ') ++text;
		snprintf(shown, sizeof(shown), "%s", text);
		for (char *c = shown; *c; ++c) if (*c == '\n') *c = ' ';
		float previous = measure_scale;
		measure_scale = 0.4f;
		size_t length = strlen(shown);
		while (length > 0 && measure_text(shown, length, NULL) > text_w) {
			do --length; while (length > 0 && ((unsigned char)shown[length] & 0xC0) == 0x80);
		}
		measure_scale = previous;
		if (length < strlen(shown) && length > 3) strcpy(shown + length - 3, "...");
		draw_line(shown, 14, y + (entry->speaker[0] ? 15.0f : 8.0f), 0.4f, theme_text(), Z_TEXT, 0.0f);
		if (voiced) {
			draw_play_icon(BOTTOM_W - 46, y + 4, LOG_ROW_H - 12);
		}
	}
	draw_line("Swipe to scroll   Tap a line: voice   B close", 14, 214, 0.38f, dim, Z_TEXT, BOTTOM_W - 28);
}

static void backlog_touch_input(u32 down, u32 held)
{
	touchPosition touch;
	if (down & KEY_TOUCH) {
		hidTouchRead(&touch);
		log_drag = 1;
		log_drag_moved = 0;
		log_drag_y = touch.py;
		return;
	}
	if (log_drag && (held & KEY_TOUCH)) {
		hidTouchRead(&touch);
		float delta = (float)touch.py - log_drag_y;
		/* dragging down shows older lines, like scrolling a list */
		while (delta >= LOG_ROW_H * 0.6f) {
			scroll_backlog(1);
			log_drag_y += LOG_ROW_H * 0.6f;
			delta -= LOG_ROW_H * 0.6f;
			log_drag_moved = 1;
		}
		while (delta <= -LOG_ROW_H * 0.6f) {
			scroll_backlog(-1);
			log_drag_y -= LOG_ROW_H * 0.6f;
			delta += LOG_ROW_H * 0.6f;
			log_drag_moved = 1;
		}
		if (delta > 6.0f || delta < -6.0f) log_drag_moved = 1;
		return;
	}
	if (log_drag && !(held & KEY_TOUCH)) {   /* released */
		log_drag = 0;
		if (!log_drag_moved && log_drag_y >= LOG_TOP) {
			int row = (int)((log_drag_y - LOG_TOP) / LOG_ROW_H);
			int age = backlog_row_age(row);
			if (row >= 0 && row < LOG_ROWS && age >= 0 && age < backlog.count) {
				const char *voice = log_voice[backlog_slot(age)];
				if (voice[0]) {
					GsEvent replay;
					memset(&replay, 0, sizeof(replay));
					snprintf(replay.asset, sizeof(replay.asset), "%s", voice);
					audio_stop_sfx();
					play_audio_event(&replay, 0);
				}
			}
		}
	}
}

static void draw_game_menu(void)
{
	draw_backdrop(IMG_MENU_BG, 225);
	const float row = menu_count > 6 ? 22.0f : menu_count > 5 ? 29.0f : 34.0f;
	const float height = menu_count > 6 ? 20.0f : menu_count > 5 ? 25.0f : 28.0f;
	draw_line(config.menu_title[0] != '\0' ? config.menu_title : "Menu", 60, 14, 0.8f,
		theme_accent(), Z_OVERLAY_TEXT, TOP_W - 120);
	float y = menu_count > 6 ? 42.0f : menu_count > 5 ? 48.0f : 62.0f;
	for (int index = 0; index < menu_count; ++index, y += row) {
		int selected = index == menu_cursor_item;
		draw_panel(selected ? IMG_ITEM_SEL : IMG_ITEM, 70, y, TOP_W - 140, height,
			selected ? theme_accent() : theme_panel(215), Z_OVERLAY);
		draw_line(config.labels[menu_items[index]], 82, y + (menu_count > 6 ? 2.0f : 5.0f), menu_count > 6 ? 0.5f : 0.55f,
			selected && !ui_image(IMG_ITEM_SEL) ? theme_dark_text() : theme_text(), Z_OVERLAY_TEXT, TOP_W - 164);
	}
	draw_line("D-pad choose   A confirm   B back", 70, 224, 0.4f, C2D_Color32(150, 170, 165, 255),
		Z_OVERLAY_TEXT, 0.0f);
}

#ifdef __WIIU__
#define OPTION_ROWS 10   /* + TV screen */
#else
#define OPTION_ROWS 9
#endif
#define OPTION_VISIBLE 6
static int option_top;   /* first Options row shown; follows the cursor */

static int option_first(void)
{
	if (panel_cursor < option_top) option_top = panel_cursor;
	if (panel_cursor >= option_top + OPTION_VISIBLE) option_top = panel_cursor - OPTION_VISIBLE + 1;
	if (option_top > OPTION_ROWS - OPTION_VISIBLE) option_top = OPTION_ROWS - OPTION_VISIBLE;
	if (option_top < 0) option_top = 0;
	return option_top;
}

/* Labels and current values of the Options rows. */
static void option_texts(const char **labels, const char **values)
{
	static const char *speeds[] = {"Slow", "Normal", "Fast", "Instant"};
	static const char *waits[] = {"Short", "Normal", "Long"};
	static const char *names[OPTION_ROWS] = {"Textbox", "Display style", "Text screen", "Text speed",
		"Auto speed", "Fast-forward", "Master volume", "Music volume", "Voice volume"
#ifdef __WIIU__
		, "TV screen"
#endif
	};
	static char volumes[3][8];
	for (int i = 0; i < 3; ++i) {
		snprintf(volumes[i], sizeof(volumes[i]), "%d", *volume_field(&display_options, 6 + i));
		values[6 + i] = volumes[i];
	}
	for (int i = 0; i < OPTION_ROWS; ++i) labels[i] = names[i];
	values[0] = display_options_use_custom(&display_options, current_textbox_available()) ? "Custom" : "Default";
	values[1] = display_options.nvl ? "NVL" : "ADV";
	values[2] = display_options.bottom_text ? "Bottom" : display_options.touch_choices ? "Top, choices below" : "Top";
	values[3] = speeds[display_options.text_speed & 3];
	values[4] = waits[display_options.auto_speed % 3];
	values[5] = display_options.skip_read ? "Read only" : "All";
#ifdef __WIIU__
	values[9] = display_options.tv_wide ? "Wide" : "Normal";
#endif
}

static void draw_options(void)
{
	const float ox = SCENE_X;   /* centred on a wide TV screen */
	draw_backdrop(IMG_MENU_BG, 225);
	draw_line(config.labels[LABEL_OPTIONS], ox + 40, 14, 0.7f, theme_accent(), Z_OVERLAY_TEXT, 320);
	const char *labels[OPTION_ROWS], *values[OPTION_ROWS];
	option_texts(labels, values);
	int first = option_first();
	for (int i = first; i < first + OPTION_VISIBLE; ++i) {
		float y = 46.0f + (float)(i - first) * 27.0f;
		int selected = i == panel_cursor;
		draw_panel(selected ? IMG_ITEM_SEL : IMG_ITEM, ox + 40, y, 320, 24,
			selected ? theme_accent() : theme_panel(215), Z_OVERLAY);
		u32 color = selected && !ui_image(IMG_ITEM_SEL) ? theme_dark_text() : theme_text();
		draw_line(labels[i], ox + 52, y + 4, 0.46f, color, Z_OVERLAY_TEXT, 155);
		const int *volume = volume_field(&display_options, i);
		if (volume != NULL) {   /* slider: track, filled part, value */
			rect(ox + 216, y + 10, 100, 4, C2D_Color32(255, 255, 255, 60), Z_OVERLAY_TEXT);
			rect(ox + 216, y + 10, 10.0f * (float)*volume, 4, color, Z_OVERLAY_TEXT + 0.001f);
			rect(ox + 214 + 10.0f * (float)*volume, y + 6, 4, 12, color, Z_OVERLAY_TEXT + 0.002f);
			draw_line(values[i], ox + 326, y + 4, 0.46f, color, Z_OVERLAY_TEXT, 30);
		} else {
			draw_line(values[i], ox + 216, y + 4, 0.46f, color, Z_OVERLAY_TEXT, 130);
		}
	}
	/* scrollbar: the list is longer than the box */
	float track = OPTION_VISIBLE * 27.0f - 3.0f;
	rect(ox + 364, 46, 3, track, C2D_Color32(255, 255, 255, 40), Z_OVERLAY);
	rect(ox + 364, 46 + track * (float)first / OPTION_ROWS, 3, track * OPTION_VISIBLE / OPTION_ROWS,
		theme_accent(), Z_OVERLAY + 0.001f);
	draw_line(first + OPTION_VISIBLE < OPTION_ROWS ? "A / Left / Right change   B back   Down: more"
		: "A / Left / Right change   B back", ox + 40, 213, 0.4f,
		C2D_Color32(150, 170, 165, 255), Z_OVERLAY_TEXT, 0.0f);
}

static int list_first(int cursor, int total);
static void draw_list_row(float y, const char *label, int selected);

static void draw_glossary(void)
{
	draw_backdrop(IMG_MENU_BG, 225);
	draw_line(config.labels[LABEL_GLOSSARY], 14, 8, 0.5f, theme_accent(), Z_OVERLAY_TEXT, TOP_W - 28);
	if (gloss_visible_count == 0) {
		draw_line("No terms unlocked yet.", 40, 50, 0.5f, theme_text(), Z_OVERLAY_TEXT, TOP_W - 80);
		draw_line("B back", 40, 224, 0.4f, theme_text(), Z_OVERLAY_TEXT, 0.0f);
		return;
	}
	if (!gloss_detail) {
		int first = list_first(panel_cursor, gloss_visible_count);
		for (int index = first; index < gloss_visible_count && index < first + LIST_ROWS; ++index) {
			draw_list_row(30.0f + (float)(index - first) * 23.0f,
				glossary.terms[gloss_indices[index]].name, index == panel_cursor);
		}
		char footer[64];
		snprintf(footer, sizeof(footer), "A open   B back   %d/%d", panel_cursor + 1, gloss_visible_count);
		draw_line(footer, 40, 224, 0.4f, C2D_Color32(150, 170, 165, 255), Z_OVERLAY_TEXT, 0.0f);
		return;
	}
	if (gloss_layout_for != panel_cursor) {
		measure_scale = 0.5f;
		gloss_line_count = wrap_text_lines(glossary.terms[gloss_indices[panel_cursor]].text, 372.0f,
			measure_text, NULL, gloss_starts, gloss_ends, CFG_TEXT_LEN);
		measure_scale = LOG_SCALE;
		gloss_layout_for = panel_cursor;
	}
	const GlossaryTerm *term = &glossary.terms[gloss_indices[panel_cursor]];
	draw_line(term->name, 14, 22, 0.7f, theme_accent(), Z_OVERLAY_TEXT, TOP_W - 28);
	rect(14, 46, TOP_W - 28, 1, theme_accent(), Z_OVERLAY);
	for (int line = gloss_scroll; line < gloss_line_count && line < gloss_scroll + GLOSSARY_VISIBLE_LINES; ++line) {
		char text[CFG_TEXT_LEN + 1];
		size_t length = (size_t)(gloss_ends[line] - gloss_starts[line]);
		memcpy(text, term->text + gloss_starts[line], length);
		text[length] = '\0';
		draw_line(text, 14, 52.0f + (float)(line - gloss_scroll) * 15.0f, 0.5f, theme_text(), Z_OVERLAY_TEXT, 0.0f);
	}
	draw_line("Up/Down scroll   B back", 14, 224, 0.4f,
		C2D_Color32(150, 170, 165, 255), Z_OVERLAY_TEXT, 0.0f);
}

static void draw_slot_list(void);
static void draw_quit_confirm(void);

static const char *title_item_label(int item)
{
	switch (item) {
	case TITLE_START: return config.title_start;
	case TITLE_LOAD: return config.title_load;
	case TITLE_OPTIONS: return config.title_options;
	case TITLE_EXTRAS: return config.title_extras;
	case TITLE_ENDINGS: return config.labels[LABEL_ENDINGS];
	case TITLE_GUIDE: return config.labels[LABEL_GUIDE];
	case TITLE_SYNC: return config.labels[LABEL_SYNC];
	default: return "";
	}
}

static void draw_gallery(void)
{
	rect(0, 0, TOP_W, TOP_H, C2D_Color32(0, 0, 0, 255), Z_BG);
	if (gallery_detail) {
		Picture *picture = get_picture("bg", gallery_items[gallery_cursor]);
		if (picture != NULL) {
			float scale = TOP_W / (float)picture->width;
			if (TOP_H / (float)picture->height < scale) {
				scale = TOP_H / (float)picture->height;
			}
			float width = picture->width * scale;
			float height = picture->height * scale;
			C2D_DrawImageAt(picture->image, (TOP_W - width) / 2.0f, (TOP_H - height) / 2.0f,
				Z_BG + 0.01f, NULL, scale, scale);
		} else {
			draw_line("CG image is missing from bg/", 18, 110, 0.5f,
				C2D_Color32(255, 150, 140, 255), Z_OVERLAY_TEXT, TOP_W - 36);
		}
		rect(0, TOP_H - 19, TOP_W, 19, C2D_Color32(0, 0, 0, 190), Z_OVERLAY);
		draw_line(gallery_items[gallery_cursor], 8, TOP_H - 17, 0.38f,
			C2D_Color32(235, 235, 235, 255), Z_OVERLAY_TEXT, TOP_W - 16);
		return;
	}

	draw_line("CG Gallery", 10, 5, 0.55f, theme_accent(), Z_OVERLAY_TEXT, 250);
	char page[32];
	snprintf(page, sizeof(page), "%d / %d",
		gallery_cursor / GALLERY_PAGE_SIZE + 1,
		(gallery_count + GALLERY_PAGE_SIZE - 1) / GALLERY_PAGE_SIZE);
	draw_line(page, 330, 7, 0.4f, C2D_Color32(200, 210, 205, 255), Z_OVERLAY_TEXT, 60);
	for (int cell = 0; cell < GALLERY_PAGE_SIZE; ++cell) {
		int index = gallery_cursor / GALLERY_PAGE_SIZE * GALLERY_PAGE_SIZE + cell;
		if (index >= gallery_count) break;
		int column = cell % 3, row = cell / 3;
		float x = 8.0f + (float)column * 128.0f;
		float y = 30.0f + (float)row * 93.0f;
		u32 border = index == gallery_cursor ? theme_accent() : C2D_Color32(50, 65, 68, 255);
		rect(x, y, 120, 87, border, Z_OVERLAY);
		rect(x + 2, y + 2, 116, 83, C2D_Color32(16, 24, 27, 255), Z_OVERLAY + 0.01f);

		Picture *thumb = get_picture("gallery/thumbs", gallery_items[index]);
		if (thumb != NULL) {
			float scale = 112.0f / (float)thumb->width;
			if (66.0f / (float)thumb->height < scale) {
				scale = 66.0f / (float)thumb->height;
			}
			float width = (float)thumb->width * scale;
			float height = (float)thumb->height * scale;
			C2D_DrawImageAt(thumb->image, x + (120.0f - width) / 2.0f,
				y + 4.0f + (66.0f - height) / 2.0f, Z_OVERLAY + 0.02f, NULL, scale, scale);
		} else {
			draw_line("Missing thumb", x + 5.0f, y + 28.0f, 0.35f,
				C2D_Color32(255, 150, 140, 255), Z_OVERLAY_TEXT, 110);
		}
		draw_line(gallery_items[index], x + 5.0f, y + 71.0f, 0.33f,
			C2D_Color32(230, 235, 232, 255), Z_OVERLAY_TEXT, 110);
	}
	draw_line("A preview   B exit", 118, 220, 0.38f,
		C2D_Color32(165, 185, 180, 255), Z_OVERLAY_TEXT, 170);
}

static void draw_title_menu(void)
{
	draw_backdrop(IMG_MENU_BG, 255);
	const char *title = config.title[0] != '\0' ? config.title : game_name;
	draw_line(title, 24, 25, 0.8f, theme_accent(), Z_OVERLAY_TEXT, TOP_W - 48);
	rect(40, 58, TOP_W - 80, 1, theme_accent(), Z_OVERLAY);
	/* up to 4 rows at the usual spacing; more (Endings, Guide) closer together */
	float spacing = title_count > 4 ? 146.0f / (float)title_count : 34.0f;
	float first = title_count > 4 ? 68.0f : 76.0f;
	for (int index = 0; index < title_count; ++index) {
		draw_list_row(first + (float)index * spacing,
			title_item_label(title_items[index]), index == title_cursor);
	}
	draw_line("D-pad choose   A confirm   B game list", 40, 224, 0.4f,
		C2D_Color32(150, 170, 165, 255), Z_OVERLAY_TEXT, 0.0f);
}

#if defined(__WIIU__) || defined(VN_EXIT_MENU_TEST)
#define START_HINT "START exit menu   \xe2\x88\x92 GamePad only"
/* Off-TV play (Wii U): Minus shows everything on the GamePad. The GamePad's screen has
   the same logical size as the wide TV picture (427 x 240), so the TV's picture is drawn
   into the GamePad's target as it is; the TV shows a note. Touches there count as A
   while playing (the touch buttons are not on screen), except in the Guide, which is
   shown on the GamePad as usual. */
#define OFF_TV 1
#else
#define START_HINT "START exit app"
#endif
static int hit_button(const Button *buttons, int count, const touchPosition *touch);
static int touch_list_buttons(Button *out, const char *const *labels, int count, const char *extra);
static void draw_touch_list(const char *header, const char *const *labels, int count, int cursor,
	const char *extra);

#if defined(__WIIU__) || defined(VN_EXIT_MENU_TEST)
/* Wii U: START asks where to go (Homebrew Launcher, Wii U Menu) instead of quitting.
   VN_EXIT_MENU_TEST builds the same menu on PC, with stand-ins, to test it. */
#define EXIT_MENU 1
#ifdef VN_EXIT_MENU_TEST
static int vn_wiiu_can_exit_to_hbl(void) { return getenv("VN_NO_HBL") == NULL; }
static int vn_wiiu_exit_to_hbl(void) { TEST_LOG("EXIT hbl"); fprintf(stderr, "EXIT hbl\n"); return 2; }
static void vn_wiiu_exit_to_menu(void) { fprintf(stderr, "EXIT menu\n"); }
#endif
enum { EXIT_HBL, EXIT_WIIU_MENU };
static int exit_open, exit_cursor, exit_count, exit_items[2], exiting;
static const char *const exit_labels[] = {"Homebrew Launcher", "Wii U Menu"};

static void open_exit_menu(void)
{
	exit_count = 0;
	if (vn_wiiu_can_exit_to_hbl()) exit_items[exit_count++] = EXIT_HBL;
	exit_items[exit_count++] = EXIT_WIIU_MENU;
	exit_cursor = 0;
	exit_open = 1;
}

static int exit_menu_labels(const char **labels)
{
	for (int i = 0; i < exit_count; ++i) labels[i] = exit_labels[exit_items[i]];
	return exit_count;
}

/* Returns 1 when the app should end now (a plain exit goes back to the launcher). */
static int choose_exit(int item)
{
	exit_open = 0;
	if (item == EXIT_WIIU_MENU) {
		vn_wiiu_exit_to_menu();
		exiting = 1;   /* the system closes the app shortly */
		return 0;
	}
	int result = vn_wiiu_exit_to_hbl();
	if (result == 2) return 1;
	if (result == 1) {
		exiting = 1;
	} else {
		snprintf(notice, sizeof(notice), "Homebrew Launcher not found");
	}
	return 0;
}

static int handle_exit_menu(u32 down, u32 repeat)
{
	if (down & (KEY_B | KEY_START)) {
		exit_open = 0;
		return 0;
	}
	if (repeat & KEY_DDOWN) exit_cursor = (exit_cursor + 1) % exit_count;
	if (repeat & KEY_DUP) exit_cursor = (exit_cursor + exit_count - 1) % exit_count;
	if (down & KEY_A) return choose_exit(exit_items[exit_cursor]);
	if (down & KEY_TOUCH) {
		touchPosition touch;
		hidTouchRead(&touch);
		const char *labels[2];
		Button buttons[3];
		int count = exit_menu_labels(labels);
		int hit = hit_button(buttons, touch_list_buttons(buttons, labels, count, "Cancel"), &touch);
		if (hit >= 0 && hit < count) return choose_exit(exit_items[hit]);
		if (hit == count) exit_open = 0;
	}
	return 0;
}

static void draw_exit_top(void)
{
	rect(0, 0, TOP_W, TOP_H, C2D_Color32(0, 0, 0, 150), Z_OVERLAY);
	draw_panel(IMG_ITEM, 90, 60, 220, 40 + 28.0f * (exiting ? 1 : exit_count), theme_panel(235), Z_OVERLAY);
	draw_line(exiting ? "Exiting..." : "Exit to", 104, 68, 0.6f, theme_accent(), Z_OVERLAY_TEXT, 190);
	for (int i = 0; !exiting && i < exit_count; ++i) {
		float y = 96.0f + 28.0f * i;
		int selected = i == exit_cursor;
		if (selected) draw_panel(IMG_ITEM_SEL, 100, y - 2, 200, 24, theme_accent(), Z_OVERLAY + 0.001f);
		draw_line(exit_labels[exit_items[i]], 112, y + 2, 0.5f,
			selected && !ui_image(IMG_ITEM_SEL) ? theme_dark_text() : theme_text(), Z_OVERLAY_TEXT, 180);
	}
}

static void draw_exit_bottom(C3D_RenderTarget *target)
{
	C2D_TargetClear(target, C2D_Color32(17, 29, 34, 255));
	C2D_SceneBegin(target);
	C2D_TextBufClear(text_buf);
	if (exiting) {
		draw_line("Exiting...", 20, 100, 0.6f, theme_accent(), Z_TEXT, BOTTOM_W - 40);
		return;
	}
	const char *labels[2];
	int count = exit_menu_labels(labels);
	draw_touch_list("Exit to", labels, count, exit_cursor, "Cancel");
	draw_line("B / START cancel", 20, 186, 0.4f, C2D_Color32(150, 170, 165, 255), Z_TEXT, 0.0f);
}
#endif

static void draw_top(C3D_RenderTarget *target)
{
	++frame_counter;
	C2D_TargetClear(target, C2D_Color32(0, 0, 0, 255));
	C2D_SceneBegin(target);
	C2D_TextBufClear(text_buf);

	if (mode == MODE_SELECT || mode == MODE_ERROR) {
		draw_line("3DVN", 20, 20, 0.7f, COLOR_ACCENT, Z_TEXT, 0.0f);
		if (mode == MODE_ERROR) {
			draw_wrapped(message, 20, 70, TOP_W - 40, 0.5f, C2D_Color32(255, 140, 120, 255), Z_TEXT);
		}
		return;
	}
	if (mode == MODE_TITLE) {
		if (panel == PANEL_LOAD) {
			draw_slot_list();
		} else if (panel == PANEL_OPTIONS) {
			draw_options();
		} else if (panel == PANEL_ENDINGS) {
			draw_endings_top();
		} else if (panel == PANEL_SYNC) {
			draw_sync_top();
		} else {
			draw_title_menu();
		}
		return;
	}

	if (waiting == PLAY_VIDEO && video.picture != NULL) {
		float scale = TOP_W / (float)video.picture->width;
		if (TOP_H / (float)video.picture->height < scale) {
			scale = TOP_H / (float)video.picture->height;
		}
		C2D_DrawImageAt(video.picture->image, (TOP_W - (float)video.picture->width * scale) / 2.0f,
			(TOP_H - (float)video.picture->height * scale) / 2.0f, Z_BG, NULL, scale, scale);
		return;
	}
	if (waiting == PLAY_GALLERY) {
		draw_gallery();
		return;
	}
	draw_scene();
	if (panel == PANEL_BACKLOG) {
		draw_backlog();
	} else if (panel == PANEL_QUIT) {
		draw_quit_confirm();
	} else if (panel == PANEL_SAVE || panel == PANEL_LOAD) {
		draw_slot_list();
	} else if (panel == PANEL_MENU) {
		draw_game_menu();
	} else if (panel == PANEL_GLOSSARY) {
		draw_glossary();
	} else if (panel == PANEL_OPTIONS) {
		draw_options();
	} else if (panel == PANEL_ENDINGS) {
		draw_endings_top();
	} else if (panel == PANEL_SYNC) {
		draw_sync_top();
	} else if (panel == PANEL_GUIDE) {
		draw_guide_top();
	} else if (ui_hidden) {
		return;
	} else if (waiting == PLAY_MENU) {
		if (!touch_layout_active()) {
			draw_choices();
		}
	} else if (waiting == PLAY_TEXT) {
		if (!bottom_text_active()) {
			draw_textbox();
		}
	} else if (display_options.nvl && config.num[NUM_NVL_ACCUMULATE] && nvl_page.length
		&& (waiting == PLAY_TIMER || waiting == PLAY_WAITBG)) {
		draw_textbox();
	} else if (waiting == PLAY_TIMER && say_held) {
		if (!bottom_text_active()) {
			GsEvent current = event;
			event = held_say;
			draw_textbox();
			event = current;
		}
	} else if (waiting == PLAY_END && (scene.bg[0] == '\0' || !strcmp(scene.bg, "black"))) {
		/* Only over an empty screen: games that show their own end card keep it clean. */
		draw_line("THE END", 150, 100, 0.8f, C2D_Color32(255, 255, 255, 255), Z_TEXT, 0.0f);
	}
	if (ff_blocked && panel == PANEL_NONE && waiting == PLAY_TEXT) {
		draw_line("UNREAD - press A", TOP_W - 128, 4, 0.45f, COLOR_ACCENT, Z_HEADER_TEXT, 0.0f);
	} else if (fast_forward && panel == PANEL_NONE && waiting != PLAY_MENU && waiting != PLAY_END) {
		draw_line(">> FAST FORWARD", TOP_W - 110, 4, 0.45f, COLOR_ACCENT, Z_HEADER_TEXT, 0.0f);
	} else if (auto_mode && panel == PANEL_NONE && waiting != PLAY_END) {
		draw_auto_indicator();
	}
	if (bottom_text_active() && panel == PANEL_NONE && notice[0] != '\0') {
		/* The bottom screen is full of dialogue in this layout, so notices go here. */
		draw_line(notice, 6, 226, 0.4f, C2D_Color32(255, 200, 120, 255), Z_HEADER_TEXT, TOP_W - 12);
	}
}

/* Touch buttons of the play screen, in two columns (142 px wide each on the 3DS):
       Menu       Backlog
       Save       Load
       Hide text  Auto
       Guide      Quit        (Guide only when the game has one; Quit stays bottom right)
   Each button's action uses the strip_buttons numbering below (0-4), plus these: */
enum { ACT_GUIDE = 5, ACT_SAVE = 6, ACT_LOAD = 7 };
#define PLAY_COL_W ((BOTTOM_W - 36) / 2)
#define PLAY_COL_R (22 + PLAY_COL_W)
#define PLAY_ROW(n) (34 + (n) * 36)
static const Button play_buttons[] = {
	{14, PLAY_ROW(0), PLAY_COL_W, 32, "Menu  (X)"},
	{PLAY_COL_R, PLAY_ROW(0), PLAY_COL_W, 32, "Backlog  (L)"},
	{14, PLAY_ROW(1), PLAY_COL_W, 32, "Save"},
	{PLAY_COL_R, PLAY_ROW(1), PLAY_COL_W, 32, "Load"},
	{14, PLAY_ROW(2), PLAY_COL_W, 32, "Hide text  (B)"},
	{PLAY_COL_R, PLAY_ROW(2), PLAY_COL_W, 32, "Auto  (Y)"},
#ifdef OFF_TV
	{PLAY_COL_R, PLAY_ROW(3), PLAY_COL_W, 32, "Quit"},
#else
	{PLAY_COL_R, PLAY_ROW(3), PLAY_COL_W, 32, "Quit  (SELECT)"},
#endif
	{14, PLAY_ROW(3), PLAY_COL_W, 32, "Guide"},
};
static const int play_actions[] = {0, 1, ACT_SAVE, ACT_LOAD, 2, 4, 3, ACT_GUIDE};
#define AUTO_BUTTON 4        /* in strip_buttons */
#define PLAY_AUTO_BUTTON 5   /* in play_buttons */
#define COUNT_OF(array) (int)(sizeof(array) / sizeof((array)[0]))

static const Button *play_button_set(int *count)
{
	*count = COUNT_OF(play_buttons) - (guide_file[0] == '\0');   /* Guide is the last one */
	return play_buttons;
}
/* Slim strip along the bottom edge when the dialogue is shown on the bottom screen. */
#define STRIP_Y 208.0f
/* Bottom-screen choice buttons in the bottom-text layout: stacked in the area above
   the strip, as tall as fits (at most 40 px each) and centred vertically. */
static int choice_buttons(Button *out)
{
	int count = event.choice_count;
	if (count <= 0) return 0;
	const float top = 8.0f, bottom = STRIP_Y - 8.0f, gap = 4.0f;
	float h = fminf(40.0f, (bottom - top - gap * (float)(count - 1)) / (float)count);
	float y = top + ((bottom - top) - (h * (float)count + gap * (float)(count - 1))) / 2.0f;
	for (int index = 0; index < count; ++index, y += h + gap) {
		out[index] = (Button){8.0f, y, BOTTOM_W - 16.0f, h, event.choices[index]};
	}
	return count;
}

static const Button strip_buttons[] = {
	/* five equal buttons across the bottom screen (64 px apart on the 3DS) */
	{4, 208, (BOTTOM_W - 4) / 5 - 4, 28, "Menu"},
	{4 + (BOTTOM_W - 4) / 5, 208, (BOTTOM_W - 4) / 5 - 4, 28, "Log"},
	{4 + 2 * (BOTTOM_W - 4) / 5, 208, (BOTTOM_W - 4) / 5 - 4, 28, "Hide"},
	{4 + 3 * (BOTTOM_W - 4) / 5, 208, (BOTTOM_W - 4) / 5 - 4, 28, "Quit"},
	{4 + 4 * (BOTTOM_W - 4) / 5, 208, (BOTTOM_W - 4) / 5 - 4, 28, "Auto"},
};

static void draw_button_box(const Button *button, int selected)
{
	int skinned = ui_image(selected ? IMG_ITEM_SEL : IMG_ITEM) != NULL;
	draw_panel(selected ? IMG_ITEM_SEL : IMG_ITEM, button->x, button->y, button->w, button->h,
		selected ? theme_accent() : theme_item(), Z_BOX);
	draw_line(button->label, button->x + 12, button->y + button->h / 2.0f - 9.0f, 0.55f,
		selected && !skinned ? theme_dark_text() : theme_text(), Z_TEXT, button->w - 24);
}

static void draw_strip_button(const Button *button, int selected)
{
	draw_panel(selected ? IMG_ITEM_SEL : IMG_ITEM, button->x, button->y, button->w, button->h,
		selected ? theme_accent() : theme_item(), Z_BOX);
	float previous_scale = measure_scale;
	measure_scale = 0.45f;
	float width = measure_text(button->label, strlen(button->label), NULL);
	measure_scale = previous_scale;
	draw_line(button->label, button->x + (button->w - width) / 2.0f, button->y + 6.0f, 0.45f,
		selected && !ui_image(IMG_ITEM_SEL) ? theme_dark_text() : theme_text(), Z_TEXT, 0.0f);
}

/* Choices as touch buttons on the bottom screen (both touch layouts). */
static void draw_choice_buttons(void)
{
	Button buttons[GS_MAX_CHOICES];
	int count = choice_buttons(buttons);
	for (int index = 0; index < count; ++index) {
		const Button *b = &buttons[index];
		int selected = index == menu_cursor;
		int skinned = ui_image(selected ? IMG_CHOICE_SEL : IMG_CHOICE) != NULL;
		draw_panel(selected ? IMG_CHOICE_SEL : IMG_CHOICE, b->x, b->y, b->w, b->h,
			selected ? theme_accent() : theme_item(), Z_BOX + 0.01f);
		if (!selected) {
			/* Light tint and outline so the buttons read as tappable in any theme. */
			const u32 tint = C2D_Color32(255, 255, 255, 22), edge = C2D_Color32(255, 255, 255, 70);
			rect(b->x, b->y, b->w, b->h, tint, Z_BOX + 0.015f);
			rect(b->x, b->y, b->w, 1.0f, edge, Z_BOX + 0.016f);
			rect(b->x, b->y + b->h - 1.0f, b->w, 1.0f, edge, Z_BOX + 0.016f);
			rect(b->x, b->y, 1.0f, b->h, edge, Z_BOX + 0.016f);
			rect(b->x + b->w - 1.0f, b->y, 1.0f, b->h, edge, Z_BOX + 0.016f);
		}
		float scale = fminf(0.5f, b->h / 44.0f);
		draw_line(b->label, b->x + 10.0f, b->y + (b->h - 30.0f * scale) / 2.0f, scale,
			selected && !skinned ? theme_dark_text() : theme_text(), Z_TEXT, b->w - 20.0f);
	}
}

/* Bottom screen in the "Top, choices below" layout: a control panel above the strip. */
static void draw_bottom_controls(void)
{
	const float bh = STRIP_Y - 4.0f;
	rect(0, 0, BOTTOM_W, bh, theme_panel(235), Z_BOX);
	rect(0, bh - 2.0f, BOTTOM_W, 2.0f, theme_accent(), Z_BOX);
	const u32 dim = C2D_Color32(150, 170, 165, 255);
	if (waiting == PLAY_MENU && !ui_hidden) {
		draw_choice_buttons();
		return;
	}
	draw_line(config.title[0] != '\0' ? config.title : game_name, 12, 10, 0.55f, theme_accent(), Z_TEXT, BOTTOM_W - 24);
	{
		char status[48];
		status_text(status, sizeof(status));
		draw_line(status, 12, 106, 0.42f, theme_text(), Z_TEXT, 0.0f);
	}
	draw_line(ui_hidden ? "Text hidden - tap or press A" : "Tap here or press A: next line",
		12, 44, 0.45f, theme_text(), Z_TEXT, BOTTOM_W - 24);
	draw_line("Hold R: fast-forward      Y: auto", 12, 66, 0.42f, dim, Z_TEXT, BOTTOM_W - 24);
	draw_line("Choices appear here as buttons", 12, 86, 0.42f, dim, Z_TEXT, BOTTOM_W - 24);
	if (notice[0] != '\0') {
		draw_line(notice, 12, bh - 24.0f, 0.42f, C2D_Color32(255, 200, 120, 255), Z_TEXT, BOTTOM_W - 24);
	}
}

/* Dialogue panel filling the bottom screen above the button strip (bottom-text layout). */
static void draw_bottom_dialogue(void)
{
	GameConfig defaults;
	config_defaults(&defaults);
	int custom = display_options_use_custom(&display_options, current_textbox_available());
	const int *n = custom ? config.num : defaults.num;
	const float bw = BOTTOM_W, bh = STRIP_Y - 4.0f;
	u32 fill = custom && config.has_panel ? rgb_color(config.panel, 235) : C2D_Color32(8, 14, 20, 235);
	rect(0, 0, bw, bh, fill, Z_BOX);
	rect(0, bh - 2.0f, bw, 2.0f, custom ? theme_accent() : COLOR_ACCENT, Z_BOX);
	const u32 dim = C2D_Color32(150, 170, 165, 255);
	if (ui_hidden) {
		draw_line("Text hidden - press B or A", 12, 12, 0.45f, dim, Z_TEXT, 0.0f);
		return;
	}
	if (waiting == PLAY_MENU) {
		draw_choice_buttons();
		(void)dim;
		return;
	}
	if (waiting != PLAY_TEXT) {
		return;
	}
	float face_inset = 0.0f;
	for (int index = 0; index < MAX_SPRITES; ++index) {
		const Sprite *sprite = &scene.sprites[index];
		if (!sprite->active || strcmp(sprite->pos, "face") != 0 || sprite->alpha <= 0.0f) continue;
		Picture *picture = get_picture("sprites", sprite->asset);
		if (picture == NULL) continue;
		float scale = fminf(1.0f, fminf(76.0f / (float)picture->width, 96.0f / (float)picture->height));
		C2D_ImageTint tint;
		C2D_AlphaImageTint(&tint, sprite->alpha);
		C2D_DrawImageAt(picture->image, 8.0f, 8.0f, Z_BOX + 0.03f, &tint, scale, scale);
		face_inset = fmaxf(face_inset, (float)picture->width * scale + 16.0f);
	}
	int named = event.speaker[0] != '\0';
	float text_x = fmaxf(12.0f, face_inset);
	if (named) {
		draw_line(event.speaker, text_x, 8.0f, (float)n[NUM_NAME_SIZE] / 100.0f * 1.1f,
			event.color != 0 || custom ? speaker_color(event.color) : COLOR_ACCENT, Z_TEXT, 0.0f);
	}
	float text_y = named ? 34.0f : 12.0f;
	float width = bw - text_x - 12.0f;
	float scale = (float)n[NUM_TEXT_SIZE] / 100.0f * 1.15f;
	u32 color = custom && config.has_text ? theme_text() : C2D_Color32(235, 235, 235, 255);
	char shown[GS_TEXT_LEN + 64];
	size_t length = utf8_prefix(event.text, (size_t)chars_shown);
	const char *full = event.text;
	size_t visible = length;
	while (*full == '\n' || *full == '\r') {
		++full;
		if (visible > 0) --visible;
	}
	scale = fit_text_scale(event.text, width, bh - text_y - 6.0f, scale);
#ifdef VN_SDL
	/* lay out the whole line once; draw_text_reveal shows the typed part smoothly */
	reveal_with_breaks(full, strlen(full), width, scale, shown, sizeof(shown));
	float reveal = chars_shown - (float)(full - event.text);
	(void)visible;
#else
	reveal_with_breaks(full, visible, width, scale, shown, sizeof(shown));
#endif
	const char *visible_text = shown;
	Picture *advance = advance_available ? ui_image(IMG_ADVANCE) : NULL;
	float draw_width = width * REWRAP_SLACK;   /* see draw_textbox */
#ifdef VN_SDL
	if (advance != NULL && event.text[length] == '\0') {
		draw_dialogue_with_advance(visible_text, text_x, text_y, draw_width, scale, color, advance, 1);
	} else {
		draw_text_reveal(visible_text, text_x, text_y, scale, color, reveal);
	}
#else
	if (advance != NULL) {
		draw_dialogue_with_advance(visible_text, text_x, text_y, draw_width, scale, color,
			advance, event.text[length] == '\0');
	} else {
		draw_wrapped(visible_text, text_x, text_y, draw_width, scale, color, Z_TEXT);
	}
#endif
}

static int list_first(int cursor, int total)
{
	int first = cursor - LIST_ROWS / 2;
	if (first > total - LIST_ROWS) {
		first = total - LIST_ROWS;
	}
	return first < 0 ? 0 : first;
}

static void draw_list_row(float y, const char *label, int selected)
{
	draw_panel(selected ? IMG_ITEM_SEL : IMG_ITEM, 40, y, TOP_W - 80, 22,
		selected ? theme_accent() : theme_panel(215), Z_OVERLAY);
	draw_line(label, 48, y + 3, 0.45f, selected && !ui_image(IMG_ITEM_SEL) ? theme_dark_text() : theme_text(),
		Z_OVERLAY_TEXT, TOP_W - 96);
}

/* "N   MM-DD HH:MM   preview" for a save slot, or "N   - empty -". */
static void slot_label(int slot, char *label, size_t size, size_t preview_chars)
{
	if (slots[slot].used) {
		char when[24] = "";
		time_t saved = (time_t)slots[slot].saved_at;
		struct tm *parts = localtime(&saved);
		if (parts != NULL) {
			strftime(when, sizeof(when), "%m-%d %H:%M", parts);
		}
		char preview[96];
		size_t length = utf8_prefix(slots[slot].preview, preview_chars);
		if (length >= sizeof(preview)) length = sizeof(preview) - 1;
		memcpy(preview, slots[slot].preview, length);
		preview[length] = '\0';
		snprintf(label, size, "%d   %s   %s", slot + 1, when, preview);
	} else {
		snprintf(label, size, "%d   - empty -", slot + 1);
	}
}

static void draw_slot_list(void)
{
	draw_backdrop(IMG_MENU_BG, 225);
	draw_line(panel == PANEL_SAVE ? "Save to which slot?" : "Load which slot?", 40, 6, 0.55f,
		theme_accent(), Z_OVERLAY_TEXT, TOP_W - 80);
	int first = list_first(panel_cursor, SAVE_SLOTS);
	for (int slot = first; slot < first + LIST_ROWS; ++slot) {
		char label[160];
		slot_label(slot, label, sizeof(label), 30);
		draw_list_row(30.0f + (float)(slot - first) * 25.0f, label, slot == panel_cursor);
	}
	char footer[48];
	snprintf(footer, sizeof(footer), "A select   B back   %d/%d", panel_cursor + 1, SAVE_SLOTS);
	draw_line(footer, 40, 224, 0.4f, C2D_Color32(150, 170, 165, 255), Z_OVERLAY_TEXT, 0.0f);
}

static int quit_choices(const char **labels, int *actions);
static void leave_game(void);

static void draw_quit_confirm(void)
{
	draw_backdrop(IMG_MENU_BG, 225);
	draw_line("Quit?", 70, 50, 0.6f, theme_accent(), Z_OVERLAY_TEXT, TOP_W - 140);
	draw_line("Unsaved progress will be lost.", 70, 76, 0.45f, theme_text(), Z_OVERLAY_TEXT, TOP_W - 140);
	const char *labels[4];
	int actions[4];
	int count = quit_choices(labels, actions);
	for (int index = 0; index < count; ++index) {
		draw_list_row(104.0f + (float)index * 30.0f, labels[index], index == panel_cursor);
	}
	draw_line("D-pad choose   A confirm   B keep playing", 40, 224, 0.4f, C2D_Color32(150, 170, 165, 255),
		Z_OVERLAY_TEXT, 0.0f);
}

/* Menus as touch buttons on the bottom screen (title menu, in-game menu): `count` item
   rows under a header, plus one `extra` button (Game list / Close) at the bottom edge. */
#define LIST_EXTRA_Y 204.0f
static int touch_list_buttons(Button *out, const char *const *labels, int count, const char *extra)
{
	const float top = 34.0f, bottom = LIST_EXTRA_Y - 8.0f, gap = 5.0f;
	int columns = count > 6 ? 2 : 1;   /* long menus in two columns, so the buttons stay big */
	int rows = (count + columns - 1) / columns;
	float h = rows > 0 ? fminf(34.0f, (bottom - top - gap * (float)(rows - 1)) / (float)rows) : 0.0f;
	float w = (BOTTOM_W - 40.0f - gap * (float)(columns - 1)) / (float)columns;
	for (int index = 0; index < count; ++index) {
		int column = index % columns, row = index / columns;
		out[index] = (Button){20.0f + (float)column * (w + gap), top + (float)row * (h + gap), w, h, labels[index]};
	}
	out[count] = (Button){20.0f, LIST_EXTRA_Y, BOTTOM_W - 40.0f, 30.0f, extra};
	return count + 1;
}

static void draw_touch_list(const char *header, const char *const *labels, int count, int cursor,
	const char *extra)
{
	Button buttons[LABEL_COUNT + 2];
	int total = touch_list_buttons(buttons, labels, count, extra);
	draw_line(header, 20, 6, 0.6f, theme_accent(), Z_TEXT, BOTTOM_W - 40);
	for (int index = 0; index < total; ++index) {
		const Button *b = &buttons[index];
		int selected = index == cursor;
		draw_panel(selected ? IMG_ITEM_SEL : IMG_ITEM, b->x, b->y, b->w, b->h,
			selected ? theme_accent() : theme_item(), Z_BOX);
		if (!selected) {
			const u32 tint = C2D_Color32(255, 255, 255, 22), edge = C2D_Color32(255, 255, 255, 70);
			rect(b->x, b->y, b->w, b->h, tint, Z_BOX + 0.005f);
			rect(b->x, b->y, b->w, 1.0f, edge, Z_BOX + 0.006f);
			rect(b->x, b->y + b->h - 1.0f, b->w, 1.0f, edge, Z_BOX + 0.006f);
		}
		float scale = fminf(0.55f, b->h / 42.0f);
		draw_line(b->label, b->x + 12.0f, b->y + (b->h - 30.0f * scale) / 2.0f, scale,
			selected && !ui_image(IMG_ITEM_SEL) ? theme_dark_text() : theme_text(), Z_TEXT, b->w - 24.0f);
	}
}

static const char *title_item_label(int item);

static int title_touch_labels(const char **labels)
{
	for (int index = 0; index < title_count; ++index) labels[index] = title_item_label(title_items[index]);
	return title_count;
}

#define SLOT_VISIBLE 6
static int slot_top;   /* first save slot shown on the bottom screen; follows the cursor */

static int slot_first(void)
{
	if (panel_cursor < slot_top) slot_top = panel_cursor;
	if (panel_cursor >= slot_top + SLOT_VISIBLE) slot_top = panel_cursor - SLOT_VISIBLE + 1;
	if (slot_top > SAVE_SLOTS - SLOT_VISIBLE) slot_top = SAVE_SLOTS - SLOT_VISIBLE;
	if (slot_top < 0) slot_top = 0;
	return slot_top;
}

/* Bottom-screen buttons for the save slots in view. */
static int slot_touch_labels(const char **labels)
{
	static char rows[SLOT_VISIBLE][160];
	int first = slot_first();
	for (int i = 0; i < SLOT_VISIBLE; ++i) {
		slot_label(first + i, rows[i], sizeof(rows[i]), 18);
		labels[i] = rows[i];
	}
	return SLOT_VISIBLE;
}

/* A thin scrollbar along the right edge of a bottom-screen touch list. */
static void draw_touch_scrollbar(int first, int visible, int total)
{
	if (total <= visible) return;
	const float top = 34.0f, height = LIST_EXTRA_Y - 8.0f - top;
	rect(BOTTOM_W - 12.0f, top, 3.0f, height, C2D_Color32(255, 255, 255, 40), Z_TEXT);
	rect(BOTTOM_W - 12.0f, top + height * (float)first / (float)total, 3.0f,
		height * (float)visible / (float)total, theme_accent(), Z_TEXT + 0.001f);
}

/* Bottom-screen buttons for the Options rows in view (same window as the top screen). */
static int option_touch_labels(const char **labels)
{
	static char rows[OPTION_VISIBLE][64];
	const char *names[OPTION_ROWS], *values[OPTION_ROWS];
	option_texts(names, values);
	int first = option_first();
	for (int i = 0; i < OPTION_VISIBLE; ++i) {
		int row = first + i;
		if (volume_field(&display_options, row) != NULL) {
			snprintf(rows[i], sizeof(rows[i]), "%s:  -  %s  +", names[row], values[row]);
		} else {
			snprintf(rows[i], sizeof(rows[i]), "%s:  %s", names[row], values[row]);
		}
		labels[i] = rows[i];
	}
	return OPTION_VISIBLE;
}

/* The in-game Quit choices: the game list, and on the Wii U also the Homebrew Launcher and
   the Wii U Menu (as START offers); elsewhere closing the app. */
enum { QUIT_LIST, QUIT_HBL, QUIT_SYSTEM, QUIT_APP };
static int quit_app;   /* set when a Quit choice closes the app; the main loop ends */

static int quit_choices(const char **labels, int *actions)
{
	int count = 0;
	labels[count] = "Quit to the game list"; actions[count++] = QUIT_LIST;
#ifdef EXIT_MENU
	if (vn_wiiu_can_exit_to_hbl()) { labels[count] = "Quit to the Homebrew Launcher"; actions[count++] = QUIT_HBL; }
	labels[count] = "Quit to the Wii U Menu"; actions[count++] = QUIT_SYSTEM;
#else
	labels[count] = "Close 3DVN"; actions[count++] = QUIT_APP;
#endif
	return count;
}

static void choose_quit(int action)
{
	if (action == QUIT_LIST) {
		leave_game();
		return;
	}
	read_save();
	playtime_save();
#ifdef EXIT_MENU
	if (choose_exit(action == QUIT_HBL ? EXIT_HBL : EXIT_WIIU_MENU)) quit_app = 1;
#else
	quit_app = 1;
#endif
}

static int menu_touch_labels(const char **labels)
{
	for (int index = 0; index < menu_count; ++index) labels[index] = config.labels[menu_items[index]];
	return menu_count;
}

/* A straight stroke `thickness` wide (two triangles). */
static void draw_stroke(float x0, float y0, float x1, float y1, float thickness, u32 color, float z)
{
	float dx = x1 - x0, dy = y1 - y0, length = sqrtf(dx * dx + dy * dy);
	if (length <= 0.0f) return;
	float nx = -dy / length * thickness * 0.5f, ny = dx / length * thickness * 0.5f;
	C2D_DrawTriangle(x0 + nx, y0 + ny, color, x1 + nx, y1 + ny, color, x1 - nx, y1 - ny, color, z);
	C2D_DrawTriangle(x0 + nx, y0 + ny, color, x1 - nx, y1 - ny, color, x0 - nx, y0 - ny, color, z);
}

/* Endings checklist; the same list on both screens (`width` wide from `x`). */
#define ENDING_ROWS 8
static void draw_endings_list(float x, float width, int touch)
{
	const u32 dim = C2D_Color32(150, 170, 165, 255);
	int seen = 0;
	for (int index = 0; index < ending_count; ++index) seen += ending_seen(&endings[index]);
	draw_line(config.labels[LABEL_ENDINGS], x + 12, 6, 0.6f, theme_accent(), Z_OVERLAY_TEXT, width - 110);
	char count[32];
	snprintf(count, sizeof(count), "%d / %d", seen, ending_count);
	draw_line(count, x + width - 70, 10, 0.45f, theme_text(), Z_OVERLAY_TEXT, 0.0f);
	for (int row = 0; row < ENDING_ROWS && ending_top + row < ending_count; ++row) {
		const Ending *ending = &endings[ending_top + row];
		int done = ending_seen(ending);
		float y = 36.0f + (float)row * 20.0f;
		/* a check box: outline, filled with a tick when seen */
		const u32 edge = done ? theme_accent() : dim;
		rect(x + 14, y + 2, 13, 13, edge, Z_OVERLAY);
		rect(x + 16, y + 4, 9, 9, done ? theme_accent() : C2D_Color32(12, 18, 22, 255), Z_OVERLAY + 0.01f);
		if (done) {
			const u32 tick = C2D_Color32(12, 18, 22, 255);
			draw_stroke(x + 16.5f, y + 8.5f, x + 19.5f, y + 11.5f, 1.9f, tick, Z_OVERLAY + 0.02f);
			draw_stroke(x + 19.5f, y + 11.5f, x + 25.0f, y + 4.5f, 1.9f, tick, Z_OVERLAY + 0.02f);
		}
		draw_line(ending->name, x + 34, y, 0.46f, done ? theme_text() : dim, Z_OVERLAY_TEXT, width - 56);
	}
	if (ending_count > ENDING_ROWS) {
		char more[48];
		snprintf(more, sizeof(more), "%d-%d of %d   D-pad / swipe to scroll", ending_top + 1,
			ending_top + ENDING_ROWS < ending_count ? ending_top + ENDING_ROWS : ending_count, ending_count);
		draw_line(more, x + 14, 36.0f + ENDING_ROWS * 20.0f + 2.0f, 0.38f, dim, Z_OVERLAY_TEXT, width - 28);
	}
	if (touch) {
		draw_panel(IMG_ITEM, 20, LIST_EXTRA_Y, BOTTOM_W - 40, 30, theme_item(), Z_BOX);
		draw_line("Back", 32, LIST_EXTRA_Y + 6, 0.5f, theme_text(), Z_TEXT, 0.0f);
	} else {
		draw_line("B back", x + 14, 224, 0.4f, dim, Z_OVERLAY_TEXT, 0.0f);
	}
}

static void draw_endings_top(void)
{
	draw_backdrop(IMG_MENU_BG, 235);
	rect(SCENE_X + 20.0f, 0, 360.0f, TOP_H, theme_panel(225), Z_OVERLAY - 0.01f);   /* calm behind the list */
	draw_endings_list(SCENE_X + 30.0f, 340.0f, 0);
}

static void draw_guide(void)
{
	const u32 dim = C2D_Color32(150, 170, 165, 255);
	const float line_h = GUIDE_LINE_H;
	int first = (int)(guide_scroll / line_h);
	float y = GUIDE_TOP + 2.0f - (guide_scroll - (float)first * line_h);
	for (int index = first; index < guide_lines && y < GUIDE_BOTTOM; ++index, y += line_h) {
		int length = guide_ends[index] - guide_starts[index];
		if (length <= 0) continue;
		char line[512];
		if (length >= (int)sizeof(line)) length = (int)sizeof(line) - 1;
		memcpy(line, guide_text + guide_starts[index], (size_t)length);
		line[length] = '\0';
		draw_line(line, 14, y, GUIDE_SCALE, theme_text(), Z_TEXT, 0.0f);
	}
	/* bars over the text that scrolls under them */
	u32 bar = config.has_bg ? rgb_color(config.bg, 255) : C2D_Color32(17, 29, 34, 255);
	rect(0, 0, BOTTOM_W, GUIDE_TOP, bar, Z_TEXT + 0.02f);
	rect(0, GUIDE_BOTTOM, BOTTOM_W, TOP_H - GUIDE_BOTTOM, bar, Z_TEXT + 0.02f);
	rect(0, GUIDE_TOP - 1.0f, BOTTOM_W, 1.0f, theme_accent(), Z_TEXT + 0.03f);
	draw_line(config.labels[LABEL_GUIDE], 14, 3, 0.45f, theme_accent(), Z_TEXT + 0.04f, 0.0f);
	char where[32];
	float max = guide_max_scroll();
	snprintf(where, sizeof(where), "%d%%", max > 0.0f ? (int)(guide_scroll * 100.0f / max + 0.5f) : 100);
	draw_line(where, BOTTOM_W - 50, 4, 0.42f, dim, Z_TEXT + 0.04f, 0.0f);
	/* scroll bar */
	float track = GUIDE_BOTTOM - GUIDE_TOP - 4.0f;
	float total = (float)guide_lines * line_h;
	if (total > track) {
		float thumb = fmaxf(12.0f, track * track / total);
		float at = GUIDE_TOP + 2.0f + (track - thumb) * (max > 0.0f ? guide_scroll / max : 0.0f);
		rect(BOTTOM_W - 6, at, 3, thumb, dim, Z_TEXT + 0.03f);
	}
	draw_line("Swipe or D-pad to scroll   B back", 14, GUIDE_BOTTOM + 12, 0.38f, dim, Z_TEXT + 0.04f, 0.0f);
	draw_panel(IMG_ITEM, BOTTOM_W - 86, GUIDE_BOTTOM + 5, 72, 26, theme_item(), Z_TEXT + 0.03f);
	draw_line("Back", BOTTOM_W - 72, GUIDE_BOTTOM + 9, 0.45f, theme_text(), Z_TEXT + 0.04f, 0.0f);
}

static void draw_guide_top(void)
{
	rect(SCENE_X, 0, 400, 17, theme_panel(255), Z_HEADER);
	draw_line("Guide open on the touch screen", SCENE_X + 8, 1, 0.42f, theme_accent(), Z_HEADER_TEXT, 0.0f);
}

static void close_guide(void)
{
	guide_save_position();
	guide_free();
}

static void guide_input(u32 down, u32 repeat)
{
	const float line_h = GUIDE_LINE_H, page = GUIDE_BOTTOM - GUIDE_TOP - line_h;
	u32 held = hidKeysHeld();
	touchPosition touch;
	if (down & (KEY_B | KEY_X)) {
		close_guide();
		close_panel();
		return;
	}
	if (repeat & KEY_DUP) { guide_scroll -= line_h * 2.0f; guide_velocity = 0.0f; }
	if (repeat & KEY_DDOWN) { guide_scroll += line_h * 2.0f; guide_velocity = 0.0f; }
	if (repeat & KEY_DLEFT) { guide_scroll -= page; guide_velocity = 0.0f; }
	if (repeat & KEY_DRIGHT) { guide_scroll += page; guide_velocity = 0.0f; }
	if (down & KEY_TOUCH) {
		hidTouchRead(&touch);
		if (touch.py >= GUIDE_BOTTOM && touch.px >= BOTTOM_W - 90) {
			close_guide();
			close_panel();
			return;
		}
		guide_drag = 1;
		guide_drag_moved = 0;
		guide_drag_y = touch.py;
		guide_velocity = 0.0f;
	} else if (guide_drag && (held & KEY_TOUCH)) {
		hidTouchRead(&touch);
		float delta = (float)touch.py - guide_drag_y;
		guide_scroll -= delta;            /* the text follows the finger */
		guide_velocity = guide_velocity * 0.5f - delta * 0.5f;
		guide_drag_y = touch.py;
	} else {
		guide_drag = 0;
		guide_scroll += guide_velocity;   /* keeps gliding after a swipe, slowing down */
		guide_velocity *= 0.92f;
		if (fabsf(guide_velocity) < 0.3f) guide_velocity = 0.0f;
	}
	float max = guide_max_scroll();
	if (guide_scroll < 0.0f) { guide_scroll = 0.0f; guide_velocity = 0.0f; }
	if (guide_scroll > max) { guide_scroll = max; guide_velocity = 0.0f; }
}

static int ending_drag;
static float ending_drag_y;

static void endings_input(u32 down, u32 repeat)
{
	int last_top = ending_count > ENDING_ROWS ? ending_count - ENDING_ROWS : 0;
	touchPosition touch;
	if (down & (KEY_B | KEY_X)) {
		close_panel();
		return;
	}
	if ((repeat & KEY_DUP) && ending_top > 0) --ending_top;
	if ((repeat & KEY_DDOWN) && ending_top < last_top) ++ending_top;
	if (down & KEY_TOUCH) {
		hidTouchRead(&touch);
		if (touch.py >= LIST_EXTRA_Y) {
			close_panel();
			return;
		}
		ending_drag = 1;
		ending_drag_y = touch.py;
	} else if (ending_drag && (hidKeysHeld() & KEY_TOUCH)) {
		hidTouchRead(&touch);
		while ((float)touch.py - ending_drag_y >= 20.0f && ending_top > 0) { --ending_top; ending_drag_y += 20.0f; }
		while ((float)touch.py - ending_drag_y <= -20.0f && ending_top < last_top) { ++ending_top; ending_drag_y -= 20.0f; }
	} else {
		ending_drag = 0;
	}
}

/* ---- Save sync with a game-sync FTP server (sync-server/) ----
   Settings in SETTINGS_DIR/sync.cfg ("address 192.168.0.10:2121", optional "user ...",
   "password ...", "folder /3dvn"). For the game being played: each save slot is
   compared with the server's copy (the newer one by the time it was saved), then the
   player picks Pull (server -> here), Push (here -> server) or Newest of each. Endings
   and unlocks (.3vglobal) and read lines (.3vread) are merged both ways. Saves are
   stored in the shared byte order; older Wii U saves are converted on the way. Every
   replaced file keeps a .bak copy (here and on the server). */
enum { SYNC_ADDRESS, SYNC_WORKING, SYNC_REVIEW, SYNC_DONE };
enum { SYNC_SAME, SYNC_LOCAL_NEWER, SYNC_REMOTE_NEWER, SYNC_LOCAL_ONLY, SYNC_REMOTE_ONLY };
enum { WORK_NONE, WORK_SCAN, WORK_PULL, WORK_PUSH, WORK_NEWEST };
#define SYNC_KEYS 15

typedef struct {
	int slot, status;
	long long local_time, remote_time;
	unsigned char *remote;
	size_t remote_size;
} SyncItem;

static int sync_step, sync_cursor, sync_work, sync_wait;
static char sync_address[64], sync_user[48], sync_password[64], sync_folder[64] = "/3dvn";
static char sync_message[220];
static SyncItem sync_items[SAVE_SLOTS];
static int sync_item_count, sync_differ;
static const char *const sync_keys[SYNC_KEYS] = {"1", "2", "3", "Delete", "4", "5", "6", ".", "7", "8", "9", ":",
	"Back", "0", "Connect"};

static void sync_config_path(char *path, size_t size)
{
	snprintf(path, size, SETTINGS_DIR "/sync.cfg");
}

static void sync_load_config(void)
{
	char path[200], line[160];
	sync_config_path(path, sizeof(path));
	FILE *file = fopen(path, "r");
	if (file == NULL) return;
	while (fgets(line, sizeof(line), file) != NULL) {
		char key[32], value[128];
		if (sscanf(line, "%31s %127s", key, value) != 2) continue;
		if (!strcmp(key, "address")) snprintf(sync_address, sizeof(sync_address), "%s", value);
		else if (!strcmp(key, "user")) snprintf(sync_user, sizeof(sync_user), "%s", value);
		else if (!strcmp(key, "password")) snprintf(sync_password, sizeof(sync_password), "%s", value);
		else if (!strcmp(key, "folder")) snprintf(sync_folder, sizeof(sync_folder), "%s", value);
	}
	fclose(file);
}

static void sync_save_config(void)
{
	char path[200];
	mkdir(ROOT_DIR, 0777);
	mkdir(SETTINGS_DIR, 0777);
	sync_config_path(path, sizeof(path));
	FILE *file = fopen(path, "w");
	if (file == NULL) return;
	fprintf(file, "address %s\n", sync_address);
	if (sync_user[0]) fprintf(file, "user %s\n", sync_user);
	if (sync_password[0]) fprintf(file, "password %s\n", sync_password);
	if (strcmp(sync_folder, "/3dvn")) fprintf(file, "folder %s\n", sync_folder);
	fclose(file);
}

static void sync_free_items(void)
{
	for (int i = 0; i < sync_item_count; ++i) free(sync_items[i].remote);
	memset(sync_items, 0, sizeof(sync_items));
	sync_item_count = 0;
}

static void sync_open(void)
{
	sync_free_items();
	sync_load_config();
	sync_step = SYNC_ADDRESS;
	sync_cursor = 14;   /* Connect */
	sync_work = WORK_NONE;
	sync_message[0] = '\0';
}

static unsigned char *read_local(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	if (file == NULL) return NULL;
	fseek(file, 0, SEEK_END);
	long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	unsigned char *data = length >= 0 && length < (4L << 20) ? malloc((size_t)length + 1) : NULL;
	if (data != NULL && fread(data, 1, (size_t)length, file) != (size_t)length) {
		free(data);
		data = NULL;
	}
	fclose(file);
	if (data != NULL) {
		data[length] = '\0';
		*size = (size_t)length;
	}
	return data;
}

/* Writes a file, keeping the old one as .bak. */
static int write_local(const char *path, const unsigned char *data, size_t size)
{
	char temporary[230], backup[230];
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	snprintf(backup, sizeof(backup), "%s.bak", path);
	mkdir(ROOT_DIR, 0777);   /* a console that never saved has no folders yet */
	mkdir(SAVES_DIR, 0777);
	mkdir(SETTINGS_DIR, 0777);
	FILE *file = fopen(temporary, "wb");
	if (file == NULL) return -1;
	int ok = fwrite(data, 1, size, file) == size;
	ok = fclose(file) == 0 && ok;
	if (!ok) {
		remove(temporary);
		return -1;
	}
	remove(backup);
	rename(path, backup);
	return rename(temporary, path) == 0 ? 0 : -1;
}

/* When a save was made, read in either byte order; 0 if the data is not a save. */
static long long save_data_time(const unsigned char *data, size_t size)
{
	SaveHeader header;
	return data != NULL && save_peek(data, size, &header) ? header.saved_at : 0;
}

/* A save in the shared format (the byte order every console reads); other data as is. */
static unsigned char *portable_save(const unsigned char *data, size_t size)
{
	unsigned char *out = malloc(size ? size : 1);
	if (out == NULL) return NULL;
	memcpy(out, data, size);
	SaveData *save = size >= SAVE_FILE_SIZE ? malloc(sizeof(*save)) : NULL;
	if (save == NULL) return out;
	if (save_decode(data, size, save)) save_encode(save, out);   /* rest of the file unchanged */
	free(save);
	return out;
}

/* Endings and unlocks: every flag keeps its highest value from either side. */
static char *merge_globals_text(const char *a, const char *b)
{
	enum { MAX_FLAGS = 256 };
	static char names[MAX_FLAGS][GS_NAME_LEN];
	static long values[MAX_FLAGS];
	int count = 0;
	const char *texts[2] = {a, b};
	for (int t = 0; t < 2; ++t) {
		const char *p = texts[t];
		while (p != NULL && *p) {
			char name[GS_NAME_LEN];
			long value;
			if (sscanf(p, "%31s %ld", name, &value) == 2 && is_global_name(name)) {
				int found = -1;
				for (int i = 0; i < count && found < 0; ++i) if (!strcmp(names[i], name)) found = i;
				if (found >= 0) {
					if (value > values[found]) values[found] = value;
				} else if (count < MAX_FLAGS) {
					snprintf(names[count], GS_NAME_LEN, "%s", name);
					values[count++] = value;
				}
			}
			p = strchr(p, '\n');
			if (p) ++p;
		}
	}
	char *out = malloc((size_t)count * (GS_NAME_LEN + 24) + 1);
	if (out == NULL) return NULL;
	size_t used = 0;
	out[0] = '\0';
	for (int i = 0; i < count; ++i) used += (size_t)sprintf(out + used, "%s %ld\n", names[i], values[i]);
	return out;
}

/* Read lines: records [length][name][u32 size, little-endian][bits]; bits OR-ed. */
static unsigned char *merge_read_marks(const unsigned char *a, size_t a_size, const unsigned char *b,
	size_t b_size, size_t *out_size)
{
	enum { MAX_MAPS = 512 };
	static const unsigned char *names[MAX_MAPS], *bits[MAX_MAPS][2];
	static unsigned name_len[MAX_MAPS], bit_len[MAX_MAPS][2];
	int count = 0;
	const unsigned char *data[2] = {a, b};
	size_t sizes[2] = {a_size, b_size};
	memset(bits, 0, sizeof(bits));
	memset(bit_len, 0, sizeof(bit_len));
	for (int side = 0; side < 2; ++side) {
		size_t pos = 0;
		while (data[side] != NULL && pos + 5 <= sizes[side]) {
			unsigned length = data[side][pos];
			if (pos + 1 + length + 4 > sizes[side]) break;
			const unsigned char *name = data[side] + pos + 1, *s = name + length;
			unsigned size = s[0] | s[1] << 8 | s[2] << 16 | (unsigned)s[3] << 24;
			if (pos + 5 + length + size > sizes[side]) break;
			int found = -1;
			for (int i = 0; i < count && found < 0; ++i) {
				if (name_len[i] == length && !memcmp(names[i], name, length)) found = i;
			}
			if (found < 0 && count < MAX_MAPS) {
				found = count++;
				names[found] = name;
				name_len[found] = length;
			}
			if (found >= 0) {
				bits[found][side] = s + 4;
				bit_len[found][side] = size;
			}
			pos += 5 + length + size;
		}
	}
	size_t total = 0;
	for (int i = 0; i < count; ++i) {
		unsigned size = bit_len[i][0] > bit_len[i][1] ? bit_len[i][0] : bit_len[i][1];
		total += 5 + name_len[i] + size;
	}
	unsigned char *out = malloc(total ? total : 1);
	if (out == NULL) return NULL;
	size_t used = 0;
	for (int i = 0; i < count; ++i) {
		unsigned size = bit_len[i][0] > bit_len[i][1] ? bit_len[i][0] : bit_len[i][1];
		out[used++] = (unsigned char)name_len[i];
		memcpy(out + used, names[i], name_len[i]);
		used += name_len[i];
		out[used++] = (unsigned char)size;
		out[used++] = (unsigned char)(size >> 8);
		out[used++] = (unsigned char)(size >> 16);
		out[used++] = (unsigned char)(size >> 24);
		for (unsigned k = 0; k < size; ++k) {
			unsigned char x = k < bit_len[i][0] ? bits[i][0][k] : 0, y = k < bit_len[i][1] ? bits[i][1][k] : 0;
			out[used++] = x | y;
		}
	}
	*out_size = used;
	return out;
}

static int sync_connect(FtpConn *ftp)
{
	char host[64];
	int port = 2121;
	snprintf(host, sizeof(host), "%s", sync_address);
	char *colon = strchr(host, ':');
	if (colon != NULL) {
		*colon = '\0';
		port = atoi(colon + 1);
	}
	if (host[0] == '\0' || port <= 0) {
		snprintf(sync_message, sizeof(sync_message), "Enter the server's address, e.g. 192.168.0.10:2121");
		return -1;
	}
	if (ftp_net_init() != 0) {
		snprintf(sync_message, sizeof(sync_message), "Couldn't start the network. Is Wi-Fi on?");
		return -1;
	}
	if (ftp_connect(ftp, host, port, sync_user, sync_password) != 0) {
		snprintf(sync_message, sizeof(sync_message), "%s: %s", sync_address, ftp->error);
		ftp_net_exit();
		return -1;
	}
	return 0;
}

static void sync_remote_path(char *path, size_t size, const char *folder, const char *name)
{
	snprintf(path, size, "%s/%s/%s", sync_folder, folder, name);
}

static void slot_file_name(int slot, char *name, size_t size)
{
	snprintf(name, size, "%s_%d" SAVE_EXT, game_name, slot + 1);
}

/* Whether "NAME\n" is one of the lines of a listing. */
static int in_listing(const char *listing, const char *line)
{
	size_t length = strlen(line);
	for (const char *p = listing; (p = strstr(p, line)) != NULL; p += length) {
		if (p == listing || p[-1] == '\n') return 1;
	}
	return 0;
}

/* Compares every slot with the server; fills sync_items. */
static void sync_scan(void)
{
	FtpConn ftp;
	sync_free_items();
	if (sync_connect(&ftp) != 0) {
		sync_step = SYNC_ADDRESS;
		return;
	}
	char folder[128];
	snprintf(folder, sizeof(folder), "%s/saves", sync_folder);
	static char listing[16384];
	if (ftp_list(&ftp, folder, listing, sizeof(listing)) < 0) {
		snprintf(sync_message, sizeof(sync_message), "%s", ftp.error);
		ftp_close(&ftp);
		ftp_net_exit();
		sync_step = SYNC_ADDRESS;
		return;
	}
	sync_differ = 0;
	for (int slot = 0; slot < SAVE_SLOTS; ++slot) {
		char name[160], local_path[200], remote_path[260], wanted[170];
		slot_file_name(slot, name, sizeof(name));
		save_path(slot, local_path, sizeof(local_path));
		snprintf(wanted, sizeof(wanted), "%s\n", name);
		size_t local_size = 0;
		unsigned char *local = read_local(local_path, &local_size);
		int on_server = in_listing(listing, wanted);
		SyncItem item = {slot, SYNC_SAME, save_data_time(local, local_size), 0, NULL, 0};
		if (on_server) {
			sync_remote_path(remote_path, sizeof(remote_path), "saves", name);
			if (ftp_get(&ftp, remote_path, &item.remote, &item.remote_size) == 0) {
				item.remote_time = save_data_time(item.remote, item.remote_size);
			}
		}
		if (local == NULL && item.remote == NULL) continue;
		if (local == NULL) item.status = SYNC_REMOTE_ONLY;
		else if (item.remote == NULL) item.status = SYNC_LOCAL_ONLY;
		else if (local_size == item.remote_size && !memcmp(local, item.remote, local_size)) item.status = SYNC_SAME;
		else if (item.local_time == item.remote_time) {
			/* the same save in the other byte order (an older Wii U save) */
			unsigned char *a = portable_save(local, local_size), *b = portable_save(item.remote, item.remote_size);
			item.status = a && b && !memcmp(a, b, local_size) ? SYNC_SAME : SYNC_LOCAL_NEWER;
			free(a);
			free(b);
		} else {
			item.status = item.local_time > item.remote_time ? SYNC_LOCAL_NEWER : SYNC_REMOTE_NEWER;
		}
		free(local);
		if (item.status != SYNC_SAME) ++sync_differ;
		sync_items[sync_item_count++] = item;
	}
	ftp_close(&ftp);
	ftp_net_exit();
	sync_step = SYNC_REVIEW;
	/* suggest the direction the differences point to */
	int remote = 0, local = 0;
	for (int i = 0; i < sync_item_count; ++i) {
		int s = sync_items[i].status;
		remote += s == SYNC_REMOTE_NEWER || s == SYNC_REMOTE_ONLY;
		local += s == SYNC_LOCAL_NEWER || s == SYNC_LOCAL_ONLY;
	}
	sync_cursor = remote && !local ? 0 : local && !remote ? 1 : 2;
	sync_message[0] = '\0';
}

/* Merges a file that both sides add to (globals, read lines) and writes it both ways. */
static void sync_merge_file(FtpConn *ftp, const char *local_path, const char *folder, const char *name,
	int is_text, int *changed)
{
	char remote_path[260];
	sync_remote_path(remote_path, sizeof(remote_path), folder, name);
	size_t local_size = 0, remote_size = 0, merged_size = 0;
	unsigned char *local = read_local(local_path, &local_size), *remote = NULL;
	if (ftp_get(ftp, remote_path, &remote, &remote_size) != 0) remote = NULL;
	if (local == NULL && remote == NULL) return;
	unsigned char *merged;
	if (is_text) {
		merged = (unsigned char *)merge_globals_text(local ? (char *)local : "", remote ? (char *)remote : "");
		merged_size = merged ? strlen((char *)merged) : 0;
	} else {
		merged = merge_read_marks(local, local_size, remote, remote_size, &merged_size);
	}
	if (merged != NULL) {
		if (local == NULL || local_size != merged_size || memcmp(local, merged, merged_size)) {
			if (write_local(local_path, merged, merged_size) == 0) ++*changed;
		}
		if (remote == NULL || remote_size != merged_size || memcmp(remote, merged, merged_size)) {
			if (ftp_put(ftp, remote_path, merged, merged_size) == 0) ++*changed;
		}
	}
	free(local);
	free(remote);
	free(merged);
}

static void sync_apply(int work)
{
	FtpConn ftp;
	if (sync_connect(&ftp) != 0) {
		sync_step = SYNC_DONE;
		return;
	}
	char folder[128];
	ftp_mkdir(&ftp, sync_folder);
	snprintf(folder, sizeof(folder), "%s/saves", sync_folder);
	ftp_mkdir(&ftp, folder);
	snprintf(folder, sizeof(folder), "%s/settings", sync_folder);
	ftp_mkdir(&ftp, folder);
	int pulled = 0, pushed = 0, failed = 0, merged = 0;
	for (int i = 0; i < sync_item_count; ++i) {
		SyncItem *item = &sync_items[i];
		if (item->status == SYNC_SAME) continue;
		int remote_wins = item->status == SYNC_REMOTE_NEWER || item->status == SYNC_REMOTE_ONLY;
		int pull = work == WORK_PULL ? item->remote != NULL
			: work == WORK_PUSH ? 0 : remote_wins;
		int push = work == WORK_PUSH ? item->status != SYNC_REMOTE_ONLY
			: work == WORK_PULL ? 0 : !remote_wins;
		char name[160], local_path[200], remote_path[260];
		slot_file_name(item->slot, name, sizeof(name));
		save_path(item->slot, local_path, sizeof(local_path));
		sync_remote_path(remote_path, sizeof(remote_path), "saves", name);
		if (pull) {
			unsigned char *data = portable_save(item->remote, item->remote_size);
			if (data != NULL && write_local(local_path, data, item->remote_size) == 0) ++pulled;
			else ++failed;
			free(data);
		} else if (push) {
			size_t size = 0;
			unsigned char *local = read_local(local_path, &size);
			unsigned char *data = local ? portable_save(local, size) : NULL;
			if (data != NULL && ftp_put(&ftp, remote_path, data, size) == 0) ++pushed;
			else ++failed;
			free(local);
			free(data);
		}
	}
	/* endings / unlocks and read lines: merged both ways (flush ours first) */
	read_save();
	char local_path[220], name[160];
	globals_path(local_path, sizeof(local_path));
	snprintf(name, sizeof(name), "%s.3vglobal", game_name);
	sync_merge_file(&ftp, local_path, "saves", name, 1, &merged);
	read_path(local_path, sizeof(local_path));
	snprintf(name, sizeof(name), "%s.3vread", game_name);
	sync_merge_file(&ftp, local_path, "settings", name, 0, &merged);
	ftp_close(&ftp);
	ftp_net_exit();
	globals_load();
	read_load();
	refresh_slots();
	sync_free_items();
	if (failed) {
		snprintf(sync_message, sizeof(sync_message), "%d save(s) downloaded, %d uploaded, %d failed: %s",
			pulled, pushed, failed, ftp.error[0] ? ftp.error : "could not write");
	} else if (pulled + pushed == 0) {
		snprintf(sync_message, sizeof(sync_message), "Saves were already the same%s.",
			merged ? "; endings and read lines merged" : "");
	} else {
		snprintf(sync_message, sizeof(sync_message), "Done: %d save(s) downloaded, %d uploaded%s.",
			pulled, pushed, merged ? "; endings and read lines merged" : "");
	}
	sync_step = SYNC_DONE;
}

static void sync_start(int work, const char *message)
{
	sync_work = work;
	sync_wait = 2;   /* draw the "working" screen before blocking on the network */
	snprintf(sync_message, sizeof(sync_message), "%s", message);
	sync_step = SYNC_WORKING;
}

static int sync_review_labels(const char **labels, char text[3][48])
{
	int pull = 0, push = 0;
	for (int i = 0; i < sync_item_count; ++i) {
		int s = sync_items[i].status;
		pull += s != SYNC_SAME && sync_items[i].remote != NULL;
		push += s != SYNC_SAME && s != SYNC_REMOTE_ONLY;
	}
	snprintf(text[0], 48, "Pull from server (%d)", pull);
	snprintf(text[1], 48, "Push to server (%d)", push);
	snprintf(text[2], 48, "Newest of each");
	for (int i = 0; i < 3; ++i) labels[i] = text[i];
	return 3;
}

static void sync_keypad_buttons(Button *buttons)
{
	const float gap = 6.0f, top = 64.0f, h = 32.0f;
	const float w = (BOTTOM_W - 28.0f - 3 * gap) / 4.0f;
	for (int i = 0; i < SYNC_KEYS; ++i) {
		int row = i / 4, col = i % 4;
		buttons[i] = (Button){14.0f + col * (w + gap), top + row * (h + gap), i == 14 ? 2 * w + gap : w, h, sync_keys[i]};
	}
}

static void sync_key(int key)
{
	size_t length = strlen(sync_address);
	if (key == 3) {
		if (length > 0) sync_address[length - 1] = '\0';
	} else if (key == 12) {
		close_panel();
	} else if (key == 14) {
		sync_save_config();
		sync_start(WORK_SCAN, "Connecting to the server...");
	} else if (length + 1 < sizeof(sync_address) && length < 21) {
		sync_address[length] = sync_keys[key][0];
		sync_address[length + 1] = '\0';
	}
}

static void sync_input(u32 down, u32 repeat)
{
	touchPosition touch;
	if (sync_step == SYNC_WORKING) {
		if (sync_wait > 0 && --sync_wait == 0) {
			int work = sync_work;
			sync_work = WORK_NONE;
			if (work == WORK_SCAN) sync_scan();
			else sync_apply(work);
		}
		return;
	}
	if (sync_step == SYNC_ADDRESS) {
		Button buttons[SYNC_KEYS];
		sync_keypad_buttons(buttons);
		if (down & KEY_B) {
			if (sync_address[0]) sync_key(3);
			else close_panel();
			return;
		}
		if (down & KEY_START) return;
		if (repeat & KEY_DRIGHT) sync_cursor = sync_cursor == 13 ? 14 : (sync_cursor + 1) % SYNC_KEYS;
		if (repeat & KEY_DLEFT) sync_cursor = sync_cursor == 0 ? 14 : sync_cursor - 1;
		if (repeat & KEY_DDOWN) sync_cursor = sync_cursor + 4 < SYNC_KEYS ? sync_cursor + 4 : sync_cursor % 4;
		if (repeat & KEY_DUP) sync_cursor = sync_cursor >= 4 ? sync_cursor - 4 : (sync_cursor + 12 < SYNC_KEYS ? sync_cursor + 12 : 14);
		if (down & KEY_Y) sync_key(3);
		if (down & KEY_A) sync_key(sync_cursor);
		if (down & KEY_TOUCH) {
			hidTouchRead(&touch);
			int hit = hit_button(buttons, SYNC_KEYS, &touch);
			if (hit >= 0) {
				sync_cursor = hit;
				sync_key(hit);
			}
		}
		return;
	}
	if (sync_step == SYNC_REVIEW) {
		const char *labels[3];
		char text[3][48];
		int count = sync_review_labels(labels, text);
		if (down & KEY_B) {
			sync_free_items();
			close_panel();
			return;
		}
		if (repeat & KEY_DDOWN) sync_cursor = (sync_cursor + 1) % (count + 1);
		if (repeat & KEY_DUP) sync_cursor = (sync_cursor + count) % (count + 1);
		int chosen = -1;
		if (down & KEY_A) chosen = sync_cursor;
		if (down & KEY_TOUCH) {
			hidTouchRead(&touch);
			Button buttons[LABEL_COUNT + 2];
			chosen = hit_button(buttons, touch_list_buttons(buttons, labels, count, "Cancel"), &touch);
		}
		if (chosen == 0) sync_start(WORK_PULL, "Downloading from the server...");
		else if (chosen == 1) sync_start(WORK_PUSH, "Uploading to the server...");
		else if (chosen == 2) sync_start(WORK_NEWEST, "Syncing...");
		else if (chosen == 3) {
			sync_free_items();
			close_panel();
		}
		return;
	}
	if (down & (KEY_A | KEY_B | KEY_TOUCH)) close_panel();   /* SYNC_DONE */
}

static const char *sync_status_text(int status)
{
	switch (status) {
	case SYNC_LOCAL_NEWER: return "newer here";
	case SYNC_REMOTE_NEWER: return "newer on server";
	case SYNC_LOCAL_ONLY: return "only here";
	case SYNC_REMOTE_ONLY: return "only on server";
	default: return "same";
	}
}

static void format_time(long long when, char *out, size_t size)
{
	time_t t = (time_t)when;
	struct tm *parts = when ? localtime(&t) : NULL;
	if (parts == NULL) snprintf(out, size, "-");
	else strftime(out, size, "%m-%d %H:%M", parts);
}

static void draw_sync_top(void)
{
	const u32 dim = C2D_Color32(150, 170, 165, 255);
	draw_backdrop(IMG_MENU_BG, 235);
	float x = SCENE_X + 24.0f;
	draw_line("Sync saves", x, 8, 0.6f, theme_accent(), Z_OVERLAY_TEXT, 0.0f);
	if (sync_step == SYNC_ADDRESS) {
		char line[96];
		snprintf(line, sizeof(line), "Server: %s_", sync_address);
		draw_line(line, x, 44, 0.55f, theme_text(), Z_OVERLAY_TEXT, 352.0f);
		draw_wrapped("The game-sync FTP server on your network (see sync-server/README.md). "
			"Type its address on the touch screen; the port is 2121 unless you changed it.",
			x, 76, 352.0f, 0.42f, dim, Z_OVERLAY_TEXT);
		if (sync_message[0]) draw_wrapped(sync_message, x, 150, 352.0f, 0.44f, C2D_Color32(255, 170, 140, 255), Z_OVERLAY_TEXT);
		draw_line("D-pad + A or touch   Y delete   B back", x, 222, 0.4f, dim, Z_OVERLAY_TEXT, 0.0f);
		return;
	}
	if (sync_step == SYNC_REVIEW) {
		char line[128];
		snprintf(line, sizeof(line), "%s - %d slot(s) differ", sync_address, sync_differ);
		draw_line(line, x, 32, 0.42f, dim, Z_OVERLAY_TEXT, 352.0f);
		const float columns[4] = {0.0f, 40.0f, 130.0f, 220.0f};
		const char *heads[4] = {"Slot", "Here", "Server", ""};
		for (int c = 0; c < 3; ++c) draw_line(heads[c], x + columns[c], 50, 0.42f, theme_accent(), Z_OVERLAY_TEXT, 0.0f);
		int row = 0;
		for (int i = 0; i < sync_item_count && row < 9; ++i, ++row) {
			char cells[4][32];
			snprintf(cells[0], sizeof(cells[0]), "%d", sync_items[i].slot + 1);
			format_time(sync_items[i].local_time, cells[1], sizeof(cells[1]));
			format_time(sync_items[i].remote_time, cells[2], sizeof(cells[2]));
			snprintf(cells[3], sizeof(cells[3]), "%s", sync_status_text(sync_items[i].status));
			u32 color = sync_items[i].status == SYNC_SAME ? dim : theme_text();
			for (int c = 0; c < 4; ++c) draw_line(cells[c], x + columns[c], 66 + row * 15, 0.42f, color, Z_OVERLAY_TEXT, 0.0f);
		}
		if (sync_item_count == 0) draw_line("No saves here or on the server yet.", x, 70, 0.45f, theme_text(), Z_OVERLAY_TEXT, 0.0f);
		draw_wrapped("Pull: the server's saves replace yours. Push: yours replace the server's. Endings and "
			"read lines are merged either way. Replaced files are kept as .bak.",
			x, 204, 352.0f, 0.36f, dim, Z_OVERLAY_TEXT);
		return;
	}
	draw_wrapped(sync_message, x, 60, 352.0f, 0.5f, theme_text(), Z_OVERLAY_TEXT);
	if (sync_step == SYNC_DONE) draw_line("A / B close", x, 222, 0.4f, dim, Z_OVERLAY_TEXT, 0.0f);
}

static void draw_sync_bottom(void)
{
	const u32 dim = C2D_Color32(150, 170, 165, 255);
	if (sync_step == SYNC_ADDRESS) {
		draw_line("Server address", 14, 6, 0.55f, theme_accent(), Z_TEXT, 0.0f);
		rect(14, 30, BOTTOM_W - 28, 26, theme_item(), Z_BOX);
		char shown[80];
		snprintf(shown, sizeof(shown), "%s_", sync_address);
		draw_line(shown, 22, 34, 0.55f, theme_text(), Z_TEXT, BOTTOM_W - 44);
		Button buttons[SYNC_KEYS];
		sync_keypad_buttons(buttons);
		for (int i = 0; i < SYNC_KEYS; ++i) draw_button_box(&buttons[i], i == sync_cursor);
		return;
	}
	if (sync_step == SYNC_REVIEW) {
		const char *labels[3];
		char text[3][48];
		int count = sync_review_labels(labels, text);
		draw_touch_list("Sync saves", labels, count, sync_cursor, "Cancel");
		return;
	}
	draw_line("Sync saves", 14, 6, 0.55f, theme_accent(), Z_TEXT, 0.0f);
	draw_wrapped(sync_message, 14, 40, BOTTOM_W - 28, 0.45f, sync_step == SYNC_DONE ? theme_text() : dim, Z_TEXT);
	if (sync_step == SYNC_DONE) draw_line("Tap or press A to close", 14, 216, 0.42f, dim, Z_TEXT, 0.0f);
}

static void draw_bottom(C3D_RenderTarget *target)
{
	C2D_TargetClear(target, config.has_bg ? rgb_color(config.bg, 255) : C2D_Color32(17, 29, 34, 255));
	C2D_SceneBegin(target);
	C2D_TextBufClear(text_buf);
	const u32 dim = C2D_Color32(150, 170, 165, 255);
	const u32 white = C2D_Color32(235, 235, 235, 255);

	if (mode == MODE_SELECT) {
		draw_line("Choose a game", 14, 6, 0.55f, COLOR_ACCENT, Z_TEXT, 0.0f);
		if (game_count == 0) {
			draw_wrapped("No games found. Put each game in sd:/3ds/3dvn/games/NAME/ with a main.3vs script.",
				14, 40, BOTTOM_W - 28, 0.45f, dim, Z_TEXT);
		}
		int first = select_cursor >= SELECT_ROWS ? select_cursor - SELECT_ROWS + 1 : 0;
		for (int index = first; index < game_count && index < first + SELECT_ROWS; ++index) {
			rect(14, 30.0f + (float)(index - first) * 28.0f, BOTTOM_W - 28, 24,
				index == select_cursor ? COLOR_ACCENT : C2D_Color32(40, 62, 66, 255), Z_BOX);
			draw_line(game_titles[index][0] ? game_titles[index] : game_names[index], 22,
				33.0f + (float)(index - first) * 28.0f, 0.45f,
				index == select_cursor ? C2D_Color32(10, 16, 18, 255) : C2D_Color32(220, 230, 225, 255),
				Z_TEXT, BOTTOM_W - 44);
		}
		/* A tappable Exit button (the same as START) */
		draw_panel(IMG_ITEM, 14, SELECT_EXIT_Y, BOTTOM_W - 28, 30, C2D_Color32(40, 62, 66, 255), Z_BOX);
		draw_line("Exit", 26, SELECT_EXIT_Y + 6, 0.5f, C2D_Color32(220, 230, 225, 255), Z_TEXT, 0.0f);
		return;
	}
	if (mode == MODE_ERROR) {
		draw_line("Script error", 14, 6, 0.55f, C2D_Color32(255, 140, 120, 255), Z_TEXT, 0.0f);
		draw_wrapped(message, 14, 34, BOTTOM_W - 28, 0.45f, white, Z_TEXT);
		draw_line(extras_mode ? "B return to title" : "B back to list",
			14, 224, 0.4f, dim, Z_TEXT, 0.0f);
		return;
	}
	if (panel == PANEL_ENDINGS && (mode == MODE_TITLE || mode == MODE_PLAY)) {
		draw_endings_list(0.0f, BOTTOM_W, 1);
		return;
	}
	if (panel == PANEL_SYNC && (mode == MODE_TITLE || mode == MODE_PLAY)) {
		draw_sync_bottom();
		return;
	}
	if (panel == PANEL_GUIDE && (mode == MODE_TITLE || mode == MODE_PLAY)) {
		draw_guide();
		return;
	}
	if (panel == PANEL_OPTIONS && (mode == MODE_TITLE || mode == MODE_PLAY)) {
		const char *labels[OPTION_ROWS];
		int count = option_touch_labels(labels);
		/* The header doubles as the place for "Options saved" etc., so nothing overlaps the rows. */
		draw_touch_list(notice[0] != '\0' ? notice : "Options  (tap to change)", labels, count,
			panel_cursor - option_first(), "Back");
		draw_touch_scrollbar(option_first(), OPTION_VISIBLE, OPTION_ROWS);
		return;
	}
	if (panel == PANEL_QUIT && mode == MODE_PLAY) {
		const char *labels[4];
		int actions[4];
		int count = quit_choices(labels, actions);
		draw_touch_list("Quit? Unsaved progress is lost.", labels, count, panel_cursor, "No, keep playing");
		return;
	}
	if (mode == MODE_TITLE && panel == PANEL_LOAD) {
		const char *labels[SLOT_VISIBLE];
		int count = slot_touch_labels(labels);
		draw_touch_list(notice[0] != '\0' ? notice : "Load which slot?", labels, count, panel_cursor - slot_first(), "Back");
		draw_touch_scrollbar(slot_first(), SLOT_VISIBLE, SAVE_SLOTS);
		return;
	}
	if (mode == MODE_TITLE && panel == PANEL_NONE) {
		const char *labels[8];
		int count = title_touch_labels(labels);
		draw_touch_list(config.title[0] != '\0' ? config.title : game_name, labels, count, title_cursor,
			"Game list");
		if (notice[0] != '\0') {
			draw_line(notice, 20, LIST_EXTRA_Y - 18.0f, 0.42f, C2D_Color32(255, 200, 120, 255), Z_TEXT, BOTTOM_W - 40);
		}
		return;
	}
	if (mode == MODE_TITLE) {
		draw_line(config.title[0] != '\0' ? config.title : game_name,
			14, 6, 0.55f, theme_accent(), Z_TEXT, BOTTOM_W - 28);
		draw_line(panel == PANEL_NONE ? "A select   D-pad choose   B game list"
			: "Title menu panel on the top screen",
			14, 40, 0.45f, dim, Z_TEXT, BOTTOM_W - 28);
		if (notice[0] != '\0') {
			draw_line(notice, 14, 212, 0.45f, C2D_Color32(255, 200, 120, 255), Z_TEXT, BOTTOM_W - 28);
		}
		return;
	}

	Picture *backdrop = ui_image(IMG_BOTTOM_BG);
	if (backdrop != NULL) {
		C2D_DrawImageAt(backdrop->image, 0, 0, Z_BG, NULL, BOTTOM_W / (float)backdrop->width,
			TOP_H / (float)backdrop->height);
	}
	if (waiting == PLAY_VIDEO) {
		draw_line("Playing video", 14, 6, 0.55f, theme_accent(), Z_TEXT, 0.0f);
		draw_line(video.skippable ? "A or touch: skip" : "Please wait", 14, 40, 0.45f, dim, Z_TEXT, 0.0f);
		return;
	}
	if (waiting == PLAY_GALLERY) {
		draw_line("CG Gallery", 14, 6, 0.55f, theme_accent(), Z_TEXT, BOTTOM_W - 28);
		draw_line(gallery_detail ? "Left/Right browse  A grid" : "D-pad choose  A preview",
			14, 40, 0.44f, dim, Z_TEXT, BOTTOM_W - 28);
		draw_line("B back / exit", 14, 58, 0.44f, dim, Z_TEXT, BOTTOM_W - 28);
		return;
	}

	if (panel == PANEL_MENU) {
		const char *labels[LABEL_COUNT];
		int count = menu_touch_labels(labels);
		draw_touch_list(config.menu_title[0] != '\0' ? config.menu_title : "Menu", labels, count,
			menu_cursor_item, "Close");
		return;
	}
	if (panel == PANEL_BACKLOG) {
		draw_backlog_touch();
		return;
	}
	if (panel == PANEL_SAVE || panel == PANEL_LOAD) {
		const char *labels[SLOT_VISIBLE];
		int count = slot_touch_labels(labels);
		draw_touch_list(notice[0] != '\0' ? notice : panel == PANEL_SAVE ? "Save to which slot?" : "Load which slot?",
			labels, count, panel_cursor - slot_first(), "Back");
		draw_touch_scrollbar(slot_first(), SLOT_VISIBLE, SAVE_SLOTS);
		return;
	}
	if (panel != PANEL_NONE) {
		draw_line(config.title[0] != '\0' ? config.title : game_name, 14, 6, 0.55f, theme_accent(), Z_TEXT, BOTTOM_W - 28);
		draw_line("Menu is on the top screen", 14, 40, 0.45f, dim, Z_TEXT, 0.0f);
		if (notice[0] != '\0') {
			draw_line(notice, 14, 212, 0.45f, C2D_Color32(255, 200, 120, 255), Z_TEXT, BOTTOM_W - 28);
		}
		return;
	}

	if (touch_layout_active()) {
		if (bottom_text_active()) {
			GsEvent current = event;
			if (waiting == PLAY_TIMER && say_held) event = held_say;
			draw_bottom_dialogue();
			event = current;
		} else {
			draw_bottom_controls();
		}
		for (int index = 0; index < COUNT_OF(strip_buttons); ++index) {
			draw_strip_button(&strip_buttons[index], index == AUTO_BUTTON && auto_mode);
		}
		return;
	}
	if (waiting == PLAY_MENU && !ui_hidden && event.choice_count > 0) {
		/* Choices (story choices, chapter select) also as touch buttons here, in sync
		   with the list on the top screen. */
		draw_choice_buttons();
		draw_line("Tap a choice, or D-pad + A", 14, 216, 0.42f, dim, Z_TEXT, 0.0f);
		return;
	}
	draw_line(config.title[0] != '\0' ? config.title : game_name, 14, 6, 0.55f, theme_accent(), Z_TEXT, BOTTOM_W - 28);
	{
		char status[48];
		status_text(status, sizeof(status));
		draw_line(status, 14, 208, 0.42f, theme_text(), Z_TEXT, 0.0f);
	}
	int button_count;
	const Button *buttons = play_button_set(&button_count);
	for (int index = 0; index < button_count; ++index) {
		draw_button_box(&buttons[index], index == PLAY_AUTO_BUTTON && auto_mode);
	}
	draw_line(waiting == PLAY_MENU ? "D-pad choose   A confirm" : "A next   Y auto   Hold R fast-forward",
		14, 180, 0.42f, dim, Z_TEXT, 0.0f);
	draw_line(ui_hidden ? "Text hidden - press B or A" : START_HINT, 14, 194, 0.42f, dim, Z_TEXT, 0.0f);
	if (notice[0] != '\0') {   /* over the playtime line */
		rect(0, 206, BOTTOM_W, 18, config.has_bg ? rgb_color(config.bg, 255) : C2D_Color32(17, 29, 34, 255), Z_TEXT + 0.01f);
		draw_line(notice, 14, 208, 0.42f, C2D_Color32(255, 200, 120, 255), Z_TEXT + 0.02f, BOTTOM_W - 28);
	}
}

/* ---------- input ---------- */

static void leave_game(void)
{
	read_save();
	playtime_save();
	extras_mode = 0;
	extras_available = 0;
	auto_mode = 0;
	audio_stop_music(0.0f);
	audio_stop_sfx();
	scene.music[0] = '\0';
	gs_free(program);
	program = NULL;
	clear_cache();
	clear_bgloop();
	config_defaults(&config);
	set_game_font("");
	scan_games();
	panel = PANEL_NONE;
	mode = MODE_SELECT;
}

static void open_panel(Panel which)
{
	panel_return = panel == PANEL_MENU ? PANEL_MENU : PANEL_NONE;
	panel = which;
	ui_hidden = 0;
	if (which == PANEL_MENU) {
		menu_count = 0;
		static const int order[] = {LABEL_SAVE, LABEL_LOAD, LABEL_BACKLOG, LABEL_GLOSSARY,
			LABEL_ENDINGS, LABEL_GUIDE, LABEL_SYNC, LABEL_OPTIONS, LABEL_QUIT};
		for (int i = 0; i < LABEL_COUNT; ++i) {
			int item = order[i];
			if ((item == LABEL_GLOSSARY && glossary.count == 0) || (item == LABEL_ENDINGS && ending_count == 0)
				|| (item == LABEL_GUIDE && guide_file[0] == '\0')) {
				continue;
			}
			menu_items[menu_count++] = item;
		}
		menu_cursor_item = 0;
	} else if (which == PANEL_BACKLOG) {
		backlog_view = 0;
	} else if (which == PANEL_QUIT) {
		panel_cursor = 0;
	} else if (which == PANEL_ENDINGS) {
		ending_top = 0;
	} else if (which == PANEL_SYNC) {
		sync_open();
	} else if (which == PANEL_GUIDE) {
		guide_drag = 0;
		if (!guide_load()) {
			panel = panel_return;
			panel_return = PANEL_NONE;
		}
	} else if (which == PANEL_SAVE || which == PANEL_LOAD) {
		refresh_slots();
		panel_cursor = last_slot;
	} else if (which == PANEL_GLOSSARY) {
		panel_cursor = 0;
		gloss_visible_count = glossary_visible_indices(&glossary, &state, gloss_indices);
		gloss_layout_for = -1;
		gloss_detail = 0;
		gloss_scroll = 0;
	} else if (which == PANEL_OPTIONS) {
		panel_cursor = 0;
	}
}

/* Goes back to the menu if the panel was opened from it, otherwise back to the game. */
static void close_panel(void)
{
	panel = panel_return;
	panel_return = PANEL_NONE;
}

static void choose_menu_item(int index)
{
	switch (menu_items[index]) {
	case LABEL_SAVE: open_panel(PANEL_SAVE); break;
	case LABEL_LOAD: open_panel(PANEL_LOAD); break;
	case LABEL_BACKLOG: open_panel(PANEL_BACKLOG); break;
	case LABEL_GLOSSARY: open_panel(PANEL_GLOSSARY); break;
	case LABEL_OPTIONS: open_panel(PANEL_OPTIONS); break;
	case LABEL_ENDINGS: open_panel(PANEL_ENDINGS); break;
	case LABEL_GUIDE: open_panel(PANEL_GUIDE); break;
	case LABEL_SYNC: open_panel(PANEL_SYNC); break;
	default: open_panel(PANEL_QUIT); break;
	}
}

static void choose_title_item(void)
{
	switch (title_items[title_cursor]) {
	case TITLE_START:
		begin_game();
		break;
	case TITLE_LOAD:
		open_panel(PANEL_LOAD);
		break;
	case TITLE_ENDINGS:
		open_panel(PANEL_ENDINGS);
		break;
	case TITLE_GUIDE:
		open_panel(PANEL_GUIDE);
		break;
	case TITLE_SYNC:
		open_panel(PANEL_SYNC);
		break;
	case TITLE_OPTIONS:
		open_panel(PANEL_OPTIONS);
		break;
	case TITLE_EXTRAS:
		start_extras();
		break;
	}
}

static int hit_button(const Button *buttons, int count, const touchPosition *touch);

static void handle_title_input(u32 down, u32 repeat)
{
	if (down & KEY_B) {
		leave_game();
		return;
	}
	if (repeat & KEY_DDOWN) {
		title_cursor = (title_cursor + 1) % title_count;
	}
	if (repeat & KEY_DUP) {
		title_cursor = (title_cursor + title_count - 1) % title_count;
	}
	if (down & KEY_A) {
		choose_title_item();
		return;
	}
	if (down & KEY_TOUCH) {
		touchPosition touch;
		hidTouchRead(&touch);
		const char *labels[8];
		Button buttons[10];
		int count = title_touch_labels(labels);
		int hit = hit_button(buttons, touch_list_buttons(buttons, labels, count, "Game list"), &touch);
		if (hit >= 0 && hit < count) {
			title_cursor = hit;
			choose_title_item();
		} else if (hit == count) {
			leave_game();
		}
	}
}

static int hit_button(const Button *buttons, int count, const touchPosition *touch)
{
	for (int index = 0; index < count; ++index) {
		const Button *b = &buttons[index];
		if (touch->px >= b->x && touch->px < b->x + b->w && touch->py >= b->y && touch->py < b->y + b->h) {
			return index;
		}
	}
	return -1;
}

static void scroll_backlog(int direction)
{
	int target = backlog_view + direction;
	if (target >= 0 && target < backlog.count) {
		backlog_view = target;
	}
}

static void confirm_slot(void)
{
	if (panel == PANEL_SAVE) {
		save_slot(panel_cursor);
		panel = panel_return = PANEL_NONE;
	} else if (!slots[panel_cursor].used) {
		snprintf(notice, sizeof(notice), "slot %d is empty", panel_cursor + 1);
	} else if (load_slot(panel_cursor)) {
		panel = panel_return = PANEL_NONE;
		mode = MODE_PLAY;
	} else {
		refresh_slots();
	}
}

static void handle_panel_input(u32 down, u32 repeat)
{
	switch (panel) {
	case PANEL_MENU: {
		if (repeat & KEY_DDOWN) {
			menu_cursor_item = (menu_cursor_item + 1) % menu_count;
		}
		if (repeat & KEY_DUP) {
			menu_cursor_item = (menu_cursor_item + menu_count - 1) % menu_count;
		}
		if (down & KEY_A) {
			choose_menu_item(menu_cursor_item);
		} else if (down & (KEY_B | KEY_X)) {
			close_panel();
		} else if (down & KEY_TOUCH) {
			touchPosition touch;
			hidTouchRead(&touch);
			const char *labels[LABEL_COUNT];
			Button buttons[LABEL_COUNT + 2];
			int count = menu_touch_labels(labels);
			int hit = hit_button(buttons, touch_list_buttons(buttons, labels, count, "Close"), &touch);
			if (hit >= 0 && hit < count) {
				menu_cursor_item = hit;
				choose_menu_item(hit);
			} else if (hit == count) {
				close_panel();
			}
		}
		break;
	}
	case PANEL_ENDINGS:
		endings_input(down, repeat);
		break;
	case PANEL_SYNC:
		sync_input(down, repeat);
		break;
	case PANEL_GUIDE:
		guide_input(down, repeat);
		break;
	case PANEL_GLOSSARY: {
		if (down & (KEY_B | KEY_X)) {
			if (gloss_detail) {
				gloss_detail = 0;
			} else {
				close_panel();
			}
			break;
		}
		if (gloss_detail) {
			if ((repeat & KEY_DUP) && gloss_scroll > 0) {
				--gloss_scroll;
			}
			if ((repeat & KEY_DDOWN) && gloss_scroll + GLOSSARY_VISIBLE_LINES < gloss_line_count) {
				++gloss_scroll;
			}
			break;
		}
		if (gloss_visible_count == 0) {
			break;
		}
		if (repeat & KEY_DDOWN) {
			panel_cursor = (panel_cursor + 1) % gloss_visible_count;
		}
		if (repeat & KEY_DUP) {
			panel_cursor = (panel_cursor + gloss_visible_count - 1) % gloss_visible_count;
		}
		if (down & KEY_A) {
			gloss_detail = 1;
			gloss_scroll = 0;
		}
		break;
	}
	case PANEL_OPTIONS: {
		if (down & (KEY_B | KEY_X)) {
			close_panel();
			break;
		}
		if (repeat & KEY_DDOWN) {
			panel_cursor = (panel_cursor + 1) % OPTION_ROWS;
		}
		if (repeat & KEY_DUP) {
			panel_cursor = (panel_cursor + OPTION_ROWS - 1) % OPTION_ROWS;
		}
		int is_volume = volume_field(&display_options, panel_cursor) != NULL;
		u32 sideways = is_volume ? repeat : down;   /* hold Left/Right to slide a volume */
		if (down & KEY_A) {
			toggle_display_option(1);
		} else if (sideways & KEY_DRIGHT) {
			toggle_display_option_ex(1, !is_volume);
		} else if (sideways & KEY_DLEFT) {
			toggle_display_option_ex(-1, !is_volume);
		} else if (down & KEY_TOUCH) {
			touchPosition touch;
			hidTouchRead(&touch);
			const char *labels[OPTION_VISIBLE];
			Button buttons[OPTION_VISIBLE + 1];
			int count = option_touch_labels(labels);
			int hit = hit_button(buttons, touch_list_buttons(buttons, labels, count, "Back"), &touch);
			if (hit >= 0 && hit < count) {
				panel_cursor = option_first() + hit;
				if (volume_field(&display_options, panel_cursor) != NULL) {
					/* left half turns down, right half turns up */
					toggle_display_option_ex(touch.px < buttons[hit].x + buttons[hit].w / 2.0f ? -1 : 1, 0);
				} else {
					toggle_display_option(1);
				}
			} else if (hit == count) {
				close_panel();
			}
		}
		break;
	}
	case PANEL_BACKLOG: {
		if (repeat & KEY_DUP) {
			scroll_backlog(1);
		}
		if (repeat & KEY_DDOWN) {
			scroll_backlog(-1);
		}
		if (repeat & KEY_DRIGHT) {
			scroll_backlog(-5);
		}
		if (repeat & KEY_DLEFT) {
			scroll_backlog(5);
		}
		backlog_touch_input(down, hidKeysHeld());
		if (down & (KEY_B | KEY_L)) {
			close_panel();
		}
		break;
	}
	case PANEL_SAVE:
	case PANEL_LOAD: {
		if (down & KEY_B) {
			close_panel();
			break;
		}
		if (repeat & KEY_DDOWN) {
			panel_cursor = (panel_cursor + 1) % SAVE_SLOTS;
		}
		if (repeat & KEY_DUP) {
			panel_cursor = (panel_cursor + SAVE_SLOTS - 1) % SAVE_SLOTS;
		}
		if (down & KEY_A) {
			confirm_slot();
		} else if (down & KEY_TOUCH) {
			touchPosition touch;
			hidTouchRead(&touch);
			const char *labels[SLOT_VISIBLE];
			Button buttons[SLOT_VISIBLE + 1];
			int count = slot_touch_labels(labels);
			int hit = hit_button(buttons, touch_list_buttons(buttons, labels, count, "Back"), &touch);
			if (hit >= 0 && hit < count) {
				panel_cursor = slot_first() + hit;
				confirm_slot();
			} else if (hit == count) {
				close_panel();
			}
		}
		break;
	}
	case PANEL_QUIT: {
		const char *labels[4];
		int actions[4];
		int count = quit_choices(labels, actions);
		if (panel_cursor < 0 || panel_cursor >= count) panel_cursor = 0;
		if (repeat & KEY_DDOWN) panel_cursor = (panel_cursor + 1) % count;
		if (repeat & KEY_DUP) panel_cursor = (panel_cursor + count - 1) % count;
		if (down & KEY_A) {
			choose_quit(actions[panel_cursor]);
		} else if (down & KEY_B) {
			close_panel();
		} else if (down & KEY_TOUCH) {
			touchPosition touch;
			hidTouchRead(&touch);
			Button buttons[6];
			int hit = hit_button(buttons, touch_list_buttons(buttons, labels, count, "No, keep playing"), &touch);
			if (hit >= 0 && hit < count) {
				panel_cursor = hit;
				choose_quit(actions[hit]);
			} else if (hit == count) {
				close_panel();
			}
		}
		break;
	}
	case PANEL_NONE:
		break;
	}
}

static void handle_play_input(u32 down, u32 held, float dt)
{
	if (waiting == PLAY_GALLERY) {
		if (gallery_detail) {
			if (down & KEY_B) {
				gallery_detail = 0;
			} else if (down & KEY_A) {
				gallery_detail = 0;
			} else if (down & KEY_DLEFT) {
				gallery_cursor = (gallery_cursor + gallery_count - 1) % gallery_count;
			} else if (down & KEY_DRIGHT) {
				gallery_cursor = (gallery_cursor + 1) % gallery_count;
			}
		} else if (down & KEY_B) {
			waiting = PLAY_NONE;
			gallery_count = 0;
		} else if (down & KEY_A) {
			gallery_detail = 1;
		} else if (down & KEY_DLEFT) {
			if (gallery_cursor > 0) --gallery_cursor;
		} else if (down & KEY_DRIGHT) {
			if (gallery_cursor + 1 < gallery_count) ++gallery_cursor;
		} else if (down & KEY_DUP) {
			if (gallery_cursor >= 3) gallery_cursor -= 3;
		} else if (down & KEY_DDOWN) {
			if (gallery_cursor + 3 < gallery_count) gallery_cursor += 3;
		} else if ((down & KEY_L) && gallery_cursor >= GALLERY_PAGE_SIZE) {
			gallery_cursor -= GALLERY_PAGE_SIZE;
		} else if ((down & KEY_R) && gallery_cursor + GALLERY_PAGE_SIZE < gallery_count) {
			gallery_cursor += GALLERY_PAGE_SIZE;
		}
		fast_forward = 0;
		return;
	}
	if (extras_mode && (down & (KEY_B | KEY_X | KEY_SELECT))) {
		return_to_title();
		return;
	}
	if (waiting == PLAY_VIDEO) {
		fast_forward = 0;
		update_video(dt, down);
		return;
	}
	if (down & KEY_L) {
		open_panel(PANEL_BACKLOG);
		return;
	}
	if (down & KEY_X) {
		open_panel(PANEL_MENU);
		return;
	}
	if (down & KEY_B) {
		ui_hidden = !ui_hidden;
		return;
	}
#ifndef OFF_TV   /* on the Wii U, Minus switches off-TV play instead */
	if (down & KEY_SELECT) {
		open_panel(PANEL_QUIT);
		return;
	}
#endif
	if (down & KEY_Y) {
		auto_mode = !auto_mode;
		auto_wait = 0.0f;
		return;
	}
	if (down & KEY_TOUCH) {
		touchPosition touch;
		hidTouchRead(&touch);
		static const Panel targets[] = {PANEL_MENU, PANEL_BACKLOG, PANEL_NONE, PANEL_QUIT, PANEL_NONE,
			PANEL_GUIDE, PANEL_SAVE, PANEL_LOAD};
		int button_count;
		const Button *buttons = play_button_set(&button_count);
		int button;
		if (touch_layout_active()) {
			button = hit_button(strip_buttons, COUNT_OF(strip_buttons), &touch);
		} else if (waiting == PLAY_MENU && !ui_hidden && event.choice_count > 0) {
			button = -1;   /* choices own the screen */
		} else {
			button = hit_button(buttons, button_count, &touch);
			if (button >= 0) button = play_actions[button];
		}
		if (button == 2) {
			ui_hidden = !ui_hidden;
			return;
		}
		if (button == AUTO_BUTTON) {
			auto_mode = !auto_mode;
			auto_wait = 0.0f;
			return;
		}
		if (button >= 0) {
			open_panel(targets[button]);
			return;
		}
	}
	/* Bottom-text layout: tapping the dialogue area works like A. */
	int tap_text = 0;
	if (touch_layout_active() && (down & KEY_TOUCH) && waiting != PLAY_MENU) {
		touchPosition touch;
		hidTouchRead(&touch);
		tap_text = touch.py < STRIP_Y;
	}
	if (ui_hidden) {
		fast_forward = 0;
		if ((down & KEY_A) || tap_text) {
			ui_hidden = 0;
		}
		return;
	}
	fast_forward = (held & KEY_R) != 0;
	if (!fast_forward) {
		ff_wait = 0.0f;
	}
	ff_blocked = 0;
	if (read_dirty >= 64) {
		read_save();   /* keep progress if the console is switched off mid-chapter */
	}

	switch (waiting) {
	case PLAY_TEXT: {
		size_t total = 0;
		for (const char *c = event.text; *c != '\0'; ++c) {
			total += ((unsigned char)*c & 0xC0) != 0x80;
		}
		chars_shown += text_chars_per_sec() * dt;
		if (auto_mode && !line_voiced && audio_sfx_playing()) {
			line_voiced = 1;
		}
		int skipping = fast_forward;
		if (skipping && display_options.skip_read && !line_read(current_script, text_pc)) {
			skipping = 0;     /* unread: stop here until the player reads it */
			ff_blocked = 1;
		}
		if (skipping) {
			chars_shown = (float)total;
			ff_wait += dt;
			if (ff_wait >= FF_STEP) {
				ff_wait = 0.0f;
				audio_stop_sfx();
				mark_read(current_script, text_pc);
				waiting = PLAY_NONE;
			}
		} else if ((down & KEY_A) || tap_text) {
			if (chars_shown < (float)total) {
				chars_shown = (float)total;
			} else {
				audio_stop_sfx();
				mark_read(current_script, text_pc);
				waiting = PLAY_NONE;
			}
		} else if (auto_mode && chars_shown >= (float)total) {
			if (audio_sfx_playing()) {
				line_voiced = 1;
				auto_wait = 0.0f;
			} else {
				/* Voiced lines go on shortly after the voice ends; others get reading time. */
				float delay = line_voiced ? AUTO_DELAY_VOICED
					: AUTO_DELAY_BASE + AUTO_DELAY_PER_CHAR * (float)total;
				if (delay > AUTO_DELAY_MAX) {
					delay = AUTO_DELAY_MAX;
				}
				delay *= auto_delay_factor();
				auto_wait += dt;
				if (auto_wait >= delay) {
					mark_read(current_script, text_pc);
					waiting = PLAY_NONE;
				}
			}
		}
		break;
	}
	case PLAY_MENU:
		if (event.choice_count > 0) {
			if (down & KEY_DDOWN) {
				menu_cursor = (menu_cursor + 1) % event.choice_count;
			}
			if (down & KEY_DUP) {
				menu_cursor = (menu_cursor + event.choice_count - 1) % event.choice_count;
			}
		}
		int touched = 0;
		if (down & KEY_TOUCH) {
			touchPosition touch;
			hidTouchRead(&touch);
			Button buttons[GS_MAX_CHOICES];
			int hit = hit_button(buttons, choice_buttons(buttons), &touch);
			if (hit >= 0) {
				menu_cursor = hit;
				touched = 1;
			}
		}
		if (((down & KEY_A) || touched) && menu_cursor < event.choice_count) {
			char entry[GS_STR_LEN + 4];
			snprintf(entry, sizeof(entry), "> %s", event.choices[menu_cursor]);
			if (gs_choose(program, &state, menu_cursor)) {
				backlog_push("", 0, entry);
				waiting = PLAY_NONE;
			}
		}
		break;
	case PLAY_TIMER:
		timer_left -= dt;
		if (timer_left <= 0.0f || (down & KEY_A) || fast_forward) {
			waiting = PLAY_NONE;
		}
		break;
	case PLAY_WAITBG:
		if (scene.bg_prev[0] == '\0') {
			waiting = PLAY_NONE;
		} else if (event.seconds > 0.0f && ((down & KEY_A) || fast_forward)) {
			scene.bg_prev[0] = '\0';
			scene.bg_elapsed = scene.bg_duration;
			waiting = PLAY_NONE;
		}
		break;
	case PLAY_NONE:
	case PLAY_END:
	case PLAY_VIDEO:
	case PLAY_GALLERY:
		break;
	}
}

/* ---------- main ---------- */

int main(void)
{
	gfxInitDefault();
	cfguInit();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	osSetSpeedupEnable(true);  /* faster CPU on New 3DS; ignored on the original */
	system_font = font = C2D_FontLoadSystem(CFG_REGION_USA);
	text_buf = C2D_TextBufNew(8192);
	measure_buf = C2D_TextBufNew(NVL_PAGE_MAX_BYTES + 64);
	audio_init();
	hidSetRepeatParameters(18, 4);
	C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

	mkdir(ROOT_DIR, 0777);
	mkdir(GAMES_DIR, 0777);
	scan_games();

	char last_notice[sizeof(notice)] = "";
	float notice_age = 0.0f;
	u32 last = now_ms();
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		u32 repeat = hidKeysDownRepeat();
#ifdef OFF_TV
		if (down & KEY_SELECT) {
			off_tv = !off_tv;
			snprintf(notice, sizeof(notice), off_tv ? "GamePad only (press \xe2\x88\x92 for the TV)" : "TV and GamePad");
			down &= ~KEY_SELECT;
			repeat &= ~KEY_SELECT;
		}
		if (off_tv && panel != PANEL_GUIDE && (down & KEY_TOUCH)) {
			down &= ~KEY_TOUCH;
			repeat &= ~KEY_TOUCH;
			if (mode == MODE_PLAY && panel == PANEL_NONE) down |= KEY_A;   /* tap: next line */
		}
#endif
		if (start_requested) {   /* e.g. the game list's Exit button */
			start_requested = 0;
			down |= KEY_START;
		}
		u32 now = now_ms();
		float dt = (float)(now - last) / 1000.0f;
		last = now;

#ifdef EXIT_MENU
		if (exiting) {
			down = repeat = 0;   /* wait for the system to close the app */
		} else if (exit_open || (down & KEY_START)) {
			if (!exit_open) {
				open_exit_menu();
			} else if (handle_exit_menu(down, repeat)) {
				break;
			}
			down = repeat = 0;
		}
#else
		if (down & KEY_START) {
			break;
		}
#endif

		if (quit_app) break;
		audio_update(dt);
		if (down || hidKeysHeld()) last_activity = now;
		if (mode == MODE_PLAY && panel == PANEL_NONE && !extras_mode && now - last_activity < IDLE_LIMIT_MS
#ifdef EXIT_MENU
			&& !exit_open
#endif
			) {
			playtime += dt;
			playtime_unsaved += dt;
			if (playtime_unsaved > 30.0f) playtime_save();
		}
#ifdef AUTOTEST
		{
			static char logged_notice[sizeof(notice)], logged_message[sizeof(message)];
			if (strcmp(notice, logged_notice)) {
				if (notice[0]) TEST_LOG("NOTICE %s", notice);
				snprintf(logged_notice, sizeof(logged_notice), "%s", notice);
			}
			if (mode == MODE_ERROR && strcmp(message, logged_message)) {
				TEST_LOG("ERROR %s", message);
				snprintf(logged_message, sizeof(logged_message), "%s", message);
			}
		}
#endif

		if (mode == MODE_SELECT) {
			if (down & KEY_TOUCH) {
				touchPosition touch;
				hidTouchRead(&touch);
				if (touch.py >= SELECT_EXIT_Y) {
					start_requested = 1;   /* the Exit button */
					down &= ~(u32)KEY_TOUCH;
				}
			}
			if (game_count > 0) {
				if (down & KEY_DDOWN) {
					select_cursor = (select_cursor + 1) % game_count;
				}
				if (down & KEY_DUP) {
					select_cursor = (select_cursor + game_count - 1) % game_count;
				}
				if (down & KEY_TOUCH) {
					touchPosition touch;
					hidTouchRead(&touch);
					int first = select_cursor >= SELECT_ROWS ? select_cursor - SELECT_ROWS + 1 : 0;
					int index = first + ((int)touch.py - 30) / 28;
					if (touch.py >= 30 && touch.py < 30 + SELECT_ROWS * 28 && index < game_count) {
						select_cursor = index;
						down |= KEY_A;
					}
				}
				if (down & KEY_A) {
					start_game(game_names[select_cursor]);
				}
			}
		} else if (mode == MODE_ERROR) {
			if (down & KEY_B) {
				if (extras_mode) {
					return_to_title();
				} else {
					leave_game();
				}
			}
		} else if (mode == MODE_TITLE) {
			if (panel != PANEL_NONE) {
				handle_panel_input(down, repeat);
			} else {
				handle_title_input(down, repeat);
			}
		} else {
			fast_forward = fast_forward && panel == PANEL_NONE;
			update_scene(fast_forward ? dt * FF_SPEED : dt);
			if (panel == PANEL_NONE && waiting == PLAY_NONE) {
				advance();
			}
			if (mode == MODE_PLAY) {
				if (panel != PANEL_NONE) {
					handle_panel_input(down, repeat);
				} else {
					handle_play_input(down, hidKeysHeld(), dt);
				}
			}
			/* A line just finished (A pressed): fetch the next one now, so this frame is not
			   drawn without a text box (on Wii U the next frame can take a while, loading the
			   next voice and faces, which made the box flash off between lines). */
			if (mode == MODE_PLAY && panel == PANEL_NONE && waiting == PLAY_NONE) {
				advance();
			}
		}


		if (strcmp(notice, last_notice) != 0) {
			snprintf(last_notice, sizeof(last_notice), "%s", notice);
			notice_age = 0.0f;
		} else if (notice[0] != '\0' && (notice_age += dt) > 4.0f) {
			notice[0] = '\0';
			last_notice[0] = '\0';
		}

#ifdef VN_SDL
		{
			char frame_note[64];
			snprintf(frame_note, sizeof(frame_note), "%s%s%s%s",
				mode == MODE_PLAY ? "play" : mode == MODE_TITLE ? "title" : mode == MODE_SELECT ? "list" : "error",
				fast_forward ? " ff" : "", auto_mode ? " auto" : "", panel != PANEL_NONE ? " panel" : "");
			vn_frame_note(frame_note);
		}
#endif
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
#ifdef OFF_TV
		if (off_tv) {
			C2D_TargetClear(top, C2D_Color32(0, 0, 0, 255));
			C2D_SceneBegin(top);
			draw_line("Playing on the GamePad   (press \xe2\x88\x92 to show the TV again)", 12, 222, 0.4f,
				C2D_Color32(110, 120, 120, 255), Z_TEXT, 0.0f);
			if (panel == PANEL_GUIDE && !exit_open && !exiting) {
				draw_bottom(bottom);
			} else {
				float tv_width = top_w;
				top_w = BOTTOM_W;   /* the wide layout fills the GamePad */
				draw_top(bottom);
				if (exit_open || exiting) draw_exit_top();
				top_w = tv_width;
			}
			C3D_FrameEnd(0);
			continue;
		}
#endif
		draw_top(top);
#ifdef EXIT_MENU
		if (exit_open || exiting) {
			draw_exit_top();
			draw_exit_bottom(bottom);
		} else {
			draw_bottom(bottom);
		}
#else
		draw_bottom(bottom);
#endif
		C3D_FrameEnd(0);
	}

	stop_video();
	playtime_save();   /* the app is closing (START / exit menu): keep the last minutes */
	read_save();
	gs_free(program);
	clear_cache();
	audio_exit();
	C2D_TextBufDelete(measure_buf);
	C2D_TextBufDelete(text_buf);
	set_game_font("");
	C2D_FontFree(system_font);
	C2D_Fini();
	C3D_Fini();
	cfguExit();
	gfxExit();
	return 0;
}
