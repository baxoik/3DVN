#include "display_options.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *suffix_path(const char *path, const char *suffix)
{
	size_t length = strlen(path), extra = strlen(suffix) + 1;
	if (length > SIZE_MAX - extra)
		return NULL;
	char *result = malloc(length + extra);
	if (result != NULL) {
		memcpy(result, path, length);
		memcpy(result + length, suffix, extra);
	}
	return result;
}

void display_options_defaults(DisplayOptions *options)
{
	options->custom_textbox = true;
	options->nvl = false;
}

bool display_options_use_custom(const DisplayOptions *options, bool available)
{
	return available && options->custom_textbox;
}

void display_options_apply_game_default(DisplayOptions *options, DisplayOptionsResult result, bool default_nvl)
{
	if (result != DISPLAY_OPTIONS_OK && result != DISPLAY_OPTIONS_RECOVERED)
		options->nvl = default_nvl;
}

DisplayOptionsResult display_options_load(const char *path, DisplayOptions *options)
{
	char data[64];
	size_t length, pos;
	int seen_custom = 0, seen_nvl = 0;
	DisplayOptions parsed;
	FILE *file;
	int recovered = 0;

	if (path == NULL || path[0] == '\0' || options == NULL)
		return DISPLAY_OPTIONS_INVALID_ARGUMENT;

	file = fopen(path, "rb");
	if (file == NULL) {
		if (errno == ENOENT) {
			char *backup = suffix_path(path, ".bak");
			if (backup == NULL)
				return DISPLAY_OPTIONS_IO_ERROR;
			file = fopen(backup, "rb");
			free(backup);
			if (file == NULL) {
				if (errno != ENOENT)
					return DISPLAY_OPTIONS_IO_ERROR;
				display_options_defaults(options);
				return DISPLAY_OPTIONS_MISSING;
			}
			recovered = 1;
		} else {
			return DISPLAY_OPTIONS_IO_ERROR;
		}
	}

	length = fread(data, 1, sizeof(data), file);
	if (ferror(file)) {
		fclose(file);
		return DISPLAY_OPTIONS_IO_ERROR;
	}
	if (fclose(file) != 0)
		return DISPLAY_OPTIONS_IO_ERROR;
	if (length == sizeof(data) || memchr(data, '\0', length) != NULL)
		return DISPLAY_OPTIONS_MALFORMED;
	data[length] = '\0';

	for (pos = 0; pos < length;) {
		size_t start = pos, end;
		while (pos < length && data[pos] != '\n')
			++pos;
		end = pos;
		if (pos < length)
			++pos;
		if (pos > end && end > start && data[end - 1] == '\r')
			--end;
		data[end] = '\0';

		if (!seen_custom && strcmp(data + start, "custom_textbox=0") == 0) {
			parsed.custom_textbox = false;
			seen_custom = 1;
		} else if (!seen_custom && strcmp(data + start, "custom_textbox=1") == 0) {
			parsed.custom_textbox = true;
			seen_custom = 1;
		} else if (!seen_nvl && strcmp(data + start, "nvl=0") == 0) {
			parsed.nvl = false;
			seen_nvl = 1;
		} else if (!seen_nvl && strcmp(data + start, "nvl=1") == 0) {
			parsed.nvl = true;
			seen_nvl = 1;
		} else {
			return DISPLAY_OPTIONS_MALFORMED;
		}
	}
	if (!seen_custom || !seen_nvl)
		return DISPLAY_OPTIONS_MALFORMED;
	*options = parsed;
	return recovered ? DISPLAY_OPTIONS_RECOVERED : DISPLAY_OPTIONS_OK;
}

DisplayOptionsResult display_options_save(const char *path, const DisplayOptions *options)
{
	char *temporary, *backup;
	FILE *file;
	struct stat info;
	int failed, had_previous = 0, backup_remains = 0;

	if (path == NULL || path[0] == '\0' || options == NULL)
		return DISPLAY_OPTIONS_INVALID_ARGUMENT;
	temporary = suffix_path(path, ".tmp");
	backup = suffix_path(path, ".bak");
	if (temporary == NULL || backup == NULL) {
		free(temporary);
		free(backup);
		return DISPLAY_OPTIONS_IO_ERROR;
	}

	if (stat(backup, &info) == 0 || errno != ENOENT) {
		free(temporary);
		free(backup);
		return DISPLAY_OPTIONS_IO_ERROR;
	}

	file = fopen(temporary, "wbx");
	if (file == NULL) {
		free(temporary);
		free(backup);
		return DISPLAY_OPTIONS_IO_ERROR;
	}
	failed = fprintf(file, "custom_textbox=%d\nnvl=%d\n",
		options->custom_textbox ? 1 : 0, options->nvl ? 1 : 0) < 0;
	if (fclose(file) != 0)
		failed = 1;
	if (!failed) {
		if (stat(path, &info) == 0) {
			if (!S_ISREG(info.st_mode) || rename(path, backup) != 0)
				failed = 1;
			else
				had_previous = 1;
		} else if (errno != ENOENT) {
			failed = 1;
		}
	}
	if (!failed && rename(temporary, path) != 0) {
		failed = 1;
		if (had_previous && rename(backup, path) == 0)
			had_previous = 0;
	}
	if (failed)
		remove(temporary);
	else if (had_previous && remove(backup) != 0)
		backup_remains = 1;
	free(temporary);
	free(backup);
	return failed ? DISPLAY_OPTIONS_IO_ERROR
		: backup_remains ? DISPLAY_OPTIONS_SAVED_BACKUP_REMAINS : DISPLAY_OPTIONS_OK;
}
