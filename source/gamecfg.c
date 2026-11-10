#include "gamecfg.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *const default_labels[LABEL_COUNT] = {
	"Save Game", "Load Game", "Backlog", "Glossary", "Quit to Game List", "Options"
};
static const char *const label_keys[LABEL_COUNT] = {
	"label_save", "label_load", "label_backlog", "label_glossary", "label_quit", "label_options"
};

static const char *const image_keys[IMG_COUNT] = {
	"menu_bg", "backlog_bg", "bottom_bg", "textbox_img", "namebox_img",
	"choice_img", "choice_sel_img", "item_img", "item_sel_img", "nvl_textbox_img", "advance_img"
};

typedef struct {
	const char *key;
	int def, min, max;
} NumberKey;

static const NumberKey number_keys[NUM_COUNT] = {
	{"textbox_x", 8, 0, 400}, {"textbox_y", 150, 0, 240},
	{"textbox_w", 384, 40, 400}, {"textbox_h", 84, 20, 240}, {"textbox_alpha", 215, 0, 255},
	{"text_x", 10, 0, 200}, {"text_y", 26, 0, 200}, {"narration_y", 10, 0, 200}, {"text_size", 50, 20, 150},
	{"name_x", 10, -400, 400}, {"name_y", 5, -240, 240}, {"name_size", 55, 20, 150},
	{"namebox_x", 0, -400, 400}, {"namebox_y", -22, -240, 240},
	{"choice_w", 340, 40, 400}, {"choice_h", 24, 12, 60}, {"choice_gap", 4, 0, 40},
	{"choice_text_x", 10, 0, 200}, {"choice_size", 50, 20, 150}, {"choice_dim", 150, 0, 255},
	{"nvl_dim", 155, 0, 255}, {"nvl_border", 1, 0, 1},
	{"nvl_margin_x", 12, 0, 180}, {"nvl_margin_y", 12, 0, 90},
	{"advance_frames", 1, 1, 128}, {"advance_frame_ms", 100, 1, 10000},
	{"advance_scale", 100, 10, 400},
	{"nvl_accumulate", 0, 0, 1},
	{"face_box", 0, 0, 1},
};

void config_defaults(GameConfig *config)
{
	memset(config, 0, sizeof(*config));
	for (int index = 0; index < NUM_COUNT; ++index) {
		config->num[index] = number_keys[index].def;
	}
	for (int index = 0; index < LABEL_COUNT; ++index) {
		strcpy(config->labels[index], default_labels[index]);
	}
	strcpy(config->title_start, "Start");
	strcpy(config->title_load, "Load");
	strcpy(config->title_options, "Options");
	strcpy(config->title_extras, "Extras");
}

static void copy_text(char *out, size_t size, const char *text, size_t length)
{
	if (length >= size) {
		length = size - 1;
		while (length > 0 && ((unsigned char)text[length] & 0xC0) == 0x80) {
			--length;
		}
	}
	memcpy(out, text, length);
	out[length] = '\0';
}

static int parse_color(const char *text, unsigned *out)
{
	if (*text == '#') {
		++text;
	}
	if (strlen(text) != 6) {
		return 0;
	}
	for (int index = 0; index < 6; ++index) {
		if (!isxdigit((unsigned char)text[index])) {
			return 0;
		}
	}
	*out = (unsigned)strtoul(text, NULL, 16);
	return 1;
}

/* Trims in place and returns the start of the trimmed text. */
static char *trim(char *text)
{
	while (*text == ' ' || *text == '\t') {
		++text;
	}
	size_t length = strlen(text);
	while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t'
		|| text[length - 1] == '\r' || text[length - 1] == '\n')) {
		text[--length] = '\0';
	}
	return text;
}

void config_parse(const char *source, GameConfig *config)
{
	while (*source != '\0') {
		const char *end = strchr(source, '\n');
		size_t length = end != NULL ? (size_t)(end - source) : strlen(source);
		char line[256];
		copy_text(line, sizeof(line), source, length);
		source += length + (end != NULL ? 1 : 0);

		char *key = trim(line);
		if (*key == '\0' || *key == '#') {
			continue;
		}
		char *value = key;
		while (*value != '\0' && *value != ' ' && *value != '\t') {
			++value;
		}
		if (*value != '\0') {
			*value++ = '\0';
		}
		value = trim(value);
		if (*value == '\0') {
			continue;
		}

		unsigned rgb;
		if (strcmp(key, "accent") == 0 && parse_color(value, &rgb)) {
			config->accent = rgb;
			config->has_accent = 1;
		} else if (strcmp(key, "panel") == 0 && parse_color(value, &rgb)) {
			config->panel = rgb;
			config->has_panel = 1;
		} else if (strcmp(key, "text") == 0 && parse_color(value, &rgb)) {
			config->text = rgb;
			config->has_text = 1;
		} else if (strcmp(key, "bottom_color") == 0 && parse_color(value, &rgb)) {
			config->bg = rgb;
			config->has_bg = 1;
		} else if (strcmp(key, "font") == 0) {
			copy_text(config->font, sizeof(config->font), value, strlen(value));
		} else if (strcmp(key, "menu_title") == 0) {
			copy_text(config->menu_title, sizeof(config->menu_title), value, strlen(value));
		} else if (strcmp(key, "title") == 0) {
			copy_text(config->title, sizeof(config->title), value, strlen(value));
		} else if (strcmp(key, "title_start") == 0) {
			copy_text(config->title_start, sizeof(config->title_start), value, strlen(value));
		} else if (strcmp(key, "title_load") == 0) {
			copy_text(config->title_load, sizeof(config->title_load), value, strlen(value));
		} else if (strcmp(key, "title_options") == 0) {
			copy_text(config->title_options, sizeof(config->title_options), value, strlen(value));
		} else if (strcmp(key, "title_extras") == 0) {
			copy_text(config->title_extras, sizeof(config->title_extras), value, strlen(value));
		} else if (strcmp(key, "default_display") == 0) {
			if (strcmp(value, "nvl") == 0) config->default_nvl = 1;
			else if (strcmp(value, "adv") == 0) config->default_nvl = 0;
		} else {
			for (int index = 0; index < LABEL_COUNT; ++index) {
				if (strcmp(key, label_keys[index]) == 0) {
					copy_text(config->labels[index], CFG_LABEL_LEN, value, strlen(value));
				}
			}
			for (int index = 0; index < IMG_COUNT; ++index) {
				if (strcmp(key, image_keys[index]) == 0) {
					copy_text(config->images[index], CFG_PATH_LEN, value, strlen(value));
				}
			}
			for (int index = 0; index < NUM_COUNT; ++index) {
				char *end;
				long number = strtol(value, &end, 10);
				if (strcmp(key, number_keys[index].key) == 0 && *end == '\0'
					&& number >= number_keys[index].min && number <= number_keys[index].max) {
					config->num[index] = (int)number;
				}
			}
		}
	}
}

static void finish_term(Glossary *glossary)
{
	if (glossary->count == 0) {
		return;
	}
	char *text = glossary->terms[glossary->count - 1].text;
	size_t length = strlen(text);
	while (length > 0 && text[length - 1] == '\n') {
		text[--length] = '\0';
	}
}

int glossary_parse_checked(const char *source, Glossary *glossary)
{
	glossary->count = 0;
	int collecting = 0, line_number = 0, first_error = 0;
	while (*source != '\0') {
		const char *end = strchr(source, '\n');
		size_t length = end != NULL ? (size_t)(end - source) : strlen(source);
		char line[CFG_TEXT_LEN + 16];
		copy_text(line, sizeof(line), source, length);
		source += length + (end != NULL ? 1 : 0);
		++line_number;
		while (*line != '\0' && line[strlen(line) - 1] == '\r') {
			line[strlen(line) - 1] = '\0';
		}

		if (strncmp(line, "term ", 5) == 0 || strncmp(line, "term\t", 5) == 0) {
			finish_term(glossary);
			char *name = trim(line + 5);
			collecting = 0;
			if (*name != '\0' && glossary->count < CFG_MAX_TERMS) {
				GlossaryTerm *term = &glossary->terms[glossary->count++];
				memset(term, 0, sizeof(*term));
				copy_text(term->name, sizeof(term->name), name, strlen(name));
				collecting = 1;
			}
			continue;
		}
		if (!collecting || line[0] == '#') {
			continue;
		}
		GlossaryTerm *term = &glossary->terms[glossary->count - 1];
		if (strncmp(line, "@unlock", 7) == 0 && (line[7] == '\0'
			|| line[7] == ' ' || line[7] == '\t')) {
			char *name = trim(line + 7);
			size_t name_length = strlen(name);
			int valid = name_length > 0 && name_length < GS_NAME_LEN
				&& (isalpha((unsigned char)name[0]) || name[0] == '_');
			for (size_t i = 1; valid && i < name_length; ++i) {
				valid = isalnum((unsigned char)name[i]) || name[i] == '_';
			}
			if (!valid || term->unlock[0] != '\0' || term->unlock_invalid) {
				term->unlock_invalid = 1;
				if (!first_error) first_error = line_number;
			} else {
				strcpy(term->unlock, name);
			}
			continue;
		}
		size_t used = strlen(term->text);
		char *text = trim(line);
		if (*text == '\0' && used == 0) {
			continue;
		}
		size_t room = CFG_TEXT_LEN - used;
		if (used > 0 && room > 0) {
			term->text[used++] = '\n';
			--room;
		}
		size_t take = strlen(text);
		if (take > room) {
			take = room;
			while (take > 0 && ((unsigned char)text[take] & 0xC0) == 0x80) {
				--take;
			}
		}
		memcpy(term->text + used, text, take);
		term->text[used + take] = '\0';
	}
	finish_term(glossary);
	return first_error;
}

void glossary_parse(const char *source, Glossary *glossary)
{
	(void)glossary_parse_checked(source, glossary);
}

int glossary_visible_indices(const Glossary *glossary, const GsState *state, int *indices)
{
	int count = 0;
	for (int i = 0; i < glossary->count; ++i) {
		const GlossaryTerm *term = &glossary->terms[i];
		if (!term->unlock_invalid && (!term->unlock[0] || gs_variable_truthy(state, term->unlock))) {
			indices[count++] = i;
		}
	}
	return count;
}
