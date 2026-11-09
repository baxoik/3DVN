#include "wrap.h"

#include <string.h>

static size_t next_char(const char *text, size_t index)
{
	++index;
	while (((unsigned char)text[index] & 0xC0) == 0x80) {
		++index;
	}
	return index;
}

static int wrap_lines(const char *text, float width, WrapMeasure measure, void *user,
	unsigned short *starts, unsigned short *ends, int capacity, int combine_last)
{
	size_t length = strlen(text);
	size_t pos = 0;
	int count = 0;

	while (pos < length && count < capacity) {
		size_t start = pos;
		while (text[start] == ' ') {
			++start;
		}
		if (start >= length) {
			break;
		}
		size_t end = start;
		size_t next = length;

		if (combine_last && count == capacity - 1) {
			end = length;
		} else {
			size_t cursor = start;
			for (;;) {
				if (cursor >= length) {
					next = length;
					break;
				}
				if (text[cursor] == '\n') {
					next = cursor + 1;
					break;
				}
				size_t word_end = cursor;
				while (text[word_end] == ' ') {
					++word_end;
				}
				while (text[word_end] != '\0' && text[word_end] != ' ' && text[word_end] != '\n') {
					++word_end;
				}
				if (measure(text + start, word_end - start, user) <= width) {
					end = word_end;
					cursor = word_end;
					continue;
				}
				if (end > start) {
					next = end;
					break;
				}
				size_t last = start;
				for (size_t index = start; index < word_end;) {
					size_t following = next_char(text, index);
					if (last > start && measure(text + start, following - start, user) > width) {
						break;
					}
					last = following;
					index = following;
				}
				end = last;
				next = last;
				break;
			}
		}

		starts[count] = (unsigned short)start;
		ends[count] = (unsigned short)end;
		++count;
		pos = next;
	}
	return count;
}

void wrap_text(const char *text, float width, WrapMeasure measure, void *user, WrapLayout *out)
{
	out->count = wrap_lines(text, width, measure, user, out->start, out->end, WRAP_MAX_LINES, 1);
}

int wrap_text_lines(const char *text, float width, WrapMeasure measure, void *user,
	unsigned short *starts, unsigned short *ends, int capacity)
{
	return wrap_lines(text, width, measure, user, starts, ends, capacity, 0);
}

void wrap_indicator_position(const char *text, float width, float indicator_width, float gap,
	WrapMeasure measure, void *user, const unsigned short *starts, const unsigned short *ends,
	int count, float *x, int *line)
{
	*x = 0.0f;
	*line = count > 0 ? count - 1 : 0;
	if (count <= 0) {
		return;
	}
	*x = measure(text + starts[count - 1], ends[count - 1] - starts[count - 1], user) + gap;
	if (*x + indicator_width > width) {
		*x = 0.0f;
		++*line;
	}
}
