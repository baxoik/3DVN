#ifndef VN_SAVEFILE_H
#define VN_SAVEFILE_H

/* Save files: one format for every console and computer.

   A save is written field by field at fixed offsets, little-endian, SAVE_FILE_SIZE bytes
   (then optional extra blocks, see main.c), so the 3DS, the Wii U, a PC or Android can
   read each other's saves whatever their byte order or the size of `long` there. The
   layout is the 3DS's in-memory one, which earlier builds wrote directly:

     0      header: magic, script fingerprint, time saved (int64), preview[72]
     88     GsState: pc, var_count, vars[128] {name[32], is_str, num, str[256]},
            call_depth, call_stack[32], pending_menu, pending_map[8], pending_count
     38156  Scene: bg, bg_prev, music [256 each], bg_elapsed, bg_duration,
            sprites[8] {active, name[32], asset[256], pos[32], alpha, from, to,
            duration, elapsed}
     41684  GsEvent: type (1 byte), speaker[32], 3 bytes padding, color, text[512],
            name[32], asset[256], pos[32], seconds, choice_count, choices[8][256], line
     44616  Backlog: count, start, entries[40] {speaker[32], text[512], color}
     66544  end

   Saves from Wii U builds before 2026-11-22 are the same layout big-endian, except that
   the event type there is 4 bytes and the speaker starts at 4; save_decode reads those. */

#include <stddef.h>
#include "gs.h"

#define MAX_SPRITES 8
#define BACKLOG_MAX 40
#define SAVE_MAGIC 0x32564433u
#define SAVE_FILE_SIZE 66544u
#define SAVE_HEADER_SIZE 88u

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

/* Writes the first SAVE_FILE_SIZE bytes of a save file. */
void save_encode(const SaveData *save, unsigned char out[SAVE_FILE_SIZE]);

/* Reads a save written by any build (either byte order). 1 on success, 0 if not a save. */
int save_decode(const unsigned char *data, size_t size, SaveData *save);

/* Reads only the header (needs SAVE_HEADER_SIZE bytes). 1 on success, 0 if not a save. */
int save_peek(const unsigned char *data, size_t size, SaveHeader *header);

#endif
