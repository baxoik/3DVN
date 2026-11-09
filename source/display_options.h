#ifndef DISPLAY_OPTIONS_H
#define DISPLAY_OPTIONS_H

#include <stdbool.h>

typedef struct {
	bool custom_textbox;
	bool nvl;
} DisplayOptions;

typedef enum {
	DISPLAY_OPTIONS_OK,
	DISPLAY_OPTIONS_MISSING,
	DISPLAY_OPTIONS_RECOVERED,
	DISPLAY_OPTIONS_SAVED_BACKUP_REMAINS,
	DISPLAY_OPTIONS_IO_ERROR,
	DISPLAY_OPTIONS_MALFORMED,
	DISPLAY_OPTIONS_INVALID_ARGUMENT
} DisplayOptionsResult;

/* options must not be NULL. Defaults are custom_textbox=true, nvl=false. */
void display_options_defaults(DisplayOptions *options);

/* A saved custom preference is effective only while a custom image is available. */
bool display_options_use_custom(const DisplayOptions *options, bool available);

/* Keep saved choices; apply a game's style only when no valid file was loaded. */
void display_options_apply_game_default(DisplayOptions *options, DisplayOptionsResult result, bool default_nvl);

/*
 * The file has exactly one custom_textbox=0|1 line and one nvl=0|1 line,
 * in either order. LF and CRLF are accepted; the final newline is optional.
 * A missing file returns MISSING and sets defaults, unless a backup exists,
 * in which case it is loaded and RECOVERED is returned. Other errors leave
 * options unchanged. An empty path or NULL argument returns INVALID_ARGUMENT.
 */
DisplayOptionsResult display_options_load(const char *path, DisplayOptions *options);

/*
 * Writes path.tmp, moves an existing file to path.bak, then installs the new
 * file. Existing temporary or backup files are never overwritten. On failure
 * the prior file is restored where possible, otherwise retained as path.bak.
 * SAVED_BACKUP_REMAINS reports a successful write whose backup cleanup failed.
 * No crash-durability guarantee is made. Callers must use distinct paths.
 */
DisplayOptionsResult display_options_save(const char *path, const DisplayOptions *options);

#endif
