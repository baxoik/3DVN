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
			unsigned char fmt[22 + 4 * 32];
			size_t want = size < (long)sizeof(fmt) ? (size_t)size : sizeof(fmt);
			if (fread(fmt, 1, want, file) != want) {
				snprintf(err, err_size, "truncated format chunk");
				return 0;
			}
			unsigned tag = read_u16(fmt);
			info->channels = (int)read_u16(fmt + 2);
			info->rate = (long)read_u32(fmt + 4);
			info->bits = (int)read_u16(fmt + 14);
			if (tag == 2) {
				/* MS-ADPCM: block size, samples per block and the predictor coefficients */
				info->block_align = (int)read_u16(fmt + 12);
				info->coef_count = want >= 22 ? (int)read_u16(fmt + 20) : 0;
				if (info->bits != 4 || info->channels < 1 || info->channels > 2 || want < 22
					|| info->coef_count < 1 || info->coef_count > 32 || want < 22 + 4 * (size_t)info->coef_count
					|| info->block_align < 7 * info->channels + 1) {
					snprintf(err, err_size, "unsupported ADPCM WAV");
					return 0;
				}
				for (int index = 0; index < info->coef_count; ++index) {
					info->coef[index][0] = (short)read_u16(fmt + 22 + 4 * index);
					info->coef[index][1] = (short)read_u16(fmt + 24 + 4 * index);
				}
				info->samples_per_block = (info->block_align - 7 * info->channels) * 2 / info->channels + 2;
				info->adpcm = 1;
				info->bits = 16;
			} else if (tag != 1) {
				snprintf(err, err_size, "only PCM or MS-ADPCM WAV is supported");
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
			if (info->adpcm) {
				long rest = size % info->block_align;
				info->data_size = size;
				info->frames = size / info->block_align * info->samples_per_block;
				if (rest >= 7 * info->channels) {
					info->frames += (rest - 7 * info->channels) * 2 / info->channels + 2;
				}
			} else {
				info->data_size = size - size % info->frame_size;
				info->frames = info->data_size / info->frame_size;
			}
			if (info->data_size <= 0) {
				snprintf(err, err_size, "empty audio");
				return 0;
			}
			return 1;
		}
		position += 8 + size + (size & 1);
	}
}

static const int adapt_table[16] = {
	230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230
};

int wav_adpcm_block(const WavInfo *info, const unsigned char *block, size_t size, short *out)
{
	int channels = info->channels;
	if (size < (size_t)(7 * channels)) {
		return 0;
	}
	int c1[2], c2[2], delta[2], s1[2], s2[2];
	const unsigned char *p = block;
	for (int ch = 0; ch < channels; ++ch) {
		int index = *p++;
		if (index >= info->coef_count) index = 0;
		c1[ch] = info->coef[index][0];
		c2[ch] = info->coef[index][1];
	}
	for (int ch = 0; ch < channels; ++ch, p += 2) delta[ch] = (short)read_u16(p);
	for (int ch = 0; ch < channels; ++ch, p += 2) s1[ch] = (short)read_u16(p);
	for (int ch = 0; ch < channels; ++ch, p += 2) s2[ch] = (short)read_u16(p);
	/* the header holds the first two samples, older one (sample 2) first */
	for (int ch = 0; ch < channels; ++ch) out[ch] = (short)s2[ch];
	for (int ch = 0; ch < channels; ++ch) out[channels + ch] = (short)s1[ch];
	int frames = 2;
	size_t nibbles = (size - (size_t)(p - block)) * 2;
	size_t limit = (size_t)(info->samples_per_block - 2) * (size_t)channels;
	if (nibbles > limit) nibbles = limit;
	for (size_t n = 0; n < nibbles; ++n) {
		int ch = (int)(n % (size_t)channels);
		int code = (p[n / 2] >> ((n & 1) ? 0 : 4)) & 15;
		int signed_code = code >= 8 ? code - 16 : code;
		int sample = ((s1[ch] * c1[ch]) + (s2[ch] * c2[ch])) / 256 + signed_code * delta[ch];
		if (sample > 32767) sample = 32767;
		if (sample < -32768) sample = -32768;
		s2[ch] = s1[ch];
		s1[ch] = sample;
		delta[ch] = adapt_table[code] * delta[ch] / 256;
		if (delta[ch] < 16) delta[ch] = 16;
		out[frames * channels + ch] = (short)sample;
		if (ch == channels - 1) ++frames;
	}
	return frames;
}
