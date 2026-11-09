#ifndef GS_H
#define GS_H

/*
 * 3DVN Script: a small, platform-independent visual novel script language.
 * The VM never draws or plays anything; it yields events for a frontend.
 */

#include <stddef.h>

#define GS_MAX_VARS 128
#define GS_MAX_CALLS 32
#define GS_MAX_CHOICES 8
#define GS_NAME_LEN 32
#define GS_STR_LEN 256
#define GS_TEXT_LEN 512

typedef enum {
	GS_EV_END,
	GS_EV_SAY,
	GS_EV_MENU,
	GS_EV_BG,
	GS_EV_SHOW,
	GS_EV_HIDE,
	GS_EV_MUSIC,
	GS_EV_SFX,
	GS_EV_WAIT,
	GS_EV_ERROR,
	GS_EV_VIDEO, /* asset = video name; seconds = 1 when the player may not skip it */
	GS_EV_BGLOOP, /* asset = base; choices = overlays; seconds = hold; pos = loop fade; text = transition fade */
	GS_EV_WAITBG, /* seconds = 1 if skippable */
	GS_EV_PAGE /* clear the accumulated NVL page without waiting */
} GsEventType;

typedef struct {
	GsEventType type;
	char speaker[GS_NAME_LEN];  /* display name; empty for narration */
	unsigned color;             /* 0xRRGGBB speaker color, 0 if unset */
	char text[GS_TEXT_LEN];     /* dialogue, or error message */
	char name[GS_NAME_LEN];     /* sprite slot id */
	char asset[GS_STR_LEN];     /* image or audio file name; empty stops music */
	char pos[GS_NAME_LEN];      /* left, center, right, ... */
	float seconds;              /* fade or wait duration */
	int choice_count;
	char choices[GS_MAX_CHOICES][GS_STR_LEN];
	int line;                   /* source line for errors */
} GsEvent;

typedef struct {
	int is_str;
	long num;
	char str[GS_STR_LEN];
} GsValue;

typedef struct {
	char name[GS_NAME_LEN];
	GsValue value;
} GsVar;

/* Everything that changes while a script runs; plain data, safe to memcpy for saves. */
typedef struct {
	int pc;
	int var_count;
	GsVar vars[GS_MAX_VARS];
	int call_depth;
	int call_stack[GS_MAX_CALLS];
	int pending_menu;           /* menu id awaiting gs_choose, or -1 */
	int pending_map[GS_MAX_CHOICES];
	int pending_count;
} GsState;

typedef struct GsProgram GsProgram;

/* Returns the text of an included file as a malloc'd string (the compiler frees it), or NULL. */
typedef char *(*GsLoader)(const char *name, void *ctx);

/* Compile script text. On failure returns NULL and writes a message to err. */
GsProgram *gs_compile(const char *source, char *err, size_t err_size);
/* Like gs_compile, but 'include FILE' lines are filled in through the loader. */
GsProgram *gs_compile_ex(const char *source, GsLoader loader, void *loader_ctx, char *err, size_t err_size);
void gs_free(GsProgram *program);

void gs_state_init(const GsProgram *program, GsState *state);
/* An unset variable is false; strings and numbers use script truthiness. */
int gs_variable_truthy(const GsState *state, const char *name);

/* Run until the next event. After a GS_EV_MENU, call gs_choose before gs_next. */
void gs_next(const GsProgram *program, GsState *state, GsEvent *event);
int gs_choose(const GsProgram *program, GsState *state, int index);

/* Checks a state loaded from untrusted data (e.g. a save file) against a program. */
int gs_state_valid(const GsProgram *program, const GsState *state);

/* Advances a background fade and returns the elapsed time after it finished. */
float gs_bg_fade_advance(float *elapsed, float duration, float seconds);
float gs_bg_fade_alpha(float elapsed, float duration);

/* Loop state is separate from GsState and GsEvent to preserve old save layouts. */
#define GS_BGLOOP_V1_SAVE_SIZE (12 + 2 * GS_STR_LEN)
#define GS_BGLOOP_SAVE_SIZE (GS_BGLOOP_V1_SAVE_SIZE + 8)
typedef struct {
	int active;
	char overlays[2][GS_STR_LEN];
	float phase, hold, fade;
} GsBgLoop;

void gs_bgloop_update(GsBgLoop *loop, float seconds);
void gs_bgloop_alpha(const GsBgLoop *loop, float *first, float *second);
int gs_bgloop_encode(const GsBgLoop *loop, unsigned char data[GS_BGLOOP_SAVE_SIZE]);
int gs_bgloop_decode(GsBgLoop *loop, const unsigned char data[GS_BGLOOP_SAVE_SIZE]);
/* A zero-length extension is an old save; any other size must be exact. */
int gs_bgloop_decode_optional(GsBgLoop *loop, const unsigned char *data, size_t size);

#endif
