#ifndef WRAP_H
#define WRAP_H

#include <stddef.h>

#define WRAP_MAX_LINES 12

typedef struct {
	int count;
	unsigned short start[WRAP_MAX_LINES];
	unsigned short end[WRAP_MAX_LINES];  /* exclusive */
} WrapLayout;

/* Returns the rendered width of text[0..length). */
typedef float (*WrapMeasure)(const char *text, size_t length, void *user);

/* Greedy word wrap of UTF-8 text; honors '\n'. Words wider than the line are split. */
void wrap_text(const char *text, float width, WrapMeasure measure, void *user, WrapLayout *out);

/* Wrap without combining the last line; returns the number of lines produced. */
int wrap_text_lines(const char *text, float width, WrapMeasure measure, void *user,
	unsigned short *starts, unsigned short *ends, int capacity);

/* Position an inline indicator after the last wrapped line, or on a new line. */
void wrap_indicator_position(const char *text, float width, float indicator_width, float gap,
	WrapMeasure measure, void *user, const unsigned short *starts, const unsigned short *ends,
	int count, float *x, int *line);

#endif
