#include "vid.h"

static unsigned read_u16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned read_u32(const unsigned char *p)
{
	return read_u16(p) | (read_u16(p + 2) << 16);
}

int vid_parse_header(const unsigned char *data, size_t size, VidHeader *header)
{
	if (size < VID_HEADER_SIZE || data[0] != '3' || data[1] != 'V' || data[2] != 'V' || data[3] != '1') {
		return 0;
	}
	header->width = read_u16(data + 4);
	header->height = read_u16(data + 6);
	header->fps_milli = read_u32(data + 8);
	header->frames = read_u32(data + 12);
	return header->width >= 1 && header->width <= VID_MAX_WIDTH
		&& header->height >= 1 && header->height <= VID_MAX_HEIGHT
		&& header->fps_milli >= 1000 && header->fps_milli <= 60000
		&& header->frames >= 1 && header->frames <= VID_MAX_FRAMES;
}

int vid_read_index(const unsigned char *data, size_t size, const VidHeader *header,
	size_t file_size, unsigned *offsets)
{
	size_t count = (size_t)header->frames + 1;
	if (size < count * 4) {
		return 0;
	}
	for (size_t index = 0; index < count; ++index) {
		offsets[index] = read_u32(data + index * 4);
	}
	if (offsets[0] != VID_HEADER_SIZE + count * 4 || offsets[header->frames] > file_size) {
		return 0;
	}
	for (size_t index = 0; index < header->frames; ++index) {
		unsigned length = offsets[index + 1] - offsets[index];
		if (offsets[index + 1] <= offsets[index] || length > VID_MAX_FRAME_BYTES) {
			return 0;
		}
	}
	return 1;
}

unsigned vid_frame_at(const VidHeader *header, float seconds)
{
	if (seconds < 0.0f) {
		return 0;
	}
	double frame = (double)seconds * header->fps_milli / 1000.0;
	return frame >= header->frames ? header->frames - 1 : (unsigned)frame;
}

int vid_finished(const VidHeader *header, float seconds)
{
	return (double)seconds * header->fps_milli / 1000.0 >= header->frames;
}
