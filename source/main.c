#include <math.h>
#include <3ds.h>
#include <citro2d.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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

/* The default 32 KB stack is too small for the script VM state. */
u32 __stacksize__ = 256 * 1024;

#define TOP_W 400.0f
#define TOP_H 240.0f
#define BOTTOM_W 320.0f
#define ROOT_DIR "sdmc:/3ds/3dvn"
#define GAMES_DIR ROOT_DIR "/games"
#define SAVES_DIR ROOT_DIR "/saves"
#define SETTINGS_DIR ROOT_DIR "/settings"
#define MAX_GAMES 32
#define MAX_SPRITES 8
#define MAX_TEXTURES 32
#define MAX_GALLERY_ITEMS 512
#define GALLERY_PAGE_SIZE 6
#define SAVE_MAGIC 0x32564433u
#define SAVE_SLOTS 15
#define LIST_ROWS 8
#define FF_SPEED 8.0f
#define FF_STEP 0.07f
#define BACKLOG_MAX 40
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

typedef struct {
	int active;
	char name[GS_NAME_LEN];
	char asset[GS_STR_LEN];
	char pos[GS_NAME_LEN];
	float alpha, from, to, duration, elapsed;
} Sprite;

typedef struct {
	char bg[GS_STR_LEN];
	char bg_prev[GS_STR_LEN];
	char music[GS_STR_LEN];
	float bg_elapsed, bg_duration;
	Sprite sprites[MAX_SPRITES];
} Scene;

/* Transient screen effects; not saved. */
static float shake_left, shake_total, shake_amp, flash_left, flash_total, flash_shade;
static float fx_x, fx_y;
static float cam_pan_x, cam_pan_y, cam_zoom = 1.0f;
static float cam_from[3], cam_to[3] = {0.0f, 0.0f, 1.0f}, cam_t = 1.0f, cam_dur;
static float hop_t[16]; /* seconds left, indexed by sprite slot */
static float cur_hop;

typedef struct {
	char speaker[GS_NAME_LEN];
	char text[GS_TEXT_LEN];
	unsigned color;
} BacklogEntry;

typedef struct {
	int count, start;
	BacklogEntry entries[BACKLOG_MAX];
} Backlog;

typedef struct {
	unsigned magic;
	unsigned script_hash;
	long long saved_at;
	char preview[72];
} SaveHeader;

typedef struct {
	SaveHeader header;
	GsState state;
	Scene scene;
	GsEvent event;
	Backlog backlog;
} SaveData;

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
	PANEL_NONE, PANEL_MENU, PANEL_BACKLOG, PANEL_SAVE, PANEL_LOAD, PANEL_GLOSSARY, PANEL_QUIT, PANEL_OPTIONS
} Panel;

typedef enum { MODE_SELECT, MODE_TITLE, MODE_PLAY, MODE_ERROR } Mode;
typedef enum { TITLE_START, TITLE_LOAD, TITLE_OPTIONS, TITLE_EXTRAS } TitleItem;
typedef enum {
	PLAY_NONE, PLAY_TEXT, PLAY_MENU, PLAY_TIMER, PLAY_END, PLAY_VIDEO, PLAY_WAITBG, PLAY_GALLERY
} Wait;

static char game_names[MAX_GAMES][64];
static int game_count;
static char game_dir[160];
static char game_name[64];
static GsProgram *program;
static char current_script[GS_STR_LEN]; /* script switched to by `script`; empty while main.3vs runs */
static GsState state;
static GsEvent event;
static Scene scene;
static GsBgLoop bgloop;
static NvlPage nvl_page;
static Picture *loop_pictures[3];
static Mode mode = MODE_SELECT;
static Wait waiting = PLAY_NONE;
static char message[GS_TEXT_LEN];
static char notice[96];
static float timer_left;
static float chars_shown;
static u32 advance_started;
static int advance_visible;
static int advance_available;
static int menu_cursor;
static int select_cursor;
static int title_items[4], title_count, title_cursor;
static int extras_available, extras_mode;
static Panel panel;
static int panel_cursor;
static int last_slot;
static int backlog_view;
static unsigned script_hash;
static Backlog backlog;
static WrapLayout log_layout[BACKLOG_MAX];
static SlotInfo slots[SAVE_SLOTS];
static C2D_TextBuf measure_buf;
static GameConfig config;
static DisplayOptions display_options;
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
	FILE *file = fopen(path, "rb");
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

static Picture *load_picture(const char *path)
{
	size_t size;
	unsigned char *file_data = read_file(path, &size);
	if (file_data == NULL) {
		return NULL;
	}
	int width, height, channels;
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
	static const char *extensions[] = {".wav", ""};
	char path[400];
	for (int index = 0; index < 2; ++index) {
		snprintf(path, sizeof(path), "%s/audio/%s%s", game_dir, e->asset, extensions[index]);
		FILE *probe = fopen(path, "rb");
		if (probe != NULL) {
			fclose(probe);
			break;
		}
	}
	int ok = is_music ? audio_play_music(path, e->seconds, err, sizeof(err))
		: audio_play_sfx(path, err, sizeof(err));
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
		snprintf(path, sizeof(path), GAMES_DIR "/%s/" SCRIPT_FILE, entry->d_name);
		if (stat(path, &info) == 0) {
			snprintf(game_names[game_count++], sizeof(game_names[0]), "%s", entry->d_name);
		}
	}
	closedir(dir);
}

/* ---------- backlog ---------- */

static float measure_scale = LOG_SCALE;

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

static void load_display_options(void)
{
	char path[200];
	display_options_defaults(&display_options);
	options_path(path, sizeof(path));
	DisplayOptionsResult result = display_options_load(path, &display_options);
	display_options_apply_game_default(&display_options, result, config.default_nvl);
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

static void toggle_display_option(void)
{
	DisplayOptions next = display_options;
	char path[200];
	if (panel_cursor == 0) {
		if (!current_textbox_available()) {
			snprintf(notice, sizeof(notice), "This game has no custom textbox");
			return;
		}
		next.custom_textbox = !next.custom_textbox;
	} else {
		next.nvl = !next.nvl;
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
	snprintf(notice, sizeof(notice), "%s", result == DISPLAY_OPTIONS_SAVED_BACKUP_REMAINS
		? "Options saved; remove old settings backup" : "Options saved");
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
		save_path(slot, path, sizeof(path));
		memset(&slots[slot], 0, sizeof(slots[slot]));
		FILE *file = fopen(path, "rb");
		if (file != NULL) {
			if (fread(&header, sizeof(header), 1, file) == 1 && header.magic == SAVE_MAGIC) {
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

	unsigned char loop_data[GS_BGLOOP_SAVE_SIZE];
	int ok = gs_bgloop_encode(&bgloop, loop_data)
		&& fwrite(data, sizeof(*data), 1, file) == 1
		&& fwrite(loop_data, sizeof(loop_data), 1, file) == 1;
	if (ok && config.num[NUM_NVL_ACCUMULATE]) {
		unsigned char page_data[NVL_PAGE_SAVE_MAX_BYTES];
		size_t page_size = 0;
		ok = nvl_page_encode(&nvl_page, page_data, sizeof(page_data), &page_size) == NVL_PAGE_OK
			&& fwrite(page_data, 1, page_size, file) == page_size;
	}
	ok = fclose(file) == 0 && ok;
	free(data);
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
	snprintf(notice, sizeof(notice), "Saved to slot %d", slot + 1);
	return 1;
}

static void show_event(void)
{
	switch (event.type) {
	case GS_EV_SAY:
		waiting = PLAY_TEXT;
		chars_shown = 0.0f;
		advance_visible = 0;
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

static int load_slot(int slot)
{
	char path[200];
	save_path(slot, path, sizeof(path));
	SaveData *data = malloc(sizeof(*data));
	FILE *file = data != NULL ? fopen(path, "rb") : NULL;
	int read_ok = file != NULL && fread(data, sizeof(*data), 1, file) == 1;
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
	if (read_ok && data->header.magic == SAVE_MAGIC && data->header.script_hash == script_hash
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
	} else if (data->header.script_hash != script_hash) {
		problem = "save is from a different script version";
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
	scene = data->scene;
	event = data->event;
	backlog = data->backlog;
	nvl_page = loaded_page;
	memset(log_layout, 0, sizeof(log_layout));
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
	GsProgram *next = gs_compile_ex((const char *)source, load_include, NULL, error, sizeof(error));
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
	gs_state_init(program, &state);
	memset(&backlog, 0, sizeof(backlog));
	memset(log_layout, 0, sizeof(log_layout));
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
	title_cursor = 0;
	refresh_slots();
	mode = MODE_TITLE;
}

static void begin_game(void)
{
	gs_state_init(program, &state);
	memset(&backlog, 0, sizeof(backlog));
	memset(log_layout, 0, sizeof(log_layout));
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
	video.file = fopen(path, "rb");

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

static int start_gallery(void)
{
	char directory[320];
	snprintf(directory, sizeof(directory), "%s/gallery/thumbs", game_dir);
	DIR *handle = opendir(directory);
	if (handle == NULL) {
		snprintf(message, sizeof(message), "Gallery: cannot open gallery/thumbs");
		mode = MODE_ERROR;
		return 0;
	}

	gallery_count = 0;
	struct dirent *entry;
	while ((entry = readdir(handle)) != NULL && gallery_count < MAX_GALLERY_ITEMS) {
		const char *extension = strrchr(entry->d_name, '.');
		if (extension == NULL || (strcmp(extension, ".png") && strcmp(extension, ".jpg"))) {
			continue;
		}
		size_t length = (size_t)(extension - entry->d_name);
		if (length == 0 || length >= GS_STR_LEN) {
			continue;
		}
		char path[576];
		snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
		struct stat info;
		if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
			continue;
		}
		int duplicate = 0;
		for (int i = 0; i < gallery_count; ++i) {
			if (strlen(gallery_items[i]) == length
				&& strncmp(gallery_items[i], entry->d_name, length) == 0) {
				duplicate = 1;
				break;
			}
		}
		if (!duplicate) {
			memcpy(gallery_items[gallery_count], entry->d_name, length);
			gallery_items[gallery_count][length] = '\0';
			++gallery_count;
		}
	}
	closedir(handle);
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
	memcpy(shown, nvl_page.text, length);
	shown[length] = '\0';
	unsigned short starts[NVL_PAGE_MAX_BYTES + 1], ends[NVL_PAGE_MAX_BYTES + 1];
	float previous_scale = measure_scale;
	measure_scale = scale;
	int count = wrap_text_lines(shown, width, measure_text, NULL, starts, ends, NVL_PAGE_MAX_BYTES + 1);
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
		*x = (float)atoi(pos);
		*y = (float)atoi(strchr(pos, ',') + 1);
		return;
	}
	*y = TOP_H - height;
	float center = strcmp(pos, "left") == 0 ? 90.0f
		: strcmp(pos, "right") == 0 ? 310.0f
		: strcmp(pos, "farleft") == 0 ? 40.0f
		: strcmp(pos, "farright") == 0 ? 360.0f : 200.0f;
	*x = center - width / 2.0f;
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

	for (int index = 0; index < MAX_SPRITES; ++index) {
		const Sprite *sprite = &scene.sprites[index];
		Picture *picture = sprite->active ? get_picture("sprites", sprite->asset) : NULL;
		if (picture != NULL && !strcmp(sprite->pos, "face")) {
			/* Speaker portrait: fixed above the text box's left corner, unaffected by camera effects. */
			int face_in_box = config.num[NUM_FACE_BOX]
				&& display_options_use_custom(&display_options, current_textbox_available());
			if (!display_options.nvl && !face_in_box) {
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

static void draw_textbox(void)
{
	GameConfig defaults;
	config_defaults(&defaults);
	int custom = display_options_use_custom(&display_options, current_textbox_available());
	const int *n = custom ? config.num : defaults.num;
	const float bx = display_options.nvl ? 8.0f : (float)n[NUM_TEXTBOX_X];
	const float by = display_options.nvl ? 8.0f : (float)n[NUM_TEXTBOX_Y];
	const float bw = display_options.nvl ? 384.0f : (float)n[NUM_TEXTBOX_W];
	const float bh = display_options.nvl ? 224.0f : (float)n[NUM_TEXTBOX_H];
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
	if (custom && !display_options.nvl && n[NUM_FACE_BOX]) {
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
	char shown[GS_TEXT_LEN];
	size_t length = utf8_prefix(event.text, (size_t)chars_shown);
	memcpy(shown, event.text, length);
	shown[length] = '\0';
	const char *visible_text = shown;
	if (config.num[NUM_NVL_ACCUMULATE] && !display_options.nvl) {
		while (*visible_text == '\n' || *visible_text == '\r') ++visible_text;
	}
	Picture *advance = advance_available ? ui_image(IMG_ADVANCE) : NULL;
	if (advance != NULL) {
		draw_dialogue_with_advance(visible_text, bx + text_x, by + text_y, width, scale, color,
			advance, event.text[length] == '\0');
	} else {
		draw_wrapped(visible_text, bx + text_x, by + text_y, width, scale, color, Z_TEXT);
	}
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

static void draw_game_menu(void)
{
	draw_backdrop(IMG_MENU_BG, 225);
	const float row = menu_count > 5 ? 29.0f : 34.0f;
	const float height = menu_count > 5 ? 25.0f : 28.0f;
	draw_line(config.menu_title[0] != '\0' ? config.menu_title : "Menu", 60, 14, 0.8f,
		theme_accent(), Z_OVERLAY_TEXT, TOP_W - 120);
	float y = menu_count > 5 ? 48.0f : 62.0f;
	for (int index = 0; index < menu_count; ++index, y += row) {
		int selected = index == menu_cursor_item;
		draw_panel(selected ? IMG_ITEM_SEL : IMG_ITEM, 70, y, TOP_W - 140, height,
			selected ? theme_accent() : theme_panel(215), Z_OVERLAY);
		draw_line(config.labels[menu_items[index]], 82, y + 5, 0.55f,
			selected && !ui_image(IMG_ITEM_SEL) ? theme_dark_text() : theme_text(), Z_OVERLAY_TEXT, TOP_W - 164);
	}
	draw_line("D-pad choose   A confirm   B back", 70, 224, 0.4f, C2D_Color32(150, 170, 165, 255),
		Z_OVERLAY_TEXT, 0.0f);
}

static void draw_options(void)
{
	draw_backdrop(IMG_MENU_BG, 225);
	draw_line(config.labels[LABEL_OPTIONS], 40, 14, 0.7f, theme_accent(), Z_OVERLAY_TEXT, 320);
	const char *labels[] = {"Textbox", "Display style"};
	const char *values[] = {display_options_use_custom(&display_options, current_textbox_available()) ? "Custom" : "Default",
		display_options.nvl ? "NVL" : "ADV"};
	for (int i = 0; i < 2; ++i) {
		float y = 66.0f + i * 48.0f;
		int selected = i == panel_cursor;
		draw_panel(selected ? IMG_ITEM_SEL : IMG_ITEM, 40, y, 320, 36,
			selected ? theme_accent() : theme_panel(215), Z_OVERLAY);
		u32 color = selected && !ui_image(IMG_ITEM_SEL) ? theme_dark_text() : theme_text();
		draw_line(labels[i], 52, y + 9, 0.5f, color, Z_OVERLAY_TEXT, 155);
		draw_line(values[i], 216, y + 9, 0.5f, color, Z_OVERLAY_TEXT, 130);
	}
	draw_line("A / Left / Right change   B back", 40, 213, 0.4f,
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
	for (int index = 0; index < title_count; ++index) {
		draw_list_row(76.0f + (float)index * 34.0f,
			title_item_label(title_items[index]), index == title_cursor);
	}
	draw_line("D-pad choose   A confirm   B game list", 40, 224, 0.4f,
		C2D_Color32(150, 170, 165, 255), Z_OVERLAY_TEXT, 0.0f);
}

static void draw_top(C3D_RenderTarget *target)
{
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
	} else if (ui_hidden) {
		return;
	} else if (waiting == PLAY_MENU) {
		draw_choices();
	} else if (waiting == PLAY_TEXT) {
		draw_textbox();
	} else if (display_options.nvl && config.num[NUM_NVL_ACCUMULATE] && nvl_page.length
		&& (waiting == PLAY_TIMER || waiting == PLAY_WAITBG)) {
		draw_textbox();
	} else if (waiting == PLAY_END) {
		draw_line("THE END", 150, 100, 0.8f, C2D_Color32(255, 255, 255, 255), Z_TEXT, 0.0f);
	}
	if (fast_forward && panel == PANEL_NONE && waiting != PLAY_MENU && waiting != PLAY_END) {
		draw_line(">> FAST FORWARD", 290, 4, 0.45f, COLOR_ACCENT, Z_HEADER_TEXT, 0.0f);
	}
}

static const Button play_buttons[] = {
	{14, 44, 142, 52, "Menu  (X)"},
	{164, 44, 142, 52, "Backlog  (L)"},
	{14, 104, 142, 52, "Hide text  (B)"},
	{164, 104, 142, 52, "Quit  (SELECT)"},
};
#define COUNT_OF(array) (int)(sizeof(array) / sizeof((array)[0]))

static void draw_button_box(const Button *button, int selected)
{
	int skinned = ui_image(selected ? IMG_ITEM_SEL : IMG_ITEM) != NULL;
	draw_panel(selected ? IMG_ITEM_SEL : IMG_ITEM, button->x, button->y, button->w, button->h,
		selected ? theme_accent() : theme_item(), Z_BOX);
	draw_line(button->label, button->x + 12, button->y + button->h / 2.0f - 9.0f, 0.55f,
		selected && !skinned ? theme_dark_text() : theme_text(), Z_TEXT, button->w - 24);
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

static void draw_slot_list(void)
{
	draw_backdrop(IMG_MENU_BG, 225);
	draw_line(panel == PANEL_SAVE ? "Save to which slot?" : "Load which slot?", 40, 6, 0.55f,
		theme_accent(), Z_OVERLAY_TEXT, TOP_W - 80);
	int first = list_first(panel_cursor, SAVE_SLOTS);
	for (int slot = first; slot < first + LIST_ROWS; ++slot) {
		char label[160];
		if (slots[slot].used) {
			char when[24] = "";
			time_t saved = (time_t)slots[slot].saved_at;
			struct tm *parts = localtime(&saved);
			if (parts != NULL) {
				strftime(when, sizeof(when), "%m-%d %H:%M", parts);
			}
			char preview[96];
			size_t length = utf8_prefix(slots[slot].preview, 30);
			memcpy(preview, slots[slot].preview, length);
			preview[length] = '\0';
			snprintf(label, sizeof(label), "%d   %s   %s", slot + 1, when, preview);
		} else {
			snprintf(label, sizeof(label), "%d   - empty -", slot + 1);
		}
		draw_list_row(30.0f + (float)(slot - first) * 25.0f, label, slot == panel_cursor);
	}
	char footer[48];
	snprintf(footer, sizeof(footer), "A select   B back   %d/%d", panel_cursor + 1, SAVE_SLOTS);
	draw_line(footer, 40, 224, 0.4f, C2D_Color32(150, 170, 165, 255), Z_OVERLAY_TEXT, 0.0f);
}

static void draw_quit_confirm(void)
{
	draw_backdrop(IMG_MENU_BG, 225);
	draw_line("Quit to the game list?", 70, 80, 0.6f, theme_accent(), Z_OVERLAY_TEXT, TOP_W - 140);
	draw_line("Unsaved progress will be lost.", 70, 108, 0.45f, theme_text(), Z_OVERLAY_TEXT, TOP_W - 140);
	draw_line("A  Yes        B  No", 70, 140, 0.5f, theme_text(), Z_OVERLAY_TEXT, TOP_W - 140);
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
		int first = select_cursor >= 7 ? select_cursor - 6 : 0;
		for (int index = first; index < game_count && index < first + 7; ++index) {
			rect(14, 30.0f + (float)(index - first) * 28.0f, BOTTOM_W - 28, 24,
				index == select_cursor ? COLOR_ACCENT : C2D_Color32(40, 62, 66, 255), Z_BOX);
			draw_line(game_names[index], 22, 33.0f + (float)(index - first) * 28.0f, 0.45f,
				index == select_cursor ? C2D_Color32(10, 16, 18, 255) : C2D_Color32(220, 230, 225, 255),
				Z_TEXT, BOTTOM_W - 44);
		}
		draw_line("A select   START exit", 14, 224, 0.4f, dim, Z_TEXT, 0.0f);
		return;
	}
	if (mode == MODE_ERROR) {
		draw_line("Script error", 14, 6, 0.55f, C2D_Color32(255, 140, 120, 255), Z_TEXT, 0.0f);
		draw_wrapped(message, 14, 34, BOTTOM_W - 28, 0.45f, white, Z_TEXT);
		draw_line(extras_mode ? "B return to title" : "B back to list",
			14, 224, 0.4f, dim, Z_TEXT, 0.0f);
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

	if (panel != PANEL_NONE) {
		draw_line(game_name, 14, 6, 0.55f, theme_accent(), Z_TEXT, BOTTOM_W - 28);
		draw_line("Menu is on the top screen", 14, 40, 0.45f, dim, Z_TEXT, 0.0f);
		if (notice[0] != '\0') {
			draw_line(notice, 14, 212, 0.45f, C2D_Color32(255, 200, 120, 255), Z_TEXT, BOTTOM_W - 28);
		}
		return;
	}

	draw_line(game_name, 14, 6, 0.55f, theme_accent(), Z_TEXT, BOTTOM_W - 28);
	for (int index = 0; index < COUNT_OF(play_buttons); ++index) {
		draw_button_box(&play_buttons[index], 0);
	}
	draw_line(waiting == PLAY_MENU ? "D-pad choose   A confirm" : "A next   Hold R fast-forward",
		14, 176, 0.42f, dim, Z_TEXT, 0.0f);
	draw_line(ui_hidden ? "Text hidden - press B or A" : "START exit app", 14, 192, 0.42f, dim, Z_TEXT, 0.0f);
	if (notice[0] != '\0') {
		draw_line(notice, 14, 212, 0.45f, C2D_Color32(255, 200, 120, 255), Z_TEXT, BOTTOM_W - 28);
	}
}

/* ---------- input ---------- */

static void leave_game(void)
{
	extras_mode = 0;
	extras_available = 0;
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
			LABEL_OPTIONS, LABEL_QUIT};
		for (int i = 0; i < LABEL_COUNT; ++i) {
			int item = order[i];
			if (item == LABEL_GLOSSARY && glossary.count == 0) {
				continue;
			}
			menu_items[menu_count++] = item;
		}
		menu_cursor_item = 0;
	} else if (which == PANEL_BACKLOG) {
		backlog_view = 0;
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
	case TITLE_OPTIONS:
		open_panel(PANEL_OPTIONS);
		break;
	case TITLE_EXTRAS:
		start_extras();
		break;
	}
}

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
		}
		break;
	}
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
		if (repeat & (KEY_DUP | KEY_DDOWN)) {
			panel_cursor = 1 - panel_cursor;
		}
		if (down & (KEY_A | KEY_DLEFT | KEY_DRIGHT)) {
			toggle_display_option();
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
		}
		break;
	}
	case PANEL_QUIT: {
		if (down & KEY_A) {
			leave_game();
		} else if (down & KEY_B) {
			close_panel();
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
	if (down & KEY_SELECT) {
		open_panel(PANEL_QUIT);
		return;
	}
	if (down & KEY_TOUCH) {
		touchPosition touch;
		hidTouchRead(&touch);
		static const Panel targets[] = {PANEL_MENU, PANEL_BACKLOG, PANEL_NONE, PANEL_QUIT};
		int button = hit_button(play_buttons, COUNT_OF(play_buttons), &touch);
		if (button == 2) {
			ui_hidden = !ui_hidden;
			return;
		}
		if (button >= 0) {
			open_panel(targets[button]);
			return;
		}
	}
	if (ui_hidden) {
		fast_forward = 0;
		if (down & KEY_A) {
			ui_hidden = 0;
		}
		return;
	}
	fast_forward = (held & KEY_R) != 0;
	if (!fast_forward) {
		ff_wait = 0.0f;
	}

	switch (waiting) {
	case PLAY_TEXT: {
		size_t total = 0;
		for (const char *c = event.text; *c != '\0'; ++c) {
			total += ((unsigned char)*c & 0xC0) != 0x80;
		}
		chars_shown += TEXT_CHARS_PER_SEC * dt;
		if (fast_forward) {
			chars_shown = (float)total;
			ff_wait += dt;
			if (ff_wait >= FF_STEP) {
				ff_wait = 0.0f;
				audio_stop_sfx();
				waiting = PLAY_NONE;
			}
		} else if (down & KEY_A) {
			if (chars_shown < (float)total) {
				chars_shown = (float)total;
			} else {
				audio_stop_sfx();
				waiting = PLAY_NONE;
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
		if ((down & KEY_A) && menu_cursor < event.choice_count) {
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
		u32 now = now_ms();
		float dt = (float)(now - last) / 1000.0f;
		last = now;

		if (down & KEY_START) {
			break;
		}

		audio_update(dt);

		if (mode == MODE_SELECT) {
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
					int first = select_cursor >= 7 ? select_cursor - 6 : 0;
					int index = first + ((int)touch.py - 30) / 28;
					if (touch.py >= 30 && index < game_count) {
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
		}

		if (strcmp(notice, last_notice) != 0) {
			snprintf(last_notice, sizeof(last_notice), "%s", notice);
			notice_age = 0.0f;
		} else if (notice[0] != '\0' && (notice_age += dt) > 4.0f) {
			notice[0] = '\0';
			last_notice[0] = '\0';
		}

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		draw_top(top);
		draw_bottom(bottom);
		C3D_FrameEnd(0);
	}

	stop_video();
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
