#include "gs.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

float gs_bg_fade_alpha(float elapsed, float duration)
{
	if (duration <= 0.0f || elapsed >= duration) return 1.0f;
	return elapsed > 0.0f ? elapsed / duration : 0.0f;
}

float gs_bg_fade_advance(float *elapsed, float duration, float seconds)
{
	if (seconds <= 0.0f) return 0.0f;
	float remaining = duration - *elapsed;
	if (remaining <= 0.0f) return seconds;
	if (seconds < remaining) {
		*elapsed += seconds;
		return 0.0f;
	}
	*elapsed = duration;
	return seconds - remaining;
}

#define MAX_TOKENS 16
#define MAX_FRAMES 32
#define MAX_PATCHES 64
#define MAX_INCLUDE_DEPTH 8
#define STEP_LIMIT 1000000
#define BRACE_MARK '\x01'

void gs_bgloop_update(GsBgLoop *loop, float seconds)
{
	double cycle = 3.0 * ((double)loop->hold + loop->fade);
	if (loop->active && isfinite(seconds) && seconds > 0.0f && isfinite(loop->phase)
		&& isfinite(cycle) && cycle > 0.0) {
		double phase = (double)loop->phase + seconds;
		double period = cycle;
		while (period <= phase / 2.0) period *= 2.0;
		while (period >= cycle) {
			if (phase >= period) phase -= period;
			period *= 0.5;
		}
		loop->phase = (float)phase;
		if ((double)loop->phase >= cycle) loop->phase = 0.0f;
	}
}

static float loop_fade(float phase, float start, float hold, float fade)
{
	if (phase < start || phase >= start + hold + 2.0f * fade) return 0.0f;
	if (phase < start + fade) return (phase - start) / fade;
	if (phase < start + fade + hold) return 1.0f;
	float alpha = (start + hold + 2.0f * fade - phase) / fade;
	return alpha > 1.0f ? 1.0f : alpha;
}

void gs_bgloop_alpha(const GsBgLoop *loop, float *first, float *second)
{
	*first = loop->active && loop->fade > 0.0f
		? loop_fade(loop->phase, loop->hold, loop->hold, loop->fade) : 0.0f;
	*second = loop->active && loop->fade > 0.0f
		? loop_fade(loop->phase, 2.0f * loop->hold + loop->fade, loop->hold, loop->fade) : 0.0f;
}

static unsigned read_u32(const unsigned char *data)
{
	return (unsigned)data[0] | (unsigned)data[1] << 8
		| (unsigned)data[2] << 16 | (unsigned)data[3] << 24;
}

static void write_u32(unsigned char *data, unsigned value)
{
	for (int i = 0; i < 4; ++i) data[i] = (unsigned char)(value >> (8 * i));
}

static int valid_timing(float hold, float fade)
{
	return isfinite(hold) && hold >= 0.0f && hold <= 600.0f
		&& isfinite(fade) && fade >= 0.001f && fade <= 600.0f;
}

int gs_bgloop_encode(const GsBgLoop *loop, unsigned char data[GS_BGLOOP_SAVE_SIZE])
{
	memset(data, 0, GS_BGLOOP_SAVE_SIZE);
	memcpy(data, "BGL2", 4);
	if (!loop->active) return loop->active == 0;
	if (loop->active != 1 || !valid_timing(loop->hold, loop->fade)
		|| !isfinite(loop->phase) || loop->phase < 0.0f
		|| (double)loop->phase >= 3.0 * ((double)loop->hold + loop->fade)
		|| !loop->overlays[0][0] || !loop->overlays[1][0]
		|| !memchr(loop->overlays[0], 0, GS_STR_LEN)
		|| !memchr(loop->overlays[1], 0, GS_STR_LEN)) return 0;
	data[4] = 1;
	unsigned hold = (unsigned)((double)loop->hold * 1000000.0 + 0.5);
	unsigned fade = (unsigned)((double)loop->fade * 1000000.0 + 0.5);
	unsigned phase = (unsigned)((double)loop->phase * 1000000.0);
	unsigned cycle = 3 * (hold + fade);
	if (phase >= cycle) phase = cycle - 1;
	write_u32(data + 8, phase);
	for (int i = 0; i < 2; ++i)
		memcpy(data + 12 + i * GS_STR_LEN, loop->overlays[i], strlen(loop->overlays[i]) + 1);
	write_u32(data + GS_BGLOOP_V1_SAVE_SIZE, hold);
	write_u32(data + GS_BGLOOP_V1_SAVE_SIZE + 4, fade);
	return 1;
}

static int decode_bgloop(GsBgLoop *loop, const unsigned char *data, size_t size)
{
	int v1 = size == GS_BGLOOP_V1_SAVE_SIZE && !memcmp(data, "BGL1", 4);
	int v2 = size == GS_BGLOOP_SAVE_SIZE && !memcmp(data, "BGL2", 4);
	if ((!v1 && !v2) || data[4] > 1 || data[5] || data[6] || data[7]) return 0;
	unsigned phase = read_u32(data + 8);
	unsigned hold = v1 ? 450000 : read_u32(data + GS_BGLOOP_V1_SAVE_SIZE);
	unsigned fade = v1 ? 150000 : read_u32(data + GS_BGLOOP_V1_SAVE_SIZE + 4);
	if (data[4] && (hold > 600000000 || fade < 1000 || fade > 600000000
		|| phase >= 3u * (hold + fade))) return 0;
	if (data[4] && (!data[12] || !data[12 + GS_STR_LEN]
		|| !memchr(data + 12, 0, GS_STR_LEN)
		|| !memchr(data + 12 + GS_STR_LEN, 0, GS_STR_LEN))) return 0;
	if (!data[4]) {
		for (size_t i = 8; i < size; ++i) if (data[i]) return 0;
	}
	memset(loop, 0, sizeof(*loop));
	loop->active = data[4];
	if (loop->active) {
		loop->hold = hold / 1000000.0f;
		loop->fade = fade / 1000000.0f;
		loop->phase = phase / 1000000.0f;
		if ((double)loop->phase >= 3.0 * ((double)loop->hold + loop->fade))
			loop->phase = 0.0f;
		memcpy(loop->overlays, data + 12, sizeof(loop->overlays));
	}
	return 1;
}

int gs_bgloop_decode(GsBgLoop *loop, const unsigned char data[GS_BGLOOP_SAVE_SIZE])
{
	return decode_bgloop(loop, data, GS_BGLOOP_SAVE_SIZE);
}

int gs_bgloop_decode_optional(GsBgLoop *loop, const unsigned char *data, size_t size)
{
	if (size == 0) {
		memset(loop, 0, sizeof(*loop));
		return 1;
	}
	return data && (size == GS_BGLOOP_SAVE_SIZE || size == GS_BGLOOP_V1_SAVE_SIZE)
		&& decode_bgloop(loop, data, size);
}

typedef enum {
	OP_SAY, OP_BG, OP_SHOW, OP_HIDE, OP_MUSIC, OP_SFX, OP_VIDEO, OP_WAIT, OP_SET,
	OP_JUMP, OP_JUMPF, OP_CALL, OP_RET, OP_END, OP_MENU, OP_BGLOOP, OP_WAITBG,
	OP_PAGE, OP_SCRIPT, OP_EFFECT, OP_GALLERY
} Op;

typedef struct {
	Op op;
	int line;
	char *a, *b, *c;
	float seconds, loop_fade;
	float transition;
	int target;
	int menu;
} Instr;

typedef struct {
	int menu;
	char *text;
	char *cond;
	int target;
} Option;

typedef struct {
	char id[GS_NAME_LEN];
	char name[GS_NAME_LEN];
	unsigned color;
} Character;

typedef struct {
	char name[GS_NAME_LEN];
	int pc;
} Label;

struct GsProgram {
	Instr *code;
	int code_count, code_cap;
	Option *options;
	int option_count, option_cap;
	int menu_count;
	Character *characters;
	int character_count, character_cap;
	Label *labels;
	int label_count, label_cap;
};

typedef struct {
	int is_string;
	char text[GS_TEXT_LEN];
} Token;

typedef enum { FRAME_IF, FRAME_WHILE, FRAME_MENU } FrameKind;

typedef struct {
	FrameKind kind;
	int line;
	int cond_jump;
	int start;
	int menu;
	int in_choice;
	int saw_else;
	int patches[MAX_PATCHES];
	int patch_count;
} Frame;

/* ---------- small helpers ---------- */

static char *dup_string(const char *text)
{
	size_t length = strlen(text);
	char *copy = malloc(length + 1);
	if (copy != NULL) {
		memcpy(copy, text, length + 1);
	}
	return copy;
}

static void *grow(void *array, int count, int *capacity, size_t item_size)
{
	if (count < *capacity) {
		return array;
	}
	int new_capacity = *capacity ? *capacity * 2 : 64;
	void *resized = realloc(array, (size_t)new_capacity * item_size);
	if (resized != NULL) {
		*capacity = new_capacity;
	}
	return resized;
}

static void copy_text(char *destination, size_t size, const char *source)
{
	snprintf(destination, size, "%s", source ? source : "");
}

/* ---------- expressions ---------- */

typedef struct {
	const char *cursor;
	const GsState *state;
	char error[96];
} ExprContext;

static GsValue make_number(long number)
{
	GsValue value;
	memset(&value, 0, sizeof(value));
	value.num = number;
	return value;
}

static GsValue make_string(const char *text)
{
	GsValue value;
	memset(&value, 0, sizeof(value));
	value.is_str = 1;
	copy_text(value.str, sizeof(value.str), text);
	return value;
}

static const GsValue *find_var(const GsState *state, const char *name)
{
	for (int index = 0; index < state->var_count; ++index) {
		if (strcmp(state->vars[index].name, name) == 0) {
			return &state->vars[index].value;
		}
	}
	return NULL;
}

static int set_var(GsState *state, const char *name, const GsValue *value)
{
	for (int index = 0; index < state->var_count; ++index) {
		if (strcmp(state->vars[index].name, name) == 0) {
			state->vars[index].value = *value;
			return 1;
		}
	}
	if (state->var_count >= GS_MAX_VARS) {
		return 0;
	}
	copy_text(state->vars[state->var_count].name, GS_NAME_LEN, name);
	state->vars[state->var_count].value = *value;
	++state->var_count;
	return 1;
}

static int is_truthy(const GsValue *value)
{
	return value->is_str ? value->str[0] != '\0' : value->num != 0;
}

const char *gs_variable_text(const GsState *state, const char *name)
{
	const GsValue *value = find_var(state, name);
	return value != NULL && value->is_str ? value->str : NULL;
}

int gs_variable_truthy(const GsState *state, const char *name)
{
	const GsValue *value = find_var(state, name);
	return value != NULL && is_truthy(value);
}

static void value_to_text(const GsValue *value, char *out, size_t size)
{
	if (value->is_str) {
		copy_text(out, size, value->str);
	} else {
		snprintf(out, size, "%ld", value->num);
	}
}

static void skip_space(ExprContext *ctx)
{
	while (isspace((unsigned char)*ctx->cursor)) {
		++ctx->cursor;
	}
}

static int match_word(ExprContext *ctx, const char *word)
{
	skip_space(ctx);
	size_t length = strlen(word);
	if (strncmp(ctx->cursor, word, length) == 0
		&& !isalnum((unsigned char)ctx->cursor[length]) && ctx->cursor[length] != '_') {
		ctx->cursor += length;
		return 1;
	}
	return 0;
}

static int match_symbol(ExprContext *ctx, const char *symbol)
{
	skip_space(ctx);
	size_t length = strlen(symbol);
	if (strncmp(ctx->cursor, symbol, length) == 0) {
		ctx->cursor += length;
		return 1;
	}
	return 0;
}

static void expr_fail(ExprContext *ctx, const char *message)
{
	if (ctx->error[0] == '\0') {
		copy_text(ctx->error, sizeof(ctx->error), message);
	}
}

static GsValue parse_or(ExprContext *ctx);

static GsValue parse_primary(ExprContext *ctx)
{
	skip_space(ctx);
	if (*ctx->cursor == '(') {
		++ctx->cursor;
		GsValue inner = parse_or(ctx);
		if (!match_symbol(ctx, ")")) {
			expr_fail(ctx, "missing ')'");
		}
		return inner;
	}
	if (*ctx->cursor == '"') {
		char text[GS_STR_LEN];
		size_t length = 0;
		++ctx->cursor;
		while (*ctx->cursor != '\0' && *ctx->cursor != '"') {
			if (length + 1 < sizeof(text)) {
				text[length++] = *ctx->cursor;
			}
			++ctx->cursor;
		}
		text[length] = '\0';
		if (*ctx->cursor != '"') {
			expr_fail(ctx, "unterminated string");
		} else {
			++ctx->cursor;
		}
		return make_string(text);
	}
	if (isdigit((unsigned char)*ctx->cursor)) {
		char *end;
		long number = strtol(ctx->cursor, &end, 10);
		ctx->cursor = end;
		return make_number(number);
	}
	if (isalpha((unsigned char)*ctx->cursor) || *ctx->cursor == '_') {
		char name[GS_NAME_LEN];
		size_t length = 0;
		while (isalnum((unsigned char)*ctx->cursor) || *ctx->cursor == '_') {
			if (length + 1 < sizeof(name)) {
				name[length++] = *ctx->cursor;
			}
			++ctx->cursor;
		}
		name[length] = '\0';
		if (strcmp(name, "true") == 0) {
			return make_number(1);
		}
		if (strcmp(name, "false") == 0) {
			return make_number(0);
		}
		const GsValue *found = find_var(ctx->state, name);
		return found != NULL ? *found : make_number(0);
	}
	expr_fail(ctx, "unexpected token in expression");
	return make_number(0);
}

static GsValue parse_unary(ExprContext *ctx)
{
	if (match_symbol(ctx, "-")) {
		GsValue operand = parse_unary(ctx);
		return make_number(-operand.num);
	}
	if (match_word(ctx, "not") || match_symbol(ctx, "!")) {
		GsValue operand = parse_unary(ctx);
		return make_number(!is_truthy(&operand));
	}
	return parse_primary(ctx);
}

static GsValue parse_product(ExprContext *ctx)
{
	GsValue left = parse_unary(ctx);
	for (;;) {
		skip_space(ctx);
		char op = *ctx->cursor;
		if (op != '*' && op != '/' && op != '%') {
			return left;
		}
		++ctx->cursor;
		GsValue right = parse_unary(ctx);
		if (op == '*') {
			left = make_number(left.num * right.num);
		} else if (right.num == 0) {
			expr_fail(ctx, "division by zero");
			left = make_number(0);
		} else {
			left = make_number(op == '/' ? left.num / right.num : left.num % right.num);
		}
	}
}

static GsValue parse_sum(ExprContext *ctx)
{
	GsValue left = parse_product(ctx);
	for (;;) {
		skip_space(ctx);
		char op = *ctx->cursor;
		if (op != '+' && op != '-') {
			return left;
		}
		++ctx->cursor;
		GsValue right = parse_product(ctx);
		if (op == '+' && (left.is_str || right.is_str)) {
			char joined[GS_STR_LEN];
			char rhs[GS_STR_LEN];
			value_to_text(&left, joined, sizeof(joined));
			value_to_text(&right, rhs, sizeof(rhs));
			size_t used = strlen(joined);
			snprintf(joined + used, sizeof(joined) - used, "%s", rhs);
			left = make_string(joined);
		} else {
			left = make_number(op == '+' ? left.num + right.num : left.num - right.num);
		}
	}
}

static GsValue parse_comparison(ExprContext *ctx)
{
	GsValue left = parse_sum(ctx);
	for (;;) {
		const char *symbols[] = {"==", "!=", "<=", ">=", "<", ">"};
		int matched = -1;
		for (int index = 0; index < 6; ++index) {
			if (match_symbol(ctx, symbols[index])) {
				matched = index;
				break;
			}
		}
		if (matched < 0) {
			return left;
		}
		GsValue right = parse_sum(ctx);
		int order;
		if (left.is_str && right.is_str) {
			order = strcmp(left.str, right.str);
		} else {
			order = (left.num > right.num) - (left.num < right.num);
		}
		int result = 0;
		switch (matched) {
		case 0: result = order == 0; break;
		case 1: result = order != 0; break;
		case 2: result = order <= 0; break;
		case 3: result = order >= 0; break;
		case 4: result = order < 0; break;
		default: result = order > 0; break;
		}
		left = make_number(result);
	}
}

static GsValue parse_and(ExprContext *ctx)
{
	GsValue left = parse_comparison(ctx);
	while (match_word(ctx, "and") || match_symbol(ctx, "&&")) {
		GsValue right = parse_comparison(ctx);
		left = make_number(is_truthy(&left) && is_truthy(&right));
	}
	return left;
}

static GsValue parse_or(ExprContext *ctx)
{
	GsValue left = parse_and(ctx);
	while (match_word(ctx, "or") || match_symbol(ctx, "||")) {
		GsValue right = parse_and(ctx);
		left = make_number(is_truthy(&left) || is_truthy(&right));
	}
	return left;
}

static int evaluate(const GsState *state, const char *text, GsValue *result, char *err, size_t err_size)
{
	ExprContext ctx;
	ctx.cursor = text;
	ctx.state = state;
	ctx.error[0] = '\0';
	*result = parse_or(&ctx);
	skip_space(&ctx);
	if (ctx.error[0] == '\0' && *ctx.cursor != '\0') {
		copy_text(ctx.error, sizeof(ctx.error), "unexpected text after expression");
	}
	if (ctx.error[0] != '\0') {
		copy_text(err, err_size, ctx.error);
		return 0;
	}
	return 1;
}

/* Replace {name} with variable values; BRACE_MARK becomes a literal '{'. */
static void interpolate(const GsState *state, const char *source, char *out, size_t size)
{
	size_t length = 0;
	while (*source != '\0' && length + 1 < size) {
		if (*source == BRACE_MARK) {
			out[length++] = '{';
			++source;
		} else if (*source == '{') {
			const char *close = strchr(source, '}');
			if (close == NULL) {
				out[length++] = *source++;
				continue;
			}
			char name[GS_NAME_LEN];
			size_t name_length = (size_t)(close - source - 1);
			if (name_length >= sizeof(name)) {
				name_length = sizeof(name) - 1;
			}
			memcpy(name, source + 1, name_length);
			name[name_length] = '\0';
			const GsValue *value = find_var(state, name);
			char text[GS_STR_LEN];
			if (value != NULL) {
				value_to_text(value, text, sizeof(text));
			} else {
				snprintf(text, sizeof(text), "{%s}", name);
			}
			for (size_t index = 0; text[index] != '\0' && length + 1 < size; ++index) {
				out[length++] = text[index];
			}
			source = close + 1;
		} else {
			out[length++] = *source++;
		}
	}
	out[length] = '\0';
}

/* ---------- compiler ---------- */

typedef struct {
	GsProgram *program;
	Frame frames[MAX_FRAMES];
	int frame_count;
	int line;
	const char *file;           /* name of the included file being compiled, NULL for the main script */
	GsLoader loader;
	void *loader_ctx;
	int depth;
	char *err;
	size_t err_size;
	int failed;
} Compiler;

static int fail(Compiler *compiler, const char *format, const char *detail)
{
	if (!compiler->failed) {
		char message[640];
		snprintf(message, sizeof(message), format, detail ? detail : "");
		if (compiler->file != NULL) {
			snprintf(compiler->err, compiler->err_size, "%s line %d: %s", compiler->file, compiler->line,
				message);
		} else {
			snprintf(compiler->err, compiler->err_size, "line %d: %s", compiler->line, message);
		}
		compiler->failed = 1;
	}
	return -1;
}

static int emit(Compiler *compiler, Op op)
{
	GsProgram *program = compiler->program;
	Instr *grown = grow(program->code, program->code_count, &program->code_cap, sizeof(Instr));
	if (grown == NULL) {
		return fail(compiler, "out of memory%s", "");
	}
	program->code = grown;
	Instr *instr = &program->code[program->code_count];
	memset(instr, 0, sizeof(*instr));
	instr->op = op;
	instr->line = compiler->line;
	instr->target = -1;
	instr->menu = -1;
	return program->code_count++;
}

static int tokenize(Compiler *compiler, const char *line, Token *tokens)
{
	int count = 0;
	while (*line != '\0') {
		while (isspace((unsigned char)*line)) {
			++line;
		}
		if (*line == '\0') {
			break;
		}
		if (count == MAX_TOKENS) {
			return fail(compiler, "too many words on one line%s", "");
		}
		Token *token = &tokens[count++];
		size_t length = 0;
		token->is_string = *line == '"';
		if (token->is_string) {
			++line;
			while (*line != '\0' && *line != '"') {
				char character = *line++;
				if (character == '\\' && *line != '\0') {
					char escaped = *line++;
					character = escaped == 'n' ? '\n'
						: escaped == '{' ? BRACE_MARK : escaped;
				}
				if (length + 1 < sizeof(token->text)) {
					token->text[length++] = character;
				}
			}
			if (*line != '"') {
				return fail(compiler, "unterminated string%s", "");
			}
			++line;
		} else {
			while (*line != '\0' && !isspace((unsigned char)*line) && *line != '"') {
				if (length + 1 < sizeof(token->text)) {
					token->text[length++] = *line;
				}
				++line;
			}
		}
		token->text[length] = '\0';
	}
	return count;
}

static int is_word(const Token *token, const char *word)
{
	return !token->is_string && strcmp(token->text, word) == 0;
}

static int valid_name(const char *name)
{
	if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) {
		return 0;
	}
	for (const char *cursor = name; *cursor != '\0'; ++cursor) {
		if (!isalnum((unsigned char)*cursor) && *cursor != '_') {
			return 0;
		}
	}
	return strlen(name) < GS_NAME_LEN;
}

static int add_patch(Compiler *compiler, Frame *frame, int instr_index)
{
	if (frame->patch_count == MAX_PATCHES) {
		return fail(compiler, "block has too many branches%s", "");
	}
	frame->patches[frame->patch_count++] = instr_index;
	return 0;
}

static Frame *push_frame(Compiler *compiler, FrameKind kind)
{
	if (compiler->frame_count == MAX_FRAMES) {
		fail(compiler, "blocks nested too deeply%s", "");
		return NULL;
	}
	Frame *frame = &compiler->frames[compiler->frame_count++];
	memset(frame, 0, sizeof(*frame));
	frame->kind = kind;
	frame->line = compiler->line;
	frame->cond_jump = -1;
	return frame;
}

static Frame *top_frame(Compiler *compiler)
{
	return compiler->frame_count > 0 ? &compiler->frames[compiler->frame_count - 1] : NULL;
}

static const char *rest_after_first_word(const char *line)
{
	while (isspace((unsigned char)*line)) {
		++line;
	}
	while (*line != '\0' && !isspace((unsigned char)*line)) {
		++line;
	}
	while (isspace((unsigned char)*line)) {
		++line;
	}
	return line;
}

static int check_expression(Compiler *compiler, const char *text)
{
	GsState *scratch = calloc(1, sizeof(*scratch));
	if (scratch == NULL) {
		return fail(compiler, "out of memory%s", "");
	}
	GsValue ignored;
	char message[96];
	int ok = evaluate(scratch, text, &ignored, message, sizeof(message))
		|| strstr(message, "division") != NULL;
	free(scratch);
	return ok ? 0 : fail(compiler, "%s", message);
}

static int parse_color(const char *text, unsigned *color)
{
	if (*text == '#') {
		++text;
	}
	if (strlen(text) != 6) {
		return 0;
	}
	char *end;
	unsigned long value = strtoul(text, &end, 16);
	if (*end != '\0') {
		return 0;
	}
	*color = (unsigned)value;
	return 1;
}

static int is_number_token(const Token *token)
{
	if (token->is_string || token->text[0] == '\0') {
		return 0;
	}
	char *end;
	strtol(token->text, &end, 10);
	return *end == '\0';
}

/* Reads optional "at POS", "at X Y" (pixels) and "fade SECONDS" options starting at tokens[first]. */
static int parse_modifiers(Compiler *compiler, Token *tokens, int count, int first,
	Instr *instr, int allow_pos)
{
	int index = first;
	while (index < count) {
		if (index + 1 >= count) {
			return fail(compiler, "'%s' needs a value", tokens[index].text);
		}
		if (allow_pos && is_word(&tokens[index], "at")) {
			free(instr->c);
			instr->c = NULL;
			if (is_number_token(&tokens[index + 1])) {
				if (index + 2 >= count || !is_number_token(&tokens[index + 2])) {
					return fail(compiler, "'at' with a number needs both X and Y%s", "");
				}
				char coordinates[GS_NAME_LEN];
				snprintf(coordinates, sizeof(coordinates), "%ld,%ld", strtol(tokens[index + 1].text, NULL, 10),
					strtol(tokens[index + 2].text, NULL, 10));
				instr->c = dup_string(coordinates);
				index += 3;
				continue;
			}
			instr->c = dup_string(tokens[index + 1].text);
		} else if (is_word(&tokens[index], "fade")) {
			instr->seconds = (float)atof(tokens[index + 1].text);
		} else {
			return fail(compiler, "unknown option '%s'", tokens[index].text);
		}
		index += 2;
	}
	return 0;
}

static int include_file(Compiler *compiler, const char *name);

static int compile_line(Compiler *compiler, const char *line)
{
	GsProgram *program = compiler->program;
	Token tokens[MAX_TOKENS];
	int count = tokenize(compiler, line, tokens);
	if (count < 0) {
		return -1;
	}
	if (count == 0) {
		return 0;
	}

	Frame *frame = top_frame(compiler);
	int is_choice = is_word(&tokens[0], "choice");
	int is_end = is_word(&tokens[0], "end");
	if (frame != NULL && frame->kind == FRAME_MENU && !frame->in_choice && !is_choice && !is_end) {
		return fail(compiler, "menu must start with a 'choice'%s", "");
	}

	if (is_word(&tokens[0], "include")) {
		if (count != 2) {
			return fail(compiler, "usage: include FILE%s", "");
		}
		return include_file(compiler, tokens[1].text);
	}

	const char *first = tokens[0].text;
	size_t first_length = strlen(first);

	if (is_word(&tokens[0], "page")) {
		if (count != 1) return fail(compiler, "usage: page%s", "");
		return emit(compiler, OP_PAGE) < 0 ? -1 : 0;
	}

	if (tokens[0].is_string) {
		if (count != 1) {
			return fail(compiler, "unexpected text after narration%s", "");
		}
		int index = emit(compiler, OP_SAY);
		if (index < 0) {
			return -1;
		}
		program->code[index].b = dup_string(tokens[0].text);
		return 0;
	}

	if (first_length > 1 && first[first_length - 1] == ':' && count == 2 && tokens[1].is_string) {
		char id[GS_NAME_LEN];
		copy_text(id, sizeof(id), first);
		id[first_length - 1] = '\0';
		int index = emit(compiler, OP_SAY);
		if (index < 0) {
			return -1;
		}
		program->code[index].a = dup_string(id);
		program->code[index].b = dup_string(tokens[1].text);
		return 0;
	}

	if (is_word(&tokens[0], "label")) {
		if (count != 2 || !valid_name(tokens[1].text)) {
			return fail(compiler, "usage: label NAME%s", "");
		}
		for (int index = 0; index < program->label_count; ++index) {
			if (strcmp(program->labels[index].name, tokens[1].text) == 0) {
				return fail(compiler, "duplicate label '%s'", tokens[1].text);
			}
		}
		Label *grown = grow(program->labels, program->label_count, &program->label_cap, sizeof(Label));
		if (grown == NULL) {
			return fail(compiler, "out of memory%s", "");
		}
		program->labels = grown;
		copy_text(program->labels[program->label_count].name, GS_NAME_LEN, tokens[1].text);
		program->labels[program->label_count].pc = program->code_count;
		++program->label_count;
		return 0;
	}

	if (is_word(&tokens[0], "character")) {
		if (count < 3 || count > 4 || !valid_name(tokens[1].text) || !tokens[2].is_string) {
			return fail(compiler, "usage: character ID \"Display Name\" [#RRGGBB]%s", "");
		}
		Character *grown = grow(program->characters, program->character_count,
			&program->character_cap, sizeof(Character));
		if (grown == NULL) {
			return fail(compiler, "out of memory%s", "");
		}
		program->characters = grown;
		Character *character = &program->characters[program->character_count++];
		memset(character, 0, sizeof(*character));
		copy_text(character->id, sizeof(character->id), tokens[1].text);
		copy_text(character->name, sizeof(character->name), tokens[2].text);
		if (count == 4 && !parse_color(tokens[3].text, &character->color)) {
			return fail(compiler, "bad color '%s' (use #RRGGBB)", tokens[3].text);
		}
		return 0;
	}

	if (is_word(&tokens[0], "bgloop")) {
		if ((count != 4 && count != 6 && count != 8) || !tokens[1].text[0] || !tokens[2].text[0] || !tokens[3].text[0]
			|| strlen(tokens[1].text) >= GS_STR_LEN || strlen(tokens[2].text) >= GS_STR_LEN
			|| strlen(tokens[3].text) >= GS_STR_LEN) {
			return fail(compiler, "usage: bgloop BASE OVERLAY1 OVERLAY2 [HOLD FADE]%s", "");
		}
		float hold = 0.45f, fade = 0.15f;
		if (count >= 6) {
			char *end_hold, *end_fade;
			double h = strtod(tokens[4].text, &end_hold);
			double f = strtod(tokens[5].text, &end_fade);
			if (tokens[4].is_string || tokens[5].is_string || !tokens[4].text[0] || !tokens[5].text[0]
				|| *end_hold || *end_fade || !isfinite(h) || !isfinite(f)
				|| h < 0.0 || h > 600.0 || f < 0.001 || f > 600.0)
				return fail(compiler, "bgloop HOLD must be 0..600 and FADE 0.001..600 seconds%s", "");
			hold = (float)((unsigned)(h * 1000000.0 + 0.5) / 1000000.0);
			fade = (float)((unsigned)(f * 1000000.0 + 0.5) / 1000000.0);
		}
		float transition = 0.0f;
		if (count == 8) {
			char *end;
			double value = strtod(tokens[7].text, &end);
			if (!is_word(&tokens[6], "fade") || tokens[7].is_string || !tokens[7].text[0]
				|| *end || !isfinite(value) || value < 0.0 || value > 600.0)
				return fail(compiler, "bgloop transition fade must be 0..600 seconds%s", "");
			transition = (float)value;
		}
		int index = emit(compiler, OP_BGLOOP);
		if (index < 0) return -1;
		Instr *instr = &program->code[index];
		instr->a = dup_string(tokens[1].text);
		instr->b = dup_string(tokens[2].text);
		instr->c = dup_string(tokens[3].text);
		instr->seconds = hold;
		instr->loop_fade = fade;
		instr->transition = transition;
		if (!instr->a || !instr->b || !instr->c) return fail(compiler, "out of memory%s", "");
		return 0;
	}

	if (is_word(&tokens[0], "bg") || is_word(&tokens[0], "show")) {
		int is_show = tokens[0].text[0] == 's';
		int min_count = 2;
		if (count < min_count) {
			return fail(compiler, "usage: %s NAME...", tokens[0].text);
		}
		int index = emit(compiler, is_show ? OP_SHOW : OP_BG);
		if (index < 0) {
			return -1;
		}
		Instr *instr = &program->code[index];
		instr->a = dup_string(tokens[1].text);
		int next = 2;
		if (is_show) {
			instr->b = dup_string(tokens[1].text);
			if (count > 2 && !is_word(&tokens[2], "at") && !is_word(&tokens[2], "fade")) {
				free(instr->b);
				instr->b = dup_string(tokens[2].text);
				next = 3;
			}
			instr->c = dup_string("center");
		}
		return parse_modifiers(compiler, tokens, count, next, instr, is_show);
	}

	if (is_word(&tokens[0], "hide")) {
		if (count < 2) {
			return fail(compiler, "usage: hide NAME [fade SECONDS]%s", "");
		}
		int index = emit(compiler, OP_HIDE);
		if (index < 0) {
			return -1;
		}
		program->code[index].a = dup_string(tokens[1].text);
		return parse_modifiers(compiler, tokens, count, 2, &program->code[index], 0);
	}

	if (is_word(&tokens[0], "music") || is_word(&tokens[0], "sfx")) {
		if (count < 2) {
			return fail(compiler, "usage: %s FILE", tokens[0].text);
		}
		int index = emit(compiler, tokens[0].text[0] == 'm' ? OP_MUSIC : OP_SFX);
		if (index < 0) {
			return -1;
		}
		program->code[index].a = dup_string(is_word(&tokens[1], "stop") ? "" : tokens[1].text);
		return parse_modifiers(compiler, tokens, count, 2, &program->code[index], 0);
	}

	if (is_word(&tokens[0], "video")) {
		if (count < 2 || count > 3 || (count == 3 && !is_word(&tokens[2], "noskip"))) {
			return fail(compiler, "usage: video NAME [noskip]%s", "");
		}
		int index = emit(compiler, OP_VIDEO);
		if (index < 0) {
			return -1;
		}
		program->code[index].a = dup_string(tokens[1].text);
		program->code[index].seconds = count == 3 ? 1.0f : 0.0f;
		return 0;
	}

	if (is_word(&tokens[0], "gallery")) {
		if (count != 1) return fail(compiler, "usage: gallery%s", "");
		return emit(compiler, OP_GALLERY) < 0 ? -1 : 0;
	}

	if (is_word(&tokens[0], "camera")) {
		const char *angle = "c";
		float seconds = 0.5f;
		int small = 0;
		for (int i = 1; i < count; ++i) {
			if (!strcmp(tokens[i].text, "small")) small = 1;
			else if (!strcmp(tokens[i].text, "l") || !strcmp(tokens[i].text, "c")
				|| !strcmp(tokens[i].text, "r") || !strcmp(tokens[i].text, "d")) angle = tokens[i].text;
			else if (isdigit((unsigned char)tokens[i].text[0]) || tokens[i].text[0] == '.')
				seconds = (float)atof(tokens[i].text);
			else return fail(compiler, "usage: camera [l|c|r|d] [SECONDS] [small]%s", "");
		}
		int index = emit(compiler, OP_EFFECT);
		if (index < 0) return -1;
		program->code[index].a = dup_string("camera");
		program->code[index].b = dup_string(angle);
		program->code[index].c = dup_string(small ? "small" : "");
		program->code[index].seconds = seconds;
		return 0;
	}

	if (is_word(&tokens[0], "hop")) {
		if (count != 2) return fail(compiler, "usage: hop ID%s", "");
		int index = emit(compiler, OP_EFFECT);
		if (index < 0) return -1;
		program->code[index].a = dup_string("hop");
		program->code[index].b = dup_string(tokens[1].text);
		program->code[index].c = dup_string("");
		program->code[index].seconds = 0.35f;
		return 0;
	}

	if (is_word(&tokens[0], "shake") || is_word(&tokens[0], "flash")) {
		int shake = is_word(&tokens[0], "shake");
		if (count < 2 || count > 3) {
			return fail(compiler, "usage: shake SECONDS [PIXELS] | flash SECONDS [white|black]%s", "");
		}
		if (!shake && count == 3 && strcmp(tokens[2].text, "white") && strcmp(tokens[2].text, "black")) {
			return fail(compiler, "flash color must be white or black%s", "");
		}
		int index = emit(compiler, OP_EFFECT);
		if (index < 0) return -1;
		program->code[index].a = dup_string(shake ? "shake" : "flash");
		program->code[index].b = dup_string(count == 3 ? tokens[2].text : (shake ? "6" : "white"));
		program->code[index].c = dup_string("");
		program->code[index].seconds = (float)atof(tokens[1].text);
		return 0;
	}

	if (is_word(&tokens[0], "waitbg")) {
		if (count > 2 || (count == 2 && !is_word(&tokens[1], "skippable")))
			return fail(compiler, "usage: waitbg [skippable]%s", "");
		int index = emit(compiler, OP_WAITBG);
		if (index < 0) return -1;
		program->code[index].seconds = count == 2 ? 1.0f : 0.0f;
		return 0;
	}

	if (is_word(&tokens[0], "wait")) {
		if (count != 2) {
			return fail(compiler, "usage: wait SECONDS%s", "");
		}
		int index = emit(compiler, OP_WAIT);
		if (index < 0) {
			return -1;
		}
		program->code[index].seconds = (float)atof(tokens[1].text);
		return 0;
	}

	if (is_word(&tokens[0], "set")) {
		const char *rest = rest_after_first_word(line);
		char name[GS_NAME_LEN];
		size_t length = 0;
		while (isalnum((unsigned char)rest[length]) || rest[length] == '_') {
			if (length + 1 < sizeof(name)) {
				name[length] = rest[length];
			}
			++length;
		}
		name[length < sizeof(name) ? length : sizeof(name) - 1] = '\0';
		const char *cursor = rest + length;
		while (isspace((unsigned char)*cursor)) {
			++cursor;
		}
		char op = '=';
		if ((cursor[0] == '+' || cursor[0] == '-') && cursor[1] == '=') {
			op = cursor[0];
			cursor += 2;
		} else if (cursor[0] == '=') {
			++cursor;
		} else {
			return fail(compiler, "usage: set NAME = EXPRESSION%s", "");
		}
		if (!valid_name(name) || *cursor == '\0') {
			return fail(compiler, "usage: set NAME = EXPRESSION%s", "");
		}
		if (check_expression(compiler, cursor) < 0) {
			return -1;
		}
		char expression[GS_TEXT_LEN];
		if (op == '=') {
			snprintf(expression, sizeof(expression), "%s", cursor);
		} else {
			snprintf(expression, sizeof(expression), "%s %c (%s)", name, op, cursor);
		}
		int index = emit(compiler, OP_SET);
		if (index < 0) {
			return -1;
		}
		program->code[index].a = dup_string(name);
		program->code[index].b = dup_string(expression);
		return 0;
	}

	if (is_word(&tokens[0], "jump") || is_word(&tokens[0], "call")) {
		if (count != 2) {
			return fail(compiler, "usage: %s LABEL", tokens[0].text);
		}
		int index = emit(compiler, tokens[0].text[0] == 'j' ? OP_JUMP : OP_CALL);
		if (index < 0) {
			return -1;
		}
		program->code[index].a = dup_string(tokens[1].text);
		return 0;
	}

	if (is_word(&tokens[0], "script")) {
		if (count != 2 || !tokens[1].text[0]) {
			return fail(compiler, "usage: %s FILE", "script");
		}
		int index = emit(compiler, OP_SCRIPT);
		if (index < 0) {
			return -1;
		}
		program->code[index].a = dup_string(tokens[1].text);
		return 0;
	}

	if (is_word(&tokens[0], "return") || is_word(&tokens[0], "stop")) {
		return emit(compiler, is_word(&tokens[0], "return") ? OP_RET : OP_END) < 0 ? -1 : 0;
	}

	if (is_word(&tokens[0], "if") || is_word(&tokens[0], "while")) {
		const char *expression = rest_after_first_word(line);
		if (*expression == '\0' || check_expression(compiler, expression) < 0) {
			return fail(compiler, "'%s' needs a condition", tokens[0].text);
		}
		int is_while = tokens[0].text[0] == 'w';
		int start = program->code_count;
		Frame *created = push_frame(compiler, is_while ? FRAME_WHILE : FRAME_IF);
		if (created == NULL) {
			return -1;
		}
		created->start = start;
		int index = emit(compiler, OP_JUMPF);
		if (index < 0) {
			return -1;
		}
		program->code[index].b = dup_string(expression);
		created->cond_jump = index;
		return 0;
	}

	if (is_word(&tokens[0], "elif") || is_word(&tokens[0], "else")) {
		int is_else = tokens[0].text[2] == 's';
		if (frame == NULL || frame->kind != FRAME_IF || frame->saw_else) {
			return fail(compiler, "'%s' without a matching 'if'", tokens[0].text);
		}
		int skip = emit(compiler, OP_JUMP);
		if (skip < 0 || add_patch(compiler, frame, skip) < 0) {
			return -1;
		}
		program->code[frame->cond_jump].target = program->code_count;
		frame->cond_jump = -1;
		if (is_else) {
			frame->saw_else = 1;
			return 0;
		}
		const char *expression = rest_after_first_word(line);
		if (*expression == '\0' || check_expression(compiler, expression) < 0) {
			return fail(compiler, "'elif' needs a condition%s", "");
		}
		int index = emit(compiler, OP_JUMPF);
		if (index < 0) {
			return -1;
		}
		program->code[index].b = dup_string(expression);
		frame->cond_jump = index;
		return 0;
	}

	if (is_word(&tokens[0], "menu")) {
		Frame *created = push_frame(compiler, FRAME_MENU);
		if (created == NULL) {
			return -1;
		}
		created->menu = program->menu_count++;
		int index = emit(compiler, OP_MENU);
		if (index < 0) {
			return -1;
		}
		program->code[index].menu = created->menu;
		return 0;
	}

	if (is_choice) {
		if (frame == NULL || frame->kind != FRAME_MENU) {
			return fail(compiler, "'choice' outside a menu%s", "");
		}
		if (count < 2 || !tokens[1].is_string) {
			return fail(compiler, "usage: choice \"Text\" [if CONDITION]%s", "");
		}
		const char *condition = NULL;
		if (count > 2) {
			if (!is_word(&tokens[2], "if")) {
				return fail(compiler, "expected 'if' after choice text%s", "");
			}
			const char *scan = strchr(line, '"');
			while (scan != NULL && *++scan != '\0' && *scan != '"') {
				scan += *scan == '\\' && scan[1] != '\0';
			}
			condition = (scan != NULL && *scan == '"') ? strstr(scan, "if") : NULL;
			condition = condition != NULL ? condition + 2 : NULL;
			if (condition == NULL || *condition == '\0' || check_expression(compiler, condition) < 0) {
				return fail(compiler, "bad choice condition%s", "");
			}
		}
		if (frame->in_choice) {
			int skip = emit(compiler, OP_JUMP);
			if (skip < 0 || add_patch(compiler, frame, skip) < 0) {
				return -1;
			}
		}
		Option *grown = grow(program->options, program->option_count, &program->option_cap, sizeof(Option));
		if (grown == NULL) {
			return fail(compiler, "out of memory%s", "");
		}
		program->options = grown;
		Option *option = &program->options[program->option_count++];
		option->menu = frame->menu;
		option->text = dup_string(tokens[1].text);
		option->cond = condition != NULL ? dup_string(condition) : NULL;
		option->target = program->code_count;
		frame->in_choice = 1;
		return 0;
	}

	if (is_end) {
		if (frame == NULL) {
			return fail(compiler, "'end' without an open block%s", "");
		}
		if (frame->kind == FRAME_WHILE) {
			int back = emit(compiler, OP_JUMP);
			if (back < 0) {
				return -1;
			}
			program->code[back].target = frame->start;
		}
		if (frame->kind == FRAME_MENU && !frame->in_choice) {
			return fail(compiler, "menu has no choices%s", "");
		}
		if (frame->cond_jump >= 0) {
			program->code[frame->cond_jump].target = program->code_count;
		}
		for (int index = 0; index < frame->patch_count; ++index) {
			program->code[frame->patches[index]].target = program->code_count;
		}
		--compiler->frame_count;
		return 0;
	}

	return fail(compiler, "unknown command '%s'", tokens[0].text);
}

static void free_instr(Instr *instr)
{
	free(instr->a);
	free(instr->b);
	free(instr->c);
}

void gs_free(GsProgram *program)
{
	if (program == NULL) {
		return;
	}
	for (int index = 0; index < program->code_count; ++index) {
		free_instr(&program->code[index]);
	}
	for (int index = 0; index < program->option_count; ++index) {
		free(program->options[index].text);
		free(program->options[index].cond);
	}
	free(program->code);
	free(program->options);
	free(program->characters);
	free(program->labels);
	free(program);
}

static int find_label(const GsProgram *program, const char *name)
{
	for (int index = 0; index < program->label_count; ++index) {
		if (strcmp(program->labels[index].name, name) == 0) {
			return program->labels[index].pc;
		}
	}
	return -1;
}

/* Compiles every line of one source text (the main script or an included file). */
static void compile_text(Compiler *compiler, const char *source, char *line)
{
	const char *cursor = source;
	compiler->line = 0;
	while (*cursor != '\0' && !compiler->failed) {
		size_t length = strcspn(cursor, "\n");
		size_t copied = length < 1023 ? length : 1023;
		memcpy(line, cursor, copied);
		line[copied] = '\0';
		if (copied > 0 && line[copied - 1] == '\r') {
			line[copied - 1] = '\0';
		}
		++compiler->line;
		cursor += length + (cursor[length] == '\n');

		const char *trimmed = line;
		while (isspace((unsigned char)*trimmed)) {
			++trimmed;
		}
		if (*trimmed == '#' || *trimmed == '\0') {
			continue;
		}
		compile_line(compiler, trimmed);
	}
}

static int include_file(Compiler *compiler, const char *name)
{
	if (compiler->loader == NULL) {
		return fail(compiler, "include is not available here%s", "");
	}
	if (compiler->depth >= MAX_INCLUDE_DEPTH) {
		return fail(compiler, "includes are nested too deeply%s", "");
	}
	char *text = compiler->loader(name, compiler->loader_ctx);
	if (text == NULL) {
		return fail(compiler, "cannot read included file '%s'", name);
	}
	char *line = malloc(1024);
	if (line == NULL) {
		free(text);
		return fail(compiler, "out of memory%s", "");
	}
	const char *saved_file = compiler->file;
	int saved_line = compiler->line;
	compiler->file = name;
	++compiler->depth;
	compile_text(compiler, text, line);
	--compiler->depth;
	if (!compiler->failed) {
		compiler->file = saved_file;
		compiler->line = saved_line;
	}
	free(line);
	free(text);
	return compiler->failed ? -1 : 0;
}

GsProgram *gs_compile_ex(const char *source, GsLoader loader, void *loader_ctx, char *err, size_t err_size)
{
	/* Heap-allocated: the compiler state is too big for the 3DS's small stack. */
	Compiler *compiler = calloc(1, sizeof(*compiler));
	char *line = malloc(1024);
	GsProgram *program = calloc(1, sizeof(GsProgram));
	if (compiler == NULL || line == NULL || program == NULL) {
		snprintf(err, err_size, "out of memory");
		free(compiler);
		free(line);
		free(program);
		return NULL;
	}
	compiler->program = program;
	compiler->loader = loader;
	compiler->loader_ctx = loader_ctx;
	compiler->err = err;
	compiler->err_size = err_size;
	if (err_size > 0) {
		err[0] = '\0';
	}

	compile_text(compiler, source, line);

	if (!compiler->failed && compiler->frame_count > 0) {
		compiler->file = NULL;
		compiler->line = compiler->frames[compiler->frame_count - 1].line;
		fail(compiler, "block is never closed with 'end'%s", "");
	}

	for (int index = 0; index < program->code_count && !compiler->failed; ++index) {
		Instr *instr = &program->code[index];
		if ((instr->op == OP_JUMP && instr->a != NULL) || instr->op == OP_CALL) {
			instr->target = find_label(program, instr->a);
			if (instr->target < 0) {
				compiler->line = instr->line;
				fail(compiler, "unknown label '%s'", instr->a);
			}
		}
	}

	int failed = compiler->failed;
	free(compiler);
	free(line);
	if (failed) {
		gs_free(program);
		return NULL;
	}
	return program;
}

/* ---------- virtual machine ---------- */

void gs_state_init(const GsProgram *program, GsState *state)
{
	(void)program;
	memset(state, 0, sizeof(*state));
	state->pending_menu = -1;
}

static void runtime_error(GsEvent *event, int line, const char *message)
{
	event->type = GS_EV_ERROR;
	event->line = line;
	copy_text(event->text, sizeof(event->text), message);
}

static const Character *find_character(const GsProgram *program, const char *id)
{
	for (int index = 0; index < program->character_count; ++index) {
		if (strcmp(program->characters[index].id, id) == 0) {
			return &program->characters[index];
		}
	}
	return NULL;
}

static void fill_menu(const GsProgram *program, const GsState *state, GsEvent *event)
{
	event->type = GS_EV_MENU;
	event->choice_count = state->pending_count;
	for (int index = 0; index < state->pending_count; ++index) {
		interpolate(state, program->options[state->pending_map[index]].text,
			event->choices[index], sizeof(event->choices[index]));
	}
}

void gs_next(const GsProgram *program, GsState *state, GsEvent *event)
{
	memset(event, 0, sizeof(*event));

	if (state->pending_menu >= 0) {
		fill_menu(program, state, event);
		return;
	}

	for (int steps = 0; steps < STEP_LIMIT; ++steps) {
		if (state->pc < 0 || state->pc >= program->code_count) {
			event->type = GS_EV_END;
			return;
		}
		const Instr *instr = &program->code[state->pc++];
		event->line = instr->line;
		char err[96];

		switch (instr->op) {
		case OP_PAGE:
			event->type = GS_EV_PAGE;
			return;
		case OP_SAY: {
			const Character *character = instr->a != NULL ? find_character(program, instr->a) : NULL;
			event->type = GS_EV_SAY;
			if (character != NULL) {
				copy_text(event->speaker, sizeof(event->speaker), character->name);
				event->color = character->color;
			} else if (instr->a != NULL) {
				copy_text(event->speaker, sizeof(event->speaker), instr->a);
			}
			interpolate(state, instr->b, event->text, sizeof(event->text));
			return;
		}
		case OP_BG:
		case OP_MUSIC:
		case OP_SFX:
		case OP_SHOW:
		case OP_HIDE:
			event->type = instr->op == OP_BG ? GS_EV_BG
				: instr->op == OP_MUSIC ? GS_EV_MUSIC
				: instr->op == OP_SFX ? GS_EV_SFX
				: instr->op == OP_SHOW ? GS_EV_SHOW : GS_EV_HIDE;
			if (instr->op == OP_SHOW || instr->op == OP_HIDE) {
				copy_text(event->name, sizeof(event->name), instr->a);
				copy_text(event->asset, sizeof(event->asset), instr->b);
				copy_text(event->pos, sizeof(event->pos), instr->c);
			} else {
				interpolate(state, instr->a, event->asset, sizeof(event->asset));
			}
			event->seconds = instr->seconds;
			return;
		case OP_BGLOOP:
			event->type = GS_EV_BGLOOP;
			interpolate(state, instr->a, event->asset, sizeof(event->asset));
			interpolate(state, instr->b, event->choices[0], sizeof(event->choices[0]));
			interpolate(state, instr->c, event->choices[1], sizeof(event->choices[1]));
			event->seconds = instr->seconds;
			snprintf(event->pos, sizeof(event->pos), "%.9g", (double)instr->loop_fade);
			snprintf(event->text, sizeof(event->text), "%.9g", (double)instr->transition);
			return;
		case OP_VIDEO:
			event->type = GS_EV_VIDEO;
			interpolate(state, instr->a, event->asset, sizeof(event->asset));
			event->seconds = instr->seconds;
			return;
		case OP_WAIT:
			event->type = GS_EV_WAIT;
			event->seconds = instr->seconds;
			return;
		case OP_EFFECT:
			event->type = GS_EV_EFFECT;
			snprintf(event->asset, sizeof(event->asset), "%s", instr->a);
			snprintf(event->pos, sizeof(event->pos), "%s", instr->b);
			snprintf(event->text, sizeof(event->text), "%s", instr->c);
			event->seconds = instr->seconds;
			return;
		case OP_WAITBG:
			event->type = GS_EV_WAITBG;
			event->seconds = instr->seconds;
			return;
		case OP_SET: {
			GsValue value;
			if (!evaluate(state, instr->b, &value, err, sizeof(err))) {
				runtime_error(event, instr->line, err);
				return;
			}
			if (!set_var(state, instr->a, &value)) {
				runtime_error(event, instr->line, "too many variables");
				return;
			}
			break;
		}
		case OP_JUMP:
			state->pc = instr->target;
			break;
		case OP_JUMPF: {
			GsValue value;
			if (!evaluate(state, instr->b, &value, err, sizeof(err))) {
				runtime_error(event, instr->line, err);
				return;
			}
			if (!is_truthy(&value)) {
				state->pc = instr->target;
			}
			break;
		}
		case OP_CALL:
			if (state->call_depth == GS_MAX_CALLS) {
				runtime_error(event, instr->line, "call stack overflow");
				return;
			}
			state->call_stack[state->call_depth++] = state->pc;
			state->pc = instr->target;
			break;
		case OP_RET:
			if (state->call_depth == 0) {
				runtime_error(event, instr->line, "'return' with no active call");
				return;
			}
			state->pc = state->call_stack[--state->call_depth];
			break;
		case OP_SCRIPT: {
			GsValue value = make_string(instr->a);
			if (!set_var(state, "_script", &value)) {
				runtime_error(event, instr->line, "too many variables");
				return;
			}
			event->type = GS_EV_SCRIPT;
			copy_text(event->asset, sizeof(event->asset), instr->a);
			state->pc = 0;
			state->call_depth = 0;
			return;
		}
		case OP_GALLERY:
			event->type = GS_EV_GALLERY;
			return;
		case OP_END:
			event->type = GS_EV_END;
			state->pc = program->code_count;
			return;
		case OP_MENU: {
			state->pending_count = 0;
			for (int index = 0; index < program->option_count; ++index) {
				const Option *option = &program->options[index];
				if (option->menu != instr->menu || state->pending_count == GS_MAX_CHOICES) {
					continue;
				}
				GsValue condition = make_number(1);
				if (option->cond != NULL && !evaluate(state, option->cond, &condition, err, sizeof(err))) {
					runtime_error(event, instr->line, err);
					return;
				}
				if (is_truthy(&condition)) {
					state->pending_map[state->pending_count++] = index;
				}
			}
			if (state->pending_count == 0) {
				runtime_error(event, instr->line, "menu has no available choices");
				return;
			}
			state->pending_menu = instr->menu;
			fill_menu(program, state, event);
			return;
		}
		}
	}
	runtime_error(event, event->line, "script ran too long without output (infinite loop?)");
}

int gs_choose(const GsProgram *program, GsState *state, int index)
{
	if (state->pending_menu < 0 || index < 0 || index >= state->pending_count) {
		return 0;
	}
	state->pc = program->options[state->pending_map[index]].target;
	state->pending_menu = -1;
	state->pending_count = 0;
	return 1;
}

int gs_state_valid(const GsProgram *program, const GsState *state)
{
	if (state->pc < 0 || state->pc > program->code_count
		|| state->var_count < 0 || state->var_count > GS_MAX_VARS
		|| state->call_depth < 0 || state->call_depth > GS_MAX_CALLS
		|| state->pending_menu < -1 || state->pending_menu >= program->menu_count
		|| state->pending_count < 0 || state->pending_count > GS_MAX_CHOICES
		|| (state->pending_menu >= 0 && state->pending_count == 0)) {
		return 0;
	}
	for (int index = 0; index < state->call_depth; ++index) {
		if (state->call_stack[index] < 0 || state->call_stack[index] > program->code_count) {
			return 0;
		}
	}
	for (int index = 0; index < state->pending_count; ++index) {
		if (state->pending_map[index] < 0 || state->pending_map[index] >= program->option_count) {
			return 0;
		}
	}
	for (int index = 0; index < state->var_count; ++index) {
		const GsVar *var = &state->vars[index];
		if (memchr(var->name, '\0', GS_NAME_LEN) == NULL
			|| memchr(var->value.str, '\0', GS_STR_LEN) == NULL) {
			return 0;
		}
	}
	return 1;
}

GsProgram *gs_compile(const char *source, char *err, size_t err_size)
{
	return gs_compile_ex(source, NULL, NULL, err, err_size);
}
