#include "wav.h"

#include <string.h>

static unsigned read_u32(const unsigned char *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned read_u16(const unsigned char *p)
{
	return p[0] | (p[1] << 8);
}

int wav_open_info(FILE *file, WavInfo *info, char *err, size_t err_size)
{
	unsigned char head[12];
	memset(info, 0, sizeof(*info));
	if (fseek(file, 0, SEEK_END) != 0) {
		snprintf(err, err_size, "unreadable file");
		return 0;
	}
	long file_size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (fread(head, 1, 12, file) != 12 || memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0) {
		snprintf(err, err_size, "not a WAV file");
		return 0;
	}

	int have_format = 0;
	long position = 12;
	for (;;) {
		unsigned char chunk[8];
		if (fseek(file, position, SEEK_SET) != 0 || fread(chunk, 1, 8, file) != 8) {
			snprintf(err, err_size, "no sample data found");
			return 0;
		}
		long size = (long)read_u32(chunk + 4);
		if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
			unsigned char fmt[16];
			if (fread(fmt, 1, 16, file) != 16) {
				snprintf(err, err_size, "truncated format chunk");
				return 0;
			}
			unsigned tag = read_u16(fmt);
			info->channels = (int)read_u16(fmt + 2);
			info->rate = (long)read_u32(fmt + 4);
			info->bits = (int)read_u16(fmt + 14);
			if (tag != 1) {
				snprintf(err, err_size, "only uncompressed PCM WAV is supported");
				return 0;
			}
			if ((info->channels != 1 && info->channels != 2) || (info->bits != 8 && info->bits != 16)
				|| info->rate < 4000 || info->rate > 48000) {
				snprintf(err, err_size, "need 8/16-bit mono or stereo, 4-48 kHz");
				return 0;
			}
			have_format = 1;
		} else if (memcmp(chunk, "data", 4) == 0) {
			if (!have_format) {
				snprintf(err, err_size, "data before format chunk");
				return 0;
			}
			info->frame_size = info->channels * info->bits / 8;
			info->data_offset = position + 8;
			if (size > file_size - info->data_offset) {
				size = file_size - info->data_offset;
			}
			info->data_size = size - size % info->frame_size;
			if (info->data_size <= 0) {
				snprintf(err, err_size, "empty audio");
				return 0;
			}
			return 1;
		}
		position += 8 + size + (size & 1);
	}
}
