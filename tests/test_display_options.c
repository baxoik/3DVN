/* Host test for display_options load/save, including the optional bottom_text line.
 * Run on a PC:  cc -Isource -o /tmp/tdo tests/test_display_options.c source/display_options.c && /tmp/tdo /tmp */
#include "display_options.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void write_file(const char *path, const char *text) { FILE *f = fopen(path, "wb"); fputs(text, f); fclose(f); }
int main(int argc, char **argv) {
	(void)argc;
	char path[512]; snprintf(path, sizeof(path), "%s/opts.txt", argv[1]);
	DisplayOptions o;
	write_file(path, "custom_textbox=1\nnvl=0\n");                      /* old two-line file */
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_OK && o.custom_textbox && !o.nvl && !o.bottom_text);
	assert(o.text_speed == 1 && o.auto_speed == 1 && !o.skip_read && !o.touch_choices); /* defaults */
	write_file(path, "custom_textbox=1\nnvl=0\ntouch_choices=1\n");
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_OK && o.touch_choices && !o.bottom_text);
	write_file(path, "custom_textbox=1\nnvl=0\ntouch_choices=yes\n");
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_MALFORMED);
	write_file(path, "custom_textbox=1\nnvl=0\ntext_speed=3\nskip_read=1\nauto_speed=0\n");
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_OK && o.text_speed == 3 && o.auto_speed == 0 && o.skip_read);
	write_file(path, "custom_textbox=1\nnvl=0\ntext_speed=4\n");    /* out of range */
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_MALFORMED);
	write_file(path, "custom_textbox=1\nnvl=0\nauto_speed=3\n");
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_MALFORMED);
	write_file(path, "custom_textbox=1\nnvl=0\ntext_speed=12\n");
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_MALFORMED);
	write_file(path, "nvl=1\r\nbottom_text=1\r\ncustom_textbox=0\r\n");   /* any order, CRLF */
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_OK && !o.custom_textbox && o.nvl && o.bottom_text);
	write_file(path, "custom_textbox=1\nnvl=0\nbottom_text=1\nbottom_text=0\n"); /* duplicate */
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_MALFORMED);
	write_file(path, "custom_textbox=1\nnvl=0\nbottom_text=2\n");       /* bad value */
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_MALFORMED);
	write_file(path, "bottom_text=1\nnvl=0\n");                         /* required line missing */
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_MALFORMED);
	unlink(path);
	DisplayOptions w = {true, false, false, 2, 0, true, true, 7, 0, 10}; /* round trip */
	DisplayOptionsResult r = display_options_save(path, &w);
	assert(r == DISPLAY_OPTIONS_OK || r == DISPLAY_OPTIONS_SAVED_BACKUP_REMAINS);
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_OK && o.custom_textbox && !o.nvl && !o.bottom_text);
	assert(o.touch_choices && o.text_speed == 2 && o.auto_speed == 0 && o.skip_read);
	assert(o.master_volume == 7 && o.music_volume == 0 && o.voice_volume == 10);
	unlink(path);
	write_file(path, "custom_textbox=1\nnvl=0\n");                      /* older file: full volume */
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_OK);
	assert(o.master_volume == 10 && o.music_volume == 10 && o.voice_volume == 10);
	write_file(path, "custom_textbox=1\nnvl=0\nvoice_volume=3\nmaster_volume=10\n");
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_OK && o.voice_volume == 3 && o.music_volume == 10);
	write_file(path, "custom_textbox=1\nnvl=0\nmusic_volume=11\n");     /* out of range */
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_MALFORMED);
	write_file(path, "custom_textbox=1\nnvl=0\nmusic_volume=4\nmusic_volume=5\n"); /* duplicate */
	assert(display_options_load(path, &o) == DISPLAY_OPTIONS_MALFORMED);
	puts("display_options: all tests passed");
	return 0;
}
