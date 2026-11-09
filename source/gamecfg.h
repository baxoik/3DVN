#ifndef GAMECFG_H
#define GAMECFG_H

#include "gs.h"

/* Per-game menu appearance (game.3vc) and glossary (glossary.3vg) parsing. */

#define CFG_MAX_TERMS 64
#define CFG_TERM_LEN 48
#define CFG_TEXT_LEN 500
#define CFG_LABEL_LEN 32
#define CFG_PATH_LEN 128

enum { LABEL_SAVE, LABEL_LOAD, LABEL_BACKLOG, LABEL_GLOSSARY, LABEL_QUIT, LABEL_OPTIONS, LABEL_COUNT };

typedef struct {
	char name[CFG_TERM_LEN];
	char text[CFG_TEXT_LEN + 1];
	char unlock[GS_NAME_LEN];
	int unlock_invalid;
} GlossaryTerm;

typedef struct {
	int count;
	GlossaryTerm terms[CFG_MAX_TERMS];
} Glossary;

enum {
	IMG_MENU_BG, IMG_BACKLOG_BG, IMG_BOTTOM_BG, IMG_TEXTBOX, IMG_NAMEBOX,
	IMG_CHOICE, IMG_CHOICE_SEL, IMG_ITEM, IMG_ITEM_SEL, IMG_NVL_TEXTBOX, IMG_ADVANCE, IMG_COUNT
};

enum {
	NUM_TEXTBOX_X, NUM_TEXTBOX_Y, NUM_TEXTBOX_W, NUM_TEXTBOX_H, NUM_TEXTBOX_ALPHA,
	NUM_TEXT_X, NUM_TEXT_Y, NUM_NARRATION_Y, NUM_TEXT_SIZE,
	NUM_NAME_X, NUM_NAME_Y, NUM_NAME_SIZE, NUM_NAMEBOX_X, NUM_NAMEBOX_Y,
	NUM_CHOICE_W, NUM_CHOICE_H, NUM_CHOICE_GAP, NUM_CHOICE_TEXT_X, NUM_CHOICE_SIZE, NUM_CHOICE_DIM,
	NUM_NVL_DIM, NUM_NVL_BORDER, NUM_NVL_MARGIN_X, NUM_NVL_MARGIN_Y,
	NUM_ADVANCE_FRAMES, NUM_ADVANCE_FRAME_MS, NUM_ADVANCE_SCALE,
	NUM_NVL_ACCUMULATE,
	NUM_COUNT
};

typedef struct {
	int has_accent, has_panel, has_text, has_bg;
	unsigned accent, panel, text, bg;  /* 0xRRGGBB */
	char images[IMG_COUNT][CFG_PATH_LEN];
	int num[NUM_COUNT];
	char font[CFG_PATH_LEN];
	char menu_title[CFG_TERM_LEN];
	char labels[LABEL_COUNT][CFG_LABEL_LEN];
	int default_nvl;
} GameConfig;

void config_defaults(GameConfig *config);
/* Unknown keys and bad values are ignored; defaults stay in place. */
void config_parse(const char *source, GameConfig *config);

void glossary_parse(const char *source, Glossary *glossary);
/* Returns the first invalid unlock directive's line, or zero. Invalid terms stay hidden. */
int glossary_parse_checked(const char *source, Glossary *glossary);
int glossary_visible_indices(const Glossary *glossary, const GsState *state, int *indices);

#endif
