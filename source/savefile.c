#include "savefile.h"

#include <string.h>

/* A cursor over the file: little-endian for current saves, big-endian for older Wii U
   ones. Writes and reads go through the same field list (code_save), so the two can't
   drift apart. */
typedef struct {
	unsigned char *bytes;
	size_t at;
	int writing, big_endian, old_wiiu;
} Cursor;

static void raw(Cursor *c, void *field, size_t size)
{
	if (c->writing) memcpy(c->bytes + c->at, field, size);
	else memcpy(field, c->bytes + c->at, size);
	c->at += size;
}

static void text(Cursor *c, char *field, size_t size)
{
	if (c->writing) {
		size_t length = strnlen(field, size);
		memset(c->bytes + c->at, 0, size);
		memcpy(c->bytes + c->at, field, length);
	} else {
		memcpy(field, c->bytes + c->at, size);
		field[size - 1] = '\0';
	}
	c->at += size;
}

static void u32(Cursor *c, unsigned *value)
{
	unsigned char *b = c->bytes + c->at;
	if (c->writing) {
		unsigned v = *value;
		for (int i = 0; i < 4; ++i) b[c->big_endian ? 3 - i : i] = (unsigned char)(v >> (8 * i));
	} else {
		unsigned v = 0;
		for (int i = 0; i < 4; ++i) v |= (unsigned)b[c->big_endian ? 3 - i : i] << (8 * i);
		*value = v;
	}
	c->at += 4;
}

static void i32(Cursor *c, int *value)
{
	unsigned v = (unsigned)*value;
	u32(c, &v);
	*value = (int)v;
}

static void num32(Cursor *c, long *value)   /* `long` is 32 or 64 bits; the file keeps 32 */
{
	int v = (int)*value;
	i32(c, &v);
	*value = v;
}

static void f32(Cursor *c, float *value)
{
	unsigned v;
	memcpy(&v, value, 4);
	u32(c, &v);
	memcpy(value, &v, 4);
}

static void i64(Cursor *c, long long *value)
{
	unsigned lo = (unsigned)((unsigned long long)*value & 0xFFFFFFFFu);
	unsigned hi = (unsigned)((unsigned long long)*value >> 32);
	if (c->big_endian) {
		u32(c, &hi);
		u32(c, &lo);
	} else {
		u32(c, &lo);
		u32(c, &hi);
	}
	*value = (long long)(((unsigned long long)hi << 32) | lo);
}

static void code_header(Cursor *c, SaveHeader *h)
{
	u32(c, &h->magic);
	u32(c, &h->script_hash);
	i64(c, &h->saved_at);
	text(c, h->preview, sizeof(h->preview));
}

static void code_event_head(Cursor *c, GsEvent *e)
{
	size_t start = c->at;
	if (c->old_wiiu) {   /* 4-byte type, speaker at 4 */
		unsigned type = (unsigned)e->type;
		u32(c, &type);
		e->type = (GsEventType)type;
		text(c, e->speaker, sizeof(e->speaker));
	} else {             /* 1-byte type, speaker at 1, padding to 36 */
		unsigned char type = (unsigned char)e->type;
		if (c->writing) memset(c->bytes + c->at, 0, 36);
		raw(c, &type, 1);
		e->type = (GsEventType)type;
		text(c, e->speaker, sizeof(e->speaker));
	}
	c->at = start + 36;
}

static void code_save(Cursor *c, SaveData *s)
{
	code_header(c, &s->header);

	GsState *st = &s->state;
	i32(c, &st->pc);
	int count = st->var_count;   /* the main block holds up to SAVE_FILE_VARS; see VARX */
	if (c->writing && count > SAVE_FILE_VARS) count = SAVE_FILE_VARS;
	i32(c, &count);
	if (!c->writing) st->var_count = count;
	for (int i = 0; i < SAVE_FILE_VARS; ++i) {
		GsVar *var = &st->vars[i];
		text(c, var->name, sizeof(var->name));
		i32(c, &var->value.is_str);
		num32(c, &var->value.num);
		text(c, var->value.str, sizeof(var->value.str));
	}
	i32(c, &st->call_depth);
	for (int i = 0; i < GS_MAX_CALLS; ++i) i32(c, &st->call_stack[i]);
	i32(c, &st->pending_menu);
	for (int i = 0; i < GS_MAX_CHOICES; ++i) i32(c, &st->pending_map[i]);
	i32(c, &st->pending_count);

	Scene *sc = &s->scene;
	text(c, sc->bg, sizeof(sc->bg));
	text(c, sc->bg_prev, sizeof(sc->bg_prev));
	text(c, sc->music, sizeof(sc->music));
	f32(c, &sc->bg_elapsed);
	f32(c, &sc->bg_duration);
	for (int i = 0; i < MAX_SPRITES; ++i) {
		Sprite *sp = &sc->sprites[i];
		i32(c, &sp->active);
		text(c, sp->name, sizeof(sp->name));
		text(c, sp->asset, sizeof(sp->asset));
		text(c, sp->pos, sizeof(sp->pos));
		f32(c, &sp->alpha);
		f32(c, &sp->from);
		f32(c, &sp->to);
		f32(c, &sp->duration);
		f32(c, &sp->elapsed);
	}

	GsEvent *e = &s->event;
	code_event_head(c, e);
	u32(c, &e->color);
	text(c, e->text, sizeof(e->text));
	text(c, e->name, sizeof(e->name));
	text(c, e->asset, sizeof(e->asset));
	text(c, e->pos, sizeof(e->pos));
	f32(c, &e->seconds);
	i32(c, &e->choice_count);
	for (int i = 0; i < GS_MAX_CHOICES; ++i) text(c, e->choices[i], sizeof(e->choices[i]));
	i32(c, &e->line);

	Backlog *log = &s->backlog;
	i32(c, &log->count);
	i32(c, &log->start);
	for (int i = 0; i < BACKLOG_MAX; ++i) {
		BacklogEntry *entry = &log->entries[i];
		text(c, entry->speaker, sizeof(entry->speaker));
		text(c, entry->text, sizeof(entry->text));
		u32(c, &entry->color);
	}
}

void save_encode(const SaveData *save, unsigned char out[SAVE_FILE_SIZE])
{
	Cursor c = {out, 0, 1, 0, 0};
	memset(out, 0, SAVE_FILE_SIZE);
	code_save(&c, (SaveData *)save);   /* writing does not change the struct */
}

/* 1 = little-endian (current), 2 = big-endian (older Wii U), 0 = not a save */
static int save_order(const unsigned char *data, size_t size)
{
	if (size < 4) return 0;
	unsigned le = data[0] | data[1] << 8 | data[2] << 16 | (unsigned)data[3] << 24;
	unsigned be = data[3] | data[2] << 8 | data[1] << 16 | (unsigned)data[0] << 24;
	return le == SAVE_MAGIC ? 1 : be == SAVE_MAGIC ? 2 : 0;
}

int save_decode(const unsigned char *data, size_t size, SaveData *save)
{
	int order = save_order(data, size);
	if (order == 0 || size < SAVE_FILE_SIZE) return 0;
	memset(save, 0, sizeof(*save));
	Cursor c = {(unsigned char *)data, 0, 0, order == 2, order == 2};
	code_save(&c, save);
	return c.at == SAVE_FILE_SIZE;
}

size_t save_encode_vars(const SaveData *save, unsigned char *out, size_t cap)
{
	const GsState *st = &save->state;
	if (st->var_count <= SAVE_FILE_VARS) return 0;
	size_t used = 8;
	if (cap < used) return 0;
	memcpy(out, "VARX", 4);
	unsigned count = (unsigned)(st->var_count - SAVE_FILE_VARS);
	for (int i = 0; i < 4; ++i) out[4 + i] = (unsigned char)(count >> (8 * i));
	for (int v = SAVE_FILE_VARS; v < st->var_count; ++v) {
		const GsVar *var = &st->vars[v];
		size_t name = strnlen(var->name, sizeof(var->name));
		size_t text = var->value.is_str ? strnlen(var->value.str, sizeof(var->value.str)) : 0;
		size_t need = 1 + name + 1 + (var->value.is_str ? 2 + text : 4);
		if (used + need > cap) return 0;
		out[used++] = (unsigned char)name;
		memcpy(out + used, var->name, name);
		used += name;
		out[used++] = (unsigned char)(var->value.is_str ? 1 : 0);
		if (var->value.is_str) {
			out[used++] = (unsigned char)text;
			out[used++] = (unsigned char)(text >> 8);
			memcpy(out + used, var->value.str, text);
			used += text;
		} else {
			unsigned n = (unsigned)(int)var->value.num;
			for (int i = 0; i < 4; ++i) out[used++] = (unsigned char)(n >> (8 * i));
		}
	}
	return used;
}

long save_decode_vars(const unsigned char *data, size_t size, SaveData *save)
{
	if (size < 8 || memcmp(data, "VARX", 4) != 0) return 0;
	unsigned count = data[4] | data[5] << 8 | data[6] << 16 | (unsigned)data[7] << 24;
	size_t at = 8;
	GsState *st = &save->state;
	for (unsigned k = 0; k < count; ++k) {
		if (at + 2 > size || st->var_count >= GS_MAX_VARS) return -1;
		size_t name = data[at++];
		if (name >= GS_NAME_LEN || at + name + 1 > size) return -1;
		GsVar *var = &st->vars[st->var_count];
		memset(var, 0, sizeof(*var));
		memcpy(var->name, data + at, name);
		at += name;
		var->value.is_str = data[at++] != 0;
		if (var->value.is_str) {
			if (at + 2 > size) return -1;
			size_t text = data[at] | data[at + 1] << 8;
			at += 2;
			if (text >= GS_STR_LEN || at + text > size) return -1;
			memcpy(var->value.str, data + at, text);
			at += text;
		} else {
			if (at + 4 > size) return -1;
			unsigned n = data[at] | data[at + 1] << 8 | data[at + 2] << 16 | (unsigned)data[at + 3] << 24;
			var->value.num = (int)n;
			at += 4;
		}
		st->var_count++;
	}
	return (long)at;
}

int save_peek(const unsigned char *data, size_t size, SaveHeader *header)
{
	int order = save_order(data, size);
	if (order == 0 || size < SAVE_HEADER_SIZE) return 0;
	memset(header, 0, sizeof(*header));
	Cursor c = {(unsigned char *)data, 0, 0, order == 2, order == 2};
	code_header(&c, header);
	return 1;
}
